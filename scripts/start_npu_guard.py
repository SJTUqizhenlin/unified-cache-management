"""Clear same-user NPU occupants only before and during server startup.

Inspect all NPUs: python scripts/start_npu_guard.py --dry-run
Launch: python scripts/start_npu_guard.py -- bash server_test_host.sh --ucm off
Include root-owned occupants: add --sudo before the command separator.
All unrelated same-user NPU processes on selected devices are eligible for killing,
including active jobs. --sudo also permits root-owned jobs, not other users' jobs.
Do not run the whole launcher with sudo.
"""

import argparse
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.request
import uuid


def parse_processes(text, devices):
    processes = set()
    in_table = False
    for line in text.splitlines():
        if "Process id" in line:
            in_table = True
            continue
        if not in_table:
            continue
        fields = [field.strip() for field in line.split("|")[1:-1]]
        if (len(fields) >= 4 and re.fullmatch(r"\d+\s+\d+", fields[0])
                and fields[1].isdigit()):
            if devices is None or int(fields[0].split()[0]) in devices:
                processes.add(int(fields[1]))
    if not in_table:
        raise RuntimeError("npu-smi process table not recognized; refusing to kill")
    return processes


def process_info(pid):
    path = Path(f"/proc/{pid}")
    stat = (path / "stat").read_text()
    fields = stat[stat.rfind(")") + 2:].split()
    return path.stat().st_uid, int(fields[1]), int(fields[19])


def is_protected(pid, root_pid, token):
    if pid in (os.getpid(), os.getppid(), root_pid):
        return True
    if token and Path(f"/proc/{pid}").stat().st_uid == os.getuid():
        try:
            environment = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
            if f"NPU_STARTUP_GUARD_TOKEN={token}".encode() in environment:
                return True
        except PermissionError:
            # An unreadable environment must never permit an accidental kill.
            return True
    seen = set()
    while pid > 1 and pid not in seen:
        if pid in (root_pid, os.getpid()):
            return True
        seen.add(pid)
        _, pid, _ = process_info(pid)
    return False


def signal_root_process(pid, identity, action):
    # Open and validate the pidfd inside the privileged helper, never use sudo kill PID.
    code = """
import os, signal, sys
pid, uid, start, sig = map(int, sys.argv[1:])
fd = os.pidfd_open(pid)
try:
    path = f'/proc/{pid}'
    with open(path + '/stat') as stream:
        stat = stream.read()
    current_start = int(stat[stat.rfind(')') + 2:].split()[19])
    if os.stat(path).st_uid != uid or current_start != start:
        raise SystemExit('process identity changed; refusing to signal')
    signal.pidfd_send_signal(fd, sig)
finally:
    os.close(fd)
"""
    subprocess.run(
        ["sudo", "-n", "--", sys.executable, "-c", code,
         str(pid), str(identity[0]), str(identity[2]), str(int(action))],
        check=True, timeout=15,
    )


def sweep(devices, root_pid, token, pending, grace, dry_run=False, allow_root=False):
    result = subprocess.run(
        ["npu-smi", "info"], check=True, capture_output=True, text=True, timeout=15
    )
    occupants = parse_processes(result.stdout, devices)
    blocked = False
    now = time.monotonic()
    for pid in sorted(occupants):
        try:
            identity = process_info(pid)
            if is_protected(pid, root_pid, token):
                continue
            blocked = True
            privileged = identity[0] == 0 and allow_root
            if identity[0] != os.getuid() and not privileged:
                print(f"[guard] PID {pid} belongs to UID {identity[0]}; not killing", flush=True)
                continue
            key = (pid, identity[2])
            if key in pending and now - pending[key] < grace:
                continue
            action = signal.SIGKILL if key in pending else signal.SIGTERM
            print(f"[guard] {'would send' if dry_run else 'sending'} {action.name} to PID {pid}", flush=True)
            if dry_run:
                continue
            if privileged:
                if process_info(pid) != identity or is_protected(pid, root_pid, token):
                    continue
                signal_root_process(pid, identity, action)
                pending.setdefault(key, now)
                continue
            # pidfd keeps the signal tied to this process, even if its PID is reused.
            fd = os.pidfd_open(pid)
            try:
                if process_info(pid) != identity or is_protected(pid, root_pid, token):
                    continue
                signal.pidfd_send_signal(fd, action)
                pending.setdefault(key, now)
            finally:
                os.close(fd)
        except (FileNotFoundError, ProcessLookupError):
            continue
    return blocked


