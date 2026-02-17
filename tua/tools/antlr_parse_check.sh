#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v antlr >/dev/null 2>&1; then
  echo "error: antlr not found in PATH" >&2
  exit 1
fi

if ! command -v javac >/dev/null 2>&1; then
  echo "error: javac not found in PATH" >&2
  exit 1
fi

ANTLR_BIN="$(command -v antlr)"
ANTLR_REAL="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$ANTLR_BIN")"

CLASSPATH_LINE="$(sed -n '2p' "$ANTLR_REAL" || true)"
CLASSPATH_VAL="$(printf '%s\n' "$CLASSPATH_LINE" | sed -nE 's/.*CLASSPATH="([^"]*)".*/\1/p')"
ANTLR_JAR="${CLASSPATH_VAL%%:*}"

if [[ -z "${ANTLR_JAR}" || ! -f "${ANTLR_JAR}" ]]; then
  echo "error: failed to locate antlr complete jar from: ${ANTLR_REAL}" >&2
  exit 1
fi

OUT="$ROOT/.tmp/antlr_ref"
rm -rf "$OUT"
mkdir -p "$OUT"

antlr -Dlanguage=Java -o "$OUT" "$ROOT/tuaparser.g4"
javac -cp "$ANTLR_JAR" "$OUT"/*.java

run_one() {
  local file="$1"
  local err
  err="$(java -cp "$OUT:$ANTLR_JAR" org.antlr.v4.gui.TestRig tuaparser compilationUnit -tree "$file" >/dev/null 2>&1 || true)"
  if [[ -n "$err" ]]; then
    echo "[PARSE-ERR] $file" >&2
    echo "$err" >&2
    return 1
  fi
  return 0
}

paths=()
if [[ "$#" -gt 0 ]]; then
  paths=("$@")
else
  paths=("examples" "tests")
fi

count=0
fail=0
while IFS= read -r -d '' f; do
  base="$(basename "$f")"
  if [[ "$base" == fail_* || "$base" == *_fail_* ]]; then
    continue
  fi
  if ! run_one "$f"; then
    fail=1
  fi
  count=$((count+1))
done < <(find "${paths[@]}" -type f -name '*.tua' -print0)

if [[ "$fail" -ne 0 ]]; then
  echo "[FAIL] ANTLR parse errors (checked $count files)" >&2
  exit 1
fi

echo "[OK] ANTLR parsed $count files"
