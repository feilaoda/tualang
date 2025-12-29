#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include "pool.h"

#define tua_uint16 unsigned short
#define tua_int16 short
#define tua_uint32 unsigned int
#define tua_int32 int
#define tua_uint64 unsigned long long
#define tua_int64 long long
#define tua_byte unsigned char
#define tua_double double


typedef tua_uint32 tua_instruction ;

#define L_INTHASBITS(b)		((UINT_MAX >> ((b) - 1)) >= 1)



#define MAXARG_A	((1<<SIZE_A)-1)
#define MAXARG_B	((1<<SIZE_B)-1)
#define MAXARG_C	((1<<SIZE_C)-1)
#define OFFSET_sC	(MAXARG_C >> 1)

#define int2sC(i)	((i) + OFFSET_sC)
#define sC2int(i)	((i) - OFFSET_sC)


#define SIZE_C		8
#define SIZE_B		8
#define SIZE_Bx		(SIZE_C + SIZE_B + 1)
#define SIZE_A		8
#define SIZE_Ax		(SIZE_Bx + SIZE_A)
#define SIZE_sJ		(SIZE_Bx + SIZE_A)

#define SIZE_OP		7
#define POS_OP		0

#define POS_A		(POS_OP + SIZE_OP)
#define POS_k		(POS_A + SIZE_A)
#define POS_B		(POS_k + 1)
#define POS_C		(POS_B + SIZE_B)

#define POS_Bx		POS_k

#define POS_Ax		POS_A

#define POS_sJ		POS_A

#if L_INTHASBITS(SIZE_Bx)
#define MAXARG_Bx	((1<<SIZE_Bx)-1)
#else
#define MAXARG_Bx	INT_MAX
#endif

#define OFFSET_sBx	(MAXARG_Bx>>1)         /* 'sBx' is signed */


#define MASK1(n,p)	((~((~(tua_instruction)0)<<(n)))<<(p))

#define MASK0(n,p)	(~MASK1(n,p))


#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(tua_instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))
#define NUM_OPCODES	((int)(OP_EXTRAARG) + 1)


#define cast(t, exp)	((t)(exp))

#define cast_void(i)	cast(void, (i))
#define cast_voidp(i)	cast(void *, (i))
#define cast_num(i)	cast(lua_Number, (i))
#define cast_int(i)	cast(int, (i))
#define cast_uint(i)	cast(unsigned int, (i))
#define cast_byte(i)	cast(lu_byte, (i))
#define cast_uchar(i)	cast(unsigned char, (i))
#define cast_char(i)	cast(char, (i))
#define cast_charp(i)	cast(char *, (i))
#define cast_sizet(i)	cast(size_t, (i))

#define check_exp(c,e)		(e)
#define checkopm(i,m)   ()
// #define checkopm(i,m)	(getOpMode(GET_OPCODE(i)) == m)

#define getarg(i,pos,size)	(cast_int(((i)>>(pos)) & MASK1(size,0)))
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                ((cast(tua_instruction, v)<<pos)&MASK1(size,pos))))


#define GETARG_A(i)	getarg(i, POS_A, SIZE_A)
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)

#define GETARG_B(i)	check_exp(checkopm(i, iABC), getarg(i, POS_B, SIZE_B))
#define GETARG_sB(i)	sC2int(GETARG_B(i))
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)

#define GETARG_C(i)	check_exp(checkopm(i, iABC), getarg(i, POS_C, SIZE_C))
#define GETARG_sC(i)	sC2int(GETARG_C(i))
#define SETARG_C(i,v)	setarg(i, v, POS_C, SIZE_C)

#define GETARG_k(i)	check_exp(checkopm(i, iABC), getarg(i, POS_k, 1))
#define SETARG_k(i,v)	setarg(i, v, POS_k, 1)

#define GETARG_Bx(i)	check_exp(checkopm(i, iABx), getarg(i, POS_Bx, SIZE_Bx))
#define SETARG_Bx(i,v)	setarg(i, v, POS_Bx, SIZE_Bx)

