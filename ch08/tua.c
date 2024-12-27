#include "tua.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define MAX_INT 2147483647

char RGSTR(int i) {
    return i+'a'-1;
}


int REG(char i) {
    int r = i-'a'+1;
    // printf("get rg:%c=%d\n", i,r);
    return r;
}

char OPSTR(int op) {
    if(op == OP_ADD) {
        return '+';
    } else if(op == OP_SUB) {
        return '-';
    } else if(op == OP_MUL) {
        return '*';
    } else if(op == OP_DIV) {
        return '/';
    } else if(op == OP_MOD) {
        return '%';
    } else if(op == OP_MOV) {
        return 'm';
    } else if(op == OP_RET) {
        return 'r';
    }
    return 0;
}


static const char* OpCodeNames[] = {
    "OP_INIT", "OP_MOV", "OP_ADD", "OP_SUB", "OP_MUL", "OP_DIV", "OP_MOD",
    "OP_RET", "OP_CALL", "OP_JMP", "OP_JZ", "OP_JNZ", "OP_JLT", "OP_JLE",
    "OP_JGT", "OP_JGE", "OP_LABEL", "OP_LOADI", "OP_LOADF", "OP_LOADS",
    "OP_LOADA", "OP_FORPREP", "OP_FORLOOP", "OP_ADDI", "OP_SUBI", "OP_MULI",
    "OP_DIVI", "OP_MODI", "OP_IPUSH", "OP_ISTORE", "OP_GOTO", "OP_IADD",
    "OP_IFICMPLT", "OP_IFICMPGT", "OP_IRETURN", "OP_IINC", "OP_ILOAD","OP_ARGSTORE"
};

const char* opcode_tostr(OpCode op) {
    if (op < 0 || op >= sizeof(OpCodeNames) / sizeof(OpCodeNames[0])) {
        return "UNKNOWN";
    }
    return OpCodeNames[op];
}

#define TUA_STACK_SIZE 32
tua_state* tua_newstate(int stack_size) {
    struct tua_state* t = (tua_state*)malloc(sizeof(struct tua_state));
    t->st = (tua_stack*)malloc(sizeof(struct tua_stack));
    t->st->array = malloc(sizeof(int)*stack_size);
    t->st->p = 0;
    t->st->size = stack_size;

    t->array = malloc(sizeof(int)*stack_size);
    t->p = 0;
    t->size = stack_size;
    return t;
}

void tua_free(tua_state *t) {
    free(t->st->array);
    free(t->st);
    free(t);
}

void tua_setnumberv(tua_state *t, int pos, int n) {
    t->array[pos] = n;
    // printf("set %d=%d\n", pos, n);
}
int tua_tonumber(tua_state *t, int n) {
    int v = t->array[n];
    // printf("get rg vaule R%c=%d\n", n+'a'-1, v);
    return v;
}

void tua_setnumber(tua_state *t, int pos, int n) {
    t->array[pos] = n;
    // printf("set %d=%d\n", pos, n);
}

int tua_toint(tua_state *t, int n) {
    int v = t->array[n];
    // printf("get rg vaule R%c=%d\n", n+'a'-1, v);
    return v;
}


void add_op(tua_state *t, int pos1,  int pos2, int pos3) {
    
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left + right);
    // printf("add %d=%d+%d %d,%d\n", pos1, left, right, pos2, pos3);
}

void sub_op(tua_state *t, int pos1, int pos2, int pos3) {
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left - right);
}

void mul_op(tua_state *t,int pos1, int pos2, int pos3) {
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left * right);
}

void div_op(tua_state *t, int pos1, int pos2, int pos3) {
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left / right);
}

void mod_op(tua_state *t, int pos1, int pos2, int pos3) {
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left % right);
}

void add_opi(tua_state *t, int pos1, int r2, int pos2, int r3, int pos3) {
    
    int left = r2==1?tua_toint(t,pos2):pos2;
    int right = r3==1?tua_toint(t, pos3):pos3;
    tua_setnumber(t, pos1, left + right);
    // printf("add %d=%d+%d %d,%d\n", pos1, left, right, pos2, pos3);
}

void op_addi(tua_state *t, int pos1, int pos2,  int v) {
    int left = tua_toint(t,pos2);
    tua_setnumber(t, pos1, left + v);
}
void op_subi(tua_state *t, int pos1, int pos2,  int v) {
    int left = tua_toint(t,pos2);
    tua_setnumber(t, pos1, left - v);
}

void mul_opi(tua_state *t,int pos1, int r2, int pos2, int r3, int pos3) {
    int left = r2==1?tua_toint(t,pos2):pos2;
    int right = r3==1?tua_toint(t, pos3):pos3;
    tua_setnumber(t, pos1, left * right);
}

void div_opi(tua_state *t, int pos1, int r2, int pos2, int r3, int pos3) {
    int left = r2==1?tua_toint(t,pos2):pos2;
    int right = r3==1?tua_toint(t, pos3):pos3;
    tua_setnumber(t, pos1, left / right);
}

