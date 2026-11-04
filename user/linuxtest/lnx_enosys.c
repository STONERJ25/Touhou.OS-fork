/*
 * Calls that the Linux personality does not implement answer ENOSYS, and the kernel names each one once in its log. socket (41) and a number far beyond the table
 * are called twice each; the log must show the first as "not implemented" exactly once and the second kind as "does not exist".
 */
#include "lnx.h"

void lnx_main(long *sp) {
    (void)sp;
    lnx_line("socket", lnx_sys3(41, 2, 1, 0));
    lnx_line("socket again", lnx_sys3(41, 2, 1, 0));
    lnx_line("a number that is not a system call", lnx_sys0(5000));
    lnx_exit(0);
}
