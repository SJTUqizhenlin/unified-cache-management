#!/usr/bin/env bash
# ucm_store_stats.sh — UCM Posix / SPDK 运行时统计（server/client 运行中随时可用）
#
# 三层数据源：
#   [逻辑层] vllm /metrics 的 ucm:* 系列（读写字节、block 数、命中率、任务平均时长/带宽、
#             Cache 层 load/dump 分解：存储等待 vs H2D/D2H）
#   [文件层] CACHE_DIR 落盘统计（唯一文件数、总字节、覆盖写次数）
#   [syscall层] /proc/<pid>/io（write/read syscall 次数与字节，自动定位 dump worker）
#
# 用法：
#   ./ucm_store_stats.sh                     # 快照模式：立即输出三层全景
#   ./ucm_store_stats.sh snapshot -i 5 -n 10 # 每 5s 输出完整快照，共 10 次
#   ./ucm_store_stats.sh snapshot -i 5       # 持续完整快照，Ctrl+C 停止
#   ./ucm_store_stats.sh watch               # 监视模式：每 10s 输出一行增量
#   ./ucm_store_stats.sh watch -i 5 -n 60    # 每 5s 一次，共 60 次
#   ./ucm_store_stats.sh probe [-t 10]       # strace 采样文件 IO：psync→pwrite64/pread64，
#                                            # aio→io_submit iocb(PREAD/PWRITE)+openat 计数
#
# 环境变量覆盖：
#   HOST=127.0.0.1  PORT=7780  CACHE_DIR=~/ucm-cache/test
#   STORE_BACKEND=auto|posix|spdk  UCM_CONFIG=/path/to/config.yaml
#   STATS_ALL_PROCESSES=1  包含日志、resource tracker、编译等辅助子进程（默认仅 EngineCore/Worker）
#   SPDK_RPC=/path/to/rpc.py  SPDK_RPC_SOCKET=/var/tmp/spdk.sock
#   SPDK_BDEV=Nvme0n1  SPDK_RPC_SUDO=1 (default; noninteractive sudo)
#   SPDK probe uses read-only RPC deltas, not strace. No counters are reset.
#   SPDK: files/syscalls and unexported Store capacity/GC/health are N/A.
#
# 提示：metrics 为累计值；窗口分析用 watch 模式（自动做差分）。
#       指标在有推理请求时才被 vllm 同步到 /metrics，空闲期数值不动是正常现象。
#       FAWA load barrier 显示非空加载屏障的累计 avg/p99（跨 rank 合并，非 NPU 空闲时长）；
#       watch 中该时延仍为累计值，p99 为 histogram 插值估算，超过最大桶只显示下界。
#       start_load_kv total 包含全部 hook 调用（含无 Load），与 FAWA 非空屏障样本集合不同。
#       注意 /proc 的 syscw 含大量与 UCM 无关的后台小写入（各 worker ~3k/s 的 8B IPC 写），
#       文件写次数请以 probe 模式或 UC_DEBUG 日志的 task_count 为准。
#       aio 引擎注意：libaio(io_submit) 不计入 /proc/<pid>/io（wchar/syscw 对 UCM IO 失明），
#       引擎从实际 YAML 读取（缺字段默认 psync），不按流量猜测；配置无法读取则显示 unknown。
#       若启动后修改了 YAML，应以启动日志为准；stats 不证明进程已应用新配置。

set -uo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-7780}"
CACHE_DIR="${CACHE_DIR:-$HOME/ucm-cache/test}"
URL="http://$HOST:$PORT/metrics"
MODE="snapshot"
INTERVAL=10
MAX_CYCLES=0
REPEAT_SNAPSHOT=0
PROBE_SEC=10

