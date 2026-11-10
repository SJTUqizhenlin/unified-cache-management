#!/usr/bin/env bash
# client_test_pod.sh — client_test.sh 的 Pod 适配版（保留 client_test_host.sh 的测试目标）
#
# 与 client_test_host.sh 的差异（仅环境适配套路不同）：
#   1. 环境加载：Pod 镜像已预置 CANN/vLLM；没有 ~/ucm-serve-env-023.sh 时直接复用当前环境
#   2. trace 自动选址：优先 syfi_coding_small_64k_replay.jsonl（Pod 共享目录，
#      272 请求 / 271s，最大 input+output 56872 token，可全量回放）；
#      否则宿主机用 ~/downloads/traces/syfi_coding_large_64k_replay.jsonl，
#      Pod 再退回 /workspace-genet/Downloads/synthetic_trace.jsonl；
#      synthetic_trace 含 >64k 请求时默认切前 REPLAY_WINDOW_SEC=30 秒（108 请求）；
#      切片与回放缓存隔离在 $TRACE_DIR/work/，不动源 trace；超长请求校验失败时退出，不自动过滤
#   3. 端口检查：Pod 常无 iproute2/net-tools，port_in_use() 自动降级到 /proc/net/tcp
#   4. 其余与 host 版一致：冒烟测试发两次、健康检查、trace 回放、prompts 缓存复用、
#      --stop-server 有界清理
#
# 用法（先在另一个终端起服务：./server_test_pod.sh）：
#   ./client_test_pod.sh [--smoke-only] [--replay-only] [--stop-server]
#   --stop-server：测试流程走完后自动停止本机对应端口的 server（SIGINT 优雅关闭
#                 + 有界清理本轮子进程，不清理其它实例的孤儿进程/共享内存）；trace_replay 异常退出也会停
#                 （结果是否采信看终端输出的 Benchmark Result 判定）
#
# 环境变量覆盖（例如测 plus 实例 / 调整回放规模）：
#   PORT=7880 ./client_test_pod.sh
#   REPLAY_WINDOW_SEC=300 ./client_test_pod.sh --replay-only    # 前 5 分钟窗口
#   REPLAY_WINDOW_SEC=0 ./client_test_pod.sh --replay-only      # 全量回放
#   TRACE_FILE=/workspace-genet/Downloads/syfi_coding_small_64k_replay.jsonl ./client_test_pod.sh
#   MAX_INPUT_LEN=4096 ./client_test_pod.sh --replay-only
#   MAX_INPUT_LEN=0 means no extra input cap; the server context limit still applies.

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

# ---------------- 可调参数 ----------------
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-7780}"
# MODEL="${MODEL:-/mnt/model/DeepSeek-V2-Lite-Chat}"
MODEL="${MODEL:-/mnt/model/DeepSeek-V4-Flash-w8a8-mtp}"
REPO="${REPO:-$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")}"
# trace 目录：宿主机优先 ~/downloads/traces；pod 里共享目录通常是 /workspace-genet/Downloads
if [[ -z "${TRACE_DIR:-}" ]]; then
  if [[ -d "$HOME/downloads/traces" ]]; then
    TRACE_DIR="$HOME/downloads/traces"
  elif [[ -d /workspace-genet/Downloads ]]; then
    TRACE_DIR=/workspace-genet/Downloads
  else
    TRACE_DIR="$HOME/downloads/traces"
  fi
fi
# trace 文件：优先 small 64k 回放集；否则沿用宿主机 large trace；
# Pod 里再退回共享目录的 Mooncake synthetic trace。
if [[ -z "${TRACE_FILE:-}" ]]; then
  if [[ -f "$TRACE_DIR/hot_prefix_8k_128_s14.jsonl.jsonl" ]]; then
    TRACE_FILE="$TRACE_DIR/hot_prefix_8k_128_s14.jsonl.jsonl"
  elif [[ -f "$TRACE_DIR/syfi_coding_large_64k_replay.jsonl" ]]; then
    TRACE_FILE="$TRACE_DIR/syfi_coding_large_64k_replay.jsonl"  # 自选agentic trace
  elif [[ -f "$TRACE_DIR/synthetic_trace.jsonl" ]]; then
    TRACE_FILE="$TRACE_DIR/synthetic_trace.jsonl"
  else
    TRACE_FILE="$TRACE_DIR/syfi_coding_large_64k_replay.jsonl"
  fi
