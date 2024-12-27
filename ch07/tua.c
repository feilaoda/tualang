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


char * opcode_tostr(int opcode) {
    switch (opcode)
    {
    case OP_MOV:
        return "OP_MOV";
    case OP_ADD:
        return "OP_ADD";
    case OP_SUB:
        return "OP_SUB";
    case OP_MUL:
        return "OP_MUL";
    case OP_DIV:
        return "OP_DIV";
    case OP_MOD:
        return "OP_MOD";
    case OP_RET:
        return "OP_RET";
    case OP_CALL:
        return "OP_CALL";
    case OP_JMP:
        return "OP_JMP";
    case OP_JZ:
        return "OP_JZ";
    case OP_JNZ:
        return "OP_JNZ";
    case OP_JLT:
        return "OP_JLT";
    case OP_JLE:
        return "OP_JLE";
    case OP_JGT:
        return "OP_JGT";
    case OP_JGE:
        return "OP_JGE";
    case OP_LABEL:
        return "OP_LABEL";
    case OP_LOADI:
        return "OP_LOADI";
    case OP_LOADF:
        return "OP_LOADF";
    case OP_LOADS:
        return "OP_LOADS";
    case OP_LOADA:
        return "OP_LOADA";
    case OP_FORPREP:
        return "OP_FORPREP";
    case OP_FORLOOP:
        return "OP_FORLOOP";
    case OP_MULI:
        return "OP_MULI";
    default:
        return "UNKNOWN";
    }
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

// int tua_execute1(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv)  {
//     int pc = 0;
//     int cnt = 0;
//     t->pc = &pc;
//     t->argv = argv;
//     while (pc < num_bytecodes) {
//         cnt ++;

//         tua_bytecode bytecode = bytecodes[*t->pc];
//         // printf("pc:%d/%d cnt:%d op:%s\n", pc, *t->pc, cnt, opcode_tostr(bytecode.opcode));

//         void (*func)(tua_state *, tua_bytecode *) = global_func[bytecode.opcode];
//         func(t, &bytecode);
//         // printf("func:%p %d pc=%d/%d\n", func, bytecode.opcode, pc, *t->pc);
//     }
//     printf("return %d loop cnt:%d\n", t->ret, cnt);
//     return t->ret;
// }
// int tua_execute2(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv) {
//     int pc = 0;
//     int cnt = 0;
//     t->pc = &pc;
//     // printf("line %d %d", i, code[0]);
//     while (pc < num_bytecodes) {
//         cnt ++;
//         tua_bytecode bytecode = bytecodes[pc];
//         // printf("CNT:%d op:%s pc:%d\n",cnt, opcode_tostr(instr.opcode), pc);
//         switch (bytecode.opcode)
//         {
//             case OP_LOADI:
//             {
//                 int r = bytecode.arg1;
//                 tua_setnumber(t, r, bytecode.arg2);
//                 // printf("OP_LOADI %c=%d\n", RGSTR(r), bytecode.arg2);
//                 pc++;
//                 break;
//             }
//             case OP_LOADA:
//             {
//                 int r = bytecode.arg1;
//                 int index = bytecode.arg2;
//                 int v = argv[index];
//                 tua_setnumber(t, r, v);
//                 // printf("OP_LOADA %c=%d\n", RGSTR(r), v);
//                 pc++;
//                 break;
//             }
//         case OP_MOV:
//             /* code */
//             {
//                 // printf("call before op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode), RGSTR(r),tua_toint(t,r), v);
//                 tua_setnumber(t, bytecode.arg1, bytecode.arg2);
//                 // printf("call after op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode),  RGSTR(r),tua_toint(t,r), v);
//                 pc++;
//             break;
//             }
//         case OP_RET:
//         {
//             pc++;
//             int v = tua_toint(t,bytecode.arg1);
//             // printf("call op:%s %d=%d\n", opcode_tostr(instr.opcode), r,v);
//             return v;
//         }
//         case OP_ADD:
//         {
//             // add_op(t, bytecode.arg1, bytecode.arg2, bytecode.arg3);
//             // int * array = t->array;
//             int * arg1 = t->array + bytecode.arg1;
//             int * arg2 = t->array + bytecode.arg2;
//             int * arg3 = t->array + bytecode.arg3;

//             // int left = *arg2; //tua_toint(t,bytecode.arg2);
//             // int right = *arg3; // tua_toint(t, bytecode.arg3);
//             // *arg1 = left + right;// tua_setnumber(t, bytecode.arg1, left + right);

//             // int * arg1 = t->array + bytecode.arg1;
//             // int * arg2 = t->array + bytecode.arg2;
//             // int * arg3 = t->array + bytecode.arg3;

//             int left = t->array[bytecode.arg2]; //tua_toint(t,bytecode.arg2);
//             int right = t->array[bytecode.arg3]; // tua_toint(t, bytecode.arg3);
//             // t->array[bytecode.arg1] = left + right;// tua_setnumber(t, bytecode.arg1, left + right);
//             *arg1 = left + right;
//             pc++;
//             break;
//         }
//         case OP_SUB:
//         case OP_MUL:
//         case OP_DIV:
//         case OP_MOD:
//         {
//             // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(bytecode.opcode), bytecode.arg1+'a'-1,tua_toint(t,bytecode.arg1), bytecode.arg2+'a'-1,tua_toint(t,bytecode.arg2),  bytecode.arg3+'a'-1 ,  tua_toint(t,bytecode.arg3));
//             call_op(t, bytecode.opcode, bytecode.arg1, bytecode.arg2, bytecode.arg3);
//             pc++;
//             // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(bytecode.opcode), bytecode.arg1+'a'-1,tua_toint(t,bytecode.arg1), bytecode.arg2+'a'-1,tua_toint(t,bytecode.arg2),  bytecode.arg3+'a'-1 ,  tua_toint(t,bytecode.arg3));
//             break;
//         }
//         case OP_ADDI:
//         case OP_SUBI:
//         case OP_MULI:
//         case OP_DIVI:
//         case OP_MODI:
//         {

//             // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3);
//             // call_opi(t, bytecode.opcode, pos1, r2,r2==1?pos2:v2, r3,r3==1 ? pos3 : v3);
//             pc++;
//             // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3 );
//             break;
//         }
//         case OP_JZ:
//         {
//             int r = bytecode.arg1;
//             int v = tua_toint(t, r);
//             // printf("jz r:%d=%d\n",r, v);
//             if (v == 0) {
//                 int l = bytecode.arg2;
//                 int pos = labels[l];
//                 pc = pos+1;
//                 // printf("jump to %d, label:%d v:%d r:%d\n", pc, l,v,r);
//                 //jump to arg2
//             }else {
//                 pc++;
//             }
//             break;
//         }
//         case OP_FORPREP: 
//         {
//             // if(tua_toint(t, bytecode.arg1) >= tua_toint(t, bytecode.arg2)) {
//             //     pc++;
//             // }else {
//             //     pc+=bytecode.arg3;
//             // }

//             int count = tua_toint(t, bytecode.arg1) - tua_toint(t, bytecode.arg2);
//             tua_setnumber(t, bytecode.arg1, count);
//             if(count>0) {
//                 pc++;
//             }else {
//                 pc+=bytecode.arg3;
//             }
//             break;
//         }
//         case OP_FORLOOP: {
//             // int ret = op_forloop(t, bytecode.arg1, bytecode.arg2, bytecode.arg3);
//             // pc = pc - ret;
//             int * arg1 = t->array + bytecode.arg1;
//             int left = *arg1; //t->array[bytecode.arg1];// tua_toint(t, bytecode.arg1);
//                 if(left>0) {
//                     // printf("for loop true %c:%d>%c:%d\n", RGSTR(pos1),start,RGSTR(pos2), end);
//                     // t->array[bytecode.arg1] = left - 1; //tua_setnumber(t, bytecode.arg1, left - 1);
//                     // t->array[bytecode.arg1] = left - 1;
//                     *arg1 = left - 1;
//                     // op_subi(t, bytecode->arg1, bytecode->arg1, 1);
//                     pc-=bytecode.arg2;
//                 }
//                 else {
//                     // printf("for loop end %c:%d>%c/%d:%d\n", RGSTR(pos1),start,RGSTR(pos2),pos2, end);
//                     pc++;
//                 }

//             // printf("for loop next %d %d\n", pc, ret);
//             break;
//         }
//         case OP_JMP: {
//             int l = bytecode.arg1;
//             int pos = labels[l];
//             // printf("pos:%d jump to %d, label:%d\n", pc, pos+1, l);
//             pc = pos+1;
//             // if(cnt>70) return 0;
//             break;
//         }
//         case OP_LABEL:
//             pc++;
//             break;
//         default:
//             break;
//         }
//     }
//     exit(-1);
//     return 0;
// }
int tua_execute(tua_state *t, tua_bytecode* bytecodes, int num_bytecodes, int labels[], int *argv) {
    tua_instruction *pc = 0;
    int cnt = 0;
    pc = t->savedpc;
    int *base = t->array;
    // printf("line %d %d", i, code[0]);
    while (1) {
        tua_instruction i = *pc++;
        // printf("CNT:%d op:%s pc:%d\n",cnt, opcode_tostr(instr.opcode), pc);
        switch (GET_OPCODE(i))
        {
            case OP_LOADI:
            {
                int *ra = RA(i);
                int v = GETARG_k(i);
                *ra = GETARG_k(i);
                // printf("OP_LOADI %c=%d\n", RGSTR(GETARG_A(i)), v);
                break;
            }
            case OP_LOADA:
            {
                int *ra = RA(i);
                int k = GETARG_k(i);
                int v = argv[k];
                *ra = v;
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
            int *ra = RA(i);
            int v = *ra;
            printf("OP_RET %c=%d\n", RGSTR(GETARG_A(i)),v);
            return v;
        }
        case OP_ADD:
        {
            int *ra = RA(i);
            int *rb = RB(i);
            int *rc = RC(i);
            *ra = *rb + *rc;
            // printf("OP_ADD %c=%c+%c %d,%d,%d, step:%d\n", RGSTR(GETARG_A(i)), RGSTR(GETARG_B(i)), RGSTR(GETARG_C(i)), *ra, *rb, *rc, *pc);
            break;
        }
        case OP_SUB:
        case OP_MUL:{
            int *ra = RA(i);
            int *rb = RB(i);
            int *rc = RC(i);
            *ra = *rb * *rc;
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
            int *ra = RA(i);
            int *rb = RB(i);
            int count = *ra - *rb;
            // printf("OP_FORPREP %d %d count:%d k=%d\n", *ra, *rb, count, GETARG_C(i));
            *ra = count;
            if(*ra<=0) {
                int k = GETARG_C(i);
                pc+=k;
            }
            break;
        }
        case OP_FORLOOP: {
            int *ra = RA(i);
            int count = *ra;
            if(count>0) {
                *ra = count - 1;
                pc-=GETARG_Bx(i);
            }
            
            // printf("OP_FORLOOP %d cnt:%d, next:%d\n", *ra, count, GETARG_Bx(i));
            
            break;
        }
        case OP_JMP: {
            
            break;
        }
        case OP_LABEL:
            break;
        default:
            break;
        }
    }
    exit(-1);
    return 0;
}


void assert_equal(int a, int b) {
    if(a != b) {
        printf("assert failed %d != %d\n", a, b);
    }else {
        printf(" true\n");
    }
}

tua_bytecode parse_line(char *line) {
    char arg1[32];
    char arg2[32];
    char arg3[32];
    tua_bytecode bc = {0};
    char op[32];

    sscanf(line, "%s %s %s %s", op, arg1, arg2, arg3);
    printf("parse line %s %s %s %s\n", op,  arg1, arg2, arg3);

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
   


    tua_instruction i = 0;
    if (strcmp(op, "OP_MOV") == 0) {
        bc.opcode = OP_MOV;
        SET_OPCODE(i, OP_MOV);
        SETARG_A(i, bc.arg1);
        SETARG_B(i, bc.arg2);
    } else if (strcmp(op, "OP_ADD") == 0) {
        bc.opcode = OP_ADD;
        SET_OPCODE(i, OP_ADD);
        SETARG_A(i, bc.arg1);
        SETARG_B(i, bc.arg2);
        SETARG_C(i, bc.arg3);
    } else if (strcmp(op, "OP_SUB") == 0) {
        bc.opcode = OP_SUB;
    } else if (strcmp(op, "OP_MUL") == 0) {
        bc.opcode = OP_MUL;
        SET_OPCODE(i, OP_MUL);
        SETARG_A(i, bc.arg1);
        SETARG_B(i, bc.arg2);
        SETARG_C(i, bc.arg3);
    } else if (strcmp(op, "OP_DIV") == 0) {
        bc.opcode = OP_DIV;
    } else if (strcmp(op, "OP_MOD") == 0) {
        bc.opcode = OP_MOD;
    } else if (strcmp(op, "OP_RET") == 0) {
        bc.opcode = OP_RET;
        SET_OPCODE(i, OP_RET);
        SETARG_A(i, bc.arg1);
    } else if (strcmp(op, "OP_JZ") == 0) {
        bc.opcode = OP_JZ;
    } else if (strcmp(op, "OP_JNZ") == 0) {
        bc.opcode = OP_JNZ;
    } else if (strcmp(op, "OP_JLT") == 0) {
        bc.opcode = OP_JLT;
    } else if (strcmp(op, "OP_JLE") == 0) {
        bc.opcode = OP_JLE;
    } else if (strcmp(op, "OP_JGT") == 0) {
        bc.opcode = OP_JGT;
    } else if (strcmp(op, "OP_JGE") == 0) {
        bc.opcode = OP_JGE;
    } else if (strcmp(op,"OP_LABEL") == 0) {
        bc.opcode = OP_LABEL;
    } else if (strcmp(op,"OP_CALL") == 0) {
        bc.opcode = OP_CALL;
    } else if (strcmp(op,"OP_JMP") == 0) {
        bc.opcode = OP_JMP;
    } else if (strcmp(op,"OP_LOADI") == 0) {
        bc.opcode = OP_LOADI;
        SET_OPCODE(i, OP_LOADI);
        SETARG_A(i, bc.arg1);
        SETARG_k(i, bc.arg2);
    } else if (strcmp(op,"OP_LOADF") == 0) {
        bc.opcode = OP_LOADF;
    } else if (strcmp(op,"OP_LOADS") == 0) {
        bc.opcode = OP_LOADS;
    } else if (strcmp(op,"OP_LOADA") == 0) {
        bc.opcode = OP_LOADA;
        SET_OPCODE(i, OP_LOADA);
        SETARG_A(i, bc.arg1);
        SETARG_k(i, bc.arg2);
    } else if (strcmp(op,"OP_FORPREP") == 0) {
        bc.opcode = OP_FORPREP;
        SET_OPCODE(i, OP_FORPREP);
        SETARG_A(i, bc.arg1);
        SETARG_B(i, bc.arg2);
        SETARG_C(i, bc.arg3);
    } else if (strcmp(op,"OP_FORLOOP") == 0) {
        bc.opcode = OP_FORLOOP;
        SET_OPCODE(i, OP_FORLOOP);
        SETARG_A(i, bc.arg1);
        SETARG_Bx(i, bc.arg2);
    } else if (strcmp(op,"OP_MULI") == 0) {
        bc.opcode = OP_MULI;
    } else if (strcmp(op,"OP_ADDI") == 0) {
        bc.opcode = OP_ADDI;
    } else if (strcmp(op,"OP_SUBI") == 0) {
        bc.opcode = OP_SUBI;
    } else if (strcmp(op,"OP_DIVI") == 0) {
        bc.opcode = OP_DIVI;
    } else if (strcmp(op,"OP_MODI") == 0) {
        bc.opcode = OP_MODI;
    } else {
        fprintf(stderr, "Unknown instruction: %s\n", op);
        exit(EXIT_FAILURE);
    }

    
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

int run_tua(const char * filename, int *argv, int argc) {
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
  gettimeofday(&start, NULL);
  //do stuff

    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    t->savedpc = instructions;
    print_bytecodes(bytecodes, num_bytecode);

    
    int ret = tua_execute(t, bytecodes, num_bytecode, labels, argv);
    tua_free(t);
  gettimeofday(&stop, NULL);

    time_t e = time(NULL);
    printf("result: %s time: %fs",filename, (float)((stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec)/1000000.0);
    print_array(argv, argc);
    return ret;
}

int fib(int n) {
    int a = 0;
    for (int i = n; i > 0; i--) {
        a = a + i;
    }
    return a;
}

int main(int argc, const char *argv[]) {
    if(argc != 3) {
        printf("Usage: tua <filename> <number1>\n");
        return 1;
    }
    init_bytecodefunc();
    int v = atoi(argv[2]);
    // assert_equal(8, run_tua("test1.tvm", (int[]){v}, 1));
    assert_equal(0, run_tua(argv[1], (int[]){v}, 1));
    
    tua_instruction i = 0;
    SETARG_A(i, 10);
    printf("i:%d\n", GETARG_A(i));

    return 0;
}

