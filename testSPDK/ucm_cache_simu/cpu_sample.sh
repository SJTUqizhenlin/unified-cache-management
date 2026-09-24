#!/bin/bash
# cpu_sample.sh - measure average CPU usage (cores) of a cache benchmark run.
#
# usage: ./cpu_sample.sh <kernel|spdk> [extra benchmark args...]
#
#   ./cpu_sample.sh kernel                # kernel_cache_test, all defaults
#   ./cpu_sample.sh kernel -n 2000 -M 0   # defaults + user overrides
#   sudo ./cpu_sample.sh spdk -t 15       # SPDK variant needs sudo
#
# Waits for the measured phase to start, then samples /proc/<pid>/stat
# utime+stime over 6 seconds to get the average core count consumed by the
# whole process (all threads aggregated). Prints the throughput afterwards.
#
# Phase detection: init-dump prints "Progress: <done>/<total> blocks" while
# the measured phase prints "Progress: <count> blocks" (no slash), so the
# wait loop matches only the latter and the sample never lands inside
# init-dump, regardless of -n.
#
# Notes:
#   - run it from anywhere; binaries are resolved next to this script
#   - the spdk variant is a root process: launch this script with sudo
#   - pass -t >= 15 so the 6s sample window fits inside the measured phase

set -u
BIN_DIR=$(cd "$(dirname "$0")" && pwd)

sel=${1:-}
case "$sel" in
kernel | kernel_cache_test)
	bin="$BIN_DIR/kernel_cache_test"
	comm=kernel_cache_te
	;;
spdk | nvme | nvme_cache_test)
	bin="$BIN_DIR/nvme_cache_test"
	comm=nvme_cache_test
	;;
*)
	echo "usage: $0 <kernel|spdk> [extra benchmark args...]" >&2
	exit 1
	;;
esac
shift

if [ ! -x "$bin" ]; then
	echo "binary not found: $bin" >&2
	exit 1
fi

out=/tmp/cpu_sample_$$.out
"$bin" "$@" >"$out" 2>&1 &

# Warm up: wait until the measured phase reports progress (plain counter,
# no "<done>/<total>" form), bounded so a stuck run cannot hang the script.
progress=0
waited=0
for _ in $(seq 1 150); do # 150 x 0.2s = 30s max
	if grep -qE 'Progress: [0-9]+ blocks' "$out" 2>/dev/null; then
		progress=1
		break
	fi
	if ! kill -0 $! 2>/dev/null; then
		break
	fi
	sleep 0.2
	waited=$((waited + 1))
done
if [ "$progress" -ne 1 ]; then
	echo "WARNING: no progress line seen; sampling anyway" >&2
fi
echo "phase-sync: measured-phase progress after $((waited * 200))ms; sampling 6s from there"

pid=$(pgrep -x "$comm" | head -1)
if [ -z "$pid" ]; then
	echo "pid not found for comm '$comm'" >&2
	wait
	cat "$out"
	rm -f "$out"
	exit 1
fi

SAMPLE_SECS=6
t1=$(awk '{print $14+$15}' /proc/$pid/stat)
sleep "$SAMPLE_SECS"
t2=$(awk '{print $14+$15}' /proc/$pid/stat)
nthr=$(awk '{print $20}' /proc/$pid/stat)
hz=$(getconf CLK_TCK)

cores=$(awk -v a="$t1" -v b="$t2" -v s="$SAMPLE_SECS" -v h="$hz" \
	'BEGIN{printf "%.2f",(b-a)/h/s}')
echo "[$comm] pid=$pid threads=$nthr avg_cpu=${cores} cores (${SAMPLE_SECS}s sample)"

wait
grep -E 'Throughput' "$out"
rm -f "$out"
