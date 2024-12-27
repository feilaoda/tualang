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
    printf("left:%d, right:%d\n", left, right);
    tua_pushint(t, left + right);
}

int parse_int(const char *s) {
    int n = 0;
    while (*s) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

int main(int argc, char const *argv[]) {
    if(argc != 3) {
        printf("Usage: tua <number1> <number2>\n");
        return 1;
    }
    tua_state *t = tua_newstate(TUA_STACK_SIZE);
    int a = parse_int(argv[1]);
    int b = parse_int(argv[2]);
    tua_pushint(t, a);
    tua_pushint(t, b);
    add_op(t);
    int result = tua_toint(t, -1);
    printf("result:%d + %d = %d\n", a, b, result);
    tua_free(t);
    return 0;
}