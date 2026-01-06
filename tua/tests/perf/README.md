# Performance Benchmarks

These are small `.tua` programs intended to measure **end-to-end** performance of `tuac`
(parse + analyze + LLVM codegen + JIT + run).

Run one benchmark:

```sh
make tuac
time ./bin/tuac tests/perf/map_int_getset.tua
```

Run all benchmarks (includes a warm-up run for each):

```sh
bash tests/perf/run.sh
```

Notes:
- JIT startup dominates small programs; run multiple times for stable numbers.
- Benchmarks include `assert(...)` to keep results “used” and validate correctness.
