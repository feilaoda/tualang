#include "tua.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
    // printf("get %d=%d\n", n, v);
    return v;
}

void add_op(tua_state *t, int pos1, int pos2, int pos3) {
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

int parse_int(const char *s) {
    int n = 0;
    while (*s) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

void call_op(tua_state *t, int op, int pos1, int pos2, int pos3) {
    switch (op) {
        case OP_ADD:
            add_op(t, pos1, pos2, pos3);
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


int RA(char i) {
    int r = i-'a'+1;
    printf("RA %d=%d\n", i,r);
    return i;
}
int REG(char i) {
    int r = i-'a'+1;
    // printf("get r:%d\n", r);
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

void execute_vm_code(tua_instruction *instructions, int num_instructions, int *argv) {
    int registers[256] = {0};

    for (int i = 0; i < num_instructions; i++) {
        tua_instruction instr = instructions[i];

        switch (instr.opcode) {
            case OP_MOV:
                if (instr.arg2[0] == '$') {
                    int index = atoi(&instr.arg2[5]);
                    registers[instr.arg1[1]] = argv[index];
                } else {
                    registers[instr.arg1[1]] = atoi(instr.arg2);
                }
                break;
            case OP_SUB:
                registers[instr.arg1[1]] = registers[instr.arg2[1]] - registers[instr.arg3[1]];
                break;
            case OP_RET:
                printf("Result: %d\n", registers[instr.arg1[1]]);
                return;
            default:
                fprintf(stderr, "Unknown opcode: %d\n", instr.opcode);
                exit(EXIT_FAILURE);
        }
    }
}
int tua_execute(tua_state *t, tua_instruction* instructions, int num_instructions, int *argv) {
    int i = 0;
    // printf("line %d %d", i, code[0]);
    for (int i = 0; i < num_instructions; i++) {
        tua_instruction instr = instructions[i];
        switch (instr.opcode)
        {
        case OP_MOV:
            /* code */
            {
                if (instr.arg2[0] == '$') {
                    int index = atoi(&instr.arg2[5]);
                    int r = REG(instr.arg1[1]);
                    tua_setnumber(t, r, argv[index]);

                } else {
                    int r = REG(instr.arg1[1]);
                    tua_setnumber(t, r, atoi(instr.arg2));
                }
           
            break;
            }
        case OP_RET:
        {
            int r = REG(instr.arg1[1]);
            return tua_toint(t,r);
        }
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        {
            int pos1 = REG(instr.arg1[1]);
            int pos2 = REG(instr.arg2[1]);
            int pos3 = REG(instr.arg3[1]);
            call_op(t, instr.opcode, pos1, pos2, pos3);
            break;
        }
        default:
            break;
        }
    }
    exit(-1);
    return 0;
}

int OP(char op) {
    if(op == '+') {
        return OP_ADD;
    } else if(op == '-') {
        return OP_SUB;
    } else if(op == '*') {
        return OP_MUL;
    } else if(op == '/') {
        return OP_DIV;
    } else if(op == '%') {
        return OP_MOD;
    } return 0;
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
    tua_instruction instr;
    char op[10];
    sscanf(line, "%s %s %s %s", op, instr.arg1, instr.arg2, instr.arg3);

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
    } else {
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
    printf("]");
}

int run_tua(char * filename, int *argv, int argc) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open file");
        exit(EXIT_FAILURE);
    }
    char line[256];
    tua_instruction instructions[10];
    int num_instructions = 0;

    while (fgets(line, sizeof(line), file)) {
        instructions[num_instructions++] = parse_line(line);
    }
    fclose(file);
    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    int ret = tua_execute(t, instructions, num_instructions, argv);
    tua_free(t);
    printf("result: %s ",filename);
    print_array(argv, argc);
    return ret;
}

int main(int argc, const char *argv[]) {
    assert_equal(8, run_tua("test1.tvm", (int[]){5, 3}, 2));
    assert_equal(2, run_tua("test1.tvm", (int[]){5, -3}, 2));
    assert_equal(-2, run_tua("test1.tvm", (int[]){-5, 3}, 2));
    assert_equal(-8, run_tua("test1.tvm", (int[]){-5, -3}, 2));
    assert_equal(0, run_tua("test1.tvm", (int[]){0, 0}, 2));

    assert_equal(2, run_tua("test2.tvm", (int[]){5, 3}, 2));
    assert_equal(8, run_tua("test2.tvm", (int[]){5, -3}, 2));
    assert_equal(-8, run_tua("test2.tvm", (int[]){-5, 3}, 2));
    assert_equal(-2, run_tua("test2.tvm", (int[]){-5, -3}, 2));
    assert_equal(0, run_tua("test2.tvm", (int[]){0, 0}, 2));
   
    return 0;
}

