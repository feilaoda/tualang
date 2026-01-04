#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"

make tuac >/dev/null

fail=0
total=0
skip=0

net_ok=1
python3 - <<'PY' >/dev/null 2>&1 || net_ok=0
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.bind(("127.0.0.1", 0))
    s.listen(1)
finally:
    s.close()
PY

for f in tests/*.tua; do
  [ -e "$f" ] || continue
  total=$((total+1))
  base="$(basename "$f")"
  extra=""
  case "$base" in
    *opt_unchecked_*) extra="--unchecked-index" ;;
    *opt_stack_*) extra="--stack-fixed-arrays" ;;
    *opt_perf_*) extra="--perf" ;;
    *check_extern_*) extra="--check-extern" ;;
  esac

  wants_aot=0
  expects_fail=0
  if [[ "$base" == aot_* || "$base" == *_aot_* ]]; then wants_aot=1; fi
  if [[ "$base" == fail_* || "$base" == *_fail_* ]]; then expects_fail=1; fi

  if [[ "$wants_aot" -eq 1 && "$expects_fail" -eq 0 ]]; then
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

  if [[ "$wants_aot" -eq 1 && "$expects_fail" -eq 1 ]]; then
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_aot_XXXXXX")"
    out="$tmpd/a.out"
    tmp="$(mktemp)"
    if ./bin/tuac $extra --output "$out" "$f" >/dev/null 2>"$tmp"; then
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
    rm -rf "$tmpd"
    continue
  fi

  if [[ "$expects_fail" -eq 1 ]]; then
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
    case "$base" in
      std_rt_net_*)
        if [[ "$net_ok" -ne 1 ]]; then
          echo "[SKIP] $base (network sandboxed)"
          skip=$((skip+1))
          continue
        fi
        ;;
    esac
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