while [[ $# -gt 0 ]]; do
  case "$1" in
    snapshot)  MODE="snapshot" ;;
    watch)     MODE="watch" ;;
    probe)     MODE="probe" ;;
    -i)        [[ $# -ge 2 ]] || { echo "ERROR: -i requires seconds" >&2; exit 2; }; INTERVAL="$2"; REPEAT_SNAPSHOT=1; shift ;;
    -n)        [[ $# -ge 2 ]] || { echo "ERROR: -n requires a count" >&2; exit 2; }; MAX_CYCLES="$2"; REPEAT_SNAPSHOT=1; shift ;;
    -t)        PROBE_SEC="$2"; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)         echo "未知参数: $1（支持: snapshot | watch [-i 秒] [-n 次] | probe [-t 秒]）" >&2; exit 2 ;;
  esac
  shift
done
if [[ ! "$INTERVAL" =~ ^[1-9][0-9]*$ || ! "$MAX_CYCLES" =~ ^(0|[1-9][0-9]*)$ ]]; then
  echo "ERROR: -i must be a positive integer; -n must be a nonnegative integer (0 = unlimited)" >&2
  exit 2
fi
if [[ "$MODE" == "snapshot" && "$REPEAT_SNAPSHOT" == "0" ]]; then
  MAX_CYCLES=1
fi

# ---------------- 工具函数 ----------------
fmt_bytes() {  # 字节 → 人类可读（十进制 GB，与 metrics 口径一致）
  awk -v b="$1" 'BEGIN{
    if (b+0 >= 1e12)      printf "%.2f TB", b/1e12
    else if (b+0 >= 1e9)  printf "%.2f GB", b/1e9
    else if (b+0 >= 1e6)  printf "%.1f MB", b/1e6
    else if (b+0 >= 1e3)  printf "%.1f kB", b/1e3
    else                  printf "%d B", b }'
}
fmt_cnt() {
  awk -v b="$1" 'BEGIN{
    if (b+0 >= 1e9)      printf "%.2fB", b/1e9
    else if (b+0 >= 1e6) printf "%.2fM", b/1e6
    else if (b+0 >= 1e3) printf "%.1fk", b/1e3
    else                 printf "%d", b }'
}
fmt_rate() {  # bytes + 秒 → 速率
  awk -v b="$1" -v t="$2" 'BEGIN{ if (t+0 <= 0) {print "-"; exit}
    r=b/t; if (r >= 1e9) printf "%.2f GB/s", r/1e9; else if (r >= 1e6) printf "%.1f MB/s", r/1e6
    else if (r >= 1e3) printf "%.1f kB/s", r/1e3; else printf "%d B/s", r }'
}

# metrics 文本解析：对形如 name{labels} value 的行按 name 求和 / 取最大
msum() { awk -v n="^$1[{]" '$0 ~ n { v=$NF; if (v ~ /^-?[0-9]/) s += v } END { printf "%.0f", s+0 }' "$2"; }
mmax() { awk -v n="^$1[{]" '$0 ~ n { v=$NF; if (v ~ /^-?[0-9]/ && v+0 > m+0) m=v } END { printf "%.0f", m+0 }' "$2"; }
# 直方图平均：name_sum / name_count（缺失时输出 -）
mavg_ms() {
  local s c
  s=$(msum "${1}_sum" "$2"); c=$(msum "${1}_count" "$2")
  awk -v s="$s" -v c="$c" 'BEGIN{ if (c+0 > 0) printf "%.1f", s/c; else print "-" }'
}
# 某 metric 的 per-rank 明细（rank:GB 列表）
mrank() {
  grep -E "^$1[{]" "$2" 2>/dev/null | \
    sed -E 's/.*worker_rank="([^"]*)".*\} *([0-9.eE+-]+).*/\1 \2/' | sort -n | \
    awk '{ printf "%s:%s ", $1, sprintf("%.1fG", $2/1e9) }'
}

# ---------------- metrics 抓取 ----------------
METRICS=$(mktemp /tmp/ucm_stats_metrics.XXXXXX)
trap 'rm -f "$METRICS"' EXIT
fetch_metrics() {
  if ! curl -fsS --max-time 5 "$URL" -o "$METRICS" 2>/dev/null || [[ ! -s "$METRICS" ]]; then
    return 1
  fi
  grep -q "^ucm:" "$METRICS" || return 1
  return 0
}

