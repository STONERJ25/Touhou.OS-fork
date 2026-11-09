/* Handlers of the Linux system call table that live outside kernel/linux/syscall.c (docs/COMPAT.md). */
#ifndef K_LINUX_CALLS_H
#define K_LINUX_CALLS_H

#include "linux.h"

/* kernel/linux/syscall.c: the directory an *at() call starts from, and the copy of its path (at_done releases both) */
long at_path(int dirfd, u64 upath, char **path, VNode **base, File **hold);
void at_done(char *path, File *hold);

/* kernel/sys/syscalls.c: the working directory becomes v (a reference taken over); abs is its normalized absolute path */
void process_set_cwd(Process *p, VNode *v, const char *abs);

/* kernel/linux/ioctl.c: ioctl, with the terminal attributes translated between Linux's struct termios and the native one, and the FIONBIO and close-on-exec requests */
long lnx_ioctl(TrapFrame *tf);

/* kernel/linux/mem.c */
long lnx_madvise(TrapFrame *tf);
long lnx_mremap(TrapFrame *tf);

/* kernel/linux/fsops.c */
long lnx_statfs(TrapFrame *tf);
long lnx_fstatfs(TrapFrame *tf);
long lnx_fchdir(TrapFrame *tf);
long lnx_fchmod(TrapFrame *tf);
long lnx_fchmodat(TrapFrame *tf);
long lnx_fchown(TrapFrame *tf);
long lnx_lchown(TrapFrame *tf);
long lnx_fchownat(TrapFrame *tf);
long lnx_utimensat(TrapFrame *tf);
long lnx_linkat(TrapFrame *tf);
long lnx_noxattr(TrapFrame *tf);
long lnx_hint(TrapFrame *tf);
long lnx_getpgrp(TrapFrame *tf);
long lnx_getsid(TrapFrame *tf);
long lnx_fcntl(TrapFrame *tf);
long lnx_open_via_proc(const char *abs, int flags);       /* open() of /dev/stdout, /proc/self/fd/N ...: a descriptor for the open file itself, or -ENOSYS (not such a path) */

/* kernel/linux/signal.c */
u64 lnx_sigset_to_kernel(u64 lset);         /* a Linux sigset_t (bit n-1 is signal n) as the kernel's signal set (kernel/include/siginfo.h) */
u64 lnx_sigset_to_linux(u64 kset);
long lnx_rt_sigaction(TrapFrame *tf);
long lnx_rt_sigprocmask(TrapFrame *tf);
long lnx_rt_sigpending(TrapFrame *tf);
long lnx_rt_sigsuspend(TrapFrame *tf);
long lnx_rt_sigtimedwait(TrapFrame *tf);
long lnx_sigaltstack(TrapFrame *tf);
long lnx_rt_sigreturn(TrapFrame *tf);

/* kernel/linux/net.c */
long lnx_socket(TrapFrame *tf);
long lnx_socketpair(TrapFrame *tf);
long lnx_accept4(TrapFrame *tf);
long lnx_sendmsg(TrapFrame *tf);
long lnx_recvmsg(TrapFrame *tf);
long lnx_sendmmsg(TrapFrame *tf);
long lnx_recvmmsg(TrapFrame *tf);
long lnx_sendfile(TrapFrame *tf);

/* kernel/linux/epoll.c: the calls that wait for events, on the readiness interface of kernel/include/pollq.h */
long lnx_poll(TrapFrame *tf);
long lnx_ppoll(TrapFrame *tf);
long lnx_select(TrapFrame *tf);
long lnx_pselect6(TrapFrame *tf);
long lnx_epoll_create(TrapFrame *tf);
long lnx_epoll_create1(TrapFrame *tf);
long lnx_epoll_ctl(TrapFrame *tf);
long lnx_epoll_wait(TrapFrame *tf);
long lnx_epoll_pwait(TrapFrame *tf);
long lnx_epoll_pwait2(TrapFrame *tf);
long lnx_eventfd(TrapFrame *tf);
long lnx_eventfd2(TrapFrame *tf);
long lnx_timerfd_create(TrapFrame *tf);
long lnx_timerfd_settime(TrapFrame *tf);
long lnx_timerfd_gettime(TrapFrame *tf);

/* kernel/linux/sigsend.c */
long lnx_kill(TrapFrame *tf);
long lnx_tkill(TrapFrame *tf);
long lnx_tgkill(TrapFrame *tf);
long lnx_rt_sigqueueinfo(TrapFrame *tf);
long lnx_rt_tgsigqueueinfo(TrapFrame *tf);
long lnx_waitid(TrapFrame *tf);
long lnx_wait4(TrapFrame *tf);

/* kernel/linux/ids.c */
long lnx_setreuid(TrapFrame *tf);
long lnx_setregid(TrapFrame *tf);
long lnx_setresuid(TrapFrame *tf);
long lnx_getresuid(TrapFrame *tf);
long lnx_setresgid(TrapFrame *tf);
long lnx_getresgid(TrapFrame *tf);

/* kernel/linux/timers.c */
long lnx_times(TrapFrame *tf);
long lnx_getrusage(TrapFrame *tf);
long lnx_nanosleep(TrapFrame *tf);
long lnx_clock_nanosleep(TrapFrame *tf);
long lnx_clock_gettime(TrapFrame *tf);
long lnx_clock_getres(TrapFrame *tf);
long lnx_getitimer(TrapFrame *tf);
long lnx_setitimer(TrapFrame *tf);
long lnx_timer_create(TrapFrame *tf);
long lnx_timer_settime(TrapFrame *tf);
long lnx_timer_gettime(TrapFrame *tf);
long lnx_timer_getoverrun(TrapFrame *tf);
long lnx_timer_delete(TrapFrame *tf);

#endif
