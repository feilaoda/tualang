#include <stdio.h>
#include <stdlib.h>

int loop(int u, int r) {
    int a[100000];
    for (int i = 0; i < 100000; i++) {
        a[i] = 1;
    }
    int res = 0;
    for (int i = 0; i < 100000; i++) {
        int acc = a[i];
        for (int j = 0; j < 100000; j++) {
            acc = acc + j / u;
        }
        a[i] = acc + r;
        res += a[i];
    }
    return res;
}
int main(int argc, char **argv) {
    int u = atoi(argv[1]);
    printf("%d\n", loop(u, 1));
    return 0;
}
