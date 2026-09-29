#!/usr/bin/env bash
# server_test_host.sh — server_test.sh 的宿主机（裸机）适配版
#
# 与 pod 版 server_test.sh 的差异：
#   1. 自动加载宿主机环境：CANN(~/ascend) + conda(vllm-ucm) + ENABLE_UCM_PATCH
#   2. 移除每次启动的 `pip install -e .`：UCM 已 editable 安装，改代码即时生效
#   3. 缓存目录改为当前用户私有目录（默认 ~/ucm-cache/test），不再使用共享的 /mnt/test
#   4. 破坏性操作前置安全检查：
#      - /dev/shm 存在他人 uc_shm_cache* → 中止；本用户残留 → 清理
#      - 缓存目录必须位于 $HOME 下且属主为本人，才允许清空
#   5. 修正 pod 硬编码路径（UCM_CONFIG_FILE），配置文件首次自动生成、之后可手工编辑
#   6. TP 数量自动跟随 DEVICES 数量；启动前预检 6 步（残留自检/构建新旧/Python/模型/端口/UCM 配置）
#      残留自检 = 关服三查的前置版：自动清理上次崩溃遗留的孤儿进程（PPID=1 的 worker，
#      会占卡导致启动失败）与未映射的 shm 泄漏；HBM 被占则提前报错而不是让 vllm 崩
#      构建新旧检查 = C++/构建文件 mtime 新于最新 .so 时自动重编译（覆盖 unstaged 编辑 /
#      git pull / 切分支三种过期场景）；SKIP_REBUILD=1 跳过；--ucm off 时只提示不重编
#
# 用法：
#   ./server_test_host.sh [--ucm on|off]
#
# 环境变量覆盖（例如同时起一个 "plus" 对比实例）：
#   DEVICES=1,3,5,7 PORT=7880 CACHE_DIR=~/ucm-cache/test-plus ./server_test_host.sh

# ---------------- 可调参数 ----------------
UCM_ENABLED=1
CACHE_DIR="${CACHE_DIR:-$HOME/ucm-cache/test}"
MODEL="${MODEL:-/mnt/model/DeepSeek-V2-Lite-Chat}"
DEVICES="${DEVICES:-0,1,2,3}"
PORT="${PORT:-7780}"
REPO="$HOME/devA2/unified-cache-management"
# 配置文件放在缓存目录外（避免被清空逻辑误删），按缓存目录名隔离
UCM_CONFIG="$HOME/ucm-cache/ucm_config_$(basename "${CACHE_DIR%/}").yaml"

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
source "$HOME/ucm-serve-env.sh"                 # CANN(~/ascend) + conda(vllm-ucm) + ENABLE_UCM_PATCH=1
export ENABLE_UCM_PATCH="$UCM_ENABLED"          # 覆盖 env 文件默认值，支持 --ucm off
export PLATFORM=ascend
export ASCEND_ROOT="$HOME/ascend/ascend-toolkit/latest"   # UCM 定位 CANN（宿主机非默认路径）
export SPDK_ROOT_DIR="${SPDK_ROOT_DIR:-$HOME/devSPDK/spdk}"  # spdkstore 后端定位 SPDK
export ASCEND_RT_VISIBLE_DEVICES="$DEVICES"
export UCM_LOG_PATH="$HOME/ucm-logs/$PORT"
mkdir -p "$UCM_LOG_PATH" "$HOME/ucm-cache"
cd "$REPO"

set -euo pipefail

TP=$(awk -F',' '{print NF}' <<< "$DEVICES")

# ---------------- 预检 ----------------
echo "==> [1/6] 上次实例残留自检（Ctrl+C 崩溃可能遗留孤儿进程 / shm / 占卡）"