void mod_opi(tua_state *t, int pos1, int r2, int pos2, int r3, int pos3) {
    int left = tua_toint(t,pos2);
    int right = tua_toint(t, pos3);
    tua_setnumber(t, pos1, left % right);
}


int op_forloop(tua_state *t, int pos1, int pos2, int step) {
    int start = tua_toint(t, pos1);
    int end = tua_toint(t, pos2);
    if(start >= end) {
        // printf("for loop true %c:%d>%c:%d\n", RGSTR(pos1),start,RGSTR(pos2), end);
        op_subi(t, pos1, pos1, 1);
        return step;
    }
    else {
        // printf("for loop end %c:%d>%c/%d:%d\n", RGSTR(pos1),start,RGSTR(pos2),pos2, end);
        return -1;
    }
}

int parse_int(const char *s) {
    int n = 0;
    while (*s) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

void call_op(tua_state *t, int op, int pos1,  int pos2, int pos3) {
    switch (op) {
        case OP_ADD:
            add_op(t, pos1,  pos2,  pos3);
            break;
        case OP_SUB:
            sub_op(t, pos1, pos2, pos3);
            break;
        case OP_MUL:
            mul_op(t, pos1, pos2, pos3);
            break;
        case OP_DIV:
            div_op(t, pos1, pos2, pos3);
            break;
        case OP_MOD:
            mod_op(t, pos1, pos2, pos3);
            break;
        default:
            printf("Unknown operator\n");
            exit(-1);
            break;
    }
}

void call_opi(tua_state *t, int op, int pos1, int r2, int pos2, int r3, int pos3) {
    switch (op) {
        case OP_ADD:
            add_opi(t, pos1, r2,pos2, r3, pos3);
            break;
        case OP_SUB:
            op_subi(t, pos1,  pos2,  pos3);
            break;
        case OP_MUL:
            mul_opi(t, pos1, r2,pos2, r3, pos3);
            break;
        case OP_DIV:
            div_opi(t, pos1, r2,pos2, r3, pos3);
            break;
        case OP_MOD:
            mod_opi(t, pos1, r2,pos2, r3, pos3);
            break;
        default:
            printf("Unknown operator\n");
            exit(-1);
            break;
    }
}




void run_op_add(tua_state *t, tua_bytecode *bytecode) {
    // printf("run_op_add %d %d %d\n", bytecode->arg1, bytecode->arg2, bytecode->arg3);
    // add_op(t, bytecode->arg1, bytecode->arg2, bytecode->arg3);
    (*t->pc)++;
}
void run_op_loadi(tua_state *t, tua_bytecode *bytecode) {
    // printf("run_op_loadi %d %d\n", bytecode->arg1, bytecode->arg2);
    tua_setnumber(t, bytecode->arg1, bytecode->arg2);
    (*t->pc)++;
}

void run_op_loada(tua_state *t, tua_bytecode *bytecode) {
    // printf("run_op_loada %d %d\n", bytecode->arg1, bytecode->arg2);
    int v = t->argv[bytecode->arg2];
    tua_setnumber(t, bytecode->arg1, v);
    (*t->pc)++;
}

void run_op_mov(tua_state *t, tua_bytecode *bytecode) {
    tua_setnumber(t, bytecode->arg1, bytecode->arg2);
    (*t->pc)++;
}

void run_op_ret(tua_state *t, tua_bytecode *bytecode) {
    int v = tua_toint(t,bytecode->arg1);
    (*t->pc) = MAX_INT;
    t->ret = v;
}

void run_op_forprep(tua_state *t, tua_bytecode *bytecode) {
    int count = tua_toint(t, bytecode->arg1) - tua_toint(t, bytecode->arg2);
    tua_setnumber(t, bytecode->arg1, count);
    if(count>0) {
        (*t->pc)++;
    }else {
        (*t->pc)+=bytecode->arg3;
    }
}
int n = 0;
void run_op_forloop(tua_state *t, tua_bytecode *bytecode) {
    int left = tua_toint(t, bytecode->arg1);
    if(left>0) {
        // printf("for loop true %c:%d>%c:%d\n", RGSTR(pos1),start,RGSTR(pos2), end);
        tua_setnumber(t, bytecode->arg1, left - 1);
        // op_subi(t, bytecode->arg1, bytecode->arg1, 1);
        n++;
        (*t->pc)-=bytecode->arg2;
    }
    else {
        // printf("for loop end %c:%d>%c/%d:%d\n", RGSTR(pos1),start,RGSTR(pos2),pos2, end);
       (*t->pc)++;
       n++;
    }
    // if(n< 10000000) {
    //     n++;
    //     (*t->pc)-=bytecode->arg3;
    // }else {
    //     (*t->pc)++;
    // }
}

typedef void (*bytecode_func)(tua_state *t, tua_bytecode *bytecode);

void (*global_func[256])(tua_state *t, tua_bytecode *bytecode) = {0};

void init_bytecodefunc() {
    global_func[OP_ADD] = run_op_add;
    global_func[OP_LOADI] = run_op_loadi;
    global_func[OP_LOADA] = run_op_loada;
    global_func[OP_MOV] = run_op_mov;
    global_func[OP_RET] = run_op_ret;
    global_func[OP_FORPREP] = run_op_forprep;
    global_func[OP_FORLOOP] = run_op_forloop;
}

#define VAR(i) (vars[i])
#define VALUEPTR tua_value*
#define VALUE(x, b) ((RA(x))->i = b)
#define GETIVALUE(i) ((RA(i))->i)

#define GETVALUE(x)  ((base+(x))->i)


int tua_execute(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv) {
    tua_instruction *pc = 0;
    int cnt = argv[0];
    pc = t->savedpc;
    // int *base = t->array;
    tua_stack base_stack;
    tua_stack *st = &base_stack;
    INIT_STACK(st);
    // NEW_STACK(st, 32);
    // printf("line %d %d", i, code[0]);
    int vars[32] = {0};
    //使用union存储变量
    tua_instruction *savedpc = t->savedpc;
    tua_value *base = base_stack.value;

    int a=0,b=0,c=0;
    while (1) {
        tua_instruction i = *pc;
        // printf("CNT:%d op:%s arg1:%d pc:%d\n",cnt++, opcode_tostr(GET_OPCODE(i)), GETARG_A(i), *pc);
        switch (GET_OPCODE(i))
        {
            case OP_LOADI:
            {
                // int* ra = RA(i);
                // int v = GETARG_B(i);
                // *ra = GETARG_B(i);
                VALUE(i, GETARG_B(i));
                pc++;
                // printf("OP_LOADI %c=%d\n", RGSTR(GETARG_A(i)), v);
                break;
            }
            case OP_LOADA:
            {
                // int *ra = RA(i);
                // int k = GETARG_B(i);
                // int v = argv[k];
                // *ra = v;

                VALUE(i, argv[GETARG_B(i)]);
                pc++;
                // printf("OP_LOADA %c=%d\n", RGSTR(GETARG_A(i)), v);
                break;
            }
        case OP_MOV:
            /* code */
            {
                // printf("call before op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode), RGSTR(r),tua_toint(t,r), v);
                // tua_setnumber(t, bytecode.arg1, bytecode.arg2);
                // printf("call after op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode),  RGSTR(r),tua_toint(t,r), v);
                // pc++;
            break;
            }
        case OP_RET:
        {
            // int *ra = RA(i);
            // int v = *ra;

            int v = GETIVALUE(i);

            printf("OP_RET %c=%d cnt:%d\n", RGSTR(GETARG_A(i)),v, cnt);
            pc++;
            return v;
        }
        case OP_ADD:
        {
            // int *ra = RA(i);
            // int *rb = RB(i);
            // int *rc = RC(i);
            // *ra = *rb + *rc;
            // int v = GETVALUE(GETARG_B(i)) + GETVALUE(GETARG_C(i));
            // VALUE(i,  v);
            // (base+GETARG_A(i))->i = (base+GETARG_B(i))->i + (base+GETARG_C(i))->i;
                base[GETARG_A(i)].i = base[GETARG_B(i)].i + base[GETARG_C(i)].i;
            pc++;
            // printf("OP_ADD %c=%c+%c %d,%d,%d, step:%d\n", RGSTR(GETARG_A(i)), RGSTR(GETARG_B(i)), RGSTR(GETARG_C(i)), *ra, *rb, *rc, *pc);
            break;
        }
        case OP_SUB:
        case OP_MUL:{
            // int *ra = RA(i);
            // int *rb = RB(i);
            // int *rc = RC(i);
            // *ra = *rb * *rc;
            // printf("OP_MUL %c=%c+%c %d,%d,%d, step:%d\n", RGSTR(GETARG_A(i)), RGSTR(GETARG_B(i)), RGSTR(GETARG_C(i)), *ra, *rb, *rc, *pc);
            break;
        }
        case OP_DIV:
        case OP_MOD:
        {
            // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(bytecode.opcode), bytecode.arg1+'a'-1,tua_toint(t,bytecode.arg1), bytecode.arg2+'a'-1,tua_toint(t,bytecode.arg2),  bytecode.arg3+'a'-1 ,  tua_toint(t,bytecode.arg3));
            // call_op(t, bytecode.opcode, bytecode.arg1, bytecode.arg2, bytecode.arg3);
            // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(bytecode.opcode), bytecode.arg1+'a'-1,tua_toint(t,bytecode.arg1), bytecode.arg2+'a'-1,tua_toint(t,bytecode.arg2),  bytecode.arg3+'a'-1 ,  tua_toint(t,bytecode.arg3));
            break;
        }
        case OP_ADDI:
        case OP_SUBI:
        case OP_MULI:
        case OP_DIVI:
        case OP_MODI:
        {

            // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3);
            // call_opi(t, bytecode.opcode, pos1, r2,r2==1?pos2:v2, r3,r3==1 ? pos3 : v3);
            // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3 );
            break;
        }
        case OP_JZ:
        {
            break;
        }
        case OP_FORPREP: 
        {
            // int *ra = RA(i);
            // int *rb = RB(i);
            // int count = *ra - *rb;
    
            // // printf("OP_FORPREP %d %d count:%d k=%d\n", *ra, *rb, count, GETARG_C(i));
            // *ra = count;
            // if(*ra<=0) {
            //     int k = GETARG_C(i);
            //     pc+=k;
            // }

            int count = GETVALUE(GETARG_A(i)) - GETVALUE(GETARG_B(i));
    
            // printf("OP_FORPREP %d %d count:%d k=%d\n", *ra, *rb, count, GETARG_C(i));
            VALUE(i, count);

            if(count<=0) {
                int k = GETARG_C(i);
                pc+=k;
            }
            pc++;
            break;
        }
        case OP_FORLOOP: {
            // int *ra = RA(i);
            // int count = *ra;
            // if(count>0) {
            //     *ra = count - 1;
            //     pc-=GETARG_B(i);
            // }

            int count = base[GETARG_A(i)].i;

            if(base[GETARG_A(i)].i>0) {
                // VALUE(i, count - 1);
                base[GETARG_A(i)].i --;
                pc -= GETARG_B(i);
            }else {
                pc++;
            }
            
            // printf("OP_FORLOOP %d cnt:%d, next:%d\n", *ra, count, GETARG_Bx(i));
            
            break;
        }
        case OP_JMP: {
            
            break;
        }
        case OP_LABEL:
            break;

        case OP_IPUSH:
        {
            int v = GETARG_A(i);
            PUSH(v);
            break;
        }
        case OP_ISTORE:
        {
            int v;
            POP(v);
            int r = GETARG_A(i);
            VAR(r) = v;
            // printf("OP_ISTORE %d=%d\n", r, v);
            break;
        }
        case OP_IRETURN:
        {
            int r = GETARG_A(i);
            return VAR(r);
            break;
        }
        case OP_GOTO:
        {
            int k = GETARG_A(i);
            pc = savedpc + k;
            break;
        }
        case OP_ILOAD:
        {
            int r = GETARG_A(i);
            int v = VAR(r);
            PUSH(v);
            // printf("OP_ILOAD %d=%d\n", r, v);
            break;
        }
        case OP_IFICMPLT:
            break;
        case OP_IFICMPGT:
        {
            int v1, v2;
            POP(v2);
            POP(v1);
            // printf("OP_IFICMPGT %d %d\n", v1, v2);
            if(v1>v2) {
                int k = GETARG_A(i);
                pc += k - 1;
            }
            break;
        }
        case OP_IFICMPGTX:
        {
            int va = VAR(GETARG_A(i));
            int vb = VAR(GETARG_B(i));
            if(va > vb) {
                int k = GETARG_C(i);
                pc += k - 1;
            }
            break;
        }
        case OP_IINC:
        {
            int r = GETARG_A(i);
            // int b = GETARG_B(i);
            VAR(r) += 1;
            break;
        }
        case OP_IADD:
        {
            int v1, v2;
            POP(v1);
            POP(v2);
            PUSH(v1+v2);
            break;
        }
        case OP_IADDX:
        {
            int va = VAR(GETARG_A(i));
            int vb = VAR(GETARG_B(i));
            VAR(GETARG_C(i)) = va + vb;
            break;
        }
        case OP_ARGSTORE:
        {
            int v = GETARG_A(i);
            int k = GETARG_B(i);
            VAR(v) = argv[k];
            break;
        }
        default:
            break;
        }
    }
    printf("return cnt:%d\n", cnt);
    return 0;
}

int tua_execute0(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv) {
    int cnt = argv[0];
    // int *base = t->array;
    tua_stack base_stack;
    tua_stack *st = &base_stack;
    INIT_STACK(st);
    int vars[32] = {0};
    //使用union存储变量
    tua_instruction *savedpc = t->savedpc;
    tua_value *base = base_stack.value;
    tua_bytecode *pc = bytecodes;
    int a=0,b=0,c=0;
    while (1) {
        // tua_instruction i = *pc;
        tua_bytecode *i = pc;
        // printf("CNT:%d op:%s arg1:%d pc:%d\n",cnt++, opcode_tostr(i->opcode), i->arg1, i->arg2);
        switch (i->opcode)
        {
            case OP_LOADI:
            {
                // int* ra = RA(i);
                // int v = GETARG_B(i);
                // *ra = GETARG_B(i);
                vars[i->arg1] = i->arg2;
                pc++;
                printf("OP_LOADI %c=%d\n", RGSTR(i->arg1),i->arg2);
                break;
            }
            case OP_LOADA:
            {
                // int *ra = RA(i);
                // int k = GETARG_B(i);
                // int v = argv[k];
                // *ra = v;
                vars[i->arg1] = argv[i->arg2];
                pc++;
                printf("OP_LOADA %c=%d\n", RGSTR(i->arg1), argv[i->arg2]);
                break;
            }
        
        case OP_RET:
        {
            // int *ra = RA(i);
            // int v = *ra;

            int v = vars[i->arg1];
            printf("OP_RET %c=%d cnt:%d\n", RGSTR(i->arg1),v, cnt);
            pc++;
            return v;
        }
        case OP_ADD:
        {
            // int *ra = RA(i);
            // int *rb = RB(i);
            // int *rc = RC(i);
            // *ra = *rb + *rc;
            // int v = GETVALUE(GETARG_B(i)) + GETVALUE(GETARG_C(i));
            // VALUE(i,  v);
            vars[i->arg1] = vars[i->arg2] + vars[i->arg3];
            pc++;
            // printf("OP_ADD %c=%c+%c %d,%d,%d, step:%d\n", RGSTR(i->arg1), RGSTR(i->arg2), RGSTR(i->arg3), vars[i->arg1],vars[i->arg2],vars[i->arg3],cnt);
            break;
        }
        case OP_FORPREP: 
        {
            int count = vars[i->arg1] - vars[i->arg2];
            // printf("OP_FORPREP %d %d count:%d k=%d\n", *ra, *rb, count, GETARG_C(i));
            vars[i->arg1] = count;
            if(count<=0) {
                pc+=i->arg3;
            }
            pc++;
            break;
        }
        case OP_FORLOOP: {

            int count = vars[i->arg1];
            if(count>0) {
                vars[i->arg1] --;
                pc-= i->arg2;
            }else {
                pc++;
            }
            
            // printf("OP_FORLOOP %d cnt:%d, next:%d\n", *ra, count, GETARG_Bx(i));
            
            break;
        }
        
        default:
            break;
        }
    }
    printf("return cnt:%d\n", cnt);
    return 0;
}

void assert_equal(int a, int b) {
    if(a != b) {
        printf("assert failed %d != %d\n", a, b);
    }else {
        printf(" true\n");
    }
}

OpCode str_to_opcode(const char* op) {
    if (strcmp(op, "OP_INIT") == 0) return OP_INIT;
    else if (strcmp(op, "OP_MOV") == 0) return OP_MOV;
    else if (strcmp(op, "OP_ADD") == 0) return OP_ADD;
    else if (strcmp(op, "OP_SUB") == 0) return OP_SUB;
    else if (strcmp(op, "OP_MUL") == 0) return OP_MUL;
    else if (strcmp(op, "OP_DIV") == 0) return OP_DIV;
    else if (strcmp(op, "OP_MOD") == 0) return OP_MOD;
    else if (strcmp(op, "OP_RET") == 0) return OP_RET;
    else if (strcmp(op, "OP_CALL") == 0) return OP_CALL;
    else if (strcmp(op, "OP_JMP") == 0) return OP_JMP;
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
    else if (strcmp(op, "OP_IPUSH") == 0) return OP_IPUSH;
    else if (strcmp(op, "OP_ISTORE") == 0) return OP_ISTORE;
    else if (strcmp(op, "OP_GOTO") == 0) return OP_GOTO;
    else if (strcmp(op, "OP_IADD") == 0) return OP_IADD;
    else if (strcmp(op, "OP_IFICMPLT") == 0) return OP_IFICMPLT;
    else if (strcmp(op, "OP_IFICMPGT") == 0) return OP_IFICMPGT;
    else if (strcmp(op, "OP_IRETURN") == 0) return OP_IRETURN;
    else if (strcmp(op, "OP_IINC") == 0) return OP_IINC;
    else if (strcmp(op, "OP_ILOAD") == 0) return OP_ILOAD;
    else if (strcmp(op, "OP_ARGSTORE") == 0) return OP_ARGSTORE;
    else if (strcmp(op, "OP_IFICMPGTX") == 0) return OP_IFICMPGTX;
    else if (strcmp(op, "OP_IADDX") == 0) return OP_IADDX;
    

    
    else return OP_UNKNOWN; // Default or error value
}

tua_bytecode parse_line(char *line) {
    char arg1[32];
    char arg2[32];
    char arg3[32];
    tua_bytecode bc = {0};
    char op[32];

    sscanf(line, "%s %s %s %s", op, arg1, arg2, arg3);
    // printf("parse line %s %s %s %s\n", op,  arg1, arg2, arg3);
    if(arg1[0] == 'R') {
        int r = arg1[1]-'a'+1;
        bc.arg1 = r;
    }else if (arg1[0] == '$') {
        bc.arg1 = atoi(arg1+5);
    } else {
        bc.arg1 = atoi(arg1);
    }
    
    if(arg2[0] == 'R') {
        int r = arg2[1]-'a'+1;
        bc.arg2 = r;
    }else if (arg2[0] == '$') {
        bc.arg2 = atoi(arg2+5);
    } else {
        bc.arg2 = atoi(arg2);
    }
    
    if(arg3[0] == 'R') {
        int r = arg3[1]-'a'+1;
        bc.arg3 = r;
    }else if (arg3[0] == '$') {
        bc.arg3 = atoi(arg3+5);
    } else {
        bc.arg3 = atoi(arg3);
    }
    if (arg3[0] == ';') {
        bc.arg3 = 0;
    }
    
    
   


    tua_instruction i = 0;

    OpCode opcode = str_to_opcode(op);
    bc.opcode = opcode;

        SET_OPCODE(i, opcode);
        SETARG_A(i, bc.arg1);
        SETARG_B(i, bc.arg2);
        SETARG_C(i, bc.arg3);
    // printf("opcode %d\n", opcode);
    // if (strcmp(op, "OP_MOV") == 0) {
    //     bc.opcode = OP_MOV;
    //     SET_OPCODE(i, OP_MOV);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_B(i, bc.arg2);
    // } else if (strcmp(op, "OP_ADD") == 0) {
    //     bc.opcode = OP_ADD;
    //     SET_OPCODE(i, OP_ADD);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_B(i, bc.arg2);
    //     SETARG_C(i, bc.arg3);
    // } else if (strcmp(op, "OP_SUB") == 0) {
    //     bc.opcode = OP_SUB;
    // } else if (strcmp(op, "OP_MUL") == 0) {
    //     bc.opcode = OP_MUL;
    //     SET_OPCODE(i, OP_MUL);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_B(i, bc.arg2);
    //     SETARG_C(i, bc.arg3);
    // } else if (strcmp(op, "OP_DIV") == 0) {
    //     bc.opcode = OP_DIV;
    // } else if (strcmp(op, "OP_MOD") == 0) {
    //     bc.opcode = OP_MOD;
    // } else if (strcmp(op, "OP_RET") == 0) {
    //     bc.opcode = OP_RET;
    //     SET_OPCODE(i, OP_RET);
    //     SETARG_A(i, bc.arg1);
    // } else if (strcmp(op, "OP_JZ") == 0) {
    //     bc.opcode = OP_JZ;
    // } else if (strcmp(op, "OP_JNZ") == 0) {
    //     bc.opcode = OP_JNZ;
    // } else if (strcmp(op, "OP_JLT") == 0) {
    //     bc.opcode = OP_JLT;
    // } else if (strcmp(op, "OP_JLE") == 0) {
    //     bc.opcode = OP_JLE;
    // } else if (strcmp(op, "OP_JGT") == 0) {
    //     bc.opcode = OP_JGT;
    // } else if (strcmp(op, "OP_JGE") == 0) {
    //     bc.opcode = OP_JGE;
    // } else if (strcmp(op,"OP_LABEL") == 0) {
    //     bc.opcode = OP_LABEL;
    // } else if (strcmp(op,"OP_CALL") == 0) {
    //     bc.opcode = OP_CALL;
    // } else if (strcmp(op,"OP_JMP") == 0) {
    //     bc.opcode = OP_JMP;
    // } else if (strcmp(op,"OP_LOADI") == 0) {
    //     bc.opcode = OP_LOADI;
    //     SET_OPCODE(i, OP_LOADI);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_k(i, bc.arg2);
    // } else if (strcmp(op,"OP_LOADF") == 0) {
    //     bc.opcode = OP_LOADF;
    // } else if (strcmp(op,"OP_LOADS") == 0) {
    //     bc.opcode = OP_LOADS;
    // } else if (strcmp(op,"OP_LOADA") == 0) {
    //     bc.opcode = OP_LOADA;
    //     SET_OPCODE(i, OP_LOADA);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_k(i, bc.arg2);
    // } else if (strcmp(op,"OP_FORPREP") == 0) {
    //     bc.opcode = OP_FORPREP;
    //     SET_OPCODE(i, OP_FORPREP);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_B(i, bc.arg2);
    //     SETARG_C(i, bc.arg3);
    // } else if (strcmp(op,"OP_FORLOOP") == 0) {
    //     bc.opcode = OP_FORLOOP;
    //     SET_OPCODE(i, OP_FORLOOP);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_Bx(i, bc.arg2);
    // } else if (strcmp(op,"OP_MULI") == 0) {
    //     bc.opcode = OP_MULI;
    // } else if (strcmp(op,"OP_ADDI") == 0) {
    //     bc.opcode = OP_ADDI;
    // } else if (strcmp(op,"OP_SUBI") == 0) {
    //     bc.opcode = OP_SUBI;
    // } else if (strcmp(op,"OP_DIVI") == 0) {
    //     bc.opcode = OP_DIVI;
    // } else if (strcmp(op,"OP_MODI") == 0) {
    //     bc.opcode = OP_MODI;
    // } 
    // else if (strcmp(op,"OP_ISTORE") == 0) {
    //     /* code */
    //     SET_OPCODE(i, OP_ISTORE);
    //     SETARG_A(i, bc.arg1);
    // }
    // else if (strcmp(op,"OP_GOTO") == 0) {
    //     /* code */
    //     SET_OPCODE(i, OP_GOTO);
    //     SETARG_A(i, bc.arg1);
    // }
    // else if (strcmp(op,"OP_IFICMPLT") == 0) {
    //     /* code */
    //     SET_OPCODE(i, OP_IFICMPLT);
    //     SETARG_A(i, bc.arg1);
    // }
    // else if (strcmp(op,"OP_IFICMPGT") == 0)
    // {
    //     /* code */
    //     SET_OPCODE(i, OP_IFICMPGT);
    //     SETARG_A(i, bc.arg1);
    // }else if (strcmp(op,"OP_IRETURN") == 0)
    // {
    //     /* code */
    //     SET_OPCODE(i, OP_IRETURN);
    //     SETARG_A(i, bc.arg1);
    //     /* code */
    // }else if (strcmp(op,"OP_ILOAD") == 0)
    // {
    //     /* code */
    //     SET_OPCODE(i, OP_ILOAD);
    //     SETARG_A(i, bc.arg1);
    //     SETARG_k(i, bc.arg2);
    // }else if(strcmp(op,"OP_IINC") == 0) {
    //     SET_OPCODE(i, OP_IINC);
    //     SETARG_A(i, bc.arg1);
    // }else if(strcmp(op,"OP_IPUSH") == 0) {
    //     SET_OPCODE(i, OP_IPUSH);
    //     SETARG_A(i, bc.arg1);
    // }else if (strcmp(op,"OP_INIT") == 0)
    // {
    //     SET_OPCODE(i, OP_INIT);
    // }else if (strcmp(op,"OP_IADD") == 0)
    // {
    //     SET_OPCODE(i, OP_IADD);
    // }
    
    
       
    // else {
    //     fprintf(stderr, "Unknown instruction: %s\n", op);
    //     exit(EXIT_FAILURE);
    // }

    
    bc.i = i;
    return bc;
}


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

void print_bytecodes(tua_bytecode *bytecode, int num_bytecode) {
    for (int i = 0; i < num_bytecode; i++) {
        tua_bytecode instr = bytecode[i];
        printf("%03d: %s %d %d %d\n", i, opcode_tostr(instr.opcode), instr.arg1, instr.arg2, instr.arg3);
    }
}

int run_tua0(const char * filename, int *argv, int argc) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    char line[256];
    tua_bytecode bytecodes[256];
    int num_bytecode = 0;
    int labels[256] = {0};
    tua_instruction instructions[256] = {0};

    while (fgets(line, sizeof(line), file)) {
        if(line[0] == ';') {
            continue;
        }
        tua_bytecode bytecode = parse_line(line);
        if(bytecode.opcode == OP_LABEL) {
            int pos = bytecode.arg1;
            printf("parse label %d\n", pos);
            labels[pos] = num_bytecode;
        }
        bytecodes[num_bytecode] = bytecode;

        instructions[num_bytecode] = bytecode.i;
        num_bytecode++;
    }
    fclose(file);
    time_t s = time(NULL);
struct timeval stop, start;
  //do stuff

    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    t->savedpc = instructions;
    // print_bytecodes(bytecodes, num_bytecode);

//   gettimeofday(&start, NULL);

    for (int i = 0; i < num_bytecode; i++)
    {
        /* code */
        // printf("0x%04X, %d\n", bytecodes[i].i,bytecodes[i].i);   
    }
    
    int ret;
    ret = tua_execute0(t, bytecodes, num_bytecode, labels, argv);
//   gettimeofday(&stop, NULL);

    tua_free(t);

    time_t e = time(NULL);
    // printf("result: %s time: %fs",filename, (float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    // print_array(argv, argc);
    return ret;
}

