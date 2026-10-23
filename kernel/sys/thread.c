/* Threads: the system calls that create and end them, and thread-directed signals. See docs/THREADS.md.
 *
 * A thread is a Task that lives inside a Process: it shares the address space, descriptors, cwd, umask, credentials and
 * signal dispositions, and owns a kernel stack, a TLS base (FS), a signal mask and a thread id. Alice strings the puppets. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "initcall.h"

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
    int pid = (int)A0(tf), tid = (int)A1(tf), sig = (int)A2(tf);
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    Process *self = current->proc;
    Process *p = process_by_pid(pid);
    if (!p || p->state == PROC_ZOMBIE) return -ESRCH;
    if (self->cred.uid != 0 && self->cred.uid != p->cred.uid && self != p) return -EPERM;
    u64 f = irq_save();                             /* a thread cannot leave while we look at the list */
    Task *found = NULL;
    for (Task *t = p->threads; t; t = t->thread_next)
        if (t->utid == tid) { found = t; break; }
    if (found && sig) signal_send_thread(found, sig);
    irq_restore(f);
    return found ? 0 : -ESRCH;
}

static void thread_init(void) {
    syscall_register(SYS_thread_create, sys_thread_create);
    syscall_register(SYS_thread_exit, sys_thread_exit);
    syscall_register(SYS_gettid, sys_gettid);
    syscall_register(SYS_exit_group, sys_exit_group);
    syscall_register(SYS_set_tls, sys_set_tls);
    syscall_register(SYS_tgkill, sys_tgkill);
}
INITCALL(thread_init);
