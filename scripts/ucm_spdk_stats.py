"""Read-only SPDK target statistics for ucm_store_stats.sh; no engine initialization."""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.request

from ucm_metric_summary import fawa_start_load_summary, fawa_wait_summary, start_load_summary


def load_config(path):
    import yaml

    with open(path) as stream:
        config = yaml.safe_load(stream)
    entries = config.get("ucm_connectors", [])
    if len(entries) != 1:
        raise ValueError("statistics require exactly one ucm_connectors entry")
    return entries[0]["ucm_connector_config"]


def backend(config):
    stages = config.get("store_pipeline", "").lower().split("|")
    if "spdk" in stages:
        return "spdk"
    if "posix" in stages:
        return "posix"
    raise ValueError("unsupported store_pipeline; select STORE_BACKEND explicitly")


def posix_engine(config):
    if backend(config) != "posix":
        raise ValueError("configuration is not a Posix pipeline")
    engine = config.get("posix_io_engine", "psync")
    if engine not in ("psync", "aio"):
        raise ValueError(f"unsupported posix_io_engine: {engine!r}")
    return engine


def rpc(method, *args):
    script = os.environ.get(
        "SPDK_RPC", str(Path(os.environ.get("SPDK_ROOT_DIR", str(Path.home() / "devSPDK/spdk"))) / "scripts/rpc.py")
    )
    command = [script, "-s", os.environ.get("SPDK_RPC_SOCKET", "/var/tmp/spdk.sock"), method, *args]
    if os.environ.get("SPDK_RPC_SUDO", "1") == "1":
        command = ["sudo", "-n", *command]
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "RPC failed")
    return json.loads(result.stdout)


def resolve_target(config, host):
    trid = dict(part.split(":", 1) for part in config.get("spdk_trid", "").split())
    # A local RPC socket must never silently represent an unrelated remote target.
    if trid.get("trtype", "").lower() != "rdma":
        raise ValueError("target RPC statistics require an RDMA store, not direct PCIe")
    if host not in ("127.0.0.1", "localhost"):
        raise ValueError("local target RPC cannot monitor a remote vLLM HOST; run on its target host")
    addresses = json.loads(subprocess.check_output(["ip", "-j", "address"], text=True, timeout=5))
    local = {a["local"] for interface in addresses for a in interface.get("addr_info", [])}
    if str(ipaddress.ip_address(trid["traddr"])) not in local:
        raise ValueError("spdk_trid is not a local target; run statistics on the target host")
    matches = []
    for subsystem in rpc("nvmf_get_subsystems"):
        if subsystem.get("subtype") != "NVMe":
            continue
        if trid.get("subnqn") and subsystem["nqn"] != trid["subnqn"]:
            continue
        if any(
            a.get("trtype", a.get("transport", "")).lower() == "rdma"
            and a.get("traddr") == trid["traddr"]
            and str(a.get("trsvcid")) == trid.get("trsvcid", "4420")
            for a in subsystem.get("listen_addresses", [])
        ):
            matches.append(subsystem)
    if len(matches) != 1:
        raise ValueError("no unique subsystem matches configured RDMA listener/NQN")
    subsystem = matches[0]
    names = [ns["bdev_name"] for ns in subsystem.get("namespaces", [])]
    selected = os.environ.get("SPDK_BDEV")
    if selected and selected not in names:
        raise ValueError("SPDK_BDEV is not a namespace of the configured subsystem")
    if not selected and len(names) != 1:
        raise ValueError("multiple namespaces: set SPDK_BDEV explicitly")
    return subsystem["nqn"], selected or names[0]


def sample(nqn, name):
    result = rpc("bdev_get_iostat", "--names", name)
    bdev = next(b for b in result["bdevs"] if b["name"] == name)
    controllers = rpc("nvmf_subsystem_get_controllers", nqn)
    return {"bdev": bdev, "tick_rate": result["tick_rate"], "ticks": result["ticks"],
            "connections": len(controllers),
            "qpairs": sum(c.get("num_io_qpairs", 0) for c in controllers)}


def report(current, previous=None):
    data = current["bdev"]
    elapsed = None
    if previous:
        keys = ("bytes_read", "bytes_written", "num_read_ops", "num_write_ops",
                "read_latency_ticks", "write_latency_ticks")
        if (current["tick_rate"] != previous["tick_rate"]
                or current["ticks"] <= previous["ticks"]
                or any(data[k] < previous["bdev"][k] for k in keys)):
            print("  Counters restarted/reset; new baseline, no delta reported.")
            return
        elapsed = (current["ticks"] - previous["ticks"]) / current["tick_rate"]
        data = {k: data[k] - previous["bdev"][k] for k in keys}
    label = f"delta {elapsed:.2f}s" if elapsed else "cumulative since target/statistics start"
    print(f"[{time.strftime('%H:%M:%S')}] {current['bdev']['name']} ({label})")
    for op in ("read", "write"):
        count = data[f"num_{op}_ops"]
        size = data["bytes_read" if op == "read" else "bytes_written"]
        latency = (f"{data[f'{op}_latency_ticks'] / current['tick_rate'] * 1e6 / count:.1f} us"
                   if count else "N/A")
        rate = f" | {size / elapsed / 1e9:.3f} GB/s | {count / elapsed:.1f} IOPS" if elapsed else ""
        print(f"  {op}: {size:,} B ({size / 2**30:.3f} GiB), {count:,} ops{rate} | avg bdev latency {latency}")
    print(f"  controllers: {current['connections']} | reported I/O qpairs: {current['qpairs']}")
    print("  io_error (target cumulative, not Store health): "
          + json.dumps(current["bdev"].get("io_error", "N/A"), sort_keys=True))


