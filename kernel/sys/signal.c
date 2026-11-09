/* Signals: pending/blocked sets, default actions, user handlers via a frame on the user stack.
 * The mask and the thread-directed pending set belong to the thread (Task); dispositions and the process-directed pending
 * set belong to the Process. A process-directed signal is taken by whichever thread that does not block it passes through
 * signal_deliver first; signal_send wakes one such thread (the running one if it qualifies) so a sleeping syscall returns.
 *
 * Signal sets are u64 (siginfo.h): signal n is bit n, so everything that was written for the 31 native signals with `1U << sig` still means what it did; the
 * real-time signals 32..64 of the Linux personality use the rest (signal 64 is bit 0, which no signal needs otherwise).
 *
 * Queued signals. A process of the Linux personality asks for more than a pending bit: a handler or sigwaitinfo receives a siginfo that says who sent the
 * signal (kill, tkill, sigqueue, a timer, a child that exited ...), and real-time signals queue: three sigqueue(3) calls are three deliveries, lowest number
 * first, oldest first among equals (POSIX 2.4.2). So behind each pending set (one per thread, one per process) there is a list of entries, SigQ, each a
 * signal number and its 128-byte siginfo, taken from a fixed pool (generation also happens in interrupt context: alarm, timers, the terminal's ^C). The
 * bit of a signal is set exactly while its entries are listed; a standard signal (1..31) is held once -- a second instance while the first is pending is
 * dropped, with its siginfo, as Linux does -- and a real-time signal as often as it was sent, up to SIGQ_PER_PROC per process. Where code sets or clears a bit
 * on its own (signal_force, the reset in execve) an entry may be left behind or missing: an entry whose bit is clear is garbage and is freed by the next
 * deposit of that signal, a bit without an entry stands for a signal whose siginfo is the kernel's own. Native processes have no entries at all, only bits. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "linux.h"
#include "irq.h"

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

/* Signal 31 is the native thread library's SIGCANCEL: a sticky, thread-directed request that never runs a handler (below). For a program of the Linux personality it is
 * SIGSYS, an ordinary signal (the owner freed it for Linux programs on 2026-11-09): this is the bit of the thread's sets that means "cancel", or 0 for such a program. */
static inline u64 cancel_bit(const Task *t) { return t->proc && t->proc->abi == ABI_LINUX ? 0 : sigbit(SIGCANCEL); }

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
    u64 pend = (t->sig_pending | t->proc->sig_pending) & ~t->sig_blocked;
    if (t->ignore_cancel) pend &= ~cancel_bit(t);
    return thread_must_die(t) || pend != 0;
}

/* pthread_cancel's request is a sticky thread-directed SIGCANCEL: it never runs a handler (signal_deliver leaves it alone),
 * it makes interruptible waits return, and syscall_dispatch then reports -ECANCELED instead of -EINTR. Blocking the signal
 * is how a thread disables cancellation. */
bool signal_cancel_pending(Task *t) {
    u64 c = cancel_bit(t);
    return (t->sig_pending & c) && !(t->sig_blocked & c);
}

/* ------------------------------------------------------------------ queued siginfo (Linux personality) */

#define SIGQ_POOL 1024

typedef struct SigQ {
    struct SigQ *next;
    int sig;
    SigInfo info;
} SigQ;

static SigQ g_sigq_pool[SIGQ_POOL];
static SigQ *g_sigq_free;
static bool g_sigq_ready;

static SigQ *sigq_alloc(void) {                     /* irq off */
    if (!g_sigq_ready) {
        for (int i = SIGQ_POOL - 1; i >= 0; i--) { g_sigq_pool[i].next = g_sigq_free; g_sigq_free = &g_sigq_pool[i]; }
        g_sigq_ready = true;
    }
    SigQ *q = g_sigq_free;
    if (q) g_sigq_free = q->next;
    return q;
}

static void sigq_release(SigQ *q) {                 /* irq off */
    q->next = g_sigq_free;
    g_sigq_free = q;
}

