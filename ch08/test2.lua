local test = function(n,k)
    local a = 0
    for i = n, 1, -k do
        a = a + i
    end
    return a
end

print( test(arg[1],arg[2]))
-- Example usage
-- local n = 10000000 -- Example input
-- local result = fib(n)
-- print(string.format("fib(%d) = %d", n, result))
-- print(string.dump(fib))