# a) 孤儿进程清理：本用户属主 + PPID=1（父进程已死被 init 接管）的 vllm worker/EngineCore/resource_tracker
#    活着的 worker 父进程一定是 EngineCore，PPID=1 即为上次崩溃的残留，会占着 NPU 卡导致本次启动失败
ORPHANS=$(ps -eo pid=,ppid=,uid=,comm=,args= 2>/dev/null | awk -v uid="$(id -u)" '
  $3 == uid && $2 == 1 {
    if ($4 ~ /^VLLM::(Worker|EngineCor)/) print $1
    else if (index($0, "resource_tracker") > 0) print $1
  }') || ORPHANS=""
if [[ -n "$ORPHANS" ]]; then
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
  echo "    ✓ 无孤儿进程"
fi

# b) /dev/shm 残留：用 /proc/*/maps 判断是否仍被存活进程映射
#    未映射 → 上次崩溃的泄漏（曾出现过 100GB tmpfs 泄漏）→ 清理本用户的
#    仍被映射 → 别的 UCM 实例正在运行 → 只提示、绝不删除（文件名含 per-instance UUID，不冲突）
#    注意：grep 读他人进程的 maps 会权限拒绝（exit 2），映射内容仍有效；
#         扫描超时（D 状态进程持锁）则保守视为 LIVE 不删除 —— 两者都不能让 set -e 杀脚本
SHM_STALE=0
for f in /dev/shm/uc_shm_cache*; do
  [[ -e "$f" ]] || continue
  gout=$(timeout 5 grep -lF "$f" /proc/[0-9]*/maps 2>/dev/null) && gstat=0 || gstat=$?
  if [[ "$gstat" == "124" ]]; then
    echo "    注意: $f 映射扫描超时（有进程处于 D 状态持锁），保守跳过不删除"
    continue
  fi
  mapped=$(sed -E 's#/proc/([0-9]+)/.*#\1#' <<< "$gout" | awk 'NR<=3')
  if [[ -n "$mapped" ]]; then
    echo "    注意: $f 仍被进程 $(echo $mapped | tr '\n' ' ') 使用（其它 UCM 实例在运行，跳过）"
  elif [[ -O "$f" ]]; then
    rm -f "$f"; SHM_STALE=1
  else
    echo "    注意: $f 为他人遗留且无进程使用（不动他人的文件）"
  fi
done
if [[ "$SHM_STALE" == "1" ]]; then
  echo "    ✓ 已清理本用户残留 shm（上次未正常退出的泄漏）"
fi
ls /dev/shm/uc_shm_cache* >/dev/null 2>&1 || echo "    ✓ /dev/shm 无 uc_shm_cache 残留"

# c) 所选 NPU 的 HBM 基线检查（基线约 3.4GB；>8GB 视为被占用 → 本实例必然启动失败，提前报错）
#    注：npu-smi 表格中每张卡两行 —— 第一行含 NPU 编号，第二行（含 Bus-Id）才有 HBM
NPU_BLOCKED=$(npu-smi info 2>/dev/null | tr -s ' ' | awk -F'|' -v devs=",$DEVICES," '
  $3 ~ /:[0-9a-f]+\./ && $2 ~ /^[ ]*[0-9]+[ ]*$/ {
    if (index(devs, "," npu ",") > 0) {
      s = $0; last = ""
      while (match(s, /[0-9]+ \/ [0-9]+/)) { last = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH) }
      split(last, u, " / ")
      if (u[1] + 0 > 8000) printf "    NPU %d HBM 已用 %s MB", npu, u[1]
    }
  }
  $2 ~ /^[ ]*[0-9]+[ ]+[^ ]/ { npu = $2 + 0 }') || NPU_BLOCKED=""
if [[ -n "$NPU_BLOCKED" ]]; then
  echo "ERROR: 所选设备中存在 HBM 占用（可能是他人任务或未清理干净）：" >&2
  echo "$NPU_BLOCKED" >&2
  echo "       请调整 DEVICES 环境变量换卡，或等待占用释放后再启动" >&2
  exit 1
fi
echo "    ✓ 所选 NPU（$DEVICES）HBM 均为基线水平"

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

exec vllm serve "$MODEL" \
  --served-model-name "$MODEL" \
  --max-model-len 65536 \
  --tensor-parallel-size "$TP" --gpu_memory_utilization 0.9 \
  --block_size 128 --trust-remote-code --port "$PORT" --enforce-eager \
  --enable-prefix-caching \
  "${UCM_ARGS[@]}"
