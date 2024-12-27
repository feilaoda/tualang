#include "tua.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

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

int tua_execute(tua_state *t, int *codes[], int lines) {
    int i = 0;
    int *code = codes[i];
    // printf("line %d %d", i, code[0]);
    for(;;) {
        const int *code = codes[i];
        int op = code[0];
        // printf(" op:%d %c -> %d %d %d\n", op, OPSTR(op), code[1],  code[2], code[3]);
        switch (op)
        {
        case OP_MOV:
            /* code */
            {
            int r = REG(code[1]);
            tua_setnumber(t, r, code[2]);
            break;
            }
        case OP_RET:
        {
            int r = REG(code[1]);
            return tua_toint(t,r);
        }
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        {
            int pos1 = REG(code[1]);
            int pos2 = REG(code[2]);
            int pos3 = REG(code[3]);
            call_op(t, op, pos1, pos2, pos3);
            break;
        }
        default:
            break;
        }
        i++;
        if (i >= lines) 
        {
            break;
            /* code */
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


int test1(int a, char op, int b) {
    tua_state *t = tua_newstate(TUA_STACK_SIZE);

    int code1[] = {OP_MOV, 'b', a};
    int code2[] = {OP_MOV, 'c', b};
    int code3[] = {OP(op), 'a', 'b', 'c'};
    int code4[] = {OP_RET, 'a'};

    int *codes[] = {
        code1,
        code2,
        code3,
        code4
    };

    int result = tua_execute(t, codes, sizeof(codes));
    printf("result1: %d %c %d = %d", a, op, b, result);
    tua_free(t);
    return result;
}



int test2(int a, char op, int b, char op2, int c) {
    tua_state *t = tua_newstate(TUA_STACK_SIZE);
   
    int code1[] = {OP_MOV, 'b', a};
    int code2[] = {OP_MOV, 'c', b};
    int code3[] = {OP(op), 'a', 'b', 'c'};
    int code4[] = {OP_MOV, 'b', c};
    int code5[] = {OP(op2), 'a', 'a', 'b'};
    int code6[] = {OP_RET, 'a'};

    int *codes[] = {
        code1,
        code2,
        code3,
        code4,
        code5,
        code6
        
    };

    int result = tua_execute(t, codes, sizeof(codes));
    printf("result2: %d %c %d %c %d = %d", a,op, b, op2, c, result);
    tua_free(t);
    return result;
}

void assert_equal(int a, int b) {
    if(a != b) {
        printf("assert failed %d != %d\n", a, b);
        exit(-1);
    }else {
        printf(" true\n");
    }
}

int main(int argc, char const *argv[]) {
    assert_equal(8,test1(5, '+', 3));
    assert_equal(2,test1(5, '+', -3));
    assert_equal(-2,test1(-5, '+', 3));
    assert_equal(-7,test1(-10, '+', 3));

    assert_equal(10, test2(5, '+', 3, '+', 2));
    assert_equal(6,test2(5, '+', 3, '-', 2));
    assert_equal(0, test2(5, '-', 3, '-', 2));
    return 0;
}



//5 + 3
//OP_MOV rb 5
//OP_MOV rc 3
//OP_ADD ra rb rc
//OP_RET ra