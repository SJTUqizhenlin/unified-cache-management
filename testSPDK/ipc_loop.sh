#!/usr/bin/env bash
# ipc_loop.sh — 本地 PCIe NVMe IPC target 一键启停
#
# 用法:
#   sudo ./ipc_loop.sh start [trid] [socket]
#   sudo ./ipc_loop.sh stop
#   sudo ./ipc_loop.sh status
#   sudo ./ipc_loop.sh restart [trid] [socket]
#
# 默认从 $UCM_CONFIG（缺省 ~/ucm-cache/ucm_config_test.yaml）读取：
#   spdk_trid: "ipc:<socket>|<target-trid>"
# 没有配置文件时使用 0000:84:00.0 + /tmp/ucm_spdk_ipc.sock。
#
# 环境变量覆盖:
#   UCM_CONFIG=/path/to/config.yaml
#   IPC_TARGET_TRID / IPC_SOCKET
#   IPC_TARGET_CPUS / IPC_TARGET_LCORE_MAP / IPC_TARGET_REACTORS / IPC_TARGET_CPU_BASE
#   IPC_TARGET_MEM_MB / IPC_TARGET_IOVA_MODE / IPC_TARGET_QPAIR_DEPTH
#   IPC_TARGET_PIDFILE / IPC_TARGET_LOG
#
# status 退出码: 0=运行中，1=未运行。

set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TGT="$REPO/testSPDK/ucm_spdk_ipc/ucm_spdk_ipc_target"

if [[ "$(id -u)" -ne 0 ]]; then
	echo "must run as root (sudo $0 ...)" >&2
	exit 1
fi

if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
	OWNER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
else
	OWNER_HOME=${HOME:-/root}
fi
OWNER_HOME=${OWNER_HOME:-/root}
CONFIG=${UCM_CONFIG:-$OWNER_HOME/ucm-cache/ucm_config_test.yaml}
PIDFILE=${IPC_TARGET_PIDFILE:-$OWNER_HOME/ucm-cache/ucm_spdk_ipc_target.pid}
LOG=${IPC_TARGET_LOG:-$OWNER_HOME/ucm-logs/ucm_spdk_ipc_target.log}

