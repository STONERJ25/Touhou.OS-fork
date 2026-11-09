/*
 * Signals for the Linux personality (docs/COMPAT.md): rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigsuspend, rt_sigtimedwait, sigaltstack, rt_sigreturn,
 * and the frame a handler runs on. The calls that send signals (kill, tkill, tgkill, rt_sigqueueinfo ...) are in sigsend.c.
 *
 * Generation, blocking, queueing, the default actions and job control are the machinery of kernel/sys/signal.c, which keeps signal sets as u64 with signal n in
 * bit n (signal 64, the last real-time signal, in bit 0: see siginfo.h) and, for a process of this personality, the siginfo of every pending signal. What
 * differs on this side is what a program can see: the call structures, the bit order of a signal set (bit n-1 is signal n in Linux, so a set is rotated by one
 * bit on its way in and out) and the stack frame, which is the x86-64 Linux one -- struct rt_sigframe: the handler's return address (the restorer the C library
 * supplied), a ucontext holding the machine state, a siginfo, and the floating-point state below them -- so that the restorer's call to rt_sigreturn finds what
 * it expects and a handler that looks at its arguments sees the real thing.
 *
 * Signals 1..64 all exist for a Linux program. Signal 31 is SIGSYS, an ordinary signal as in Linux (until 2026-11-09 it was hidden, because the native thread library
 * uses it as its SIGCANCEL; that meaning is now the native personality's alone, kernel/sys/signal.c, and a Linux program cannot cancel a native thread with it).
 * Signals 32 and 33 are the C library's (pthread_cancel, the set-user-id broadcast) and reach their handlers as any other real-time signal does.
 *
 * Written from the sigaction(2), sigreturn(2), sigaltstack(2), sigprocmask(2), sigtimedwait(2) and signal(7) manual pages, POSIX, and the psABI; the layouts are
 * checked against the C library's by tests/host/test_linux_abi.py and test_linux_signals_abi.py.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "numbers.h"
#include "calls.h"

/* ------------------------------------------------------------------ signal sets */

/* A Linux sigset_t is 64 bits with bit n-1 for signal n; the kernel's sets have bit n for signal n and signal 64 in bit 0 (siginfo.h): one rotation apart. */
#define LNX_SIGSET_SIZE 8
#define UNMASKABLE (sigbit(SIGKILL) | sigbit(SIGSTOP))

u64 lnx_sigset_to_kernel(u64 lset) { return (lset << 1) | (lset >> 63); }       /* also for calls that take a mask (ppoll, epoll_pwait, pselect6) */
u64 lnx_sigset_to_linux(u64 kset) { return (kset >> 1) | (kset << 63); }
#define to_kernel lnx_sigset_to_kernel
#define to_linux lnx_sigset_to_linux

static bool is_signal(int sig) { return sig > 0 && sig <= SIGRTMAX_K; }

/* ------------------------------------------------------------------ the calls */

struct lnx_sigaction { u64 handler, flags, restorer, mask; };       /* the kernel's struct sigaction on x86-64 (32 bytes) */

