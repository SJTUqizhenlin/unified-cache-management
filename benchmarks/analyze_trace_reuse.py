#!/usr/bin/env python3
"""Analyze prefix-hit reuse distances in a JSONL KV-cache trace.

The trace must contain one request per line with `hash_ids`, `input_length`,
`session_id`, and `round_index`.  Prefix hits are the contiguous repeated
blocks at the start of a request; the first unseen block ends the prefix.
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
import json
import math
from pathlib import Path


def percentile(values, p):
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * p / 100) - 1))
    return ordered[index]


def summary(values):
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "min": min(values),
        "mean": round(sum(values) / len(values), 1),
        "p50": percentile(values, 50),
        "p90": percentile(values, 90),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "p999": percentile(values, 99.9),
        "max": max(values),
    }


def print_summary(name, values):
    data = summary(values)
    if not data["count"]:
        print(f"{name}: none")
        return
    print(
        f"{name}: n={data['count']} min={data['min']} mean={data['mean']} "
        f"p50={data['p50']} p90={data['p90']} p95={data['p95']} "
        f"p99={data['p99']} p999={data['p999']} max={data['max']}"
    )


def load_records(path):
    records = []
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            record["_line"] = line_number
            records.append(record)
    return records


def analyze(records, block_tokens, csv_path=None):
    global_hits = []
    session_hits = []
    by_previous_type = {"write": [], "hit": []}
    last = {}
    session_last = {}
    session_cursor = {}
    csv_rows = []
    cursor = 0
    block_accesses = 0
    unique_blocks = set()
    prefix_hits = 0
    writes = 0

    for request_index, record in enumerate(records):
        session = record.get("session_id", "")
        round_index = record.get("round_index", "")
        session_start = session_cursor.get(session, 0)
        session_map = session_last.setdefault(session, {})
        prefix_available = True

        for block_index, block_id in enumerate(record["hash_ids"]):
            token_pos = cursor + block_index * block_tokens
            session_pos = session_start + block_index * block_tokens
            current_block = block_accesses
            block_accesses += 1

            previous = last.get(block_id)
            if previous is not None and prefix_available:
                distance = token_pos - previous["token_pos"]
                block_distance = current_block - previous["block"]
                previous_type = previous["type"]
                global_hits.append(distance)
                by_previous_type[previous_type].append(distance)
                prefix_hits += 1
                if csv_path is not None:
                    csv_rows.append({
                        "request": request_index,
                        "line": record["_line"],
                        "session": session,
                        "round": round_index,
                        "block_index": block_index,
                        "block_id": block_id,
                        "previous_type": previous_type,
                        "previous_request": previous["request"],
                        "previous_session": previous["session"],
                        "previous_round": previous["round"],
                        "previous_block_index": previous["block_index"],
                        "distance_tokens": distance,
                        "distance_blocks": block_distance,
                    })
                previous["type"] = "hit"
            else:
                if previous is None:
                    prefix_available = False
                    unique_blocks.add(block_id)
                    writes += 1
                else:
                    # Seen but after a prefix miss: vLLM recomputes/dumps it.
                    previous["type"] = "write"
                if previous is None:
                    previous = {}

            previous.update({
                "token_pos": token_pos,
                "block": current_block,
                "request": request_index,
                "session": session,
                "round": round_index,
                "block_index": block_index,
                "type": previous.get("type", "write"),
            })
            last[block_id] = previous

            session_previous = session_map.get(block_id)
            if session_previous is not None and prefix_available:
                session_hits.append(session_pos - session_previous)
            session_map[block_id] = session_pos

        cursor += record["input_length"]
        session_cursor[session] = session_start + record["input_length"]

    if csv_path is not None:
        import csv
        csv_path.parent.mkdir(parents=True, exist_ok=True)
        with csv_path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(csv_rows[0]) if csv_rows else [])
            if csv_rows:
                writer.writeheader()
                writer.writerows(csv_rows)

    result = {
        "requests": len(records),
        "block_tokens": block_tokens,
        "block_accesses": block_accesses,
        "unique_blocks": len(unique_blocks),
        "writes": writes,
        "prefix_hits": prefix_hits,
        "prefix_hit_token_fraction": prefix_hits * block_tokens / cursor if cursor else 0,
        "global_token_distance": summary(global_hits),
        "previous_type": {
            key: summary(values) for key, values in by_previous_type.items()
        },
        "per_session_token_distance": summary(session_hits),
        "histogram": {},
        "csv": str(csv_path) if csv_path is not None else None,
    }
    bins = (
        (0, 512), (512, 4_096), (4_096, 16_384), (16_384, 65_536),
        (65_536, 262_144), (262_144, 1_048_576),
        (1_048_576, 4_194_304), (4_194_304, 8_388_608),
        (8_388_608, 16_777_216), (16_777_216, 1 << 62),
    )
    result["histogram"] = {
        f"{low}..{high}": sum(1 for value in global_hits if low <= value < high)
        for low, high in bins
    }
    result["sessions"] = len(session_last)
    result["total_input_tokens"] = cursor
    result["max_prefix_hit_rate"] = (
        prefix_hits / block_accesses if block_accesses else 0
    )
    result["_global_values"] = global_hits
    return result


def lru_curve(records, block_tokens, capacities, block_bytes):
    results = []
    for capacity in capacities:
        cache = OrderedDict()
        hits = 0
        accesses = 0
        for record in records:
            prefix_available = True
            for block_id in record["hash_ids"]:
                accesses += 1
                if block_id in cache:
                    cache.move_to_end(block_id)
                    if prefix_available:
                        hits += 1
                else:
                    if prefix_available:
                        prefix_available = False
                    cache[block_id] = None
                    if len(cache) > capacity:
                        cache.popitem(last=False)
        results.append({
            "capacity_blocks": capacity,
            "capacity_gib": capacity * block_bytes / (1 << 30),
            "hits": hits,
            "hit_rate": hits / accesses if accesses else 0,
        })
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--block-tokens", type=int, default=512)
    parser.add_argument("--block-bytes", type=int, default=3186688)
    parser.add_argument("--csv", type=Path)
    parser.add_argument(
        "--lru-capacity-blocks",
        type=int,
        nargs="+",
        default=[4096, 6738, 8192, 12000, 16000, 20000, 24000, 32000],
    )
    args = parser.parse_args()
    if args.block_tokens <= 0 or args.block_bytes <= 0:
        parser.error("block tokens/bytes must be positive")

    records = load_records(args.trace)
    result = analyze(records, args.block_tokens, args.csv)
    result["lru_curve"] = lru_curve(
        records, args.block_tokens, args.lru_capacity_blocks, args.block_bytes
    )

    print(f"trace: {args.trace}")
    print(
        f"requests={result['requests']} block_accesses={result['block_accesses']} "
        f"unique_blocks={result['unique_blocks']} writes={result['writes']} "
        f"prefix_hits={result['prefix_hits']} "
        f"prefix_hit_token_fraction={result['prefix_hit_token_fraction']:.4f} "
        f"sessions={result['sessions']} total_input_tokens={result['total_input_tokens']}"
    )
    print(
        f"max_achievable_prefix_hit_rate="
        f"{result['max_prefix_hit_rate']:.4f}"
    )
    print("\nGlobal reuse distance (tokens):")
    print_summary("  all prefix hits", result["_global_values"])
    print("\nPrevious-access type summaries:")
    for previous_type, values in result["previous_type"].items():
        if values.get("count"):
            print(
                f"  previous {previous_type}: n={values['count']} "
                f"min={values['min']} mean={values['mean']} p50={values['p50']} "
                f"p90={values['p90']} p99={values['p99']} max={values['max']}"
            )
    print("\nPer-session reuse distance (tokens):")
    data = result["per_session_token_distance"]
    if data.get("count"):
        print(
            f"  n={data['count']} min={data['min']} mean={data['mean']} "
            f"p50={data['p50']} p90={data['p90']} p95={data['p95']} "
            f"p99={data['p99']} p999={data['p999']} max={data['max']}"
        )
    print("\nGlobal reuse-distance histogram (tokens):")
    for key, count in result["histogram"].items():
        print(f"  {key:>12}: {count}")
    del result["_global_values"]
    print("\nIdeal LRU prefix-hit curve:")
    for row in result["lru_curve"]:
        print(
            f"  capacity={row['capacity_blocks']:6d} blocks "
            f"({row['capacity_gib']:7.1f} GiB/FA-block): "
            f"hits={row['hits']:6d} hit_rate={row['hit_rate'] * 100:6.2f}%"
        )
    if result["csv"]:
        print(f"\nper-hit CSV: {result['csv']}")


if __name__ == "__main__":
    main()
