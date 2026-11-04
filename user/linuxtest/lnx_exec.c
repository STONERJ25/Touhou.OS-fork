/*
 * execve from a Linux program: the personality is kept across exec (the program started is a Linux program too), and the errors are the ones execve(2) names.
 * With no argument it starts the hello program that lives beside it; with one, that program (also the way to point it at a copy on another path).
 */
#include "lnx.h"

void lnx_main(long *sp) {
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    char *none[] = { 0 };
    lnx_line("execve of a missing file", lnx_sys3(L_execve, "/no/such/program", none, envp));
    lnx_line("execve of a directory", lnx_sys3(L_execve, "/", none, envp));
    lnx_line("execve with a bad path pointer", lnx_sys3(L_execve, 1, none, envp));
    const char *target = argc > 1 ? argv[1] : "/ScarletDevilMansion/share/linux-tests/lnx_hello";
    char *args[] = { (char *)target, 0 };
    lnx_puts("starting the next program\n");
    long r = lnx_sys3(L_execve, target, args, envp);
    lnx_line("execve failed", r);
    lnx_exit(1);
}
