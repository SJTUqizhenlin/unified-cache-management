#!/usr/bin/env bash
# server_test_pod.sh — server_test.sh 的 Pod 适配版（保留 server_test_host.sh 的测试目标）
#
# 与 server_test_host.sh 的差异（仅环境适配套路不同，预检/缓存/NPU 策略完全一致）：
#   1. 环境加载：Pod 镜像已预置 CANN/vLLM；没有 ~/ucm-serve-env-023.sh 时直接复用当前环境
#   2. /dev/shm 清理：Pod 内通常无 sudo 且以 root 运行，改用 scripts/cleanup_cache_files_pod.py
#      （root 时传 --allow-root，仍按 maps/FD 判活，不删正在使用的文件）
#   3. 端口检查：Pod 常无 iproute2/net-tools，port_in_use() 自动降级到 /proc/net/tcp
#   4. 启动前确保 ucm_patch.pth 已注入 site-packages（editable 重装后可能丢失）；
#      它会让 UCM 的 bind_memory no-op 补丁生效：保留 CPU/IRQ 绑核，禁用内核 migratepages
#   5. 其余与 host 版一致：6 步预检、构建新旧检查、孤儿进程保留、NPU BUSY 重试、缓存/配置生成
#
# 用法：
#   ./server_test_pod.sh [--ucm on|off] [--msprof [--msprof-dir DIR] [--msprof-duration SEC]]
#
#   --msprof              用 msprof app 模式启动 vllm（采集应用级数据，生成 db）；
#                         不指定 --msprof-duration 时一直采集到服务退出。
#   --msprof-dir DIR      指定 msprof 输出目录（默认 ~/ucm-prof/$PORT/msprof_<时间戳>）。
#   --msprof-duration SEC 只采集前 SEC 秒（msprof --duration）。
#   --msprof-delay SEC    延迟 SEC 秒再开始采集（msprof --delay，可用来跳过启动阶段）。
#   --msprof-type TYPE    db（默认，只导出统一 db，文件少）或 text（额外导出 mindstudio_profiler_output，文件多且大）。
#   --msprof 默认同时开启 --msproftx=on，采集 UCM 的 mstx 区间（名称前缀 ucm.）；
#   如不需要，设 MSPROF_MSTX=0。
#
# 环境变量覆盖（例如同时起一个 "plus" 对比实例）：
#   DEVICES=1,3,5,7 PORT=7880 CACHE_DIR=~/ucm-cache/test-plus ./server_test_pod.sh
#   NPU_BUSY_RETRY_SECONDS=1800 NPU_BUSY_MAX_RETRIES=0 KILL_ORPHANS=0 ./server_test_pod.sh
#   MSPROF=1 MSPROF_OUTPUT=/root/prof/vllm ./server_test_pod.sh
#   MSPROF_EXTRA_ARGS="--aic-metrics=MemoryAccess" MSPROF=1 ./server_test_pod.sh
#   MAX_MODEL_LEN=32768 MAX_NUM_SEQS=4 ./server_test_pod.sh
#   默认 GLM-5.2-W4A8：TP8 + EP，8K 上下文，单序列，eager，无 MTP/C8。

