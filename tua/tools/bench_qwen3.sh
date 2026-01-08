#!/usr/bin/env bash
set -euo pipefail

# MODEL_DIR=examples/models/qwen3-4b PROMPT=Hello THREADS=8 MAX_NEW_TOKENS=64 KV_MAX_SEQ=256 REPEAT=3 ./tools/bench_qwen3.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"

MODEL_DIR="${MODEL_DIR:-examples/models/qwen3-4b}"
PROMPT="${PROMPT:-Hello}"
THREADS="${THREADS:-8}"
MAX_NEW_TOKENS="${MAX_NEW_TOKENS:-64}"
KV_MAX_SEQ="${KV_MAX_SEQ:-256}"
REPEAT="${REPEAT:-1}"

FLAGS_BASE=(--raw --no-thinking --greedy --threads "$THREADS" --max-new-tokens "$MAX_NEW_TOKENS" --kv-max-seq "$KV_MAX_SEQ")

run_case() {
  local name="$1"; shift
  local -a extra=("$@")
  local i
  for ((i=1;i<=REPEAT;i++)); do
    local out
    out="$(./bin/tuac examples/llm_run_qwen3.tua "$MODEL_DIR" "$PROMPT" "${FLAGS_BASE[@]}" "${extra[@]}")"
    local toks
    toks="$(echo "$out" | rg -n "tok/s=" | tail -n 1 | awk -F 'tok/s=' '{print $2}' | awk '{print $1}')"
    echo "$name\t$toks"
  done
}

echo -e "case\ttok/s"
run_case "w=f32 kv=f32" --w-f32 --kv-f32
run_case "w=f32 kv=bf16" --w-f32 --kv-bf16
run_case "w=bf16 kv=f32" --w-bf16 --kv-f32
run_case "w=bf16 kv=bf16" --w-bf16 --kv-bf16
