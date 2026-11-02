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

# Target-side iobuf sizing. UCM shards are typically 3-6 MiB:
#   - max_io_size 8 MiB avoids splitting a shard into many 128 KiB requests
#     (must be a power of two; MDTS becomes log2(8 MiB / 4 KiB) = 11).
#   - 1 MiB large buffers keep SGE count <= 8 for an 8 MiB request
#     (RDMA requires max_io_size / large_bufsize <= 16).
#   - 2048 buffers is a fixed 2 GiB memory budget. Requests beyond it queue
#     on iobuf backpressure; they do not grow target memory.
# Raise IOBUF_LARGE_POOL_COUNT only together with nvmf_tgt -s / hugepages.
IOBUF_LARGE_BUFSIZE=1048576
IOBUF_LARGE_POOL_COUNT=2048
MAX_IO_SIZE=8388608

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

	echo "1/7 starting $UNIT (nvmf_tgt -m 0xF0 -s 8192 --wait-for-rpc) ..."
	systemd-run --unit="$UNIT" "$TGT" -m 0xF0 -s 8192 --wait-for-rpc
	if ! wait_rpc; then
		journalctl -u "$UNIT" --no-pager -n 30
		systemctl stop "$UNIT"
		echo "nvmf_tgt failed to start (log above)" >&2
		exit 1
	fi

	# iobuf_set_options is a STARTUP-only RPC: it must run before
	# framework_start_init, and nvmf_create_transport must run after it so
	# the SGE validation sees the configured large_bufsize.
	echo "2/7 setting iobuf (${IOBUF_LARGE_POOL_COUNT} x ${IOBUF_LARGE_BUFSIZE} bytes) ..."
	"$RPC" iobuf_set_options \
		--large-bufsize "$IOBUF_LARGE_BUFSIZE" \
		--large-pool-count "$IOBUF_LARGE_POOL_COUNT"

	echo "3/7 initializing subsystems ..."
	"$RPC" framework_start_init

	echo "4/7 creating RDMA transport (max-queue-depth 1024, max-io-size $MAX_IO_SIZE) ..."
	"$RPC" nvmf_create_transport -t RDMA -u 4096 \
		--max-queue-depth 1024 --max-io-size "$MAX_IO_SIZE"

	echo "5/7 attaching backend SSD $PCI_ADDR ..."
	"$RPC" bdev_nvme_attach_controller -b "$BDEV" -t PCIe -a "$PCI_ADDR"

	echo "6/7 creating subsystem + NS ..."
	"$RPC" nvmf_create_subsystem "$SUBSYS_NQN" -a -s "$SERIAL"
	"$RPC" nvmf_subsystem_add_ns "$SUBSYS_NQN" "${BDEV}n1"

	echo "7/7 adding RDMA listener $TRADDR:$TRSVCID ..."
	"$RPC" nvmf_subsystem_add_listener "$SUBSYS_NQN" -t RDMA -a "$TRADDR" -s "$TRSVCID"

	trap - ERR
	cat <<EOF

target ready: 84:00.0 now owned by $UNIT (PCIe 直连测试暂停).
iobuf      : ${IOBUF_LARGE_POOL_COUNT} x ${IOBUF_LARGE_BUFSIZE} bytes (large pool)
max-io-size: $MAX_IO_SIZE bytes
check      : sudo $RPC iobuf_get_stats
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