# 汇总所有关键值（快照与监视共用的采集函数）：输出 "key value" 行
collect() {
  local f="$METRICS"
  echo "m.h2s        $(msum ucm:posix_h2s_bytes_total "$f")"
  echo "m.s2h        $(msum ucm:posix_s2h_bytes_total "$f")"
  echo "m.dump_shard $(msum ucm:cache_dump_shards_total "$f")"
  echo "m.dump_bytes $(msum ucm:cache_dump_bytes_total "$f")"
  echo "m.load_bytes $(msum ucm:cache_load_bytes_total "$f")"
  echo "m.lookup_q   $(msum ucm:posix_lookup_query_blocks_total "$f")"
  echo "m.lookup_h   $(msum ucm:posix_lookup_hit_blocks_total "$f")"
  echo "m.qtok       $(msum ucm:total_prefix_query_tokens_total "$f")"
  echo "m.hbm_tok    $(msum ucm:gpu_hbm_hit_tokens_total "$f")"
  echo "m.ucm_tok    $(msum ucm:ucm_hit_tokens_total "$f")"
  echo "m.avg_dump_ms $(mavg_ms ucm:posix_dump_task_duration_ms "$f")"
  echo "m.avg_load_ms $(mavg_ms ucm:posix_load_task_duration_ms "$f")"
  echo "m.avg_h2s_bw  $(mavg_ms ucm:posix_h2s_bandwidth_gbps "$f")"
  echo "m.avg_s2h_bw  $(mavg_ms ucm:posix_s2h_bandwidth_gbps "$f")"
  # Different task/shard populations: these durations are not an additive decomposition.
  echo "m.cache_load_ms   $(mavg_ms ucm:cache_load_duration_ms "$f")"
  echo "m.cache_dump_ms   $(mavg_ms ucm:cache_dump_duration_ms "$f")"
  echo "m.bkwait_ms       $(mavg_ms ucm:cache_shard_backend_wait_ms "$f")"
  echo "m.h2d_submit_ms   $(mavg_ms ucm:cache_h2d_submit_ms "$f")"
  echo "m.h2d_sync_ms     $(mavg_ms ucm:cache_h2d_sync_ms "$f")"
  echo "m.d2h_ms          $(mavg_ms ucm:cache_d2h_duration_ms "$f")"
  echo "m.mkbuf_ms        $(mavg_ms ucm:cache_dump_mkbuf_duration_ms "$f")"
  echo "m.prereq_ms       $(mavg_ms ucm:cache_dump_prereq_wait_ms "$f")"
  # 关键路径判定指标：dump 是否阻塞调度 / load 的排队占比
  echo "m.avg_save_wait_ms    $(mavg_ms ucm:save_completion_wait_duration "$f")"
  echo "m.avg_load_queue_ms   $(mavg_ms ucm:posix_load_queue_wait_duration_ms "$f")"
  echo "m.avg_dump_queue_ms   $(mavg_ms ucm:posix_dump_queue_wait_duration_ms "$f")"
  echo "m.store_used  $(mmax ucm:posix_store_used_bytes "$f")"
  echo "m.store_cap   $(mmax ucm:posix_store_capacity_bytes "$f")"
  echo "m.gc_running  $(mmax ucm:posix_gc_running "$f")"
  echo "m.health      $(mmax ucm:posix_store_health "$f")"
  # /proc 层
  local p
  for p in "${PIDS[@]:-}"; do
    [[ -z "$p" ]] && continue
    local io
    io=$(cat "/proc/$p/io" 2>/dev/null) || continue
    echo "p.$p.wchar    $(awk '/^wchar:/{print $2}' <<<"$io")"
    echo "p.$p.syscw    $(awk '/^syscw:/{print $2}' <<<"$io")"
    echo "p.$p.rchar    $(awk '/^rchar:/{print $2}' <<<"$io")"
    echo "p.$p.syscr    $(awk '/^syscr:/{print $2}' <<<"$io")"
  done
  # 文件层
  local fc=0 fb=0
  if [[ -d "$CACHE_DIR" ]]; then
    fc=$(find "$CACHE_DIR" -type f 2>/dev/null | wc -l)
    fb=$(find "$CACHE_DIR" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{printf "%.0f", s+0}')
  fi
  echo "f.count $fc"
  echo "f.bytes $fb"
}

# ---------------- 进程发现（master → EngineCore → workers） ----------------
declare -a PIDS=()
HIDDEN_PROCESS_COUNT=0
discover_pids() {
  local master="" p cl
  MASTER_PID=""
  PIDS=()
  HIDDEN_PROCESS_COUNT=0
  [[ "$HOST" == "127.0.0.1" || "$HOST" == "localhost" ]] || return 0
  for p in $(pgrep -f "vllm serve" 2>/dev/null); do
    [[ "$(ps -p "$p" -o comm= 2>/dev/null)" == "vllm" ]] || continue
    cl=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null) || continue
    if [[ "$cl" == *"--port $PORT"* ]]; then master=$p; break; fi
  done
  [[ -z "$master" ]] && return 0
  # Traverse helpers too: a relevant worker can be below an intermediary process.
  local -a queue=("$master")
  local depth=0
  local comm
  while ((${#queue[@]} > 0 && depth < 4)); do
    local -a next=()
    for p in "${queue[@]}"; do
      local c
      while read -r c; do
        [[ -z "$c" ]] && continue
        next+=("$c")
        comm=$(cat "/proc/$c/comm" 2>/dev/null) || continue
        if [[ "${STATS_ALL_PROCESSES:-0}" == "1" || "$comm" == VLLM::EngineCor* || "$comm" == VLLM::Worker* ]]; then
          PIDS+=("$c")
        else
          ((HIDDEN_PROCESS_COUNT++))
        fi
      done < <(ps -o pid= --ppid "$p" 2>/dev/null)
    done
    queue=("${next[@]:-}")
    ((depth++))
  done
  MASTER_PID="$master"
}

# ---------------- 快照输出 ----------------
snapshot() {
  echo "════════════ UCM Store Stats @ $(date '+%F %T') ════════════"
  echo "  server: $HOST:$PORT   cache_dir: $CACHE_DIR"
  echo "  Posix engine (configured): $POSIX_IO_ENGINE | config: ${UCM_CONFIG:-unknown}"
  echo
  if fetch_metrics; then
    local v
    declare -A V=()
    while read -r k v; do V[$k]="$v"; done < <(collect)

    echo "── [逻辑层] metrics（累计值）"
    python "$(dirname "${BASH_SOURCE[0]}")/scripts/ucm_metric_summary.py" "$METRICS"
    echo "  Posix 写(host→SSD) : $(fmt_bytes "${V[m.h2s]}")    读(SSD→host): $(fmt_bytes "${V[m.s2h]}")"
    echo "    写 per-rank: $(mrank ucm:posix_h2s_bytes_total "$METRICS" | cut -c1-200)"
    echo "    读 per-rank: $(mrank ucm:posix_s2h_bytes_total "$METRICS" | cut -c1-200)"
    echo "  dump blocks: $(fmt_cnt "${V[m.dump_shard]}")（$(fmt_bytes "${V[m.dump_bytes]}")）   cache 层 load: $(fmt_bytes "${V[m.load_bytes]}")"
    echo "  lookup: $(fmt_cnt "${V[m.lookup_q]}") 查询 / $(fmt_cnt "${V[m.lookup_h]}") 命中（$(awk -v h="${V[m.lookup_h]}" -v q="${V[m.lookup_q]}" 'BEGIN{if(q>0)printf "%.1f%%",h/q*100;else print "-"}')）"
    echo "  前缀命中: HBM $(awk -v h="${V[m.hbm_tok]}" -v q="${V[m.qtok]}" 'BEGIN{if(q>0)printf "%.1f%%",h/q*100;else print "-"}') / UCM $(awk -v u="${V[m.ucm_tok]}" -v q="${V[m.qtok]}" 'BEGIN{if(q>0)printf "%.1f%%",u/q*100;else print "-"}')  （query tokens $(fmt_cnt "${V[m.qtok]}")）"
    echo "  任务平均: dump ${V[m.avg_dump_ms]:--}ms @ ${V[m.avg_h2s_bw]:--}GB/s   load ${V[m.avg_load_ms]:--}ms @ ${V[m.avg_s2h_bw]:--}GB/s"
    echo "  （带宽为逐 task 有效带宽的算术平均，非设备聚合吞吐；task 时延含排队，不含H2D/D2H）"
    if [[ "$POSIX_IO_ENGINE" == "aio" ]]; then
      echo "  AIO Dump 完成可早于异步 rename；task 完成不保证最终文件已可查询，也不代表 fsync 持久化"
    fi
	echo "  等待指标: save完成确认 ${V[m.avg_save_wait_ms]:--}ms（- 表示无样本；较小不证明 dump 无反压）   load排队 ${V[m.avg_load_queue_ms]:--}ms / dump排队 ${V[m.avg_dump_queue_ms]:--}ms"
	echo "  Cache层load: 端到端 ${V[m.cache_load_ms]:--}ms | per-shard 存储等待 ${V[m.bkwait_ms]:--}ms + H2D提交 ${V[m.h2d_submit_ms]:--}ms + H2D收尾 ${V[m.h2d_sync_ms]:--}ms"
	echo "  Cache层dump: 端到端 ${V[m.cache_dump_ms]:--}ms | mkbuf+D2H提交 ${V[m.mkbuf_ms]:--}ms | D2H同步 ${V[m.d2h_ms]:--}ms（含等计算事件 ${V[m.prereq_ms]:--}ms）"
	echo "  （以上指标样本粒度不同且存在重叠，不可相加/相减；backend wait 是剩余就绪等待，H2D sync 是流收尾等待，不能单独判定瓶颈）"
    if [[ "${V[m.store_cap]:-0}" -gt 0 ]]; then
      echo "  store 容量: $(fmt_bytes "${V[m.store_used]}") / $(fmt_bytes "${V[m.store_cap]}")（$(awk -v u="${V[m.store_used]}" -v c="${V[m.store_cap]}" 'BEGIN{if(c>0)printf "%.1f%%",u/c*100;else print "-"}')）   GC: $( [[ "${V[m.gc_running]}" == "1" ]] && echo 运行中 || echo 空闲 )   健康: $( [[ "${V[m.health]}" == "1" ]] && echo OK || echo 熔断/未知 )"
    fi
    echo
  else
    echo "── [逻辑层] ✗ $URL 不可达或无 ucm:* 指标（服务未起/未发过请求）"
    echo
  fi

  if [[ "${STATS_ALL_PROCESSES:-0}" == "1" ]]; then
    echo "── [syscall层] /proc/<pid>/io（完整子进程树，按写入排序）"
  else
    echo "── [syscall层] /proc/<pid>/io（仅 EngineCore/Worker，按写入排序）"
    echo "  已隐藏辅助进程 $HIDDEN_PROCESS_COUNT 个；STATS_ALL_PROCESSES=1 可显示全部"
  fi
  if [[ "$POSIX_IO_ENGINE" == "aio" ]]; then
    echo "  AIO: /proc rchar/wchar/syscr/syscw 不完整覆盖 libaio 数据 IO；表中仍有权重/日志/IPC等流量"
    echo "    后端数据量看 metrics，io_submit 请求看 probe；不可凭此表推断 dump worker 或读写吞吐"
  fi
  if [[ ${#PIDS[@]} -gt 0 ]]; then
    printf "  %-10s %-18s %-12s %-18s %-12s %s\n" PID 进程 write字节 write次数 read字节 read次数
    local p io comm wc sc rc rcr mark
    while read -r p wc sc rc rcr; do
      [[ -z "$p" ]] && continue
      comm=$(cat "/proc/$p/comm" 2>/dev/null || echo '?')
      mark=""
      # 标记 dump worker（wchar 显著大于其它进程）
      [[ "$POSIX_IO_ENGINE" == "psync" && "$comm" == VLLM::Worker* && "$wc" -gt 1073741824 ]] && mark="  ← possible dump worker"
      printf "  %-10s %-18s %-18s %-12s %-18s %-12s%s\n" "$p" "${comm:0:17}" "$(fmt_bytes "$wc")" "$(fmt_cnt "$sc")" "$(fmt_bytes "$rc")" "$(fmt_cnt "$rcr")" "$mark"
    done < <(for p in "${PIDS[@]}"; do
        io=$(cat "/proc/$p/io" 2>/dev/null) || continue
        awk -v p="$p" '/^wchar:/{w=$2}/^syscw:/{sw=$2}/^rchar:/{r=$2}/^syscr:/{sr=$2}END{print p, w+0, sw+0, r+0, sr+0}' <<<"$io"
      done | sort -k2 -rn)
    echo "  （进程启动以来的逻辑 syscall 累计值，含权重加载/IPC/日志，≠ KV 或物理磁盘 IO；文件写用 probe 模式测）"
    echo
  else
    echo "  ✗ 未找到 vllm serve 进程"
    echo
  fi

  echo "── [文件层] $CACHE_DIR"
  if [[ -d "$CACHE_DIR" ]]; then
    local fc fb fs
    fc=$(find "$CACHE_DIR" -type f 2>/dev/null | wc -l)
    fb=$(find "$CACHE_DIR" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{printf "%.0f", s+0}')
    echo "  唯一文件: $(fmt_cnt "$fc") 个 / $(fmt_bytes "$fb")"
    echo "  文件大小分布（前3）:"
    find "$CACHE_DIR" -type f -printf '%s\n' 2>/dev/null | sort -n | uniq -c | sort -rn | awk 'NR<=3' | \
      awk '{printf "    %8d 个 × %s\n", $1, $2}' 
    if fetch_metrics && [[ "$fc" -gt 0 ]]; then
      local ds; ds=$(msum ucm:cache_dump_shards_total "$METRICS")
      awk -v d="$ds" -v c="$fc" 'BEGIN{ if (d>c) printf "  dump %d 次 − 唯一 %d = %d 次覆盖写（重复 block 去重）\n", d, c, d-c }'
    fi
  else
    echo "  ✗ 目录不存在"
  fi
  echo
}

# ---------------- 监视模式 ----------------
watch() {
  echo "监视 $URL + $CACHE_DIR（每 ${INTERVAL}s 采样，Ctrl-C 退出）"
  if ! fetch_metrics; then echo "✗ metrics 不可达，先确认服务在跑" >&2; exit 1; fi
  declare -A PREV=()
  local k v cycle=0 elapsed t0
  local LAST_H2S=0 LAST_S2H=0 LAST_LQ=0 LAST_LH=0 LAST_FC=0 LAST_SW=0 LAST_T=0
  while true; do
    fetch_metrics || true
    while read -r k v; do
      [[ -z "$k" ]] && continue
      PREV[$k]="$v"
    done < <(collect)

    if ((cycle == 0)); then
      LAST_H2S="${PREV[m.h2s]:-0}"; LAST_S2H="${PREV[m.s2h]:-0}"
      LAST_LQ="${PREV[m.lookup_q]:-0}"; LAST_LH="${PREV[m.lookup_h]:-0}"
      LAST_FC="${PREV[f.count]:-0}"
      # 基线时锁定 dump worker 的 syscw 基数
      local bp="" bwv=-1
      for p in "${PIDS[@]:-}"; do
        [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == VLLM::Worker* ]] || continue
        w="${PREV[p.$p.wchar]:-0}"
        if (( w > bwv )); then bwv=$w; bp=$p; fi
      done
      [[ -n "$bp" ]] && LAST_SW="${PREV[p.$bp.syscw]:-0}"
      echo "[$(date '+%T')] 基线: 写 $(fmt_bytes "$LAST_H2S") | 读 $(fmt_bytes "$LAST_S2H") | lookup $(fmt_cnt "$LAST_LQ") | 文件 $(fmt_cnt "$LAST_FC")"
    else
      t0=$(date +%s.%N)
      # 定位 dump worker（wchar 最大的进程）
      local toppid="" topw=-1 p w
      for p in "${PIDS[@]:-}"; do
        [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == VLLM::Worker* ]] || continue
        w="${PREV[p.$p.wchar]:-0}"
        if (( w > topw )); then topw=$w; toppid=$p; fi
      done
      local dsw=0 aio_tag=""
      [[ -n "$toppid" ]] && dsw=$(( ${PREV[p.$toppid.syscw]:-0} - LAST_SW ))
      [[ "$POSIX_IO_ENGINE" == "aio" ]] && aio_tag="(AIO数据IO不完整；非文件写次数)"
      [[ "$POSIX_IO_ENGINE" == "unknown" ]] && aio_tag="(引擎未知；非文件写次数)"

      # 重算进程列表（worker 可能新增）并刷新 PREV 中的 /proc 键
      discover_pids
      local dh ds dq dhit df
      dh=$(( ${PREV[m.h2s]:-0} - LAST_H2S ))
      ds=$(( ${PREV[m.s2h]:-0} - LAST_S2H ))
      dq=$(( ${PREV[m.lookup_q]:-0} - LAST_LQ ))
      dhit=$(( ${PREV[m.lookup_h]:-0} - LAST_LH ))
      df=$(( ${PREV[f.count]:-0} - LAST_FC ))
      local dt
      dt=$(awk -v a="$LAST_T" -v b="$(date +%s.%N)" 'BEGIN{d=b-a; if(d<1)d=1; printf "%.0f", d}')
      echo "[$(date '+%T')] Δ${dt}s | 写 +$(fmt_bytes "$dh") ($(fmt_rate "$dh" "$dt")) | 读 +$(fmt_bytes "$ds") | lookup +$(fmt_cnt "$dq")(hit +$(fmt_cnt "$dhit")) | 新文件 +$(fmt_cnt "$df") | write调用 +$(fmt_cnt "$dsw")${aio_tag}(pid ${toppid:-?})"
      LAST_H2S="${PREV[m.h2s]:-0}"; LAST_S2H="${PREV[m.s2h]:-0}"
      LAST_LQ="${PREV[m.lookup_q]:-0}"; LAST_LH="${PREV[m.lookup_h]:-0}"
      LAST_FC="${PREV[f.count]:-0}"
      [[ -n "$toppid" ]] && LAST_SW="${PREV[p.$toppid.syscw]:-0}"
    fi
    # Barrier latencies remain cumulative, unlike the byte deltas above.
    python "$(dirname "${BASH_SOURCE[0]}")/scripts/ucm_metric_summary.py" "$METRICS"
    LAST_T=$(date +%s.%N)
    ((cycle++))
    if [[ "$MAX_CYCLES" -gt 0 && "$cycle" -ge "$MAX_CYCLES" ]]; then
      echo "（已达 -n $MAX_CYCLES 次上限退出；累计全景可再跑一次 snapshot）"
      break
    fi
    sleep "$INTERVAL"
  done
}

# ---------------- strace 探测：文件读写 syscall 尺寸分布 ----------------
# 区分「文件IO」与「后台小写入」——syscw 计数无法区分两者
# psync：pwrite64/pread64 专项；aio：io_submit 的 iocb（IOCB_CMD_PREAD/PWRITE + aio_nbytes）
# + openat/close 计数（两引擎每 shard 都要 open/close，元数据开销可见）
# 注意：strace 输出必须用 -o 落文件（stderr 管道会因缓冲丢行）
probe() {
  command -v strace >/dev/null || { echo "✗ 需要 strace（sudo -E yum --disablerepo=kubernetes install -y strace）" >&2; exit 1; }

  local h2s=0 p
  if fetch_metrics; then h2s=$(msum ucm:posix_h2s_bytes_total "$METRICS"); fi
  # Both engines distribute IO across ranks; trace all actual workers.
  local -a TPIDS=()
    for p in "${PIDS[@]:-}"; do
      [[ -z "$p" ]] && continue
      [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == VLLM::Worker* ]] && TPIDS+=("$p")
    done
  if [[ ${#TPIDS[@]} -eq 0 ]]; then echo "✗ 未找到 vllm 进程" >&2; exit 1; fi

  # probe 窗口前后的 metrics 写入量（syscall 与 metrics 交叉对账）
  local m_before=0 m_after=0
  m_before=$h2s

  local -a SARGS=()
  for p in "${TPIDS[@]}"; do SARGS+=(-p "$p"); done

  local tmpf
  tmpf=$(mktemp /tmp/ucm_strace_XXXXXX.txt)
  local mode_desc="$POSIX_IO_ENGINE (configured); pread/pwrite + io_submit iocb + openat"
  echo "strace 采样 ${PROBE_SEC}s @ pid ${TPIDS[*]}"
  echo "  （模式: ${mode_desc}，共 ${#TPIDS[@]} 个进程）..."
  timeout "$PROBE_SEC" strace -f -e trace=pwrite64,pread64,write,read,io_submit,io_getevents,openat,close \
    "${SARGS[@]}" -o "$tmpf" 2>/dev/null

  # 窗口结束后的 metrics 写入量（syscall 与 metrics 交叉对账）
  if fetch_metrics; then m_after=$(msum ucm:posix_h2s_bytes_total "$METRICS"); fi

  awk -v t="$PROBE_SEC" -v mb="$m_before" -v ma="$m_after" '
    /pwrite64/ {
      if (match($0, /, ([0-9]+), -?[0-9]+(\) = -?[0-9]+| <unfinished)/, m)) {
        n_pw++; sz_pw += m[1]
        if (m[1] >= 3*1024*1024) b4++
        else if (m[1] >= 1024*1024) b1++
        else if (m[1] >= 64*1024) b64k++
        else bsmall++
      }
      next
    }
    /pread64/ {
      if (match($0, /, ([0-9]+), -?[0-9]+(\) = -?[0-9]+| <unfinished)/, m)) {
        n_pr++; sz_pr += m[1]
        if (m[1] >= 3*1024*1024) r4++
        else if (m[1] >= 1024*1024) r1++
        else if (m[1] >= 64*1024) r64k++
        else rsmall++
      }
      next
    }
    /io_submit/ {
      n_submit++
      rest = $0
      while (match(rest, /IOCB_CMD_PWRITE[^}]*aio_nbytes=([0-9]+)/, m)) {
        n_aw++; sz_aw += m[1]
        if (m[1] >= 3*1024*1024) ab4++
        else if (m[1] >= 1024*1024) ab1++
        else if (m[1] >= 64*1024) ab64k++
        else absmall++
        rest = substr(rest, RSTART + RLENGTH)
      }
      rest = $0
      while (match(rest, /IOCB_CMD_PREAD[^}]*aio_nbytes=([0-9]+)/, m)) {
        n_ar++; sz_ar += m[1]
        if (m[1] >= 3*1024*1024) ar4++
        else if (m[1] >= 1024*1024) ar1++
        else if (m[1] >= 64*1024) ar64k++
        else arsmall++
        rest = substr(rest, RSTART + RLENGTH)
      }
      next
    }
    /io_getevents/ { n_getev++; next }
    /openat/  { n_open++; next }
    /close/   { n_close++; next }
    /write\(/ { n_w++; next }
    /read\(/  { n_r++; next }
    END {
      if (n_pw > 0) {
        printf "  ── 文件写 pwrite64: %d 次, 共 %.2f GB（平均 %s/次, %.1f 次/s）\n", n_pw, \
          sz_pw/1e9, (n_pw>0 ? sprintf("%.2f MB", sz_pw/n_pw/1e6) : "-"), n_pw/t
        printf "     尺寸分布: ≥3MB:%d  1-3MB:%d  64KB-1MB:%d  <64KB:%d\n", b4, b1, b64k, bsmall
      }
      if (n_pr > 0) {
        printf "  ── 文件读 pread64: %d 次, 共 %.2f GB（平均 %s/次, %.1f 次/s）\n", n_pr, \
          sz_pr/1e9, (n_pr>0 ? sprintf("%.2f MB", sz_pr/n_pr/1e6) : "-"), n_pr/t
        printf "     尺寸分布: ≥3MB:%d  1-3MB:%d  64KB-1MB:%d  <64KB:%d\n", r4, r1, r64k, rsmall
      }
      if (n_aw + n_ar > 0) {
        printf "  ── aio 文件写 iocb(PWRITE): %d 个, 共 %.2f GB（%.1f 个/s）\n", n_aw, sz_aw/1e9, n_aw/t
        printf "     尺寸分布: ≥3MB:%d  1-3MB:%d  64KB-1MB:%d  <64KB:%d\n", ab4, ab1, ab64k, absmall
        printf "  ── aio 文件读 iocb(PREAD): %d 个, 共 %.2f GB（%.1f 个/s）\n", n_ar, sz_ar/1e9, n_ar/t
        printf "     尺寸分布: ≥3MB:%d  1-3MB:%d  64KB-1MB:%d  <64KB:%d\n", ar4, ar1, ar64k, arsmall
        printf "  ── aio 调用: io_submit %d 次（%.1f 次/s）, io_getevents %d 次\n", n_submit, n_submit/t, n_getev
      }
      if (n_open > 0) {
         printf "  ── 元数据: openat %d 次, close %d 次（open %.1f 次/s；含采样期间非UCM文件操作）\n", \
          n_open, n_close, n_open/t
      }
      printf "  ── 后台读写(非文件, 多为 8B IPC): write %d 次, read %d 次\n", n_w, n_r
      if (mb+0 > 0 && ma+0 > 0)
        printf "  ── 对账: metrics 写入增量 %.2f GB vs strace 文件写 %.2f GB\n", (ma-mb)/1e9, (sz_pw+sz_aw)/1e9
    }' "$tmpf"
  rm -f "$tmpf"
}

# ---------------- 主流程 ----------------
discover_pids
STATS_HELPER="$(dirname "$(readlink -f "$0")")/scripts/ucm_spdk_stats.py"
UCM_CONFIG="${UCM_CONFIG:-}"
if [[ -z "$UCM_CONFIG" && -n "$MASTER_PID" ]]; then
  UCM_CONFIG=$(python3 "$STATS_HELPER" --pid "$MASTER_PID" --config-path) || exit 1
fi
if [[ -z "$UCM_CONFIG" && ( "$HOST" == "127.0.0.1" || "$HOST" == "localhost" ) ]]; then
  UCM_CONFIG="$HOME/ucm-cache/ucm_config_$(basename "${CACHE_DIR%/}").yaml"
fi
STORE_BACKEND="${STORE_BACKEND:-auto}"
if [[ "$STORE_BACKEND" == "auto" ]]; then
  if [[ -f "$UCM_CONFIG" ]]; then
    STORE_BACKEND=$(python3 "$STATS_HELPER" --config "$UCM_CONFIG" --detect) || exit 1
  else
    echo "WARN: 无法识别后端，按 Posix 显示；SPDK 请设置 STORE_BACKEND=spdk 与 UCM_CONFIG" >&2
    STORE_BACKEND=posix
  fi
fi
case "$STORE_BACKEND" in
  spdk)
    python3 "$STATS_HELPER" --config "$UCM_CONFIG" --host "$HOST" --port "$PORT" \
      --mode "$MODE" --interval "$INTERVAL" --cycles "$MAX_CYCLES" --probe-seconds "$PROBE_SEC"
    exit $? ;;
  posix) ;;
  *) echo "ERROR: STORE_BACKEND must be auto, posix or spdk" >&2; exit 2 ;;
esac
POSIX_IO_ENGINE=unknown
if [[ -f "$UCM_CONFIG" ]]; then
  POSIX_IO_ENGINE=$(python3 "$STATS_HELPER" --config "$UCM_CONFIG" --posix-engine) || POSIX_IO_ENGINE=unknown
fi
echo "Posix engine (configured): $POSIX_IO_ENGINE; startup logs determine the running engine if YAML changed after launch."
case "$MODE" in
  watch)     watch ;;
  probe)     probe ;;
   *)
     cycle=0
     while :; do
       discover_pids
       snapshot
       ((cycle++))
       if (( MAX_CYCLES > 0 && cycle >= MAX_CYCLES )); then break; fi
       sleep "$INTERVAL"
     done ;;
esac
