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
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGWINCH;      /* the stop signals are not here: their default is to stop */
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
    u32 pend = (t->sig_pending | t->proc->sig_pending) & ~t->sig_blocked;
    if (t->ignore_cancel) pend &= ~(1U << SIGCANCEL);
    return thread_must_die(t) || pend != 0;
}

/* pthread_cancel's request is a sticky thread-directed SIGCANCEL: it never runs a handler (signal_deliver leaves it alone),
 * it makes interruptible waits return, and syscall_dispatch then reports -ECANCELED instead of -EINTR. Blocking the signal
 * is how a thread disables cancellation. */
bool signal_cancel_pending(Task *t) {
    return (t->sig_pending & (1U << SIGCANCEL)) && !(t->sig_blocked & (1U << SIGCANCEL));
}

/* ------------------------------------------------------------------ job control
 *
 * SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU stop the whole process, SIGCONT resumes it (docs/JOBCONTROL.md). A stop whose action is
 * the default takes effect when the signal is generated: Process.stopped is set and the parent hears about it at once. The
 * threads then park (job_park) at the next edge of user mode, so a thread blocked in read() just stays blocked and a stop
 * never turns into EINTR. SIGCONT always continues the process, even when it is blocked or ignored; a handler, if any, still
 * runs. Generating a stop signal discards a pending SIGCONT and the other way round. A stop signal that is blocked in every
 * thread stays pending and stops the process when it is unblocked and delivered. The orphaned-process-group rule (a stop
 * signal other than SIGSTOP is dropped there) is POSIX's. */

#define STOP_MASK ((1U << SIGSTOP) | (1U << SIGTSTP) | (1U << SIGTTIN) | (1U << SIGTTOU))

bool signal_is_stop(int sig) { return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU; }

static void discard_pending(Process *p, u32 mask) {            /* irq off */
    p->sig_pending &= ~mask;
    for (Task *t = p->threads; t; t = t->thread_next) t->sig_pending &= ~mask;
}

/* The parent learns that a child stopped or continued: SIGCHLD (unless it asked for SA_NOCLDSTOP) and a wake-up for waitpid. */
static void job_notify(Process *p) {
    Process *parent = p->parent;
    if (!parent || parent->state == PROC_ZOMBIE) return;
    if (!(parent->sigact[SIGCHLD].flags & SA_NOCLDSTOP)) signal_send(parent, SIGCHLD);
    wq_wake_all(&parent->wait_wq);
}

void process_stop(Process *p, int sig) {
    if (p == g_init_proc || p->state == PROC_ZOMBIE || p->exiting) return;      /* the shrine's first process never stops */
    u64 f = irq_save();
    bool fresh = !p->stopped;
    p->stopped = true;
    p->stop_sig = sig;
    if (fresh) { p->stop_unreported = true; p->continued_unreported = false; }
    irq_restore(f);
    if (fresh) job_notify(p);
}

void process_continue(Process *p) {
    u64 f = irq_save();
    bool was = p->stopped;
    if (was) { p->stopped = false; p->stop_unreported = false; p->continued_unreported = true; }
    irq_restore(f);
    if (!was) return;
    wq_wake_all(&p->stop_wq);
    job_notify(p);
}

/* Sleep while the process is stopped. Leaves at SIGCONT, when the thread must die, or when SIGKILL is pending. */
void job_park(Process *p) {
    Task *t = current;
    u64 f = irq_save();
    while (p->stopped && !thread_must_die(t) && !((t->sig_pending | p->sig_pending) & (1U << SIGKILL))) {
        t->intr = true;
        wq_block(&p->stop_wq);
        t->intr = false;
    }
    irq_restore(f);
}

/* Is there a thread that could take `sig` right now? (`only` limits the question to one thread.) */
static bool someone_takes(Process *p, int sig, Task *only) {
    if (only) return !(only->sig_blocked & (1U << sig));
    for (Task *t = p->threads; t; t = t->thread_next) if (!(t->sig_blocked & (1U << sig))) return true;
    return false;
}