static inline u64 *box_set(Process *p, Task *t) { return t ? &t->sig_pending : &p->sig_pending; }
static inline SigList *box_list(Process *p, Task *t) { return t ? &t->sigl : &p->sigl; }

static void list_append(SigList *l, SigQ *q) {
    q->next = NULL;
    if (l->tail) l->tail->next = q; else l->head = q;
    l->tail = q;
}

/* Frees the entries of the list whose signal is in `mask`. irq off. */
static void list_purge(Process *p, SigList *l, u64 mask) {
    SigQ *prev = NULL, *q = l->head;
    while (q) {
        SigQ *next = q->next;
        if (sigbit(q->sig) & mask) {
            if (prev) prev->next = next; else l->head = next;
            if (l->tail == q) l->tail = prev;
            if (q->info.si_code == SI_TIMER) ktimer_signal_dropped(p, &q->info);       /* the timer may queue its next signal now */
            sigq_release(q);
            p->sigq_count--;
        } else {
            prev = q;
        }
        q = next;
    }
}

static bool list_has(const SigList *l, int sig) {
    for (const SigQ *q = l->head; q; q = q->next) if (q->sig == sig) return true;
    return false;
}

/* timer_delete: the entries that carry the signal of timer `id` are freed, and the bit of a signal that has no entry left goes with them. irq off. */
static void drop_timer_entries(Process *p, SigList *l, u64 *set, int id) {
    SigQ *prev = NULL, *q = l->head;
    u64 gone = 0;
    while (q) {
        SigQ *next = q->next;
        if (q->info.si_code == SI_TIMER && siginfo_get_s32(&q->info, 0) == id) {
            if (prev) prev->next = next; else l->head = next;
            if (l->tail == q) l->tail = prev;
            gone |= sigbit(q->sig);
            sigq_release(q);
            p->sigq_count--;
        } else {
            prev = q;
        }
        q = next;
    }
    for (int sig = 1; sig <= SIGRTMAX_K && gone; sig++)
        if ((gone & sigbit(sig)) && !list_has(l, sig)) { *set &= ~sigbit(sig); gone &= ~sigbit(sig); }
}

void signal_drop_timer(Process *p, int timerid) {
    drop_timer_entries(p, &p->sigl, &p->sig_pending, timerid);
    for (Task *t = p->threads; t; t = t->thread_next) drop_timer_entries(p, &t->sigl, &t->sig_pending, timerid);
}

/* Unlinks and returns the oldest entry of signal sig (NULL if there is none); *more says whether another one of that signal is left. irq off. */
static SigQ *list_take(Process *p, SigList *l, int sig, bool *more) {
    SigQ *prev = NULL, *q = l->head;
    *more = false;
    while (q && q->sig != sig) { prev = q; q = q->next; }
    if (!q) return NULL;
    if (prev) prev->next = q->next; else l->head = q->next;
    if (l->tail == q) l->tail = prev;
    p->sigq_count--;
    for (SigQ *r = q->next; r; r = r->next) if (r->sig == sig) { *more = true; break; }
    return q;
}

/* Records one instance of sig for the process (t == NULL) or for one of its threads. Returns 0 when an entry holds it, 1 when only the bit does (a native process, a
 * standard signal that was pending already, a full pool), -EAGAIN when a queued signal found no room. irq off. */
static int deposit(Process *p, Task *t, int sig, const SigInfo *info) {
    u64 *set = box_set(p, t);
    u64 bit = sigbit(sig);
    int rc = 1;
    if (p->abi == ABI_LINUX) {
        SigList *l = box_list(p, t);
        if (!(*set & bit)) list_purge(p, l, bit);                           /* entries of an earlier life of this signal (the set was cleared by someone else) */
        else if (sig < SIGRTMIN_K) return 1;                                /* a standard signal is held once: the first siginfo stays */
        SigQ *q = p->sigq_count < SIGQ_PER_PROC ? sigq_alloc() : NULL;
        if (q) {
            q->sig = sig;
            if (info) q->info = *info; else siginfo_clear(&q->info, sig, SI_KERNEL);
            q->info.si_signo = sig;
            list_append(l, q);
            p->sigq_count++;
            rc = 0;
        } else if (sig >= SIGRTMIN_K && info && info->si_code < 0 && info->si_code != SI_TIMER) {
            return -EAGAIN;                                                 /* sigqueue(3) fails; kill(2) and the kernel's own signals still arrive, without a siginfo */
        }
    }
    *set |= bit;
    return rc;
}

