/* The smallest Linux program: write, exit_group. The first checkpoint of docs/COMPAT.md (L0). */
#include "lnx.h"

void lnx_main(long *sp) {
    (void)sp;
    lnx_puts("hello from a Linux program\n");
    lnx_exit(0);
}
