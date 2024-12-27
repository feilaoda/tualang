

#define tua_uint32 unsigned int
#define tua_int32 int
#define tua_uint64 unsigned long long
#define tua_int64 long long
#define tua_byte unsigned char

typedef tua_uint32 tua_instruction ;

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


int TUA_BASE[512] = {0};

#define ISRG(i) (i)

#define RG(i) (t->st->array[(int)(i)])


typedef enum {
    OP_INIT,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_RET,
    OP_CALL,
    OP_JMP,
    OP_JZ,
    OP_JNZ,
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

    OP_IPUSH,
    OP_ISTORE,
    OP_GOTO,
    OP_IADD,
    OP_IFICMPLT,
    OP_IFICMPGT,
    OP_IRETURN,
    OP_IINC,
    OP_ILOAD,
    OP_ARGSTORE,
    OP_IFICMPGTX,
    OP_IADDX,
    OP_UNKNOWN
} OpCode ;

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


#define RA(i) (base+GETARG_A(i))
#define RB(i) (base+GETARG_B(i))
#define RC(i) (base+GETARG_C(i))

#define VARS(x) (vars[(x)].i)

typedef struct tua_call_info {
    int func;
    int top;
    int base;
} tua_call_info;


typedef struct tua_bytecode {
    int opcode;
    int arg1;
    int arg2;
    int arg3;
    tua_instruction i;
} tua_bytecode;

typedef union tua_value {
    int i;
    double n;
    char *s;
} tua_value;

typedef struct tua_stack
{
    tua_value value[32];
    int *array;
    int base[32];
    int *p;
    int size;
} tua_stack;

#define NEW_STACK(st,s) ((st)=malloc(sizeof(tua_stack)), (st)->size=s, (st)->base=malloc(sizeof(int)*s), (st)->p=(st)->base)
#define INIT_STACK(st) ((st)->p=(st)->base)
#define PUSH(i) ((*st->p)=(i), st->p++)
#define POP(v) ((v) = *(--st->p))

typedef struct tua_state {
    int *array;
    int p;
    int size;
    tua_stack *st;
    tua_call_info *ci;
    tua_call_info *base_ci;
    int size_ci;
    int end_ci;
    int n;
    tua_instruction* savedpc;
    tua_instruction* pc;
    int * argv;
    int ret;
} tua_state;


void tua_pushint(tua_state *t, int n);
void tua_popint(tua_state *t, int n);