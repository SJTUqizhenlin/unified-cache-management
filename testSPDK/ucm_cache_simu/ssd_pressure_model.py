#!/usr/bin/env python3
"""SSD read/write pressure model for KV-cache offload serving.

Input direction (user-driven): the service is specified by latency and
concurrency targets, and the request rate is DERIVED:

    RPS = N / (TTFT + O * TPOT)          (Little's law on a request)

Then the storage chain:

    block gen rate = RPS * (P + O) / block_size
    write bandwidth = block rate * bytes-per-block (model dependent)
    read  bandwidth = write bandwidth * rw_ratio

plus feasibility checks against measured SSD ceilings, metadata-op rates,
the in-flight HBM footprint, and the disk-side TTFT floor.

Assumptions: GPU infinitely fast (utilization ~0, no queueing); steady
state; every completed block is dumped; reads model prefix-hit reloads.

Examples:
  ./ssd_pressure_model.py --model glm-5.3
  ./ssd_pressure_model.py --model kimi-k3 --input 8192 --output 2048 \
      --concurrency 320 --tpot 0.04 --ttft 2.0 --ratio 0.5 --state-interval 16
  ./ssd_pressure_model.py --json
"""

import argparse
import json
import math
import sys

GiB = 1024 ** 3
MiB = 1024 ** 2

# ---------------------------------------------------------------------------
# Model profiles.
# kv_bytes_per_token: per attention layer per token.
# state_bytes_per_layer: per linear-attention layer per 128-token boundary
#   (mamba-align snapshot: recurrent + short-conv states, bf16).
# ---------------------------------------------------------------------------
MODELS = {
    "glm-5.3": dict(
        attn_layers=78, kv_bytes_per_token=1152,   # (kv_lora 512 + rope 64) * bf16
        state_layers=0, state_bytes_per_layer=0,
        note="pure MLA, homogeneous layers; layerwise and whole-block both real",
    ),
    "glm-5.3-flash": dict(
        attn_layers=11, kv_bytes_per_token=1024,   # rope=0, kv_lora 512 * bf16
        state_layers=34, state_bytes_per_layer=2293760,  # [64,128,128]x2B + conv
        note="hybrid: 34 linear attn (per-sequence state) + 11 DSA",
    ),
    "kimi-k3": dict(
        attn_layers=24, kv_bytes_per_token=1152,
        state_layers=69, state_bytes_per_layer=3440640,  # [96,128,128]x2B + conv
        note="hybrid: 69 KDA linear attn + 24 full attn MLA",
    ),
    "dsv4-pro": dict(
        attn_layers=61, kv_bytes_per_token=1152,
        state_layers=0, state_bytes_per_layer=0,
        note="UPPER BOUND: ignores sliding window (128) and per-layer "
             "compression; non-layerwise only",
    ),
}

# Measured on the ES3000 V6 test drives (this project's benchmarks):
#   mixed write ceiling ~3.5 GiB/s (kernel path), read ceiling ~6.2 GiB/s.
SSD_WRITE_GBS_DEFAULT = 3.5
SSD_READ_GBS_DEFAULT = 6.2

# Per-op costs of the UCM POSIX metadata path (measured, kernel_cache_test):
META_COST_US = {"rename": 113.0, "unlink": 80.0, "access": 5.0}


def fmt_bw(bps):
    if bps >= GiB:
        return f"{bps / GiB:.2f} GiB/s"
    if bps >= MiB:
        return f"{bps / MiB:.1f} MiB/s"
    return f"{bps / 1024:.1f} KiB/s"


def fmt_bytes(n):
    if n >= GiB:
        return f"{n / GiB:.2f} GiB"
    if n >= MiB:
        return f"{n / MiB:.2f} MiB"
    return f"{n / 1024:.1f} KiB"


