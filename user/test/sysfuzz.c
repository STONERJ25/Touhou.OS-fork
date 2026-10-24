/* sysfuzz: a system call fuzzer for the Gensokyo kernel (docs/HARDENING.md).
 *
 *   sysfuzz [--seed N] [--time SECONDS] [--workers N] [--calls N] [--root] [--trace] [--only-worker N] [--config k:v,k:v]
 *
 * A supervisor forks workers; each worker drops to a throw-away user id, builds a zoo of memory regions and descriptors (files,
 * pipes, sockets, a pty, /proc, /dev nodes) and then makes system calls with random and edge-case arguments: every syscall number
 * including the undefined ones, null and kernel-half and unaligned pointers, buffers that end one byte past a mapping, huge and
 * negative lengths, bad descriptors, odd flags, overlapping buffers, mutated signal frames. Worker threads and forked children
 * race with it. Everything is derived from the seed, so a run can be repeated: when the kernel panics, the last "worker N seed S"
 * line on the console names the worker, and `--only-worker N --trace` replays just that one with every call printed first.
 *
 * What counts as a failure: a kernel panic or hang (the supervisor notices a worker that cannot be killed), a warning in the
 * kernel log, objects that do not come back (/proc/kstats before and after), a kernel that writes outside the buffer it was given
 * (canaries around output buffers) or returns something that is not a valid result. The workers themselves may die of anything.
 * The program lives in /usr/lib/kernel-tests, which is in nobody's PATH; the kernel command line "sysfuzz=seed:1,time:60" makes
 * init run it as root and power off with its verdict. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <syscall.h>
#include <abi.h>

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

#define USER_TOP    0x0000800000000000UL
#define PG          4096UL
#define FUZZ_UID    5000
#define MAX_WORKERS 4
#define POOL        48
#define RING        16

static inline long sc(long n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    long r;
    register long r10 __asm__("r10") = (long)d;
    register long r8 __asm__("r8") = (long)e;
    register long r9 __asm__("r9") = (long)f;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}
#define SC0(n) sc(n, 0, 0, 0, 0, 0, 0)
#define SC1(n, a) sc(n, (u64)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b) sc(n, (u64)(a), (u64)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) sc(n, (u64)(a), (u64)(b), (u64)(c), 0, 0, 0)

/* ---------------------------------------------------------------- random numbers */

typedef struct { u64 s; } Rng;

