#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fib(int n) {
    int a = 1;
    for (int i = n; i > 0; i--) {
        a = a * i;
    }
    return a;
}

int main(int argc, char *argv[]) {
    int n = atoi(argv[1]); // Example input
    int result = fib(n);
    printf("fib(%d) = %d\n", n, result);
    return 0;
}