#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${TUA_SRC_DIR:-}" ]]; then
  if [[ -x "$ROOT/bin/tuac" ]]; then
    TUA_SRC_DIR="$("$ROOT/bin/tuac" --print-ffi-include-dir 2>/dev/null || true)"
  fi
  if [[ -z "${TUA_SRC_DIR:-}" ]]; then
    TUA_SRC_DIR="$ROOT/src"
  fi
fi

LLAMA_PREFIX="${LLAMA_PREFIX:-}"
if [[ -z "$LLAMA_PREFIX" ]]; then
  if [[ -f "/usr/local/opt/llama.cpp/include/llama.h" ]]; then
    LLAMA_PREFIX="/usr/local/opt/llama.cpp"
  elif [[ -f "/opt/homebrew/opt/llama.cpp/include/llama.h" ]]; then
    LLAMA_PREFIX="/opt/homebrew/opt/llama.cpp"
  else
    echo "error: cannot find llama.cpp (set LLAMA_PREFIX=/path/to/llama.cpp prefix)" >&2
    exit 2
  fi
fi

export CFLAGS="-O3 -fPIC -I\"$TUA_SRC_DIR\" -I\"$LLAMA_PREFIX/include\""

exec "$ROOT/tools/build_clib.sh" tuaextllama "$ROOT/examples/ffi_tuaext_llama_simple.c"
