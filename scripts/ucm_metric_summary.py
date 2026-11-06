"""Summarize cumulative connector/load histograms without importing UCM."""

import argparse
import math
import re


def histogram_summary(text, name, label):
    totals = {"sum": 0.0, "count": 0.0}
    buckets = {}
    for line in text.splitlines():
        match = re.fullmatch(
            re.escape(name) + r"_(sum|count|bucket)(\{[^\n]*\})?\s+(\S+)(?:\s+\S+)?", line
        )
        if not match:
            continue
        kind, labels, raw = match.groups()
        value = float(raw)
        if not math.isfinite(value) or value < 0:
            continue
        if kind != "bucket":
            totals[kind] += value
        else:
            bound = re.search(r'(?:\{|,)\s*le="([^"]+)"', labels or "")
            if bound:
                upper = float(bound[1])
                buckets[upper] = buckets.get(upper, 0.0) + value
    count = totals["count"]
    average = f"{totals['sum'] / count:.3f} ms" if count else "N/A"
    p99 = "N/A"
    # Interpolate cumulative buckets, never average per-rank percentiles.
    ordered = sorted(buckets.items())
    if count and ordered and math.isinf(ordered[-1][0]) and ordered[-1][1] == count:
        target = 0.99 * count
        lower, previous = 0.0, 0.0
        for upper, cumulative in ordered:
            if cumulative < previous:
                break
            if cumulative >= target:
                if math.isinf(upper):
                    p99 = f">= {lower:.3f} ms (overflow bucket)"
                elif cumulative > previous:
                    estimate = lower + (upper - lower) * (target - previous) / (cumulative - previous)
                    p99 = f"~{estimate:.3f} ms"
                break
            lower, previous = upper, cumulative
    return f"{label} (cumulative, ranks pooled): count={count:g} avg={average} p99={p99}"


def fawa_wait_summary(text):
    return histogram_summary(text, "ucm:fawa_load_wait_duration_ms", "FAWA load barrier")


def start_load_summary(text):
    return histogram_summary(
        text, "ucm:connector_start_load_kv_duration_ms",
        "start_load_kv total [all calls, including no-load]",
    )


def fawa_start_load_summary(text):
    return histogram_summary(
        text, "ucm:fawa_start_load_kv_duration_ms", "FAWA start_load_kv total [nonempty loads]",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metrics_file")
    args = parser.parse_args()
    with open(args.metrics_file) as stream:
        text = stream.read()
    print(start_load_summary(text))
    print(fawa_start_load_summary(text))
    print(fawa_wait_summary(text))
