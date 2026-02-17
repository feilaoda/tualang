#!/usr/bin/env bash
set -euo pipefail

# MODEL_DIR=examples/models/qwen3-4b PROMPT="Hello" THREADS=8 MAX_NEW_TOKENS=64 KV_MAX_SEQ=256 ./tools/profile_qwen3.sh
#
# Notes:
# - BNNS (Accelerate) may have large one-time "first token" setup cost on some machines.
#   Override with: TUA_LLM_BNNS=0 ./tools/profile_qwen3.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"
export TUA_LLM_BNNS=0

MODEL_DIR="${MODEL_DIR:-examples/models/qwen3-4b}"
PROMPT="${PROMPT:-Hello}"
THREADS="${THREADS:-8}"
MAX_NEW_TOKENS="${MAX_NEW_TOKENS:-64}"
KV_MAX_SEQ="${KV_MAX_SEQ:-256}"
WEIGHTS="${WEIGHTS:---w-bf16}"

echo "TUA_LLM_BNNS=0 ./bin/tuac --perf packages/llm/examples/llm_run_qwen3.tua $MODEL_DIR $PROMPT --raw --no-thinking --greedy --threads $THREADS --max-new-tokens $MAX_NEW_TOKENS --kv-max-seq $KV_MAX_SEQ --profile $WEIGHTS"

TUA_LLM_BNNS=0 ./bin/tuac --perf packages/llm/examples/llm_run_qwen3.tua "$MODEL_DIR" "$PROMPT" --raw --no-thinking --greedy --threads "$THREADS" --max-new-tokens "$MAX_NEW_TOKENS" --kv-max-seq "$KV_MAX_SEQ" --profile $WEIGHTS
