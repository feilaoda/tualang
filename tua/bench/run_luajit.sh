#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/bench/luajit/bench.lua"

LUAJIT_BIN="${LUAJIT_BIN:-}"
ROUNDS=1
USE_O3=1

usage() {
  cat <<EOF
Usage: bench/run_luajit.sh [options]

Options:
  --luajit <path>   LuaJIT binary path (or use LUAJIT_BIN env)
  --rounds <n>      Number of rounds (default: 1)
  --no-o3           Do not pass -O3 to LuaJIT
  -h, --help        Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --luajit)
      LUAJIT_BIN="$2"
      shift 2
      ;;
    --rounds)
      ROUNDS="$2"
      shift 2
      ;;
    --no-o3)
      USE_O3=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$LUAJIT_BIN" ]]; then
  if command -v luajit >/dev/null 2>&1; then
    LUAJIT_BIN="$(command -v luajit)"
  else
    echo "LuaJIT binary not found. Pass --luajit <path> or set LUAJIT_BIN." >&2
    exit 1
  fi
fi

if [[ ! -x "$LUAJIT_BIN" ]]; then
  echo "LuaJIT binary is not executable: $LUAJIT_BIN" >&2
  exit 1
fi

if ! [[ "$ROUNDS" =~ ^[0-9]+$ ]] || [[ "$ROUNDS" -lt 1 ]]; then
  echo "--rounds must be a positive integer" >&2
  exit 1
fi

cmd=("$LUAJIT_BIN")
if [[ "$USE_O3" -eq 1 ]]; then
  cmd+=("-O3")
fi
cmd+=("$SCRIPT")

for ((i = 1; i <= ROUNDS; i++)); do
  echo "== LuaJIT round ${i}/${ROUNDS} =="
  (cd "$ROOT" && "${cmd[@]}")
done
