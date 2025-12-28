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
  if [[ "$base" == fail_* ]]; then
    tmp="$(mktemp)"
    if ./bin/tuac "$f" >/dev/null 2>"$tmp"; then
      echo "[FAIL] $base (expected failure, got success)"
      fail=1
    else
      if grep -q "at line" "$tmp"; then
        echo "[PASS] $base (expected failure)"
      else
        echo "[FAIL] $base (expected failure, missing line info)"
        fail=1
      fi
    fi
    rm -f "$tmp"
  else
    if ./bin/tuac "$f" >/dev/null 2>&1; then
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
