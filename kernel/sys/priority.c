/* Scheduling priorities: getpriority / setpriority. A nice value runs from -20 (greedy) to 19 (polite); it belongs to the
 * process, every thread of it carries a copy, and the scheduler turns it into a time slice (slice_for in sched/task.c).
 * Anybody may make their own processes politer; only root may lower a nice value. See docs/JOBCONTROL.md. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "initcall.h"

typedef struct {
    int which;
    int who;                    /* id, already resolved (0 meaning "the caller" is replaced by the caller's own id) */
    int nice;                   /* setpriority: the new value */
    bool set;
    Process *caller;
    int matched;                /* processes that matched */
    int changed;                /* processes that were changed */
    int best;                   /* getpriority: lowest nice value seen */
    int err;                    /* first permission error */
} PrioScan;

static bool prio_matches(const PrioScan *a, const Process *p) {
    if (p->state == PROC_ZOMBIE) return false;
    switch (a->which) {
    case PRIO_PROCESS: return p->pid == a->who;
    case PRIO_PGRP:    return p->pgid == a->who;
    case PRIO_USER:    return p->cred.uid == (u32)a->who;
    }
    return false;
}

static void prio_cb(Process *p, void *ctx) {
    PrioScan *a = ctx;
    if (!prio_matches(a, p)) return;
    a->matched++;
    if (!a->set) {
        if (a->matched == 1 || p->nice < a->best) a->best = p->nice;
        return;
    }
    const Cred *c = &a->caller->cred;
    if (c->uid != 0 && c->uid != p->cred.uid && a->caller->ruid != p->cred.uid) { if (!a->err) a->err = -EPERM; return; }
    if (c->uid != 0 && a->nice < p->nice) { if (!a->err) a->err = -EACCES; return; }      /* raising the priority needs root */
    p->nice = a->nice;
    for (Task *t = p->threads; t; t = t->thread_next) t->nice = a->nice;
    a->changed++;
}

static int prio_resolve(int which, int who, Process *me) {
    if (who) return who;
    switch (which) {
    case PRIO_PROCESS: return me->pid;
    case PRIO_PGRP:    return me->pgid;
    default:           return (int)me->cred.uid;
    }
}

static long sys_getpriority(TrapFrame *tf) {
    int which = (int)A0(tf);
    if (which < PRIO_PROCESS || which > PRIO_USER) return -EINVAL;
    Process *me = current->proc;
    PrioScan a = { .which = which, .who = prio_resolve(which, (int)A1(tf), me), .caller = me };
    process_for_each(prio_cb, &a);
    if (!a.matched) return -ESRCH;
    return 20 - a.best;                     /* 1 (polite) .. 40 (greedy): the raw value can be negative, the return code cannot */
}

static long sys_setpriority(TrapFrame *tf) {
    int which = (int)A0(tf);
    if (which < PRIO_PROCESS || which > PRIO_USER) return -EINVAL;
    int nice = (int)A2(tf);
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;
    Process *me = current->proc;
    PrioScan a = { .which = which, .who = prio_resolve(which, (int)A1(tf), me), .nice = nice, .set = true, .caller = me };
    process_for_each(prio_cb, &a);
    if (!a.matched) return -ESRCH;
    if (a.err && !a.changed) return a.err;
    return 0;
}

static void priority_init(void) {
    syscall_register(SYS_getpriority, sys_getpriority);
    syscall_register(SYS_setpriority, sys_setpriority);
}
INITCALL(priority_init);
