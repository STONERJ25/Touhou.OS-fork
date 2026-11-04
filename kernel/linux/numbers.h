/*
 * x86-64 Linux system call numbers and the constants of the calls this kernel implements (docs/COMPAT.md). The numbers are the interface that every Linux program is
 * built against; tests/host/test_linux_abi.py compares each one with the value a running Linux reports (through <sys/syscall.h> in WSL) and with the name list in
 * names.inc, so a typing mistake cannot survive. Calls that are not listed here are answered with ENOSYS and named once in the kernel log.
 */
#ifndef K_LINUX_NUMBERS_H
#define K_LINUX_NUMBERS_H

#define LINUX_NSYSCALLS     512

#define LNX_read                0
#define LNX_write               1
#define LNX_open                2
#define LNX_close               3
#define LNX_stat                4
#define LNX_fstat               5
#define LNX_lstat               6
#define LNX_poll                7
#define LNX_lseek               8
#define LNX_mmap                9
#define LNX_mprotect            10
#define LNX_munmap              11
#define LNX_brk                 12
#define LNX_rt_sigaction        13
#define LNX_rt_sigprocmask      14
#define LNX_rt_sigreturn        15
#define LNX_ioctl               16
#define LNX_pread64             17
#define LNX_pwrite64            18
#define LNX_readv               19
#define LNX_writev              20
#define LNX_access              21
#define LNX_pipe                22
#define LNX_sched_yield         24
#define LNX_madvise             28
#define LNX_dup                 32
#define LNX_dup2                33
#define LNX_pause               34
#define LNX_nanosleep           35
#define LNX_alarm               37
#define LNX_getpid              39
#define LNX_clone               56
#define LNX_fork                57
#define LNX_vfork               58
#define LNX_execve              59
#define LNX_exit                60
#define LNX_wait4               61
#define LNX_kill                62
#define LNX_uname               63
#define LNX_fcntl               72
#define LNX_flock               73
#define LNX_fsync               74
#define LNX_fdatasync           75
#define LNX_truncate            76
#define LNX_ftruncate           77
#define LNX_getcwd              79
#define LNX_chdir               80
#define LNX_rename              82
#define LNX_mkdir               83
#define LNX_rmdir               84
#define LNX_creat               85
#define LNX_link                86
#define LNX_unlink              87
#define LNX_symlink             88
#define LNX_readlink            89
#define LNX_chmod               90
#define LNX_chown               92
#define LNX_umask               95
#define LNX_gettimeofday        96
#define LNX_getrlimit           97
#define LNX_sysinfo             99
#define LNX_getuid              102
#define LNX_getgid              104
#define LNX_setuid              105
#define LNX_setgid              106
#define LNX_geteuid             107
#define LNX_getegid             108
#define LNX_setpgid             109
#define LNX_getppid             110
#define LNX_setsid              112
#define LNX_getpgid             121
#define LNX_rt_sigpending       127
#define LNX_rt_sigtimedwait     128
#define LNX_rt_sigsuspend       130
#define LNX_sigaltstack         131
#define LNX_prctl               157
#define LNX_arch_prctl          158
#define LNX_setrlimit           160
#define LNX_sync                162
#define LNX_gettid              186
#define LNX_tkill               200
#define LNX_time                201
#define LNX_futex               202
#define LNX_sched_setaffinity   203
#define LNX_sched_getaffinity   204
#define LNX_getdents64          217
#define LNX_set_tid_address     218
#define LNX_clock_gettime       228
#define LNX_clock_nanosleep     230
#define LNX_exit_group          231
#define LNX_tgkill              234
#define LNX_openat              257
#define LNX_mkdirat             258
#define LNX_newfstatat          262
#define LNX_unlinkat            263
#define LNX_renameat            264
#define LNX_symlinkat           266
#define LNX_readlinkat          267
#define LNX_faccessat           269
#define LNX_set_robust_list     273
#define LNX_get_robust_list     274
#define LNX_dup3                292
#define LNX_pipe2               293
#define LNX_prlimit64           302
#define LNX_renameat2           316
#define LNX_getrandom           318
#define LNX_statx               332
#define LNX_rseq                334
#define LNX_clone3              435
#define LNX_faccessat2          439

