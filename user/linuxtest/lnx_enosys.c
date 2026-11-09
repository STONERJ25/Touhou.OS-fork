/*
 * Calls that the Linux personality does not implement answer ENOSYS, and the kernel names each one once in its log. inotify_init (253) and a number far beyond the table
 * are called twice each; the log must show the first as "not implemented" exactly once and the second kind as "does not exist".
 */
#include "lnx.h"

void lnx_main(long *sp) {
    (void)sp;
    lnx_line("inotify_init", lnx_sys0(253));
    lnx_line("inotify_init again", lnx_sys0(253));
    lnx_line("a number that is not a system call", lnx_sys0(5000));
    lnx_exit(0);
}
