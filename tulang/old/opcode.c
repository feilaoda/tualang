#include "opcode.h"
#include <string.h>

static const char* OpCodeNames[] = {
    "OP_INIT",
    "OP_MOV",
    "OP_ADD",
    "OP_SUB",
    "OP_MUL", 
    "OP_DIV",
    "OP_MOD",
    "OP_RETURN",
    "OP_RETURN0",
    "OP_RETURN1",
    "OP_JMP",
    "OP_JE",
    "OP_JNE",
    "OP_JZ",
    "OP_JNZ", 
    "OP_JLT",
    "OP_JLE",
    "OP_JGT",
    "OP_JGE",
    "OP_LABEL",
    "OP_LOADI",
    "OP_LOADF",
    "OP_LOADS",
    "OP_LOADA",
    "OP_FORPREP",
    "OP_FORLOOP",
    "OP_ADDI",
    "OP_SUBI", 
    "OP_MULI",
    "OP_DIVI",
    "OP_MODI",
    "OP_CALL",
    "OP_LOADFUNC",
    "OP_LOADENV",
    "OP_PRINT",
    "OP_GOTO",
    "OP_UNKNOWN"
};


const char* opcode_tostr(OpCode op) {
    if (op < 0 || op >= sizeof(OpCodeNames) / sizeof(OpCodeNames[0])) {
        return "UNKNOWN";
    }
    return OpCodeNames[op];
}

OpCode str_to_opcode(const char* op) {
    if (strcmp(op, "OP_INIT") == 0) return OP_INIT;
    else if (strcmp(op, "OP_MOV") == 0) return OP_MOV;
    else if (strcmp(op, "OP_ADD") == 0) return OP_ADD;
    else if (strcmp(op, "OP_SUB") == 0) return OP_SUB;
    else if (strcmp(op, "OP_MUL") == 0) return OP_MUL;
    else if (strcmp(op, "OP_DIV") == 0) return OP_DIV;
    else if (strcmp(op, "OP_MOD") == 0) return OP_MOD;

    
    else if (strcmp(op, "OP_RETURN") == 0) return OP_RETURN;
    else if (strcmp(op, "OP_RETURN0") == 0) return OP_RETURN0;
    else if (strcmp(op, "OP_RETURN1") == 0) return OP_RETURN1;
    else if (strcmp(op, "OP_JMP") == 0) return OP_JMP;
    else if (strcmp(op, "OP_JE") == 0) return OP_JE;
    else if (strcmp(op, "OP_JNE") == 0) return OP_JNE;
    else if (strcmp(op, "OP_JZ") == 0) return OP_JZ;
    
    
    else if (strcmp(op, "OP_JNZ") == 0) return OP_JNZ;
    else if (strcmp(op, "OP_JLT") == 0) return OP_JLT;
    else if (strcmp(op, "OP_JLE") == 0) return OP_JLE;
    else if (strcmp(op, "OP_JGT") == 0) return OP_JGT;
    else if (strcmp(op, "OP_JGE") == 0) return OP_JGE;
    else if (strcmp(op, "OP_LABEL") == 0) return OP_LABEL;
    else if (strcmp(op, "OP_LOADI") == 0) return OP_LOADI;
    else if (strcmp(op, "OP_LOADF") == 0) return OP_LOADF;
    else if (strcmp(op, "OP_LOADS") == 0) return OP_LOADS;
    else if (strcmp(op, "OP_LOADA") == 0) return OP_LOADA;
    else if (strcmp(op, "OP_FORPREP") == 0) return OP_FORPREP;
    else if (strcmp(op, "OP_FORLOOP") == 0) return OP_FORLOOP;
    else if (strcmp(op, "OP_ADDI") == 0) return OP_ADDI;
    else if (strcmp(op, "OP_SUBI") == 0) return OP_SUBI;
    else if (strcmp(op, "OP_MULI") == 0) return OP_MULI;
    else if (strcmp(op, "OP_DIVI") == 0) return OP_DIVI;
    else if (strcmp(op, "OP_MODI") == 0) return OP_MODI;
    else if (strcmp(op, "OP_CALL") == 0) return OP_CALL;
    else if (strcmp(op, "OP_LOADFUNC") == 0) return OP_LOADFUNC;
    else if (strcmp(op, "OP_LOADENV") == 0) return OP_LOADENV;
    else if (strcmp(op, "OP_PRINT") == 0) return OP_PRINT;
    else if (strcmp(op, "OP_GOTO") == 0) return OP_GOTO;
    return OP_UNKNOWN;
}