/* arch_prctl(2) */
#define LNX_ARCH_SET_GS     0x1001
#define LNX_ARCH_SET_FS     0x1002
#define LNX_ARCH_GET_FS     0x1003
#define LNX_ARCH_GET_GS     0x1004

/* the *at() family (fcntl.h) */
#define LNX_AT_SYMLINK_NOFOLLOW 0x100
#define LNX_AT_NO_AUTOMOUNT     0x800
#define LNX_AT_EMPTY_PATH       0x1000
#define LNX_AT_REMOVEDIR        0x200      /* unlinkat */
#define LNX_AT_EACCESS          0x200      /* faccessat */

/* clone(2) flags */
#define LNX_CLONE_VM            0x100
#define LNX_CLONE_VFORK         0x4000
#define LNX_CLONE_THREAD        0x10000
#define LNX_CLONE_SETTLS        0x80000
#define LNX_CLONE_PARENT_SETTID 0x100000
#define LNX_CLONE_CHILD_CLEARTID 0x200000
#define LNX_CLONE_CHILD_SETTID  0x1000000
#define LNX_CSIGNAL             0xff
#define LNX_CLONE_FS            0x200
#define LNX_CLONE_FILES         0x400
#define LNX_CLONE_SIGHAND       0x800
#define LNX_CLONE_SYSVSEM       0x40000

/* futex(2) operations, and the flags or-ed into them */
#define LNX_FUTEX_WAIT          0
#define LNX_FUTEX_WAKE          1
#define LNX_FUTEX_REQUEUE       3
#define LNX_FUTEX_CMP_REQUEUE   4
#define LNX_FUTEX_WAIT_BITSET   9
#define LNX_FUTEX_WAKE_BITSET   10
#define LNX_FUTEX_PRIVATE_FLAG  128
#define LNX_FUTEX_CLOCK_REALTIME 256
#define LNX_FUTEX_CMD_MASK    0x7f

/* sigaction(2), sigprocmask(2), sigaltstack(2): flags and commands (asm/signal.h) */
#define LNX_SA_NOCLDSTOP        0x00000001u
#define LNX_SA_NOCLDWAIT        0x00000002u
#define LNX_SA_SIGINFO          0x00000004u
#define LNX_SA_RESTORER         0x04000000u
#define LNX_SA_ONSTACK          0x08000000u
#define LNX_SA_RESTART          0x10000000u
#define LNX_SA_NODEFER          0x40000000u
#define LNX_SA_RESETHAND        0x80000000u
#define LNX_SIG_BLOCK           0
#define LNX_SIG_UNBLOCK         1
#define LNX_SIG_SETMASK         2
#define LNX_SS_ONSTACK          1
#define LNX_SS_DISABLE          2
#define LNX_MINSIGSTKSZ         2048

/* getrlimit(2): resources and the "no limit" value */
#define LNX_RLIMIT_CPU      0
#define LNX_RLIMIT_FSIZE    1
#define LNX_RLIMIT_DATA     2
#define LNX_RLIMIT_STACK    3
#define LNX_RLIMIT_CORE     4
#define LNX_RLIMIT_RSS      5
#define LNX_RLIMIT_NPROC    6
#define LNX_RLIMIT_NOFILE   7
#define LNX_RLIMIT_MEMLOCK  8
#define LNX_RLIMIT_AS       9
#define LNX_RLIMIT_LOCKS    10
#define LNX_RLIMIT_SIGPENDING 11
#define LNX_RLIMIT_MSGQUEUE 12
#define LNX_RLIMIT_NICE     13
#define LNX_RLIMIT_RTPRIO   14
#define LNX_RLIMIT_RTTIME   15
#define LNX_RLIM_NLIMITS    16
#define LNX_RLIM_INFINITY   (~0ULL)

#endif
