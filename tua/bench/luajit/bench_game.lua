local ffi = require('ffi')
local bit = require('bit')

ffi.cdef[[
typedef long time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);

typedef struct {
  double x;
  double y;
  double z;
  double vx;
  double vy;
  double vz;
  double mass;
} body_t;
]]

local CLOCK_MONOTONIC = 6
local body_arr_t = ffi.typeof('body_t[5]')

local function now_ns()
  local ts = ffi.new('struct timespec')
  ffi.C.clock_gettime(CLOCK_MONOTONIC, ts)
  return tonumber(ts.tv_sec) * 1e9 + tonumber(ts.tv_nsec)
end

local function report(name, iters, dt_ns, sink)
  io.write(string.format('%s iters=%d dt_ns=%.0f ns/iter=%.6f sink=%d\n', name, iters, dt_ns, dt_ns / iters, sink))
end

local function parse_int(s, def)
  local n = tonumber(s)
  if not n then
    return def
  end
  n = math.floor(n)
  if n < 0 then
    return def
  end
  return n
end

local function run_case(name, iters, warm_fn, fn)
  if warm_fn then
    warm_fn()
    warm_fn()
  end
  collectgarbage('collect')
  local t0 = now_ns()
  local sink = fn()
  local t1 = now_ns()
  report(name, iters, t1 - t0, sink)
end

-- ---------------- binary-trees ----------------

local function make_tree(item, depth)
  if depth > 0 then
    return { item, make_tree(item * 2 - 1, depth - 1), make_tree(item * 2, depth - 1) }
  end
  return { item }
end

local function item_check(node)
  local left = node[2]
  if left == nil then
    return node[1]
  end
  return node[1] + item_check(left) - item_check(node[3])
end

local function bench_binary_trees(max_depth_in)
  local min_depth = 4
  local max_depth = max_depth_in
  if max_depth < min_depth + 2 then
    max_depth = min_depth + 2
  end

  local stretch_depth = max_depth + 1
  local stretch_tree = make_tree(0, stretch_depth)
  local stretch_check = item_check(stretch_tree)

  local long_lived_tree = make_tree(0, max_depth)
  local acc = stretch_check
  local depth = min_depth
  while depth <= max_depth do
    local iterations = bit.lshift(1, max_depth - depth + min_depth)
    local chk = 0
    for i = 1, iterations do
      chk = chk + item_check(make_tree(i, depth))
      chk = chk + item_check(make_tree(-i, depth))
    end
    acc = acc + chk
    depth = depth + 2
  end

  return acc + item_check(long_lived_tree)
end

-- ---------------- spectral-norm ----------------

local function eval_a(i, j)
  local ij = i + j
  return 1.0 / (((ij * (ij + 1)) * 0.5) + i + 1.0)
end

local function eval_a_times_u(u, out, n)
  for i = 0, n - 1 do
    local s = 0.0
    for j = 0, n - 1 do
      s = s + eval_a(i, j) * u[j]
    end
    out[i] = s
  end
end

local function eval_at_times_u(u, out, n)
  for i = 0, n - 1 do
    local s = 0.0
    for j = 0, n - 1 do
      s = s + eval_a(j, i) * u[j]
    end
    out[i] = s
  end
end

local function spectral_norm(n, iters)
  local u = ffi.new('double[?]', n)
  local v = ffi.new('double[?]', n)
  local tmp = ffi.new('double[?]', n)
  for i = 0, n - 1 do
    u[i] = 1.0
    v[i] = 0.0
    tmp[i] = 0.0
  end

  for _ = 1, iters do
    eval_a_times_u(u, tmp, n)
    eval_at_times_u(tmp, v, n)
    eval_a_times_u(v, tmp, n)
    eval_at_times_u(tmp, u, n)
  end

  local vbv = 0.0
  local vv = 0.0
  for i = 0, n - 1 do
    vbv = vbv + u[i] * v[i]
    vv = vv + v[i] * v[i]
  end
  return math.sqrt(vbv / vv)
end

-- ---------------- fannkuch-redux ----------------

local function fannkuch_redux(n)
  local perm1 = ffi.new('int[?]', n)
  local count = ffi.new('int[?]', n)
  local perm = ffi.new('int[?]', n)
  for i = 0, n - 1 do
    perm1[i] = i
    count[i] = 0
  end

  local max_flips = 0
  local checksum = 0
  local perm_count = 0
  local r = n

  while true do
    while r ~= 1 do
      count[r - 1] = r
      r = r - 1
    end

    for i = 0, n - 1 do
      perm[i] = perm1[i]
    end

    local flips = 0
    local k = perm[0]
    while k ~= 0 do
      local lo = 0
      local hi = k
      while lo < hi do
        local t = perm[lo]
        perm[lo] = perm[hi]
        perm[hi] = t
        lo = lo + 1
        hi = hi - 1
      end
      flips = flips + 1
      k = perm[0]
    end

    if flips > max_flips then
      max_flips = flips
    end
    if bit.band(perm_count, 1) == 0 then
      checksum = checksum + flips
    else
      checksum = checksum - flips
    end

    while true do
      if r == n then
        return checksum * 1000000 + max_flips
      end

      local first = perm1[0]
      for i = 0, r - 1 do
        perm1[i] = perm1[i + 1]
      end
      perm1[r] = first

      count[r] = count[r] - 1
      if count[r] > 0 then
        perm_count = perm_count + 1
        break
      end
      r = r + 1
    end
  end
