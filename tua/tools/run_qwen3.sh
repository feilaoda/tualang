#!/usr/bin/env bash
set -euo pipefail

# ./tools/run_qwen3.sh [modelDir] [prompt] [其它参数...]

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"

MODEL_DIR="${1:-examples/models/qwen3-0.6b}"
PROMPT="${2:-Hello}"
shift $(( $# >= 1 ? 1 : 0 )) || true
shift $(( $# >= 1 ? 1 : 0 )) || true

exec ./bin/tuac packages/llm/examples/llm_run_qwen3.tua "$MODEL_DIR" "$PROMPT" "$@"