read_config() {
	local -a lines=()
	[[ -f "$CONFIG" ]] || return 0
	mapfile -t lines < <(python3 - "$CONFIG" <<'PY'
import sys

try:
    import yaml
    with open(sys.argv[1]) as stream:
        config = yaml.safe_load(stream)
except Exception:
    sys.exit(0)

for entry in (config or {}).get("ucm_connectors", []):
    cfg = entry.get("ucm_connector_config", {})
    stages = str(cfg.get("store_pipeline", "")).lower().split("|")
    if "spdk" not in stages:
        continue
    trid = str(cfg.get("spdk_trid", "")).strip()
    if not trid.startswith("ipc:"):
        break
    spec = trid[4:]
    sock, separator, target = spec.partition("|")
    target = target.strip() if separator else str(cfg.get("ipc_target_trid", "")).strip()
    print(target)
    print(sock.strip())
    break
PY
	)
	if [[ ${#lines[@]} -ge 2 ]]; then
		CFG_TRID="${lines[0]}"
		CFG_SOCK="${lines[1]}"
	fi
}

CFG_TRID=""
CFG_SOCK=""
read_config

TRID=${IPC_TARGET_TRID:-${CFG_TRID:-trtype:PCIe traddr:0000:84:00.0}}
SOCKET=${IPC_SOCKET:-${CFG_SOCK:-/tmp/ucm_spdk_ipc.sock}}
REACTORS=${IPC_TARGET_REACTORS:-32}
CPUS=${IPC_TARGET_CPUS:-}
CPU_BASE=${IPC_TARGET_CPU_BASE:-}
LCORE_MAP=${IPC_TARGET_LCORE_MAP:-}
MEM_MB=${IPC_TARGET_MEM_MB:-8192}
IOVA_MODE=${IPC_TARGET_IOVA_MODE:-pa}
QPAIR_DEPTH=${IPC_TARGET_QPAIR_DEPTH:-1024}

COMMAND=${1:-}
if [[ $# -gt 0 ]]; then
	shift
fi
case "$COMMAND" in
	start|restart)
		[[ $# -gt 0 ]] && TRID=$1
		[[ $# -gt 1 ]] && SOCKET=$2
		;;
esac

pci_addr() {
	sed -n 's/.*traddr:\([^ ]*\).*/\1/p' <<<"$TRID"
}

process_matches() {
	local pid=$1
	local cmd
	[[ -r "/proc/$pid/cmdline" ]] || return 1
	cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
	[[ "$cmd" == *ucm_spdk_ipc_target* && "$cmd" == *"--socket $SOCKET"* ]]
}

find_pid() {
	local pid=""
	if [[ -f "$PIDFILE" ]]; then
		pid=$(cat "$PIDFILE" 2>/dev/null || true)
	fi
	if [[ -n "$pid" ]] && process_matches "$pid"; then
		echo "$pid"
		return 0
	fi
	local found
	found=$(pgrep -f -- "ucm_spdk_ipc_target.*--socket $SOCKET" 2>/dev/null | head -n1 || true)
	if [[ -n "$found" ]] && process_matches "$found"; then
		echo "$found"
		return 0
	fi
	return 1
}

print_stats() {
	python3 - "$SOCKET" <<'PY' || true
import json
import socket
import sys

path = sys.argv[1]
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(2)
        conn.connect(path)
        conn.sendall(b"UCM_STATS\n")
        chunks = []
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
    data = json.loads(b"".join(chunks))
except Exception as error:
    print(f"  stats     : unavailable ({error})")
    sys.exit(0)

read_gib = data["bytes_read"] / 2**30
write_gib = data["bytes_written"] / 2**30
tick_rate = data["tick_rate"] or 1
read_us = data["read_latency_ticks"] / tick_rate * 1e6 / data["num_read_ops"] if data["num_read_ops"] else 0
write_us = data["write_latency_ticks"] / tick_rate * 1e6 / data["num_write_ops"] if data["num_write_ops"] else 0
print(f"  stats     : read {read_gib:.2f} GiB / {data['num_read_ops']} ops / {read_us:.1f} us avg")
print(f"              write {write_gib:.2f} GiB / {data['num_write_ops']} ops / {write_us:.1f} us avg")
print(f"  sessions  : {data.get('connections', 0)} active, {data.get('qpairs', 0)} qpairs")
print(f"  errors    : completion={data.get('completion_errors', 0)} submit={data.get('submit_errors', 0)}")
PY
}

start() {
	local pid
	if pid=$(find_pid); then
		echo "[ucm-spdk-ipc] already running (pid $pid, socket $SOCKET)"
		return 0
	fi

	[[ -x "$TGT" ]] || {
		echo "ERROR: missing target binary: $TGT" >&2
		echo "       build it first: make -C testSPDK/ucm_spdk_ipc -j" >&2
		exit 1
	}
	local -a plan_args=(--reactors "$REACTORS")
	[[ -z "$CPUS" ]] || plan_args+=(--cpus "$CPUS")
	[[ -z "$CPU_BASE" ]] || plan_args+=(--cpu-base "$CPU_BASE")
	[[ -z "$LCORE_MAP" ]] || plan_args+=(--lcore-map "$LCORE_MAP")
	local plan
	plan=$(python3 "$REPO/scripts/spdk_ipc_cpu_plan.py" "${plan_args[@]}")
	local -a plan_lines
	mapfile -t plan_lines <<< "$plan"
	CPUS="${plan_lines[0]}"
	LCORE_MAP="${plan_lines[1]}"

	if systemctl is-active --quiet nvmf-loop; then
		echo "stopping nvmf-loop to release the NVMe controller ..."
		systemctl stop nvmf-loop
	fi

	local pci driver
	pci=$(pci_addr)
	if [[ -z "$pci" ]]; then
		echo "ERROR: cannot derive PCI address from trid: $TRID" >&2
		exit 1
	fi
	driver=""
	if [[ -e "/sys/bus/pci/devices/$pci/driver" ]]; then
		driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")")
	fi
	if [[ "$driver" != "vfio-pci" ]]; then
		echo "ERROR: $pci driver is '${driver:-none}', IPC target requires vfio-pci" >&2
		exit 1
	fi

	mkdir -p "$(dirname "$LOG")" "$(dirname "$PIDFILE")"
	rm -f "$PIDFILE"
	if [[ -S "$SOCKET" ]]; then
		rm -f "$SOCKET"
	fi

	local -a argv=(
		"$TGT"
		--trid "$TRID"
		--socket "$SOCKET"
		--lcore-map "$LCORE_MAP"
		--reactors "$REACTORS"
		--cpus "$CPUS"
		--mem-size "$MEM_MB"
		--iova-mode "$IOVA_MODE"
		--qpair-depth "$QPAIR_DEPTH"
	)
	printf 'starting IPC target:'
	printf ' %q' "${argv[@]}"
	printf '\n'

	nohup "${argv[@]}" >"$LOG" 2>&1 </dev/null &
	pid=$!
	echo "$pid" >"$PIDFILE"

	local i
	for i in $(seq 1 40); do
		if [[ -S "$SOCKET" ]] && kill -0 "$pid" 2>/dev/null; then
			break
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			break
		fi
		sleep 0.5
	done
	if [[ ! -S "$SOCKET" ]] || ! kill -0 "$pid" 2>/dev/null; then
		echo "ERROR: IPC target failed to start; log tail:" >&2
		tail -40 "$LOG" >&2 || true
		rm -f "$PIDFILE"
		exit 1
	fi

	chmod 0666 "$SOCKET" 2>/dev/null || true
	echo "[ucm-spdk-ipc] started (pid $pid)"
	echo "  socket : $SOCKET"
	echo "  trid   : $TRID"
	echo "  log    : $LOG"
}

stop() {
	local -a pids=()
	local pid i

	if pid=$(find_pid); then
		pids+=("$pid")
	fi
	if [[ ${#pids[@]} -eq 0 ]]; then
		local found
		found=$(pgrep -f -- "ucm_spdk_ipc_target.*--socket $SOCKET" 2>/dev/null || true)
		if [[ -n "$found" ]]; then
			mapfile -t pids <<<"$found"
		fi
	fi

	if [[ ${#pids[@]} -eq 0 ]]; then
		echo "[ucm-spdk-ipc] not running"
		rm -f "$PIDFILE"
		if [[ -S "$SOCKET" ]]; then
			rm -f "$SOCKET"
		fi
		return 0
	fi

	for pid in "${pids[@]}"; do
		echo "stopping IPC target (pid $pid) ..."
		kill -TERM "$pid" 2>/dev/null || true
	done
	for i in $(seq 1 50); do
		local alive=0
		for pid in "${pids[@]}"; do
			kill -0 "$pid" 2>/dev/null && alive=1
		done
		[[ "$alive" == 0 ]] && break
		sleep 0.2
	done
	for pid in "${pids[@]}"; do
		if kill -0 "$pid" 2>/dev/null; then
			echo "SIGTERM timed out, killing pid $pid"
			kill -KILL "$pid" 2>/dev/null || true
		fi
	done

	rm -f "$PIDFILE" "$SOCKET"
	echo "[ucm-spdk-ipc] stopped"
}

status() {
	local pid
	if ! pid=$(find_pid); then
		echo "[ucm-spdk-ipc] state: stopped"
		if [[ -S "$SOCKET" ]]; then
			echo "  stale socket: $SOCKET"
		fi
		return 1
	fi

	echo "[ucm-spdk-ipc] active (pid $pid)"
	echo "  socket : $SOCKET"
	echo "  log    : $LOG"
	if [[ ! -S "$SOCKET" ]]; then
		echo "  WARN   : process is alive but socket is missing"
		return 1
	fi
	echo "  mode   : $(stat -c '%a %U:%G' "$SOCKET" 2>/dev/null || echo unknown)"
	print_stats
	return 0
}

case "$COMMAND" in
	start)   start ;;
	stop)    stop ;;
	status)  status ;;
	restart) stop; start ;;
	*)       echo "usage: sudo $0 {start|stop|status|restart} [trid] [socket]" >&2; exit 1 ;;
esac
