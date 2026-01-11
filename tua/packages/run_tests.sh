#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export TUA_STDLIB_DIR="$ROOT/std"
export TUA_PACKAGE_DIR="${TUA_PACKAGE_DIR:-$ROOT/packages}"
export TMPDIR="$ROOT/build/tmp"
mkdir -p "$TMPDIR"

make tuac >/dev/null

files=()
while IFS= read -r f; do
  [[ -n "$f" ]] || continue
  files+=("$f")
done < <(find "$ROOT/packages" -type f -name '*.tua' -path '*/tests/*' | sort)

if [[ "${#files[@]}" -eq 0 ]]; then
  echo "No tests found under packages/*/tests"
  exit 0
fi

# If any tests request external libraries via `// tuac: ...`, build them once up-front.
if grep -Eq "^// tuac:.*(^|[[:space:]])-l[[:space:]]*tuaextbpe([[:space:]]|$)" "${files[@]}" >/dev/null 2>&1; then
  if [[ ! -f "$ROOT/build/clib/libtuaextbpe.a" ]]; then
    ./tools/build_clib.sh tuaextbpe packages/clib/llm/tua_extbpe.c >/dev/null
  fi
fi

if grep -Eq "^// tuac:.*(^|[[:space:]])-l[[:space:]]*tuallm([[:space:]]|$)" "${files[@]}" >/dev/null 2>&1; then
  if [[ ! -f "$ROOT/build/clib/libtuallm.a" ]]; then
    ./tools/build_clib.sh tuallm packages/clib/llm/tua_llm.c packages/clib/llm/tua_llm_q4.c >/dev/null
  fi
fi

if grep -Eq "^// tuac:.*(^|[[:space:]])-l[[:space:]]*tuajson([[:space:]]|$)" "${files[@]}" >/dev/null 2>&1; then
  if [[ ! -f "$ROOT/build/clib/libtuajson.a" ]]; then
    ./tools/build_clib.sh tuajson packages/clib/json/tua_json.c >/dev/null
  fi
fi

llama_ok=0
if grep -Eq "^// tuac:.*(^|[[:space:]])-l[[:space:]]*tuaextllama([[:space:]]|$)" "${files[@]}" >/dev/null 2>&1; then
  if [[ -n "${LLAMA_PREFIX:-}" || -f "/usr/local/opt/llama.cpp/include/llama.h" || -f "/opt/homebrew/opt/llama.cpp/include/llama.h" ]]; then
    if [[ ! -f "$ROOT/build/clib/libtuaextllama.a" ]]; then
      if ./tools/build_llama_clib.sh >/dev/null 2>&1; then
        llama_ok=1
      else
        llama_ok=0
      fi
    else
      llama_ok=1
    fi
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

  if [[ "$llama_ok" -ne 1 && -n "$tuac_line" ]] && echo " $tuac_line " | grep -Eq "(^|[[:space:]])-l[[:space:]]*tuaextllama([[:space:]]|$)"; then
    echo "[SKIP] $f (llama.cpp not available)"
    skip=$((skip+1))
    continue
  fi

  if [[ "$wants_aot" -eq 1 && "$expects_fail" -eq 0 ]]; then
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_aot_XXXXXX")"
    out="$tmpd/a.out"
    if ./bin/tuac ${args[@]+"${args[@]}"} --output "$out" "$f" >/dev/null 2>&1; then
      rc=0
      "$out" >/dev/null 2>&1 || rc=$?
      if [[ -n "$expect_exit" ]]; then
        if [[ "$rc" -ne "$expect_exit" ]]; then
          echo "[FAIL] $f (expected exit $expect_exit, got $rc)"
          fail=1
        else
          echo "[PASS] $f"
        fi
      else
        if [[ "$rc" -ne 0 ]]; then
          echo "[FAIL] $f (expected exit 0, got $rc)"
          fail=1
        else
          echo "[PASS] $f"
        fi
      fi
    else
      echo "[FAIL] $f"
      fail=1
    fi
    rm -rf "$tmpd"
    continue
  fi

  if [[ "$wants_aot" -eq 1 && "$expects_fail" -eq 1 ]]; then
    tmpd="$(mktemp -d "${TMPDIR:-/tmp}/tuac_aot_XXXXXX")"
    out="$tmpd/a.out"
    tmp="$(mktemp)"
    if ./bin/tuac ${args[@]+"${args[@]}"} --output "$out" "$f" >/dev/null 2>"$tmp"; then
      echo "[FAIL] $f (expected failure, got success)"
      fail=1
    else
      if grep -Eq ":[0-9]+(:[0-9]+)?: error:|error:[0-9]+(:[0-9]+)?:" "$tmp"; then
        echo "[PASS] $f (expected failure)"
      else
        echo "[FAIL] $f (expected failure, missing line info)"
        fail=1
      fi
    fi
    rm -f "$tmp"
    rm -rf "$tmpd"
    continue
  fi

  if [[ "$expects_fail" -eq 1 ]]; then
    tmp="$(mktemp)"
    if ./bin/tuac ${args[@]+"${args[@]}"} "$f" >/dev/null 2>"$tmp"; then
      echo "[FAIL] $f (expected failure, got success)"
      fail=1
    else
      if grep -Eq ":[0-9]+(:[0-9]+)?: error:|error:[0-9]+(:[0-9]+)?:" "$tmp"; then
        echo "[PASS] $f (expected failure)"
      else
        echo "[FAIL] $f (expected failure, missing line info)"
        fail=1
      fi
    fi
    rm -f "$tmp"
  else
    case "$base" in
      std_rt_net_*)
        if [[ "$net_ok" -ne 1 ]]; then
          echo "[SKIP] $f (network sandboxed)"
          skip=$((skip+1))
          continue
        fi
        ;;
    esac
    rc=0
    ./bin/tuac ${args[@]+"${args[@]}"} "$f" >/dev/null 2>&1 || rc=$?
    if [[ -n "$expect_exit" ]]; then
      if [[ "$rc" -ne "$expect_exit" ]]; then
        echo "[FAIL] $f (expected exit $expect_exit, got $rc)"
        fail=1
      else
        echo "[PASS] $f"
      fi
    else
      if [[ "$rc" -ne 0 ]]; then
        echo "[FAIL] $f"
        fail=1
      else
        echo "[PASS] $f"
      fi
    fi
  fi
done

if [[ "$total" -eq 0 ]]; then
  echo "No tests found under packages/*/tests"
  exit 0
fi

exit "$fail"
