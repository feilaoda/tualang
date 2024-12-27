#include "tua.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>


char RGSTR(int i) {
    return i+'a'-1;
}

int RA(char i) {
    int r = i-'a'+1;
    // printf("RA %d=%d\n", i,r);
    return i;
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
    return t;
}

void tua_free(tua_state *t) {
    free(t->st->array);
    free(t->st);
    free(t);
}


void tua_setnumber(tua_state *t, int pos, int n) {
    tua_stack *st = t->st;
    st->array[pos] = n;
    // printf("set %d=%d\n", pos, n);
}

int tua_toint(tua_state *t, int n) {
    tua_stack *st = t->st;
    int v = st->array[n];
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
        // printf("for loop continue %c:%d>%c:%d\n", RGSTR(pos1),start,RGSTR(pos2), end);
        op_subi(t, pos1, pos1, 1);
        return step;
    }
    else {
        // printf("for loop end %d:%d<%d:%d\n", pos1,start,pos2, end);
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


int tua_execute(tua_state *t, tua_instruction* instructions, int num_instructions, int labels[], int *argv) {
    int pc = 0;
    int cnt = 0;
    // printf("line %d %d", i, code[0]);
    while (pc < num_instructions) {
        cnt ++;
        tua_instruction instr = instructions[pc];
        // printf("CNT:%d op:%s pc:%d\n",cnt, opcode_tostr(instr.opcode), pc);
        switch (instr.opcode)
        {
            case OP_LOADI:
            {
                int r = REG(instr.arg1[1]);
                int v = atoi(instr.arg2);
                tua_setnumber(t, r, v);
                pc++;
                break;
            }
            case OP_LOADA:
            {
                int r = REG(instr.arg1[1]);
                int v;
                if (instr.arg2[0] == '$') {
                    int index = atoi(&instr.arg2[5]);
                    v = argv[index];
                }else {
                    v = atoi(instr.arg2);
                }
                tua_setnumber(t, r, v);
                pc++;
                break;
            }
        case OP_MOV:
            /* code */
            {
                int r;
                int v;
                if (instr.arg2[0] == '$') {
                    int index = atoi(&instr.arg2[5]);
                    r = REG(instr.arg1[1]);
                    v = argv[index];
                } else if(instr.arg2[0] == 'R') {
                    r = REG(instr.arg1[1]);
                    int r2 = REG(instr.arg2[1]);
                    v = tua_toint(t,r2);
                }else{
                    r = REG(instr.arg1[1]);
                    v = atoi(instr.arg2);
                }

                // printf("call before op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode), RGSTR(r),tua_toint(t,r), v);

                tua_setnumber(t, r, v);

                // printf("call after op:%s R%c=%d v=%d\n", opcode_tostr(instr.opcode),  RGSTR(r),tua_toint(t,r), v);
                pc++;
            break;
            }
        case OP_RET:
        {
            int r = REG(instr.arg1[1]);
            pc++;
            int v = tua_toint(t,r);
            // printf("call op:%s %d=%d\n", opcode_tostr(instr.opcode), r,v);
            return v;
        }
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        {
            int pos1 = REG(instr.arg1[1]);
            int pos2 ;
            int pos3 ;

            if (instr.arg2[0] == 'R') {
                pos2 = REG(instr.arg2[1]);
            } else {
                
            }
            if (instr.arg3[0] == 'R') {
                pos3 = REG(instr.arg3[1]);
            } else {

            }

            // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3);
            call_op(t, instr.opcode, pos1, pos2 , pos3);
            pc++;
            // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3 );
            break;
        }
        case OP_ADDI:
        case OP_SUBI:
        case OP_MULI:
        case OP_DIVI:
        case OP_MODI:
        {
            int pos1 = REG(instr.arg1[1]);
            int pos2 ;
            int r2,r3,v2,v3;
            int pos3 ;

            if (instr.arg2[0] == 'R') {
                pos2 = REG(instr.arg2[1]);
                r2 = 1;
            } else {
                 v2 = atoi(instr.arg2);
                r2 = 0;
            }
            if (instr.arg3[0] == 'R') {
                pos3 = REG(instr.arg3[1]);
                r3 = 1;
                v3 = 0;
            } else {
                v3 = atoi(instr.arg3);
                r3 = 0;
            }

            // printf("call before op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3);
            call_opi(t, instr.opcode, pos1, r2,r2==1?pos2:v2, r3,r3==1 ? pos3 : v3);
            pc++;
            // printf("call after op:%s R%c=%d R%c=%d R%c=%d\n", opcode_tostr(instr.opcode), pos1+'a'-1,tua_toint(t,pos1), pos2+'a'-1,tua_toint(t,pos2), r3==1? pos3+'a'-1: '?',r3==1 ? tua_toint(t,pos3) : v3 );
            break;
        }
        case OP_JZ:
        {
            int r = REG(instr.arg1[1]);
            int v = tua_toint(t, r);
            // printf("jz r:%d=%d\n",r, v);
            if (v == 0) {
                int l = parse_int(instr.arg2+1);
                int pos = labels[l];
                pc = pos+1;
                // printf("jump to %d, label:%d v:%d r:%d\n", pc, l,v,r);
                //jump to arg2
            }else {
                pc++;
            }
            break;
        }
        case OP_FORPREP: 
        {
            int pos1 = REG(instr.arg1[1]);
            int pos2 = REG(instr.arg2[1]);
            int step = atoi(instr.arg3);
            if(tua_toint(t, pos1) >= tua_toint(t, pos2)) {
                pc++;
            }else {
                pc+=step;
            }
            break;
        }
        case OP_FORLOOP: {
            int pos1 = REG(instr.arg1[1]);
            int pos2 = REG(instr.arg2[1]);
            int step = atoi(instr.arg3);
            int ret = op_forloop(t, pos1, pos2, step);
            pc = pc - ret;
            // printf("for loop next %d %d\n", pc, ret);
            break;
        }
        case OP_JMP: {
            int l = parse_int(instr.arg1+1);
            int pos = labels[l];
            // printf("pos:%d jump to %d, label:%d\n", pc, pos+1, l);
            pc = pos+1;
            // if(cnt>70) return 0;
            break;
        }
        case OP_LABEL:
            pc++;
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
        exit(-1);
    }else {
        printf(" true\n");
    }
}

tua_instruction parse_line(char *line) {
    tua_instruction instr = {0};
    char op[32];

    sscanf(line, "%s %s %s %s", op, instr.arg1, instr.arg2, instr.arg3);
    printf("parse line %s %s %s %s\n", op, instr.arg1, instr.arg2, instr.arg3);

    if (strcmp(op, "OP_MOV") == 0) {
        instr.opcode = OP_MOV;
    } else if (strcmp(op, "OP_ADD") == 0) {
        instr.opcode = OP_ADD;
    } else if (strcmp(op, "OP_SUB") == 0) {
        instr.opcode = OP_SUB;
    } else if (strcmp(op, "OP_MUL") == 0) {
        instr.opcode = OP_MUL;
    } else if (strcmp(op, "OP_DIV") == 0) {
        instr.opcode = OP_DIV;
    } else if (strcmp(op, "OP_MOD") == 0) {
        instr.opcode = OP_MOD;
    } else if (strcmp(op, "OP_RET") == 0) {
        instr.opcode = OP_RET;
    } else if (strcmp(op, "OP_JZ") == 0) {
        instr.opcode = OP_JZ;
    } else if (strcmp(op, "OP_JNZ") == 0) {
        instr.opcode = OP_JNZ;
    } else if (strcmp(op, "OP_JLT") == 0) {
        instr.opcode = OP_JLT;
    } else if (strcmp(op, "OP_JLE") == 0) {
        instr.opcode = OP_JLE;
    } else if (strcmp(op, "OP_JGT") == 0) {
        instr.opcode = OP_JGT;
    } else if (strcmp(op, "OP_JGE") == 0) {
        instr.opcode = OP_JGE;
    } else if (strcmp(op,"OP_LABEL") == 0) {
        instr.opcode = OP_LABEL;
    } else if (strcmp(op,"OP_CALL") == 0) {
        instr.opcode = OP_CALL;
    } else if (strcmp(op,"OP_JMP") == 0) {
        instr.opcode = OP_JMP;
    } else if (strcmp(op,"OP_LOADI") == 0) {
        instr.opcode = OP_LOADI;
    } else if (strcmp(op,"OP_LOADF") == 0) {
        instr.opcode = OP_LOADF;
    } else if (strcmp(op,"OP_LOADS") == 0) {
        instr.opcode = OP_LOADS;
    } else if (strcmp(op,"OP_LOADA") == 0) {
        instr.opcode = OP_LOADA;
    } else if (strcmp(op,"OP_FORPREP") == 0) {
        instr.opcode = OP_FORPREP;
    } else if (strcmp(op,"OP_FORLOOP") == 0) {
        instr.opcode = OP_FORLOOP;
    } else if (strcmp(op,"OP_MULI") == 0) {
        instr.opcode = OP_MULI;
    } else if (strcmp(op,"OP_ADDI") == 0) {
        instr.opcode = OP_ADDI;
    } else if (strcmp(op,"OP_SUBI") == 0) {
        instr.opcode = OP_SUBI;
    } else if (strcmp(op,"OP_DIVI") == 0) {
        instr.opcode = OP_DIVI;
    } else if (strcmp(op,"OP_MODI") == 0) {
        instr.opcode = OP_MODI;
    }

        
    else {
        fprintf(stderr, "Unknown instruction: %s\n", op);
        exit(EXIT_FAILURE);
    }

    return instr;
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

void print_instructions(tua_instruction *instructions, int num_instructions) {
    for (int i = 0; i < num_instructions; i++) {
        tua_instruction instr = instructions[i];
        printf("%03d: %s %s %s %s\n", i, opcode_tostr(instr.opcode), instr.arg1, instr.arg2, instr.arg3);
    }
}

int run_tua(char * filename, int *argv, int argc) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    char line[256];
    tua_instruction instructions[256];
    int num_instructions = 0;
    int labels[256] = {0};

    while (fgets(line, sizeof(line), file)) {
        
        tua_instruction instr = parse_line(line);
        if(instr.opcode == OP_LABEL) {
            int pos = atoi(instr.arg1+1);
            printf("parse label %d\n", pos);
            labels[pos] = num_instructions;
        }
        instructions[num_instructions++] = instr;
    }
    fclose(file);
    tua_state *t = tua_newstate(TUA_STACK_SIZE);

    print_instructions(instructions, num_instructions);
    int ret = tua_execute(t, instructions, num_instructions, labels, argv);
    tua_free(t);
    printf("result: %s ",filename);
    print_array(argv, argc);
    return ret;
}

int fib(int n) {
    int a = 1;
    for (int i = n; i > 0; i--) {
        a = a + i;
    }
    return a;
}

int main(int argc, const char *argv[]) {
    if(argc != 2) {
        printf("Usage: tua <number1>\n");
        return 1;
    }
    int v = atoi(argv[1]);
    // assert_equal(8, run_tua("test1.tvm", (int[]){v}, 1));
    assert_equal(0, run_tua("test2.tvm", (int[]){v}, 1));
   
    return 0;
}

