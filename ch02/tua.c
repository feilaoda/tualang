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

void tua_pushint(tua_state *t, int n) {
    tua_stack *st = t->st;
    st->array[st->p] = n;
    st->p++;
}

int tua_toint(tua_state *t, int n) {
    tua_stack *st = t->st;
    int v = st->array[st->p + n];
    return v;
}


void add_op(tua_state *t) {
    int left = tua_toint(t,-2);
    int right = tua_toint(t, -1);
    tua_pushint(t, left + right);
}

void sub_op(tua_state *t) {
    int left = tua_toint(t,-2);
    int right = tua_toint(t, -1);
    tua_pushint(t, left - right);
}

void mul_op(tua_state *t) {
    int left = tua_toint(t,-2);
    int right = tua_toint(t, -1);
    tua_pushint(t, left * right);
}

void div_op(tua_state *t) {
    int left = tua_toint(t,-2);
    int right = tua_toint(t, -1);
    tua_pushint(t, left / right);
}

void mod_op(tua_state *t) {
    int left = tua_toint(t,-2);
    int right = tua_toint(t, -1);
    tua_pushint(t, left % right);
}

int parse_int(const char *s) {
    int n = 0;
    while (*s) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

void call_op(tua_state *t, char op) {
    switch (op) {
        case '+':
            add_op(t);
            break;
        case '-':
            sub_op(t);
            break;
        case '*':
            mul_op(t);
            break;
        case '/':
            div_op(t);
            break;
        case '%':
            mod_op(t);
            break;
        default:
            printf("Unknown operator\n");
            exit(-1);
            break;
    }
}

int tua_pcall(tua_state *t, void (*func)(tua_state*,char), char op, int a, int b) {
    tua_pushint(t, a);
    tua_pushint(t, b);
    func(t, op);
    return tua_toint(t, -1);
}

int main(int argc, char const *argv[]) {
    if(argc != 4) {
        printf("Usage: tua <number1> <op> <number2>\n");
        return 1;
    }
    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    int a = parse_int(argv[1]);
    char op = *argv[2];
    int b = parse_int(argv[3]);
    
    tua_pushint(t, a);
    tua_pushint(t, b);
    call_op(t, op);
    int result = tua_toint(t, -1);
    printf("result:%d %c %d = %d\n", a,op, b, result);

    int result2 = tua_pcall(t, call_op, op, a, b);
    printf("result2:%d %c %d = %d\n", a,op, b, result2);
    tua_free(t);
    return 0;
}