long lnx_rt_sigaction(TrapFrame *tf) {
    int sig = (int)A0(tf);
    if (A3(tf) != LNX_SIGSET_SIZE || !is_signal(sig)) return -EINVAL;
    if (A1(tf) && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    Process *p = current->proc;
    struct gsigaction *cur = &p->sigact[sig];
    if (A2(tf)) {
        struct lnx_sigaction old = { cur->handler, cur->flags, cur->restorer, to_linux(cur->mask) };
        if (copy_to_user((void *)A2(tf), &old, sizeof old)) return -EFAULT;
    }
    if (A1(tf)) {
        struct lnx_sigaction a;
        if (copy_from_user(&a, (void *)A1(tf), sizeof a)) return -EFAULT;
        cur->handler = a.handler;
        cur->flags = a.flags;
        cur->restorer = a.restorer;
        cur->mask = to_kernel(a.mask) & ~UNMASKABLE;
        signal_action_changed(p, sig);                  /* "ignore" discards what is pending */
    }
    return 0;
}

long lnx_rt_sigprocmask(TrapFrame *tf) {
    if (A3(tf) != LNX_SIGSET_SIZE) return -EINVAL;
    Task *t = current;
    u64 old = to_linux(t->sig_blocked);
    if (A1(tf)) {
        u64 set;
        if (copy_from_user(&set, (void *)A1(tf), sizeof set)) return -EFAULT;
        u64 n = to_kernel(set) & ~UNMASKABLE;
        switch (A0(tf)) {
        case LNX_SIG_BLOCK:   t->sig_blocked |= n; break;
        case LNX_SIG_UNBLOCK: t->sig_blocked &= ~n; break;
        case LNX_SIG_SETMASK: t->sig_blocked = n; break;
        default: return -EINVAL;
        }
    }
    if (A2(tf) && copy_to_user((void *)A2(tf), &old, sizeof old)) return -EFAULT;
    return 0;
}

long lnx_rt_sigpending(TrapFrame *tf) {
    if (A1(tf) != LNX_SIGSET_SIZE) return -EINVAL;
    u64 pend = to_linux((current->sig_pending | current->proc->sig_pending) & current->sig_blocked);
    return copy_to_user((void *)A0(tf), &pend, sizeof pend);
}

/* rt_sigsuspend(set, size): replaces the mask, sleeps until a signal needs attention and always ends with EINTR; the handler runs under the temporary mask
 * and rt_sigreturn brings the old one back (Task.saved_mask), as for the native sigsuspend. */
long lnx_rt_sigsuspend(TrapFrame *tf) {
    u64 set;
    if (A1(tf) != LNX_SIGSET_SIZE) return -EINVAL;
    if (copy_from_user(&set, (void *)A0(tf), sizeof set)) return -EFAULT;
    Task *t = current;
    u64 f = irq_save();
    t->saved_mask = t->sig_blocked;
    t->saved_mask_valid = true;
    t->sig_blocked = to_kernel(set) & ~UNMASKABLE;
    while (!signal_pending_now(t->proc)) {
        t->intr = true;
        t->wake_ms = ~0ULL;
        t->state = TASK_SLEEPING;
        schedule();
        t->intr = false;
    }
    irq_restore(f);
    return -EINTR;
}

struct lnx_siginfo { s32 si_signo, si_errno, si_code, pad; u8 fields[112]; };      /* siginfo_t: 128 bytes; the union starts at offset 16 */
_Static_assert(sizeof(struct lnx_siginfo) == 128, "siginfo_t is 128 bytes");
_Static_assert(sizeof(struct lnx_siginfo) == sizeof(SigInfo), "the kernel's SigInfo is the siginfo_t of Linux");

/* rt_sigtimedwait(set, info, timeout, size): takes a pending signal of the set without running a handler and returns its number, with the siginfo that says who
 * sent it. No timeout waits for ever; a timeout of zero only looks. EAGAIN when the time ran out, EINTR when a signal outside the set interrupted the wait. */
long lnx_rt_sigtimedwait(TrapFrame *tf) {
    u64 set, deadline = ~0ULL;
    if (A3(tf) != LNX_SIGSET_SIZE) return -EINVAL;
    if (copy_from_user(&set, (void *)A0(tf), sizeof set)) return -EFAULT;
    if (A2(tf)) {
        long rc = deadline_from_timespec((void *)A2(tf), &deadline);
        if (rc) return rc;
    }
    SigInfo si;
    long r = signal_wait(to_kernel(set), deadline, &si);
    if (r > 0 && A1(tf) && copy_to_user((void *)A1(tf), &si, sizeof si)) return -EFAULT;
    return r;
}

struct lnx_stack { u64 ss_sp; s32 ss_flags, pad; u64 ss_size; };       /* stack_t (24 bytes) */

static bool on_altstack(Task *t, u64 sp) { return t->alt_size && sp >= t->alt_sp && sp < t->alt_sp + t->alt_size; }

/* sigaltstack(ss, old): the stack handlers with SA_ONSTACK run on. Changing it while running on it is EPERM; a stack smaller than MINSIGSTKSZ is ENOMEM. */
long lnx_sigaltstack(TrapFrame *tf) {
    Task *t = current;
    bool on = on_altstack(t, tf->rsp);
    if (A1(tf)) {
        struct lnx_stack old = { t->alt_sp, on ? LNX_SS_ONSTACK : (t->alt_size ? 0 : LNX_SS_DISABLE), 0, t->alt_size };
        if (copy_to_user((void *)A1(tf), &old, sizeof old)) return -EFAULT;
    }
    if (A0(tf)) {
        struct lnx_stack ss;
        if (copy_from_user(&ss, (void *)A0(tf), sizeof ss)) return -EFAULT;
        if (on) return -EPERM;
        if (ss.ss_flags & ~(s32)LNX_SS_DISABLE) return -EINVAL;
        if (ss.ss_flags & LNX_SS_DISABLE) {
            t->alt_sp = t->alt_size = 0;
        } else {
            if (ss.ss_size < LNX_MINSIGSTKSZ) return -ENOMEM;
            t->alt_sp = ss.ss_sp;
            t->alt_size = ss.ss_size;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ the frame */

/* sigcontext (256 bytes): the registers of the interrupted code in the order the kernel stores them, then the fault details and a pointer to the FXSAVE area. */
struct lnx_sigcontext {
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    u64 eflags;
    u16 cs, gs, fs, ss;
    u64 err, trapno, oldmask, cr2;
    u64 fpstate;
    u64 reserved1[8];
};
struct lnx_ucontext {                                   /* the kernel's ucontext on x86-64 (304 bytes) */
    u64 uc_flags;
    u64 uc_link;
    struct lnx_stack uc_stack;
    struct lnx_sigcontext uc_mcontext;
    u64 uc_sigmask;
};
struct lnx_rt_sigframe {                                /* what the handler finds at its stack pointer: the return address, then these */
    u64 pretcode;
    struct lnx_ucontext uc;
    struct lnx_siginfo info;
};
_Static_assert(sizeof(struct lnx_sigcontext) == 256, "sigcontext");
_Static_assert(sizeof(struct lnx_ucontext) == 304, "ucontext");
_Static_assert(sizeof(struct lnx_rt_sigframe) == 440, "rt_sigframe");

#define FPSTATE_SIZE 512                                /* FXSAVE: x87 and SSE state (no AVX here) */
#define MXCSR_MASK   0xFFBFu

/* An interrupted system call is started again after the handler (the frame is built so that sigreturn lands on the syscall instruction with the number back
 * in rax) when the handler asked for SA_RESTART and the call is one that sleeps for something that may still come. The others fail with EINTR, as in Linux
 * (signal(7): nanosleep, poll, futex, sigtimedwait and the like are never restarted). */
static bool restartable(u64 nr) {
    switch (nr) {
    case LNX_read: case LNX_write: case LNX_readv: case LNX_writev: case LNX_pread64: case LNX_pwrite64:
    case LNX_wait4: case LNX_waitid: case LNX_open: case LNX_openat:
        return true;
    default:
        return false;
    }
}

static int fault_code(Task *t, int sig) {                /* si_code of a signal that a fault of this thread raised (0: it was sent) */
    if (!t->fault_code || (sig != SIGSEGV && sig != SIGBUS && sig != SIGILL && sig != SIGFPE && sig != SIGTRAP)) return 0;
    return t->fault_code;
}

/* Builds the handler's frame on the user stack and aims the thread at the handler. Called by signal_deliver with the frame that was about to return to user
 * mode; oldmask is the signal mask to restore when the handler returns, info what the handler sees as its siginfo. Returns 0, or a negative errno when the
 * frame cannot be written (the caller kills the process, as Linux does with SIGSEGV). */
int linux_signal_frame(TrapFrame *tf, int sig, struct gsigaction *act, u64 oldmask, const SigInfo *info) {
    Task *t = current;
    if (!(act->flags & LNX_SA_RESTORER) || !act->restorer) return -EFAULT;        /* x86-64 needs the restorer; the C library always supplies it */

    if (tf->vector == 0x80 && (long)tf->rax == -EINTR && (act->flags & LNX_SA_RESTART) && restartable(tf->error)) {
        tf->rip -= 2;                                   /* the syscall instruction */
        tf->rax = tf->error;                            /* its number, saved by the dispatcher */
    }

    u64 sp = tf->rsp;
    if ((act->flags & LNX_SA_ONSTACK) && t->alt_size && !on_altstack(t, sp)) sp = t->alt_sp + t->alt_size;
    sp -= 128;                                          /* the red zone below the stack pointer belongs to the interrupted code */
    sp = (sp - FPSTATE_SIZE) & ~0x3FUL;
    u64 fp = sp;
    sp = ((sp - sizeof(struct lnx_rt_sigframe)) & ~0xFUL) - 8;      /* so that rsp + 8 is 16-byte aligned when the handler starts */

    u8 fx[FPSTATE_SIZE] __attribute__((aligned(16)));
    __asm__ volatile ("fxsave (%0)" : : "r"(fx) : "memory");        /* the live floating-point state of the interrupted code */

    struct lnx_rt_sigframe fr;
    memset(&fr, 0, sizeof fr);
    fr.pretcode = act->restorer;
    fr.uc.uc_stack.ss_sp = t->alt_sp;
    fr.uc.uc_stack.ss_size = t->alt_size;
    fr.uc.uc_stack.ss_flags = on_altstack(t, tf->rsp) ? LNX_SS_ONSTACK : (t->alt_size ? 0 : LNX_SS_DISABLE);
    struct lnx_sigcontext *c = &fr.uc.uc_mcontext;
    c->r8 = tf->r8; c->r9 = tf->r9; c->r10 = tf->r10; c->r11 = tf->r11; c->r12 = tf->r12; c->r13 = tf->r13; c->r14 = tf->r14; c->r15 = tf->r15;
    c->rdi = tf->rdi; c->rsi = tf->rsi; c->rbp = tf->rbp; c->rbx = tf->rbx; c->rdx = tf->rdx; c->rax = tf->rax; c->rcx = tf->rcx;
    c->rsp = tf->rsp;
    c->rip = tf->rip;
    c->eflags = tf->rflags;
    c->cs = (u16)tf->cs;
    c->ss = (u16)tf->ss;
    c->trapno = tf->vector == 0x80 ? 0 : tf->vector;
    c->err = tf->vector == 0x80 ? 0 : tf->error;
    c->cr2 = fault_code(t, sig) ? t->fault_addr : 0;
    c->fpstate = fp;
    fr.uc.uc_sigmask = to_linux(oldmask);
    memcpy(&fr.info, info, sizeof fr.info);
    fr.info.si_signo = sig;
    if (fault_code(t, sig)) {                           /* a fault of this thread: the reason and the address, not a sender */
        memset(fr.info.fields, 0, sizeof fr.info.fields);
        fr.info.si_code = fault_code(t, sig);
        *(u64 *)fr.info.fields = t->fault_addr;         /* si_addr */
    }
    t->fault_code = 0;

    if (copy_to_user((void *)fp, fx, FPSTATE_SIZE) || copy_to_user((void *)sp, &fr, sizeof fr)) return -EFAULT;

    tf->rsp = sp;
    tf->rip = act->handler;
    tf->rdi = (u64)sig;
    if (act->flags & LNX_SA_SIGINFO) {
        tf->rsi = sp + __builtin_offsetof(struct lnx_rt_sigframe, info);
        tf->rdx = sp + __builtin_offsetof(struct lnx_rt_sigframe, uc);
    }
    tf->rax = 0;
    tf->rflags = (tf->rflags & ~(u64)(RFLAGS_TF | RFLAGS_DF | 0x4000 | 0x40000)) | RFLAGS_IF | 2;       /* not trapping, direction flag clear, no NT or AC */
    tf->cs = USER_CS;
    tf->ss = USER_DS;
    t->sig_blocked |= act->mask | ((act->flags & LNX_SA_NODEFER) ? 0 : sigbit(sig));
    if (act->flags & LNX_SA_RESETHAND) act->handler = SIG_DFL;
    return 0;
}

/* rt_sigreturn(): the restorer jumped here with the stack pointer just above the return address the handler popped; the frame is there. The registers, the
 * signal mask and the floating-point state come back from it. The result is the restored rax, so that the dispatcher's store leaves it as it was. */
long lnx_rt_sigreturn(TrapFrame *tf) {
    struct lnx_rt_sigframe fr;
    if (copy_from_user(&fr, (void *)(tf->rsp - 8), sizeof fr)) { klog("Eiki", "'%s' (pid %d): sigreturn found no frame -- exorcised", current->proc->name, current->proc->pid); process_exit(SIGSEGV); }
    struct lnx_sigcontext *c = &fr.uc.uc_mcontext;
    tf->r8 = c->r8; tf->r9 = c->r9; tf->r10 = c->r10; tf->r11 = c->r11; tf->r12 = c->r12; tf->r13 = c->r13; tf->r14 = c->r14; tf->r15 = c->r15;
    tf->rdi = c->rdi; tf->rsi = c->rsi; tf->rbp = c->rbp; tf->rbx = c->rbx; tf->rdx = c->rdx; tf->rax = c->rax; tf->rcx = c->rcx;
    tf->rsp = c->rsp;
    tf->rip = c->rip;
    tf->rflags = (c->eflags & 0xDD5) | RFLAGS_IF | 2;       /* never trust the saved flags beyond the arithmetic ones */
    tf->cs = USER_CS;
    tf->ss = USER_DS;
    current->sig_blocked = to_kernel(fr.uc.uc_sigmask) & ~UNMASKABLE;
    if (c->fpstate) {
        u8 fx[FPSTATE_SIZE] __attribute__((aligned(16)));
        if (copy_from_user(fx, (void *)c->fpstate, FPSTATE_SIZE) == 0) {
            u32 mxcsr;
            memcpy(&mxcsr, fx + 24, sizeof mxcsr);
            mxcsr &= MXCSR_MASK;                            /* reserved bits in MXCSR would fault inside the kernel */
            memcpy(fx + 24, &mxcsr, sizeof mxcsr);
            __asm__ volatile ("fxrstor (%0)" : : "r"(fx) : "memory");
        }
    }
    return (long)tf->rax;
}
