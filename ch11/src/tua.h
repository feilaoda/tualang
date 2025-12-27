#include "opcode.h"
#include "tvm.h"


int TUA_BASE[512] = {0};

#define ISRG(i) (i)

#define RG(i) (t->st->array[(int)(i)])




void tua_pushint(tua_state *t, int n);
void tua_popint(tua_state *t, int n);