# ---------------- 可调参数 ----------------
UCM_ENABLED=1
CACHE_DIR="${CACHE_DIR:-$HOME/ucm-cache/test}"
MODEL="${MODEL:-/mnt/model/GLM-5.2-w4a8}"
DEVICES="${DEVICES:-0,1,2,3,4,5,6,7}"
PORT="${PORT:-7780}"
REPO="${REPO:-$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")}"
# 配置文件放在缓存目录外（避免被清空逻辑误删），按缓存目录名隔离
UCM_CONFIG="${UCM_CONFIG:-$HOME/ucm-cache/ucm_config_$(basename "${CACHE_DIR%/}").yaml}"
# NPU 占用策略：
#   KILL_ORPHANS=0            默认不杀任何已有 NPU 进程（包括上次崩溃遗留的孤儿进程）
#   NPU_BUSY_RETRY_SECONDS    所选 NPU 被占用时的重试间隔，默认 1800s
#   NPU_BUSY_MAX_RETRIES      重试次数上限；0 表示一直等，直到 NPU 释放
KILL_ORPHANS="${KILL_ORPHANS:-0}"
NPU_BUSY_RETRY_SECONDS="${NPU_BUSY_RETRY_SECONDS:-1800}"
NPU_BUSY_MAX_RETRIES="${NPU_BUSY_MAX_RETRIES:-0}"
# Hybrid multi-group KV 模型（如 DeepSeek-V4-Flash）不支持 recompute；默认 fail。
KV_LOAD_FAILURE_POLICY="${KV_LOAD_FAILURE_POLICY:-fail}"
# Posix store 的 GC 预算（GiB）；留空时沿用配置里的值，但若超过缓存目录可用空间会自动收敛。
POSIX_CAPACITY_GB="${POSIX_CAPACITY_GB:-}"
# GLM-5.2 的 store block 很大（约 14MB），data_dir_shard_bytes=3 会生成 16^3=4096 个
# 分片目录，GC 最低容量门槛约 805GiB。小盘默认收敛到 2（256 个目录，门槛约 51GiB）；
# 设为 1（16 个目录）或 0（单 data 目录）可进一步降低门槛。
DATA_DIR_SHARD_BYTES="${DATA_DIR_SHARD_BYTES:-2}"
# UCM Cache 共享缓冲区容量（GiB）；留空沿用 YAML。Pod 的 2MiB hugepage 限额需 >= 此值，
# 否则 memfd 会回退到 4K base pages（可运行但性能变差，并加重 migratepages）。
CACHE_BUFFER_CAPACITY_GB="${CACHE_BUFFER_CAPACITY_GB:-}"
# vllm-ascend 的 CPU 绑核默认保留（enable_cpu_binding=true）。UCM 的 boot patch 会把
# CpuAlloc.bind_memory 变成 no-op，只跳过内核 migratepages，taskset/IRQ 绑核仍然生效。
# 设为 0 可彻底关闭 CPU 绑定（一般不需要）。
ENABLE_CPU_BINDING="${ENABLE_CPU_BINDING:-1}"
# msprof app 模式：显式开启后在 msprof 下启动 vllm，产出应用级 db。
MSPROF="${MSPROF:-0}"
MSPROF_OUTPUT="${MSPROF_OUTPUT:-}"
MSPROF_DURATION="${MSPROF_DURATION:-}"
MSPROF_DELAY="${MSPROF_DELAY:-}"
MSPROF_TYPE="${MSPROF_TYPE:-db}"
MSPROF_MSTX="${MSPROF_MSTX:-1}"
MSPROF_EXTRA_ARGS="${MSPROF_EXTRA_ARGS:-}"

usage() {
  echo "Usage: $0 [--ucm on|off] [--msprof [--msprof-dir DIR] [--msprof-duration SEC] [--msprof-delay SEC] [--msprof-type text|db]]"
}

# Port check that works with or without iproute2/net-tools (pods often ship neither `ss` nor `netstat`).
port_in_use() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -tln 2>/dev/null | awk -v p=":$port" '$4 ~ p"$" {found=1} END{exit !found}'
  elif command -v netstat >/dev/null 2>&1; then
    netstat -tln 2>/dev/null | awk -v p=":$port" '$4 ~ p"$" {found=1} END{exit !found}'
  else
    python3 - "$port" <<'PY'
import sys

port = int(sys.argv[1])
want = f":{port:04X}"
for name in ("/proc/net/tcp", "/proc/net/tcp6"):
    try:
        with open(name) as table:
            for line in table:
                fields = line.split()
                if fields and fields[0] != "sl" and fields[3] == "0A" and fields[1].endswith(want):
                    sys.exit(0)
    except OSError:
        pass
