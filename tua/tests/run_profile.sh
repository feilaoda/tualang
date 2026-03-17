#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"
export TUA_PACKAGE_DIR="${TUA_PACKAGE_DIR:-$ROOT/packages}"
export TMPDIR="$ROOT/build/tmp"
mkdir -p "$TMPDIR"

TUA_TUAC="${TUA_TUAC:-$ROOT/bin/tuac}"
if [[ "$TUA_TUAC" == "$ROOT/bin/tuac" ]]; then
  make tuac >/dev/null
fi
TUA_TEST_QUIET="${TUA_TEST_QUIET:-1}"

script_files=()
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  script_files+=("$f")
done < <(find tests -type f -name 'profile_script_*.tua' | sort)

system_fail_files=()
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  system_fail_files+=("$f")
done < <(find tests -type f -name 'profile_system_fail_*.tua' | sort)

if [[ "${#script_files[@]}" -eq 0 && "${#system_fail_files[@]}" -eq 0 ]]; then
  echo "No ownership profile tests found."
  exit 1
fi

fail=0
pass=0
total=0

for f in "${script_files[@]}"; do
  total=$((total + 1))
  base="$(basename "$f")"
  tmp="$(mktemp)"
  if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
    if "$TUA_TUAC" --profile script "$f" >/dev/null 2>"$tmp"; then
      echo "[PASS] $base"
      pass=$((pass + 1))
    else
      echo "[FAIL] $base"
      cat "$tmp"
      fail=1
    fi
  else
    if "$TUA_TUAC" --profile script "$f" 2>"$tmp"; then
      echo "[PASS] $base"
      pass=$((pass + 1))
    else
      echo "[FAIL] $base"
      cat "$tmp"
      fail=1
    fi
  fi
  rm -f "$tmp"
done

for f in "${system_fail_files[@]}"; do
  total=$((total + 1))
  base="$(basename "$f")"
  tmp="$(mktemp)"
  if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
    if "$TUA_TUAC" --profile system "$f" >/dev/null 2>"$tmp"; then
      echo "[FAIL] $base (expected failure, got success)"
      fail=1
    else
      if grep -Eq ":[0-9]+(:[0-9]+)?: error:|error:[0-9]+(:[0-9]+)?:" "$tmp"; then
        echo "[PASS] $base (expected failure)"
        pass=$((pass + 1))
      else
        echo "[FAIL] $base (expected failure, missing line info)"
        cat "$tmp"
        fail=1
      fi
    fi
  else
    if "$TUA_TUAC" --profile system "$f" 2>"$tmp"; then
      echo "[FAIL] $base (expected failure, got success)"
      fail=1
    else
      if grep -Eq ":[0-9]+(:[0-9]+)?: error:|error:[0-9]+(:[0-9]+)?:" "$tmp"; then
        echo "[PASS] $base (expected failure)"
        pass=$((pass + 1))
      else
        echo "[FAIL] $base (expected failure, missing line info)"
        cat "$tmp"
        fail=1
      fi
    fi
  fi
  rm -f "$tmp"
done

echo "profile tests: $pass/$total passed"
exit "$fail"
