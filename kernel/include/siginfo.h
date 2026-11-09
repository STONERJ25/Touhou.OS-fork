/*
 * Signal numbers beyond the native ones, signal sets, and siginfo (docs/COMPAT.md, "Real-time signals").
 *
 * A signal set is a u64 in which signal n (1..63) is bit n: the very bit the native 32-bit masks always used, so code that handles signals 1..31 with `1U << sig`
 * is still right. Signal 0 does not exist, so its bit carries the last real-time signal, 64 (SIGRTMAX): sigbit() does the folding and sig_first() puts signal 64 last.
 * Native programs use 1..31 (NSIG, the native ABI does not change); a process of the Linux personality can name 1..64 (KNSIG). Linux's own signal-set order (bit n-1
 * for signal n) is a one-bit rotation away; kernel/linux/signal.c converts at the system call boundary.
 *
 * SigInfo is the 128-byte siginfo_t of x86-64 Linux, kept as the kernel stores it for a queued signal: the three header words, then the union at offset 16
 * (sender: pid at 16 and uid at 20; a value at 24; for SIGCHLD the status at 24 and the CPU times at 32 and 40; for a POSIX timer the timer id at 16 and the overrun
 * count at 20). Written from sigaction(2), sigqueue(3) and the layouts in <asm-generic/siginfo.h> as documented; tests/host/test_linux_signals_abi.py checks the
 * offsets against the C library's header on a real Linux.
 */
#ifndef K_SIGINFO_H
#define K_SIGINFO_H

#include "types.h"
#include "string.h"

#define KNSIG       65                  /* array size for signal numbers 1..64 (index 0 unused) */
#define SIGRTMIN_K  32                  /* the kernel's first real-time signal; the C library keeps 32 and 33 for itself and reports 34 */
#define SIGRTMAX_K  64

#ifndef SIGVTALRM
#define SIGVTALRM   26                  /* ITIMER_VIRTUAL expired: not a native signal, the Linux personality's setitimer sends it */
#define SIGPROF     27                  /* ITIMER_PROF expired */
#endif

static inline u64 sigbit(int sig) { return 1ULL << (sig & 63); }

/* The lowest-numbered signal of a non-empty set: 1..63 by bit, 64 (bit 0) after all of them, as POSIX asks for real-time signals. */
static inline int sig_first(u64 set) {
    u64 low = set & ~1ULL;
    return low ? __builtin_ctzll(low) : SIGRTMAX_K;
}

/* si_code values (siginfo.h) */
#define SI_USER     0                   /* kill(2) */
#define SI_KERNEL   0x80                /* sent by the kernel: alarm, a terminal's ^C, SIGPIPE */
#define SI_QUEUE    (-1)                /* sigqueue(3) */
#define SI_TIMER    (-2)                /* a POSIX timer expired */
#define SI_MESGQ    (-3)
#define SI_ASYNCIO  (-4)
#define SI_SIGIO    (-5)
#define SI_TKILL    (-6)                /* tkill(2), tgkill(2): raise(3), pthread_kill(3) */
#define CLD_EXITED    1                 /* SIGCHLD: the child called exit */
#define CLD_KILLED    2                 /*          was killed by a signal */
#define CLD_DUMPED    3                 /*          was killed by a signal and dumped core */
#define CLD_TRAPPED   4                 /*          was traced and trapped */
#define CLD_STOPPED   5                 /*          stopped */
#define CLD_CONTINUED 6                 /*          continued */

typedef struct SigInfo {
    s32 si_signo, si_errno, si_code, pad;
    u8  u[112];                         /* the union, from offset 16 */
} SigInfo;
_Static_assert(sizeof(SigInfo) == 128, "siginfo_t is 128 bytes");

static inline void siginfo_clear(SigInfo *i, int sig, int code) {
    memset(i, 0, sizeof *i);
    i->si_signo = sig;
    i->si_code = code;
}
static inline void siginfo_sender(SigInfo *i, s32 pid, u32 uid) { memcpy(i->u, &pid, 4); memcpy(i->u + 4, &uid, 4); }       /* si_pid (16), si_uid (20) */
static inline void siginfo_value(SigInfo *i, u64 v) { memcpy(i->u + 8, &v, 8); }                                           /* si_value (24) */
static inline u64  siginfo_get_value(const SigInfo *i) { u64 v; memcpy(&v, i->u + 8, 8); return v; }
static inline void siginfo_child(SigInfo *i, s32 status, s64 utime, s64 stime) {                                           /* SIGCHLD: si_status (24), si_utime (32), si_stime (40) */
    memcpy(i->u + 8, &status, 4);
    memcpy(i->u + 16, &utime, 8);
    memcpy(i->u + 24, &stime, 8);
}
static inline void siginfo_timer(SigInfo *i, s32 timerid, s32 overrun, u64 value) {                                        /* POSIX timer: si_tid (16), si_overrun (20), value (24) */
    memcpy(i->u, &timerid, 4);
    memcpy(i->u + 4, &overrun, 4);
    memcpy(i->u + 8, &value, 8);
}
static inline s32  siginfo_get_s32(const SigInfo *i, int off) { s32 v; memcpy(&v, i->u + off, 4); return v; }              /* off is relative to the union (offset 16) */
static inline void siginfo_put_s32(SigInfo *i, int off, s32 v) { memcpy(i->u + off, &v, 4); }

/* The queue behind a pending set (kernel/sys/signal.c): entries of signals that carry a siginfo. The structure is private to signal.c. */
struct SigQ;
typedef struct SigList { struct SigQ *head, *tail; } SigList;

#endif