sys.exit(1)
PY
  fi
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
    --msprof)
      MSPROF=1
      shift
      ;;
    --msprof-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: --msprof-dir requires a directory." >&2
        usage >&2
        exit 2
      fi
      MSPROF_OUTPUT="$2"
      shift 2
      ;;
    --msprof-duration)
      if [[ $# -lt 2 ]]; then
        echo "Error: --msprof-duration requires seconds." >&2
        usage >&2
        exit 2
      fi
      MSPROF_DURATION="$2"
      shift 2
      ;;
    --msprof-delay)
      if [[ $# -lt 2 ]]; then
        echo "Error: --msprof-delay requires seconds." >&2
        usage >&2
        exit 2
      fi
      MSPROF_DELAY="$2"
      shift 2
      ;;
    --msprof-type)
      if [[ $# -lt 2 ]]; then
        echo "Error: --msprof-type requires text or db." >&2
        usage >&2
        exit 2
      fi
      MSPROF_TYPE="$2"
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
# Pod 镜像已预置环境；只有宿主机风格的 env 文件存在时才 source。
if [[ -f "$HOME/ucm-serve-env-023.sh" ]]; then
  source "$HOME/ucm-serve-env-023.sh" || exit 1    # CANN 9.1.0 + vLLM/Ascend 0.23
elif [[ -z "${ASCEND_TOOLKIT_HOME:-}" ]]; then
  echo "ERROR: 未找到 $HOME/ucm-serve-env-023.sh，且当前环境未预置 CANN（ASCEND_TOOLKIT_HOME 为空）" >&2
  exit 1
fi
export ENABLE_UCM_PATCH="$UCM_ENABLED"          # 覆盖 env 文件默认值，支持 --ucm off
export PLATFORM=ascend
export ASCEND_ROOT="$ASCEND_TOOLKIT_HOME"       # Follow the selected CANN, never override with the old path.
export ASCEND_RT_VISIBLE_DEVICES="$DEVICES"
export UCM_LOG_PATH="$HOME/ucm-logs/$PORT"
mkdir -p "$UCM_LOG_PATH" "$HOME/ucm-cache"
# Preserve native CANN stderr as well as Python logs for crash diagnosis.
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

# b) Clean only this user's unused Cache files.
# Pod 内通常无 sudo 且以 root 运行；用 pod 版脚本的 --allow-root 清理 UID 0（仍按 maps/FD 判活）。
CLEAN_PRIV=()
if [[ "$(id -u)" -ne 0 ]] && command -v sudo >/dev/null 2>&1; then
  CLEAN_PRIV=(sudo -n)
fi
CLEAN_ROOT=()
if [[ "$(id -u)" -eq 0 ]]; then
  CLEAN_ROOT=(--allow-root)
fi
if ! timeout 20s "${CLEAN_PRIV[@]}" python3 "$REPO/scripts/cleanup_cache_files_pod.py" \
    --uid "$(id -u)" "${CLEAN_ROOT[@]}" --directory /dev/shm; then
  echo "    WARN: Cache 残留检查未完成，保守保留文件" >&2
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
  src_t=$( { find "$REPO/ucm" -type f \( -name '*.cc' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' -o -name 'CMakeLists.txt' \) -printf '%T@\n' 2>/dev/null || true
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

echo "==> [2.5/6] UCM 启动补丁注入（ucm_patch.pth + pod NUMA 对齐）"
if [[ "$UCM_ENABLED" == "1" ]]; then
  SITE_PACKAGES=$(python3 -c 'import site; print(site.getsitepackages()[0])')
  for spec in "ucm_patch.pth:ucm_patch.pth" "ucm_pod_numa.pth:zz_ucm_pod_numa.pth"; do
    src_name="${spec%%:*}"
    dst_name="${spec##*:}"
    src="$REPO/$src_name"
    dst="$SITE_PACKAGES/$dst_name"
    if [[ ! -f "$src" ]]; then
      echo "    WARN: 未找到 $src，跳过 $dst_name" >&2
      continue
    fi
    src_real=$(readlink -f "$src")
    if [[ "$(readlink -f "$dst" 2>/dev/null)" != "$src_real" ]]; then
      ln -sf "$src_real" "$dst"
      echo "    已注入: $dst -> $src_real"
    else
      echo "    ✓ $dst 已就绪"
    fi
  done
else
  echo "    --ucm off，跳过"
fi

echo "==> [3/6] Python 环境预检"
python - <<'PY'
import os
import torch, torch_npu, vllm, vllm_ascend  # noqa: F401
from vllm_ascend.cpu_binding import CpuAlloc
import ucm.integration.vllm.cache_config as cache_config
print(f"    torch {torch.__version__} | npu {torch.npu.device_count()} | vllm {vllm.__version__} OK")
patched = getattr(CpuAlloc, "_ucm_bind_memory_patched", False)
print(f"    UCM bind_memory patch: {'active (CPU/IRQ 绑核保留，migratepages 禁用)' if patched else 'MISSING'}")
if os.environ.get("ENABLE_UCM_PATCH") == "1" and not patched:
    raise SystemExit(
        "ERROR: UCM bind_memory 补丁未生效；worker 会卡在 migratepages。"
        "请检查 ucm_patch.pth 是否在 site-packages 且位于 editable .pth 之后加载。"
    )
aligned = getattr(cache_config, "_ucm_pod_numa_align", False)
print(f"    Pod NUMA align patch: {'active' if aligned else 'MISSING (使用 PCI NUMA)'}")
if os.environ.get("ENABLE_UCM_PATCH") == "1" and not aligned:
    print("    WARN: pod NUMA 对齐补丁未生效，UCM memfd 可能落在相邻 NPU 的 NUMA 节点")
PY

echo "==> [4/6] 模型检查: $MODEL"
if [[ ! -f "$MODEL/config.json" ]]; then
  echo "ERROR: 模型不存在或 NFS 未挂载: $MODEL" >&2
  exit 1
fi

echo "==> [5/6] 端口检查"
if port_in_use "$PORT"; then
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

  # b) UCM 配置：首次从 example 生成；已有配置允许复用，但会自动修正失效的 storage_backends。
  if [[ ! -f "$UCM_CONFIG" ]]; then
    CONFIG_TEMPLATE="$REPO/examples/ucm_config_example.yaml"
    if [[ "$(basename "${MODEL%/}")" == GLM-5.2-w4a8 ]]; then
      CONFIG_TEMPLATE="$REPO/examples/ucm_config_glm52_pod.yaml"
    fi
    sed -E "s|^([[:space:]]*storage_backends:[[:space:]]*).*|\1\"$CACHE_DIR\"|" \
      "$CONFIG_TEMPLATE" > "$UCM_CONFIG"
    echo "    已生成 UCM 配置: $UCM_CONFIG (storage_backends=$CACHE_DIR)"
  else
    echo "    复用已有 UCM 配置: $UCM_CONFIG"
  fi
  if ! grep -q "storage_backends" "$UCM_CONFIG"; then
    echo "ERROR: $UCM_CONFIG 缺少 storage_backends 字段" >&2
    exit 1
  fi

  # 已有配置可能来自其它机器（如 /home/qizhenlin/...）：路径不存在或非本实例 CACHE_DIR 时自动纠正。
  CONFIG_BACKEND=$(sed -nE 's|^[[:space:]]*storage_backends:[[:space:]]*"?([^"#]*)"?.*|\1|p' \
    "$UCM_CONFIG" | head -1 | tr -d '[:space:]')
  if [[ -z "$CONFIG_BACKEND" || "$CONFIG_BACKEND" != "$CACHE_DIR" || ! -d "$CONFIG_BACKEND" ]]; then
    sed -i -E "s|^([[:space:]]*storage_backends:[[:space:]]*).*|\1\"$CACHE_DIR\"|" "$UCM_CONFIG"
    echo "    已修正 UCM storage_backends: ${CONFIG_BACKEND:-<empty>} -> $CACHE_DIR"
  fi
  mkdir -p "$CACHE_DIR"

  # GLM-5.2：降低 Posix 分片目录数量，避免 GC 因 block 太大而要求超大容量。
  # data_dir_shard_bytes=N 对应 16^N 个目录；旧配置没有该字段时自动补上。
  if [[ "$(basename "${MODEL%/}")" == GLM-5.2-w4a8 ]]; then
    CONFIG_SHARD_BYTES=$(sed -nE \
      's|^[[:space:]]*data_dir_shard_bytes:[[:space:]]*([0-9]+).*|\1|p' \
      "$UCM_CONFIG" | head -1)
    if [[ -z "$CONFIG_SHARD_BYTES" ]]; then
      sed -i -E \
        "s|^([[:space:]]*)(posix_capacity_gb:.*)$|\1\2\n\1data_dir_shard_bytes: $DATA_DIR_SHARD_BYTES|" \
        "$UCM_CONFIG"
      echo "    UCM data_dir_shard_bytes: unset -> $DATA_DIR_SHARD_BYTES（降低 GLM-5.2 GC 容量门槛）"
    elif [[ "$CONFIG_SHARD_BYTES" != "$DATA_DIR_SHARD_BYTES" ]]; then
      sed -i -E \
        "s|^([[:space:]]*data_dir_shard_bytes:[[:space:]]*)[0-9]+.*|\1$DATA_DIR_SHARD_BYTES|" \
        "$UCM_CONFIG"
      echo "    UCM data_dir_shard_bytes: $CONFIG_SHARD_BYTES -> $DATA_DIR_SHARD_BYTES（降低 GLM-5.2 GC 容量门槛）"
    fi
  fi

  # posix_capacity_gb 是 Posix store 的 GC 预算（HMA 会再对 FA/WA 各分一半），不是预分配；
  # 但预算超过实际可用空间会在写满时触发 ENOSPC，因此这里默认收敛到可用空间的 90%。
  AVAIL_GB=$(df -Pk "$CACHE_DIR" 2>/dev/null | awk 'NR==2 {printf "%d", $4/1024/1024}')
  CONFIG_CAP=$(sed -nE 's|^[[:space:]]*posix_capacity_gb:[[:space:]]*([0-9]+).*|\1|p' \
    "$UCM_CONFIG" | head -1)
  TARGET_CAP="$CONFIG_CAP"
  if [[ -n "$POSIX_CAPACITY_GB" ]]; then
    if ! [[ "$POSIX_CAPACITY_GB" =~ ^[1-9][0-9]*$ ]]; then
      echo "ERROR: POSIX_CAPACITY_GB 必须是正整数（GiB），当前: $POSIX_CAPACITY_GB" >&2
      exit 1
    fi
    TARGET_CAP="$POSIX_CAPACITY_GB"
  elif [[ -n "$CONFIG_CAP" && -n "$AVAIL_GB" && "$CONFIG_CAP" -gt "$AVAIL_GB" ]]; then
    TARGET_CAP=$(( AVAIL_GB * 9 / 10 ))
    (( TARGET_CAP < 1 )) && TARGET_CAP=1
  fi
  if [[ -n "$TARGET_CAP" && "$TARGET_CAP" != "$CONFIG_CAP" ]]; then
    sed -i -E "s|^([[:space:]]*posix_capacity_gb:[[:space:]]*)[0-9]+.*|\1$TARGET_CAP|" "$UCM_CONFIG"
    echo "    UCM posix_capacity_gb: ${CONFIG_CAP:-unset} -> $TARGET_CAP GiB（缓存目录可用 ${AVAIL_GB:-?} GiB）"
  fi

  # cache_buffer_capacity_gb 走 memfd，Pod 的 2MiB hugepage 限额不够时会回退到 base pages。
  CONFIG_BUFFER=$(sed -nE 's|^[[:space:]]*cache_buffer_capacity_gb:[[:space:]]*([0-9]+).*|\1|p' \
    "$UCM_CONFIG" | head -1)
  if [[ -n "$CACHE_BUFFER_CAPACITY_GB" ]]; then
    if ! [[ "$CACHE_BUFFER_CAPACITY_GB" =~ ^[1-9][0-9]*$ ]]; then
      echo "ERROR: CACHE_BUFFER_CAPACITY_GB 必须是正整数（GiB），当前: $CACHE_BUFFER_CAPACITY_GB" >&2
      exit 1
    fi
    if [[ "$CACHE_BUFFER_CAPACITY_GB" != "$CONFIG_BUFFER" ]]; then
      sed -i -E "s|^([[:space:]]*cache_buffer_capacity_gb:[[:space:]]*)[0-9]+.*|\1$CACHE_BUFFER_CAPACITY_GB|" "$UCM_CONFIG"
      echo "    UCM cache_buffer_capacity_gb: ${CONFIG_BUFFER:-unset} -> $CACHE_BUFFER_CAPACITY_GB GiB"
    fi
    CONFIG_BUFFER="$CACHE_BUFFER_CAPACITY_GB"
  fi
  HUGEPAGE_LIMIT_BYTES=""
  for f in /sys/fs/cgroup/hugetlb/hugetlb.2MB.limit_in_bytes \
           /sys/fs/cgroup/hugetlb.2MB.max; do
    if [[ -r "$f" ]]; then HUGEPAGE_LIMIT_BYTES=$(cat "$f" 2>/dev/null); break; fi
  done
  if [[ -n "$CONFIG_BUFFER" && "$HUGEPAGE_LIMIT_BYTES" =~ ^[0-9]+$ && "$HUGEPAGE_LIMIT_BYTES" -gt 0 ]]; then
    HUGEPAGE_LIMIT_GB=$(( HUGEPAGE_LIMIT_BYTES / 1024 / 1024 / 1024 ))
    if (( CONFIG_BUFFER > HUGEPAGE_LIMIT_GB )); then
      echo "    WARN: cache_buffer_capacity_gb=${CONFIG_BUFFER}GB > Pod 2MiB hugepage 限额=${HUGEPAGE_LIMIT_GB}GB；" \
           "约 $((CONFIG_BUFFER - HUGEPAGE_LIMIT_GB))GB 会回退到 base pages（可运行，但更慢）。" \
           "可提高 Pod hugepage 限额，或用 CACHE_BUFFER_CAPACITY_GB=${HUGEPAGE_LIMIT_GB} 覆盖。" >&2
    fi
  fi

  UCM_ARGS=(
    --kv-transfer-config
    '{
      "kv_connector": "UCMConnector",
      "kv_connector_module_path": "ucm.integration.vllm.ucm_connector",
      "kv_role": "kv_both",
      "kv_load_failure_policy": "'"$KV_LOAD_FAILURE_POLICY"'",
      "kv_connector_extra_config": {
        "UCM_CONFIG_FILE": "'"$UCM_CONFIG"'"
      }
    }'
  )
else
  echo "==> [6/6] UCM 已关闭（--ucm off），跳过缓存配置"
  UCM_ARGS=()
fi

# ---------------- msprof app 模式 ----------------
if [[ "$MSPROF" == "1" ]]; then
  if ! command -v msprof >/dev/null 2>&1; then
    echo "ERROR: --msprof 需要 msprof 在 PATH 中（CANN toolkit）" >&2
    exit 1
  fi
  if [[ -z "$MSPROF_OUTPUT" ]]; then
    MSPROF_OUTPUT="$HOME/ucm-prof/$PORT/msprof_$(date +%Y%m%d_%H%M%S)"
  fi
  case "$MSPROF_TYPE" in
    text|db) ;;
    *) echo "ERROR: --msprof-type 必须是 text 或 db，当前: $MSPROF_TYPE" >&2; exit 2 ;;
  esac
  mkdir -p "$(dirname "$MSPROF_OUTPUT")"
  MSPROF_ARGS=(--output="$MSPROF_OUTPUT" --type="$MSPROF_TYPE")
  if [[ -n "$MSPROF_DELAY" ]]; then
    if ! [[ "$MSPROF_DELAY" =~ ^[1-9][0-9]*$ ]]; then
      echo "ERROR: --msprof-delay 必须是正整数（秒），当前: $MSPROF_DELAY" >&2
      exit 2
    fi
    MSPROF_ARGS+=(--delay="$MSPROF_DELAY")
  fi
  if [[ -n "$MSPROF_DURATION" ]]; then
    if ! [[ "$MSPROF_DURATION" =~ ^[1-9][0-9]*$ ]]; then
      echo "ERROR: --msprof-duration 必须是正整数（秒），当前: $MSPROF_DURATION" >&2
      exit 2
    fi
    MSPROF_ARGS+=(--duration="$MSPROF_DURATION")
  fi
  if [[ "$MSPROF_MSTX" == "1" && "$MSPROF_EXTRA_ARGS" != *"--msproftx"* ]]; then
    MSPROF_ARGS+=(--msproftx=on)
  fi
  if [[ -n "$MSPROF_EXTRA_ARGS" ]]; then
    read -r -a _msprof_extra <<< "$MSPROF_EXTRA_ARGS"
    MSPROF_ARGS+=("${_msprof_extra[@]}")
  fi
  echo "==> msprof app 模式：$(command -v msprof)"
  echo "    输出: $MSPROF_OUTPUT"
  echo "    注意: 不指定 --msprof-duration 时采集到服务退出；请拿走完整 PROF_* 目录"
else
  MSPROF_ARGS=()
fi

echo "==================================================================="
echo " UCM: $([[ "$UCM_ENABLED" == "1" ]] && echo on || echo off) | model: $(basename "$MODEL")"
echo " devices: $DEVICES (TP=$TP) | port: $PORT"
echo " cache: ${CACHE_DIR:-(ucm off)} | UCM log: $UCM_LOG_PATH"
echo "==================================================================="

MODEL_ARGS=()
DEFAULT_MAX_MODEL_LEN=65536
# CPU/IRQ 绑核保留；启动补丁单独禁用 migratepages。
if [[ "$ENABLE_CPU_BINDING" == "1" ]]; then
  MODEL_ARGS+=(--additional-config '{"enable_cpu_binding": true}')
else
  MODEL_ARGS+=(--additional-config '{"enable_cpu_binding": false}')
fi
if [[ "$(basename "${MODEL%/}")" == GLM-5.2-w4a8 ]]; then
  DEFAULT_MAX_MODEL_LEN=8192
  # 只使用 checkpoint 自带的 shared-indexer 配置；不覆盖 index_topk_freq/offset。
  # W4A8 权重量化与 C8 KV 量化独立；首轮使用 auto KV dtype，不启用 MTP。
  export PYTORCH_NPU_ALLOC_CONF="${PYTORCH_NPU_ALLOC_CONF:-expandable_segments:True}"
  export HCCL_BUFFSIZE="${HCCL_BUFFSIZE:-200}"
  export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"
  MODEL_ARGS+=(--quantization ascend --enable-expert-parallel
    --max-num-seqs "${MAX_NUM_SEQS:-1}"
    --max-num-batched-tokens "${MAX_NUM_BATCHED_TOKENS:-2048}"
    --enable-chunked-prefill --kv-cache-dtype auto)
else
  if [[ -n "${MAX_NUM_SEQS:-}" ]]; then
    MODEL_ARGS+=(--max-num-seqs "$MAX_NUM_SEQS")
  fi
  if [[ -n "${MAX_NUM_BATCHED_TOKENS:-}" ]]; then
    MODEL_ARGS+=(--max-num-batched-tokens "$MAX_NUM_BATCHED_TOKENS")
  fi
fi
# UCM on/off 使用相同的 HBM prefix cache 策略，便于对比外部缓存收益。
MODEL_ARGS+=(--enable-prefix-caching)

echo " limits: max_model_len=${MAX_MODEL_LEN:-$DEFAULT_MAX_MODEL_LEN} gpu_memory_utilization=${GPU_MEMORY_UTILIZATION:-0.9} model_args=${MODEL_ARGS[*]}"

VLLM_CMD=(
  vllm serve "$MODEL"
  --served-model-name "$MODEL"
  --max-model-len "${MAX_MODEL_LEN:-$DEFAULT_MAX_MODEL_LEN}"
  --tensor-parallel-size "$TP" --gpu_memory_utilization "${GPU_MEMORY_UTILIZATION:-0.9}"
  --block_size 128 --trust-remote-code --port "$PORT" --enforce-eager
  "${MODEL_ARGS[@]}"
  "${UCM_ARGS[@]}"
)

if [[ "$MSPROF" == "1" ]]; then
  echo "==> exec under msprof (${MSPROF_ARGS[*]})"
  exec msprof "${MSPROF_ARGS[@]}" "${VLLM_CMD[@]}"
fi
exec "${VLLM_CMD[@]}"
