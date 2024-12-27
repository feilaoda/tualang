function fib(n)
    local a = 1
    local b = 1
    for i = n, 1, -1 do
        a = a * i
    end
    return a
end

fib(1000)
-- Example usage
-- local n = 10000000 -- Example input
-- local result = fib(n)
-- print(string.format("fib(%d) = %d", n, result))
-- print(string.dump(fib))