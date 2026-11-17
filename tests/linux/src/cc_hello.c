/*
 * The smallest program a C compiler is asked for (tests/case_linux_py.py, linux_py_gcc_*): a header, a call into the C library, an exit status. The guest compiles it with the gcc of
 * the Linux root, the oracle (tools/linux-oracle.py) with the gcc of the machine the root was made from, and the object files must be the same bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("hello from a program compiled in the Linux root: %d arguments\n", argc - 1);
    for (int i = 1; i < argc; i++)
        printf("argument %d: %s (%zu bytes)\n", i, argv[i], strlen(argv[i]));
    return argc > 99 ? EXIT_FAILURE : 0;
}
