

int TUA_BASE[512] = {0};



#define OP_MOV 0
#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4
#define OP_MOD 5
#define OP_RET 6

typedef struct tua_stack
{
    /* data */
    int *array;
    int p;
    int size;
} tua_stack;


typedef struct tua_state {
    tua_stack *st;
    int n;
    int size;
} tua_state;


void tua_pushint(tua_state *t, int n);
void tua_popint(tua_state *t, int n);