/* Handlers of the Linux system call table that live outside kernel/linux/syscall.c (docs/COMPAT.md). */
#ifndef K_LINUX_CALLS_H
#define K_LINUX_CALLS_H

#include "linux.h"

/* kernel/linux/signal.c */
long lnx_rt_sigaction(TrapFrame *tf);
long lnx_rt_sigprocmask(TrapFrame *tf);
long lnx_rt_sigpending(TrapFrame *tf);
long lnx_rt_sigsuspend(TrapFrame *tf);
long lnx_rt_sigtimedwait(TrapFrame *tf);
long lnx_sigaltstack(TrapFrame *tf);
long lnx_tkill(TrapFrame *tf);
long lnx_rt_sigreturn(TrapFrame *tf);

#endif