end

-- ---------------- n-body ----------------

local function make_bodies()
  local pi = 3.141592653589793
  local solar_mass = 4.0 * pi * pi
  local days_per_year = 365.24

  local bodies = body_arr_t()

  bodies[0].x = 0.0
  bodies[0].y = 0.0
  bodies[0].z = 0.0
  bodies[0].vx = 0.0
  bodies[0].vy = 0.0
  bodies[0].vz = 0.0
  bodies[0].mass = solar_mass

  bodies[1].x = 4.84143144246472090e+00
  bodies[1].y = -1.16032004402742839e+00
  bodies[1].z = -1.03622044471123109e-01
  bodies[1].vx = 1.66007664274403694e-03 * days_per_year
  bodies[1].vy = 7.69901118419740425e-03 * days_per_year
  bodies[1].vz = -6.90460016972063023e-05 * days_per_year
  bodies[1].mass = 9.54791938424326609e-04 * solar_mass

  bodies[2].x = 8.34336671824457987e+00
  bodies[2].y = 4.12479856412430479e+00
  bodies[2].z = -4.03523417114321381e-01
  bodies[2].vx = -2.76742510726862411e-03 * days_per_year
  bodies[2].vy = 4.99852801234917238e-03 * days_per_year
  bodies[2].vz = 2.30417297573763929e-05 * days_per_year
  bodies[2].mass = 2.85885980666130812e-04 * solar_mass

  bodies[3].x = 1.28943695621391310e+01
  bodies[3].y = -1.51111514016986312e+01
  bodies[3].z = -2.23307578892655734e-01
  bodies[3].vx = 2.96460137564761618e-03 * days_per_year
  bodies[3].vy = 2.37847173959480950e-03 * days_per_year
  bodies[3].vz = -2.96589568540237556e-05 * days_per_year
  bodies[3].mass = 4.36624404335156298e-05 * solar_mass

  bodies[4].x = 1.53796971148509165e+01
  bodies[4].y = -2.59193146099879641e+01
  bodies[4].z = 1.79258772950371181e-01
  bodies[4].vx = 2.68067772490389322e-03 * days_per_year
  bodies[4].vy = 1.62824170038242295e-03 * days_per_year
  bodies[4].vz = -9.51592254519715870e-05 * days_per_year
  bodies[4].mass = 5.15138902046611451e-05 * solar_mass

  return bodies
end

local function offset_momentum(bodies)
  local px = 0.0
  local py = 0.0
  local pz = 0.0
  for i = 0, 4 do
    px = px + bodies[i].vx * bodies[i].mass
    py = py + bodies[i].vy * bodies[i].mass
    pz = pz + bodies[i].vz * bodies[i].mass
  end

  local pi = 3.141592653589793
  local solar_mass = 4.0 * pi * pi
  bodies[0].vx = -px / solar_mass
  bodies[0].vy = -py / solar_mass
  bodies[0].vz = -pz / solar_mass
end

local function advance(bodies, dt, steps)
  local n = 5
  for _ = 1, steps do
    for i = 0, n - 1 do
      local bix = bodies[i].x
      local biy = bodies[i].y
      local biz = bodies[i].z
      local bivx = bodies[i].vx
      local bivy = bodies[i].vy
      local bivz = bodies[i].vz
      local bimass = bodies[i].mass

      for j = i + 1, n - 1 do
        local dx = bix - bodies[j].x
        local dy = biy - bodies[j].y
        local dz = biz - bodies[j].z
        local dist2 = dx * dx + dy * dy + dz * dz
        local invdist = 1.0 / math.sqrt(dist2)
        local mag = dt * invdist * invdist * invdist
        local bim = bodies[j].mass * mag
        local bjm = bimass * mag

        bivx = bivx - dx * bim
        bivy = bivy - dy * bim
        bivz = bivz - dz * bim

        bodies[j].vx = bodies[j].vx + dx * bjm
        bodies[j].vy = bodies[j].vy + dy * bjm
        bodies[j].vz = bodies[j].vz + dz * bjm
      end

      bodies[i].vx = bivx
      bodies[i].vy = bivy
      bodies[i].vz = bivz
    end

    for i = 0, n - 1 do
      bodies[i].x = bodies[i].x + dt * bodies[i].vx
      bodies[i].y = bodies[i].y + dt * bodies[i].vy
      bodies[i].z = bodies[i].z + dt * bodies[i].vz
    end
  end
