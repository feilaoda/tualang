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

files=()
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  files+=("$f")
done < <(find tests -type f -name '*.tua' -not -path 'tests/modules/*' -not -path 'tests/perf/*' | sort)

if [[ "${#files[@]}" -eq 0 ]]; then
  echo "No tests found under tests (excluding tests/modules and tests/perf)"
  exit 1
fi

if grep -Eq "^// tuac:.*(^|[[:space:]])-l[[:space:]]*tuajson([[:space:]]|$)" "${files[@]}" >/dev/null 2>&1; then
  if [[ ! -f "$ROOT/build/clib/libtuajson.a" ]]; then
    ./tools/build_clib.sh tuajson packages/clib/json/tua_json.c >/dev/null
  fi
fi

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

for f in "${files[@]}"; do
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

  # Optional per-test directives (must appear in the leading comment header):
  #   // tuac: <extra flags>
  #   // expect-exit: <code>
  tuac_line=""
  expect_exit=""
  while IFS= read -r line; do
    case "$line" in
      "// tuac:"*)
        tuac_line="${line#// tuac:}"
        tuac_line="${tuac_line# }"
        ;;
      "// expect-exit:"*)
        expect_exit="${line#// expect-exit:}"
        expect_exit="${expect_exit# }"
        ;;
      "//"*) ;;
      "") ;;
      *) break ;;
    esac
  done < "$f"

  args=()
  if [[ -n "$extra" ]]; then args+=("$extra"); fi
  if [[ -n "$tuac_line" ]]; then
    read -r -a dirArgs <<< "$tuac_line"
    args+=("${dirArgs[@]}")
  fi

  if [[ "$wants_aot" -eq 1 && "$expects_fail" -eq 0 ]]; then
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_aot_XXXXXX")"
    out="$tmpd/a.out"
    if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
      compile_ok=0
      "$TUA_TUAC" ${args[@]+"${args[@]}"} --output "$out" "$f" >/dev/null 2>&1 && compile_ok=1
    else
      compile_ok=0
      "$TUA_TUAC" ${args[@]+"${args[@]}"} --output "$out" "$f" && compile_ok=1
    fi
    if [[ "$compile_ok" -eq 1 ]]; then
      rc=0
      if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
        "$out" >/dev/null 2>&1 || rc=$?
      else
        "$out" || rc=$?
      fi
      if [[ -n "$expect_exit" ]]; then
        if [[ "$rc" -ne "$expect_exit" ]]; then
          echo "[FAIL] $base (expected exit $expect_exit, got $rc)"
          fail=1
        else
          echo "[PASS] $base"
        fi
      else
        if [[ "$rc" -ne 0 ]]; then
          echo "[FAIL] $base (expected exit 0, got $rc)"
          fail=1
        else
          echo "[PASS] $base"
        fi
      fi
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
    if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
      if "$TUA_TUAC" ${args[@]+"${args[@]}"} --output "$out" "$f" >/dev/null 2>"$tmp"; then
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
    else
      if "$TUA_TUAC" ${args[@]+"${args[@]}"} --output "$out" "$f" 2>"$tmp"; then
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
    fi
    rm -f "$tmp"
    rm -rf "$tmpd"
    continue
  fi

  if [[ "$expects_fail" -eq 1 ]]; then
    tmp="$(mktemp)"
    if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
      if "$TUA_TUAC" ${args[@]+"${args[@]}"} "$f" >/dev/null 2>"$tmp"; then
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
    else
      if "$TUA_TUAC" ${args[@]+"${args[@]}"} "$f" 2>"$tmp"; then
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
    rc=0
    if [[ "$TUA_TEST_QUIET" -eq 1 ]]; then
      "$TUA_TUAC" ${args[@]+"${args[@]}"} "$f" >/dev/null 2>&1 || rc=$?
    else
      "$TUA_TUAC" ${args[@]+"${args[@]}"} "$f" || rc=$?
    fi
    if [[ -n "$expect_exit" ]]; then
      if [[ "$rc" -ne "$expect_exit" ]]; then
        echo "[FAIL] $base (expected exit $expect_exit, got $rc)"
        fail=1
      else
        echo "[PASS] $base"
      fi
    else
      if [[ "$rc" -ne 0 ]]; then
        echo "[FAIL] $base"
        fail=1
      else
        echo "[PASS] $base"
      fi
    fi
  fi
done

exit "$fail"
