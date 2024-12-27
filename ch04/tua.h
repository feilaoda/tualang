

int TUA_BASE[512] = {0};

#define RG(i) (t->st->array[(int)(i)])

#define ra RG(i)
#define rb RG(i+1)
#define rc RG(i+2)


typedef enum {
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_RET
} OpCode ;

typedef struct tua_call_info {
    int func;
    int top;
    int base;
} tua_call_info;

typedef struct tua_instruction{
    int opcode;
    char arg1[32];
    char arg2[32];
    char arg3[32];
} tua_instruction;

typedef struct tua_stack
{
    /* data */
    int *array;
    int p;
    int size;
} tua_stack;


typedef struct tua_state {
    tua_stack *st;
    tua_call_info *ci;
    tua_call_info *base_ci;
    int size_ci;
    int end_ci;
    int n;
    int size;
} tua_state;


void tua_pushint(tua_state *t, int n);
void tua_popint(tua_state *t, int n);