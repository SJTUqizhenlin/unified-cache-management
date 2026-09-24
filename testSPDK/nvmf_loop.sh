#!/usr/bin/env bash
# nvmf_loop.sh — NVMe-oF RDMA 回环 target 一键启停
# 用法:
#   sudo ./nvmf_loop.sh start [traddr] [trsvcid]   # 默认 192.168.1.12:4420
#   sudo ./nvmf_loop.sh stop
#   sudo ./nvmf_loop.sh status                     # 只查状态，退出码 0=运行中 / 1=未运行
# 手动流程与背景见 testSPDK/SPDK_DEV_GUIDE.md 第五节。
# 注意: start 后 84:00.0 被 target 独占(PCIe 直连测试暂停), stop 后恢复。

set -euo pipefail

SPDK_ROOT=/home/qizhenlin/devSPDK/spdk
RPC="$SPDK_ROOT/scripts/rpc.py"
TGT="$SPDK_ROOT/build/bin/nvmf_tgt"
UNIT=nvmf-loop

PCI_ADDR=0000:84:00.0
SUBSYS_NQN=nqn.2016-06.io.spdk:c1
SERIAL=SPDK0001
BDEV=Nvme0

TRADDR=${2:-192.168.1.12}
TRSVCID=${3:-4420}

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root (sudo $0 ...)" >&2
	exit 1
fi

wait_rpc() {
	local i
	# socket 文件出现 = RPC server 就绪（版本无关的探测）
	for i in $(seq 1 50); do
		[ -S /var/tmp/spdk.sock ] && break
		sleep 0.2
	done
	[ -S /var/tmp/spdk.sock ] || return 1
	# 再确认一次 RPC 真正可答（v26.09 起方法名为 rpc_get_methods）
	for i in $(seq 1 20); do
		if "$RPC" rpc_get_methods >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.2
	done
	return 1
}

start() {
	if systemctl is-active --quiet "$UNIT"; then
		echo "[$UNIT] already running — stop it first ($0 stop)"
		exit 0
	fi
	systemctl reset-failed "$UNIT" 2>/dev/null || true

	if ! ip -o addr | grep -qw "$TRADDR"; then
		echo "WARN: $TRADDR not found on any local interface (RoCE IP wrong?)"
	fi

	trap 'rc=$?; echo "start failed, rolling back" >&2; \
		systemctl stop "$UNIT" 2>/dev/null || true; exit $rc' ERR

	echo "1/5 starting $UNIT (nvmf_tgt -m 0xF0 -s 8192) ..."
	systemd-run --unit="$UNIT" "$TGT" -m 0xF0 -s 8192
	if ! wait_rpc; then
		journalctl -u "$UNIT" --no-pager -n 30
		systemctl stop "$UNIT"
		echo "nvmf_tgt failed to start (log above)" >&2
		exit 1
	fi

	echo "2/5 creating RDMA transport (max-queue-depth 1024) ..."
	"$RPC" nvmf_create_transport -t RDMA -u 4096 --max-queue-depth 1024

	echo "3/5 attaching backend SSD $PCI_ADDR ..."
	"$RPC" bdev_nvme_attach_controller -b "$BDEV" -t PCIe -a "$PCI_ADDR"

	echo "4/5 creating subsystem + NS ..."
	"$RPC" nvmf_create_subsystem "$SUBSYS_NQN" -a -s "$SERIAL"
	"$RPC" nvmf_subsystem_add_ns "$SUBSYS_NQN" "${BDEV}n1"

	echo "5/5 adding RDMA listener $TRADDR:$TRSVCID ..."
	"$RPC" nvmf_subsystem_add_listener "$SUBSYS_NQN" -t RDMA -a "$TRADDR" -s "$TRSVCID"

	trap - ERR
	cat <<EOF

target ready: 84:00.0 now owned by $UNIT (PCIe 直连测试暂停).
connect with e.g.:
  sudo ./nvme_iops_test -r 'trtype:RDMA adrfam:IPv4 traddr:$TRADDR trsvcid:$TRSVCID' -o 144 -S 8 -t 5 -w randread
teardown:
  sudo $0 stop
EOF
}

stop() {
	if ! systemctl is-active --quiet "$UNIT"; then
		echo "[$UNIT] not running"
		exit 0
	fi
	echo "stopping $UNIT ..."
	systemctl stop "$UNIT"
	echo "done. PCIe 直连 84:00.0 已恢复"
}

status() {
	local state subs nqn n

	state=$(systemctl is-active "$UNIT" 2>/dev/null) || true
	if [[ "$state" != "active" ]]; then
		echo "[$UNIT] state: ${state:-unknown}（PCIe 直连 84:00.0 可用）"
		return 1
	fi

	echo "[$UNIT] active (running)，84:00.0 被 target 独占（PCIe 直连测试暂停）"

	subs=$("$RPC" nvmf_get_subsystems 2>/dev/null) || subs=""
	if [[ -z "$subs" ]]; then
		echo "  WARN: RPC 未响应（target 刚启动或异常）"
		return 0
	fi

	echo "$subs" | python3 -c '
import json, sys

try:
    subs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for s in subs:
    if s.get("subtype") == "Discovery":
        continue
    print("  subsystem : %s" % s.get("nqn", "?"))
    ns = [n.get("bdev_name", "?") for n in s.get("namespaces", [])]
    print("  namespace : %s" % (", ".join(ns) if ns else "-"))
    la = []
    for a in s.get("listen_addresses", []):
        t = a.get("transport") or a.get("trtype") or "?"
        la.append("%s %s:%s" % (t, a.get("traddr", "?"), a.get("trsvcid", "?")))
    print("  listener  : %s" % (", ".join(la) if la else "-"))
'
	nqn=$(echo "$subs" | python3 -c '
import json, sys

try:
    subs = [s for s in json.load(sys.stdin) if s.get("subtype") != "Discovery"]
except Exception:
    subs = []
print(subs[0]["nqn"] if subs else "")
')
	if [[ -n "$nqn" ]]; then
		n=$("$RPC" nvmf_subsystem_get_controllers "$nqn" 2>/dev/null | python3 -c '
import json, sys

try:
    d = json.load(sys.stdin)
except Exception:
    d = []
print(len(d) if isinstance(d, list) else 0)
') || n="?"
		echo "  initiators: $n connected"
	fi
	return 0
}

case "${1:-}" in
	start)  start ;;
	stop)   stop ;;
	status) status ;;
	*)      echo "usage: sudo $0 start [traddr] [trsvcid] | stop | status" >&2; exit 1 ;;
esac
