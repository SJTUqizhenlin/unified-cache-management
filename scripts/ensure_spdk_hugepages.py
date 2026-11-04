#!/usr/bin/env python3
"""Budget explicit Cache hugepages, including IPC or Empty backends.

Run as the server user, not through sudo. Only SPDK's setup.sh is privileged.
Pass target memory as zero for Cache|Empty. For IPC, stop the old target first.
FAWA capacity is a TOTAL across FA/WA, not twice the configured capacity.
Use --cache-multiplier 1 only for a confirmed shared-total layout (e.g. V4 MLA).
The default device-count multiplier conservatively covers per-rank layouts.
This checks global availability, not NUMA placement, cgroup limits or memlock.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

import yaml


HUGEPAGES = Path("/sys/kernel/mm/hugepages/hugepages-2048kB")


def validate_mount(directory):
    directory = Path(directory).resolve(strict=True)
    if not directory.is_dir():
        raise RuntimeError(f"{directory} is not a directory")
    # Select the deepest mount, so an overmounted subdirectory cannot pass.
    selected = None
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        left, right = line.split(" - ", 1)
        fields, fs = left.split(), right.split()
        mount = Path(re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), fields[4]))
        if directory == mount or mount in directory.parents:
            if selected is None or len(mount.parts) >= selected[0]:
                selected = (len(mount.parts), fields[5].split(","), fs)
    if selected is None or selected[2][0] != "hugetlbfs":
        raise RuntimeError(
            f"{directory} must be on an existing hugetlbfs mount; ask an administrator "
            "to mount it with -t hugetlbfs -o pagesize=2M and grant the server user access"
        )
    _, options, fs = selected
    super_options = fs[2].split(",")
    size = next((o.split("=", 1)[1].lower() for o in super_options
                 if o.startswith("pagesize=")), None)
    if size is None:
        match = re.search(r"^Hugepagesize:\s+(\d+) kB$",
                          Path("/proc/meminfo").read_text(), re.MULTILINE)
        size = f"{match[1]}k" if match else "unknown"
    if size not in {"2m", "2048k", "2097152"}:
        raise RuntimeError(f"{directory}: hugetlbfs must use 2 MiB pages, got {size}")
    if "ro" in options or "ro" in super_options or not os.access(directory, os.W_OK | os.X_OK):
        raise RuntimeError(
            f"{directory} must be writable/searchable by the original server user; "
            "ask an administrator to correct mount permissions (not changed by preflight)"
        )


def cache_budget(config, multiplier):
    if not isinstance(config, dict) or not isinstance(config.get("ucm_connectors"), list):
        raise RuntimeError("YAML must contain a ucm_connectors list")
    total = 0
    mounts = set()
    for entry in config["ucm_connectors"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("ucm_connector_config"), dict):
            raise RuntimeError("Each connector must contain a ucm_connector_config mapping")
        cfg = entry["ucm_connector_config"]
        pipeline = [s.strip().lower() for s in str(cfg.get("store_pipeline", "")).split("|")]
        ipc = "spdk" in pipeline and str(cfg.get("spdk_trid", "")).strip().startswith("ipc:")
        explicit_cache = "cache" in pipeline and cfg.get("shm_use_hugepage_file") is True
        if not ipc and not explicit_cache:
            continue
        if ipc and pipeline != ["cache", "spdk"]:
            raise RuntimeError("IPC preflight supports only store_pipeline: Cache|Spdk")
        for flag in ("share_buffer_enable", "shm_use_hugepage_file"):
            if cfg.get(flag) is not True:
                raise RuntimeError(f"Hugepage Cache requires explicit {flag}: true")
        # MADV_HUGEPAGE is advisory and cannot replace shm_use_hugepage_file.
        capacity = cfg.get("cache_buffer_capacity_gb")
        if type(capacity) is not int or capacity <= 0:
            raise RuntimeError("cache_buffer_capacity_gb must be an explicit positive integer (GiB total)")
        directory = cfg.get("shm_hugepage_dir", "/dev/hugepages")
        if not isinstance(directory, str) or not directory.startswith("/"):
            raise RuntimeError("shm_hugepage_dir must be an absolute path")
        mounts.add(directory)
        total += capacity * 1024 * multiplier
    for directory in sorted(mounts):
        validate_mount(directory)
    return total


def page_counts():
    counts = [int((HUGEPAGES / name).read_text().strip()) for name in
              ("free_hugepages", "resv_hugepages", "nr_hugepages")]
    if any(n < 0 for n in counts):
        raise RuntimeError("Invalid negative 2 MiB hugepage counters")
    free, reserved, total = counts
    return max(0, free - reserved), free, reserved, total


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--target-mem-mb", type=int, default=8192)
    parser.add_argument("--devices", required=True, help="Server DEVICES comma-separated list")
    parser.add_argument("--setup-script", required=True, type=Path)
    parser.add_argument("--reserve-mb", type=int, default=2048)
    parser.add_argument("--cache-multiplier", type=int,
                        help="Cache copies; default: device count. Use 1 for confirmed shared-total MLA")
    args = parser.parse_args(argv)
    devices = [d.strip() for d in args.devices.split(",")]
    if any(not d.isdecimal() for d in devices) or len(set(map(int, devices))) != len(devices):
        parser.error("--devices must contain distinct nonnegative device IDs")
    if args.target_mem_mb < 0 or args.reserve_mb < 0:
        parser.error("--target-mem-mb and --reserve-mb must be nonnegative")
    if args.cache_multiplier is not None and args.cache_multiplier <= 0:
        parser.error("--cache-multiplier must be positive")
    multiplier = args.cache_multiplier or len(devices)
    try:
        if os.geteuid() != os.getuid() or int(os.environ.get("SUDO_UID", os.getuid())) != os.getuid():
            raise RuntimeError("Run preflight as the original server user without sudo")
        config = yaml.safe_load(args.config.read_text())
        cache_mb = cache_budget(config, multiplier)
        if not cache_mb:
            print("Hugepage preflight: no explicit hugepage Cache connectors; nothing to do")
            return 0
        required = (cache_mb + args.target_mem_mb + args.reserve_mb + 1) // 2
        available, free, reserved, total = page_counts()
        reason = "explicit layout override" if args.cache_multiplier else "conservative device-count default"
        print(f"Hugepage budget: cache={cache_mb} MiB (multiplier={multiplier}, {reason}); "
              f"stopped target additional={args.target_mem_mb} MiB; "
              f"headers/metadata reserve={args.reserve_mb} MiB; required={required} x 2 MiB. "
              f"Available={available} (free={free} - reserved={reserved}), total={total}", flush=True)
        if available >= required:
            return 0
        # SPDK versions using /proc/sys/vm/nr_hugepages for global allocation
        # operate on the kernel default size even when HUGEPGSZ is supplied.
        default_size = re.search(r"^Hugepagesize:\s+(\d+) kB$",
                                 Path("/proc/meminfo").read_text(), re.MULTILINE)
        if default_size is None or int(default_size[1]) != 2048:
            raise RuntimeError(
                "Safe global SPDK setup.sh expansion requires kernel default Hugepagesize=2048 kB; "
                "ask an administrator to provision the 2 MiB pool or change the boot default"
            )
        setup = args.setup_script.resolve(strict=True)
        if not setup.is_file() or not os.access(setup, os.X_OK):
            raise RuntimeError(f"SPDK setup script is not executable: {setup}")
        requested = total + required - available
        # NVME_ALLOWED can append to PCI_ALLOWED. DRIVER_OVERRIDE=none also
        # prevents setup.sh's vfio branch from changing mount ownership/mode.
        command = ["sudo", "-n", "env", "-u", "HUGENODE", "-u", "SKIP_HUGE",
                   "-u", "NVME_ALLOWED", "-u", "PCI_BLOCKED", "-u", "TARGET_USER",
                   "PCI_ALLOWED=none", "DEV_TYPE=NVME", "SKIP_PCI=1", "DRIVER_OVERRIDE=none",
                   "HUGEPGSZ=2048", f"NRHUGE={requested}", "SHRINK_HUGE=no",
                   "CLEAR_HUGE=no", "PERSIST_HUGE=no", str(setup), "config"]
        print(f"Expanding 2 MiB pool via SPDK setup.sh: NRHUGE={requested}", flush=True)
        result = subprocess.run(command, check=False)
        available, free, reserved, total = page_counts()
        if result.returncode or available < required:
            raise RuntimeError(
                f"SPDK hugepage allocation failed/incomplete (setup exit={result.returncode}): "
                f"need {required} unreserved 2 MiB pages, have {available} "
                f"(free={free}, reserved={reserved}, total={total}). "
                "Check sudo -n authorization and host memory fragmentation/availability; "
                "ask an administrator to provision hugepages before retrying. No pages were cleared or shrunk."
            )
        print(f"Hugepage preflight passed: {available} unreserved 2 MiB pages available")
        return 0
    except (OSError, ValueError, RuntimeError, yaml.YAMLError) as exc:
        print(f"ERROR: hugepage preflight: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