def healthy(opener, port):
    try:
        with opener.open(f"http://127.0.0.1:{port}/health", timeout=1) as response:
            return response.status == 200
    except OSError:
        return False


def server_config(command):
    if Path(command[0]).name == "bash" and len(command) > 1:
        script = Path(command[1])
    else:
        script = Path(command[0])
    source = script.read_text()
    values = {}
    for name in ("DEVICES", "PORT"):
        # Only accept the launcher's literal ${NAME:-default} assignments.
        match = re.search(
            rf'^\s*{name}="\$\{{{name}:-([0-9,]+)\}}"\s*(?:#.*)?$',
            source, re.MULTILINE,
        )
        if name in os.environ:
            values[name] = os.environ[name]
        elif match:
            values[name] = match[1]
        else:
            raise RuntimeError(f"cannot safely determine {name} from {script}; set it in the environment")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, help="override the server port")
    parser.add_argument("--interval", type=float, default=2)
    parser.add_argument("--grace", type=float, default=15)
    parser.add_argument("--preflight-timeout", type=float, default=60)
    parser.add_argument("--startup-timeout", type=float, default=1800)
    parser.add_argument("--dry-run", action="store_true", help="inspect once; do not kill or start server")
    parser.add_argument("--sudo", action="store_true", help="also allow killing root-owned NPU occupants using sudo")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if min(args.interval, args.grace, args.preflight_timeout, args.startup_timeout) <= 0:
        parser.error("timeouts must be positive")
    if os.getuid() == 0:
        parser.error("run as the server user, not root or sudo")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        parser.error("Linux pidfd support is required for safe signaling")
    pending = {}
    if args.dry_run:
        sweep(None, None, None, pending, args.grace, dry_run=True, allow_root=args.sudo)
        return 0
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        command = ["bash", str(Path(__file__).resolve().parents[1] / "server_test_host.sh")]
    config = server_config(command)
    try:
        devices = {int(value) for value in config["DEVICES"].split(",")}
        args.port = args.port if args.port is not None else int(config["PORT"])
        if not devices or min(devices) < 0 or not 0 < args.port < 65536:
            raise ValueError("invalid device IDs or port")
    except ValueError as error:
        parser.error(str(error))
    print(f"[guard] server configuration: DEVICES={config['DEVICES']} PORT={args.port}", flush=True)
    with socket.socket() as probe:
        probe.settimeout(1)
        if probe.connect_ex(("127.0.0.1", args.port)) == 0:
            parser.error(f"port {args.port} is already in use; no processes were killed")

    if args.sudo:
        subprocess.run(["sudo", "-v"], check=True)

    deadline = time.monotonic() + args.preflight_timeout
    while sweep(devices, None, None, pending, args.grace, allow_root=args.sudo):
        if time.monotonic() >= deadline:
            raise RuntimeError("selected NPUs remain occupied; server was not started")
        time.sleep(args.interval)
    # Allow driver-side HBM accounting to settle before the server's own check.
    time.sleep(args.interval)
    token = uuid.uuid4().hex
    env = dict(os.environ, DEVICES=config["DEVICES"], PORT=str(args.port), NPU_STARTUP_GUARD_TOKEN=token)
    server = subprocess.Popen(command, env=env)
    print(f"[guard] server PID {server.pid}; watching NPUs {sorted(devices)} until ready", flush=True)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    deadline = time.monotonic() + args.startup_timeout
    try:
        while server.poll() is None:
            if healthy(opener, args.port):
                print("[guard] server ready; NPU monitoring and killing stopped", flush=True)
                break
            if time.monotonic() >= deadline:
                print("[guard] startup timeout; monitoring stopped, server left running", flush=True)
                break
            try:
                sweep(devices, server.pid, token, pending, args.grace, allow_root=args.sudo)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                print(f"[guard] monitoring stopped safely: {error}", flush=True)
                break
            time.sleep(args.interval)
        return server.wait()
    except KeyboardInterrupt:
        # The foreground terminal also delivers SIGINT to the server.
        print("[guard] interrupted; monitoring stopped", flush=True)
        return 130


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"[guard] ERROR: {error}", file=sys.stderr)
        sys.exit(1)
