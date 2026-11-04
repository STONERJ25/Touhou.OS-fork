/* Threads: the system calls that create and end them, and thread-directed signals. See docs/THREADS.md.
 *
 * A thread is a Task that lives inside a Process: it shares the address space, descriptors, cwd, umask, credentials and
 * signal dispositions, and owns a kernel stack, a TLS base (FS), a signal mask and a thread id. Alice strings the puppets. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "initcall.h"
#include "irq.h"

static long sys_thread_create(TrapFrame *tf) {
    Process *p = current->proc;
    u64 flags = A0(tf), stack = A1(tf), tls = A2(tf), tid_addr = A3(tf), entry = A4(tf), arg = A5(tf);
    if (flags) return -EINVAL;
    if (!entry || !stack || entry >= USER_TOP || stack >= USER_TOP || tls >= USER_TOP) return -EINVAL;
    if (tid_addr && (tid_addr & 3 || !user_range_ok((void *)tid_addr, 4))) return -EFAULT;
    if (p->nthreads >= THREADS_PER_PROC_MAX || thread_count() >= THREADS_TOTAL_MAX) return -EAGAIN;

    int utid = process_alloc_id();
    if (tid_addr && copy_to_user((void *)tid_addr, &utid, sizeof utid)) return -EFAULT;

    TrapFrame ntf;
    memset(&ntf, 0, sizeof ntf);
    ntf.rip = entry;
    ntf.rsp = stack;
    ntf.rdi = arg;
    ntf.cs = USER_CS;
    ntf.ss = USER_DS;
    ntf.rflags = RFLAGS_IF | 2;
    ThreadInit ti = { .utid = utid, .fs_base = tls, .clear_tid = tid_addr, .sig_blocked = current->sig_blocked };
    Task *t = task_create_thread(p->name, &ntf, p, &ti);
    if (!t) return -EAGAIN;
    return utid;
}

static long sys_thread_exit(TrapFrame *tf) {
    (void)tf;
    thread_leave();
}

static long sys_gettid(TrapFrame *tf) { (void)tf; return current->utid; }

static long sys_exit_group(TrapFrame *tf) { process_exit((int)((A0(tf) & 0xFF) << 8)); }

static long sys_set_tls(TrapFrame *tf) {
    if (A0(tf) >= USER_TOP) return -EINVAL;
    task_set_fs_base(current, A0(tf));
    return 0;
}

/* tgkill(pid, tid, sig): the signal is aimed at one thread of one process (pthread_kill, raise). */
static long sys_tgkill(TrapFrame *tf) {
    if (A0(tf) > 0x7fffffffUL || A1(tf) > 0x7fffffffUL || A2(tf) >= NSIG) return -EINVAL;
    int pid = (int)A0(tf), tid = (int)A1(tf), sig = (int)A2(tf);
    Process *self = current->proc;
    Process *p = process_by_pid(pid);
    if (!p || p->state == PROC_ZOMBIE) return -ESRCH;
    if (!signal_permitted(self, p)) return -EPERM;
    u64 f = irq_save();                             /* a thread cannot leave while we look at the list */
    Task *found = NULL;
    for (Task *t = p->threads; t; t = t->thread_next)
        if (t->utid == tid) { found = t; break; }
    if (found && sig) signal_send_thread(found, sig);
    irq_restore(f);
    return found ? 0 : -ESRCH;
}

/* sigsuspend(mask): atomically replace the thread's mask and sleep until a signal is delivered. The handler runs under the
 * temporary mask and sigreturn brings the old one back (Task.saved_mask). Always returns -EINTR. */
static long sys_sigsuspend(TrapFrame *tf) {
    Task *t = current;
    u64 f = irq_save();
    t->saved_mask = t->sig_blocked;
    t->saved_mask_valid = true;
    t->sig_blocked = (u32)A0(tf) & ~((1U << SIGKILL) | (1U << SIGSTOP));
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

/* sigtimedwait(set, timeout): accepts one pending signal of `set` (the caller normally blocks them) and returns its number
 * without running a handler. Timeout is a relative struct gtimespec* or NULL; -EAGAIN when it expires, -EINTR when some
 * other signal needs a handler. */
static long sys_sigtimedwait(TrapFrame *tf) {
    Task *t = current;
    Process *p = t->proc;
    u32 set = (u32)A0(tf) & ~((1U << SIGKILL) | (1U << SIGSTOP));
    u64 deadline = ~0ULL;
    if (A1(tf)) {
        long rc0 = deadline_from_timespec((void *)A1(tf), &deadline);
        if (rc0) return rc0;
    }
    long rc;
    u64 f = irq_save();
    t->sigwait_set = set;
    for (;;) {
        u32 hit = (t->sig_pending | p->sig_pending) & set;
        if (hit) {
            int sig = __builtin_ctz(hit);
            t->sig_pending &= ~(1U << sig);
            p->sig_pending &= ~(1U << sig);
            rc = sig;
            break;
        }
        /* something else that needs attention (a handler, or the thread must die) */
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

static long sys_sigpending(TrapFrame *tf) {
    Task *t = current;
    u32 pend = (t->sig_pending | t->proc->sig_pending) & t->sig_blocked;
    return copy_to_user((void *)A0(tf), &pend, sizeof pend);
}

static void thread_init(void) {
    syscall_register(SYS_sigsuspend, sys_sigsuspend);
    syscall_register(SYS_sigtimedwait, sys_sigtimedwait);
    syscall_register(SYS_sigpending, sys_sigpending);
    syscall_register(SYS_thread_create, sys_thread_create);
    syscall_register(SYS_thread_exit, sys_thread_exit);
    syscall_register(SYS_gettid, sys_gettid);
    syscall_register(SYS_exit_group, sys_exit_group);
    syscall_register(SYS_set_tls, sys_set_tls);
    syscall_register(SYS_tgkill, sys_tgkill);
}
INITCALL(thread_init);
