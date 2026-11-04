#!/usr/bin/env bash
# server_test_host.sh — server_test.sh 的宿主机（裸机）适配版
#
# 与 pod 版 server_test.sh 的差异：
#   1. 自动加载宿主机环境：CANN 9.1.0 + conda(vllm-ucm-023) + ENABLE_UCM_PATCH
#   2. 移除每次启动的 `pip install -e .`：UCM 已 editable 安装，改代码即时生效
#   3. 缓存目录改为当前用户私有目录（默认 ~/ucm-cache/test），不再使用共享的 /mnt/test
#   4. 破坏性操作前置安全检查：
#      - /dev/shm 存在他人 uc_shm_cache* → 中止；本用户残留 → 清理
#      - 缓存目录必须位于 $HOME 下且属主为本人，才允许清空
#   5. 修正 pod 硬编码路径（UCM_CONFIG_FILE），配置文件首次自动生成、之后可手工编辑
#   6. TP 数量自动跟随 DEVICES 数量；启动前预检 6 步（残留自检/构建新旧/Python/模型/端口/UCM 配置）
#      残留自检 = 关服三查的前置版：默认保留已有孤儿进程、不杀任何已有 NPU 进程；
#      所选 NPU 被占用时，每隔 NPU_BUSY_RETRY_SECONDS（默认 1800s）重试，直到释放。
#      KILL_ORPHANS=1 可恢复旧的孤儿进程清理；NPU_BUSY_MAX_RETRIES 可限制重试次数。
#      构建新旧检查 = C++/构建文件 mtime 新于最新 .so 时自动重编译（覆盖 unstaged 编辑 /
#      git pull / 切分支三种过期场景）；SKIP_REBUILD=1 跳过；--ucm off 时只提示不重编
#
# 用法：
#   ./server_test_host.sh [--ucm on|off]
#
# 环境变量覆盖（例如同时起一个 "plus" 对比实例）：
#   DEVICES=1,3,5,7 PORT=7880 CACHE_DIR=~/ucm-cache/test-plus ./server_test_host.sh
#   NPU_BUSY_RETRY_SECONDS=1800 NPU_BUSY_MAX_RETRIES=0 KILL_ORPHANS=0 ./server_test_host.sh
# IPC target placement is normally read from UCM_CONFIG as one line:
#   spdk_trid: "ipc:<socket>|<target-trid>"
#   e.g. "ipc:/tmp/ucm_spdk_ipc.sock|trtype:PCIe traddr:0000:84:00.0"
# Legacy separate ipc_target_trid is still accepted.
# Other target knobs (lcore map, reactors, cpu base, mem size, IOVA mode,
# pidfile/log) keep script defaults and can be overridden by IPC_TARGET_*.
# Defaults distribute 32 pollers across NUMA nodes; IPC_TARGET_CPUS overrides the list.
# IPC targets on the configured socket are restarted before each server launch.

# ---------------- 可调参数 ----------------
UCM_ENABLED=1
CACHE_DIR="${CACHE_DIR:-$HOME/ucm-cache/test}"
# MODEL="${MODEL:-/mnt/model/DeepSeek-V2-Lite-Chat}"
MODEL="${MODEL:-/mnt/model/DeepSeek-V4-Flash-w8a8-mtp}"
DEVICES="${DEVICES:-0,1,2,3,4,5,6,7}"
PORT="${PORT:-7780}"
REPO="$HOME/devA2/unified-cache-management"
# 配置文件放在缓存目录外（避免被清空逻辑误删），按缓存目录名隔离
UCM_CONFIG="$HOME/ucm-cache/ucm_config_$(basename "${CACHE_DIR%/}").yaml"
# NPU 占用策略：
#   KILL_ORPHANS=0            默认不杀任何已有 NPU 进程（包括上次崩溃遗留的孤儿进程）
#   NPU_BUSY_RETRY_SECONDS    所选 NPU 被占用时的重试间隔，默认 1800s
#   NPU_BUSY_MAX_RETRIES      重试次数上限；0 表示一直等，直到 NPU 释放
KILL_ORPHANS="${KILL_ORPHANS:-0}"
NPU_BUSY_RETRY_SECONDS="${NPU_BUSY_RETRY_SECONDS:-1800}"
NPU_BUSY_MAX_RETRIES="${NPU_BUSY_MAX_RETRIES:-0}"
# Optional local IPC target (used when spdk_trid starts with ipc:).  The
# target owns the raw NVMe device; workers do not need vfio.
IPC_TARGET_TRID="${IPC_TARGET_TRID:-}"
IPC_TARGET_PCI_ADDR="${IPC_TARGET_PCI_ADDR:-}"
IPC_TARGET_REACTORS="${IPC_TARGET_REACTORS:-32}"
IPC_TARGET_CPUS="${IPC_TARGET_CPUS:-}"
IPC_TARGET_CPU_BASE="${IPC_TARGET_CPU_BASE:-}"
IPC_TARGET_LCORE_MAP="${IPC_TARGET_LCORE_MAP:-}"
IPC_TARGET_MEM_MB="${IPC_TARGET_MEM_MB:-8192}"
IPC_TARGET_IOVA_MODE="${IPC_TARGET_IOVA_MODE:-pa}"
# Hugepage overhead beyond Cache payload and target memory, in MiB.
IPC_HUGEPAGE_RESERVE_MB="${IPC_HUGEPAGE_RESERVE_MB:-2048}"
# Optional override for shared-total (1) versus per-rank Cache copies.
IPC_CACHE_MULTIPLIER="${IPC_CACHE_MULTIPLIER:-}"
IPC_TARGET_PIDFILE="${IPC_TARGET_PIDFILE:-$HOME/ucm-cache/ucm_spdk_ipc_target.pid}"
IPC_TARGET_LOG="${IPC_TARGET_LOG:-$HOME/ucm-logs/ucm_spdk_ipc_target.log}"

