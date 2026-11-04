"""Identify and stop only an owned host server on Linux, without accelerator tools.

Start a launch chain with UCM_HOST_SERVER_TOKEN set in its environment at exec.
Identify prints ``PID STARTTIME`` (Linux /proc starttime ticks); stop is silent.
Exit status is zero on success, one on refusal/incomplete cleanup, two on bad CLI.
"""

import argparse
import errno
import math
import os
from pathlib import Path
import re
import signal
import sys
import time
from typing import NamedTuple


TOKEN_ENV = b"UCM_HOST_SERVER_TOKEN="


class Process(NamedTuple):
    pid: int
    start: int
    ppid: int
    uid: int
    state: str

    @property
    def identity(self):
        return self.pid, self.start

    @property
    def alive(self):
        return self.state not in ("Z", "X", "x")


def read_process(pid):
    try:
        path = Path("/proc") / str(pid)
        fields = (path / "stat").read_text().rsplit(")", 1)[1].split()
        return Process(pid, int(fields[19]), int(fields[1]), path.stat().st_uid,
                       fields[0])
    except (OSError, ValueError, IndexError):
        return None


def processes():
    for path in Path("/proc").iterdir():
        if path.name.isdigit():
            process = read_process(int(path.name))
            if process is not None and process.uid == os.getuid() and process.alive:
                yield process


def same_process(process):
    current = read_process(process.pid)
    return (current is not None and current.alive
            and current.uid == process.uid == os.getuid()
            and current.start == process.start)


def has_token(process, token):
    try:
        environment = (Path("/proc") / str(process.pid) / "environ").read_bytes()
        return (TOKEN_ENV + os.fsencode(token) in environment.split(b"\0")
                and same_process(process))
    except OSError:
        return False


def is_server(process, port):
    try:
        raw = (Path("/proc") / str(process.pid) / "cmdline").read_bytes()
        if not raw.endswith(b"\0"):
            return False
        argv = raw[:-1].split(b"\0")
        executable = os.path.basename(argv[0])
        index = 0
        if re.fullmatch(rb"python(?:[0-9]+(?:\.[0-9]+)*)?", executable):
            index = 1
            while index < len(argv) and argv[index].startswith(b"-"):
                option = argv[index]
                if option in (b"-W", b"-X"):
                    index += 2
                elif option in (b"-u", b"-B", b"-E", b"-I", b"-s", b"-S",
                                b"-O", b"-OO", b"-q", b"-v", b"-b", b"-bb"):
                    index += 1
                else:
                    return False
        if (index + 1 >= len(argv) or os.path.basename(argv[index]) != b"vllm"
                or argv[index + 1] != b"serve"):
            return False
        ports = []
        index += 2
        while index < len(argv):
            arg = argv[index]
            if arg == b"--":
                break
            if arg == b"--port":
                index += 1
                if index == len(argv):
                    return False
                ports.append(argv[index])
            elif arg.startswith(b"--port="):
                ports.append(arg[len(b"--port="):])
            index += 1
        return bool(ports) and all(p == str(port).encode() for p in ports) and same_process(process)
    except (OSError, IndexError):
        return False


def identify(port, pid=None, start_time=None):
    if pid is not None:
        process = read_process(pid)
        if (process is None or process.start != start_time
                or not same_process(process) or not is_server(process, port)):
            raise RuntimeError("explicit PID/starttime is not the owned server; refusing fallback")
        return process
    matches = [p for p in processes() if is_server(p, port)]
    if len(matches) != 1:
        raise RuntimeError(f"expected one owned vllm serve on port {port}, found {len(matches)}")
    return matches[0]


def protected_pids():
    protected = {os.getpid(), os.getppid()}
    pid = os.getppid()
    while pid > 1:
        process = read_process(pid)
        if process is None or process.ppid in protected:
            break
        pid = process.ppid
        protected.add(pid)
    protected.add(1)
    return protected


