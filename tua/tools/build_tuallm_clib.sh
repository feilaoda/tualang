#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

exec ./tools/build_clib.sh tuallm \
  packages/clib/llm/tua_llm.c \
  packages/clib/llm/tua_llm_q4.c