int run_tua1(const char * filename, int *argv, int argc) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    char line[256];
    tua_bytecode bytecodes[256];
    int num_bytecode = 0;
    int labels[256] = {0};
    tua_instruction instructions[256] = {0};

    while (fgets(line, sizeof(line), file)) {
        if(line[0] == ';') {
            continue;
        }
        tua_bytecode bytecode = parse_line(line);
        if(bytecode.opcode == OP_LABEL) {
            int pos = bytecode.arg1;
            printf("parse label %d\n", pos);
            labels[pos] = num_bytecode;
        }
        bytecodes[num_bytecode] = bytecode;

        instructions[num_bytecode] = bytecode.i;
        num_bytecode++;
    }
    fclose(file);
    time_t s = time(NULL);
struct timeval stop, start;
  //do stuff

    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    t->savedpc = instructions;
    // print_bytecodes(bytecodes, num_bytecode);

//   gettimeofday(&start, NULL);

    for (int i = 0; i < num_bytecode; i++)
    {
        /* code */
        // printf("0x%04X, %d\n", bytecodes[i].i,bytecodes[i].i);   
    }
    
    int ret;
    ret = tua_execute(t, bytecodes, num_bytecode, labels, argv);
//   gettimeofday(&stop, NULL);

    tua_free(t);

    time_t e = time(NULL);
    // printf("result: %s time: %fs",filename, (float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    // print_array(argv, argc);
    return ret;
}