end

local function energy(bodies)
  local e = 0.0
  local n = 5
  for i = 0, n - 1 do
    e = e + 0.5 * bodies[i].mass * (bodies[i].vx * bodies[i].vx + bodies[i].vy * bodies[i].vy + bodies[i].vz * bodies[i].vz)
    for j = i + 1, n - 1 do
      local dx = bodies[i].x - bodies[j].x
      local dy = bodies[i].y - bodies[j].y
      local dz = bodies[i].z - bodies[j].z
      local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
      e = e - (bodies[i].mass * bodies[j].mass) / dist
    end
  end
  return e
end

local function bench_nbody(steps)
  local bodies = make_bodies()
  offset_momentum(bodies)
  local e0 = energy(bodies)
  advance(bodies, 0.01, steps)
  local e1 = energy(bodies)

  local sink = 0
  if e1 > e0 then
    sink = sink + 4
  else
    sink = sink - 4
  end
  if bodies[0].x > 0.0 then
    sink = sink + 1
  else
    sink = sink - 1
  end
  if bodies[1].y > 0.0 then
    sink = sink + 2
  else
    sink = sink - 2
  end
  return sink
end

local function run_all()
  local depth = 10
  local spectral_n = 260
  local spectral_iters = 10
  local fannkuch_n = 8
  local nbody_steps = 20000

  run_case('luajit/binary_trees', 1, function()
    bench_binary_trees(6)
  end, function()
    return bench_binary_trees(depth)
  end)

  run_case('luajit/spectral_norm', 1, function()
    spectral_norm(80, 4)
  end, function()
    return math.floor(spectral_norm(spectral_n, spectral_iters) * 1000000000.0)
  end)

  run_case('luajit/fannkuch_redux', 1, function()
    fannkuch_redux(7)
  end, function()
    return fannkuch_redux(fannkuch_n)
  end)

  run_case('luajit/nbody', 1, function()
    bench_nbody(3000)
  end, function()
    return bench_nbody(nbody_steps)
  end)
end

local function run_one(name, iters)
  local depth = 10
  local spectral_n = 260
  local spectral_iters = 10
  local fannkuch_n = 8
  local nbody_steps = 20000

  if name == 'binary_trees' then
    depth = iters
  elseif name == 'spectral_norm' then
    spectral_n = iters
  elseif name == 'fannkuch_redux' then
    fannkuch_n = iters
  elseif name == 'nbody' then
    nbody_steps = iters
  end

  if name == 'binary_trees' then
    local t0 = now_ns()
    local sink = bench_binary_trees(depth)
    local t1 = now_ns()
    report('luajit/binary_trees', 1, t1 - t0, sink)
    return 0
  end

  if name == 'spectral_norm' then
    local t0 = now_ns()
    local sink = math.floor(spectral_norm(spectral_n, spectral_iters) * 1000000000.0)
    local t1 = now_ns()
    report('luajit/spectral_norm', 1, t1 - t0, sink)
    return 0
  end

  if name == 'fannkuch_redux' then
    local t0 = now_ns()
    local sink = fannkuch_redux(fannkuch_n)
    local t1 = now_ns()
    report('luajit/fannkuch_redux', 1, t1 - t0, sink)
    return 0
  end

  if name == 'nbody' then
    local t0 = now_ns()
    local sink = bench_nbody(nbody_steps)
    local t1 = now_ns()
    report('luajit/nbody', 1, t1 - t0, sink)
    return 0
  end

  io.stderr:write('unknown bench: ', name, '\n')
  return 2
end

local function main(args)
  if #args == 0 or args[1] == '--all' then
    run_all()
    return 0
  end

  if args[1] ~= '--bench' then
    io.stderr:write('usage: bench_game.lua --all | --bench <name> [--iters N]\n')
    return 2
  end

  if #args < 2 then
    io.stderr:write('missing bench name\n')
    return 2
  end

  local name = args[2]
  local iters = 0
  local i = 3
  while i <= #args do
    if args[i] == '--iters' and i + 1 <= #args then
      iters = parse_int(args[i + 1], 0)
    end
    i = i + 1
  end

  if iters <= 0 then
    if name == 'binary_trees' then
      iters = 10
    elseif name == 'spectral_norm' then
      iters = 260
    elseif name == 'fannkuch_redux' then
      iters = 8
    elseif name == 'nbody' then
      iters = 20000
    end
  end

  return run_one(name, iters)
end

os.exit(main(arg))
