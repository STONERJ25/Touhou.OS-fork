/*
 * System call numbers. Calling convention (x86-64 `syscall`): number in rax, arguments in rdi, rsi, rdx, r10, r8, r9;
 * result in rax (negative errno on failure); rcx and r11 are clobbered.
 *
 * Storage-related calls: mount(source, target, fstype, flags[, options]) - when flags has MNT_OPTS (abi.h) the fifth
 * argument (r8) is a pointer to an option string; sync() flushes every writable filesystem to its device;
 * statfs(path, struct gstatfs *) is number 67.
 */
#ifndef SHARED_SYSCALL_H
#define SHARED_SYSCALL_H

/* Gensokyo extensions registered by their own subsystems (kernel/fs/socket.c, kernel/drivers/pty.c) */
#define SYS_socket        70      /* (domain, type, protocol) -> fd; AF_UNIX / SOCK_STREAM only */
#define SYS_bind          71      /* (fd, path) */
#define SYS_listen        72      /* (fd, backlog) */
#define SYS_accept        73      /* (fd) -> new fd */
#define SYS_connect       74      /* (fd, path) */
#define SYS_shutdown      75      /* (fd, how) */
#define SYS_socketpair    76      /* (type, int sv[2]) */
#define SYS_getgroups     77      /* (n, gid_t list[]) -> count; n == 0 only counts */
#define SYS_setgroups     78      /* (n, gid_t list[]); root only */
#define SYS_geteuid       79
#define SYS_getegid       80      /* getuid/getgid return the real ids, these the effective ones */
#define SYS_settime       81      /* (ms since the epoch, rtc epoch or 0): set the wall clock, and the clock chip if the second is not 0; root only */

#define SYS_exit          0
#define SYS_read          1
#define SYS_write         2
#define SYS_open          3
#define SYS_close         4
#define SYS_lseek         5
#define SYS_stat          6
#define SYS_fstat         7
#define SYS_lstat         8
#define SYS_readdir       9
#define SYS_mkdir         10
#define SYS_unlink        11
#define SYS_rmdir         12
#define SYS_rename        13
#define SYS_chdir         14
#define SYS_getcwd        15
#define SYS_dup           16
#define SYS_dup2          17
#define SYS_pipe          18
#define SYS_fork          19
#define SYS_execve        20
#define SYS_waitpid       21
#define SYS_getpid        22
#define SYS_getppid       23
#define SYS_kill          24
#define SYS_brk           25
#define SYS_mmap          26
#define SYS_munmap        27
#define SYS_yield         28
#define SYS_nanosleep     29
#define SYS_clock_gettime 30
#define SYS_ioctl         31
#define SYS_uname         32
#define SYS_sysinfo       33
#define SYS_reboot        34
#define SYS_mount         35
#define SYS_umount        36
#define SYS_sync          37
#define SYS_ftruncate     38
#define SYS_getrandom     39
#define SYS_symlink       40
#define SYS_readlink      41
#define SYS_chmod         42
#define SYS_chown         43
#define SYS_getuid        44
#define SYS_getgid        45
#define SYS_setuid        46
#define SYS_setgid        47
#define SYS_umask         48
#define SYS_fcntl         49
#define SYS_poll          50
#define SYS_setpgid       51
#define SYS_getpgid       52
#define SYS_sigaction     53
#define SYS_sigreturn     54
#define SYS_sigprocmask   55
#define SYS_truncate      56
#define SYS_access        57
#define SYS_setsid        58
#define SYS_mprotect      59
#define SYS_pause         60
#define SYS_alarm         61
#define SYS_setname       62      /* set the process name shown by ps */
#define SYS_debuglog      63      /* write a line to the kernel log */
#define SYS_ptyopen       64      /* open a pseudo-terminal pair: returns master fd, slave path in buf */
#define SYS_gettime_ms    65      /* wall clock, milliseconds since the epoch */
#define SYS_tcsetpgrp     66
#define SYS_statfs        67      /* statfs(path, struct gstatfs *buf): size and free space of the filesystem holding path (abi.h) */
/* Number ranges for subsystems that register their own calls: networking 90-99, audio 100-105, threads 106-115, free 116-127. */
/* Threads (kernel/sys/thread.c, kernel/sys/futex.c); see docs/THREADS.md */
#define SYS_thread_create 106     /* (flags, stack_top, tls_base, tid_addr, entry, arg) -> new tid; the thread starts in entry(arg) on stack_top */
#define SYS_thread_exit   107     /* (status): ends the calling thread only; zeroes *tid_addr and wakes it; the last thread out ends the process */
#define SYS_gettid        108
#define SYS_futex         109     /* (uaddr, op, val, timeout, uaddr2, val3) */
#define SYS_tgkill        110     /* (pid, tid, sig): signal one thread */
#define SYS_exit_group    111     /* (status): ends every thread (SYS_exit does the same) */
#define SYS_set_tls       112     /* (base): sets the calling thread's FS base */
#define SYS_sigsuspend    113     /* (mask): replace the thread's mask and sleep until a handler has run; returns -EINTR */
#define SYS_sigtimedwait  114     /* (set, timeout): accept one pending signal of set without a handler; returns its number, -EAGAIN on timeout */
#define SYS_sigpending    115     /* (u32 *set): signals pending and blocked */
#define SYS_nsyscalls     128

#define FUTEX_WAIT        0       /* sleep while *uaddr == val; timeout is a struct gtimespec* (relative) or NULL */
#define FUTEX_WAKE        1       /* wake up to val waiters */
#define FUTEX_CMP_REQUEUE 4       /* if *uaddr == val3: wake val waiters, move up to (long)timeout waiters to uaddr2 */
#define FUTEX_CANCELLABLE 0x100   /* or-ed into op: this wait is a cancellation point (a pending pthread_cancel ends it with -ECANCELED) */
#define THREADS_PER_PROC_MAX 256
#define THREADS_TOTAL_MAX    1024

struct gpollfd {
    int   fd;
    short events;
    short revents;
};
#define POLLIN   1
#define POLLOUT  4
#define POLLERR  8
#define POLLHUP  16
#define POLLNVAL 32

struct gsigaction {
    unsigned long handler;      /* 0 = default, 1 = ignore, else user function */
    unsigned long flags;
    unsigned long mask;
    unsigned long restorer;     /* user-space trampoline that calls sigreturn */
};
#define SIG_DFL 0UL
#define SIG_IGN 1UL
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#endif