def cache_metrics(host, port):
    try:
        text = urllib.request.urlopen(f"http://{host}:{port}/metrics", timeout=5).read().decode()
        names = ("cache_dump_bytes_total", "cache_load_bytes_total", "cache_dump_shards_total",
                 "posix_lookup_query_blocks_total", "posix_lookup_hit_blocks_total",
                 "total_prefix_query_tokens_total", "gpu_hbm_hit_tokens_total", "ucm_hit_tokens_total")
        values = {}
        for name in names:
            samples = re.findall(r"^ucm:" + name + r"(?:\{[^\n]*\})?\s+([^\s]+)$", text, re.M)
            values[name] = sum(map(float, samples)) if samples else None
        print("  UCM /metrics (cumulative, ranks summed):")
        print("    " + start_load_summary(text))
        print("    " + fawa_start_load_summary(text))
        print("    " + fawa_wait_summary(text))
        for name, value in values.items():
            print(f"    {name}: {value:,.0f}" if value is not None else f"    {name}: N/A")
        for label, hit, query in (
            ("backend lookup hit", "posix_lookup_hit_blocks_total", "posix_lookup_query_blocks_total"),
            ("HBM token hit", "gpu_hbm_hit_tokens_total", "total_prefix_query_tokens_total"),
            ("UCM token hit (not SSD-only)", "ucm_hit_tokens_total", "total_prefix_query_tokens_total"),
        ):
            ratio = (f"{100 * values[hit] / values[query]:.2f}%"
                     if values[query] and values[hit] is not None else "N/A")
            print(f"    {label}: {ratio}")
        for name in ("cache_load_duration_ms", "cache_dump_duration_ms",
                     "cache_shard_backend_wait_ms", "cache_h2d_submit_ms",
                     "cache_h2d_sync_ms", "cache_d2h_duration_ms",
                     "cache_dump_mkbuf_duration_ms", "cache_dump_prereq_wait_ms",
                     "save_completion_wait_duration"):
            totals = []
            for suffix in ("sum", "count"):
                samples = re.findall(r"^ucm:" + name + "_" + suffix
                                     + r"(?:\{[^\n]*\})?\s+([^\s]+)$", text, re.M)
                totals.append(sum(map(float, samples)))
            average = f"{totals[0] / totals[1]:.3f} ms" if totals[1] else "N/A"
            print(f"    {name} average: {average}")
    except Exception as error:
        print(f"  UCM metrics unavailable: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="")
    parser.add_argument("--pid")
    parser.add_argument("--config-path", action="store_true")
    parser.add_argument("--detect", action="store_true")
    parser.add_argument("--posix-engine", action="store_true")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default="7780")
    parser.add_argument("--mode", choices=("snapshot", "watch", "probe"), default="snapshot")
    parser.add_argument("--interval", type=float, default=10)
    parser.add_argument("--cycles", type=int, default=None)
    parser.add_argument("--probe-seconds", type=float, default=10)
    args = parser.parse_args()
    if args.cycles is None:
        args.cycles = 1 if args.mode == "snapshot" else 0
    if args.config_path:
        argv = Path(f"/proc/{args.pid}/cmdline").read_bytes().decode().split("\0")
        if "--kv-transfer-config" in argv:
            config = json.loads(argv[argv.index("--kv-transfer-config") + 1])
            print(config.get("kv_connector_extra_config", {}).get("UCM_CONFIG_FILE", ""))
        return
    config = load_config(args.config)
    if args.posix_engine:
        print(posix_engine(config))
        return
    if args.detect:
        print(backend(config))
        return
    if backend(config) != "spdk":
        raise ValueError("configuration is not a Spdk pipeline")
    if args.interval <= 0 or args.probe_seconds <= 0 or args.cycles < 0:
        raise ValueError("interval/probe duration must be positive; cycles must be nonnegative")
    nqn, name = resolve_target(config, args.host)
    print(f"SPDK target: {config['spdk_trid']} | bdev: {name} | config: {args.config}")
    print(f"Configured pool: {config.get('spdk_capacity_gb', 'N/A')} GiB (not measured usage)")
    print("Read-only RPC; counters are NOT reset. Target-wide completed I/O includes all clients,")
    print("startup/recovery and index I/O; these are not per-model payload or durability counters.")
    print("N/A: Posix files/syscalls, Store used capacity/GC/health and Store task latency (not exported).")
    print("Cache load / external hits may come from host Cache, not SSD. Bdev latency excludes H2D/D2H.")
    previous = sample(nqn, name)
    report(previous)
    cache_metrics(args.host, args.port)
    sys.stdout.flush()
    cycles = 1
    while args.mode == "probe" or not args.cycles or cycles < args.cycles:
        time.sleep(args.probe_seconds if args.mode == "probe" else args.interval)
        try:
            current = sample(nqn, name)
        except Exception as error:
            print(f"  RPC unavailable: {error}; baseline discarded", flush=True)
            previous = None
            if args.mode == "probe":
                raise
        else:
            report(current, None if args.mode == "snapshot" else previous)
            previous = current
            cache_metrics(args.host, args.port)
        sys.stdout.flush()
        cycles += 1
        if args.mode == "probe":
            return


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception as error:
        print(f"SPDK statistics error: {error}", file=sys.stderr)
        sys.exit(1)
