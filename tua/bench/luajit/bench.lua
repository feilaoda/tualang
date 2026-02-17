local ffi = require('ffi')
local bit = require('bit')

ffi.cdef[[
typedef long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);
]]

local CLOCK_MONOTONIC = 6

local function now_ns()
  local ts = ffi.new('struct timespec')
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts.tv_sec) * 1e9 + tonumber(ts.tv_nsec)
end

local function report(name, iters, dt_ns, sink)
  io.write(string.format('%s iters=%d dt_ns=%.0f ns/iter=%.6f sink=%d\n', name, iters, dt_ns, dt_ns / iters, sink))
end

local function bench_arith_int(iters)
  local x = 1
  local acc = 0
  for _ = 1, iters do
    x = bit.tobit(x * 1664525 + 1013904223)
    acc = acc + bit.band(x, 0xffff)
  end
  return acc
end

local function make_float_array(n, seed)
  local a = ffi.new('float[?]', n)
  for i = 0, n - 1 do
    a[i] = i * 0.001 + seed
  end
  return a
end

local function bench_dot_f32(iters, a, b, n)
  local sum = 0.0
  for _ = 1, iters do
    local s = 0.0
    for i = 0, n - 1 do
      s = s + a[i] * b[i]
    end
    sum = sum + s
  end
  return sum
end

local function make_bytes(n)
  local b = ffi.new('uint8_t[?]', n)
  for i = 0, n - 1 do
    b[i] = bit.band(i * 131 + 7, 0xff)
  end
  return b
end

local function bench_bytes_scan(iters, b, n, needle)
  local total = 0
  for _ = 1, iters do
    local cnt = 0
    for i = 0, n - 1 do
      if b[i] == needle then
        cnt = cnt + 1
      end
    end
    total = total + cnt
  end
  return total
end

local function make_map(size)
  local m = {}
  for i = 0, size - 1 do
    m[i] = i * 2 + 1
  end
  return m
end

local function bench_map_lookup(iters, m, mod)
  local acc = 0
  local x = 1
  local mask = mod - 1
  for _ = 1, iters do
    x = bit.band(x * 1103515245 + 12345, 0x7fffffff)
    local k = bit.band(x, mask)
    acc = acc + m[k]
  end
  return acc
end

local function make_map_foo(size)
  local m = {}
  for i = 0, size - 1 do
    m[i] = { a = i * 2 + 1 }
  end
  return m
end

local function bench_map_lookup_foo(iters, m, mod)
  local acc = 0
  local x = 1
  local mask = mod - 1
  for _ = 1, iters do
    x = bit.band(x * 1103515245 + 12345, 0x7fffffff)
    local k = bit.band(x, mask)
    acc = acc + m[k].a
  end
  return acc
end

local function bench_json_scan_top_long(iters, s)
  local acc = 0
  for _ = 1, iters do
    local v = s:match('\"id\"%s*:%s*(-?%d+)')
    if not v then
      return -1
    end
    acc = acc + tonumber(v)
  end
  return acc
end

local function run_case(name, iters, fn)
  -- Warm JIT traces before timing.
  fn(math.max(10, math.floor(iters / 200)))
  fn(math.max(10, math.floor(iters / 200)))
  collectgarbage('collect')

  local t0 = now_ns()
  local sink = fn(iters)
  local t1 = now_ns()
  report(name, iters, t1 - t0, sink)
end

local function run_all()
  local iters_arith = 50000000
  local iters_lookup = 20000000
  local iters_bytes = 50
  local iters_dot = 3000
  local iters_json = 2000

  run_case('luajit/arith_int', iters_arith, function(it)
    return bench_arith_int(it)
  end)

  local n = 1024
  local a = make_float_array(n, 0.25)
  local b = make_float_array(n, 0.75)
  run_case('luajit/dot_f32_1024', iters_dot, function(it)
    return bench_dot_f32(it, a, b, n)
  end)

  local bytes_n = 1024 * 1024
  local bytes = make_bytes(bytes_n)
  run_case('luajit/bytes_scan_1m', iters_bytes, function(it)
    return bench_bytes_scan(it, bytes, bytes_n, 127)
  end)

  run_case('luajit/bytes_scan_1m_slice', iters_bytes, function(it)
    return bench_bytes_scan(it, bytes, bytes_n, 127)
  end)

  local m = make_map(8192)
  run_case('luajit/map_lookup_8k', iters_lookup, function(it)
    return bench_map_lookup(it, m, 8192)
  end)

  local mf = make_map_foo(8192)
  run_case('luajit/map_lookup_foo_8k', iters_lookup, function(it)
    return bench_map_lookup_foo(it, mf, 8192)
  end)

  local jf = io.open('bench/data/scan_top_level.json', 'rb')
  if jf then
    local s = jf:read('*a')
    jf:close()
    run_case('luajit/json_scan_top_long', iters_json, function(it)
      return bench_json_scan_top_long(it, s)
    end)
  else
    io.write('luajit/json_scan_top_long skipped (missing input)\n')
  end
end

run_all()
