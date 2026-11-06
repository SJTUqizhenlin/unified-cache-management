"""Fail closed if selected NPUs are busy or their HBM usage cannot be parsed."""

import argparse
import re
import sys


def parse_hbm(text):
    usage = {}
    npu = None
    for line in text.splitlines():
        fields = [field.strip() for field in line.split("|")[1:-1]]
        if len(fields) < 3:
            continue
        # Stop before the separate process table; its rows are not device headers.
        if "Process id" in line:
            break
        if re.fullmatch(r"\d+\s+\S+", fields[0]):
            npu = int(fields[0].split()[0])
        elif (npu is not None and re.fullmatch(r"\d+", fields[0])
              and re.fullmatch(r"[0-9a-fA-F]+:[0-9a-fA-F]+:[0-9a-fA-F]+\.[0-9a-fA-F]+", fields[1])):
            pairs = re.findall(r"(\d+)\s*/\s*(\d+)", fields[-1])
            if not pairs:
                continue
            used, total = map(int, pairs[-1])
            if total <= 0 or used > total:
                raise ValueError(f"invalid HBM usage for NPU {npu}: {used}/{total}")
            usage[npu] = max(usage.get(npu, 0), used)
    return usage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--devices", required=True)
    parser.add_argument("--threshold-mb", type=int, default=8000)
    args = parser.parse_args()
    try:
        devices = [int(device.strip()) for device in args.devices.split(",")]
        if not devices or any(device < 0 for device in devices):
            raise ValueError("invalid device IDs")
        usage = parse_hbm(sys.stdin.read())
        missing = sorted(set(devices) - usage.keys())
        if missing:
            raise ValueError(f"no valid HBM usage for selected NPUs: {missing}")
    except ValueError as error:
        print(f"ERROR: NPU HBM check failed: {error}", file=sys.stderr)
        return 1
    busy = False
    for device in devices:
        blocked = usage[device] > args.threshold_mb
        print(f"    NPU {device}: HBM {usage[device]} MB"
              f" (threshold {args.threshold_mb} MB): {'BUSY' if blocked else 'OK'}")
        busy |= blocked
    return int(busy)


if __name__ == "__main__":
    sys.exit(main())