fi
MAX_CONCURRENCY="${MAX_CONCURRENCY:-1}"
MAX_INPUT_LEN="${MAX_INPUT_LEN:-0}"  # Token cap for trace inputs; 0 follows server limits only.
# REPLAY_WINDOW_SEC=0 表示全量回放。
# pod 共享的 synthetic_trace.jsonl 含 >64k 的请求，默认取前 30s 窗口（108 请求）以通过服务端长度校验；
# 宿主机默认 trace 仍保持全量回放。
if [[ -z "${REPLAY_WINDOW_SEC:-}" ]]; then
  if [[ "$(basename "$TRACE_FILE")" == "synthetic_trace.jsonl" ]]; then
    REPLAY_WINDOW_SEC=30
  else
    REPLAY_WINDOW_SEC=0
  fi
fi
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
# Pod 镜像已预置环境；只有宿主机风格的 env 文件存在时才 source。
if [[ -f "$HOME/ucm-serve-env-023.sh" ]]; then
  source "$HOME/ucm-serve-env-023.sh" || exit 1    # Match the server's CANN / vLLM environment.
elif [[ -z "${ASCEND_TOOLKIT_HOME:-}" ]]; then
  echo "ERROR: 未找到 $HOME/ucm-serve-env-023.sh，且当前环境未预置 CANN（ASCEND_TOOLKIT_HOME 为空）" >&2
  exit 1
fi
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
  local -a identity=()
  if [[ -n "${UCM_HOST_SERVER_PID:-}" || -n "${UCM_HOST_SERVER_START_TIME:-}" ]]; then
    identity+=(--pid "${UCM_HOST_SERVER_PID:-}" --start-time "${UCM_HOST_SERVER_START_TIME:-}")
    if [[ -n "${UCM_HOST_SERVER_TOKEN:-}" ]]; then
      identity+=(--token "$UCM_HOST_SERVER_TOKEN")
    fi
  fi
  # Standalone use discovers one exact owned vllm/port; chain use never falls
  # back to another server if the supplied PID/starttime no longer matches.
  echo "==> 停止本轮 server（SIGINT，必要时有界 TERM/KILL 清理）"
  python "$REPO/scripts/host_server_process.py" stop --port "$PORT" "${identity[@]}"

  if port_in_use "$PORT"; then
    echo "    WARN: 端口 $PORT 仍被占用，请手动检查（ss/netstat -tlnp | grep $PORT）"
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
MODELS_JSON=$(curl -fsS --max-time 10 "http://$HOST:$PORT/v1/models")
SERVER_MAX_MODEL_LEN=$(jq -er --arg model "$MODEL" \
  '.data[] | select(.id == $model) | .max_model_len | select(type == "number" and . > 0)' <<< "$MODELS_JSON") || {
  echo "ERROR: 无法获取模型 $MODEL 的服务端长度上限，请检查 /v1/models" >&2
  exit 1
}
echo "    served model: $MODEL | max_model_len=$SERVER_MAX_MODEL_LEN"
if [[ ! "$MAX_INPUT_LEN" =~ ^(0|[1-9][0-9]*)$ ]]; then
  echo "ERROR: MAX_INPUT_LEN 必须为非负整数（0 表示不额外限制输入长度）" >&2
  exit 1
fi
echo "    trace input cap: MAX_INPUT_LEN=$MAX_INPUT_LEN (0 = server limit only)"

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
  else
    echo "    源 trace: $TRACE_FILE ($(wc -l < "$TRACE_FILE") 行)"
  fi

  # Select only an explicitly requested time window; validate lengths without dropping requests.
  WORK_DIR="$TRACE_DIR/work"
  mkdir -p "$WORK_DIR"
  REPLAY_TRACE="$WORK_DIR/$(basename "$TRACE_FILE" .jsonl)_w${REPLAY_WINDOW_SEC}s.jsonl"
  python - "$TRACE_FILE" "$REPLAY_TRACE" "$REPLAY_WINDOW_SEC" "$SERVER_MAX_MODEL_LEN" "$MAX_INPUT_LEN" <<'PY'
import json, sys

src, dst = sys.argv[1:3]
win, max_len, max_input_len = map(int, sys.argv[3:6])
if win < 0:
    raise SystemExit("REPLAY_WINDOW_SEC must be nonnegative")