static u64 rnd(Rng *r) {                                    /* splitmix64 */
    u64 z = (r->s += 0x9E3779B97F4A7C15UL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
    return z ^ (z >> 31);
}
static u64 below(Rng *r, u64 n) { return n ? rnd(r) % n : 0; }
static int pct(Rng *r, int p) { return (int)below(r, 100) < p; }
static u64 mix(u64 a, u64 b) { Rng r = { a * 0x9E3779B97F4A7C15UL + b }; return rnd(&r); }

/* ---------------------------------------------------------------- shared state (a tmpfs file mapped MAP_SHARED) */

typedef struct { u64 nr; u64 a[6]; long ret; } Entry;

typedef struct {
    volatile u64 calls;                                     /* completed calls */
    volatile int in_call;
    volatile int pid;
    volatile int main_tid;
    volatile u32 ring_pos;
    volatile u64 cur_nr;                                    /* the call in progress (valid while in_call) */
    volatile u64 cur_a[6];
    volatile int viol;
    volatile int done;
    char vmsg[200];
    Entry ring[RING];
} Slot;

typedef struct {
    volatile u64 stop;
    Slot slot[MAX_WORKERS];
} Shared;

static Shared *g_sh;

/* ---------------------------------------------------------------- configuration */

typedef struct {
    u64 seed;
    int secs;
    int workers;
    int ncalls;
    int root;
    int trace;
    int only_worker;
    int stuck_ms;
    int verbose;
    const char *scratch;
    int supervisor_pid;
} Cfg;

static Cfg g_cfg;

static u64 now_ms(void) {
    struct gtimespec ts;
    SC2(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct gtimespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    SC2(SYS_nanosleep, &ts, 0);
}

/* ---------------------------------------------------------------- the table of calls */

enum { SF_WRAP = 1, SF_ROOT = 2, SF_SKIP = 4 };

typedef struct {
    int nr;
    const char *name;
    const char *kinds;                                      /* one letter per argument, see gen_args */
    unsigned short osz;                                     /* size of the output structure for an 'o' argument */
    unsigned char weight;
    unsigned char flags;
} Spec;

/* kinds: i integer  f descriptor  F small descriptor number  p path  P path that may be changed  b input buffer  B output buffer
 *        l length (of the buffer)  o output structure  s input structure  m mode  q flags  g signal  d pid  a address
 *        r prot  M map flags  u uid  O offset  w whence  t timespec pointer  c command  v pointer to a small value  k count */
static const Spec g_specs[] = {
    { SYS_exit, "exit", "i", 0, 1, SF_WRAP },
    { SYS_read, "read", "fBl", 0, 8, 0 },
    { SYS_write, "write", "fbl", 0, 8, 0 },
    { SYS_open, "open", "pqm", 0, 8, 0 },
    { SYS_close, "close", "f", 0, 5, 0 },
    { SYS_lseek, "lseek", "fOw", 0, 4, 0 },
    { SYS_stat, "stat", "po", sizeof(struct gstat), 4, 0 },
    { SYS_fstat, "fstat", "fo", sizeof(struct gstat), 4, 0 },
    { SYS_lstat, "lstat", "po", sizeof(struct gstat), 3, 0 },
    { SYS_readdir, "readdir", "fBk", 0, 4, 0 },
    { SYS_mkdir, "mkdir", "Pm", 0, 3, 0 },
    { SYS_unlink, "unlink", "P", 0, 3, 0 },
    { SYS_rmdir, "rmdir", "P", 0, 3, 0 },
    { SYS_rename, "rename", "PP", 0, 3, 0 },
    { SYS_chdir, "chdir", "P", 0, 2, 0 },
    { SYS_getcwd, "getcwd", "Bl", 0, 3, 0 },
    { SYS_dup, "dup", "f", 0, 3, 0 },
    { SYS_dup2, "dup2", "fF", 0, 3, 0 },
    { SYS_pipe, "pipe", "o", 8, 3, 0 },
    { SYS_fork, "fork", "", 0, 1, 0 },
    { SYS_execve, "execve", "pss", 0, 2, SF_WRAP },
    { SYS_waitpid, "waitpid", "dvq", 0, 3, 0 },
    { SYS_getpid, "getpid", "", 0, 1, 0 },
    { SYS_getppid, "getppid", "", 0, 1, 0 },
    { SYS_kill, "kill", "dg", 0, 5, 0 },
    { SYS_brk, "brk", "a", 0, 2, 0 },
    { SYS_mmap, "mmap", "alrMfO", 0, 10, 0 },
    { SYS_munmap, "munmap", "al", 0, 6, 0 },
    { SYS_yield, "yield", "", 0, 1, 0 },
    { SYS_nanosleep, "nanosleep", "t", 0, 2, 0 },
    { SYS_clock_gettime, "clock_gettime", "io", sizeof(struct gtimespec), 2, 0 },
    { SYS_ioctl, "ioctl", "fcs", 0, 12, 0 },
    { SYS_uname, "uname", "o", sizeof(struct guname), 1, 0 },
    { SYS_sysinfo, "sysinfo", "o", sizeof(struct gsysinfo), 1, 0 },
    { SYS_reboot, "reboot", "i", 0, 0, SF_SKIP },
    { SYS_mount, "mount", "PPsqs", 0, 2, SF_ROOT },
    { SYS_umount, "umount", "P", 0, 2, SF_ROOT },
    { SYS_sync, "sync", "", 0, 1, 0 },
    { SYS_ftruncate, "ftruncate", "fO", 0, 4, 0 },
    { SYS_getrandom, "getrandom", "Bl", 0, 3, 0 },
    { SYS_symlink, "symlink", "pP", 0, 3, 0 },
    { SYS_readlink, "readlink", "pBl", 0, 3, 0 },
    { SYS_chmod, "chmod", "Pm", 0, 3, 0 },
    { SYS_chown, "chown", "Puu", 0, 3, 0 },
    { SYS_getuid, "getuid", "", 0, 1, 0 },
    { SYS_getgid, "getgid", "", 0, 1, 0 },
    { SYS_setuid, "setuid", "u", 0, 2, 0 },
    { SYS_setgid, "setgid", "u", 0, 2, 0 },
    { SYS_umask, "umask", "m", 0, 2, 0 },
    { SYS_fcntl, "fcntl", "fcF", 0, 5, 0 },
    { SYS_poll, "poll", "skI", 0, 5, 0 },
    { SYS_setpgid, "setpgid", "dd", 0, 3, 0 },
    { SYS_getpgid, "getpgid", "d", 0, 2, 0 },
    { SYS_sigaction, "sigaction", "gss", 0, 6, 0 },
    { SYS_sigreturn, "sigreturn", "", 0, 0, SF_SKIP },    /* done by fuzz_sigreturn */
    { SYS_sigprocmask, "sigprocmask", "ivv", 0, 5, 0 },
    { SYS_truncate, "truncate", "PO", 0, 3, 0 },
    { SYS_access, "access", "pm", 0, 3, 0 },
    { SYS_setsid, "setsid", "", 0, 1, 0 },
    { SYS_mprotect, "mprotect", "alr", 0, 8, 0 },
    { SYS_pause, "pause", "", 0, 1, SF_WRAP },
    { SYS_alarm, "alarm", "i", 0, 1, 0 },
    { SYS_setname, "setname", "s", 0, 1, 0 },
    { SYS_debuglog, "debuglog", "s", 0, 1, 0 },
    { SYS_ptyopen, "ptyopen", "Bl", 0, 3, 0 },
    { SYS_gettime_ms, "gettime_ms", "", 0, 1, 0 },
    { SYS_tcsetpgrp, "tcsetpgrp", "d", 0, 2, 0 },
    { SYS_statfs, "statfs", "po", sizeof(struct gstatfs), 2, 0 },
    { SYS_socket, "socket", "iii", 0, 6, 0 },
    { SYS_bind, "bind", "fxk", 0, 6, 0 },
    { SYS_listen, "listen", "fi", 0, 4, 0 },
    { SYS_accept, "accept", "fXv", 0, 4, 0 },
    { SYS_connect, "connect", "fxk", 0, 6, 0 },
    { SYS_shutdown, "shutdown", "fi", 0, 3, 0 },
    { SYS_socketpair, "socketpair", "io", 8, 3, 0 },
    { SYS_getgroups, "getgroups", "kB", 0, 2, 0 },
    { SYS_setgroups, "setgroups", "ks", 0, 2, 0 },
    { SYS_geteuid, "geteuid", "", 0, 1, 0 },
    { SYS_getegid, "getegid", "", 0, 1, 0 },
    { SYS_settime, "settime", "ii", 0, 1, SF_ROOT },
    { SYS_utimes, "utimes", "PtI", 0, 2, 0 },
    { SYS_sendto, "sendto", "fblixk", 0, 6, 0 },
    { SYS_recvfrom, "recvfrom", "fBliXv", 0, 6, 0 },
    { SYS_getsockname, "getsockname", "fXv", 0, 3, 0 },
    { SYS_getpeername, "getpeername", "fXv", 0, 3, 0 },
    { SYS_setsockopt, "setsockopt", "fiiskc", 0, 5, 0 },
    { SYS_getsockopt, "getsockopt", "fiiBv", 0, 5, 0 },
    { SYS_netctl, "netctl", "ci", 0, 4, 0 },
    { SYS_thread_create, "thread_create", "iaiviI", 0, 2, 0 },
    { SYS_thread_exit, "thread_exit", "i", 0, 1, SF_WRAP },
    { SYS_gettid, "gettid", "", 0, 1, 0 },
    { SYS_futex, "futex", "acitaI", 0, 8, 0 },
    { SYS_tgkill, "tgkill", "ddg", 0, 4, 0 },
    { SYS_exit_group, "exit_group", "i", 0, 1, SF_WRAP },
    { SYS_set_tls, "set_tls", "a", 0, 2, 0 },
    { SYS_sigsuspend, "sigsuspend", "i", 0, 1, SF_WRAP },
    { SYS_sigtimedwait, "sigtimedwait", "it", 0, 3, SF_WRAP },
    { SYS_sigpending, "sigpending", "v", 0, 2, 0 },
    { SYS_getpriority, "getpriority", "id", 0, 2, 0 },
    { SYS_setpriority, "setpriority", "idi", 0, 2, 0 },
    { SYS_link, "link", "pP", 0, 3, 0 },
};
#define NSPECS ((int)(sizeof g_specs / sizeof g_specs[0]))

/* ---------------------------------------------------------------- the worker's world */

typedef struct W {
    Rng rng;
    int idx;
    int depth;                                              /* 0 worker, 1 forked child, 2 thread */
    Slot *slot;
    int fuzz_uid_dropped;
    u8 *rw;                                                 /* big read/write mapping: buffers [0,0x20000), paths, structures */
    u8 *small;                                              /* one page, followed by an unmapped page */
    u8 *ro;
    u8 *none;
    u8 *rwx;
    u8 *fmap;                                               /* private mapping of a scratch file */
    u8 *shm;                                                /* shared mapping (a tmpfs file) */
    u8 *stk;                                                /* the worker's stack, roughly */
    int fds[POOL];
    int nfds;
    int pids[16];
    int npids;
    int nforks;
    int last_fd;
    char scratch[96];
    char fbname[16];
    long budget;
} W;

#define BUFAREA   0x20000UL
#define PATHAREA  0x20000UL
#define STRUCTAREA 0x21000UL
#define RWLEN     0x25000UL

static void note_violation(W *w, const char *what, const Spec *sp) {
    Slot *s = w->slot;
    s->viol++;
    if (s->vmsg[0]) return;
    char *o = s->vmsg;
    const char *n = sp ? sp->name : "?";
    int i = 0;
    for (; what[i] && i < 120; i++) o[i] = what[i];
    o[i++] = ' ';
    o[i++] = '(';
    for (int j = 0; n[j] && i < 190; j++) o[i++] = n[j];
    o[i++] = ')';
    o[i] = 0;
}

/* ---------------------------------------------------------------- values */

static const u64 g_edge[] = {
    0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 100, 127, 128, 255, 256, 511, 512, 1000, 1023, 1024, 4095, 4096, 4097, 8192,
    65535, 65536, 65537, 0x100000UL, 0x7fffffffUL, 0x80000000UL, 0xffffffffUL, 0x100000000UL, 0x100000001UL, 0x7fffffffffffUL, 0x800000000000UL,
    0x7fffffffffffffffUL, 0x8000000000000000UL, 0xffffffffffffffffUL, 0xfffffffffffffffeUL, 0xfffffffffffff000UL, 0xffffffffffffefffUL,
    USER_TOP - 1, USER_TOP, USER_TOP + 1, USER_TOP - 4096, USER_TOP - 8, 0x7ffffffff000UL, 0x7fffffffe000UL, 0xffff800000000000UL,
    0xffffffff80000000UL, 0xffffc00000000000UL, 0xffffc80000000000UL, 0x4000000000000000UL,
};
#define NEDGE ((int)(sizeof g_edge / sizeof g_edge[0]))

static u64 gen_int(W *w) {
    Rng *r = &w->rng;
    switch (below(r, 10)) {
    case 0: case 1: case 2: case 3: return g_edge[below(r, NEDGE)];
    case 4: case 5: return below(r, 100);
    case 6: return (u64)(-(long)below(r, 5000));
    case 7: return rnd(r) & 0xffffffffUL;
    case 8: return rnd(r);
    default: return below(r, 65537);
    }
}

static u64 gen_len(W *w, u64 room) {
    Rng *r = &w->rng;
    switch (below(r, 12)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return room;
    case 3: return room + 1;
    case 4: return room ? room - 1 : 0;
    case 5: case 6: return below(r, 256);
    case 7: return below(r, 70000);
    case 8: return g_edge[below(r, NEDGE)];
    case 9: return 4096 + below(r, 3) - 1;
    default: return below(r, 2000);
    }
}

static u64 gen_fd(W *w) {
    Rng *r = &w->rng;
    if (w->last_fd > 0 && pct(r, 30)) return (u64)w->last_fd;
    switch (below(r, 10)) {
    case 0: return (u64)(long)(int)(0xffffffff - below(r, 3));                  /* -1, -2, -3 */
    case 1: return below(r, 3);                                                 /* 0, 1, 2: already /dev/null */
    case 2: return 60 + below(r, 12);                                           /* around MAX_FD */
    case 3: return g_edge[below(r, NEDGE)];
    case 4: return below(r, 70);
    default: return w->nfds ? (u64)w->fds[below(r, (u64)w->nfds)] : 3;
    }
}

static u64 gen_pid(W *w) {
    Rng *r = &w->rng;
    long p;
    switch (below(r, 10)) {
    case 0: p = 0; break;
    case 1: p = -1; break;
    case 2: p = (long)below(r, 300); break;
    case 3: p = -(long)below(r, 300); break;
    case 4: p = (long)g_edge[below(r, NEDGE)]; break;
    case 5: case 6: p = SC0(SYS_getpid); break;
    case 7: p = -SC0(SYS_getpid); break;
    default: p = w->npids ? w->pids[below(r, (u64)w->npids)] : SC0(SYS_getpid); break;
    }
    /* never the supervisor, init, or "everybody": a real kill(-1) or kill(1) would end the machine's day, and it is the kernel's
     * job to refuse those for us (the fuzzer tries them once, from a throw-away user, in the supervisor's self test) */
    long ap = p < 0 ? -p : p;
    if (ap == g_cfg.supervisor_pid || ap == 1 || p == -1) p = SC0(SYS_getpid);
    if (g_cfg.root && p != SC0(SYS_getpid) && p != -SC0(SYS_getpid) && p != 0) {
        int mine = 0;
        for (int i = 0; i < w->npids; i++) if (p == w->pids[i]) mine = 1;
        if (!mine) p = SC0(SYS_getpid);
    }
    return (u64)p;
}

static u64 gen_sig(W *w) {
    Rng *r = &w->rng;
    switch (below(r, 6)) {
    case 0: { u64 s = below(r, 40); return (s >= 19 && s <= 22) ? 18 : s; }         /* stop signals would end the worker: nobody continues it */
    case 1: { u64 s = g_edge[below(r, NEDGE)]; return (s >= 19 && s <= 22) ? 18 : s; }
    default: {
        static const int sigs[] = { 0, 1, 2, 3, 10, 12, 13, 14, 15, 17, 18, 28, 31, 26, 27 };       /* no stop signals: nobody would continue the worker */
        return (u64)sigs[below(r, sizeof sigs / sizeof sigs[0])];
    }
    }
}

/* A pointer where the kernel will touch `need` bytes. */
static u64 gen_ptr(W *w, u64 need, int writable) {
    Rng *r = &w->rng;
    u8 *base = w->rw;
    u64 room = BUFAREA;
    if (need > room - 64) need = room - 64;
    switch (below(r, 100) / 5) {                              /* twenty slots of five percent */
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: {      /* 45%: inside the big buffer, with room */
        u64 off = 32 + below(r, room - need - 64);
        if (pct(r, 20)) off &= ~7UL;
        return (u64)(base + off);
    }
    case 9: return (u64)(base + BUFAREA - need);                                   /* ends exactly at the end of the buffer area */
    case 10: return (u64)(w->small + PG - need);                                   /* ends exactly where the next page is unmapped */
    case 11: return (u64)(w->small + PG - need + 1);                               /* one byte too far */
    case 12: return (u64)(base + 1 + below(r, 7));                                 /* unaligned */
    case 13: return 0;
    case 14: return 0xffff800000000000UL + below(r, 0x100000) * 4096;              /* kernel half */
    case 15: {
        static const u64 k[] = { 0xffffffff80000000UL, 0xffffc00000000000UL, 0xffffc80000000000UL, 0xffff800000000000UL, 0x800000000000UL,
                                 0x8000000000000000UL, 0xffffffffffffffffUL, 0x7fffffffffffUL, USER_TOP - 1, USER_TOP - 4096, USER_TOP };
        return k[below(r, sizeof k / sizeof k[0])];
    }
    case 16: return writable ? (u64)w->ro + below(r, 2 * PG) : (u64)w->ro + below(r, PG);   /* read-only memory (writing must fail) */
    case 17: return pct(r, 50) ? (u64)w->none : (u64)w->rwx + below(r, PG);                  /* PROT_NONE or executable memory */
    default: return pct(r, 30) ? below(r, 4096) : (pct(r, 50) ? (u64)w->stk : rnd(r));       /* small ints, the stack, anything */
    }
}

/* ---------------------------------------------------------------- strings and structures in the scratch pages */

static void put_str(u8 *dst, const char *s) { while ((*dst++ = (u8)*s++)) { } }

/* A path in the paths page. `mutable` ones stay inside the scratch directory (the fuzzer may run as root). */
static u64 gen_path(W *w, int mut) {
    Rng *r = &w->rng;
    u8 *buf = w->rw + PATHAREA;
    static const char *const sys[] = {
        "/", ".", "..", "", "/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/kmsg", "/proc/meminfo", "/proc/self/status",
        "/proc/self/stat", "/proc/self/cmdline", "/proc/self/fd", "/proc/self/fd/0", "/proc/self/cwd", "/proc/self/exe", "/proc/tasks",
        "/proc/mounts", "/proc/interrupts", "/proc/cpuinfo", "/proc/version", "/proc/uptime", "/proc/loadavg", "/proc/kstats",
        "/proc/net/dev", "/proc/net/tcp", "/proc/net/udp", "/proc/net/route", "/proc/net/arp", "/proc/pci", "/proc/partitions", "/proc/1/status",
        "/proc/1/fd", "/proc/999999/status", "/bin/true", "/bin/hello", "/bin", "/etc", "/etc/passwd", "/tmp", "/dev", "/dev/pts", "/proc",
        "/BambooForest", "/nonexistent", "/bin/true/x", "//", "/./.", "/proc/self/fd/../fd/../fd", "/dev/tty0", "/dev/pts/0",
    };
    static const char *const local[] = { "f0", "f1", "f2", "d0", "d0/f3", "d0/d1", "d0/d1/f4", "l0", "l1", "loop1", "loop2", "sock0", "nope", "d0/../f0", "./f0", "f0/", "f0/x" };
    char tmp[1200];
    int n = 0;
    int kind = (int)below(r, 20);
    if (mut) kind = kind < 12 ? 0 : (kind < 16 ? 1 : 2);
    if (kind < 12 && !mut) {
        const char *p = sys[below(r, sizeof sys / sizeof sys[0])];
        put_str(buf, p);
        return (u64)buf;
    }
    if (kind < 9 || mut) {                                   /* in the scratch directory */
        const char *l = local[below(r, sizeof local / sizeof local[0])];
        for (const char *s = w->scratch; *s; s++) tmp[n++] = *s;
        tmp[n++] = '/';
        if (kind != 0 || pct(r, 30)) {                       /* sometimes a generated name instead */
            int len = 1 + (int)below(r, pct(r, 10) ? 300 : 12);
            for (int i = 0; i < len && n < 1000; i++) tmp[n++] = (char)('a' + below(r, 6));
        } else {
            for (; *l; l++) tmp[n++] = *l;
        }
        tmp[n] = 0;
        put_str(buf, tmp);
        return (u64)buf;
    }
    /* odd shapes: long, deep, dots, slashes, high bytes (never for a path that may be changed) */
    int len = (int)below(r, pct(r, 30) ? 1100 : 60);
    for (int i = 0; i < len && n < 1190; i++) {
        switch (below(r, 6)) {
        case 0: tmp[n++] = '/'; break;
        case 1: tmp[n++] = '.'; if (pct(r, 50) && n < 1189) tmp[n++] = '.'; break;
        case 2: tmp[n++] = (char)(0x80 + below(r, 128)); break;
        default: tmp[n++] = (char)('a' + below(r, 26)); break;
        }
    }
    tmp[n] = 0;
    put_str(buf, tmp);
    return (u64)buf;
}

static void put_u16(u8 *p, unsigned v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_u32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void put_u64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

/* A structure the kernel is going to read, in the structures page: valid in shape more often than not, with a mutated field now and then. */
static u64 gen_struct(W *w, char who, const Spec *sp, u64 hint) {
    Rng *r = &w->rng;
    u8 *s = w->rw + STRUCTAREA + below(r, 8) * 256;
    for (int i = 0; i < 256; i++) s[i] = 0;
    switch (sp->nr) {
    case SYS_execve: {                                       /* argv / envp: an array of pointers to strings in the paths page */
        int n = (int)below(r, 6);
        u64 *v = (u64 *)s;
        for (int i = 0; i < n; i++) v[i] = (u64)(w->rw + PATHAREA + 0x100 + i * 8);
        w->rw[PATHAREA + 0x100] = 'x';
        if (pct(r, 15)) v[below(r, 6)] = gen_ptr(w, 8, 0);
        return pct(r, 5) ? gen_ptr(w, 8, 0) : (u64)s;
    }
    case SYS_sigaction: {                                    /* struct gsigaction { handler, flags, mask, restorer } */
        u64 h = pct(r, 40) ? 0 : (pct(r, 20) ? 1 : (pct(r, 60) ? (u64)w->rwx : gen_int(w)));   /* mostly harmless; sometimes any value at all */
        put_u64(s, h);
        put_u64(s + 8, pct(r, 70) ? 0 : rnd(r));
        put_u64(s + 16, pct(r, 70) ? 0 : rnd(r));
        put_u64(s + 24, pct(r, 70) ? 0 : gen_int(w));
        return pct(r, 5) ? gen_ptr(w, 32, 0) : (u64)s;
    }
    case SYS_poll: {                                         /* struct gpollfd { int fd; short events, revents; } */
        int n = (int)below(r, 6);
        for (int i = 0; i < n; i++) {
            put_u32(s + i * 8, (u32)gen_fd(w));
            put_u16(s + i * 8 + 4, (unsigned)(pct(r, 70) ? 5 : rnd(r)));
        }
        return pct(r, 5) ? gen_ptr(w, 64, 1) : (u64)s;
    }
    case SYS_bind: case SYS_connect: case SYS_sendto: {      /* sockaddr_in or sockaddr_un */
        if (pct(r, 55)) {
            put_u16(s, 2);                                   /* AF_INET */
            put_u16(s + 2, (unsigned)(pct(r, 70) ? 0x5000 + below(r, 40) : rnd(r)));        /* port, network order */
            put_u32(s + 4, pct(r, 60) ? 0x0100007f : (u32)rnd(r));                         /* 127.0.0.1 */
        } else {
            put_u16(s, 1);                                   /* AF_UNIX */
            const char *l = w->scratch;
            int n = 0;
            for (; l[n]; n++) s[2 + n] = (u8)l[n];
            s[2 + n++] = '/'; s[2 + n++] = 's'; s[2 + n++] = (u8)('0' + below(r, 4));
        }
        return pct(r, 5) ? gen_ptr(w, 16, 0) : (u64)s;
    }
    case SYS_setsockopt: case SYS_getsockopt: case SYS_getsockname: case SYS_accept: case SYS_recvfrom: {
        put_u32(s, (u32)(pct(r, 60) ? 16 : rnd(r)));         /* the length word that goes with an address buffer */
        put_u32(s + 4, (u32)rnd(r));
        put_u64(s + 8, pct(r, 50) ? 1 : rnd(r));
        return pct(r, 5) ? gen_ptr(w, 16, 1) : (u64)s;
    }
    case SYS_nanosleep: case SYS_sigtimedwait: case SYS_utimes: case SYS_futex: {
        long sec = pct(r, 85) ? 0 : (long)gen_int(w), nsec = pct(r, 80) ? (long)below(r, 2000000) : (long)gen_int(w);
        put_u64(s, (u64)sec);
        put_u64(s + 8, (u64)nsec);
        put_u64(s + 16, (u64)(pct(r, 80) ? 0 : gen_int(w)));
        put_u64(s + 24, (u64)(pct(r, 80) ? 0 : gen_int(w)));
        return pct(r, 5) ? gen_ptr(w, 32, 0) : (u64)s;
    }
    case SYS_netctl: {                                       /* gnet_if / gnet_route / gnet_arp, filled with small numbers */
        for (int i = 0; i < 128; i++) s[i] = (u8)(pct(r, 60) ? 0 : rnd(r));
        return (u64)s;
    }
    case SYS_ioctl: case SYS_setgroups: case SYS_setname: case SYS_debuglog: case SYS_mount: {
        (void)hint;
        if (sp->nr == SYS_setname || sp->nr == SYS_debuglog) { put_str(s, "fuzz"); return pct(r, 10) ? (pct(r, 50) ? 0 : 0xffffffff80000000UL) : (u64)s; }       /* names go to the log: no random bytes */
        if (sp->nr == SYS_mount) { put_str(s, pct(r, 50) ? "tmpfs" : (pct(r, 50) ? "procfs" : "bogus")); return pct(r, 10) ? gen_ptr(w, 8, 0) : (u64)s; }
        for (int i = 0; i < 128; i++) s[i] = (u8)(pct(r, 50) ? 0 : (pct(r, 50) ? below(r, 4) : rnd(r)));
        if (sp->nr == SYS_ioctl && pct(r, 40)) put_u32(s + 8, 25);                     /* a plausible termios / winsize */
        return pct(r, 6) ? gen_ptr(w, 128, 1) : (u64)s;
    }
    default:
        break;
    }
    for (int i = 0; i < 128; i++) s[i] = (u8)(pct(r, 50) ? 0 : rnd(r));
    (void)who;
    return pct(r, 20) ? gen_ptr(w, 128, 0) : (u64)s;
}

/* ---------------------------------------------------------------- the descriptor pool */

static void pool_add(W *w, long fd) {
    if (fd < 0 || fd > 62) return;
    if (w->nfds < POOL) w->fds[w->nfds++] = (int)fd;
    else w->fds[below(&w->rng, POOL)] = (int)fd;
}

static void sockaddr_in(u8 *s, unsigned port) {
    for (int i = 0; i < 16; i++) s[i] = 0;
    put_u16(s, 2);
    s[2] = (u8)(port >> 8); s[3] = (u8)port;
    put_u32(s + 4, 0x0100007f);
}

static void setup_fds(W *w) {
    char path[160];
    int n;
    #define PATHF(name) do { n = 0; for (const char *s = w->scratch; *s; s++) path[n++] = *s; path[n++] = '/'; for (const char *s = name; *s; s++) path[n++] = *s; path[n] = 0; } while (0)
    PATHF("f0");  pool_add(w, SC3(SYS_open, path, O_RDWR | O_CREAT, 0666));
    PATHF("f1");  pool_add(w, SC3(SYS_open, path, O_RDWR | O_CREAT | O_APPEND, 0666));
    PATHF("f0");  pool_add(w, SC3(SYS_open, path, O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, w->scratch, O_RDONLY | O_DIRECTORY, 0));
    pool_add(w, SC3(SYS_open, "/", O_RDONLY, 0));
    int pf[2];
    if (SC1(SYS_pipe, pf) == 0) {
        SC3(SYS_fcntl, pf[0], F_SETFL, O_NONBLOCK);
        SC3(SYS_fcntl, pf[1], F_SETFL, O_NONBLOCK);
        pool_add(w, pf[0]); pool_add(w, pf[1]);
    }
    int sv[2];
    if (SC2(SYS_socketpair, SOCK_STREAM | SOCK_NONBLOCK, sv) == 0) { pool_add(w, sv[0]); pool_add(w, sv[1]); }
    /* a listening AF_UNIX socket in the scratch directory, with a client and the accepted end */
    long ls = SC3(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ls >= 0) {
        u8 *sa = w->rw + STRUCTAREA + 0x800;
        for (int i = 0; i < 100; i++) sa[i] = 0;
        put_u16(sa, 1);
        n = 0;
        for (const char *s = w->scratch; *s; s++) sa[2 + n++] = (u8)*s;
        sa[2 + n++] = '/'; sa[2 + n++] = 's'; sa[2 + n++] = '0';
        if (SC3(SYS_bind, ls, sa, 110) == 0 && SC2(SYS_listen, ls, 4) == 0) {
            long cs = SC3(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (cs >= 0 && SC3(SYS_connect, cs, sa, 110) == 0) {
                pool_add(w, cs);
                long as = SC3(SYS_accept, ls, 0, 0);
                if (as >= 0) pool_add(w, as);
            } else if (cs >= 0) SC1(SYS_close, cs);
        }
        pool_add(w, ls);
    }
    /* Internet sockets over loopback: a UDP socket, a TCP listener with a connection */
    u8 *sa = w->rw + STRUCTAREA + 0x900;
    long us = SC3(SYS_socket, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (us >= 0) { sockaddr_in(sa, 0x5100 + (unsigned)(w->idx * 4)); SC3(SYS_bind, us, sa, 16); pool_add(w, us); }
    long tl = SC3(SYS_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (tl >= 0) {
        unsigned port = 0x5200 + (unsigned)(w->idx * 4);
        sockaddr_in(sa, port);
        if (SC3(SYS_bind, tl, sa, 16) == 0 && SC2(SYS_listen, tl, 4) == 0) {
            long tc = SC3(SYS_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (tc >= 0) { SC3(SYS_connect, tc, sa, 16); pool_add(w, tc); }
            for (int i = 0; i < 20; i++) { long ta = SC3(SYS_accept, tl, 0, 0); if (ta >= 0) { pool_add(w, ta); break; } SC0(SYS_yield); }
        }
        pool_add(w, tl);
    }
    /* a pty pair */
    char pty[48];
    long m = SC2(SYS_ptyopen, pty, sizeof pty);
    if (m >= 0) {
        SC3(SYS_fcntl, m, F_SETFL, O_NONBLOCK);
        pool_add(w, m);
        pool_add(w, SC3(SYS_open, pty, O_RDWR | O_NONBLOCK, 0));
    }
    pool_add(w, SC3(SYS_open, "/dev/null", O_RDWR, 0));
    pool_add(w, SC3(SYS_open, "/dev/zero", O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, "/dev/random", O_RDONLY | O_NONBLOCK, 0));
    pool_add(w, SC3(SYS_open, "/proc/meminfo", O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, "/proc/self/status", O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, "/proc/self/fd", O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, "/proc", O_RDONLY, 0));
    pool_add(w, SC3(SYS_open, "/dev/kmsg", O_RDONLY | O_NONBLOCK, 0));
    if (g_cfg.root) {
        pool_add(w, SC3(SYS_open, "/dev/fb0", O_RDWR, 0));
        pool_add(w, SC3(SYS_open, "/dev/input/events", O_RDONLY | O_NONBLOCK, 0));
    }
    #undef PATHF
}

/* ---------------------------------------------------------------- memory */

static u8 *map_region(u64 len, int prot) {
    long r = sc(SYS_mmap, 0, len, (u64)prot, MAP_PRIVATE | MAP_ANONYMOUS, (u64)-1L, 0);
    return (r < 0 && r > -4096) ? 0 : (u8 *)r;
}

static void setup_memory(W *w) {
    w->rw = map_region(RWLEN, PROT_READ | PROT_WRITE);
    w->small = map_region(2 * PG, PROT_READ | PROT_WRITE);
    w->ro = map_region(2 * PG, PROT_READ | PROT_WRITE);
    w->none = map_region(PG, PROT_READ | PROT_WRITE);
    w->rwx = map_region(PG, PROT_READ | PROT_WRITE | PROT_EXEC);
    w->stk = (u8 *)((u64)__builtin_frame_address(0) & ~(PG - 1));
    if (!w->rw || !w->small || !w->ro || !w->none || !w->rwx) { SC1(SYS_exit, 3 << 8); }
    SC3(SYS_mprotect, w->rw + 0x22000, PG, PROT_NONE);                 /* guard pages inside and after the big mapping */
    SC3(SYS_mprotect, w->rw + 0x24000, PG, PROT_NONE);
    SC3(SYS_munmap, w->small + PG, PG, 0);                              /* the page after `small` does not exist */
    for (int i = 0; i < 16; i++) w->ro[i] = (u8)i;
    SC3(SYS_mprotect, w->ro, 2 * PG, PROT_READ);
    SC3(SYS_mprotect, w->none, PG, PROT_NONE);
    char path[160];
    int n = 0;
    for (const char *s = w->scratch; *s; s++) path[n++] = *s;
    path[n++] = '/'; path[n++] = 'f'; path[n++] = '0'; path[n] = 0;
    long fd = SC3(SYS_open, path, O_RDWR | O_CREAT, 0666);
    if (fd >= 0) {
        u8 data[64];
        for (int i = 0; i < 64; i++) data[i] = (u8)('A' + i % 26);
        SC3(SYS_write, fd, data, 64);
        SC2(SYS_ftruncate, fd, 2 * PG);
        long r = sc(SYS_mmap, 0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, (u64)fd, 0);
        w->fmap = (r < 0 && r > -4096) ? 0 : (u8 *)r;
        SC1(SYS_close, fd);
    }
    w->shm = g_sh ? (u8 *)g_sh + 4096 : 0;
}

/* ---------------------------------------------------------------- one call */

static const u64 g_ioctls[] = {
    TCGETS, TCSETS, TCSETSW, TCSETSF, TIOCGPGRP, TIOCSPGRP, TIOCGWINSZ, TIOCSWINSZ, FIONREAD, TIOCGNAME, FBIOGET_INFO, FBIO_FLUSH, FBIO_GRAPHICS,
    INPUT_GRAB, SOCK_PEERCRED,
};

static u64 gen_cmd(W *w, const Spec *sp) {
    Rng *r = &w->rng;
    switch (sp->nr) {
    case SYS_ioctl:
        if (pct(r, 80)) return g_ioctls[below(r, sizeof g_ioctls / sizeof g_ioctls[0])];
        return pct(r, 50) ? 0x5400 + below(r, 128) : rnd(r);
    case SYS_fcntl: return pct(r, 85) ? below(r, 6) : gen_int(w);
    case SYS_netctl: return pct(r, 85) ? below(r, 11) : gen_int(w);
    case SYS_setsockopt: case SYS_getsockopt: return pct(r, 80) ? below(r, 25) : gen_int(w);
    case SYS_futex: return pct(r, 80) ? (pct(r, 50) ? 0 : (pct(r, 50) ? 1 : 4)) | (pct(r, 20) ? 0x100 : 0) : gen_int(w);
    default: return gen_int(w);
    }
}

typedef struct { u8 *p; u64 len; } Canary;

static void canary_set(u8 *p, u64 len, Canary *c) {
    c->p = p; c->len = len;
    for (int i = 1; i <= 16; i++) { p[-i] = 0xA5; p[len + (u64)i - 1] = 0xA5; }
}

static int canary_ok(const Canary *c) {
    if (!c->p) return 1;
    for (int i = 1; i <= 16; i++) if (c->p[-i] != 0xA5 || c->p[c->len + (u64)i - 1] != 0xA5) return 0;
    return 1;
}

/* Fills args for one call; returns the number of canaries armed. */
static int gen_args(W *w, const Spec *sp, u64 a[6], Canary can[2]) {
    Rng *r = &w->rng;
    int ncan = 0;
    int nk = (int)strlen(sp->kinds);
    u64 len = 0;
    int have_len = 0;
    for (int i = 0; i < nk; i++) a[i] = 0;
    /* first the plain values, so that the lengths and flags are known when the pointers are chosen */
    for (int i = 0; i < nk; i++) {
        switch (sp->kinds[i]) {
        case 'i': a[i] = gen_int(w); break;
        case 'f': a[i] = gen_fd(w); w->last_fd = (int)a[i]; break;
        case 'F': a[i] = pct(r, 80) ? below(r, 12) + (pct(r, 20) ? 50 : 0) : gen_int(w); break;
        case 'l': len = gen_len(w, BUFAREA); a[i] = len; have_len = 1; break;
        case 'k': a[i] = pct(r, 80) ? below(r, 20) : gen_int(w); break;
        case 'm': a[i] = pct(r, 70) ? below(r, 0777 + 1) : (pct(r, 50) ? rnd(r) & 0xffffffffUL : rnd(r)); break;
        case 'q':
            if (sp->nr == SYS_open) {
                static const u32 fl[] = { O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_EXCL, O_TRUNC, O_APPEND, O_NONBLOCK, O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC, O_NOCTTY };
                u64 v = below(r, 4);
                for (int j = (int)below(r, 4); j > 0; j--) v |= fl[below(r, sizeof fl / sizeof fl[0])];
                a[i] = pct(r, 10) ? rnd(r) : v;
            } else a[i] = pct(r, 60) ? below(r, 16) : (pct(r, 50) ? (1UL << below(r, 64)) : rnd(r));
            break;
        case 'g': a[i] = gen_sig(w); break;
        case 'd': a[i] = gen_pid(w); break;
        case 'u': a[i] = pct(r, 40) ? below(r, 2000) : (pct(r, 50) ? 0xffffffffUL : gen_int(w)); break;
        case 'O': a[i] = pct(r, 60) ? below(r, 3 * PG) : gen_int(w); break;
        case 'I': a[i] = pct(r, 70) ? (pct(r, 50) ? 0 : below(r, 20)) : gen_int(w); break;
        case 'w': a[i] = pct(r, 85) ? below(r, 3) : gen_int(w); break;
        case 'c': a[i] = gen_cmd(w, sp); break;
        case 'r': a[i] = pct(r, 70) ? below(r, 8) : (pct(r, 50) ? 0xffffffffUL : rnd(r)); break;
        case 'M': a[i] = pct(r, 50) ? (MAP_PRIVATE | MAP_ANONYMOUS) | (pct(r, 20) ? MAP_FIXED : 0) : (pct(r, 50) ? below(r, 0x40) : rnd(r)); break;
        case 'a':
            switch (below(r, 12)) {
            case 0: a[i] = 0; break;
            case 1: case 2: a[i] = (u64)w->rw + (below(r, RWLEN / PG) << 12) + (pct(r, 20) ? below(r, 4096) : 0); break;
            case 3: a[i] = (u64)(pct(r, 50) ? w->small : w->ro) + below(r, 3) * PG; break;
            case 4: a[i] = sp->nr == SYS_mmap ? USER_TOP - below(r, 4) * PG : 0x100000000000UL + below(r, 64) * PG; break;       /* never our own stack */
            case 5: a[i] = 0x100000000000UL + (below(r, 1 << 20) << 12); break;           /* the mmap area */
            case 6: a[i] = g_edge[below(r, NEDGE)]; break;
            case 7: a[i] = (u64)w->stk - (16 + below(r, 200)) * PG; break;                  /* unused stack pages far below the live ones */
            default: a[i] = below(r, 1 << 30) << 12; break;
            }
            if (sp->nr == SYS_mmap && !(a[3] & MAP_FIXED) && pct(r, 70)) a[i] = 0;
            if (sp->nr == SYS_brk && pct(r, 70)) a[i] = 0;
            break;
        default: break;
        }
    }
    if (sp->nr == SYS_mmap) {                                /* mostly sane lengths and offsets, protections from the real set */
        if (pct(r, 70)) a[1] = (1 + below(r, 64)) * PG;
        if (pct(r, 60)) a[2] = below(r, 8);
        if (pct(r, 60)) a[5] = below(r, 4) * PG;
        if (a[3] & MAP_ANONYMOUS) { if (pct(r, 80)) a[4] = (u64)-1L; }
        else if (!(pct(r, 30))) a[3] = (a[3] & ~(u64)MAP_ANONYMOUS) | MAP_PRIVATE | (pct(r, 30) ? MAP_SHARED : 0);
        /* never map over our own stack or the structures we need unless the dice insist: MAP_FIXED over the worker is part of the fun */
        if ((a[3] & MAP_FIXED) && pct(r, 70)) a[0] = 0x100000000000UL + (below(r, 1 << 12) << 12);
    }
    if (sp->nr == SYS_munmap || sp->nr == SYS_mprotect) {
        if (pct(r, 60)) a[1] = (1 + below(r, 4)) * PG;
        if (pct(r, 80) && a[0] >= (u64)w->rw && a[0] < (u64)w->rw + RWLEN) a[0] = 0x100000000000UL + (below(r, 1 << 12) << 12);   /* spare the big buffer mostly */
    }
    if (sp->nr == SYS_futex && pct(r, 70)) a[0] = (u64)(pct(r, 60) ? w->shm ? w->shm : w->rw : w->rw) + (below(r, 64) << 2);
    if (sp->nr == SYS_thread_create) { a[0] = pct(r, 90) ? 0 : a[0]; a[1] = (u64)w->rw + 0x18000; a[4] = (u64)w->rwx; }
    /* now the pointers */
    for (int i = 0; i < nk; i++) {
        switch (sp->kinds[i]) {
        case 'p': a[i] = gen_path(w, g_cfg.root && sp->nr == SYS_open && (a[1] & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND | 3))); break;
        case 'P': a[i] = gen_path(w, 1); break;
        case 'b': a[i] = gen_ptr(w, have_len ? len : 16, 0); break;
        case 'B': {
            u64 need = have_len ? len : (sp->nr == SYS_readdir ? sizeof(struct gdirent) : 16);
            if (sp->nr == SYS_readdir) need = a[i + 1] * sizeof(struct gdirent);
            a[i] = gen_ptr(w, need, 1);
            u64 p = a[i];
            if (ncan < 2 && p >= (u64)w->rw + 32 && need < BUFAREA - 128 && p + need + 16 <= (u64)w->rw + BUFAREA && need) {
                canary_set((u8 *)p, need, &can[ncan]); ncan++;
            }
            break;
        }
        case 'o': {
            u64 need = sp->osz ? sp->osz : 8;
            a[i] = gen_ptr(w, need, 1);
            u64 p = a[i];
            if (ncan < 2 && p >= (u64)w->rw + 32 && p + need + 16 <= (u64)w->rw + BUFAREA) { canary_set((u8 *)p, need, &can[ncan]); ncan++; }
            break;
        }
        case 's': case 'x': a[i] = gen_struct(w, sp->kinds[i], sp, 0); break;
        case 'X': case 'v': {
            a[i] = gen_struct(w, sp->kinds[i], sp, 0);
            break;
        }
        case 't': a[i] = gen_struct(w, 't', sp, 0); break;
        default: break;
        }
    }
    /* overlapping buffers: point a second pointer argument at (or next to) the first */
    if (pct(r, 6)) {
        int first = -1;
        for (int i = 0; i < nk; i++) {
            char k = sp->kinds[i];
            if (k == 'b' || k == 'B' || k == 'p' || k == 'P' || k == 'o') {
                if (first < 0) first = i;
                else { a[i] = a[first] + (pct(r, 50) ? 0 : below(r, 64)); break; }
            }
        }
    }
    return ncan;
}

/* Runs one call in a grandchild that is given a moment to finish (exit, exec, calls that may sleep for ever). */
static long wrapped_call(W *w, u64 nr, u64 a[6]) {
    long pid = SC0(SYS_fork);
    if (pid == 0) {
        sc((long)nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        SC1(SYS_exit_group, 0);
        for (;;) { }
    }
    if (pid < 0) return pid;
    for (int i = 0; i < 8; i++) {
        int st;
        long r = sc(SYS_waitpid, (u64)pid, (u64)&st, WNOHANG, 0, 0, 0);
        if (r == pid || r < 0) return 0;
        sleep_ms(5);
    }
    SC2(SYS_kill, pid, SIGKILL);
    int st;
    sc(SYS_waitpid, (u64)pid, (u64)&st, 0, 0, 0, 0);
    (void)w;
    return 0;
}

/* The saved frame of a signal handler, as signal.c lays it out: restorer, TrapFrame, mask, signal number. */
typedef struct { u64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, vector, error, rip, cs, rflags, rsp, ss; } TFrame;
typedef struct { u64 restorer; TFrame saved; u32 saved_mask; u32 signo; } SigFrame;

static W *g_sigw;
static volatile int g_mutate_level;

static void on_mutate(int sig) {
    (void)sig;
    SigFrame *sf = (SigFrame *)((u8 *)__builtin_frame_address(0) + 8);
    W *w = g_sigw;
    Rng *r = &w->rng;
    int level = g_mutate_level;
    if (level & 1) sf->saved.rflags = pct(r, 50) ? rnd(r) : (sf->saved.rflags | (1UL << (8 + below(r, 12))));
    if (level & 2) { sf->saved.cs = rnd(r); sf->saved.ss = rnd(r); }
    if (level & 4) sf->saved.rax = rnd(r);
    if (level & 8) { static const u64 bad[] = { 0x800000000000UL, 0x8000000000000000UL, 0xffffffff80000000UL, 0xffffc00000000000UL, 0xffffffffffffffffUL, 0, USER_TOP }; sf->saved.rip = bad[below(r, 7)]; }
    if (level & 16) { static const u64 bad[] = { 0x800000000000UL, 0x8000000000000000UL, 0xffffffff80000000UL, 0xffffffffffffffffUL, 0, 1, USER_TOP, 0xffff800000000000UL }; sf->saved.rsp = bad[below(r, 8)]; }
    if (level & 32) { sf->saved_mask = (u32)rnd(r); sf->signo = (u32)rnd(r); }
}

static void fuzz_sigreturn(W *w) {
    struct gsigaction act;
    act.handler = (u64)on_mutate;
    act.flags = 0;
    act.mask = 0;
    extern void __restore_rt(void);
    act.restorer = (u64)__restore_rt;
    SC3(SYS_sigaction, SIGUSR1, &act, 0);
    g_sigw = w;
    /* harmless mutations most of the time; the ones that make the process crash after the return (bad rip, bad rsp) rarely */
    int lv = (int)below(&w->rng, 64);
    if ((lv & 8) && !pct(&w->rng, 25)) lv &= ~8;
    if ((lv & 16) && !pct(&w->rng, 25)) lv &= ~16;
    g_mutate_level = lv;
    SC2(SYS_kill, SC0(SYS_getpid), SIGUSR1);
    g_mutate_level = 0;
    act.handler = 0;                                        /* SIG_DFL */
    SC3(SYS_sigaction, SIGUSR1, &act, 0);
}

static void ring_push(Slot *s, u64 nr, const u64 a[6], long ret) {
    Entry *e = &s->ring[s->ring_pos & (RING - 1)];
    e->nr = nr;
    for (int i = 0; i < 6; i++) e->a[i] = a[i];
    e->ret = ret;
    s->ring_pos++;
}

static void trace_call(const char *name, u64 nr, const u64 a[6]) {
    char line[200];
    int n = 0;
    const char *hex = "0123456789abcdef";
    line[n++] = 'T'; line[n++] = ' ';
    for (const char *s = name; *s && n < 40; s++) line[n++] = *s;
    for (int i = 0; i < 6; i++) {
        line[n++] = ' ';
        line[n++] = '0'; line[n++] = 'x';
        int started = 0;
        for (int sh = 60; sh >= 0; sh -= 4) {
            int d = (int)((a[i] >> sh) & 15);
            if (d || started || sh == 0) { line[n++] = hex[d]; started = 1; }
        }
    }
    (void)nr;
    line[n++] = '\n';
    SC3(SYS_write, 2, line, n);
}

static int g_nthreads_started;

static void fuzz_loop(W *w, long ncalls);

static void thread_main(u64 arg) {
    W *w = (W *)arg;
    fuzz_loop(w, 150);
    SC1(SYS_thread_exit, 0);
    for (;;) { }
}

static void start_thread(W *w) {
    static u8 stacks[3][65536] __attribute__((aligned(16)));
    static W twins[3];
    int k = g_nthreads_started;
    if (k >= 3) return;
    g_nthreads_started++;
    twins[k] = *w;
    twins[k].depth = 2;
    twins[k].rng.s = rnd(&w->rng);
    twins[k].nfds = w->nfds;
    long tid = sc(SYS_thread_create, 0, (u64)(stacks[k] + 65536 - 8), 0, 0, (u64)thread_main, (u64)&twins[k]);
    (void)tid;
}

/* ---------------------------------------------------------------- scenarios: runs of related calls that random ones rarely form */

/* One call of a scenario: recorded for the post-mortem like any other. */
static long call(W *w, long nr, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    Slot *s = w->slot;
    u64 args[6] = { a, b, c, d, e, f };
    if (g_cfg.trace) trace_call("scenario", (u64)nr, args);
    s->cur_nr = (u64)nr;
    for (int i = 0; i < 6; i++) s->cur_a[i] = args[i];
    s->in_call = 1;
    long ret = sc(nr, a, b, c, d, e, f);
    s->in_call = 0;
    s->calls++;
    ring_push(s, (u64)nr, args, ret);
    if (ret < -4095) note_violation(w, "error result out of range", 0);
    return ret;
}

static const u64 g_offs[] = {
    0, 1, 4095, 4096, 4097, 0x10000, 0x100000, 0x7fffffffUL, 0x80000000UL, 0xffffffffUL, 0x100000000UL, 0x100000001UL, 0x10000000000UL,
    0x100000000000UL, 0xfffffffff000UL, 0x1000000000000UL, 0x4000000000000000UL, 0x7ffffffffffff000UL, 0x7fffffffffffffffUL,
    0x8000000000000000UL, 0xfffffffffffff000UL, 0xffffffffffffffffUL,
};
#define NOFFS ((int)(sizeof g_offs / sizeof g_offs[0]))

static u64 gen_off(W *w) { return pct(&w->rng, 25) ? below(&w->rng, 3 * PG) : g_offs[below(&w->rng, NOFFS)]; }

static void scratch_path(W *w, char *out, const char *name) {
    int n = 0;
    for (const char *s = w->scratch; *s; s++) out[n++] = *s;
    out[n++] = '/';
    for (const char *s = name; *s; s++) out[n++] = *s;
    out[n] = 0;
}

/* A regular file seen from every side: seek, write, read, truncate, map, stat, unlink, with offsets at every power of two. */
static void scn_file(W *w) {
    Rng *r = &w->rng;
    char path[160], name[8] = { 's', (char)('0' + below(r, 4)), 0 };
    scratch_path(w, path, name);
    long fd = call(w, SYS_open, (u64)path, O_RDWR | O_CREAT | (pct(r, 20) ? O_APPEND : 0) | (pct(r, 15) ? O_TRUNC : 0), 0666, 0, 0, 0);
    if (fd < 0) return;
    u8 *buf = w->rw + 64 + below(r, 0x8000);
    for (int i = 0; i < 64; i++) buf[i] = (u8)('a' + i % 26);
    int n = 4 + (int)below(r, 8);
    for (int i = 0; i < n; i++) {
        switch (below(r, 10)) {
        case 0: call(w, SYS_lseek, (u64)fd, gen_off(w), below(r, 4), 0, 0, 0); break;
        case 1: call(w, SYS_write, (u64)fd, (u64)buf, pct(r, 70) ? below(r, 200) : gen_len(w, 0x8000), 0, 0, 0); break;
        case 2: call(w, SYS_read, (u64)fd, (u64)buf, pct(r, 70) ? below(r, 200) : gen_len(w, 0x8000), 0, 0, 0); break;
        case 3: call(w, SYS_ftruncate, (u64)fd, gen_off(w), 0, 0, 0, 0); break;
        case 4: {
            u64 len = pct(r, 70) ? (1 + below(r, 4)) * PG : gen_len(w, 0);
            u64 prot = below(r, 8);
            long m = call(w, SYS_mmap, 0, len, prot, (pct(r, 50) ? MAP_SHARED : MAP_PRIVATE), (u64)fd, pct(r, 70) ? below(r, 2) * PG : gen_off(w));
            if (m > 0 && m < (long)USER_TOP && pct(r, 50)) { if ((prot & 1) && len <= 4 * PG && gen_off(w) < 4 * PG) { volatile u8 *p = (volatile u8 *)m; (void)p[0]; } call(w, SYS_munmap, (u64)m, len, 0, 0, 0, 0); }
            break;
        }
        case 5: call(w, SYS_fstat, (u64)fd, (u64)(w->rw + 0x1000), 0, 0, 0, 0); break;
        case 6: call(w, SYS_truncate, (u64)path, gen_off(w), 0, 0, 0, 0); break;
        case 7: call(w, SYS_dup2, (u64)fd, 20 + below(r, 5), 0, 0, 0, 0); break;
        case 8: call(w, SYS_fcntl, (u64)fd, below(r, 6), below(r, 3000), 0, 0, 0); break;
        default: call(w, SYS_unlink, (u64)path, 0, 0, 0, 0, 0); break;
        }
    }
    call(w, SYS_close, (u64)fd, 0, 0, 0, 0, 0);
    call(w, SYS_unlink, (u64)path, 0, 0, 0, 0, 0);
}

/* Sockets of both families: bind/listen/connect/accept, data in both directions in odd sizes, options, shutdown in any order. */
static void scn_sock(W *w) {
    Rng *r = &w->rng;
    int inet = pct(r, 55);
    int dgram = inet && pct(r, 40);
    u8 *sa = w->rw + STRUCTAREA + 0xa00;
    u8 *sb = w->rw + STRUCTAREA + 0xb00;
    u8 *buf = w->rw + 64 + below(r, 0x4000);
    char path[160];
    for (int i = 0; i < 100; i++) { sa[i] = 0; sb[i] = 0; }
    if (inet) {
        unsigned port = 0x5300 + (unsigned)below(r, 8) + (unsigned)w->idx * 16;
        sockaddr_in(sa, port);
        sockaddr_in(sb, port);
        if (pct(r, 10)) put_u32(sb + 4, (u32)rnd(r));
    } else {
        scratch_path(w, path, "u0");
        put_u16(sa, 1);
        for (int i = 0; path[i]; i++) sa[2 + i] = (u8)path[i];
        for (int i = 0; i < 100; i++) sb[i] = sa[i];
    }
    long srv = call(w, SYS_socket, inet ? AF_INET : AF_UNIX, (dgram ? SOCK_DGRAM : SOCK_STREAM) | SOCK_NONBLOCK, 0, 0, 0, 0);
    long cli = call(w, SYS_socket, inet ? AF_INET : AF_UNIX, (dgram ? SOCK_DGRAM : SOCK_STREAM) | (pct(r, 80) ? SOCK_NONBLOCK : 0), 0, 0, 0, 0);
    if (srv < 0 || cli < 0) { if (srv >= 0) call(w, SYS_close, (u64)srv, 0, 0, 0, 0, 0); if (cli >= 0) call(w, SYS_close, (u64)cli, 0, 0, 0, 0, 0); return; }
    call(w, SYS_bind, (u64)srv, (u64)sa, inet ? 16 : 110, 0, 0, 0);
    if (!dgram) call(w, SYS_listen, (u64)srv, below(r, 10), 0, 0, 0, 0);
    call(w, SYS_connect, (u64)cli, (u64)sb, inet ? 16 : 110, 0, 0, 0);
    long acc = dgram ? -1 : call(w, SYS_accept, (u64)srv, 0, 0, 0, 0, 0);
    long fds[3] = { srv, cli, acc };
    int n = 4 + (int)below(r, 12);
    for (int i = 0; i < n; i++) {
        long f = fds[below(r, 3)];
        if (f < 0) f = cli;
        switch (below(r, 9)) {
        case 0: call(w, SYS_write, (u64)f, (u64)buf, pct(r, 80) ? below(r, 3000) : gen_len(w, 0x4000), 0, 0, 0); break;
        case 1: call(w, SYS_read, (u64)f, (u64)buf, pct(r, 80) ? below(r, 3000) : gen_len(w, 0x4000), 0, 0, 0); break;
        case 2: call(w, SYS_sendto, (u64)f, (u64)buf, below(r, 70000), pct(r, 30) ? 0x4000 : below(r, 0x8000), pct(r, 50) ? (u64)sb : 0, pct(r, 90) ? 16 : gen_int(w)); break;
        case 3: call(w, SYS_recvfrom, (u64)f, (u64)buf, below(r, 70000), pct(r, 30) ? 0x42 : below(r, 0x8000), pct(r, 50) ? (u64)(w->rw + STRUCTAREA + 0xc00) : 0, (u64)(w->rw + STRUCTAREA + 0xd00)); break;
        case 4: call(w, SYS_setsockopt, (u64)f, below(r, 3) ? 1 : below(r, 20), below(r, 25), (u64)buf, below(r, 20), 0); break;
        case 5: call(w, SYS_getsockopt, (u64)f, below(r, 3) ? 1 : below(r, 20), below(r, 25), (u64)(w->rw + STRUCTAREA + 0xc00), (u64)(w->rw + STRUCTAREA + 0xd00), 0); break;
        case 6: call(w, SYS_shutdown, (u64)f, below(r, 4), 0, 0, 0, 0); break;
        case 7: call(w, SYS_getsockname, (u64)f, (u64)(w->rw + STRUCTAREA + 0xc00), (u64)(w->rw + STRUCTAREA + 0xd00), 0, 0, 0); break;
        default: call(w, SYS_poll, (u64)(w->rw + STRUCTAREA + 0xe00), 1, 0, 0, 0, 0); break;
        }
    }
    for (int i = 0; i < 3; i++) if (fds[i] >= 0) call(w, SYS_close, (u64)fds[i], 0, 0, 0, 0, 0);
    if (!inet) call(w, SYS_unlink, (u64)path, 0, 0, 0, 0, 0);
}

/* A pseudo-terminal pair: line discipline with control characters, termios changes, window sizes, closing either end first. */
static void scn_pty(W *w) {
    Rng *r = &w->rng;
    char slave[48];
    long m = call(w, SYS_ptyopen, (u64)slave, sizeof slave, 0, 0, 0, 0);
    if (m < 0) return;
    long s = call(w, SYS_open, (u64)slave, O_RDWR | O_NONBLOCK | (pct(r, 20) ? O_NOCTTY : 0), 0, 0, 0, 0);
    u8 *buf = w->rw + 64 + below(r, 0x4000);
    static const u8 ctl[] = { 3, 26, 28, 4, '\n', '\r', 'a', 'b', 0x7f, 0x15, 0x17, 0x1b, 0, 0xff, 0x08, '\t', 0x13, 0x11 };
    int n = 6 + (int)below(r, 14);
    for (int i = 0; i < n; i++) {
        long f = pct(r, 50) ? m : s;
        if (f < 0) f = m;
        switch (below(r, 9)) {
        case 0: { int k = 1 + (int)below(r, 12); for (int j = 0; j < k; j++) buf[j] = pct(r, 60) ? ctl[below(r, sizeof ctl)] : (u8)rnd(r); call(w, SYS_write, (u64)f, (u64)buf, (u64)k, 0, 0, 0); break; }
        case 1: call(w, SYS_read, (u64)f, (u64)buf, pct(r, 70) ? below(r, 200) : gen_len(w, 0x4000), 0, 0, 0); break;
        case 2: { u8 *t = w->rw + STRUCTAREA + 0xf00; for (int j = 0; j < 32; j++) t[j] = (u8)(pct(r, 40) ? rnd(r) : 0); call(w, SYS_ioctl, (u64)f, pct(r, 70) ? TCSETS : (pct(r, 50) ? TCSETSW : TCSETSF), (u64)t, 0, 0, 0); break; }
        case 3: call(w, SYS_ioctl, (u64)f, TCGETS, (u64)(w->rw + STRUCTAREA + 0xf00), 0, 0, 0); break;
        case 4: { u8 *t = w->rw + STRUCTAREA + 0xf00; put_u16(t, (unsigned)rnd(r)); put_u16(t + 2, (unsigned)rnd(r)); call(w, SYS_ioctl, (u64)f, TIOCSWINSZ, (u64)t, 0, 0, 0); break; }
        case 5: { u8 *t = w->rw + STRUCTAREA + 0xf00; put_u32(t, pct(r, 50) ? (u32)SC0(SYS_getpid) : (u32)gen_pid(w)); call(w, SYS_ioctl, (u64)f, pct(r, 60) ? TIOCSPGRP : TIOCGPGRP, (u64)t, 0, 0, 0); break; }
        case 6: call(w, SYS_ioctl, (u64)f, FIONREAD, (u64)(w->rw + STRUCTAREA + 0xf00), 0, 0, 0); break;
        case 7: call(w, SYS_poll, (u64)(w->rw + STRUCTAREA + 0xe00), 1, 0, 0, 0, 0); break;
        default: call(w, SYS_ioctl, (u64)f, TIOCGNAME, (u64)(w->rw + STRUCTAREA + 0xf00), 0, 0, 0); break;
        }
    }
    if (pct(r, 50)) { call(w, SYS_close, (u64)m, 0, 0, 0, 0, 0); if (s >= 0) call(w, SYS_close, (u64)s, 0, 0, 0, 0, 0); }
    else { if (s >= 0) call(w, SYS_close, (u64)s, 0, 0, 0, 0, 0); call(w, SYS_close, (u64)m, 0, 0, 0, 0, 0); }
}

/* Processes: children that exit, crash, sleep or are killed; wait in every flavour; groups; zombies reaped in odd order. */
static void scn_proc(W *w) {
    Rng *r = &w->rng;
    if (w->depth > 0) return;
    long kids[4];
    int nk = 0;
    int want = 1 + (int)below(r, 4);
    for (int i = 0; i < want; i++) {
        long pid = call(w, SYS_fork, 0, 0, 0, 0, 0, 0);
        if (pid == 0) {                                      /* child */
            switch (below(r, 5)) {
            case 0: call(w, SYS_exit, (u64)below(r, 256) << 8, 0, 0, 0, 0, 0); break;
            case 1: *(volatile int *)0 = 1; break;
            case 2: { struct gtimespec ts = { 0, (long)(below(r, 20) * 1000000) }; call(w, SYS_nanosleep, (u64)&ts, 0, 0, 0, 0, 0); call(w, SYS_exit, 0, 0, 0, 0, 0, 0); break; }
            case 3: call(w, SYS_pause, 0, 0, 0, 0, 0, 0); break;
            default: call(w, SYS_setpgid, 0, 0, 0, 0, 0, 0); call(w, SYS_exit, 0, 0, 0, 0, 0, 0); break;
            }
            SC1(SYS_exit_group, 0);
            for (;;) { }
        }
        if (pid > 0 && nk < 4) kids[nk++] = pid;
    }
    int n = 3 + (int)below(r, 8);
    for (int i = 0; i < n && nk; i++) {
        long k = kids[below(r, (u64)nk)];
        switch (below(r, 7)) {
        case 0: call(w, SYS_kill, (u64)k, gen_sig(w), 0, 0, 0, 0); break;
        case 1: call(w, SYS_waitpid, (u64)k, (u64)(w->rw + STRUCTAREA + 0xf00), pct(r, 50) ? WNOHANG : WNOHANG | WUNTRACED | WCONTINUED, 0, 0, 0); break;
        case 2: call(w, SYS_waitpid, (u64)-1L, (u64)(w->rw + STRUCTAREA + 0xf00), WNOHANG, 0, 0, 0); break;
        case 3: call(w, SYS_setpgid, (u64)k, pct(r, 50) ? 0 : (u64)k, 0, 0, 0, 0); break;
        case 4: call(w, SYS_kill, (u64)-k, gen_sig(w), 0, 0, 0, 0); break;
        case 5: call(w, SYS_getpgid, (u64)k, 0, 0, 0, 0, 0); break;
        default: { struct gtimespec ts = { 0, 2000000 }; call(w, SYS_nanosleep, (u64)&ts, 0, 0, 0, 0, 0); break; }
        }
    }
    for (int i = 0; i < nk; i++) { call(w, SYS_kill, (u64)kids[i], SIGKILL, 0, 0, 0, 0); call(w, SYS_waitpid, (u64)kids[i], (u64)(w->rw + STRUCTAREA + 0xf00), 0, 0, 0, 0); }
}

/* Address space surgery: overlapping and fixed mappings, protections, brk, the stack guard, fork with all of it. */
static void scn_mem(W *w) {
    Rng *r = &w->rng;
    u64 base = 0x200000000000UL + (below(r, 1 << 10) << 24);
    u64 live[6][2];
    int nl = 0;
    int n = 6 + (int)below(r, 10);
    for (int i = 0; i < n; i++) {
        switch (below(r, 8)) {
        case 0: case 1: case 2: {
            u64 len = pct(r, 80) ? (1 + below(r, 16)) * PG : gen_len(w, 0);
            u64 addr = pct(r, 60) ? base + (below(r, 64) << 12) : 0;
            u64 prot = below(r, 8);
            long m = call(w, SYS_mmap, addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | (addr && pct(r, 70) ? MAP_FIXED : 0), (u64)-1L, 0);
            if (m > 0 && m < (long)USER_TOP && nl < 6) { live[nl][0] = (u64)m; live[nl][1] = len; nl++; if ((prot & 2) && pct(r, 60)) *(volatile u8 *)m = 1; }
            break;
        }
        case 3: if (nl) { int k = (int)below(r, (u64)nl); call(w, SYS_munmap, live[k][0] + (pct(r, 30) ? PG : 0), pct(r, 70) ? live[k][1] : gen_len(w, 0), 0, 0, 0, 0); } break;
        case 4: if (nl) { int k = (int)below(r, (u64)nl); call(w, SYS_mprotect, live[k][0] + (pct(r, 30) ? PG : 0), pct(r, 70) ? live[k][1] : gen_len(w, 0), below(r, 8), 0, 0, 0); } break;
        case 5: call(w, SYS_brk, pct(r, 50) ? 0 : base + (below(r, 4) << 20), 0, 0, 0, 0, 0); break;
        case 6: {                                            /* a fork with a populated address space, the child touches every mapping and leaves */
            if (w->nforks < 6 && w->depth == 0) {
                w->nforks++;
                long pid = call(w, SYS_fork, 0, 0, 0, 0, 0, 0);
                if (pid == 0) { SC1(SYS_exit_group, 0); for (;;) { } }
                if (pid > 0) call(w, SYS_waitpid, (u64)pid, 0, 0, 0, 0, 0);
            }
            break;
        }
        default: call(w, SYS_mmap, 0, 0x7fffffffffffUL, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, (u64)-1L, 0); break;
        }
    }
    for (int k = 0; k < nl; k++) call(w, SYS_munmap, live[k][0], live[k][1], 0, 0, 0, 0);
}

/* execve with argument vectors at and beyond the limits (kernel/include/process.h), in a child. */
static void scn_exec(W *w) {
    Rng *r = &w->rng;
    if (w->depth > 0) return;
    u64 count = pct(r, 30) ? below(r, 40000) : below(r, 40);
    u64 slen = pct(r, 25) ? 100000 + below(r, 60000) : below(r, 64);
    u64 vbytes = (count + 8) * 8;
    u8 *vec = map_region(vbytes + PG, PROT_READ | PROT_WRITE);
    u8 *str = map_region(slen + 2 * PG, PROT_READ | PROT_WRITE);
    if (!vec || !str) return;
    for (u64 i = 0; i < slen; i++) str[i] = 'x';
    str[slen] = 0;
    u64 *v = (u64 *)vec;
    v[0] = (u64)"/usr/lib/kernel-tests/sysfuzz";
    v[1] = (u64)"--noop";
    for (u64 i = 0; i < count; i++) v[2 + i] = (u64)str + (pct(r, 90) ? 0 : slen);
    v[2 + count] = 0;
    long pid = call(w, SYS_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) {
        call(w, SYS_execve, (u64)v[0], (u64)v, pct(r, 50) ? (u64)v : 0, 0, 0, 0);
        SC1(SYS_exit_group, 0);
        for (;;) { }
    }
    if (pid > 0) call(w, SYS_waitpid, (u64)pid, 0, 0, 0, 0, 0);
    SC2(SYS_munmap, vec, vbytes + PG);
    SC2(SYS_munmap, str, slen + 2 * PG);
}

static void scenario(W *w) {
    switch (below(&w->rng, 6)) {
    case 0: scn_file(w); break;
    case 1: scn_sock(w); break;
    case 2: scn_pty(w); break;
    case 3: scn_proc(w); break;
    case 4: scn_mem(w); break;
    default: if (pct(&w->rng, 40)) scn_exec(w); else scn_file(w); break;
    }
}

static void one_call(W *w) {
    Rng *r = &w->rng;
    Slot *slot = w->slot;
    const Spec *sp = 0;
    u64 nr, a[6];
    Canary can[2] = { { 0, 0 }, { 0, 0 } };
    int ncan = 0;
    if (w->depth <= 1 && pct(r, 10)) { scenario(w); return; }
    int pick = (int)below(r, 100);
    if (pick < 90) {
        int total = 0;
        for (int i = 0; i < NSPECS; i++) total += g_specs[i].weight;
        int t = (int)below(r, (u64)total);
        for (int i = 0; i < NSPECS; i++) {
            t -= g_specs[i].weight;
            if (t < 0) { sp = &g_specs[i]; break; }
        }
        if (!sp) sp = &g_specs[0];
        nr = (u64)sp->nr;
    } else if (pick < 97) {
        nr = below(r, 140);
        for (int i = 0; i < NSPECS; i++) if ((u64)g_specs[i].nr == nr) sp = &g_specs[i];
    } else {
        nr = pct(r, 50) ? g_edge[below(r, NEDGE)] : rnd(r);
    }
    if (sp && ((sp->flags & SF_SKIP) || ((sp->flags & SF_ROOT) && !g_cfg.root && pct(r, 90)))) return;
    if (!sp) {                                               /* a number with no entry in the table: anything goes */
        for (int i = 0; i < 6; i++) a[i] = pct(r, 50) ? gen_int(w) : gen_ptr(w, 64, 1);
        sp = &g_specs[0];
        if (nr == (u64)SYS_exit || nr == (u64)SYS_exit_group || nr == (u64)SYS_thread_exit || nr == (u64)SYS_reboot || nr == (u64)SYS_execve ||
            nr == (u64)SYS_pause || nr == (u64)SYS_sigsuspend || nr == (u64)SYS_sigsuspend || nr == (u64)SYS_sigreturn || nr == (u64)SYS_fork)
            return;
    } else {
        ncan = gen_args(w, sp, a, can);
    }
    if (nr == (u64)SYS_sigreturn) { fuzz_sigreturn(w); return; }
    if (nr == (u64)SYS_fork) {
        if (w->nforks >= 6 || w->depth > 0) return;
        w->nforks++;
    }
    if (nr == (u64)SYS_thread_create && sp->nr == SYS_thread_create) {            /* a thread with a sane body instead of a random entry */
        if (w->depth == 0 && pct(r, 40)) start_thread(w);
        return;
    }
    if (g_cfg.trace) trace_call(sp->name, nr, a);
    slot->cur_nr = nr;
    for (int i = 0; i < 6; i++) slot->cur_a[i] = a[i];
    slot->in_call = 1;
    long ret;
    if (sp->flags & SF_WRAP) ret = wrapped_call(w, nr, a);
    else ret = sc((long)nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    slot->in_call = 0;
    slot->calls++;
    ring_push(slot, nr, a, ret);

    if (nr == (u64)SYS_fork && ret > 0 && w->npids < 16) w->pids[w->npids++] = (int)ret;
    if (nr == (u64)SYS_fork && ret == 0) {                   /* the child of a fuzzed fork: shorter life, no further forks */
        w->depth = 1;
        w->rng.s ^= 0x5555 + (u64)SC0(SYS_getpid);
        w->budget = 60;
    }
    if (ret < 0 && ret > -4096) { /* an error, as expected */ }
    else if (ret < -4095) note_violation(w, "error result out of range", sp);
    if (nr == (u64)SYS_mmap && ret > 0 && (((u64)ret & (PG - 1)) || (u64)ret >= USER_TOP)) note_violation(w, "mmap returned a bad address", sp);
    if (nr == (u64)SYS_getpid && ret != SC0(SYS_getpid)) note_violation(w, "getpid changed", sp);
    for (int i = 0; i < ncan; i++)
        if (!canary_ok(&can[i])) note_violation(w, "the kernel wrote outside the buffer it was given", sp);
    if ((nr == (u64)SYS_open || nr == (u64)SYS_dup || nr == (u64)SYS_socket || nr == (u64)SYS_accept) && ret >= 0) { pool_add(w, ret); w->last_fd = (int)ret; }
    /* related calls in a row find more than random ones: a handler that was just installed gets its signal, a fresh mapping is used */
    if (nr == (u64)SYS_sigaction && ret == 0 && a[1] && pct(r, 50) && a[0] != SIGUSR1 && a[0] < NSIG) {
        SC2(SYS_kill, SC0(SYS_getpid), a[0]);
    }
    if (nr == (u64)SYS_mmap && ret > 0 && ret < (long)USER_TOP && pct(r, 50)) {
        volatile u8 *m = (volatile u8 *)ret;
        u64 len = a[1] > 2 * PG ? 2 * PG : a[1];
        if ((a[2] & PROT_READ) && (a[3] & MAP_ANONYMOUS) && !(a[3] & MAP_FIXED)) { (void)m[0]; if (len > 1) (void)m[len - 1]; }
        if (pct(r, 50)) SC2(SYS_munmap, ret, a[1]);
        else SC3(SYS_mprotect, ret, a[1], below(r, 8));
    }
    if (nr == (u64)SYS_pipe && ret == 0) { int *pf = (int *)a[0]; if (a[0] >= (u64)w->rw && a[0] < (u64)w->rw + BUFAREA) { pool_add(w, pf[0]); pool_add(w, pf[1]); } }
}

static void fuzz_loop(W *w, long ncalls) {
    for (long i = 0; i < ncalls && w->budget > 0 && !g_sh->stop; i++) {
        one_call(w);
        w->budget--;
    }
}

/* Interrupts a call that sleeps for ever: a helper thread nudges the main thread now and then, which turns a sleeping call into
 * EINTR (when SIGUSR2 still has the do-nothing handler the worker started with). */
static void nudge_handler(int sig) { (void)sig; }

static void watchdog_main(u64 arg) {
    Slot *s = (Slot *)arg;
    u64 last = ~0UL;
    int still = 0;
    for (;;) {
        sleep_ms(60);
        if (s->done) break;
        if (s->calls == last && s->in_call) {
            if (++still >= 2) SC3(SYS_tgkill, s->pid, s->main_tid, SIGUSR2);
        } else { still = 0; last = s->calls; }
    }
    SC1(SYS_thread_exit, 0);
    for (;;) { }
}

static void drop_privileges(void) {
    SC1(SYS_setgid, FUZZ_UID);
    SC1(SYS_setuid, FUZZ_UID);
}

static void worker_main(int widx, u64 seed) {
    static W w;
    static u8 wd_stack[16384] __attribute__((aligned(16)));
    SC2(SYS_setpgid, 0, 0);
    /* stdin/stdout/stderr go to /dev/null so that a fuzzed write cannot spend minutes on the console (trace mode keeps them) */
    if (!g_cfg.trace) {
        long nul = SC3(SYS_open, "/dev/null", O_RDWR, 0);
        for (int i = 0; i < 3; i++) SC2(SYS_dup2, nul, i);
        if (nul > 2) SC1(SYS_close, nul);
    }
    Slot *slot = &g_sh->slot[widx];
    slot->pid = (int)SC0(SYS_getpid);
    slot->main_tid = (int)SC0(SYS_gettid);
    w.rng.s = seed;
    w.idx = widx;
    w.slot = slot;
    w.budget = g_cfg.ncalls;
    for (int i = 0; g_cfg.scratch[i] && i < 90; i++) w.scratch[i] = g_cfg.scratch[i];
    if (!g_cfg.root) drop_privileges();
    setup_memory(&w);
    setup_fds(&w);
    struct gsigaction act = { (u64)nudge_handler, 0, 0, 0 };
    extern void __restore_rt(void);
    act.restorer = (u64)__restore_rt;
    SC3(SYS_sigaction, SIGUSR2, &act, 0);
    struct gsigaction ign = { 1, 0, 0, 0 };                 /* SIG_IGN */
    static const int quiet[] = { 1, 2, 3, 10, 13, 14, 15, 26, 27, 29, 30 };       /* not 12: SIGUSR2 is the watchdog thread's way of waking the main thread */
    for (unsigned i = 0; i < sizeof quiet / sizeof quiet[0]; i++) SC3(SYS_sigaction, quiet[i], &ign, 0);
    sc(SYS_thread_create, 0, (u64)(wd_stack + sizeof wd_stack - 8), 0, 0, (u64)watchdog_main, (u64)slot);
    fuzz_loop(&w, g_cfg.ncalls);
    /* reap whatever the fuzzed forks left, then go (the supervisor's cleanup takes care of anything that will not) */
    for (int i = 0; i < w.npids; i++) {
        int st;
        sc(SYS_waitpid, (u64)w.npids, (u64)&st, WNOHANG, 0, 0, 0);
    }
    slot->done = 1;
    SC1(SYS_exit_group, 0);
}

/* ---------------------------------------------------------------- the supervisor */

typedef struct {
    u64 free_pages, total_pages, heap_bytes, slab_pages, tmpfs_pages;
    long tasks, processes, user_threads, kstacks, files, vnodes, pipes, unix_sockets, inet_sockets, ptys, warnings, oom_kills, watchdog_reports;
} KStats;

static int read_kstats(KStats *k) {
    memset(k, 0, sizeof *k);
    int fd = open("/proc/kstats", O_RDONLY);
    if (fd < 0) return -1;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    for (char *line = buf; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *sp = strchr(line, ' ');
        if (sp) {
            *sp = 0;
            long v = strtol(sp + 1, 0, 10);
            #define KEY(name, field) if (!strcmp(line, name)) k->field = v
            KEY("free_pages", free_pages); KEY("total_pages", total_pages); KEY("heap_bytes", heap_bytes); KEY("slab_pages", slab_pages);
            KEY("tmpfs_pages", tmpfs_pages); KEY("tasks", tasks); KEY("processes", processes); KEY("user_threads", user_threads);
            KEY("kstacks", kstacks); KEY("files", files); KEY("vnodes", vnodes); KEY("pipes", pipes); KEY("unix_sockets", unix_sockets);
            KEY("inet_sockets", inet_sockets); KEY("ptys", ptys); KEY("warnings", warnings); KEY("oom_kills", oom_kills);
            KEY("watchdog_reports", watchdog_reports);
            #undef KEY
        }
        if (!nl) break;
        line = nl + 1;
    }
    return 0;
}

static void print_kstats(const char *tag, const KStats *k) {
    printf("sysfuzz: %s: free %lu+%lu slab pages, heap %lu bytes, tasks %ld, processes %ld, threads %ld, kstacks %ld, files %ld, vnodes %ld, "
           "pipes %ld, unix %ld, inet %ld, ptys %ld, warnings %ld\n", tag, k->free_pages, k->slab_pages, k->heap_bytes, k->tasks, k->processes,
           k->user_threads, k->kstacks, k->files, k->vnodes, k->pipes, k->unix_sockets, k->inet_sockets, k->ptys, k->warnings);
}

/* Kills the processes of the throw-away user (the workers' descendants may have left their process group). With `orphans_only`
 * just those that init has adopted, i.e. whose worker is gone: the running workers are left alone. */
static int kill_fuzz_procs(int orphans_only) {
    DIR *d = opendir("/proc");
    int killed = 0;
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == getpid()) continue;
        char path[64], buf[300];
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        char *u = strstr(buf, "Uid:");
        char *pp = strstr(buf, "PPid:");
        if (u && atoi(u + 4) == FUZZ_UID && (!orphans_only || (pp && atoi(pp + 5) == 1))) { kill(pid, SIGKILL); killed++; }
    }
    closedir(d);
    return killed;
}
static int kill_fuzz_users(void) { return kill_fuzz_procs(0); }

static void read_file_into(const char *path, char *buf, size_t cap) {
    buf[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = 0;
}

/* What the vnode ledger holds now that it did not hold before: the first lines, newest first, are the leaks. */
static void print_new_vnodes(const char *before, char *after) {
    int shown = 0;
    for (char *line = after; *line && shown < 12;) {
        char *nl = strchr(line, 10);
        if (nl) *nl = 0;
        char key[24];
        size_t k = 0;
        while (line[k] && line[k] != ' ' && k < sizeof key - 1) { key[k] = line[k]; k++; }
        key[k] = 0;
        if (k && !strstr(before, key)) { printf("sysfuzz:   new vnode %s\n", line); shown++; }
        if (!nl) break;
        line = nl + 1;
    }
}

static void remove_tree(const char *path, int depth);

/* Entries of the fuzz user directly in /tmp (a worker that changed directory there made some). */
static void sweep_tmp(void) {
    DIR *d = opendir("/tmp");
    if (!d) return;
    char names[64][64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 64) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[300];
        snprintf(sub, sizeof sub, "/tmp/%s", e->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && st.st_uid == FUZZ_UID) snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char sub[300];
        snprintf(sub, sizeof sub, "/tmp/%s", names[i]);
        remove_tree(sub, 1);
    }
}

static void remove_tree(const char *path, int depth) {
    for (int pass = 0; pass < 50; pass++) {                  /* until a pass finds nothing to remove */
        DIR *d = opendir(path);
        int removed = 0;
        if (!d || depth >= 8) break;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[600];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub, depth + 1);
            else unlink(sub);
            removed++;
        }
        closedir(d);
        if (!removed) break;
    }
    rmdir(path);
}

static int count_in_kmsg(const char *needle) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    static char buf[140000];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, sizeof buf - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof buf - 1) break;
    }
    close(fd);
    buf[total] = 0;
    int c = 0;
    for (char *p = buf; (p = strstr(p, needle)); p += strlen(needle)) c++;
    return c;
}

typedef struct { int pid; int widx; u64 started; u64 last_calls; u64 last_change; u64 killed_at; u64 seed; } Run;

static void describe_stuck(const Slot *s) {
    printf("sysfuzz:   last calls of pid %d:\n", s->pid);
    for (int i = 0; i < RING; i++) {
        const Entry *e = &s->ring[(s->ring_pos + (u32)i) & (RING - 1)];
        if (!e->nr && !e->a[0]) continue;
        printf("sysfuzz:     nr=%lu args=%lx %lx %lx %lx %lx %lx -> %ld\n", e->nr, e->a[0], e->a[1], e->a[2], e->a[3], e->a[4], e->a[5], e->ret);
    }
    if (s->in_call) printf("sysfuzz:   INSIDE nr=%lu args=%lx %lx %lx %lx %lx %lx\n", s->cur_nr, s->cur_a[0], s->cur_a[1], s->cur_a[2], s->cur_a[3], s->cur_a[4], s->cur_a[5]);
}

static int supervisor(void) {
    int fail = 0;
    KStats before, after;
    {                                                        /* things that appear once and stay (the /dev/pts directory) belong to the baseline */
        char pty[48];
        long m = SC2(SYS_ptyopen, pty, sizeof pty);
        if (m >= 0) SC1(SYS_close, m);
        sleep_ms(50);
    }
    read_kstats(&before);
    static char vn_before[20000], vn_after[20000];
    read_file_into("/proc/vnodes", vn_before, sizeof vn_before);
    char scratch[96];
    snprintf(scratch, sizeof scratch, "/tmp/sysfuzz.%d", getpid());
    g_cfg.scratch = scratch;
    g_cfg.supervisor_pid = getpid();
    remove_tree(scratch, 0);
    mkdir(scratch, 0777);
    chmod(scratch, 0777);

    char shmpath[128];
    snprintf(shmpath, sizeof shmpath, "%s/shm", scratch);
    int shfd = open(shmpath, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (shfd < 0 || ftruncate(shfd, 3 * 4096) < 0) { printf("sysfuzz: cannot create %s\n", shmpath); return 2; }
    void *m = (void *)sc(SYS_mmap, 0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, (u64)shfd, 0);
    if ((long)m < 0 && (long)m > -4096) { printf("sysfuzz: cannot map the shared page\n"); return 2; }
    g_sh = m;
    memset(g_sh, 0, sizeof *g_sh);
    chmod(shmpath, 0666);

    print_kstats("before", &before);
    printf("sysfuzz: seed %lu, %d s, %d workers, %d calls each, %s\n", g_cfg.seed, g_cfg.secs, g_cfg.workers, g_cfg.ncalls, g_cfg.root ? "as root" : "as an ordinary user");
    fflush(stdout);

    Run runs[MAX_WORKERS];
    memset(runs, 0, sizeof runs);
    u64 t0 = now_ms(), deadline = t0 + (u64)g_cfg.secs * 1000, last_report = t0;
    u64 iteration = 0, total_calls = 0, nviol = 0, nstuck = 0, nsig = 0, nwork = 0;
    int unkillable = 0;
    u64 sigcount[32] = { 0 };
    for (;;) {
        u64 now = now_ms();
        int alive = 0;
        for (int i = 0; i < g_cfg.workers; i++) {
            Run *r = &runs[i];
            Slot *s = &g_sh->slot[i];
            if (r->pid) {
                int st;
                pid_t p = waitpid(r->pid, &st, WNOHANG);
                if (p == r->pid) {
                    total_calls += s->calls;
                    nviol += (u64)s->viol;
                    if (s->viol) { printf("sysfuzz: worker %d (seed %lu): %s\n", r->widx, r->seed, s->vmsg); describe_stuck(s); fail = 1; }
                    if (WIFSIGNALED(st)) {
                        nsig++;
                        int sg = WTERMSIG(st);
                        if (sg > 0 && sg < 32) sigcount[sg]++;
                        if (g_cfg.verbose && sg != SIGKILL) {
                            printf("sysfuzz: worker %d (pid %d) died of signal %d after %lu calls\n", r->widx, r->pid, sg, s->calls);
                            describe_stuck(s);
                        }
                    } else sigcount[0]++;
                    r->pid = 0;
                    kill_fuzz_procs(1);
                } else {
                    if (s->calls != r->last_calls || !s->in_call) { r->last_calls = s->calls; r->last_change = now; }
                    int stuck = s->in_call && now - r->last_change > (u64)g_cfg.stuck_ms;
                    int old = now - r->started > (u64)g_cfg.stuck_ms * 6;
                    if ((stuck || old) && !r->killed_at) {
                        if (stuck) { nstuck++; if (g_cfg.verbose) { printf("sysfuzz: worker %d stuck in a call; killing it\n", r->widx); describe_stuck(s); } }
                        kill(-r->pid, SIGKILL);
                        kill(r->pid, SIGKILL);
                        kill_fuzz_procs(1);
                        r->killed_at = now;
                    } else if (r->killed_at && now - r->killed_at > 5000) {
                        printf("sysfuzz: UNKILLABLE: worker %d (pid %d) is still there 5 s after SIGKILL\n", r->widx, r->pid);
                        describe_stuck(s);
                        unkillable++;
                        fail = 1;
                        r->pid = 0;
                    }
                }
            }
            if (!r->pid && now < deadline && !unkillable && (g_cfg.only_worker < 0 || iteration == 0)) {
                u64 seed = mix(g_cfg.seed, iteration);
                int widx = i;
                if (g_cfg.only_worker >= 0) { seed = mix(g_cfg.seed, (u64)g_cfg.only_worker); }
                memset((void *)s, 0, sizeof *s);
                printf("sysfuzz: worker %lu seed %lu starts\n", iteration, seed);
                fflush(stdout);
                pid_t pid = fork();
                if (pid == 0) {
                    worker_main(widx, seed);
                    _exit(0);
                }
                if (pid < 0) { printf("sysfuzz: fork failed (%d)\n", errno); sleep_ms(50); continue; }
                r->pid = pid;
                r->widx = widx;
                r->started = now;
                r->last_calls = 0;
                r->last_change = now;
                r->killed_at = 0;
                r->seed = seed;
                iteration++;
                nwork++;
            }
            if (r->pid) alive++;
        }
        if (!alive && (now >= deadline || unkillable || (g_cfg.only_worker >= 0 && iteration > 0))) break;
        if (now - last_report > 10000) {
            printf("sysfuzz: %lu s, %lu workers so far, %lu calls\n", (now - t0) / 1000, nwork, total_calls);
            fflush(stdout);
            last_report = now;
        }
        sleep_ms(10);
    }
    g_sh->stop = 1;
    kill_fuzz_users();
    sleep_ms(300);
    kill_fuzz_users();
    sleep_ms(700);                                           /* zombies reparented to init are collected asynchronously */
    while (waitpid(-1, 0, WNOHANG) > 0) { }                  /* workers that were killed in the last moments */
    munmap(g_sh, 3 * 4096);
    close(shfd);
    remove_tree(scratch, 0);
    sweep_tmp();
    sleep_ms(300);

    read_kstats(&after);
    print_kstats("after", &after);
    long warn_lines = count_in_kmsg("[WARNING]");
    long incident = count_in_kmsg("INCIDENT");
    printf("sysfuzz: %lu workers, %lu calls, %lu died of a signal, %lu were stuck, %lu violations\n", nwork, total_calls, nsig, nstuck, nviol);
    printf("sysfuzz: deaths:");
    for (int i = 0; i < 32; i++) if (sigcount[i]) printf(" %s%d=%lu", i ? "signal " : "exit ", i, sigcount[i]);
    printf("\n");

    /* what must come back: every counted object. Memory: free + slab pages may not shrink by more than a few hundred KiB. */
    #define SAME(field, what) do { if (after.field != before.field) { printf("sysfuzz: LEAK: %s %ld -> %ld\n", what, before.field, after.field); fail = 1; } } while (0)
    SAME(files, "open files"); SAME(pipes, "pipes"); SAME(unix_sockets, "unix sockets"); SAME(ptys, "ptys");
    if (after.vnodes != before.vnodes) { read_file_into("/proc/vnodes", vn_after, sizeof vn_after); print_new_vnodes(vn_before, vn_after); }
    SAME(processes, "processes"); SAME(user_threads, "user threads"); SAME(kstacks, "kernel stacks"); SAME(vnodes, "vnodes"); SAME(tasks, "tasks");
    if (after.inet_sockets > before.inet_sockets + 64) { printf("sysfuzz: LEAK: inet sockets %ld -> %ld\n", before.inet_sockets, after.inet_sockets); fail = 1; }
    if (after.heap_bytes > before.heap_bytes + 256 * 1024) { printf("sysfuzz: LEAK: heap in use %lu -> %lu bytes\n", before.heap_bytes, after.heap_bytes); fail = 1; }
    long lost_slab = (long)after.slab_pages - (long)before.slab_pages;
    long lost_tmpfs = (long)after.tmpfs_pages - (long)before.tmpfs_pages;
    long gone = (long)before.free_pages - (long)after.free_pages - lost_slab - lost_tmpfs;
    printf("sysfuzz: memory: %ld pages fewer free, %ld of them went to the slabs, %ld to tmpfs files, %ld unexplained\n",
           (long)before.free_pages - (long)after.free_pages, lost_slab, lost_tmpfs, gone);
    if (gone > 128) { printf("sysfuzz: LEAK: %ld pages (%ld KiB) are gone\n", gone, gone * 4); fail = 1; }
    if (after.warnings != before.warnings || warn_lines) { printf("sysfuzz: the kernel warned %ld time(s) (%d lines in the log)\n", after.warnings - before.warnings, (int)warn_lines); fail = 1; }
    if (incident) { printf("sysfuzz: the kernel log mentions an INCIDENT\n"); fail = 1; }
    if (nviol) fail = 1;
    if (unkillable) fail = 1;
    #undef SAME
    printf(fail ? "SYSFUZZ FAIL\n" : "SYSFUZZ PASS\n");
    fflush(stdout);
    return fail ? 1 : 0;
}

/* "seed:1,time:60,workers:2,calls:300,root:1" */
static void parse_config(char *s) {
    while (*s) {
        char *comma = strchr(s, ',');
        if (comma) *comma = 0;
        char *colon = strchr(s, ':');
        if (colon) {
            *colon = 0;
            long v = strtol(colon + 1, 0, 0);
            if (!strcmp(s, "seed")) g_cfg.seed = (u64)v;
            else if (!strcmp(s, "time")) g_cfg.secs = (int)v;
            else if (!strcmp(s, "workers")) g_cfg.workers = (int)v;
            else if (!strcmp(s, "calls")) g_cfg.ncalls = (int)v;
            else if (!strcmp(s, "root")) g_cfg.root = (int)v;
            else if (!strcmp(s, "trace")) g_cfg.trace = (int)v;
            else if (!strcmp(s, "only")) g_cfg.only_worker = (int)v;
            else if (!strcmp(s, "stuck")) g_cfg.stuck_ms = (int)v;
            else if (!strcmp(s, "verbose")) g_cfg.verbose = (int)v;
        }
        if (!comma) break;
        s = comma + 1;
    }
}

int main(int argc, char **argv) {
    g_cfg.seed = 1;
    g_cfg.secs = 30;
    g_cfg.workers = 2;
    g_cfg.ncalls = 1500;
    g_cfg.stuck_ms = 1500;
    g_cfg.only_worker = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--noop")) return 0;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_cfg.seed = (u64)strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) g_cfg.secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--workers") && i + 1 < argc) g_cfg.workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--calls") && i + 1 < argc) g_cfg.ncalls = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only-worker") && i + 1 < argc) g_cfg.only_worker = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--root")) g_cfg.root = 1;
        else if (!strcmp(argv[i], "--trace")) g_cfg.trace = 1;
        else if (!strcmp(argv[i], "--verbose")) g_cfg.verbose = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) parse_config(argv[++i]);
        else { printf("usage: sysfuzz [--seed N] [--time S] [--workers N] [--calls N] [--root] [--trace] [--only-worker N] [--config k:v,...]\n"); return 2; }
    }
    if (g_cfg.workers < 1) g_cfg.workers = 1;
    if (g_cfg.workers > MAX_WORKERS) g_cfg.workers = MAX_WORKERS;
    if (getuid() != 0 && !g_cfg.root) { printf("sysfuzz: not root: the workers keep this user id and nothing is cleaned up for them\n"); }
    if (g_cfg.only_worker >= 0) g_cfg.workers = 1;
    setpgid(0, 0);
    return supervisor();
}
