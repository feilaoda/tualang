#include <stdio.h>
#include <stdlib.h>

//gcc -o /tmp/for_loop_c -O3  tests/perf/for_loop.c 


int loop(int u, int r, int times) {
    int a[100000];
    for (int i = 0; i < times; i++) {
        a[i] = 1;
    }
    int res = 0;
    for (int i = 0; i < times; i++) {
        int acc = a[i];
        for (int j = 0; j < times; j++) {
            acc = acc + j / u;
        }
        a[i] = acc + r;
        res += a[i];
    }
    return res;
}
int main(int argc, char **argv) {
    int u = atoi(argv[1]);
    int r = atoi(argv[2]);
    int times = atoi(argv[3]);
    printf("%d\n", loop(u, r, times));
    return 0;
}
