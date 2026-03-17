#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_DIR="$ROOT/bench"
BIN_DIR="$BENCH_DIR/bin"
TUA_SRC="$BENCH_DIR/tua/bench_game.tua"
TUA_BIN="$BIN_DIR/bench_game_tua"
LUA_SCRIPT="$BENCH_DIR/luajit/bench_game.lua"

LUAJIT_BIN="${LUAJIT_BIN:-}"
ROUNDS=5
REBUILD=1
USE_O3=1
CSV_OUT="$BIN_DIR/compare_tua_luajit_game.csv"

usage() {
  cat <<EOF
Usage: bench/compare_luajit_game.sh [options]

Options:
  --luajit <path>     LuaJIT binary path (or use LUAJIT_BIN env)
  --rounds <n>        Number of rounds for median (default: 5)
  --csv <path>        Output CSV path (default: bench/bin/compare_tua_luajit_game.csv)
  --no-rebuild        Skip rebuilding Tua benchmark binary
  --no-o3             Do not pass -O3 to LuaJIT
  -h, --help          Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --luajit)
      LUAJIT_BIN="$2"
      shift 2
      ;;
    --rounds)
      ROUNDS="$2"
      shift 2
      ;;
    --csv)
      CSV_OUT="$2"
      shift 2
      ;;
    --no-rebuild)
      REBUILD=0
      shift
      ;;
    --no-o3)
      USE_O3=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$LUAJIT_BIN" ]]; then
  if command -v luajit >/dev/null 2>&1; then
    LUAJIT_BIN="$(command -v luajit)"
  else
    echo "LuaJIT binary not found. Pass --luajit <path> or set LUAJIT_BIN." >&2
    exit 1
  fi
fi

if [[ ! -x "$LUAJIT_BIN" ]]; then
  echo "LuaJIT binary is not executable: $LUAJIT_BIN" >&2
  exit 1
fi

if ! [[ "$ROUNDS" =~ ^[0-9]+$ ]] || [[ "$ROUNDS" -lt 1 ]]; then
  echo "--rounds must be a positive integer" >&2
  exit 1
fi

if [[ ! -f "$LUA_SCRIPT" ]]; then
  echo "Missing LuaJIT benchmark script: $LUA_SCRIPT" >&2
  exit 1
fi

mkdir -p "$BIN_DIR"
mkdir -p "$(dirname "$CSV_OUT")"

if [[ "$REBUILD" -eq 1 ]]; then
  if [[ ! -x "$ROOT/bin/tuac" ]]; then
    make -C "$ROOT" tuac >/dev/null
  fi
  export TUA_STDLIB_DIR="$ROOT/std"
  export TUA_PACKAGE_DIR="$ROOT/packages"
  echo "[build] tua bench_game (profile=script, llvm-O3)"
  "$ROOT/bin/tuac" --profile script --llvm-O3 --no-loc --output "$TUA_BIN" "$TUA_SRC"
fi

if [[ ! -x "$TUA_BIN" ]]; then
  echo "Missing Tua benchmark binary: $TUA_BIN" >&2
  echo "Run without --no-rebuild or build it manually first." >&2
  exit 1
fi

tmpdir="$(mktemp -d /tmp/tua_compare_game.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT

luajit_cmd=("$LUAJIT_BIN")
if [[ "$USE_O3" -eq 1 ]]; then
  luajit_cmd+=("-O3")
fi
luajit_cmd+=("$LUA_SCRIPT" "--all")

for ((i = 1; i <= ROUNDS; i++)); do
  echo "[run] round ${i}/${ROUNDS}"
  (cd "$ROOT" && "$TUA_BIN" --all) > "$tmpdir/tua_${i}.log"
  (cd "$ROOT" && "${luajit_cmd[@]}") > "$tmpdir/luajit_${i}.log"
done

python3 - "$tmpdir" "$ROUNDS" "$CSV_OUT" <<'PY'
import csv
import re
import statistics
import sys
from pathlib import Path

tmpdir = Path(sys.argv[1])
rounds = int(sys.argv[2])
csv_out = Path(sys.argv[3])

pat_tua = re.compile(r"^tua/([a-zA-Z0-9_]+).* ns/iter=([0-9.eE+-]+)")
pat_lua = re.compile(r"^luajit/([a-zA-Z0-9_]+).* ns/iter=([0-9.eE+-]+)")


def parse_file(path, pat):
    result = {}
    for line in path.read_text().splitlines():
        m = pat.match(line.strip())
        if m:
            result[m.group(1)] = float(m.group(2))
    return result


tua_runs = []
lua_runs = []
for i in range(1, rounds + 1):
    tua_runs.append(parse_file(tmpdir / f"tua_{i}.log", pat_tua))
    lua_runs.append(parse_file(tmpdir / f"luajit_{i}.log", pat_lua))

common = set().union(*[set(r.keys()) for r in tua_runs]) & set().union(*[set(r.keys()) for r in lua_runs])
if not common:
    raise SystemExit("No overlapping benchmark cases found.")

ordered = ["binary_trees", "spectral_norm", "fannkuch_redux", "nbody"]
cases = [c for c in ordered if c in common] + sorted(common - set(ordered))

rows = []
for case in cases:
    tua_vals = [r[case] for r in tua_runs if case in r]
    lua_vals = [r[case] for r in lua_runs if case in r]
    if not tua_vals or not lua_vals:
        continue
    tua_med = statistics.median(tua_vals)
    lua_med = statistics.median(lua_vals)
    ratio = tua_med / lua_med
    if ratio < 1:
        faster = "Tua"
        speedup = 1.0 / ratio
    else:
        faster = "LuaJIT"
        speedup = ratio
    rows.append((case, tua_med, lua_med, ratio, faster, speedup))

with csv_out.open("w", newline="") as f:
    w = csv.writer(f)
    w.writerow(
        [
            "case",
            "tua_ns_per_iter_median",
            "luajit_ns_per_iter_median",
            "tua_over_luajit",
            "faster",
            "speedup",
        ]
    )
    for case, tua_med, lua_med, ratio, faster, speedup in rows:
        w.writerow([case, f"{tua_med:.6f}", f"{lua_med:.6f}", f"{ratio:.6f}", faster, f"{speedup:.6f}"])

print(f"rounds={rounds}")
print("case,tua_ns,luajit_ns,tua_over_luajit,faster,speedup")
for case, tua_med, lua_med, ratio, faster, speedup in rows:
    print(f"{case},{tua_med:.6f},{lua_med:.6f},{ratio:.4f},{faster},{speedup:.2f}x")
print(f"csv={csv_out}")
PY