def send_signal(process, action, protected):
    if process.pid in protected or not same_process(process):
        return
    fd = None
    try:
        if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            try:
                fd = os.pidfd_open(process.pid, 0)
            except OSError as error:
                if error.errno != errno.ENOSYS:
                    raise
            if fd is not None:
                # Validate after opening: the fd must not refer to a recycled PID.
                if same_process(process):
                    signal.pidfd_send_signal(fd, action)
                return
        # Older Python/kernel fallback still checks identity immediately before kill.
        if same_process(process):
            os.kill(process.pid, action)
    except ProcessLookupError:
        pass
    finally:
        if fd is not None:
            os.close(fd)


def stop(port, pid=None, start_time=None, token=None, grace_seconds=90, term_seconds=2):
    protected = protected_pids()
    root = None
    if token is not None:
        if not token or "\0" in token:
            raise RuntimeError("token must be nonempty and contain no NUL")
        if pid is not None:
            root = read_process(pid)
            if root is not None:
                if root.uid != os.getuid() or root.start != start_time:
                    raise RuntimeError("explicit PID/starttime changed; refusing fallback")
                if root.alive and not has_token(root, token):
                    raise RuntimeError("explicit root does not match token")
    else:
        root = identify(port, pid, start_time)
    if root is not None and root.pid in protected:
        raise RuntimeError("refusing to stop self or an ancestor")

    known = {}
    if root is not None and root.alive:
        known[root.identity] = root

    def snapshot():
        current = list(processes())
        for process in current:
            if process.pid not in protected and token is not None and has_token(process, token):
                known[process.identity] = process
        # Only live, identity-checked parents can confer ownership. Keep old child
        # identities even after reparenting, but never follow a reused parent's PID.
        parents = {p.pid for p in known.values() if same_process(p)}
        remaining = [p for p in current if p.pid not in protected]
        while remaining:
            children = [p for p in remaining if p.ppid in parents]
            if not children:
                break
            for child in children:
                known[child.identity] = child
                parents.add(child.pid)
            remaining = [p for p in remaining if p.pid not in parents]
        return [p for p in known.values() if same_process(p)]

    survivors = snapshot()  # Capture the tree before SIGINT can orphan children.
    if root is not None and root.alive:
        send_signal(root, signal.SIGINT, protected)

    for action, seconds in ((None, grace_seconds), (signal.SIGTERM, term_seconds),
                            (signal.SIGKILL, term_seconds)):
        deadline = time.monotonic() + seconds
        signaled = set()
        while True:
            survivors = snapshot()
            if not survivors:
                return
            if action is not None:
                for process in survivors:
                    if process.identity not in signaled:
                        send_signal(process, action, protected)
                        signaled.add(process.identity)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(0.05, remaining))
    if snapshot():
        raise RuntimeError("owned processes survived bounded SIGKILL wait")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("identify", "stop"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument("--port", type=int, required=True)
        subparser.add_argument("--pid", type=int)
        subparser.add_argument("--start-time", type=int)
        if command == "stop":
            subparser.add_argument("--token")
            subparser.add_argument("--grace-seconds", type=float, default=90)
            subparser.add_argument("--term-seconds", type=float, default=2)
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if (args.pid is None) != (args.start_time is None):
        parser.error("--pid and --start-time must be supplied together")
    if args.pid is not None and (args.pid <= 0 or args.start_time < 0):
        parser.error("invalid PID/starttime")
    if args.command == "stop" and any(
            not math.isfinite(value) or value < 0
            for value in (args.grace_seconds, args.term_seconds)):
        parser.error("timeouts must be finite and nonnegative")
    try:
        if args.command == "identify":
            process = identify(args.port, args.pid, args.start_time)
            print(process.pid, process.start)
        else:
            stop(args.port, args.pid, args.start_time, args.token,
                 args.grace_seconds, args.term_seconds)
        return 0
    except (OSError, RuntimeError) as error:
        print(f"host_server_process: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