#define GETARG_Ax(i)	check_exp(checkopm(i, iAx), getarg(i, POS_Ax, SIZE_Ax))
#define SETARG_Ax(i,v)	setarg(i, v, POS_Ax, SIZE_Ax)

#define GETARG_sBx(i)   check_exp(checkopm(i, iAsBx), getarg(i, POS_Bx, SIZE_Bx) - OFFSET_sBx)
#define SETARG_sBx(i,b)	SETARG_Bx((i),cast_uint((b)+OFFSET_sBx))


#define RAV(x) (base[GETARG_A(x)].v.i)
#define RBV(x) (base[GETARG_B(x)].v.i)
#define RCV(x) (base[GETARG_C(x)].v.i)

#define RGSTR(x) (x+'a'-1)

#define NEW_STACK(st,s) ((st)=malloc(sizeof(tua_stack)), (st)->size=s, (st)->value=malloc(sizeof(tua_stack_value)*s))
#define INIT_STACK(st) ((st)->pt=(st)->value, base=(st)->value)

#define VARS(x) (base[(x)].v.i)
#define SETVALUE(x,v) (base[(x)].v.i) = v
#define VALUE(x) (base[x].v.i)

#define PUSH(i) ((*st->p)=(i), st->p++)
#define POP(v) ((v) = *(--st->p))

// typedef struct tua_call_info tua_call_info;


typedef struct tua_bytecode {
    int opcode;
    int arg1;
    int arg2;
    int arg3;
    tua_instruction i;
} tua_bytecode;

typedef struct tua_gcobject {
    void *u;
} tua_gcobject;

typedef union tua_value {
    tua_int32 i;
    tua_double n;
    tua_uint16 b; //bool
    tua_gcobject *gc;
} tua_value;

typedef struct tua_stack_value {
    tua_value v;
    tua_byte t; //tag
}tua_stack_value;

typedef tua_stack_value* tua_stack_id;

typedef union tua_stack_id_rel
{
    tua_stack_id p;
}tua_stack_id_rel;
 


typedef struct tua_object {
    int t;
    tua_value v;
}tua_object;

typedef struct tua_array {

}tua_array;


typedef struct tua_call_info {
    tua_stack_id st_start;
    tua_stack_id st_top;
    int nresults;
    struct tua_call_info *prev;
    struct tua_call_info *next;
    union fn
    {
        struct t{
            tua_instruction *savedpc;
        } t;
        struct c{
            int a;
        } c;
        /* data */
    }fn;
} tua_call_info;


typedef struct tua_function_table
{
    int num_instruction;
    int num_param;
    int num_register;
    int num_variable;
    int num_constant;
    int num_func;
    char * name;
    tua_instruction* instructions;
    int *funcs;
}tua_function_table;

typedef struct tua_proto {
    int num_instruction;
    int num_param;
    int num_register;
    int num_variable;
    int num_constant;
    int num_func;
    int is_vararg;
    int max_stack_size;
    char * name;
    struct tua_proto* sub_protos;
    int start_line;
    int last_line;
    tua_instruction* instructions;
}tua_proto;

typedef struct tua_stack
{
    tua_stack_value *value;
    // int *array;
    // int base[32];
    int size;
    struct tua_stack *prev;
} tua_stack;


typedef struct tua_state {
    
    tua_stack *st;
    tua_call_info *ci;
    tua_call_info *base_ci;
    int size_ci;
    int end_ci;
    int n;
    tua_instruction* savedpc;
    tua_instruction* pc;
    tua_stack_id st_start;
    tua_stack_id st_top;
    int * argv;
    int ret;
    pool_t *pool;
} tua_state;

typedef struct global_state {
    tua_function_table func_table;
    tua_state * state;
}global_state;

int tua_execute(tua_state *t, tua_call_info*ci, int labels[], int *argv);

void * tua_malloc(tua_state *, size_t size);