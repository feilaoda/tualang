#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
echo "root: $ROOT"
BENCH_DIR="$ROOT/bench"
BIN_DIR="$BENCH_DIR/bin"

mkdir -p "$BIN_DIR"

export TUA_STDLIB_DIR="$ROOT/std"
export TUA_PACKAGE_DIR="$ROOT/packages"

if [ ! -x "$ROOT/bin/tuac" ]; then
  make -C "$ROOT" tuac
fi

if [[ ! -f "$ROOT/build/clib/libtuaintmap.a" ]]; then
  echo "[build] c lib (tuaintmap)"
  "$ROOT/tools/build_clib.sh" tuaintmap "$ROOT/packages/clib/map/tua_intintmap.c" >/dev/null
fi

echo "[build] tua bench (AOT)"
TUAC_FLAGS=(
  --llvm-O3
  --no-loc
  -L "$ROOT/build/clib"
  -l tuaintmap
)
"$ROOT/bin/tuac" "${TUAC_FLAGS[@]}" --output "$BIN_DIR/bench_tua" "$BENCH_DIR/tua/bench.tua"

echo "[build] tua bench (AOT, native cpu)"
TUAC_NATIVE_FLAGS=(
  --llvm-O3
  --no-loc
  --llvm-native
  -L "$ROOT/build/clib"
  -l tuaintmap
)
"$ROOT/bin/tuac" "${TUAC_NATIVE_FLAGS[@]}" --output "$BIN_DIR/bench_tua_native" "$BENCH_DIR/tua/bench.tua"

echo "[build] tua bench (AOT, unchecked index)"
TUAC_UNCHECKED_FLAGS=(
  --llvm-O3
  --no-loc
  --unchecked-index
  -L "$ROOT/build/clib"
  -l tuaintmap
)
"$ROOT/bin/tuac" "${TUAC_UNCHECKED_FLAGS[@]}" --output "$BIN_DIR/bench_tua_unchecked" "$BENCH_DIR/tua/bench.tua"

echo "[build] c bench"
cc -O3 -DNDEBUG -std=c11 "$BENCH_DIR/c/bench.c" -o "$BIN_DIR/bench_c"

echo "[build] c bench (native cpu)"
cc -O3 -DNDEBUG -march=native -std=c11 "$BENCH_DIR/c/bench.c" -o "$BIN_DIR/bench_c_native"

echo
echo "== Tua =="
"$BIN_DIR/bench_tua" "$@"

echo
echo "== Tua (native cpu) =="
"$BIN_DIR/bench_tua_native" "$@"

echo
echo "== Tua (unchecked-index) =="
"$BIN_DIR/bench_tua_unchecked" "$@"

echo
echo "== C =="
"$BIN_DIR/bench_c" "$@"

echo
echo "== C (native cpu) =="
"$BIN_DIR/bench_c_native" "$@"