int fib(int n) {
    int a = 0;
    for (int i = n; i > 0; i--) {
        a = a + i;
    }
    return a;
}


int tua_execute2(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv) {
    tua_instruction *pc = 0;
    int cnt = 0;
    pc = t->savedpc;
    // int *base = t->array;
    tua_stack base_stack;
    tua_stack *st = &base_stack;
    INIT_STACK(st);
    // NEW_STACK(st, 32);
    // int *vars = malloc(sizeof(int)*32);
    //使用union存储变量
    tua_instruction *savedpc = t->savedpc;
    tua_value *base = base_stack.value;
    tua_value *vars = base_stack.value;
    int n = argv[0];
    printf("tua_execute2:n= %d\n", argv[0]);
    int steps[1] = {0};
    steps[0] = 1;
    int a = 1,b=0,c=0;
    for(;;) {
        cnt++;
        tua_instruction i = *pc;
        // printf("OPCode %d\n", GET_OPCODE(i));
        switch (GET_OPCODE(i))  
        {
            case OP_LOADA:
            {
                // a = argv[k];
                // VARS(GETARG_A(i)) = argv[GETARG_B(i)];
                VARS(3) = argv[0];
                pc++;
                // printf("OP_LOADA %d %d\n", a, k);
                break;
            }
            case OP_ADD:
            {
                // a = a + k;
                VARS(GETARG_A(i)) = VARS(GETARG_B(i)) + VARS(GETARG_C(i));
                // VARS(1) = VARS(1) + VARS(3);
                // asm("addl %1, %2;movl %1, %0\n" : "=r"(VARS(GETARG_A(i))) : "r"(VARS(GETARG_B(i))), "r"(VARS(GETARG_C(i))):);
                // asm("addl %1, %2;movl %1, %0\n" : "=r"(a) : "r"(a), "r"(cnt):);
                // asm("addl %1, %2;movl %1, %0\n" : "=r"(VARS(1)) : "r"(VARS(1)), "r"(VARS(3)):);
                
                pc++;
                // printf("OP_ADD %d %d %d\n", a, k, n);
                break;
            }
            case OP_FORLOOP:
                {
                    if(VARS(GETARG_A(i))>0) {
                        // pc-=GETARG_B(i);//constant
                        pc -= steps[0];
                        VARS(GETARG_A(i))--;
                    }else {
                        pc++;
                    }
                    // if(VARS(3)>0) {
                    //     pc-=steps[0];//constant
                    //     VARS(3)--;
                    // }else {
                    //     pc++;
                    // }
                    break;
                }
            case OP_RET:
                {
                    int v = VARS(GETARG_A(i));
                    printf("OP_RET %d cnt:%d\n", v, cnt);
                    return v;
                }
            default:
                pc++;
                break;
        }
       
    }
}