/* Generation-time part of job control. True when the signal needs nothing more (the stop or continue is done). */
static bool job_signal(Process *p, int sig, Task *only) {
    if (sig == SIGCONT) {
        u64 f = irq_save();
        discard_pending(p, STOP_MASK);
        irq_restore(f);
        process_continue(p);
        unsigned long h = p->sigact[SIGCONT].handler;
        return h == SIG_DFL || h == SIG_IGN;                    /* with a handler the signal is still delivered */
    }
    if (!signal_is_stop(sig)) return false;
    u64 f = irq_save();
    discard_pending(p, 1U << SIGCONT);
    irq_restore(f);
    unsigned long h = p->sigact[sig].handler;
    if (sig != SIGSTOP && h == SIG_IGN) return true;
    if (sig != SIGSTOP && h != SIG_DFL) return false;           /* caught: the handler runs instead of the stop */
    if (sig != SIGSTOP) {
        u64 g = irq_save();
        bool takes = someone_takes(p, sig, only);
        irq_restore(g);
        if (!takes) return false;                               /* blocked everywhere: stays pending */
    }
    if (sig != SIGSTOP && pgrp_orphaned(p->pgid)) return true;  /* nobody could ever continue it: orphaned groups ignore these */
    process_stop(p, sig);
    return true;
}

void signal_send(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG || p->state == PROC_ZOMBIE) return;
    if (job_signal(p, sig, NULL)) return;
    if (sig != SIGKILL && ignored(p, sig)) return;
    u64 f = irq_save();
    p->sig_pending |= 1U << sig;
    Task *target = NULL;
    for (Task *t = p->threads; t; t = t->thread_next) {
        if (t->sigwait_set & (1U << sig)) { target = t; break; }       /* somebody is waiting for exactly this signal */
        if (t->sig_blocked & (1U << sig)) continue;
        if (!target) target = t;
        if (t == current) target = t;                   /* the running thread picks it up on its way back to user mode */
    }
    irq_restore(f);
    if (target) task_interrupt(target);
}

void signal_send_thread(Task *t, int sig) {
    Process *p = t->proc;
    if (!p || sig <= 0 || sig >= NSIG || p->state == PROC_ZOMBIE) return;
    if (job_signal(p, sig, t)) return;                  /* stop and continue are about the whole process, whoever was aimed at */
    if (sig != SIGKILL && ignored(p, sig)) return;
    u64 f = irq_save();
    t->sig_pending |= 1U << sig;
    irq_restore(f);
    if (!(t->sig_blocked & (1U << sig)) || (t->sigwait_set & (1U << sig))) task_interrupt(t);
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
    if (p->stopped) {                                       /* the process was stopped: wait here, at the edge of user mode */
        job_park(p);
        if (thread_must_die(t)) thread_leave();
    }

    for (;;) {
        u64 f = irq_save();
        u32 pend = (t->sig_pending | p->sig_pending) & (~t->sig_blocked | (1U << SIGKILL)) & ~(1U << SIGCANCEL);
        int sig = pend ? __builtin_ctz(pend) : 0;
        if (pend) { t->sig_pending &= ~(1U << sig); p->sig_pending &= ~(1U << sig); }
        irq_restore(f);
        if (!pend) {
            if (t->saved_mask_valid) { t->sig_blocked = t->saved_mask; t->saved_mask_valid = false; }   /* sigsuspend ended without a handler */
            return;
        }
        struct gsigaction *act = &p->sigact[sig];

        if (act->handler == SIG_IGN || (act->handler == SIG_DFL && default_ignored(sig))) continue;
        if (act->handler == SIG_DFL && signal_is_stop(sig)) {      /* a stop signal that was blocked when it arrived */
            if (sig != SIGSTOP && pgrp_orphaned(p->pgid)) continue;
            process_stop(p, sig);
            job_park(p);
            if (thread_must_die(t)) thread_leave();
            continue;
        }
        if (act->handler == SIG_DFL) {
            klog("Eiki", "'%s' (pid %d) was exorcised by signal %d", p->name, p->pid, sig);
            process_exit(sig);                              /* the default action ends the whole process, not one thread */
        }

        /* Build the handler frame below the user's stack pointer (skipping the 128-byte red zone). */
        SigFrame fr;
        fr.restorer = act->restorer;
        fr.saved = *tf;
        fr.saved_mask = t->saved_mask_valid ? t->saved_mask : t->sig_blocked;     /* after sigsuspend the handler returns to the old mask */
        t->saved_mask_valid = false;
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
