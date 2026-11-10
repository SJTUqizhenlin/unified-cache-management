#!/usr/bin/env bash
# ucm_stack_pod.sh — save user-space call stacks of a running vLLM+UCM server.
#
# Uses py-spy (installed into a private venv under ~/.ucm-tools, so the vLLM
# Python environment is never modified). Native frames (C/C++ UCM store code)
# are included with --native; idle/blocked I/O threads are included with --idle.
#
# Modes:
#   record  (default)  py-spy record -> speedscope JSON per process
#   dump               timestamped py-spy dump snapshots -> one text file
#
# Usage:
#   ./ucm_stack_pod.sh --port 7780 --duration 30
#   ./ucm_stack_pod.sh --port 7780 --duration 60 --mode dump --interval 2
#   ./ucm_stack_pod.sh --pid 12345,67890 --duration 10
#   ./ucm_stack_pod.sh --list
#
# Environment overrides:
#   PORT=7780 DURATION=30 MODE=record RATE=100 OUTPUT=... ./ucm_stack_pod.sh

set -euo pipefail

PORT="${PORT:-7780}"
DURATION="${DURATION:-30}"
INTERVAL="${INTERVAL:-1}"
MODE="${MODE:-record}"
RATE="${RATE:-100}"
OUTPUT="${OUTPUT:-}"
INCLUDE_MAIN="${INCLUDE_MAIN:-0}"
PYSPY="${PYSPY:-}"
PIDS_OVERRIDE=""
LIST_ONLY=0
REPO="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

usage() {
  sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)        PORT="$2"; shift 2 ;;
    --duration)    DURATION="$2"; shift 2 ;;
    --interval)    INTERVAL="$2"; shift 2 ;;
    --mode)        MODE="$2"; shift 2 ;;
    --rate)        RATE="$2"; shift 2 ;;
    --output)      OUTPUT="$2"; shift 2 ;;
    --pid)         PIDS_OVERRIDE="$2"; shift 2 ;;
    --include-main) INCLUDE_MAIN=1; shift ;;
    --py-spy)      PYSPY="$2"; shift 2 ;;
    --list)        LIST_ONLY=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "ERROR: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

case "$MODE" in record|dump) ;; *) echo "ERROR: --mode must be record or dump" >&2; exit 2 ;; esac
if ! [[ "$DURATION" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: --duration must be a positive integer (seconds)" >&2; exit 2
fi
if ! [[ "$INTERVAL" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: --interval must be a positive integer (seconds)" >&2; exit 2
fi
if ! [[ "$RATE" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: --rate must be a positive integer (Hz)" >&2; exit 2
fi

resolve_pids() {
  if [[ -n "$PIDS_OVERRIDE" ]]; then
    if ! [[ "$PIDS_OVERRIDE" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
      echo "ERROR: --pid must be a comma-separated list of PIDs" >&2
      exit 2
    fi
    printf '%s\n' "$PIDS_OVERRIDE" | tr ',' '\n'
    return 0
  fi
  local identity root
  identity=$(python3 "$REPO/scripts/host_server_process.py" identify --port "$PORT") || {
    echo "ERROR: no owned vllm serve process found on port $PORT" >&2
    exit 1
  }
  root="${identity%% *}"
  python3 - "$root" "$INCLUDE_MAIN" <<'PY'
import os
import sys

root = int(sys.argv[1])
include_main = sys.argv[2] == "1"
children = {}
comms = {}
for entry in os.scandir("/proc"):
    if not entry.name.isdigit():
        continue
    try:
        stat = open(f"/proc/{entry.name}/stat").read()
        close = stat.rfind(")")
        fields = stat[close + 2:].split()
        ppid = int(fields[1])
        state = fields[0]
        comm = open(f"/proc/{entry.name}/comm").read().strip()
    except OSError:
        continue
    if state in ("Z", "X", "x"):
        continue
    pid = int(entry.name)
    children.setdefault(ppid, []).append(pid)
    comms[pid] = comm

selected = []
queue = [root]
while queue:
    pid = queue.pop(0)
    for child in sorted(children.get(pid, [])):
        queue.append(child)
        if comms.get(child, "").startswith("VLLM::"):
            selected.append(child)
if include_main and comms.get(root, "") == "vllm":
    selected.insert(0, root)
if not selected:
    selected = [root]
for pid in selected:
    print(pid)
PY
}

PIDS=$(resolve_pids)
if [[ -z "$PIDS" ]]; then
  echo "ERROR: failed to resolve target PIDs" >&2
  exit 1
fi

if [[ -z "$OUTPUT" ]]; then
  OUTPUT="$HOME/ucm-prof/$PORT/stacks_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUTPUT"

if [[ "$LIST_ONLY" == "1" ]]; then
  echo "==> target pids (port $PORT)"
  for pid in $PIDS; do
    printf '    %s %s\n' "$pid" "$(cat "/proc/$pid/comm" 2>/dev/null || echo '?')"
  done
  exit 0
fi

if [[ -z "$PYSPY" ]]; then
  TOOL_ROOT="$HOME/.ucm-tools"
  PYSPY="$TOOL_ROOT/pyspy-venv/bin/py-spy"
  if [[ ! -x "$PYSPY" ]]; then
    echo "==> installing py-spy into $TOOL_ROOT/pyspy-venv (vLLM env untouched)"
    mkdir -p "$TOOL_ROOT"
    python3 -m venv "$TOOL_ROOT/pyspy-venv"
    "$TOOL_ROOT/pyspy-venv/bin/pip" install -q py-spy
  fi
fi
if [[ ! -x "$PYSPY" ]]; then
  echo "ERROR: py-spy not found/executable: $PYSPY" >&2
  exit 1
fi

echo "==> ucm stacks"
echo "    mode     : $MODE"
echo "    pids     : $(echo $PIDS | tr '\n' ' ')"
echo "    duration : ${DURATION}s"
echo "    output   : $OUTPUT"

if [[ "$MODE" == "dump" ]]; then
  LOG="$OUTPUT/stacks_$(date +%Y%m%d_%H%M%S).log"
  : > "$LOG"
  deadline=$((SECONDS + DURATION))
  while (( SECONDS < deadline )); do
    for pid in $PIDS; do
      {
        echo "===== $(date '+%F %T.%N') pid=$pid comm=$(cat "/proc/$pid/comm" 2>/dev/null || echo '?') ====="
        "$PYSPY" dump --native --pid "$pid" 2>&1 || true
        echo
      } >> "$LOG"
    done
    sleep "$INTERVAL"
  done
  echo "    stacks log: $LOG"
  exit 0
fi

declare -a JOBS=()
for pid in $PIDS; do
  out="$OUTPUT/pid_${pid}.speedscope.json"
  log="$OUTPUT/py-spy_${pid}.log"
  "$PYSPY" record \
    --native --idle --threads \
    --rate "$RATE" \
    --format speedscope \
    --duration "$DURATION" \
    --output "$out" \
    --pid "$pid" >"$log" 2>&1 &
  JOBS+=("$!")
done
status=0
for job in "${JOBS[@]}"; do
  if ! wait "$job"; then status=1; fi
done

echo "    outputs:"
find "$OUTPUT" -maxdepth 1 -type f | sort | sed 's/^/      /'
if (( status != 0 )); then
  echo "WARN: some py-spy recordings failed; check py-spy_*.log" >&2
  exit "$status"
fi
