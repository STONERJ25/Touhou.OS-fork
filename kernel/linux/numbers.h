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
#define LNX_getitimer           36
#define LNX_alarm               37
#define LNX_setitimer           38
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
#define LNX_getrusage           98
#define LNX_sysinfo             99
#define LNX_times               100
#define LNX_getuid              102
#define LNX_getgid              104
#define LNX_setuid              105
#define LNX_setgid              106
#define LNX_geteuid             107
#define LNX_getegid             108
#define LNX_setpgid             109
#define LNX_getppid             110
#define LNX_setsid              112
#define LNX_setreuid            113
#define LNX_setregid            114
#define LNX_getgroups           115
#define LNX_setgroups           116
#define LNX_setresuid           117
#define LNX_getresuid           118
#define LNX_setresgid           119
#define LNX_getresgid           120
#define LNX_getpgid             121
#define LNX_rt_sigpending       127
#define LNX_rt_sigtimedwait     128
#define LNX_rt_sigqueueinfo     129
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
#define LNX_timer_create        222
#define LNX_timer_settime       223
#define LNX_timer_gettime       224
#define LNX_timer_getoverrun    225
#define LNX_timer_delete        226
#define LNX_clock_gettime       228
#define LNX_clock_getres        229
#define LNX_clock_nanosleep     230
#define LNX_exit_group          231
#define LNX_tgkill              234
#define LNX_waitid              247
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
#define LNX_rt_tgsigqueueinfo   297
#define LNX_prlimit64           302
#define LNX_renameat2           316
#define LNX_getrandom           318
#define LNX_statx               332
#define LNX_rseq                334
#define LNX_clone3              435
#define LNX_faccessat2          439

/* sockets, poll, epoll, eventfd, timerfd (kernel/linux/net.c, epoll.c): the constants and structures of these calls are in netabi.h */
#define LNX_select              23
#define LNX_sendfile            40
#define LNX_socket              41
#define LNX_connect             42
#define LNX_accept              43
#define LNX_sendto              44
#define LNX_recvfrom            45
#define LNX_sendmsg             46
#define LNX_recvmsg             47
#define LNX_shutdown            48
#define LNX_bind                49
#define LNX_listen              50
#define LNX_getsockname         51
#define LNX_getpeername         52
#define LNX_socketpair          53
#define LNX_setsockopt          54
#define LNX_getsockopt          55
#define LNX_epoll_create        213
#define LNX_epoll_wait          232
#define LNX_epoll_ctl           233
#define LNX_pselect6            270
#define LNX_ppoll               271
#define LNX_epoll_pwait         281
#define LNX_timerfd_create      283
#define LNX_eventfd             284
#define LNX_timerfd_settime     286
#define LNX_timerfd_gettime     287
#define LNX_accept4             288
#define LNX_eventfd2            290
#define LNX_epoll_create1       291
#define LNX_recvmmsg            299
#define LNX_sendmmsg            307
#define LNX_epoll_pwait2        441

/* L2 (dynamic programs, coreutils, a shell) */
#define LNX_mremap              25
#define LNX_msync               26
#define LNX_mincore             27
#define LNX_fchdir              81
#define LNX_fchmod              91
#define LNX_fchown              93
#define LNX_lchown              94
#define LNX_getpgrp             111
#define LNX_getsid              124
#define LNX_getgroups           115
#define LNX_statfs              137
#define LNX_fstatfs             138
#define LNX_setxattr            188
#define LNX_lsetxattr           189
#define LNX_fsetxattr           190
#define LNX_getxattr            191
#define LNX_lgetxattr           192
#define LNX_fgetxattr           193
#define LNX_listxattr           194
#define LNX_llistxattr          195
#define LNX_flistxattr          196
#define LNX_removexattr         197
#define LNX_lremovexattr        198
#define LNX_fremovexattr        199
#define LNX_fadvise64           221
#define LNX_fchownat            260
#define LNX_linkat              265
#define LNX_fchmodat            268
#define LNX_utimensat           280
#define LNX_copy_file_range     326

/* arch_prctl(2) */
#define LNX_ARCH_SET_GS     0x1001
#define LNX_ARCH_SET_FS     0x1002
#define LNX_ARCH_GET_FS     0x1003
#define LNX_ARCH_GET_GS     0x1004

/* the *at() family (fcntl.h) */
#define LNX_AT_SYMLINK_NOFOLLOW 0x100
#define LNX_AT_NO_AUTOMOUNT     0x800
#define LNX_AT_EMPTY_PATH       0x1000
#define LNX_AT_SYMLINK_FOLLOW   0x400      /* linkat */
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
