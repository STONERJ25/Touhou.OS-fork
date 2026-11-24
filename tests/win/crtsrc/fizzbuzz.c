/* main(argc, argv) through the mini C runtime: FizzBuzz up to argv[1] (15 without one), and printf's formats in a table. */
#include "crt.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 15;
    for (int i = 1; i <= n; i++) {
        if (i % 15 == 0) puts("FizzBuzz");
        else if (i % 3 == 0) puts("Fizz");
        else if (i % 5 == 0) puts("Buzz");
        else printf("%d\n", i);
    }
    printf("[%5d][%-5d][%05d][%x][%X][%o][%c][%s][%.3s][%8s][%-8s|]\n", 42, 42, 42, 255, 255, 8, 'k', "str", "abcdef", "right", "left");
    printf("[%ld][%lld][%zu][%u][%i][%+d][%%][%#x][%3c]\n", -7L, -123456789012345LL, (size_t)99, 4000000000u, -5, 3, 0xbeef, 'z');
    printf("[%08.3d][%-+6d][%.0d][%p]\n", 7, 9, 0, (void *)0x1234);
    return 0;
}
