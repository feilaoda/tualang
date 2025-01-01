function fib(a) 
    if a == 1 or a == 0 then
        return a
    else
    return fib(a-1) + fib(a-2)
    end
end


print(fib(tonumber(arg[1])))
