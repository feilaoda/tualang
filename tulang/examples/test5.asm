; 调用fib(a)并打印
#main
OP_LOADA R0 $0          ; load $argv[0] to R0
.L0 OP_LOADFUNC R1 .L3  ; load funcs:0 to Ra:0
OP_MOV R2 R0        
OP_CALL R1 #3 #2        ; call Ra:0 3-1params 2-1 return 1
OP_PRINT R1
OP_RETURN0

#fib
.L3 OP_LOADI R1 #0
OP_JE R0 R1 .L4    ; 
OP_RETURN1 R0
.L4 OP_LOADI R1 #1
OP_JE R0 R1 .L2
OP_RETURN1 R0
.L2 OP_LOADFUNC R2 .L3
OP_SUBI R3 R0 #1
OP_CALL R2 #2 #2
OP_LOADFUNC R4 .L3
OP_SUBI R5 R0 #2
OP_CALL R4 #2 #2
OP_ADD R6 R2 R4
OP_RETURN1 R6
#end_fib