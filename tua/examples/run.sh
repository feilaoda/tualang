#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <example.tua|path/to/file.tua> [args...]" >&2
  echo "Env: TUAC_FLAGS='--llvm-O3 --perf' (optional)" >&2
  exit 2
fi

src="$1"
shift

if [[ ! -f "$src" ]]; then
  if [[ -f "examples/$src" ]]; then
    src="examples/$src"
  fi
fi

if [[ ! -f "$src" ]]; then
  echo "Error: file not found: $src" >&2
  exit 2
fi

make tuac >/dev/null

extra=()
if [[ -n "${TUAC_FLAGS:-}" ]]; then
  # shellcheck disable=SC2206
  extra=($TUAC_FLAGS)
fi

exec ./bin/tuac ${extra[@]+"${extra[@]}"} "$src" "$@"
