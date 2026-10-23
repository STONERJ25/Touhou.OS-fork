/* Signals: pending/blocked sets, default actions, user handlers via a frame on the user stack.
 * The mask and the thread-directed pending set belong to the thread (Task); dispositions and the process-directed pending
 * set belong to the Process. A process-directed signal is taken by whichever thread that does not block it passes through
 * signal_deliver first; signal_send wakes one such thread (the running one if it qualifies) so a sleeping syscall returns. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"

typedef struct SigFrame {
    u64 restorer;           /* return address of the handler: user trampoline that issues SYS_sigreturn */
    TrapFrame saved;
    u32 saved_mask;
    u32 signo;
} SigFrame;

static bool default_ignored(int sig) {
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGWINCH;
}

static bool ignored(Process *p, int sig) {
    unsigned long h = p->sigact[sig].handler;
    return h == SIG_IGN || (h == SIG_DFL && default_ignored(sig));
}

/* A group exit or an exec in another thread wants this thread gone (it leaves at its next trip through signal_deliver). */
bool thread_must_die(Task *t) {
    Process *p = t->proc;
    return p && (p->exiting || (p->execing && p->execing != t));
}

/* Does the calling thread have something to do on the way out of the kernel? Blocking calls ask this before they sleep. */
bool signal_pending_now(Process *p) {
    Task *t = current;
    (void)p;
    if (!t->proc) return false;
    return thread_must_die(t) || ((t->sig_pending | t->proc->sig_pending) & ~t->sig_blocked) != 0;
}

void signal_send(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG || p->state == PROC_ZOMBIE) return;
    if (sig != SIGKILL && ignored(p, sig)) return;
    u64 f = irq_save();
    p->sig_pending |= 1U << sig;
    Task *target = NULL;
    for (Task *t = p->threads; t; t = t->thread_next) {
        if (t->sig_blocked & (1U << sig)) continue;
        if (!target) target = t;
        if (t == current) { target = t; break; }        /* the running thread picks it up on its way back to user mode */
    }
    irq_restore(f);
    if (target) task_interrupt(target);
}

void signal_send_thread(Task *t, int sig) {
    Process *p = t->proc;
    if (!p || sig <= 0 || sig >= NSIG || p->state == PROC_ZOMBIE) return;
    if (sig != SIGKILL && ignored(p, sig)) return;
    u64 f = irq_save();
    t->sig_pending |= 1U << sig;
    irq_restore(f);
    if (!(t->sig_blocked & (1U << sig))) task_interrupt(t);
}

/* A synchronous fault of the calling thread: the signal is aimed at that thread, with the default action. */
void signal_force(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG) return;
    Task *t = current;
    p->sigact[sig].handler = SIG_DFL;
    t->sig_blocked &= ~(1U << sig);
    u64 f = irq_save();
    t->sig_pending |= 1U << sig;
    irq_restore(f);
}

struct PgrpArg {
    int pgid, sig;
};

static void pgrp_cb(Process *p, void *ctx) {
    struct PgrpArg *a = ctx;
    if (p->pgid == a->pgid && p->state == PROC_RUNNING) signal_send(p, a->sig);
}

void signal_pgrp(int pgid, int sig) {
    struct PgrpArg a = { pgid, sig };
    process_for_each(pgrp_cb, &a);
}

/* Called with the frame that is about to return to user mode. */
void signal_deliver(TrapFrame *tf) {
    Task *t = current;
    Process *p = t->proc;
    if (!p || (tf->cs & 3) != 3) return;
    if (thread_must_die(t)) thread_leave();

    for (;;) {
        u64 f = irq_save();
        u32 pend = (t->sig_pending | p->sig_pending) & (~t->sig_blocked | (1U << SIGKILL));
        int sig = pend ? __builtin_ctz(pend) : 0;
        if (pend) { t->sig_pending &= ~(1U << sig); p->sig_pending &= ~(1U << sig); }
        irq_restore(f);
        if (!pend) return;
        struct gsigaction *act = &p->sigact[sig];

        if (act->handler == SIG_IGN || (act->handler == SIG_DFL && default_ignored(sig))) continue;
        if (act->handler == SIG_DFL) {
            klog("Eiki", "'%s' (pid %d) was exorcised by signal %d", p->name, p->pid, sig);
            process_exit(sig);                              /* the default action ends the whole process, not one thread */
        }

        /* Build the handler frame below the user's stack pointer (skipping the 128-byte red zone). */
        SigFrame fr;
        fr.restorer = act->restorer;
        fr.saved = *tf;
        fr.saved_mask = t->sig_blocked;
        fr.signo = (u32)sig;
        u64 sp = ((tf->rsp - 128 - sizeof fr) & ~0xFUL) - 8;
        if (copy_to_user((void *)sp, &fr, sizeof fr)) {
            klog("Eiki", "'%s' (pid %d): signal frame is unreachable -- exorcised", p->name, p->pid);
            process_exit(SIGSEGV);
        }
        tf->rsp = sp;
        tf->rip = act->handler;
        tf->rdi = (u64)sig;
        tf->rsi = tf->rdx = 0;
        tf->rax = 0;
        t->sig_blocked |= (u32)act->mask | (1U << sig);
        return;
    }
}

long sys_sigreturn_impl(TrapFrame *tf) {
    SigFrame fr;
    if (copy_from_user(&fr, (void *)(tf->rsp - 8), sizeof fr)) process_exit(SIGSEGV);
    *tf = fr.saved;
    tf->cs = USER_CS;                   /* never trust the saved selectors: stay in ring 3 */
    tf->ss = USER_DS;
    tf->rflags = (tf->rflags & 0xDD5) | RFLAGS_IF | 2;
    current->sig_blocked = fr.saved_mask & ~((1U << SIGKILL) | (1U << SIGSTOP));
    return (long)tf->rax;
}
