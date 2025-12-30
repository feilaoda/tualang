#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make tuac >/dev/null

fail=0
total=0

for f in tests/*.tua; do
  [ -e "$f" ] || continue
  total=$((total+1))
  base="$(basename "$f")"
  extra=""
  case "$base" in
    opt_unchecked_*|fail_opt_unchecked_*) extra="--unchecked-index" ;;
    opt_stack_*|fail_opt_stack_*) extra="--stack-fixed-arrays" ;;
    opt_perf_*|fail_opt_perf_*) extra="--perf" ;;
  esac
  if [[ "$base" == aot_* ]]; then
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_aot_XXXXXX")"
    out="$tmpd/a.out"
    if ./bin/tuac $extra --output "$out" "$f" >/dev/null 2>&1 && "$out" >/dev/null 2>&1; then
      echo "[PASS] $base"
    else
      echo "[FAIL] $base"
      fail=1
    fi
    rm -rf "$tmpd"
    continue
  fi
  if [[ "$base" == fail_* ]]; then
    tmp="$(mktemp)"
    if ./bin/tuac $extra "$f" >/dev/null 2>"$tmp"; then
      echo "[FAIL] $base (expected failure, got success)"
      fail=1
    else
      if grep -Eq ":[0-9]+(:[0-9]+)?: error:|error:[0-9]+(:[0-9]+)?:" "$tmp"; then
        echo "[PASS] $base (expected failure)"
      else
        echo "[FAIL] $base (expected failure, missing line info)"
        fail=1
      fi
    fi
    rm -f "$tmp"
  else
    if ./bin/tuac $extra "$f" >/dev/null 2>&1; then
      echo "[PASS] $base"
    else
      echo "[FAIL] $base"
      fail=1
    fi
  fi
done

if [[ "$total" -eq 0 ]]; then
  echo "No tests found in tests/*.tua"
  exit 1
fi

exit "$fail"
