#!/usr/bin/env python3
"""Probe vLLM services visible from inside the current Kubernetes pod.

This is the pod-adapted version of vllm_probe.py. /proc only exposes processes
in this pod, so there is no need for sudo, crictl or docker. Pod/container
identity comes from the downward-API environment (POD_NAME, POD_NAMESPACE,
POD_UID, HOSTNAME) when the runtime commands are unavailable.

For each `vllm serve` process the probe reports:
  * model / served model names
  * process start time and uptime
  * listening host:port
  * pod / namespace / container when available
  * currently connected client processes (PID, user, command)
  * last observed client or metrics activity, cached across probe runs
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request


CLK_TCK = os.sysconf("SC_CLK_TCK")
HOST_NET_NS = None
try:
    HOST_NET_NS = os.readlink("/proc/1/ns/net")
except OSError:
    pass
CONTAINER_PATTERNS = (
    re.compile(r"(?:cri-containerd|docker|crio|libpod)-([0-9a-f]{12,64})\.scope"),
    re.compile(r"/([0-9a-f]{64})(?:$|/)"),
)
TCP_ESTABLISHED = "01"


def boot_time():
    try:
        for line in Path("/proc/stat").read_text().splitlines():
            if line.startswith("btime "):
                return float(line.split()[1])
    except OSError:
        pass
    return time.time()


BOOT_TIME = boot_time()


def read_text(path, default=""):
    try:
        return Path(path).read_text(errors="replace")
    except OSError:
        return default


def readlink(path):
    try:
        return os.readlink(path)
    except OSError:
        return ""


def user_name(uid):
    try:
        return pwd.getpwuid(uid).pw_name
    except KeyError:
        return str(uid)


def process_start_epoch(pid):
    raw = read_text(f"/proc/{pid}/stat")
    if not raw:
        return None
    close = raw.rfind(")")
    fields = raw[close + 2:].split() if close >= 0 else []
    if len(fields) < 20:
        return None
    try:
        return BOOT_TIME + int(fields[19]) / CLK_TCK
    except (TypeError, ValueError):
        return None


def read_process(pid):
    base = Path("/proc") / str(pid)
    raw = read_text(base / "stat")
    if not raw:
        return None
    close = raw.rfind(")")
    fields = raw[close + 2:].split() if close >= 0 else []
    if len(fields) < 20 or fields[0] in ("Z", "X", "x"):
        return None
    try:
        ppid = int(fields[1])
        start_ticks = int(fields[19])
    except (TypeError, ValueError):
        return None
    status = read_text(base / "status")
    match = re.search(r"^Uid:\s+(\d+)", status, re.M)
    uid = int(match.group(1)) if match else -1
    raw_cmdline = b""
    try:
        raw_cmdline = (base / "cmdline").read_bytes()
    except OSError:
        pass
    argv = [part for part in raw_cmdline.decode("utf-8", "replace").split("\0") if part]
    return {
        "pid": int(pid),
        "ppid": ppid,
        "uid": uid,
        "user": user_name(uid),
        "comm": read_text(base / "comm").strip() or "?",
        "argv": argv,
        "cmdline": " ".join(argv),
        "start_ticks": start_ticks,
        "started": BOOT_TIME + start_ticks / CLK_TCK,
        "cgroup": read_text(base / "cgroup").strip(),
        "netns": readlink(base / "ns/net"),
        "pidns": readlink(base / "ns/pid"),
    }


def all_processes():
    try:
        entries = list(os.scandir("/proc"))
    except OSError:
        return
    for entry in entries:
        if entry.name.isdigit():
            process = read_process(entry.name)
            if process is not None:
                yield process


def parse_vllm_options(args):
    served = []
    model = None
    port = None
    host = None
    positional = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "--":
            positional.extend(args[index + 1:])
            break
        if arg.startswith("--"):
            key, separator, value = arg.partition("=")
            if key in ("--served-model-name", "--model", "--port", "--host"):
                if not separator:
                    index += 1
                    value = args[index] if index < len(args) else ""
                if key == "--served-model-name":
                    if value:
                        served.extend(part for part in value.split(",") if part)
                elif key == "--model":
                    model = value
                elif key == "--port":
                    try:
                        port = int(value)
                    except ValueError:
                        pass
                else:
                    host = value
        elif not arg.startswith("-"):
            positional.append(arg)
        index += 1
    if model is None and positional:
        model = positional[0]
    if port is None:
        port = 8000
    if host is None:
        host = "0.0.0.0"
    model = model or (served[0] if served else "")
    return {"model": model, "served": served, "port": port, "host": host}


def vllm_info(process):
    argv = process["argv"]
    for index, arg in enumerate(argv):
        if os.path.basename(arg) == "vllm" and index + 1 < len(argv) and argv[index + 1] == "serve":
            return parse_vllm_options(argv[index + 2:])
        if arg == "-m" and index + 1 < len(argv) and argv[index + 1].startswith("vllm."):
            return parse_vllm_options(argv[index + 2:])
    return None


def container_identity(cgroup):
    container_id = ""
    runtime = ""
    for line in cgroup.splitlines():
        path = line.split(":", 2)[-1]
        for pattern in CONTAINER_PATTERNS:
            match = pattern.search(path)
            if match:
                container_id = match.group(1)
                if "kubepods" in path or "cri-containerd" in path or "crio-" in path:
                    runtime = "kubernetes"
                elif "docker" in path or "libpod" in path:
                    runtime = "docker"
                break
        if container_id:
            break
    return container_id, runtime


def run_capture(command):
    attempts = [command]
    if os.geteuid() != 0 and shutil.which("sudo"):
        attempts.insert(0, ["sudo", "-n", *command])
    for candidate in attempts:
        try:
            result = subprocess.run(
                candidate, capture_output=True, text=True, timeout=8,
                env={**os.environ, "LANG": "C"},
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode == 0:
            return result.stdout
    return None


def run_json(command):
    output = run_capture(command)
    if not output:
        return None
    try:
        return json.loads(output)
    except json.JSONDecodeError:
        return None


def docker_container(container_id):
    data = run_json(["docker", "inspect", container_id])
    if isinstance(data, list) and data:
        item = data[0]
    elif isinstance(data, dict):
        item = data
    else:
        item = {}
    if not item:
        path = Path("/var/lib/docker/containers") / container_id / "config.v2.json"
        try:
            item = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            return None
    config = item.get("Config") or {}
    state = item.get("State") or {}
    name = str(item.get("Name") or "").lstrip("/")
    if not name:
        return None
    return {
        "runtime": "docker",
        "id": item.get("Id") or container_id,
        "name": name,
        "pod": "",
        "namespace": "",
        "image": config.get("Image") or "",
        "started": state.get("StartedAt") or "",
    }


def kubernetes_container(container_id):
    data = run_json(["crictl", "inspect", container_id])
    if not isinstance(data, dict):
        return None
    status = data.get("status") or {}
    labels = status.get("labels") or {}
    metadata = status.get("metadata") or {}
    image = status.get("image") or {}
    name = labels.get("io.kubernetes.container.name") or metadata.get("name") or ""
    pod = labels.get("io.kubernetes.pod.name") or ""
    namespace = labels.get("io.kubernetes.pod.namespace") or ""
    if not (name or pod):
        return None
    return {
        "runtime": "kubernetes",
        "id": status.get("id") or container_id,
        "name": name,
        "pod": pod,
        "namespace": namespace,
        "image": image.get("image") or "",
        "started": status.get("startedAt") or "",
    }


def pod_container_info(container_id):
    """Describe the current pod from downward-API env vars (no crictl/docker)."""
    pod = os.environ.get("POD_NAME", "")
    namespace = os.environ.get("POD_NAMESPACE", "")
    name = os.environ.get("CONTAINER_NAME") or os.environ.get("HOSTNAME", "")
    if not (pod or namespace or name):
        return None
    return {
        "runtime": "kubernetes",
        "id": container_id or os.environ.get("POD_UID", ""),
        "name": name,
        "pod": pod,
        "namespace": namespace,
        "image": "",
        "started": "",
    }


def container_info(container_id, runtime, cache):
    if not container_id:
        return pod_container_info("")
    if container_id in cache:
        return cache[container_id]
    info = None
    if runtime == "docker":
        info = docker_container(container_id)
        if info is None:
            info = kubernetes_container(container_id)
    else:
        info = kubernetes_container(container_id)
        if info is None:
            info = docker_container(container_id)
    if info is None:
        info = pod_container_info(container_id)
    if info is not None:
        info["id"] = container_id or info.get("id", "")
    cache[container_id] = info
    return info


def parse_proc_address(value):
    try:
        address, port = value.split(":")
    except ValueError:
        return None
    try:
        port_number = int(port, 16)
    except ValueError:
        return None
    if len(address) == 8:
        try:
            host = socket.inet_ntop(socket.AF_INET, struct.pack("<I", int(address, 16)))
        except (OSError, ValueError):
            return None
    elif len(address) == 32:
        try:
            raw = bytes.fromhex(address)
            raw = b"".join(raw[index:index + 4][::-1] for index in range(0, 16, 4))
            host = socket.inet_ntop(socket.AF_INET6, raw)
        except (OSError, ValueError):
            return None
    else:
        return None
    return host, port_number


def tcp_rows(pid):
    rows = []
    for name in ("tcp", "tcp6"):
        text = read_text(f"/proc/{pid}/net/{name}")
        for line in text.splitlines()[1:]:
            fields = line.split()
            if len(fields) < 10 or fields[3] != TCP_ESTABLISHED:
                continue
            local = parse_proc_address(fields[1])
            remote = parse_proc_address(fields[2])
            if local is None or remote is None or remote[1] == 0:
                continue
            rows.append({
                "local": local,
                "remote": remote,
                "inode": fields[9],
            })
    return rows


def socket_owner_map():
    owners = {}
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        fd_dir = f"/proc/{pid}/fd"
        try:
            fds = os.listdir(fd_dir)
        except OSError:
            continue
        for fd in fds:
            target = readlink(f"{fd_dir}/{fd}")
            if target.startswith("socket:[") and target.endswith("]"):
                owners[target[8:-1]] = pid
    return owners


def format_peer(address, port):
    host = address
    try:
        if ipaddress.ip_address(host).version == 6:
            host = f"[{host}]"
    except ValueError:
        pass
    return f"{host}:{port}"


def collect_clients(server, owners):
    same_netns = server["netns"] == HOST_NET_NS
    rows = tcp_rows(1 if same_netns else server["pid"])
    if not same_netns and not any(
            row["local"][1] == server["port"] or row["remote"][1] == server["port"]
            for row in rows):
        rows = tcp_rows(1)
    connections = {}
    for row in rows:
        local = row["local"]
        remote = row["remote"]
        if local[1] == server["port"]:
            key = (remote[0], remote[1], local[0], local[1])
            connections.setdefault(key, {})["server_inode"] = row["inode"]
        elif remote[1] == server["port"]:
            key = (local[0], local[1], remote[0], remote[1])
            connections.setdefault(key, {})["client_inode"] = row["inode"]

    clients = {}
    for key, connection in connections.items():
        client_pid = owners.get(connection.get("client_inode", ""))
        if client_pid == server["pid"]:
            continue
        peer_ip, peer_port, _, _ = key
        process = read_process(client_pid) if client_pid else None
        cache_key = f"pid:{client_pid}" if client_pid else f"peer:{peer_ip}:{peer_port}"
        item = clients.setdefault(cache_key, {
            "pid": client_pid,
            "user": process["user"] if process else "",
            "comm": process["comm"] if process else "",
            "cmdline": process["cmdline"] if process else "",
            "started": process["started"] if process else None,
            "peers": [],
        })
        peer = format_peer(peer_ip, peer_port)
        if peer not in item["peers"]:
            item["peers"].append(peer)
    return list(clients.values())


def api_metrics(server):
    host = server["host"]
    candidates = ["127.0.0.1"]
    if host not in ("0.0.0.0", "::", "127.0.0.1", "localhost", ""):
        candidates.append(host)
    if host == "::":
        candidates.append("::1")
    for candidate in candidates:
        url_host = f"[{candidate}]" if ":" in candidate else candidate
        url = f"http://{url_host}:{server['port']}/metrics"
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                text = response.read(2 * 1024 * 1024).decode("utf-8", "replace")
        except Exception:
            continue
        success = 0.0
        found = False
        for match in re.finditer(
                r"^vllm:request_success_total(?:\{[^\n]*\})?\s+([0-9.eE+-]+)$",
                text, re.M):
            success += float(match.group(1))
            found = True
        if not found:
            for match in re.finditer(
                    r"^vllm:request_total(?:\{[^\n]*\})?\s+([0-9.eE+-]+)$",
                    text, re.M):
                success += float(match.group(1))
                found = True
        if not found:
            continue
        running = sum(
            float(match.group(1)) for match in re.finditer(
                r"^vllm:num_requests_running(?:\{[^\n]*\})?\s+([0-9.eE+-]+)$",
                text, re.M,
            )
        )
        waiting = sum(
            float(match.group(1)) for match in re.finditer(
                r"^vllm:num_requests_waiting(?:\{[^\n]*\})?\s+([0-9.eE+-]+)$",
                text, re.M,
            )
        )
        return {"success_total": success, "running": running, "waiting": waiting}
    return None


def state_path():
    configured = os.environ.get("VLLM_PROBE_STATE")
    if configured:
        return Path(configured).expanduser()
    home = Path(os.environ.get("HOME", str(Path.home())))
    if os.geteuid() == 0 and os.environ.get("SUDO_USER"):
        try:
            home = Path(pwd.getpwnam(os.environ["SUDO_USER"]).pw_dir)
        except KeyError:
            pass
    return home / ".cache" / "ucm" / "vllm_probe_state.json"


def load_state(path):
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    data.setdefault("version", 1)
    data.setdefault("servers", {})
    return data


def save_state(path, state):
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
        temporary.write_text(json.dumps(state, ensure_ascii=False, indent=2))
        os.replace(temporary, path)
        if os.geteuid() == 0 and os.environ.get("SUDO_UID"):
            uid = int(os.environ["SUDO_UID"])
            gid = int(os.environ.get("SUDO_GID", uid))
            try:
                home = Path(pwd.getpwuid(uid).pw_dir)
                path.parent.relative_to(home)
            except (KeyError, ValueError):
                pass
            else:
                os.chown(path.parent, uid, gid)
            os.chown(path, uid, gid)
    except (OSError, ValueError) as error:
        print(f"WARN: cannot save probe state to {path}: {error}", file=sys.stderr)


def update_state(state, key, started, clients, metrics):
    servers = state.setdefault("servers", {})
    entry = servers.get(key)
    if not isinstance(entry, dict) or entry.get("started") != started:
        entry = {
            "started": started,
            "clients": {},
            "last_client_seen": 0,
            "metrics_total": None,
            "last_metrics_change": 0,
        }
        servers[key] = entry
    now = time.time()
    cached_clients = entry.setdefault("clients", {})
    for client in clients:
        client_key = (
            f"pid:{client['pid']}:{client.get('started') or ''}"
            if client["pid"] else f"peer:{client['peers'][0]}"
        )
        previous = cached_clients.get(client_key, {})
        cached_clients[client_key] = {
            "first_seen": previous.get("first_seen", now),
            "last_seen": now,
            "pid": client["pid"],
            "user": client["user"],
            "comm": client["comm"],
            "cmdline": client["cmdline"],
            "peers": client["peers"],
        }
        entry["last_client_seen"] = now
    oldest = now - 7 * 24 * 3600
    for client_key in list(cached_clients):
        if cached_clients[client_key].get("last_seen", 0) < oldest:
            del cached_clients[client_key]
    if metrics is not None:
        previous_total = entry.get("metrics_total")
        if previous_total is not None and metrics["success_total"] > previous_total:
            entry["last_metrics_change"] = now
        elif previous_total is not None and metrics["success_total"] < previous_total:
            entry["last_metrics_change"] = 0
        entry["metrics_total"] = metrics["success_total"]
    entry["last_activity"] = max(
        entry.get("last_client_seen", 0),
        entry.get("last_metrics_change", 0),
    )
    return entry


def cgroup_path(cgroup):
    paths = [line.split(":", 2)[-1] for line in cgroup.splitlines()]
    paths = [path for path in paths if path and path != "/"]
    return max(paths, key=len, default="/")


def format_time(epoch):
    if not epoch:
        return "-"
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(epoch))


def format_duration(seconds):
    seconds = max(0, int(seconds))
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    parts = []
    if days:
        parts.append(f"{days}d")
    if hours or days:
        parts.append(f"{hours}h")
    parts.append(f"{minutes}m")
    return " ".join(parts)


def short(text, limit=180):
    return text if len(text) <= limit else text[:limit - 3] + "..."


def container_text(info, cgroup=""):
    if not info:
        if cgroup and any(name in cgroup for name in (
                "kubepods", "docker", "containerd", "libpod", "cri-containerd")):
            return "container (unresolved)"
        return "host"
    parts = []
    if info.get("pod"):
        parts.append(f"pod={info['pod']}")
    if info.get("namespace"):
        parts.append(f"namespace={info['namespace']}")
    if info.get("name"):
        parts.append(f"container={info['name']}")
    if info.get("runtime"):
        parts.append(f"runtime={info['runtime']}")
    parts.append(f"id={str(info.get('id', ''))[:12]}")
    return " ".join(parts)


def service_payload(server, mgr, entry, clients, metrics):
    info = mgr["container"]
    client_payload = []
    for client in clients:
        client_process = read_process(client["pid"]) if client["pid"] else None
        client_container = None
        if client_process:
            client_cid, client_runtime = container_identity(client_process["cgroup"])
            if client_cid:
                client_container = container_info(
                    client_cid, client_runtime, mgr["cache"]
                )
        client_payload.append({
            "pid": client["pid"],
            "user": client["user"],
            "comm": client["comm"],
            "cmdline": client["cmdline"],
            "peers": client["peers"],
            "container": client_container,
        })
    cached = entry.get("clients", {}) if entry else {}
    last_cached = max(cached.values(), key=lambda item: item.get("last_seen", 0),
                      default=None)
    last_activity = max(
        entry.get("last_client_seen", 0) if entry else 0,
        entry.get("last_metrics_change", 0) if entry else 0,
    )
    source = ""
    if entry and entry.get("last_client_seen", 0) >= entry.get("last_metrics_change", 0) and entry.get("last_client_seen"):
        source = "client connection"
    elif entry and entry.get("last_metrics_change"):
        source = "request metrics"
    return {
        "pid": server["pid"],
        "user": server["user"],
        "started": format_time(server["started"]),
        "started_epoch": server["started"],
        "uptime_seconds": max(0, time.time() - server["started"]),
        "model": server["model"],
        "served_model_names": server["served"],
        "endpoint": f"{server['host']}:{server['port']}",
        "cgroup": cgroup_path(server["cgroup"]),
        "container": info,
        "clients": client_payload,
        "last_client": last_cached,
        "last_activity": format_time(last_activity) if last_activity else "",
        "last_activity_epoch": last_activity,
        "last_activity_source": source,
        "metrics": metrics,
    }


def print_human(servers, payloads):
    if not servers:
        print("No vllm serve process found.")
        return
    print(f"vLLM services: {len(servers)}  ({time.strftime('%Y-%m-%d %H:%M:%S')})")
    if os.geteuid() != 0:
        print("WARN: not running as root; other users' processes and container names may be incomplete.")
    for index, (server, payload, entry) in enumerate(zip(servers, payloads, [p["_state"] for p in payloads]), 1):
        print()
        print(f"[{index}] pid={server['pid']} user={server['user']} "
              f"started={payload['started']} uptime={format_duration(payload['uptime_seconds'])}")
        print(f"    model     : {payload['model'] or '-'}")
        served = ", ".join(payload["served_model_names"])
        print(f"    served    : {served or '-'}")
        print(f"    endpoint  : {payload['endpoint']}")
        print(f"    container : {container_text(payload['container'], payload['cgroup'])}")
        print(f"    cgroup    : {payload['cgroup']}")
        if payload["clients"]:
            print("    clients   :")
            for client in payload["clients"]:
                label = (f"pid={client['pid']} user={client['user']} "
                         f"comm={client['comm']}") if client["pid"] else "remote"
                peers = ", ".join(client["peers"])
                print(f"      - {label} peer={peers}")
                if client["cmdline"]:
                    print(f"        cmd: {short(client['cmdline'])}")
                if client["container"]:
                    print(f"        {container_text(client['container'])}")
        else:
            print("    clients   : none active")
            cached = entry.get("clients", {}) if entry else {}
            if cached:
                last = max(cached.values(), key=lambda item: item.get("last_seen", 0))
                who = f"pid={last.get('pid')}" if last.get("pid") else "remote"
                print(f"    last client: {format_time(last.get('last_seen'))} "
                      f"({who}, peer={', '.join(last.get('peers', []))})")
        if payload["last_activity"]:
            print(f"    last activity: {payload['last_activity']} "
                  f"({payload['last_activity_source'] or 'cached'})")
        else:
            print("    last activity: unknown (first probe run; future runs will cache it)")
        if payload["metrics"]:
            metrics = payload["metrics"]
            print(f"    requests  : running={metrics['running']:.0f} "
                  f"waiting={metrics['waiting']:.0f} success_total={metrics['success_total']:.0f}")
        else:
            print("    requests  : metrics unavailable")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    parser.add_argument("--state", default=None, help="override state/cache JSON path")
    parser.add_argument("--port", type=int, help="only probe this vLLM port")
    parser.add_argument("--no-state", action="store_true", help="do not read or write history")
    args = parser.parse_args(argv)

    state_file = Path(args.state).expanduser() if args.state else state_path()
    state = {} if args.no_state else load_state(state_file)
    cache = {}
    servers = []
    for process in all_processes():
        info = vllm_info(process)
        if info is None:
            continue
        if args.port is not None and info["port"] != args.port:
            continue
        container_id, runtime = container_identity(process["cgroup"])
        process.update(info)
        process["container_id"] = container_id
        process["runtime"] = runtime
        process["container"] = container_info(container_id, runtime, cache)
        servers.append(process)
    servers.sort(key=lambda item: item["started"])

    owners = socket_owner_map()
    payloads = []
    for server in servers:
        clients = collect_clients(server, owners)
        metrics = api_metrics(server)
        key = f"{server['container_id'] or 'host'}:{server['port']}"
        entry = None
        if not args.no_state:
            entry = update_state(state, key, server["started"], clients, metrics)
        manager = {"container": server["container"], "cache": cache}
        payload = service_payload(server, manager, entry, clients, metrics)
        payload["_state"] = entry
        payloads.append(payload)

    if not args.no_state:
        save_state(state_file, state)

    if args.json:
        services = []
        for payload in payloads:
            clean = {key: value for key, value in payload.items() if key != "_state"}
            services.append(clean)
        print(json.dumps({
            "host": socket.gethostname(),
            "generated": format_time(time.time()),
            "services": services,
        }, ensure_ascii=False, indent=2))
    else:
        print_human(servers, payloads)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except BrokenPipeError:
        sys.exit(0)
