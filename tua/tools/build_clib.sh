#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TUA_SRC_DIR="${TUA_SRC_DIR:-"$ROOT/src"}"

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <libname> <src1.c> [src2.c ...]" >&2
  echo "Env: TUA_SRC_DIR=/path/to/tua/src  CC=clang  CFLAGS='...'" >&2
  exit 2
fi

NAME="$1"
shift

OUT_DIR="${OUT_DIR:-"$ROOT/build/clib"}"
OBJ_DIR="$OUT_DIR/obj_$NAME"
LIB_PATH="$OUT_DIR/lib${NAME}.a"

mkdir -p "$OUT_DIR"
rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"

CC="${CC:-clang}"
CFLAGS_DEFAULT="-O3 -fPIC -I\"$TUA_SRC_DIR\""
CFLAGS_ALL="${CFLAGS:-$CFLAGS_DEFAULT}"

objs=()
for src in "$@"; do
  if [[ ! -f "$src" ]]; then
    echo "error: missing source file: $src" >&2
    exit 2
  fi
  base="$(basename "$src")"
  obj="$OBJ_DIR/${base%.*}.o"
  # shellcheck disable=SC2086
  eval "$CC $CFLAGS_ALL -c \"\$src\" -o \"\$obj\""
  objs+=("$obj")
done

ar rcs "$LIB_PATH" "${objs[@]}"
echo "$LIB_PATH"

