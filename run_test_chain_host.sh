#!/usr/bin/env bash
# run_test_chain_host.sh — 串联启动 server → client → store stats
#
# 流程：
#   1. 后台启动 server_test_host.sh
#   2. 等到本实例的 vLLM 端口 /health 可用
#   3. 后台启动 client_test_host.sh --stop-server（REPLAY_WINDOW_SEC=2000）
#   4. 等到 client 真正进入 trace_replay
#   5. client 启动 120s 后，后台启动 ucm_store_stats.sh -i 3000 -n 10
#   6. 等待 client 结束；默认同时停止仍存活的 stats（client 结束会停 server，
#      继续采样通常没有意义）。需要跑满 stats 的 -n 10 时设 STATS_STOP_WITH_CLIENT=0。
#
# 用法：
#   ./run_test_chain_host.sh
#
# 常用环境变量：
#   PORT=7780 HOST=127.0.0.1 DEVICES=0,1,2,3,4,5,6,7
#   REPLAY_WINDOW_SEC=2000
#   CLIENT_STATS_DELAY_SECONDS=120
#   STATS_INTERVAL=3000 STATS_COUNT=10
#   STATS_STOP_WITH_CLIENT=1      # 1=client 结束时停止 stats；0=等 stats 自己跑完
#   SERVER_READY_TIMEOUT=0        # 0=一直等 vLLM 端口就绪
#   CLIENT_READY_TIMEOUT=0        # 0=一直等 client 进入 trace_replay
#   CLIENT_READY_PATTERN="Starting initial single prompt test run..."
#                                 # 判定 client 已成功启动的日志标记
#   RUN_DIR=~/ucm-logs/chain_xxx  # 三个进程的日志目录
#
# 注意：ucm_store_stats.sh 的 -i 单位是秒，-i 3000 -n 10 约 8.3 小时。

set -euo pipefail

REPO="${REPO:-$HOME/devA2/unified-cache-management}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-7780}"
MODEL="${MODEL:-/mnt/model/DeepSeek-V4-Flash-w8a8-mtp}"
CACHE_DIR="${CACHE_DIR:-$HOME/ucm-cache/test}"
UCM_CONFIG="${UCM_CONFIG:-$HOME/ucm-cache/ucm_config_$(basename "${CACHE_DIR%/}").yaml}"

REPLAY_WINDOW_SEC="${REPLAY_WINDOW_SEC:-2000}"
CLIENT_STATS_DELAY_SECONDS="${CLIENT_STATS_DELAY_SECONDS:-120}"
STATS_INTERVAL="${STATS_INTERVAL:-3000}"
STATS_COUNT="${STATS_COUNT:-10}"
STATS_STOP_WITH_CLIENT="${STATS_STOP_WITH_CLIENT:-1}"
SERVER_READY_TIMEOUT="${SERVER_READY_TIMEOUT:-0}"
CLIENT_READY_TIMEOUT="${CLIENT_READY_TIMEOUT:-0}"
CLIENT_READY_PATTERN="${CLIENT_READY_PATTERN:-Starting initial single prompt test run...}"

RUN_DIR="${RUN_DIR:-$HOME/ucm-logs/chain_$(date +%Y%m%d_%H%M%S)}"
SERVER_LOG="$RUN_DIR/server.log"
CLIENT_LOG="$RUN_DIR/client.log"
STATS_LOG="$RUN_DIR/stats.log"

SERVER_PID=""
CLIENT_PID=""
STATS_PID=""
SERVER_START_TIME=""
SERVER_TOKEN=""
PROCESS_HELPER="$REPO/scripts/host_server_process.py"

