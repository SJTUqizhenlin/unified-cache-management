#!/usr/bin/env bash
# client_test_host.sh — client_test.sh 的宿主机（裸机）适配版
#
# 与 pod 版 client_test.sh 的差异：
#   1. 环境自动加载（~/ucm-serve-env.sh：CANN + conda vllm-ucm）
#      —— trace_replay.py 会 import vllm/torch_npu，没有 CANN 环境直接失败
#   2. 修复 pod 专属命令：apt-get → yum（openEuler），且绘图依赖改为 best-effort
#   3. 大 JSON payload 落盘为文件再 curl --data @file
#      —— 消除多行命令粘贴被终端拆碎/断行的问题（长 prompt 内嵌引号也不再依赖转义）
#   4. 冒烟测试发两次相同请求（文档推荐的验证方式）：
#      第一次 = cache miss（全量 prefill），第二次 = cache hit（观察 TTFT 差异）
#   5. trace 默认使用 ~/downloads/traces/synthetic_trace_64k.jsonl（Mooncake，原 pod 同款）；
#      因 trace_replay 按原始时间戳 pacing，默认切前 REPLAY_WINDOW_SEC=120 秒
#      （约 425 请求 / 5.4M token），REPLAY_WINDOW_SEC=0 可全量回放（3946 请求 / 1022s 跨度）；
#      切片与回放缓存隔离在 $TRACE_DIR/work/，不动源 trace；超长请求（>64000 tok）自动过滤
#   6. 修正 pod 硬编码路径（BENCHMARK_PATH / /workspace-genet/...）
#      —— vllm 源码安装后 vllm.benchmarks 直接可用，BENCHMARK_PATH 不再需要
#   7. 增加：服务健康检查（/health）、结果摘要提取、set -euo pipefail
#   8. prompts 缓存复用：同模型同 trace 连续测试时不清理 <trace名>_dataset.jsonl
#      （trace_replay 存在即加载、跳过 prompt 生成）；换模型/换 trace 内容才自动清理；
#      生成完成即可复用（原子落盘 + 行数校验，不要求压测跑成功）；强制全清 FORCE_CLEAN=1
#
# 用法（先在另一个终端起服务：./server_test_host.sh）：
#   ./client_test_host.sh [--smoke-only] [--replay-only] [--stop-server]
#   --stop-server：测试流程走完后自动停止本机对应端口的 server（SIGINT 优雅关闭
#                 + 清理崩溃遗留的孤儿进程/泄漏 shm）；trace_replay 异常退出也会停
#                 （结果是否采信看终端输出的 Benchmark Result 判定）
#
# 环境变量覆盖（例如测 plus 实例 / 调整回放规模）：
#   PORT=7880 ./client_test_host.sh
#   REPLAY_WINDOW_SEC=300 ./client_test_host.sh --replay-only    # 前 5 分钟窗口
#   REPLAY_WINDOW_SEC=0 ./client_test_host.sh --replay-only      # 全量回放
#   TRACE_FILE=~/downloads/traces/conversation_trace_64k.jsonl ./client_test_host.sh

# ---------------- 可调参数 ----------------
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-7780}"
MODEL="${MODEL:-/mnt/model/DeepSeek-V2-Lite-Chat}"
REPO="$HOME/devA2/unified-cache-management"
TRACE_DIR="${TRACE_DIR:-$HOME/downloads/traces}"              # Mooncake trace 目录
TRACE_FILE="${TRACE_FILE:-$TRACE_DIR/synthetic_trace_64k.jsonl}"  # 原 pod 脚本同款 trace
MAX_CONCURRENCY="${MAX_CONCURRENCY:-20}"
# REPLAY_WINDOW_SEC=0 表示全量回放
REPLAY_WINDOW_SEC="${REPLAY_WINDOW_SEC:-0}"
# 合成 trace 参数（仅在 TRACE_FILE 不存在时兜底生成）
GEN_REQUESTS="${GEN_REQUESTS:-12}"        # 请求数
GEN_INPUT_LEN="${GEN_INPUT_LEN:-16384}"   # 每请求输入 token 数
GEN_OUTPUT_LEN="${GEN_OUTPUT_LEN:-128}"   # 每请求输出 token 数
GEN_SHARED_BLOCKS="${GEN_SHARED_BLOCKS:-24}"  # 共享前缀块数（×512 token，需 < GEN_INPUT_LEN/512）

