#!/usr/bin/env python3
"""Remove unused uc_shm_cache_* regular files owned by an explicit UID (Pod version).

Pod caller example (usually already root, no sudo):
  timeout 20s python3 scripts/cleanup_cache_files_pod.py --uid 0 --allow-root \
      --directory /dev/shm
Repeat --directory for custom cache directories; add --dry-run to only report.
Unlike the host version, UID 0 is accepted when --allow-root is given. The same
maps/FD safety rules apply: no deletion starts until the entire /proc scan
succeeds, and mapped or open files are protected by device/inode, never path.
Pod /proc only exposes the pod's own processes, which is exactly the scope this
container can safely reason about.

Assumes no concurrent new launches/openers: /proc scanning and the final lstat
cannot make checking usage and unlinking atomic. Stop old servers first and
serialize startup. A vanished process/FD is harmless; other scan errors abort.
"""

import argparse
from contextlib import ExitStack
import errno
import os
from pathlib import Path
import stat
import sys


TRANSIENT = {errno.ENOENT, errno.ESRCH}
PREFIX = "uc_shm_cache_"


def scan_protected(proc_root=Path("/proc")):
    """Take one full process pass; propagate every nontransient scan error."""
    protected = set()
    with os.scandir(proc_root) as processes:
        for process in processes:
            if not process.name.isdecimal():
                continue
            try:
                with open(Path(process.path) / "maps", encoding="utf-8",
                          errors="surrogateescape") as maps:
                    for line in maps:
                        fields = line.split(None, 5)
                        if len(fields) < 5:
                            raise ValueError(f"Malformed maps entry in {process.path}")
                        major, minor = fields[3].split(":")
                        device = os.makedev(int(major, 16), int(minor, 16))
                        inode = int(fields[4], 10)
                        if inode < 0:
                            raise ValueError(f"Negative maps inode in {process.path}")
                        if inode:
                            protected.add((device, inode))
                with os.scandir(Path(process.path) / "fd") as descriptors:
                    for descriptor in descriptors:
                        try:
                            info = descriptor.stat(follow_symlinks=True)
                        except OSError as exc:
                            if exc.errno in TRANSIENT:
                                continue
                            raise
                        protected.add((info.st_dev, info.st_ino))
            except OSError as exc:
                if exc.errno not in TRANSIENT:
                    raise
    return protected


def cleanup(uid, directories, dry_run=False, allow_root=False):
    if uid < 0 or (uid == 0 and not allow_root):
        raise ValueError(
            "--uid must be a nonroot UID greater than zero (use --allow-root for UID 0)")
    with ExitStack() as stack:
        candidates = []
        try:
            for directory in dict.fromkeys(map(Path, directories)):
                try:
                    directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
                except FileNotFoundError:
                    continue
                stack.callback(os.close, directory_fd)
                with os.scandir(directory_fd) as entries:
                    for entry in entries:
                        if not entry.name.startswith(PREFIX):
                            continue
                        try:
                            info = os.stat(entry.name, dir_fd=directory_fd,
                                           follow_symlinks=False)
                        except OSError as exc:
                            if exc.errno in TRANSIENT:
                                continue
                            raise
                        if stat.S_ISREG(info.st_mode) and info.st_uid == uid:
                            candidates.append((directory_fd, directory / entry.name, info))
            protected = scan_protected()
        except (OSError, ValueError, OverflowError) as exc:
            print(f"WARNING: cache cleanup skipped all deletion: {exc}", file=sys.stderr)
            return 1

        result = 0
        for directory_fd, path, original in candidates:
            if (original.st_dev, original.st_ino) in protected:
                continue
            try:
                current = os.stat(path.name, dir_fd=directory_fd, follow_symlinks=False)
                if ((current.st_dev, current.st_ino, current.st_uid, current.st_mode) !=
                        (original.st_dev, original.st_ino, original.st_uid, original.st_mode)):
                    continue
                if not dry_run:
                    os.unlink(path.name, dir_fd=directory_fd)
                print(f"{'Would remove' if dry_run else 'Removed'} {path}")
            except OSError as exc:
                if exc.errno not in TRANSIENT:
                    print(f"WARNING: cache cleanup could not remove {path}: {exc}",
                          file=sys.stderr)
                    result = 1
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uid", required=True, type=int, help="Original nonroot server UID")
    parser.add_argument("--directory", required=True, action="append", type=Path,
                        help="Cache directory (repeat for shm, hugepages, and custom paths)")
    parser.add_argument("--dry-run", action="store_true", help="Report without unlinking")
    parser.add_argument("--allow-root", action="store_true",
                        help="Allow cleaning files owned by UID 0 (root-in-pod use)")
    args = parser.parse_args(argv)
    if args.uid < 0 or (args.uid == 0 and not args.allow_root):
        parser.error("--uid must be a nonroot UID greater than zero unless --allow-root is set")
    return cleanup(args.uid, args.directory, args.dry_run, args.allow_root)


if __name__ == "__main__":
    sys.exit(main())