def analyze(args):
    p = MODELS[args.model]
    B = args.block_size

    kv_bytes_per_block = p["attn_layers"] * p["kv_bytes_per_token"] * B
    # Persist a state snapshot only every N block boundaries (N=1 is the
    # mamba-align semantics; N>1 trades resume granularity for bandwidth).
    state_bytes_per_block = (p["state_layers"] * p["state_bytes_per_layer"]
                             / args.state_interval)
    bytes_per_block = kv_bytes_per_block + state_bytes_per_block
    bytes_per_token = bytes_per_block / B

    # --- Derived request rate (Little's law on one request) ---------------
    residence = args.ttft + args.output * args.tpot
    rps = args.concurrency / residence

    tokens_per_s = rps * (args.input + args.output)
    blocks_per_req = (args.input + args.output) / B
    prefill_blocks_per_req = args.input / B

    block_gen_rate = rps * blocks_per_req          # blocks/s (steady state)
    write_bw = block_gen_rate * bytes_per_block
    read_bw = write_bw * args.ratio

    # Read/write ratio feasibility: without prefix sharing, a block is read
    # at most once after being written, so max read:write = P/(P+O) < 1.
    max_ratio_no_sharing = args.input / (args.input + args.output)
    implied_hit_rate = args.ratio * (args.input + args.output) / args.input

    # Storage feasibility.
    ssd_write = args.ssd_write_gbs * GiB
    ssd_read = args.ssd_read_gbs * GiB
    max_rps_write = ssd_write / (blocks_per_req * bytes_per_block) if write_bw else math.inf
    max_rps_read = (ssd_read / (bytes_per_block * blocks_per_req * args.ratio)
                    if args.ratio > 0 else math.inf)
    ssds_needed = max(math.ceil(write_bw / ssd_write) if ssd_write else 0,
                      math.ceil(read_bw / ssd_read) if ssd_read else 0)
    fill_hours = (args.ssd_capacity_gib * GiB / write_bw / 3600) if write_bw else math.inf

    # Metadata op rates on the UCM POSIX path (steady state):
    #   dump => 1 rename per block; steady-state eviction => 1 unlink per dump;
    #   scheduling lookup => ~P/B access() per request (prefix scan).
    meta = {
        "rename_per_s": block_gen_rate,
        "unlink_per_s": block_gen_rate,           # eviction balances dumps
        "access_per_s": rps * prefill_blocks_per_req,
    }
    meta_cpu_cores = sum(rate * cost for rate, cost in
                         ((meta["rename_per_s"], META_COST_US["rename"]),
                          (meta["unlink_per_s"], META_COST_US["unlink"]),
                          (meta["access_per_s"], META_COST_US["access"]))) / 1e6

    # IO-shape profile.
    io_shape = {
        "layerwise_attn_shard": p["kv_bytes_per_token"] * B,
        "layerwise_state_shard": p["state_bytes_per_layer"] or None,
        "whole_block_io": bytes_per_block,
        "prefill_burst_per_request": prefill_blocks_per_req * bytes_per_block,
    }

    # Concurrency decomposition (consistent by construction with rps):
    #   N = N_decode + N_prefill = RPS*O*TPOT + RPS*TTFT
    n_decode = rps * args.output * args.tpot
    n_prefill = rps * args.ttft
    in_flight_kv = args.concurrency * (args.input + args.output / 2) * bytes_per_token
    read_bytes_per_req = read_bw / rps if rps else 0.0
    ttft_disk_floor = read_bytes_per_req / ssd_read if ssd_read else None

    conc = {
        "residence_s": residence,
        "derived_rps": rps,
        "n_decode": n_decode,
        "n_prefill": n_prefill,
        "decode_dump_rate_per_s": n_decode / (B * args.tpot),
        "in_flight_kv_bytes": in_flight_kv,
        "read_bytes_per_request": read_bytes_per_req,
        "ttft_disk_floor_s": ttft_disk_floor,
        "ttft_disk_floor_pct": (ttft_disk_floor / args.ttft * 100
                                if ttft_disk_floor is not None else None),
    }

    return {
        "model": args.model,
        "model_note": p["note"],
        "input_tokens": args.input,
        "output_tokens": args.output,
        "concurrency": args.concurrency,
        "tpot_s": args.tpot,
        "ttft_target_s": args.ttft,
        "rw_ratio": args.ratio,
        "block_size_tokens": B,
        "state_interval": args.state_interval,
        "rps": rps,
        "tokens_per_s": tokens_per_s,
        "blocks_per_request": blocks_per_req,
        "block_gen_rate_per_s": block_gen_rate,
        "kv_bytes_per_block": kv_bytes_per_block,
        "state_bytes_per_block": state_bytes_per_block,
        "bytes_per_block": bytes_per_block,
        "write_bw": write_bw,
        "read_bw": read_bw,
        "total_bw": write_bw + read_bw,
        "prefill_write_frac": args.input / (args.input + args.output),
        "max_ratio_no_sharing": max_ratio_no_sharing,
        "implied_single_read_hit_rate": implied_hit_rate,
        "ssds_needed": ssds_needed,
        "max_rps_write_bound": max_rps_write,
        "max_rps_read_bound": max_rps_read,
        "storage_feasible": (rps <= max_rps_write and (args.ratio == 0
                                                       or rps <= max_rps_read)),
        "fill_hours_per_ssd": fill_hours,
        "meta": meta,
        "meta_cpu_cores": meta_cpu_cores,
        "io_shape": io_shape,
        "concurrency_view": conc,
    }