/* Takes one pending instance of sig for thread t of p: from the thread's own queue first, then the process's. A standard signal does not queue, so one
 * delivery clears it everywhere. *info gets its siginfo (the kernel's own if none was recorded). irq off. */
static void take_pending(Process *p, Task *t, int sig, SigInfo *info) {
    u64 bit = sigbit(sig);
    SigQ *q = NULL;
    if (p->abi == ABI_LINUX) {
        bool more;
        if (t->sig_pending & bit) {
            q = list_take(p, &t->sigl, sig, &more);
            if (!more) t->sig_pending &= ~bit;
        } else {
            q = list_take(p, &p->sigl, sig, &more);
            if (!more) p->sig_pending &= ~bit;
        }
        if (sig < SIGRTMIN_K) {
            t->sig_pending &= ~bit;
            p->sig_pending &= ~bit;
            list_purge(p, &t->sigl, bit);
            list_purge(p, &p->sigl, bit);
        }
    } else {
        t->sig_pending &= ~bit;
        p->sig_pending &= ~bit;
    }
    if (q) {
        *info = q->info;
        sigq_release(q);
        if (info->si_code == SI_TIMER) ktimer_signal_taken(p, info);        /* the timer's overrun count goes into si_overrun */
    } else if (p->abi == ABI_LINUX) {                   /* a native program has no use for a siginfo */
        siginfo_clear(info, sig, SI_KERNEL);
    }
}

/* The signals in `mask` stop being pending, in the process and in each of its threads (a handler became SIG_IGN, SIGCONT discards the stops ...). irq off. */
static void discard_locked(Process *p, u64 mask) {
    p->sig_pending &= ~mask;
    list_purge(p, &p->sigl, mask);
    for (Task *t = p->threads; t; t = t->thread_next) {
        t->sig_pending &= ~mask;
        list_purge(p, &t->sigl, mask);
    }
}

void signal_discard(Process *p, u64 mask) {
    u64 f = irq_save();
    discard_locked(p, mask);
    irq_restore(f);
}

/* After sigaction: a signal whose action is now "ignore" (SIG_IGN, or the default of SIGCHLD and friends) is discarded where it is pending (POSIX 2.4.4). */
void signal_action_changed(Process *p, int sig) {
    if (sig > 0 && sig < KNSIG && ignored(p, sig) && sig != SIGKILL) signal_discard(p, sigbit(sig));
}

/* The thread is going away: whatever was queued for it alone is dropped (Linux flushes a dead thread's private signals too). */
void signal_thread_exit(Task *t) {
    Process *p = t->proc;
    if (!p) return;
    u64 f = irq_save();
    list_purge(p, &t->sigl, ~0ULL);
    t->sig_pending = 0;
    irq_restore(f);
}

void signal_process_exit(Process *p) {
    u64 f = irq_save();
    list_purge(p, &p->sigl, ~0ULL);
    p->sig_pending = 0;
    irq_restore(f);
}

/* execve: caught signals go back to the default (ignored ones stay ignored), nothing stays pending, and the alternate signal stack is gone. The mask, the interval
 * timers and the alarm survive, as POSIX says. */
