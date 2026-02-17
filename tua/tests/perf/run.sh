#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

#make tuac >/dev/null

bench() {
  local f="$1"
  local cmd="$@"
  echo "== $f, cmd: $cmd"
  # Warm-up (ignore output)
  ./bin/tuac --perf "$f" >/dev/null 2>&1 || true
  # Measure
  /usr/bin/time -p ./bin/tuac --perf $cmd >/dev/null
  echo
}

bench tests/perf/tailrec_sum.tua
bench tests/perf/for_arith.tua
bench tests/perf/map_int_getset.tua
bench tests/perf/option_coalesce.tua
bench tests/perf/fib_iter.tua
bench tests/perf/for_loop.tua 100 10 1000