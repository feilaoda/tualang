

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