reqs = [json.loads(l) for l in open(src)]
if not reqs:
    raise SystemExit("ERROR: trace is empty")
t0 = min(r["timestamp"] for r in reqs)
kept = [r for r in reqs if not win or r["timestamp"] - t0 <= win * 1000]
if not kept:
    raise SystemExit("ERROR: requested time window is empty")
input_exceeded = sum(bool(max_input_len and r["input_length"] > max_input_len) for r in kept)
# Leave a small margin for special tokens and text re-tokenization.
total_exceeded = sum(r["input_length"] + r["output_length"] > max_len - 32 for r in kept)
if input_exceeded or total_exceeded:
    largest_input = max(r["input_length"] for r in kept)
    largest_total = max(r["input_length"] + r["output_length"] for r in kept)
    raise SystemExit(
        f"ERROR: trace length validation failed; no requests were filtered or truncated. "
        f"input violations={input_exceeded} (MAX_INPUT_LEN={max_input_len}, 0=unlimited), "
        f"context violations={total_exceeded} (server={max_len}, margin=32). "
        f"Maximum input={largest_input}, input+output={largest_total}. "
        "Increase the limits or explicitly choose another trace."
    )
with open(dst, "w") as f:
    for r in kept:
        f.write(json.dumps(r) + "\n")
tot = sum(r["input_length"] for r in kept)
ids = [h for r in kept for h in r["hash_ids"]]
uniq = len(set(ids))
reuse = (len(ids) - uniq) / len(ids) * 100 if ids else 0.0
print(f"    长度校验通过: {len(kept)} 请求（时间窗外 {len(reqs) - len(kept)}，未按长度过滤）"
      f"总输入 {tot/1e6:.1f}M tok | 窗口内块复用率 {reuse:.1f}% | 上限 {max_len}")
PY

  # prompts 缓存复用（trace_replay: <trace名>_dataset.jsonl 存在则直接加载，跳过 prompt 生成）
  # 策略：同模型 + 同 trace 内容 → 复用；换模型（tokenizer 变）或 trace 内容变更 → 清理重建
  #       复用条件 = dataset 完整落盘（生成在回放前完成，与压测成败无关；
  #       trace_replay 原子写 + 此处行数校验，防止中断遗留的半截/陈旧缓存被误用）
  #       强制全清：FORCE_CLEAN=1
  RDIR=$(dirname "$REPLAY_TRACE")
  DATASET="$RDIR/$(basename "$REPLAY_TRACE" | cut -d. -f1)_dataset.jsonl"
  MARKER="${DATASET}.cache_marker"
  CUR_KEY="$MODEL|$(basename "$REPLAY_TRACE")|$(md5sum "$REPLAY_TRACE" 2>/dev/null | cut -d' ' -f1)"
  if [[ "${FORCE_CLEAN:-0}" == "1" ]]; then
    rm -f "$DATASET" "$MARKER"
    echo "    FORCE_CLEAN=1 → 已清理当前 trace 的 prompts 缓存"
  elif [[ -f "$MARKER" && -f "$DATASET" ]] && \
       [[ "$(cat "$MARKER" 2>/dev/null)" == "$CUR_KEY" ]] && \
       [[ "$(wc -l < "$DATASET")" -eq "$(wc -l < "$REPLAY_TRACE")" ]]; then
    echo "    同模型同 trace → 复用 prompts 缓存（$(basename "$REPLAY_TRACE" .jsonl)_dataset.jsonl）"
  else
    rm -f "$DATASET" "$MARKER"
    echo "    当前 dataset 缺失、未验证或模型/trace 变更 → 重新生成 prompts"
  fi
  # Publish identity before replay: atomic dataset generation and row-count checks
  # make completed prompts reusable even if the benchmark is interrupted.
  printf '%s\n' "$CUR_KEY" > "$MARKER"

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

  echo "    验证 cache 效果：对比两个 trace 周期的 TTFT（第二周期应显著下降），"
  echo "    或查看服务端日志 grep 'hit external' ~/ucm-logs/$PORT/ucm-*.log"
fi

# ---------------- 全部测试完成，可选停止 server ----------------
# 注意：仅在成功路径执行（set -e 下若 trace_replay 失败会直接退出，server 保留便于排查）
if [[ "$STOP_SERVER" == "1" ]]; then
  stop_server
fi
