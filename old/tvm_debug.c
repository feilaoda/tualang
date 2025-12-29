#include "tvm_debug.h"


void print_array(int *array, int size) {
    printf("[");
    for (int i = 0; i < size; i++) {
        printf("%d", array[i]);
        if (i < size - 1) {
            printf(", ");
        }
    }
    printf("]\n");
}

void print_bytecodes(tua_instruction *bytecode, int num_bytecode) {
    for (int i = 0; i < num_bytecode; i++) {
        tua_instruction instr = bytecode[i];
        // printf("%03d: %s %d %d %d\n", i, opcode_tostr(instr.opcode), instr.arg1, instr.arg2, instr.arg3);
    }
}

