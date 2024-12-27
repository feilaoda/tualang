function fib(n,k)
    local a = 1
    local b = 1
    for i = k*n, 1, -k do
        a = a + i
    end
    return a
end

fib(arg[1],arg[2])
-- Example usage
-- local n = 10000000 -- Example input
-- local result = fib(n)
-- print(string.format("fib(%d) = %d", n, result))
-- print(string.dump(fib))