usage() {
  echo "Usage: $0 [--ucm on|off]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ucm)
      if [[ $# -lt 2 ]]; then
        echo "Error: --ucm requires on or off." >&2
        usage >&2
        exit 2
      fi
      case "$2" in
        on)  UCM_ENABLED=1 ;;
        off) UCM_ENABLED=0 ;;
        *)
          echo "Error: --ucm must be on or off, got '$2'." >&2
          usage >&2
          exit 2
          ;;
      esac
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: unknown argument '$1'." >&2
      usage >&2
      exit 2
      ;;
  esac
done

# ---------------- 环境加载（在 set -e 之前，避免 CANN set_env 的非零返回干扰） ----------------
source "$HOME/ucm-serve-env-023.sh" || exit 1    # CANN 9.1.0 + vLLM/Ascend 0.23
export ENABLE_UCM_PATCH="$UCM_ENABLED"          # 覆盖 env 文件默认值，支持 --ucm off
export PLATFORM=ascend
export ASCEND_ROOT="$ASCEND_TOOLKIT_HOME"       # Follow the selected CANN, never override with the old path.
export SPDK_ROOT_DIR="${SPDK_ROOT_DIR:-$HOME/devSPDK/spdk}"  # spdkstore 后端定位 SPDK
export ASCEND_RT_VISIBLE_DEVICES="$DEVICES"
export UCM_LOG_PATH="$HOME/ucm-logs/$PORT"
mkdir -p "$UCM_LOG_PATH" "$HOME/ucm-cache"
# Preserve native SPDK/CANN stderr as well as Python logs for crash diagnosis.
SERVER_LOG="$UCM_LOG_PATH/server_$(date +%Y%m%d_%H%M%S)_$$.log"
exec > >(tee -a "$SERVER_LOG") 2>&1
export PYTHONFAULTHANDLER=1
export PYTHONUNBUFFERED=1
echo "==> 完整 server stdout/stderr: $SERVER_LOG"
cd "$REPO"

set -euo pipefail