int run_tua2(const char * file, int * argv, int argc) {
    int opcodes[] = {
        0x0091,
0x0194,
0x0111,
0x3020195,
0x3010082,
0x10196,
0x0087
    };
    tua_bytecode bytecodes[256];
    int num = sizeof(opcodes)/sizeof(int);
    tua_instruction instructions[256] = {0};

    for(int i = 0; i < num; i++) {
        bytecodes[i].i = opcodes[i];
        instructions[i] = opcodes[i];
    }
    for (int i = 0; i < num; i++)
    {
        /* code */
        // printf("0x%04X, %d\n", bytecodes[i].i,bytecodes[i].i);   
    }

    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    t->savedpc = instructions;
    int labels[256] = {0};
    int ret = tua_execute2(t, bytecodes, num, labels, argv);
    tua_free(t);
    return ret;

}

int tua_execute3(int n){
    int a = 0;
    OpCode op = OP_ADD;
    int k = 0;
    for (int i = 0; i < n; i++) {
        a = a + 1;
        while(1) {
            switch (op)
            {
            case OP_ADD:
                /* code */
                op = OP_FORLOOP;
                break;
            case OP_FORLOOP:
                op = OP_ADD;
                goto ext;
                break;
            default:
                break;
            }
        }
        ext:
        ;
    }
    printf("loop k:%d\n", k);
    return a;
}