def report(r):
    W = 62
    print("=" * W)
    print(f"SSD pressure model: {r['model']}  ({r['model_note']})")
    print("=" * W)
    c = r["concurrency_view"]
    print(f"Targets       : input {r['input_tokens']} tok, output "
          f"{r['output_tokens']} tok, TTFT {r['ttft_target_s']:.2f} s, "
          f"TPOT {r['tpot_s'] * 1000:.0f} ms")
    print(f"Concurrency N : {r['concurrency']}")
    print(f"  => RPS      : {r['rps']:.2f} req/s   "
          f"(= N / (TTFT + O*TPOT) = {r['concurrency']} / "
          f"{c['residence_s']:.2f}s)")
    print(f"  N split     : {c['n_decode']:.0f} decoding + "
          f"{c['n_prefill']:.0f} prefilling")
    print(f"Token rate    : {r['tokens_per_s']:.0f} tok/s "
          f"(prefill {r['prefill_write_frac'] * 100:.0f}% of written bytes)")
    print(f"Block rate    : {r['block_gen_rate_per_s']:.2f} blocks/s "
          f"({r['blocks_per_request']:.1f} blocks/request, "
          f"{r['block_size_tokens']} tok/block)")
    print()
    print("--- Per-block bytes ---")
    print(f"  attention KV : {fmt_bytes(r['kv_bytes_per_block'])}")
    if r["state_bytes_per_block"]:
        extra = (f", snapshot every {r['state_interval']} blocks"
                 if r["state_interval"] > 1 else "")
        print(f"  linear state : {fmt_bytes(r['state_bytes_per_block'])} "
              f"({r['state_bytes_per_block'] / r['kv_bytes_per_block']:.0f}x "
              f"the KV part{extra})")
    print(f"  total        : {fmt_bytes(r['bytes_per_block'])}")
    print()
    print("--- Bandwidth ---")
    print(f"  WRITE : {fmt_bw(r['write_bw'])}")
    print(f"  READ  : {fmt_bw(r['read_bw'])}   (read:write = "
          f"{r['rw_ratio']}:1)")
    print(f"  TOTAL : {fmt_bw(r['total_bw'])}")
    print()
    print(f"--- Storage feasibility "
          f"(ceilings: {3.5:.1f} W / {6.2:.1f} R GiB/s per SSD) ---")
    bound = ("write" if r["max_rps_write_bound"] <= r["max_rps_read_bound"]
             else "read")
    print(f"  SSDs required        : {r['ssds_needed']}  ({bound}-bound)")
    print(f"  single-SSD max RPS   : write {r['max_rps_write_bound']:.2f} / "
          f"read {r['max_rps_read_bound']:.2f} req/s  "
          f"(target {r['rps']:.2f})")
    print(f"  3.5 TiB drive fills in {r['fill_hours_per_ssd']:.2f} h of writes "
          f"(before GC)")
    print()
    print("--- Ratio sanity ---")
    print(f"  max read:write without prefix sharing = "
          f"{r['max_ratio_no_sharing']:.2f}:1  (P/(P+O))")
    h = r["implied_single_read_hit_rate"]
    if h > 1.0:
        print(f"  ratio {r['rw_ratio']}:1 implies {h:.2f}x re-read of each "
              f"block (prefix sharing required; single-read hit rate would "
              f"be >100%)")
    else:
        print(f"  equivalent single-read external hit rate = {h * 100:.0f}%")
    print()
    print("--- In-flight footprint & TTFT budget ---")
    print(f"  In-flight KV size  : {fmt_bytes(c['in_flight_kv_bytes'])} "
          f"(HBM footprint of the in-flight set)")
    if c["ttft_disk_floor_s"] is not None:
        print(f"  Disk load per req  : {fmt_bytes(c['read_bytes_per_request'])} "
              f"=> TTFT floor {c['ttft_disk_floor_s'] * 1000:.0f} ms "
              f"= {c['ttft_disk_floor_pct']:.1f}% of the "
              f"{r['ttft_target_s']:.2f} s TTFT budget")
        print(f"     (floor assumes the drive exclusively serves one load; "
              f"concurrent loads inflate it)")
    print()
    print("--- UCM POSIX metadata path ---")
    m = r["meta"]
    print(f"  rename (commit) : {m['rename_per_s']:.1f}/s")
    print(f"  unlink (evict)  : {m['unlink_per_s']:.1f}/s")
    print(f"  access (lookup) : {m['access_per_s']:.1f}/s")
    print(f"  => metadata CPU : ~{r['meta_cpu_cores']:.2f} cores "
          f"(main-thread, synchronous)")
    print()
    print("--- IO shape ---")
    io = r["io_shape"]
    print(f"  layerwise attn shard : {fmt_bytes(io['layerwise_attn_shard'])}")
    if io["layerwise_state_shard"]:
        print(f"  layerwise state shard: {fmt_bytes(io['layerwise_state_shard'])}")
    print(f"  whole-block IO       : {fmt_bytes(io['whole_block_io'])}")
    print(f"  prefill dump burst/request: {fmt_bytes(io['prefill_burst_per_request'])}")
    print("=" * W)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", choices=sorted(MODELS), default="glm-5.3")
    ap.add_argument("--input", type=int, default=4096, help="avg input tokens (P)")
    ap.add_argument("--output", type=int, default=1024, help="avg output tokens (O)")
    ap.add_argument("--concurrency", type=int, default=640,
                    help="in-flight requests N")
    ap.add_argument("--tpot", type=float, default=0.05,
                    help="target time-per-output-token in seconds")
    ap.add_argument("--ttft", type=float, default=1.0,
                    help="target time-to-first-token in seconds")
    ap.add_argument("--ratio", type=float, default=1.0,
                    help="read:write bandwidth ratio (default 1.0)")
    ap.add_argument("--block-size", type=int, default=128, help="tokens per block")
    ap.add_argument("--state-interval", type=int, default=1,
                    help="persist linear-attn state every N block boundaries "
                         "(1 = mamba-align semantics; N>1 = coarser snapshots)")
    ap.add_argument("--ssd-write-gbs", type=float, default=SSD_WRITE_GBS_DEFAULT)
    ap.add_argument("--ssd-read-gbs", type=float, default=SSD_READ_GBS_DEFAULT)
    ap.add_argument("--ssd-capacity-gib", type=float, default=3584)
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    args = ap.parse_args()

    if args.input <= 0 or args.output < 0 or args.concurrency <= 0:
        sys.exit("invalid input values")
    if args.tpot <= 0 or args.ttft <= 0 or args.ratio < 0:
        sys.exit("invalid latency/ratio values")
    if args.ttft + args.output * args.tpot <= 0:
        sys.exit("residence time is zero")

    r = analyze(args)
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        report(r)


if __name__ == "__main__":
    main()