SMOKE=1
REPLAY=1
STOP_SERVER="${STOP_SERVER:-0}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --smoke-only)  SMOKE=1; REPLAY=0 ;;
    --replay-only) SMOKE=0; REPLAY=1 ;;
    --stop-server) STOP_SERVER=1 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//' | sed -n '1,30p'
      exit 0 ;;
    *)
      echo "Error: unknown argument '$1' (supported: --smoke-only | --replay-only | --stop-server)" >&2
      exit 2 ;;
  esac
  shift
done

# ---------------- 环境加载 ----------------
source "$HOME/ucm-serve-env.sh"   # CANN(~/ascend) + conda(vllm-ucm)
cd "$REPO"

set -euo pipefail

# ---------------- 依赖（best-effort） ----------------
python -m pip install -q openpyxl termplotlib 2>/dev/null || \
  echo "WARN: openpyxl/termplotlib 安装失败（仅影响 Excel/绘图输出）" >&2
# command -v gnuplot >/dev/null || sudo -n yum install -y gnuplot >/dev/null 2>&1 || \
#   echo "WARN: gnuplot 不可用（仅影响终端绘图）" >&2

# ---------------- 测试完成后停止 server（--stop-server / STOP_SERVER=1） ----------------
stop_server() {
  if [[ "$HOST" != "127.0.0.1" && "$HOST" != "localhost" ]]; then
    echo "  跳过：--stop-server 仅支持停止本机 server（HOST=$HOST）"
    return 0
  fi
  # 按端口找本用户的 vllm serve master
  # （comm=vllm 过滤很关键：pgrep -f "vllm serve" 会误匹配命令行里含该字样的 shell）
  local master="" p cl
  for p in $(pgrep -u "$(id -u)" -f "vllm serve" 2>/dev/null); do
    [[ "$(ps -p "$p" -o comm= 2>/dev/null)" == "vllm" ]] || continue
    cl=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null) || continue
    if [[ "$cl" == *"--port $PORT"* ]]; then master=$p; break; fi
  done
  if [[ -z "$master" ]]; then
    echo "==> 未找到端口 $PORT 对应的本用户 vllm 进程，无需停止"
    return 0
  fi

  echo "==> 停止 server (pid $master, SIGINT 优雅关闭)"
  kill -INT "$master" 2>/dev/null || true
  local i
  for i in $(seq 1 90); do
    [[ -d /proc/$master ]] || break
    sleep 1
  done

  # 未优雅退出 → 强杀整棵进程树（BFS 收集子孙，避免误伤其它实例）
  if [[ -d /proc/$master ]]; then
    echo "    90s 未退出，强制清理进程树"
    local -a queue=("$master") all=("$master")
    while [[ ${#queue[@]} -gt 0 && ${#all[@]} -lt 64 ]]; do
      local -a next=()
      local pid2 c
      for pid2 in "${queue[@]}"; do
        while read -r c; do
          [[ -z "$c" ]] && continue
          all+=("$c"); next+=("$c")
        done < <(ps -o pid= --ppid "$pid2" 2>/dev/null)
      done
      queue=("${next[@]:-}")
    done
    kill -9 "${all[@]}" 2>/dev/null || true
    sleep 2
  fi

  # 残留清理（与 server_test_host.sh 启动自检同款逻辑）：
  # ① 孤儿进程（本用户 + PPID=1 的 Worker/EngineCore，Ctrl+C 崩溃遗留）② 未映射的泄漏 shm
  sleep 3
  local orphans
  orphans=$(ps -eo pid=,ppid=,uid=,comm= 2>/dev/null | awk -v uid="$(id -u)" \
    '$3==uid && $2==1 && $4 ~ /^VLLM::(Worker|EngineCor)/ {print $1}') || orphans=""
  if [[ -n "$orphans" ]]; then
    echo "    清理遗留孤儿进程: $(echo $orphans | tr '\n' ' ')"
    kill $orphans 2>/dev/null || true
    sleep 3
    kill -9 $orphans 2>/dev/null || true
  fi
  local f gout gstat
  for f in /dev/shm/uc_shm_cache*; do
    [[ -e "$f" ]] || continue
    gout=$(timeout 5 grep -lF "$f" /proc/[0-9]*/maps 2>/dev/null) && gstat=0 || gstat=$?
    [[ "$gstat" == "124" ]] && continue
    if [[ -z "$gout" && -O "$f" ]]; then
      rm -f "$f"
      echo "    清理泄漏 shm: $(basename "$f")"
    fi
  done

  if ss -tln 2>/dev/null | grep -q ":$PORT "; then
    echo "    WARN: 端口 $PORT 仍被占用，请手动检查（ss -tlnp | grep $PORT）"
  else
    echo "    ✓ server 已停止（磁盘缓存保留，是否清理由下次启动的 KEEP_CACHE 决定）"
  fi
}

# ---------------- 服务健康检查 ----------------
echo "==> [1/3] 检查服务 http://$HOST:$PORT （/health 与 /v1/models）"
for i in $(seq 1 60); do
  if curl -s --max-time 3 "http://$HOST:$PORT/health" >/dev/null 2>&1; then
    break
  fi
  if [[ $i -eq 60 ]]; then
    echo "ERROR: 服务 $HOST:$PORT 不可用（先在另一个终端运行 ./server_test_host.sh）" >&2
    exit 1
  fi
  sleep 5
done
echo "    served model: $(curl -s "http://$HOST:$PORT/v1/models" | jq -r '.data[0].id' 2>/dev/null || echo '?')"

# ---------------- 冒烟测试：同一请求发两次，观察 cache 命中 ----------------
if [[ "$SMOKE" == "1" ]]; then
  echo "==> [2/3] 冒烟测试：相同请求发两次（#1 = cache miss，#2 = cache hit）"
  echo "    优先 /v1/chat/completions（chat 模型自动套模板，输出正常文本），"
  echo "    失败则退回 /v1/completions（base 模型）"
  PAYLOAD=$(mktemp /tmp/ucm_smoke_XXXXXX.json)
  trap 'rm -f "$PAYLOAD" "${PAYLOAD}.chat"' EXIT
  cat > "$PAYLOAD" <<'EOF'
{
  "model": "__MODEL__",
  "prompt": "You are a highly specialized assistant whose mission is to faithfully reproduce English literary texts verbatim, without any deviation, paraphrasing, or omission. Your primary responsibility is accuracy: every word, every punctuation mark, and every line must appear exactly as in the original source. Core Principles: Verbatim Reproduction: If the user asks for a passage, you must output the text word-for-word. Do not alter spelling, punctuation, capitalization, or line breaks. Do not paraphrase, summarize, modernize, or \"improve\" the language. Consistency: The same input must always yield the same output. Do not generate alternative versions or interpretations. Clarity of Scope: Your role is not to explain, interpret, or critique. You are not a storyteller or commentator, but a faithful copyist of English literary and cultural texts. Recognizability: Because texts must be reproduced exactly, they will carry their own cultural recognition. You should not add labels, introductions, or explanations before or after the text. Coverage: You must handle passages from classic literature, poetry, speeches, or cultural texts. Regardless of tone—solemn, visionary, poetic, persuasive—you must preserve the original form, structure, and rhythm by reproducing it precisely. Success Criteria: A human reader should be able to compare your output directly with the original and find zero differences. The measure of success is absolute textual fidelity. Your function can be summarized as follows: verbatim reproduction only, no paraphrase, no commentary, no embellishment, no omission. Please reproduce verbatim the opening sentence of the United States Declaration of Independence (1776), starting with \"When in the Course of human events\" and continuing word-for-word without paraphrasing.",
  "max_tokens": 100,
  "temperature": 0
}
EOF
  sed -i "s|__MODEL__|$MODEL|" "$PAYLOAD"
  # 用 jq 从 completions payload 派生 chat payload（转义安全，单一数据源）
  jq '{model, messages: [{role: "user", content: .prompt}], max_tokens, temperature}' \
    "$PAYLOAD" > "${PAYLOAD}.chat"

  smoke_request() {  # $1: 请求序号
    echo "    ---- request #$1 (via $ENDPOINT) ----"
    curl -sS --max-time 600 -o /tmp/ucm_smoke_resp.json \
      -w "    HTTP %{http_code} | total %{time_total}s\n" \
      -H "Content-Type: application/json" \
      --data @"$REQ_PAYLOAD" "http://$HOST:$PORT$ENDPOINT"
    # 统一提取：chat 响应取 message.content，completions 响应取 text
    jq -r '"    finish_reason: \(.choices[0].finish_reason) | completion_tokens: \(.usage.completion_tokens // "n/a") | prompt_tokens: \(.usage.prompt_tokens // "n/a") | text: \((.choices[0].message.content // .choices[0].text // "") | .[:120] | if . == "" then "(空)" else . end)"' /tmp/ucm_smoke_resp.json 2>/dev/null ||
      { echo "    响应解析失败，原始内容："; head -c 400 /tmp/ucm_smoke_resp.json; echo; }
  }

  ENDPOINT="/v1/chat/completions"
  REQ_PAYLOAD="${PAYLOAD}.chat"
  smoke_request 1
  if ! grep -q '"choices"' /tmp/ucm_smoke_resp.json 2>/dev/null; then
    echo "    chat 接口不可用（模型无 chat template？），退回 /v1/completions"
    ENDPOINT="/v1/completions"
    REQ_PAYLOAD="$PAYLOAD"
    smoke_request 1
  fi
  sleep 1
  smoke_request 2
  echo "    提示：UCM 命中日志在服务端 ~/ucm-logs/$PORT/ （grep 'hit external'）"
fi

# ---------------- trace 回放压测 ----------------
if [[ "$REPLAY" == "1" ]]; then
  echo "==> [3/3] trace 回放压测"
  mkdir -p "$TRACE_DIR"

  if [[ ! -f "$TRACE_FILE" ]]; then
    echo "    trace 不存在，生成合成 trace: $TRACE_FILE"
    python - "$TRACE_FILE" "$GEN_REQUESTS" "$GEN_INPUT_LEN" "$GEN_OUTPUT_LEN" "$GEN_SHARED_BLOCKS" <<'PY'
import json, sys, time

path, n_req, in_len, out_len, shared = sys.argv[1:6]
n_req, in_len, out_len, shared = int(n_req), int(in_len), int(out_len), int(shared)

blocks = in_len // 512          # 每个 hash_id 对应 512 token
assert shared < blocks, f"共享块数 {shared} 必须 < 总块数 {blocks}"
now_ms = int(time.time() * 1000)

with open(path, "w") as f:
    for i in range(n_req):
        # 前 shared 个 hash_id 全请求一致（共享前缀），其余按请求唯一
        hash_ids = list(range(1000, 1000 + shared)) + \
                   [10_000 + i * 1000 + j for j in range(blocks - shared)]
        f.write(json.dumps({
            "timestamp": now_ms + i * 200,     # 请求间隔 200ms
            "input_length": in_len,
            "output_length": out_len,
            "hash_ids": hash_ids,
        }) + "\n")
print(f"    已生成 {n_req} 个请求：input={in_len}, output={out_len}, "
      f"共享前缀 {shared * 512} token / {shared}/{blocks} 块")
PY
    REPLAY_TRACE="$TRACE_FILE"
  else
    echo "    源 trace: $TRACE_FILE ($(wc -l < "$TRACE_FILE") 行)"
    # 按时间窗切片到 work 目录（重放产物/缓存均隔离在 work 内，不污染源 trace）
    WORK_DIR="$TRACE_DIR/work"
    mkdir -p "$WORK_DIR"
    if [[ "$REPLAY_WINDOW_SEC" == "0" ]]; then
      REPLAY_TRACE="$TRACE_FILE"
      echo "    REPLAY_WINDOW_SEC=0 → 全量回放（注意时间跨度与总量）"
    else
      REPLAY_TRACE="$WORK_DIR/$(basename "$TRACE_FILE" .jsonl)_w${REPLAY_WINDOW_SEC}s.jsonl"
      python - "$TRACE_FILE" "$REPLAY_TRACE" "$REPLAY_WINDOW_SEC" <<'PY'
import json, sys

src, dst, win = sys.argv[1], sys.argv[2], int(sys.argv[3])
reqs = [json.loads(l) for l in open(src)]
t0 = min(r["timestamp"] for r in reqs)
kept, dropped = [], 0
for r in reqs:
    if r["input_length"] + r["output_length"] > 64000:   # 服务端 --max-model-len 65536
        dropped += 1
        continue
    if r["timestamp"] - t0 <= win * 1000:
        kept.append(r)
with open(dst, "w") as f:
    for r in kept:
        f.write(json.dumps(r) + "\n")
tot = sum(r["input_length"] for r in kept)
ids = [h for r in kept for h in r["hash_ids"]]
uniq = len(set(ids))
reuse = (len(ids) - uniq) / len(ids) * 100 if ids else 0.0
print(f"    切片: {len(kept)} 请求（过滤超长 {dropped}）"
      f"总输入 {tot/1e6:.1f}M tok | 窗口内块复用率 {reuse:.1f}%")
PY
    fi
  fi

  # prompts 缓存复用（trace_replay: <trace名>_dataset.jsonl 存在则直接加载，跳过 prompt 生成）
  # 策略：同模型 + 同 trace 内容 → 复用；换模型（tokenizer 变）或 trace 内容变更 → 清理重建
  #       复用条件 = dataset 完整落盘（生成在回放前完成，与压测成败无关；
  #       trace_replay 原子写 + 此处行数校验，防止中断遗留的半截/陈旧缓存被误用）
  #       强制全清：FORCE_CLEAN=1
  RDIR=$(dirname "$REPLAY_TRACE")
  MARKER="$RDIR/.cache_marker"
  CUR_KEY="$MODEL|$(basename "$REPLAY_TRACE")|$(md5sum "$REPLAY_TRACE" 2>/dev/null | cut -d' ' -f1)"
  if [[ "${FORCE_CLEAN:-0}" == "1" ]]; then
    rm -f "$RDIR"/*_dataset.jsonl "$MARKER" 2>/dev/null || true
    echo "    FORCE_CLEAN=1 → 已清理 prompts 缓存"
  elif [[ -f "$MARKER" ]] && [[ "$(cat "$MARKER" 2>/dev/null)" == "$CUR_KEY" ]]; then
    echo "    同模型同 trace → 复用 prompts 缓存（$(basename "$REPLAY_TRACE" .jsonl)_dataset.jsonl）"
  else
    rm -f "$RDIR"/*_dataset.jsonl "$MARKER" 2>/dev/null || true
    echo "    模型/trace 变更 → 已清理旧 prompts 缓存"
  fi

  # trace_replay 用 ExcelWriter mode="a" 追加保存结果，要求 metrics.xlsx 已存在（不存在会崩在最后保存）
  # 兜底：缺失时用 openpyxl 创建一个合法的空工作簿
  if [[ ! -s "$REPO/benchmarks/metrics.xlsx" ]]; then
    echo "    metrics.xlsx 不存在，创建空工作簿（trace_replay 追加模式要求文件先存在）"
    python -c "import openpyxl; openpyxl.Workbook().save('$REPO/benchmarks/metrics.xlsx')"
  fi

  cd "$REPO/benchmarks"
  # 用 if 包裹：压测异常退出不触发 set -e（结果已打印的可自行判断），确保后续 stop_server 仍会执行
  if python ./trace_replay.py \
    --model "$MODEL" \
    --backend vllm \
    --trace-path "$REPLAY_TRACE" \
    --trace-mode trace \
    --host "$HOST" --port "$PORT" \
    --max-concurrency "$MAX_CONCURRENCY" \
    --save-result --save-prompts; then
    echo "    结果文件：$REPO/benchmarks/metrics.xlsx 与切片所在目录（prompts 明细）"
  else
    echo "    WARN: trace_replay 异常退出（benchmark 输出已打印，Excel 可能未保存）" >&2
  fi

  # prompts 缓存与压测成败解耦：生成阶段先于回放、已原子落盘，
  # 只要 dataset 存在且行数与 trace 请求数一致即写复用标记 —— 压测失败下次直接复用
  DATASET="$RDIR/$(basename "$REPLAY_TRACE" | cut -d. -f1)_dataset.jsonl"
  if [[ -f "$DATASET" ]] && \
     [[ "$(wc -l < "$DATASET")" -eq "$(wc -l < "$REPLAY_TRACE")" ]]; then
    echo "$CUR_KEY" > "$MARKER"
  fi
  echo "    验证 cache 效果：对比两个 trace 周期的 TTFT（第二周期应显著下降），"
  echo "    或查看服务端日志 grep 'hit external' ~/ucm-logs/$PORT/ucm-*.log"
fi

# ---------------- 全部测试完成，可选停止 server ----------------
# 注意：仅在成功路径执行（set -e 下若 trace_replay 失败会直接退出，server 保留便于排查）
if [[ "$STOP_SERVER" == "1" ]]; then
  stop_server
fi