usage() {
  echo "Usage: $0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    *) echo "Error: unknown argument '$1'." >&2; usage >&2; exit 2 ;;
  esac
done

for name in SERVER_READY_TIMEOUT CLIENT_READY_TIMEOUT; do
  value="${!name}"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "ERROR: $name 必须是非负整数，当前: $value" >&2
    exit 2
  fi
done
for name in CLIENT_STATS_DELAY_SECONDS STATS_INTERVAL; do
  value="${!name}"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    echo "ERROR: $name 必须是非负整数，当前: $value" >&2
    exit 2
  fi
done
if ! [[ "$STATS_COUNT" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: STATS_COUNT 必须是正整数，当前: $STATS_COUNT" >&2
  exit 2
fi
if [[ "$REPLAY_WINDOW_SEC" != "0" ]] && ! [[ "$REPLAY_WINDOW_SEC" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: REPLAY_WINDOW_SEC 必须是非负整数，当前: $REPLAY_WINDOW_SEC" >&2
  exit 2
fi
if ! command -v setsid >/dev/null 2>&1; then
  echo "ERROR: 需要 setsid（util-linux）来隔离并清理三个进程组" >&2
  exit 2
fi

mkdir -p "$RUN_DIR"
export HOST PORT MODEL CACHE_DIR UCM_CONFIG
export PYTHONUNBUFFERED=1
export PYTHONFAULTHANDLER=1

pid_alive() {
  local pid="${1:-}" state
  [[ -n "$pid" ]] || return 1
  state=$(ps -o stat= -p "$pid" 2>/dev/null) || return 1
  [[ -n "$state" && "$state" != Z* ]]
}

signal_tree() {
  local pid="${1:-}" sig="${2:-TERM}"
  [[ -n "$pid" ]] || return 0
  # 进程由 setsid 启动，pid 即进程组 id；优先整组发送，失败再退回单进程
  kill -"$sig" -- "-$pid" 2>/dev/null || kill -"$sig" "$pid" 2>/dev/null || true
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM

  if pid_alive "$STATS_PID"; then
    signal_tree "$STATS_PID" TERM
    wait "$STATS_PID" 2>/dev/null || true
  fi
  if pid_alive "$CLIENT_PID"; then
    signal_tree "$CLIENT_PID" TERM
    sleep 1
    signal_tree "$CLIENT_PID" KILL
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
  if [[ -n "$SERVER_PID" && -n "$SERVER_START_TIME" ]]; then
    # Token discovery also covers descendants after the master has exited.
    python3 "$PROCESS_HELPER" stop --port "$PORT" \
      --pid "$SERVER_PID" --start-time "$SERVER_START_TIME" \
      --token "$SERVER_TOKEN" --grace-seconds 30 || true
    if ! pid_alive "$SERVER_PID"; then
      wait "$SERVER_PID" 2>/dev/null || true
    fi
  fi

  echo "日志目录: $RUN_DIR"
  exit "$rc"
}
trap cleanup EXIT INT TERM

server_is_ready() {
  pid_alive "$SERVER_PID" || return 1
  python3 "$PROCESS_HELPER" identify --port "$PORT" \
    --pid "$SERVER_PID" --start-time "$SERVER_START_TIME" >/dev/null 2>&1 || return 1
  curl -fsS --max-time 3 "http://$HOST:$PORT/health" >/dev/null 2>&1
}

client_is_ready() {
  pid_alive "$CLIENT_PID" || return 1
  # 默认等 trace_replay.py 真正开始发送请求；可用 CLIENT_READY_PATTERN 改标记
  grep -qF "$CLIENT_READY_PATTERN" "$CLIENT_LOG" 2>/dev/null
}

wait_for_server() {
  local start=$SECONDS
  while :; do
    if server_is_ready; then
      echo "==> server 已就绪: http://$HOST:$PORT"
      return 0
    fi
    if ! pid_alive "$SERVER_PID"; then
      echo "ERROR: server 在端口就绪前退出，日志尾部:" >&2
      tail -n 80 "$SERVER_LOG" >&2 || true
      return 1
    fi
    if (( SERVER_READY_TIMEOUT > 0 && SECONDS - start >= SERVER_READY_TIMEOUT )); then
      echo "ERROR: 等待 server 端口超时 (${SERVER_READY_TIMEOUT}s)" >&2
      return 1
    fi
    sleep 5
  done
}

wait_for_client() {
  local start=$SECONDS
  while :; do
    if client_is_ready; then
      echo "==> client 已进入 trace_replay: $CLIENT_LOG"
      return 0
    fi
    if ! pid_alive "$CLIENT_PID"; then
      echo "ERROR: client 在进入 trace_replay 前退出，日志尾部:" >&2
      tail -n 80 "$CLIENT_LOG" >&2 || true
      return 1
    fi
    if (( CLIENT_READY_TIMEOUT > 0 && SECONDS - start >= CLIENT_READY_TIMEOUT )); then
      echo "ERROR: 等待 client 进入 trace_replay 超时 (${CLIENT_READY_TIMEOUT}s)" >&2
      return 1
    fi
    sleep 2
  done
}

wait_client_delay() {
  local remaining="$CLIENT_STATS_DELAY_SECONDS"
  while (( remaining > 0 )); do
    if ! pid_alive "$CLIENT_PID"; then
      return 1
    fi
    sleep 1
    remaining=$((remaining - 1))
  done
  return 0
}

echo "==> [1/3] 启动 server_test_host.sh (PORT=$PORT, DEVICES=${DEVICES:-default})"
echo "    日志: $SERVER_LOG"
# A detached server cannot prompt on a controlling terminal.
if ! setsid sudo -n true; then
  echo "ERROR: 后台 server 需要无控制终端的免交互 sudo 权限；请勿使用 sudo 启动整个测试链" >&2
  exit 1
fi
SERVER_TOKEN=$(python3 -c 'import uuid; print(uuid.uuid4().hex)')
UCM_HOST_SERVER_TOKEN="$SERVER_TOKEN" \
  setsid bash "$REPO/server_test_host.sh" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
SERVER_START_TIME=$(python3 - "$SERVER_PID" <<'PY'
from pathlib import Path
import sys

print((Path("/proc") / sys.argv[1] / "stat").read_text().rsplit(")", 1)[1].split()[19])
PY
)

if ! wait_for_server; then
  exit 1
fi

echo "==> [2/3] 启动 client_test_host.sh --stop-server (REPLAY_WINDOW_SEC=$REPLAY_WINDOW_SEC)"
echo "    日志: $CLIENT_LOG"
REPLAY_WINDOW_SEC="$REPLAY_WINDOW_SEC" \
  UCM_HOST_SERVER_PID="$SERVER_PID" UCM_HOST_SERVER_START_TIME="$SERVER_START_TIME" \
  UCM_HOST_SERVER_TOKEN="$SERVER_TOKEN" \
  setsid bash "$REPO/client_test_host.sh" --stop-server >"$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

if ! wait_for_client; then
  exit 1
fi

echo "==> [3/3] client 启动 ${CLIENT_STATS_DELAY_SECONDS}s 后启动 ucm_store_stats.sh -i ${STATS_INTERVAL} -n ${STATS_COUNT}"
if wait_client_delay; then
  echo "    日志: $STATS_LOG"
  setsid bash "$REPO/ucm_store_stats.sh" -i "$STATS_INTERVAL" -n "$STATS_COUNT" >"$STATS_LOG" 2>&1 &
  STATS_PID=$!
else
  echo "    client 已退出，跳过 stats"
fi

set +e
wait "$CLIENT_PID"
CLIENT_RC=$?
set -e
CLIENT_PID=""
echo "==> client 已结束 (rc=$CLIENT_RC)"

if pid_alive "$STATS_PID"; then
  if [[ "$STATS_STOP_WITH_CLIENT" == "1" ]]; then
    echo "==> client 已结束，停止仍在运行的 stats（STATS_STOP_WITH_CLIENT=0 可让它跑满 -n）"
    signal_tree "$STATS_PID" TERM
    wait "$STATS_PID" 2>/dev/null || true
    STATS_PID=""
  else
    echo "==> 等待 stats 跑满 -i ${STATS_INTERVAL} -n ${STATS_COUNT} ..."
    wait "$STATS_PID" || true
    STATS_PID=""
  fi
fi

echo "==> 完成。日志目录: $RUN_DIR"
exit "$CLIENT_RC"
