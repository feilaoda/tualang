; 调用add并打印
OP_LOADFUNC R1 #6 ; load funcs:0 to Ra:0
OP_LOADA R2 $0
OP_LOADA R3 $1
OP_CALL R1 #3 #2 ; call Ra:0 3-1params 1return all
OP_PRINT R1 ;
OP_RETURN0
OP_ADD R2 R0 R1 ; func: addr:-2
OP_RETURN1 R2
; .PRINT Ra:0