if ! [[ "$NPU_BUSY_RETRY_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: NPU_BUSY_RETRY_SECONDS 必须是正整数（秒），当前: $NPU_BUSY_RETRY_SECONDS" >&2
  exit 2
fi
if ! [[ "$NPU_BUSY_MAX_RETRIES" =~ ^[0-9]+$ ]]; then
  echo "ERROR: NPU_BUSY_MAX_RETRIES 必须是非负整数，当前: $NPU_BUSY_MAX_RETRIES" >&2
  exit 2
fi

TP=$(awk -F',' '{print NF}' <<< "$DEVICES")

# ---------------- 预检 ----------------
echo "==> [1/6] 上次实例残留自检（默认保留已有进程；shm / 占卡检查）"
echo "    NPU 占用策略: KILL_ORPHANS=${KILL_ORPHANS}，BUSY 时间隔 ${NPU_BUSY_RETRY_SECONDS}s 重试（上限 ${NPU_BUSY_MAX_RETRIES}，0=一直等）"

# a) 孤儿进程检查：本用户属主 + PPID=1（父进程已死被 init 接管）的 vllm worker/EngineCore/resource_tracker
#    默认只报告不清理（KILL_ORPHANS=0）；设 KILL_ORPHANS=1 才恢复旧的 SIGTERM/SIGKILL 清理
#    活着的 worker 父进程一定是 EngineCore，PPID=1 即为上次崩溃的残留，会占着 NPU 卡导致本次启动失败
ORPHANS=$(ps -eo pid=,ppid=,uid=,comm=,args= 2>/dev/null | awk -v uid="$(id -u)" '
  $3 == uid && $2 == 1 {
    if ($4 ~ /^VLLM::(Worker|EngineCor)/) print $1
    else if (index($0, "resource_tracker") > 0) print $1
  }') || ORPHANS=""
if [[ -n "$ORPHANS" ]]; then
  if [[ "$KILL_ORPHANS" == "1" ]]; then
    echo "    发现上次残留的孤儿进程，正在清理: $(echo $ORPHANS | tr '\n' ' ')"
    kill $ORPHANS 2>/dev/null || true
    for i in $(seq 1 10); do
      ALIVE=""
      for p in $ORPHANS; do [[ -d /proc/$p ]] && ALIVE="$ALIVE $p"; done
      [[ -z "$ALIVE" ]] && break
      sleep 1
    done
    if [[ -n "${ALIVE:-}" ]]; then
      echo "    SIGTERM 未退出，升级 SIGKILL:$ALIVE"
      kill -9 $ALIVE 2>/dev/null || true
      sleep 2
    fi
    echo "    ✓ 孤儿进程已清理"
  else
    echo "    发现上次残留的孤儿进程（KILL_ORPHANS=0，保留不杀）: $(echo $ORPHANS | tr '\n' ' ')"
    echo "    若它们占用所选 NPU，后续将等待 NPU 释放后重试；需要清理请设 KILL_ORPHANS=1"
  fi
else
  echo "    ✓ 无孤儿进程"
fi

# b) Clean only this user's unused Cache files, including explicit hugepages.
# sudo lets the inode-based maps/FD check see root IPC target mappings too.
cache_cleanup_dirs=(--directory /dev/shm --directory /dev/hugepages)
if [[ -f "$UCM_CONFIG" ]]; then
  cache_hugepage_dirs=$(python - "$UCM_CONFIG" <<'PY'
import sys
import yaml

with open(sys.argv[1]) as stream:
    config = yaml.safe_load(stream)
for entry in config.get("ucm_connectors", []):
    cfg = entry.get("ucm_connector_config", {})
    if cfg.get("shm_use_hugepage_file"):
        print(cfg.get("shm_hugepage_dir", "/dev/hugepages"))
PY
  )
  while IFS= read -r directory; do
    [[ -z "$directory" ]] || cache_cleanup_dirs+=(--directory "$directory")
  done <<< "$cache_hugepage_dirs"
fi
if ! timeout 20s sudo -n python3 "$REPO/scripts/cleanup_cache_files.py" \
    --uid "$(id -u)" "${cache_cleanup_dirs[@]}"; then
  echo "    WARN: Cache 残留检查未完成，保守保留文件；后续大页预检仍会检查可用容量" >&2
fi

# c) 所选 NPU 的 HBM 基线检查（基线约 3.4GB；>8GB 视为被占用）
#    注：npu-smi 表格中每张卡两行 —— 第一行含 NPU 编号，第二行（含 Bus-Id）才有 HBM
#    默认不杀占用者：BUSY 时按 NPU_BUSY_RETRY_SECONDS 等待后重试；
#    NPU_BUSY_MAX_RETRIES=0 表示一直等到释放。非 BUSY 的检查/解析错误立即退出。
NPU_RETRY=0
while :; do
  # set -e 下不能直接捕获非零退出码，先关闭 errexit 再执行检查
  set +e
  HBM_OUTPUT=$(npu-smi info 2>&1 | python "$REPO/scripts/check_npu_hbm.py" --devices "$DEVICES" 2>&1)
  HBM_RC=$?
  set -e
  echo "$HBM_OUTPUT"

  if [[ "$HBM_RC" == "0" ]]; then
    break
  fi

  if [[ "$HBM_OUTPUT" != *"BUSY"* ]]; then
    echo "ERROR: NPU 占用预检失败（非 BUSY，可能是 npu-smi 输出或设备编号错误）；不重试" >&2
    exit 1
  fi

  NPU_RETRY=$((NPU_RETRY + 1))
  if (( NPU_BUSY_MAX_RETRIES > 0 && NPU_RETRY > NPU_BUSY_MAX_RETRIES )); then
    echo "ERROR: NPU 仍被占用，已达到重试上限 ${NPU_BUSY_MAX_RETRIES} 次；退出" >&2
    exit 1
  fi
  echo "    NPU 被占用，${NPU_BUSY_RETRY_SECONDS}s 后重试（第 ${NPU_RETRY} 次）..."
  sleep "$NPU_BUSY_RETRY_SECONDS"
done
echo "    ✓ 所选 NPU 已释放，继续启动"

echo "==> [2/6] UCM 构建新旧检查（源码 mtime vs 编译产物 .so）"
# 规则：C++/构建文件的时间戳新于最新 .so → 需要重编（覆盖 unstaged 编辑 / pull / 切分支三种情况）
# SKIP_REBUILD=1 可跳过；--ucm off 时不重编（不加载 UCM 二进制），仅提示
if [[ "${SKIP_REBUILD:-0}" == "1" ]]; then
  echo "    SKIP_REBUILD=1 → 跳过构建检查"
else
  # 注意：不能用 "| sort -rn | head -1"（head 提前退出 → sort 收到 SIGPIPE(141) → pipefail 杀脚本），
  # 用 awk 一站式取最大值（读完全部输入，无提前退出）
  # testSPDK/ucm_spdk_store/ucm_spdk_store.c 是 spdkstore 的引擎源码（CMake 直接编译它），
  # 必须纳入 mtime 检查，否则只改引擎不触发重编 → 源码树 .so 静默过期
  src_t=$( { find "$REPO/ucm" -type f \( -name '*.cc' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' -o -name 'CMakeLists.txt' \) -printf '%T@\n' 2>/dev/null || true
             find "$REPO/testSPDK/ucm_spdk_store" -type f \( -name '*.c' -o -name '*.h' \) -printf '%T@\n' 2>/dev/null || true
             find "$REPO/ucm/store/spdk" -type f \( -name '*.c' -o -name 'Makefile' -o -name '*.sh' \) -printf '%T@\n' 2>/dev/null || true
             stat -c '%Y' "$REPO/CMakeLists.txt" "$REPO/setup.py" "$REPO/version.ini" 2>/dev/null || true; } | \
           awk '$0+0>m{m=$0+0} END{if(NR>0) printf "%.6f", m}')
  so_t=$(find "$REPO/ucm" -type f -name '*.so' -printf '%T@\n' 2>/dev/null | \
           awk '$0+0>m{m=$0+0} END{if(NR>0) printf "%.6f", m}')
  need=0
  if [[ -z "$so_t" ]]; then
    need=1
    echo "    未找到编译产物 .so → 需要构建"
  elif awk -v a="${src_t:-0}" -v b="$so_t" 'BEGIN{exit !(a>b)}'; then
    need=1
    echo "    源码新于构建产物（$(date -d "@${src_t%.*}" '+%m-%d %H:%M') > $(date -d "@${so_t%.*}" '+%m-%d %H:%M')）→ 需要重编"
  else
    echo "    ✓ 构建产物与源码同步（构建于 $(date -d "@${so_t%.*}" '+%m-%d %H:%M')）"
  fi
  if [[ "$need" == "1" ]]; then
    if [[ "$UCM_ENABLED" != "1" ]]; then
      echo "    --ucm off 本次不加载 UCM 二进制，跳过重编（下次 --ucm on 会自动执行）"
    else
      echo "    重编译中（约 4-5 分钟，日志 /tmp/ucm_rebuild.log）..."
      if ( cd "$REPO" && pip install -e . --no-build-isolation > /tmp/ucm_rebuild.log 2>&1 ); then
        echo "    ✓ 重编译完成"
      else
        echo "ERROR: UCM 重编译失败，日志尾部：" >&2
        tail -20 /tmp/ucm_rebuild.log >&2
        exit 1
      fi
    fi
  fi
fi

echo "==> [3/6] Python 环境预检"
python - <<'PY'
import torch, torch_npu, vllm, vllm_ascend  # noqa: F401
print(f"    torch {torch.__version__} | npu {torch.npu.device_count()} | vllm {vllm.__version__} OK")
PY

echo "==> [4/6] 模型检查: $MODEL"
if [[ ! -f "$MODEL/config.json" ]]; then
  echo "ERROR: 模型不存在或 NFS 未挂载: $MODEL" >&2
  exit 1
fi

echo "==> [5/6] 端口检查"
if ss -tln 2>/dev/null | grep -q ":$PORT "; then
  echo "ERROR: 端口 $PORT 已被占用（上次服务可能还活着？）" >&2
  exit 1
fi
echo "    ✓ 端口 $PORT 空闲"

if [[ "$UCM_ENABLED" == "1" ]]; then
  echo "==> [6/6] UCM 缓存目录 / 配置"

  # a) 缓存目录：必须位于 $HOME 下且属主是本人，才允许清空
  #    KEEP_CACHE=1 跳过磁盘缓存清理（验证跨重启 SSD 命中：重启后同样请求应出现 hit external）
  #    注：shm 为进程级 buffer，重启后无法复用，仍会清理本用户残留
  if [[ "${KEEP_CACHE:-0}" == "1" ]]; then
    echo "    KEEP_CACHE=1 → 保留磁盘缓存 $CACHE_DIR（$(find "$CACHE_DIR" -type f 2>/dev/null | wc -l) 个文件）"
  else
    if [[ "$CACHE_DIR" != "$HOME"/* ]]; then
      echo "ERROR: CACHE_DIR 必须位于 $HOME 之下（当前: $CACHE_DIR）" >&2
      exit 1
    fi
    if [[ -d "$CACHE_DIR" ]]; then
      if [[ ! -O "$CACHE_DIR" ]]; then
        echo "ERROR: $CACHE_DIR 属主非当前用户，拒绝清空" >&2
        exit 1
      fi
      echo "    清空缓存目录: $CACHE_DIR"
      rm -rf "${CACHE_DIR:?}"/*
    else
      mkdir -p "$CACHE_DIR"
    fi
  fi

  # b) UCM 配置：首次从 example 生成（写入实际缓存路径），之后复用、可手工编辑
  if [[ ! -f "$UCM_CONFIG" ]]; then
    sed "s|storage_backends: \"/mnt/test\"|storage_backends: \"$CACHE_DIR\"|" \
      "$REPO/examples/ucm_config_example.yaml" > "$UCM_CONFIG"
    echo "    已生成 UCM 配置: $UCM_CONFIG (storage_backends=$CACHE_DIR)"
  else
    echo "    复用已有 UCM 配置: $UCM_CONFIG ($(grep storage_backends "$UCM_CONFIG" | sed -n '1p' | tr -d ' '))"
  fi
  if ! grep -q "storage_backends" "$UCM_CONFIG"; then
    echo "ERROR: $UCM_CONFIG 缺少 storage_backends 字段" >&2
    exit 1
  fi

  # The selected SPDK backend must be ready before SPDK workers initialize.
  SPDK_INFO=$(python - "$UCM_CONFIG" <<'PY'
import sys
import yaml

with open(sys.argv[1]) as f:
    config = yaml.safe_load(f)
enabled = 0
trid = ""
target = {}
target_keys = (
    "ipc_target_trid",
)
for entry in config.get("ucm_connectors", []):
    cfg = entry.get("ucm_connector_config", {})
    pipeline = str(cfg.get("store_pipeline", "")).lower().split("|")
    if "spdk" in pipeline:
        enabled = 1
        trid = str(cfg.get("spdk_trid", "")).strip()
        target = {key: cfg.get(key, "") for key in target_keys}
        break
print("\x1f".join([str(enabled), trid] + [str(target.get(key, "")) for key in target_keys]))
PY
  )
  IFS=$'\x1f' read -r SPDK_ENABLED SPDK_TRID Y_IPC_TRID <<< "$SPDK_INFO"
  IPC_TARGET_TRID="${IPC_TARGET_TRID:-$Y_IPC_TRID}"
  if [[ "$SPDK_ENABLED" == "1" ]]; then
    if [[ "$SPDK_TRID" == ipc:* ]]; then
      IPC_SPEC="${SPDK_TRID#ipc:}"
      IPC_SOCKET="${IPC_SPEC%%|*}"
      if [[ "$IPC_SPEC" == *'|'* && -z "$IPC_TARGET_TRID" ]]; then
        IPC_TARGET_TRID="${IPC_SPEC#*|}"
      fi
      if [[ -z "$IPC_TARGET_PCI_ADDR" && -n "$IPC_TARGET_TRID" ]]; then
        IPC_TARGET_PCI_ADDR=$(sed -n 's/.*traddr:\([^ ]*\).*/\1/p' <<< "$IPC_TARGET_TRID")
      fi
      if [[ -z "$IPC_SOCKET" ]]; then
        echo "ERROR: spdk_trid 是 ipc: 但没有提供 socket 路径" >&2
        exit 1
      fi
      if [[ -z "$IPC_TARGET_TRID" || -z "$IPC_TARGET_PCI_ADDR" ]]; then
        echo "ERROR: 使用 ipc: 时必须提供 target 设备，例如 spdk_trid: 'ipc:/tmp/ucm_spdk_ipc.sock|trtype:PCIe traddr:0000:84:00.0'" >&2
        exit 1
      fi
      # Use the same NUMA-interleaved list for DPDK, pollers and worker exclusion.
      cpu_plan_args=(--reactors "$IPC_TARGET_REACTORS")
      [[ -z "$IPC_TARGET_CPUS" ]] || cpu_plan_args+=(--cpus "$IPC_TARGET_CPUS")
      [[ -z "$IPC_TARGET_CPU_BASE" ]] || cpu_plan_args+=(--cpu-base "$IPC_TARGET_CPU_BASE")
      [[ -z "$IPC_TARGET_LCORE_MAP" ]] || cpu_plan_args+=(--lcore-map "$IPC_TARGET_LCORE_MAP")
      cpu_plan=$(python "$REPO/scripts/spdk_ipc_cpu_plan.py" "${cpu_plan_args[@]}")
      mapfile -t cpu_plan_lines <<< "$cpu_plan"
      IPC_TARGET_CPUS="${cpu_plan_lines[0]}"
      IPC_TARGET_LCORE_MAP="${cpu_plan_lines[1]}"
      export UCM_IPC_TARGET_CPUS="$IPC_TARGET_CPUS"
      echo "==> SPDK 配置：本地 IPC target ($IPC_SOCKET)"
      if [[ ! -x "$REPO/testSPDK/ucm_spdk_ipc/ucm_spdk_ipc_target" ]]; then
        echo "ERROR: 缺少 IPC target 二进制：$REPO/testSPDK/ucm_spdk_ipc/ucm_spdk_ipc_target" >&2
        echo "       先执行：make -C testSPDK/ucm_spdk_ipc -j" >&2
        exit 1
      fi

      # Match executable and socket arguments, not a stale/reused pidfile PID.
      # Restarting this target also disconnects any other workers sharing it.
      echo "    重启 IPC target；共享此 socket 的其他 worker 将断开连接"
      sudo python3 - "$IPC_SOCKET" <<'PY'
import os
from pathlib import Path
import signal
import sys
import time

socket_path = sys.argv[1]

def identity(pid):
    try:
        proc = Path("/proc") / str(pid)
        args = (proc / "cmdline").read_bytes().split(b"\0")
        if not args or os.path.basename(os.fsdecode(args[0])) != "ucm_spdk_ipc_target":
            return None
        sockets = [os.fsdecode(args[i + 1]) for i, arg in enumerate(args[:-1])
                   if arg == b"--socket"]
        sockets += [os.fsdecode(arg.split(b"=", 1)[1]) for arg in args
                    if arg.startswith(b"--socket=")]
        if socket_path not in sockets:
            return None
        return (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None

targets = {}
for proc in Path("/proc").iterdir():
    if proc.name.isdigit():
        pid = int(proc.name)
        token = identity(pid)
        if token is not None:
            targets[pid] = token
for sig in (signal.SIGTERM, signal.SIGKILL):
    for pid, token in targets.items():
        if identity(pid) == token:
            print(f"    stopping IPC target pid={pid} signal={sig.name}", flush=True)
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + (10 if sig == signal.SIGTERM else 5)
    while any(identity(pid) == token for pid, token in targets.items()):
        if time.monotonic() >= deadline:
            break
        time.sleep(0.1)
if any(identity(pid) == token for pid, token in targets.items()):
    sys.exit("ERROR: old IPC target did not exit; refusing to start another")
PY

        # Starting the IPC target requires exclusive ownership of the NVMe.
        if systemctl is-active --quiet nvmf-loop; then
          echo "    停止 nvmf-loop，释放 $IPC_TARGET_PCI_ADDR ..."
          sudo "$REPO/testSPDK/nvmf_loop.sh" stop
        fi
        driver=""
        if [[ -e "/sys/bus/pci/devices/$IPC_TARGET_PCI_ADDR/driver" ]]; then
          driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$IPC_TARGET_PCI_ADDR/driver")")
        fi
        if [[ "$driver" != "vfio-pci" ]]; then
          echo "ERROR: $IPC_TARGET_PCI_ADDR 当前驱动为 '${driver:-none}'，IPC target 需要 vfio-pci。" >&2
          echo "       先按 testSPDK/SPDK_BUILD_INSTALL.md 执行 setup.sh config 绑定 vfio。" >&2
          exit 1
        fi
        # DeepSeek MLA ranks share a UUID-backed Cache; other layouts are
        # conservatively budgeted per device unless explicitly overridden.
        ipc_cache_multiplier="$IPC_CACHE_MULTIPLIER"
        if [[ -z "$ipc_cache_multiplier" ]]; then
          ipc_cache_multiplier=$(python - "$MODEL/config.json" "$TP" <<'PY'
import json
import sys

with open(sys.argv[1]) as stream:
    model = json.load(stream)
shared = model.get("model_type") in {"deepseek_v2", "deepseek_v3", "deepseek_v4"}
print(1 if shared else int(sys.argv[2]))
PY
          )
        fi
        python "$REPO/scripts/ensure_spdk_hugepages.py" \
          --config "$UCM_CONFIG" --target-mem-mb "$IPC_TARGET_MEM_MB" \
          --devices "$DEVICES" --setup-script "$SPDK_ROOT_DIR/scripts/setup.sh" \
          --reserve-mb "$IPC_HUGEPAGE_RESERVE_MB" \
          --cache-multiplier "$ipc_cache_multiplier"
        mkdir -p "$(dirname "$IPC_TARGET_LOG")" "$(dirname "$IPC_TARGET_PIDFILE")"
        sudo rm -f -- "$IPC_TARGET_PIDFILE"
        if [[ -S "$IPC_SOCKET" ]]; then sudo rm -f -- "$IPC_SOCKET"; fi
        IPC_CMD=(
          "$REPO/testSPDK/ucm_spdk_ipc/ucm_spdk_ipc_target"
          --trid "$IPC_TARGET_TRID"
          --socket "$IPC_SOCKET"
          --lcore-map "$IPC_TARGET_LCORE_MAP"
          --reactors "$IPC_TARGET_REACTORS"
          --cpus "$IPC_TARGET_CPUS"
          --mem-size "$IPC_TARGET_MEM_MB"
          --iova-mode "$IPC_TARGET_IOVA_MODE"
        )
        echo "    启动 IPC target: ${IPC_CMD[*]}"
        sudo bash -c 'umask 022; echo $$ > "$1"; log=$2; shift 2; exec "$@" > "$log" 2>&1' \
          bash "$IPC_TARGET_PIDFILE" "$IPC_TARGET_LOG" "${IPC_CMD[@]}" </dev/null &
        ipc_launcher_pid=$!
        for _ in $(seq 1 40); do
          [[ -S "$IPC_SOCKET" ]] && break
          sleep 1
        done
        ipc_pid=$(cat "$IPC_TARGET_PIDFILE" 2>/dev/null || true)
        if [[ ! -S "$IPC_SOCKET" ]] || [[ -z "$ipc_pid" ]] ||
           ! ps -p "$ipc_pid" >/dev/null 2>&1; then
          echo "ERROR: IPC target 未正常启动（socket=$IPC_SOCKET pid=${ipc_pid:-unknown}），日志尾部：" >&2
          sudo tail -40 "$IPC_TARGET_LOG" >&2 || true
          # A failed readiness check must not leave a late-starting target behind.
          sudo python3 - "$IPC_TARGET_PIDFILE" <<'PY'
import os
from pathlib import Path
import signal
import sys
import time

try:
    pid = int(Path(sys.argv[1]).read_text().strip())
    proc = Path("/proc") / str(pid)
    token = (proc / "stat").read_text().rsplit(")", 1)[1].split()[19]
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if (proc / "stat").read_text().rsplit(")", 1)[1].split()[19] != token:
            break
        if Path(os.readlink(proc / "exe")).name != "ucm_spdk_ipc_target":
            break
        os.kill(pid, sig)
        time.sleep(1)
except (OSError, ValueError, IndexError):
    pass
PY
          kill -TERM -- "$ipc_launcher_pid" 2>/dev/null || true
          sudo rm -f -- "$IPC_TARGET_PIDFILE"
          exit 1
        fi
        echo "    ✓ IPC target 已就绪 (pid $ipc_pid, log $IPC_TARGET_LOG)"
      if [[ ! -r "$IPC_SOCKET" || ! -w "$IPC_SOCKET" ]]; then
        echo "    修正 IPC socket 权限以允许当前用户 $(id -un) 访问: $IPC_SOCKET"
        sudo chmod 0666 "$IPC_SOCKET"
        if [[ ! -r "$IPC_SOCKET" || ! -w "$IPC_SOCKET" ]]; then
          echo "ERROR: IPC socket 仍不可访问: $IPC_SOCKET" >&2
          exit 1
        fi
      fi
    else
      echo "==> SPDK 配置：确保本机 nvmf-loop target 已启动"
      sudo "$REPO/testSPDK/nvmf_loop.sh" start
    fi
  fi

  # Cache|Empty still allocates the same host Cache, even without an IPC target.
  if [[ "$SPDK_ENABLED" != "1" ]]; then
    cache_multiplier="${IPC_CACHE_MULTIPLIER:-}"
    if [[ -z "$cache_multiplier" ]]; then
      cache_multiplier=$(python - "$MODEL/config.json" "$TP" <<'PY'
import json
import sys

with open(sys.argv[1]) as stream:
    model = json.load(stream)
print(1 if model.get("model_type") in {"deepseek_v2", "deepseek_v3", "deepseek_v4"} else int(sys.argv[2]))
PY
      )
    fi
    python "$REPO/scripts/ensure_spdk_hugepages.py" \
      --config "$UCM_CONFIG" --target-mem-mb 0 --devices "$DEVICES" \
      --setup-script "$SPDK_ROOT_DIR/scripts/setup.sh" \
      --reserve-mb "$IPC_HUGEPAGE_RESERVE_MB" --cache-multiplier "$cache_multiplier"
  fi

  UCM_ARGS=(
    --kv-transfer-config
    '{
      "kv_connector": "UCMConnector",
      "kv_connector_module_path": "ucm.integration.vllm.ucm_connector",
      "kv_role": "kv_both",
      "kv_load_failure_policy": "recompute",
      "kv_connector_extra_config": {
        "UCM_CONFIG_FILE": "'"$UCM_CONFIG"'"
      }
    }'
  )
else
  echo "==> [6/6] UCM 已关闭（--ucm off），跳过缓存配置"
  UCM_ARGS=()
fi

echo "==================================================================="
echo " UCM: $([[ "$UCM_ENABLED" == "1" ]] && echo on || echo off) | model: $(basename "$MODEL")"
echo " devices: $DEVICES (TP=$TP) | port: $PORT"
echo " cache: ${CACHE_DIR:-(ucm off)} | UCM log: $UCM_LOG_PATH"
echo "==================================================================="

MODEL_ARGS=()
DEFAULT_MAX_MODEL_LEN=65536
# if [[ "$(basename "${MODEL%/}")" == DeepSeek-V4-Flash* ]]; then
#   # Start V4 with a bounded profiling workload before testing long contexts.
#   DEFAULT_MAX_MODEL_LEN=8192
#   MODEL_ARGS+=(--enable-expert-parallel
#     --max-num-seqs "${MAX_NUM_SEQS:-4}"
#     --max-num-batched-tokens "${MAX_NUM_BATCHED_TOKENS:-2048}")
#   if [[ "$UCM_ENABLED" == "1" ]]; then
#     MODEL_ARGS+=(--enable-prefix-caching)
#   else
#     MODEL_ARGS+=(--no-enable-prefix-caching)
#   fi
# else
#   MODEL_ARGS+=(--enable-prefix-caching)
# fi

echo " limits: max_model_len=${MAX_MODEL_LEN:-$DEFAULT_MAX_MODEL_LEN} gpu_memory_utilization=${GPU_MEMORY_UTILIZATION:-0.9} model_args=${MODEL_ARGS[*]}"

exec vllm serve "$MODEL" \
  --served-model-name "$MODEL" \
  --max-model-len "${MAX_MODEL_LEN:-$DEFAULT_MAX_MODEL_LEN}" \
  --tensor-parallel-size "$TP" --gpu_memory_utilization "${GPU_MEMORY_UTILIZATION:-0.9}" \
  --block_size 128 --trust-remote-code --port "$PORT" --enforce-eager \
  "${MODEL_ARGS[@]}" \
  "${UCM_ARGS[@]}"