int run_tua3(const char * file, int * argv, int argc) {
    int opcodes[] = {
        0x0091,
    };
    return tua_execute3(argv[0]);
}

int run_tua4(const char * file, int * argv, int argc) {
    int a= 0;
    int k=0;
    for (int i = 0; i <= argv[0]; i++,k++) {
        a = a + i;
    }
    printf("loop k:%d %d\n", k, a);
    return a;
}
struct timeval stop, start;

void test0(const char *argv[]) {
    int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua0(argv[1], (int[]){v}, 1));
    gettimeofday(&stop, NULL);
    printf("====result0: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}

void test1(const char *argv[]) {
    int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua1(argv[1], (int[]){v}, 1));
    gettimeofday(&stop, NULL);
    printf("====result1: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}

void test2(const char *argv[]) {
    int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua2(argv[1], (int[]){v}, 1));
    gettimeofday(&stop, NULL);
    printf("====result2: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}

void test3(const char *argv[]) {
    int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua3(argv[1], (int[]){v}, 1));
    gettimeofday(&stop, NULL);
    printf("====result3: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}
void test4(const char *argv[]) {
    int v = atoi(argv[2]);
    gettimeofday(&start, NULL);
    assert_equal(0, run_tua4(argv[1], (int[]){v}, 1));
    gettimeofday(&stop, NULL);
    printf("====result4: time: %fs\n",(float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
}
int main(int argc, const char *argv[]) {
    if(argc != 3) {
        printf("Usage: tua <file> <number1>\n");
        return 1;
    }
    init_bytecodefunc();
    // assert_equal(8, run_tua("test1.tvm", (int[]){v}, 1));
    
    test0(argv);
    test1(argv);
    test2(argv);
    test3(argv);
    test4(argv);
    
    return 0;
}

