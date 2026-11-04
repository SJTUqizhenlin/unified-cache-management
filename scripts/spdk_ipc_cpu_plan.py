#!/usr/bin/env python3
"""Print an IPC reactor CPU list and one-to-one SPDK lcore map on two lines.

Default allocation interleaves NUMA nodes, reserving each node's first four
IRQ/runtime CPUs and last two ACL/release CPUs. Explicit overrides only enforce
the reactor count, unique CPU IDs and the calling process's CPU affinity.
CPU IDs are used directly; SMT sibling topology is not inspected.
"""

import argparse
import os
from pathlib import Path
import re
import sys


def parse_cpus(value):
    """Expand an ordered comma/range list, rejecting duplicates and bad syntax."""
    cpus = []
    seen = set()
    for token in value.split(","):
        match = re.fullmatch(r"\s*([0-9]+)(?:\s*-\s*([0-9]+))?\s*", token)
        if match is None:
            raise ValueError(f"Invalid CPU list {value!r}: expected comma-separated IDs/ranges")
        first = int(match[1])
        last = int(match[2]) if match[2] is not None else first
        if last < first:
            raise ValueError(f"Invalid descending CPU range {token!r}")
        for cpu in range(first, last + 1):
            if cpu in seen:
                raise ValueError(f"Duplicate CPU {cpu} in CPU list")
            seen.add(cpu)
            cpus.append(cpu)
    return cpus


def read_topology(root=Path("/sys/devices/system/node")):
    nodes = {}
    for path in Path(root).glob("node*/cpulist"):
        match = re.fullmatch(r"node([0-9]+)", path.parent.name)
        if match is not None:
            text = path.read_text().strip()
            nodes[int(match[1])] = sorted(parse_cpus(text)) if text else []
    return dict(sorted(nodes.items()))


def plan_cpus(reactors, nodes, allowed, cpus=None, cpu_base=None):
    """Select unique CPU IDs without changing affinity or falling into reserves."""
    if reactors <= 0:
        raise ValueError("--reactors must be positive")
    if cpus is not None and cpu_base is not None:
        raise ValueError("--cpus and --cpu-base are mutually exclusive")
    allowed = set(allowed)
    if cpus is not None:
        selected = parse_cpus(cpus)
    elif cpu_base is not None:
        if cpu_base < 0:
            raise ValueError("--cpu-base must be nonnegative")
        selected = list(range(cpu_base, cpu_base + reactors))
    else:
        if not nodes:
            raise ValueError("No NUMA node cpulists found; pass --cpus for an explicit allocation")
        # Slice the physical node list BEFORE affinity filtering so reservations
        # cannot move or disappear when a cpuset excludes some node CPUs.
        pools = [[cpu for cpu in sorted(nodes[node])[4:-2] if cpu in allowed]
                 for node in sorted(nodes)]
        selected = []
        seen = set()
        for offset in range(max(map(len, pools), default=0)):
            for pool in pools:
                if offset < len(pool) and pool[offset] not in seen:
                    selected.append(pool[offset])
                    seen.add(pool[offset])
                    if len(selected) == reactors:
                        return selected
        raise ValueError(
            f"Insufficient eligible CPUs: need {reactors}, have {len(selected)} within affinity "
            "after reserving each NUMA node's first 4 and last 2 CPUs; "
            "reserved CPUs will not be used. Pass --cpus for an explicit override"
        )
    if len(selected) != reactors:
        raise ValueError(f"CPU count {len(selected)} does not match --reactors {reactors}")
    unavailable = [cpu for cpu in selected if cpu not in allowed]
    if unavailable:
        raise ValueError(f"CPUs not allowed by process affinity: {','.join(map(str, unavailable))}")
    return selected


def lcore_map(cpus, custom=None):
    expected = dict(enumerate(cpus))
    if custom is not None:
        parsed = {}
        for token in custom.split(","):
            match = re.fullmatch(r"\s*([0-9]+)@([0-9]+)\s*", token)
            if match is None:
                raise ValueError(
                    "Unsupported --lcore-map syntax: expected one-to-one i@cpu entries; "
                    "pass --cpus and omit --lcore-map (group/range maps are not supported)"
                )
            core, cpu = map(int, match.groups())
            if core in parsed:
                raise ValueError(f"Duplicate lcore {core} in --lcore-map")
            parsed[core] = cpu
        if parsed != expected:
            raise ValueError(
                "--lcore-map must map every lcore 0..N-1 one-to-one to the selected CPUs "
                "in order; pass --cpus and omit --lcore-map"
            )
    return ",".join(f"{core}@{cpu}" for core, cpu in expected.items())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reactors", type=int, default=32)
    overrides = parser.add_mutually_exclusive_group()
    overrides.add_argument("--cpus", help="Ordered comma-separated CPU IDs/ranges")
    overrides.add_argument("--cpu-base", type=int, help="Legacy contiguous CPU allocation start")
    parser.add_argument("--lcore-map", help="Validate an exact one-to-one i@cpu map")
    args = parser.parse_args(argv)
    try:
        nodes = read_topology()
        cpus = plan_cpus(args.reactors, nodes, os.sched_getaffinity(0),
                         cpus=args.cpus, cpu_base=args.cpu_base)
        mapping = lcore_map(cpus, args.lcore_map)
        selected = set(cpus)
        allocation = [f"node{node}={','.join(str(cpu) for cpu in cpus if cpu in set(pool)) or '-'}"
                      for node, pool in sorted(nodes.items())]
        unknown = selected.difference(cpu for pool in nodes.values() for cpu in pool)
        if unknown:
            allocation.append(f"unknown={','.join(map(str, sorted(unknown)))}")
        mode = "explicit" if args.cpus is not None or args.cpu_base is not None else "automatic"
        print(f"NUMA allocation ({mode}, {len(cpus)} reactors): {'; '.join(allocation)}",
              file=sys.stderr)
        print(",".join(map(str, cpus)))
        print(mapping)
        return 0
    except (OSError, ValueError) as exc:
        print(f"ERROR: SPDK IPC CPU plan: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
