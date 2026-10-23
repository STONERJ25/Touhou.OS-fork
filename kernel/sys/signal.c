/* Signals: pending/blocked sets, default actions, user handlers via a frame on the user stack. */
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

bool signal_pending_now(Process *p) {
    return (p->sig_pending & ~p->sig_blocked) != 0;
}

void signal_send(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG || p->state == PROC_ZOMBIE) return;
    if (sig != SIGKILL && ignored(p, sig)) return;
    u64 f = irq_save();
    p->sig_pending |= 1U << sig;
    irq_restore(f);
    if (p->task && !(p->sig_blocked & (1U << sig))) task_interrupt(p->task);
}

void signal_force(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG) return;
    p->sigact[sig].handler = SIG_DFL;
    p->sig_blocked &= ~(1U << sig);
    u64 f = irq_save();
    p->sig_pending |= 1U << sig;
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
    Process *p = current->proc;
    if (!p || (tf->cs & 3) != 3) return;

    for (;;) {
        u32 pend = p->sig_pending & (~p->sig_blocked | (1U << SIGKILL));
        if (!pend) return;
        int sig = __builtin_ctz(pend);
        p->sig_pending &= ~(1U << sig);
        struct gsigaction *act = &p->sigact[sig];

        if (act->handler == SIG_IGN || (act->handler == SIG_DFL && default_ignored(sig))) continue;
        if (act->handler == SIG_DFL) {
            klog("Eiki", "'%s' (pid %d) was exorcised by signal %d", p->name, p->pid, sig);
            process_exit(sig);
        }

        /* Build the handler frame below the user's stack pointer (skipping the 128-byte red zone). */
        SigFrame fr;
        fr.restorer = act->restorer;
        fr.saved = *tf;
        fr.saved_mask = p->sig_blocked;
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
        p->sig_blocked |= act->mask | (1U << sig);
        return;
    }
}

long sys_sigreturn_impl(TrapFrame *tf) {
    SigFrame fr;
    if (copy_from_user(&fr, (void *)(tf->rsp - 8), sizeof fr)) process_exit(SIGSEGV);
    Process *p = current->proc;
    *tf = fr.saved;
    tf->cs = USER_CS;                   /* never trust the saved selectors: stay in ring 3 */
    tf->ss = USER_DS;
    tf->rflags = (tf->rflags & 0xDD5) | RFLAGS_IF | 2;
    p->sig_blocked = fr.saved_mask & ~((1U << SIGKILL) | (1U << SIGSTOP));
    return (long)tf->rax;
}
