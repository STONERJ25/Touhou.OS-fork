/* The smallest program a C library makes of: printf and a return from main. Built with `gcc -static` on a Linux (tools/linux-oracle.py); L1 of docs/COMPAT.md. */
#include <stdio.h>

int main(void) {
    printf("hello from glibc\n");
    return 0;
}
