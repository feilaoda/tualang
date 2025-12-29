#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

make tuac >/dev/null

bench() {
  local f="$1"
  echo "== $f"
  # Warm-up (ignore output)
  ./bin/tuac "$f" >/dev/null 2>&1 || true
  # Measure
  /usr/bin/time -p ./bin/tuac "$f" >/dev/null
  echo
}

bench examples/perf/tailrec_sum.tua
bench examples/perf/for_arith.tua
bench examples/perf/map_int_getset.tua
bench examples/perf/option_coalesce.tua
bench examples/perf/fib_iter.tua

