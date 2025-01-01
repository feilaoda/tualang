local add = function(a,b) 
    return a+b
end


local a = 0
for i = 0, arg[1], 1 do
    a = add(a,i)
end

print(a)
