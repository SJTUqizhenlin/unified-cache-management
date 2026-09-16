pip install openpyxl termplotlib
apt-get install -y gnuplot

curl http://localhost:7780/v1/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "/mnt/model/DeepSeek-V2-Lite-Chat",
    "prompt": "You are a highly specialized assistant whose mission is to faithfully reproduce English literary texts verbatim, without any deviation, paraphrasing, or omission. Your primary responsibility is accuracy: every word, every punctuation mark, and every line must appear exactly as in the original source. Core Principles: Verbatim Reproduction: If the user asks for a passage, you must output the text word-for-word. Do not alter spelling, punctuation, capitalization, or line breaks. Do not paraphrase, summarize, modernize, or \"improve\" the language. Consistency: The same input must always yield the same output. Do not generate alternative versions or interpretations. Clarity of Scope: Your role is not to explain, interpret, or critique. You are not a storyteller or commentator, but a faithful copyist of English literary and cultural texts. Recognizability: Because texts must be reproduced exactly, they will carry their own cultural recognition. You should not add labels, introductions, or explanations before or after the text. Coverage: You must handle passages from classic literature, poetry, speeches, or cultural texts. Regardless of tone—solemn, visionary, poetic, persuasive—you must preserve the original form, structure, and rhythm by reproducing it precisely. Success Criteria: A human reader should be able to compare your output directly with the original and find zero differences. The measure of success is absolute textual fidelity. Your function can be summarized as follows: verbatim reproduction only, no paraphrase, no commentary, no embellishment, no omission. Please reproduce verbatim the opening sentence of the United States Declaration of Independence (1776), starting with \"When in the Course of human events\" and continuing word-for-word without paraphrasing.",
    "max_tokens": 100,
    "temperature": 0
  }'

sleep 3

cd benchmarks

export BENCHMARK_PATH="/vllm-workspace/vllm/benchmarks"

# remove _dataset jsonl files in the Downloads folder to avoid tokenizer mismatch errors
# rm -f /workspace-genet/Downloads/*_dataset.jsonl

python3 ./trace_replay.py --model /mnt/model/DeepSeek-V2-Lite-Chat --backend vllm \
  --trace-path /workspace-genet/Downloads/synthetic_trace_64k.jsonl \
  --trace-mode trace --host 127.0.0.1 --port 7780 --max-concurrency 20 --save-result --save-prompts
