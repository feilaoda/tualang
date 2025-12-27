#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    int a = 1;
    int b = atoi(argv[1]);
    for(int i=0; i< b; i++) {
        a = a + i;
    }
    printf("%d\n",a);
    return 0;
}