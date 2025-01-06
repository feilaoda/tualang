
typedef enum {
    OP_INIT,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_RETURN,
    OP_RETURN0,
    OP_RETURN1,

    OP_JMP,
    OP_JE,  //eq
    OP_JNE, //not eq
    OP_JZ,  //zero
    OP_JNZ, //not zero
    OP_JLT,
    OP_JLE,
    OP_JGT,
    OP_JGE,
    OP_LABEL,
    OP_LOADI,
    OP_LOADF,
    OP_LOADS,
    OP_LOADA,
    OP_FORPREP,
    OP_FORLOOP,
    OP_ADDI,
    OP_SUBI,
    OP_MULI,
    OP_DIVI,
    OP_MODI,
    OP_CALL,
    OP_LOADFUNC, //LOADFUNC Ra Rb Rc
    OP_LOADENV,
    OP_PRINT,
    OP_GOTO,

    OP_UNKNOWN
} OpCode ;


OpCode str_to_opcode(const char* op);
const char* opcode_tostr(OpCode op);
