#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"

want_llm=0
want_perf=0
want_aot=0
verbose=0
filter=""

usage() {
  cat <<'USAGE'
Usage: examples/run.sh [options]

Options:
  --llm            Include llm_* examples (may be slow / memory heavy)
  --perf           Include *_perf.tua and llm_bench_* examples
  --aot            Build+run examples via AOT output (default: JIT via tuac)
  --filter <re>    Bash regex to match example filename (basename)
  --verbose        Print example stdout/stderr
  --help           Show this help

Examples:
  bash examples/run.sh
  bash examples/run.sh --llm
  bash examples/run.sh --llm --filter 'qwen3'
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --llm) want_llm=1; shift ;;
    --perf) want_perf=1; shift ;;
    --aot) want_aot=1; shift ;;
    --filter)
      if [[ $# -lt 2 ]]; then echo "missing arg for --filter" >&2; exit 2; fi
      filter="$2"
      shift 2
      ;;
    --verbose) verbose=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

make tuac >/dev/null 2>&1

fail=0
selected=0
ran=0
skip=0

run_one() {
  local f="$1"
  local base
  base="$(basename "$f")"

  local extra=""
  case "$base" in
    *opt_unchecked_*) extra="--unchecked-index" ;;
    *opt_stack_*) extra="--stack-fixed-arrays" ;;
    *opt_perf_*) extra="--perf" ;;
    *check_extern_*) extra="--check-extern" ;;
  esac

  local expects_fail=0
  if [[ "$base" == fail_* || "$base" == *_fail_* || "$base" == *_fail.tua ]]; then expects_fail=1; fi

  if [[ "$expects_fail" -eq 0 ]]; then
    if ! grep -Eq '^[[:space:]]*(fn|func)[[:space:]]+main[[:space:]]*[(]' "$f"; then
      echo "[SKIP] $base (no main)"
      skip=$((skip+1))
      return
    fi
  fi

  local tuac_line=""
  local expect_exit=""
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

  local args=()
  if [[ -n "$extra" ]]; then args+=("$extra"); fi
  if [[ -n "$tuac_line" ]]; then
    read -r -a dirArgs <<< "$tuac_line"
    args+=("${dirArgs[@]}")
  fi

  if [[ "$expects_fail" -eq 1 ]]; then
    local tmp
    tmp="$(mktemp)"
    if ./bin/tuac ${args[@]+"${args[@]}"} "$f" >/dev/null 2>"$tmp"; then
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
    ran=$((ran+1))
    return
  fi

  local rc=0
  if [[ "$want_aot" -eq 1 ]]; then
    local tmpd out
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_examples_aot_XXXXXX")"
    out="$tmpd/a.out"
    if [[ "$verbose" -eq 1 ]]; then
      ./bin/tuac ${args[@]+"${args[@]}"} --output "$out" "$f"
      "$out" || rc=$?
    else
      ./bin/tuac ${args[@]+"${args[@]}"} --output "$out" "$f" >/dev/null 2>&1
      "$out" >/dev/null 2>&1 || rc=$?
    fi
    rm -rf "$tmpd"
  else
    if [[ "$verbose" -eq 1 ]]; then
      ./bin/tuac ${args[@]+"${args[@]}"} "$f" || rc=$?
    else
      ./bin/tuac ${args[@]+"${args[@]}"} "$f" >/dev/null 2>&1 || rc=$?
    fi
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
  ran=$((ran+1))
}

for f in examples/*.tua; do
  [[ -e "$f" ]] || continue
  base="$(basename "$f")"

  if [[ -n "$filter" && ! "$base" =~ $filter ]]; then
    continue
  fi

  case "$base" in
    llm_*)
      if [[ "$want_llm" -ne 1 ]]; then
        skip=$((skip+1))
        continue
      fi
      ;;
  esac

  case "$base" in
    *_perf.tua|llm_bench_*)
      if [[ "$want_perf" -ne 1 ]]; then
        skip=$((skip+1))
        continue
      fi
      ;;
  esac

  selected=$((selected+1))
  run_one "$f"
done

if [[ "$selected" -eq 0 ]]; then
  echo "No examples selected in examples/*.tua"
  exit 1
fi

echo "examples: selected=$selected ran=$ran skip=$skip"
exit "$fail"
