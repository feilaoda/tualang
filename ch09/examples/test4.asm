; 循环调用a=add(a+1)并打印
OP_LOADI R0 #0          ; load 0 to R0
OP_LOADA R1 $0          ; load $argv[0] to R1
OP_LOADI R2 #0          ; load 0 to R2
OP_LOADI R3 #-1         ; load -1 to R3
OP_FORPREP R1 .L1       ; if R1>0 do else jmp .PRINT#
.L0 OP_LOADFUNC R4 .L3  ; load funcs:0 to Ra:0
OP_MOV R5 R0        
OP_MOV R6 R1
OP_CALL R4 #3 #2        ; call Ra:0 3-1params 2-1 return 1
OP_MOV R0 R4
.L1 OP_FORLOOP R1 .L0   ; if R1>#0 jmp .LOOP
OP_PRINT R0
OP_RETURN0
.L3 OP_ADD R2 R0 R1 ; func: addr:-2
OP_RETURN1 R2