void signal_exec_reset(Process *p) {
    for (int s = 0; s < KNSIG; s++)
        if (p->sigact[s].handler != SIG_IGN) memset(&p->sigact[s], 0, sizeof p->sigact[s]);
    u64 f = irq_save();
    list_purge(p, &p->sigl, ~0ULL);
    p->sig_pending = 0;
    list_purge(p, &current->sigl, ~0ULL);
    current->sig_pending = 0;
    irq_restore(f);
    current->alt_sp = current->alt_size = 0;            /* the alternate signal stack lived in the old program's memory */
    ktimers_process_exit(p);                            /* POSIX timers do not survive execve */
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

#define STOP_MASK (sigbit(SIGSTOP) | sigbit(SIGTSTP) | sigbit(SIGTTIN) | sigbit(SIGTTOU))

bool signal_is_stop(int sig) { return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU; }

static void discard_pending(Process *p, u64 mask) {            /* irq off */
    discard_locked(p, mask);
}

/* The siginfo of a SIGCHLD about `child`: CLD_EXITED and CLD_KILLED carry the exit code or the killing signal in `status`, CLD_STOPPED the stopping signal,
 * CLD_CONTINUED SIGCONT. The CPU times are in 1/100 s, the unit of times(2). */
void signal_child_info(SigInfo *info, const Process *child, int code, int status) {
    u64 user, total;
    process_cpu_ticks(child, &user, &total);
    siginfo_clear(info, SIGCHLD, code);
    siginfo_sender(info, child->pid, child->ruid);
    siginfo_child(info, status, (s64)(user * 100 / TIMER_HZ), (s64)((total - user) * 100 / TIMER_HZ));
}

/* The parent learns that a child stopped or continued: SIGCHLD (unless it asked for SA_NOCLDSTOP) and a wake-up for waitpid. */
static void job_notify(Process *p, int code, int status) {
    Process *parent = p->parent;
    if (!parent || parent->state == PROC_ZOMBIE) return;
    if (!(parent->sigact[SIGCHLD].flags & SA_NOCLDSTOP)) {
        SigInfo info;
        signal_child_info(&info, p, code, status);
        signal_send_info(parent, SIGCHLD, &info);
    }
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
    if (fresh) job_notify(p, CLD_STOPPED, sig);
}

void process_continue(Process *p) {
    u64 f = irq_save();
    bool was = p->stopped;
    if (was) { p->stopped = false; p->stop_unreported = false; p->continued_unreported = true; }
    irq_restore(f);
    if (!was) return;
    wq_wake_all(&p->stop_wq);
    job_notify(p, CLD_CONTINUED, SIGCONT);
}

/* Sleep while the process is stopped. Leaves at SIGCONT, when the thread must die, or when SIGKILL is pending. */
void job_park(Process *p) {
    Task *t = current;
    u64 f = irq_save();
    while (p->stopped && !thread_must_die(t) && !((t->sig_pending | p->sig_pending) & sigbit(SIGKILL))) {
        t->intr = true;
        wq_block(&p->stop_wq);
        t->intr = false;
    }
    irq_restore(f);
}

/* Is there a thread that could take `sig` right now? (`only` limits the question to one thread.) */
static bool someone_takes(Process *p, int sig, Task *only) {
    if (only) return !(only->sig_blocked & sigbit(sig));
    for (Task *t = p->threads; t; t = t->thread_next) if (!(t->sig_blocked & sigbit(sig))) return true;
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
    discard_pending(p, sigbit(SIGCONT));
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

/* init does what the shrine's first process has to do: it is the only one that can end it, with exit(). A signal it has installed no
 * handler for would only kill it (and with it the machine, see process_finalize), so such a signal is dropped, whoever sent it: the
 * kernel's own faults go through signal_force and are not affected. This is what Linux does for the first process of a namespace. */
static inline bool init_shrugs_off(const Process *p, int sig) {
    return p == g_init_proc && p->sigact[sig].handler == SIG_DFL;
}

/* May `from` send a signal to `to`? Root, or the same user (real or effective ids). */
bool signal_permitted(const Process *from, const Process *to) {
    return from == to || from->cred.uid == 0 || from->cred.uid == to->cred.uid || from->cred.uid == to->ruid || from->ruid == to->ruid ||
           from->ruid == to->cred.uid;
}

static int send_checked(Process *p, int sig, const SigInfo *info);

/* Which signal numbers a process can be sent: the native personality knows 1..31, the Linux one 1..64. */
static inline bool sendable(const Process *p, int sig) { return sig > 0 && sig < (p->abi == ABI_LINUX ? KNSIG : NSIG); }

void signal_send(Process *p, int sig) { signal_send_info(p, sig, NULL); }

/* Generates sig for the process. Returns 0 when it waits in a queue with its siginfo, 1 when it was merged into one that is pending already, thrown away or only
 * recorded as a bit, -EAGAIN when a queued (real-time) signal found no room. */
int signal_send_info(Process *p, int sig, const SigInfo *info) {
    if (!sendable(p, sig) || p->state == PROC_ZOMBIE) return 1;
    if (init_shrugs_off(p, sig)) return 1;
    return send_checked(p, sig, info);
}

/* kill(2) from a process of either personality: a receiver of the Linux personality gets a siginfo that names the sender. */
int signal_send_from(const Process *from, Process *to, int sig) {
    if (to->abi != ABI_LINUX) { signal_send(to, sig); return 0; }
    SigInfo info;
    siginfo_clear(&info, sig, SI_USER);
    siginfo_sender(&info, from->pid, from->ruid);
    return signal_send_info(to, sig, &info);
}

/* kill(2) for both personalities: pid > 0 one process, 0 the caller's group, -1 everybody the caller may signal (init and the caller excepted), < -1 the group -pid.
 * Signal 0 only checks. Returns 0 or -errno. */
long signal_kill(Process *self, long pid, int sig) {
    if (pid > 0) {
        if (pid > 0x7fffffffL) return -ESRCH;                           /* not a pid: do not let it wrap around to one (kill(2^32 + 1) was init) */
        Process *t = process_by_pid((int)pid);
        if (!t || t->state == PROC_ZOMBIE) return -ESRCH;
        if (!signal_permitted(self, t)) return -EPERM;
        if (t == g_init_proc && sig == SIGKILL && self->cred.uid == 0) { signal_kill_init(t); return 0; }       /* root may end init on purpose: the machine panics, which the panic tests rely on */
        if (sig) signal_send_from(self, t, sig);
        return 0;
    }
    if (pid == -1) {                                                    /* everybody the caller may signal (init and the caller excepted) */
        int r = signal_group_from(self, 0, sig, true);
        return r < 0 ? r : 0;
    }
    if (pid < -0x7fffffffL) return -ESRCH;
    int pg = pid == 0 ? self->pgid : (int)-pid;
    int r = signal_group_from(self, pg, sig, false);
    return r < 0 ? r : 0;
}

/* root ends init on purpose (`kill -9 1`): the machine panics in init's exit, which is how the panic screen, the netconsole and the panic record
 * are provoked from user mode (docs/RELIABILITY.md). Every other signal aimed at init, and anything from anybody else, is dropped by signal_send. */
void signal_kill_init(Process *init) {
    if (init->state == PROC_ZOMBIE) return;
    send_checked(init, SIGKILL, NULL);
}

/* A signal whose action is "ignore" is thrown away when it is generated -- unless somebody blocks it or waits for it with sigtimedwait: the action may change before
 * it is unblocked, and the waiter wants it whatever the action is (POSIX 2.4.4; Linux does the same, so a parent can sigwaitinfo for the SIGCHLD that the default
 * action would ignore). `only` is the thread a thread-directed signal is aimed at. irq off. */
static bool thrown_away(Process *p, int sig, Task *only) {
    if (sig == SIGKILL || !ignored(p, sig)) return false;
    u64 bit = sigbit(sig);
    if (only) return !((only->sig_blocked | only->sigwait_set) & bit);
    for (Task *t = p->threads; t; t = t->thread_next)
        if ((t->sig_blocked | t->sigwait_set) & bit) return false;
    return true;
}

static int send_checked(Process *p, int sig, const SigInfo *info) {
    /* SIGCANCEL is thread-directed only (syscalls.c): sent to a native process as a whole it would stay pending for ever and make every wait of the process return at once.
     * Only a program of the Linux personality can aim signal 31 that way (it is SIGSYS to it), so it is dropped here. */
    if (sig == SIGCANCEL && p->abi != ABI_LINUX) return 1;
    if (job_signal(p, sig, NULL)) return 1;
    u64 f = irq_save();
    if (thrown_away(p, sig, NULL)) { irq_restore(f); return 1; }
    int rc = 1;
    if (p->abi == ABI_LINUX) rc = deposit(p, NULL, sig, info);
    else p->sig_pending |= sigbit(sig);                 /* a native process has bits only: the common case, kept short */
    Task *target = NULL;
    u64 bit = sigbit(sig);
    for (Task *t = p->threads; t; t = t->thread_next) {
        if (t->sigwait_set & bit) { target = t; break; }       /* somebody is waiting for exactly this signal */
        if (t->sig_blocked & bit) continue;
        if (!target) target = t;
        if (t == current) target = t;                   /* the running thread picks it up on its way back to user mode */
    }
    irq_restore(f);
    if (target) task_interrupt(target);
    return rc;
}

void signal_send_thread(Task *t, int sig) { signal_send_thread_info(t, sig, NULL); }

int signal_send_thread_info(Task *t, int sig, const SigInfo *info) {
    Process *p = t->proc;
    if (!p || !sendable(p, sig) || p->state == PROC_ZOMBIE) return 1;
    if (init_shrugs_off(p, sig)) return 1;
    if (job_signal(p, sig, t)) return 1;                /* stop and continue are about the whole process, whoever was aimed at */
    u64 f = irq_save();
    if (thrown_away(p, sig, t)) { irq_restore(f); return 1; }
    int rc = 1;
    if (p->abi == ABI_LINUX) rc = deposit(p, t, sig, info);
    else t->sig_pending |= sigbit(sig);
    irq_restore(f);
    if (!(t->sig_blocked & sigbit(sig)) || (t->sigwait_set & sigbit(sig))) task_interrupt(t);
    return rc;
}

/* A synchronous fault of the calling thread: the signal is aimed at that thread, with the default action. */
void signal_force(Process *p, int sig) {
    if (sig <= 0 || sig >= NSIG) return;
    Task *t = current;
    /* A native program's fault always takes the default action. A Linux program keeps a handler it installed, unless the signal is blocked or ignored, as Linux does. */
    if (p->abi != ABI_LINUX || (t->sig_blocked & sigbit(sig)) || p->sigact[sig].handler == SIG_IGN) p->sigact[sig].handler = SIG_DFL;
    t->sig_blocked &= ~sigbit(sig);
    u64 f = irq_save();
    list_purge(p, &t->sigl, sigbit(sig));               /* an entry left over from before would be taken for this fault's: it has no siginfo of its own */
    t->sig_pending |= sigbit(sig);
    irq_restore(f);
}

struct PgrpArg {
    int pgid, sig;
    int uid;                        /* the sender, when check is set */
    bool check;
    int found, sent;
};

static void pgrp_cb(Process *p, void *ctx) {
    struct PgrpArg *a = ctx;
    if (p->pgid != a->pgid || p->state != PROC_RUNNING) return;
    a->found++;
    if (a->check && a->uid != 0 && (int)p->cred.uid != a->uid) return;      /* the same rule as for one process: root, or the owner */
    a->sent++;
    signal_send(p, a->sig);
}

void signal_pgrp(int pgid, int sig) {
    struct PgrpArg a = { pgid, sig, 0, false, 0, 0 };
    process_for_each(pgrp_cb, &a);
}

/* kill(-pgid) and kill(-1) for an ordinary caller: only the processes it may signal are signalled (all of a group, or everybody except
 * init and the caller itself). Returns how many there were: -ESRCH for none at all, -EPERM when there were some and none was allowed. */
struct FromArg {
    const Process *from;
    int pgid, sig, seen, sent;
    bool everyone;
    const SigInfo *info;            /* the siginfo for receivers of the Linux personality, or NULL to make the plain kill(2) one */
};

static void from_cb(Process *p, void *ctx) {
    struct FromArg *a = ctx;
    if (p->state != PROC_RUNNING || p == g_init_proc) return;
    if (a->everyone ? p == a->from : p->pgid != a->pgid) return;
    a->seen++;
    if (!signal_permitted(a->from, p)) return;
    a->sent++;
    if (!a->sig) return;
    if (a->info) signal_send_info(p, a->sig, a->info);
    else signal_send_from(a->from, p, a->sig);
}

int signal_group_info(const Process *from, int pgid, int sig, bool everyone, const SigInfo *info) {
    struct FromArg a = { from, pgid, sig, 0, 0, everyone, info };
    process_for_each(from_cb, &a);
    if (!a.seen) return -ESRCH;
    return a.sent ? a.sent : -EPERM;
}

int signal_group_from(const Process *from, int pgid, int sig, bool everyone) {
    return signal_group_info(from, pgid, sig, everyone, NULL);
}

/* tcsetpgrp made `pgid` the foreground group of a terminal. Members that stopped because they wanted that terminal (SIGTTIN, SIGTTOU)
 * have what they were waiting for, so they go on by themselves. A shell that hands the terminal over a little late (its child was
 * already running and reached the first read or tcsetattr) would otherwise leave the child stopped for ever, and a shell that does
 * send SIGCONT as well (fg) loses nothing: SIGCONT to a running process does nothing. A deviation from POSIX, see JOBCONTROL.md. */
static void granted_cb(Process *p, void *ctx) {
    if (p->pgid == *(int *)ctx && p->state == PROC_RUNNING && p->stopped && (p->stop_sig == SIGTTIN || p->stop_sig == SIGTTOU))
        process_continue(p);
}

void pgrp_terminal_granted(int pgid) {
    if (pgid > 0) process_for_each(granted_cb, &pgid);
}

/* sigtimedwait: waits until a signal of `set` is pending for the calling thread or the process, and takes it (no handler runs). Signals outside the set that
 * are not blocked interrupt the wait (-EINTR, and their handler runs), as does the thread being wanted elsewhere. deadline is an uptime_ms() time, ~0 for none;
 * -EAGAIN when it passes. The set may name signals the caller blocks: a thread that is waiting is woken by them (Task.sigwait_set, see send_checked). */
long signal_wait(u64 set, u64 deadline, SigInfo *info) {
    Task *t = current;
    Process *p = t->proc;
    long rc;
    set &= ~(sigbit(SIGKILL) | sigbit(SIGSTOP) | cancel_bit(t));
    u64 f = irq_save();
    t->sigwait_set = set;
    for (;;) {
        u64 hit = (t->sig_pending | p->sig_pending) & set;
        if (hit) {
            int sig = sig_first(hit);
            take_pending(p, t, sig, info);
            rc = sig;
            break;
        }
        if (thread_must_die(t) || ((t->sig_pending | p->sig_pending) & ~t->sig_blocked & ~set)) { rc = -EINTR; break; }
        if (deadline != ~0ULL && uptime_ms() >= deadline) { rc = -EAGAIN; break; }
        t->intr = true;
        t->wake_ms = deadline;
        t->state = TASK_SLEEPING;
        schedule();
        t->intr = false;
    }
    t->sigwait_set = 0;
    irq_restore(f);
    return rc;
}

/* The signals a fault raises come first (Linux takes them before any other, so a handler for SIGSEGV runs before one for SIGUSR1 that was pending as well). */
#define SYNCHRONOUS_MASK (sigbit(SIGSEGV) | sigbit(SIGBUS) | sigbit(SIGILL) | sigbit(SIGTRAP) | sigbit(SIGFPE))

static void signal_deliver_slow(TrapFrame *tf);

/* Called with the frame that is about to return to user mode, after every system call and every interrupt. Almost always there is nothing to do, and that case is
 * kept short: the work is in signal_deliver_slow. */
void signal_deliver(TrapFrame *tf) {
    Task *t = current;
    Process *p = t->proc;
    if (!p || (tf->cs & 3) != 3) return;
    if (!p->exiting && !p->execing && !p->stopped && !t->saved_mask_valid &&
        !((t->sig_pending | p->sig_pending) & (~t->sig_blocked | sigbit(SIGKILL)) & ~cancel_bit(t)))
        return;
    signal_deliver_slow(tf);
}

__attribute__((noinline)) static void signal_deliver_slow(TrapFrame *tf) {
    Task *t = current;
    Process *p = t->proc;
    if (thread_must_die(t)) thread_leave();
    if (p->stopped) {                                       /* the process was stopped: wait here, at the edge of user mode */
        job_park(p);
        if (thread_must_die(t)) thread_leave();
    }

    for (;;) {
        SigInfo info;
        u64 f = irq_save();
        u64 pend = (t->sig_pending | p->sig_pending) & (~t->sig_blocked | sigbit(SIGKILL)) & ~cancel_bit(t);
        int sig = 0;
        if (pend) {
            if (p->abi == ABI_LINUX) {
                if (pend & SYNCHRONOUS_MASK) pend &= SYNCHRONOUS_MASK;
                sig = sig_first(pend);
                take_pending(p, t, sig, &info);
            } else {                                        /* native: signals 1..31, bits only */
                sig = __builtin_ctzll(pend);
                t->sig_pending &= ~sigbit(sig);
                p->sig_pending &= ~sigbit(sig);
            }
        }
        irq_restore(f);
        if (!pend) {
            if (t->saved_mask_valid) { t->sig_blocked = t->saved_mask; t->saved_mask_valid = false; }   /* sigsuspend ended without a handler */
            return;
        }
        struct gsigaction *act = &p->sigact[sig];
        if (act->handler > SIG_IGN && act->handler >= USER_TOP) process_exit(SIGSEGV);       /* cannot happen (sigaction refuses it); no handler address is worth an iretq fault */

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

        if (p->abi == ABI_LINUX) {                          /* the frame of x86-64 Linux: kernel/linux/signal.c */
            u64 old = t->saved_mask_valid ? t->saved_mask : t->sig_blocked;
            t->saved_mask_valid = false;
            if (linux_signal_frame(tf, sig, act, old, &info) < 0) {
                klog("Eiki", "'%s' (pid %d): signal frame is unreachable -- exorcised", p->name, p->pid);
                process_exit(SIGSEGV);
            }
            continue;                                       /* as Linux does: further signals that are deliverable now get their frame on top of this one, so the handler
                                                             * of the last one set up runs first (the signals pending together run highest number first) */
        }

        /* Build the handler frame below the user's stack pointer (skipping the 128-byte red zone). */
        SigFrame fr;
        fr.restorer = act->restorer;
        fr.saved = *tf;
        fr.saved_mask = (u32)(t->saved_mask_valid ? t->saved_mask : t->sig_blocked);     /* after sigsuspend the handler returns to the old mask */
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
        t->sig_blocked |= (u32)act->mask | sigbit(sig);
        return;
    }
}

long sys_sigreturn_impl(TrapFrame *tf) {
    SigFrame fr;
    if (copy_from_user(&fr, (void *)(tf->rsp - 8), sizeof fr)) process_exit(SIGSEGV);
    if (fr.saved.rip >= USER_TOP || fr.saved.rsp >= USER_TOP) {       /* an iretq to a non-canonical or kernel address faults in ring 0 */
        klog("Eiki", "'%s' (pid %d) returned from a signal into the Boundary (rip %#lx, rsp %#lx)", current->proc->name, current->proc->pid,
             (unsigned long)fr.saved.rip, (unsigned long)fr.saved.rsp);
        process_exit(SIGSEGV);
    }
    *tf = fr.saved;
    tf->cs = USER_CS;                   /* never trust the saved selectors: stay in ring 3 */
    tf->ss = USER_DS;
    tf->rflags = (tf->rflags & 0xDD5) | RFLAGS_IF | 2;
    current->sig_blocked = fr.saved_mask & ~(u32)(sigbit(SIGKILL) | sigbit(SIGSTOP));
    return (long)tf->rax;
}
