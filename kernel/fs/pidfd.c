/*
 * pidfd: a descriptor that names a process. Written from pidfd_open(2), pidfd_send_signal(2) and waitid(2) (P_PIDFD).
 *
 * The descriptor holds the process's pid. This kernel never hands out a pid twice (process ids and thread ids come from one counter that only grows,
 * kernel/sys/process.c), so the number alone names the process and not whoever gets the number next: once the process has been reaped, process_by_pid finds
 * nothing and pidfd_send_signal is ESRCH, as the manual page promises. The descriptor polls readable (POLLIN) when the process has exited, whether or not it has
 * been reaped since; closing it releases nothing but itself.
 *
 * Readiness: every pidfd shares one wait queue, g_pidfd_wq, which process_finalize (kernel/sys/process.c) wakes through pidfd_notify when a process becomes a
 * zombie. One queue for all, because a pidfd holds a number and no reference to the process, and exits are rare: a waiter woken for another process polls again.
 */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "pollq.h"

typedef struct Pidfd { int pid; } Pidfd;

static int pfd_poll(File *f) {
    Pidfd *d = f->priv;
    u64 fl = irq_save();
    Process *p = process_by_pid(d->pid);
    bool gone = !p || p->state == PROC_ZOMBIE;
    irq_restore(fl);
    return gone ? POLLIN : 0;
}

static WaitQueue g_pidfd_wq;                    /* all zero is an empty queue (wq_init) */

/* A process has just become a zombie (kernel/sys/process.c, process_finalize). */
void pidfd_notify(void) {
    u64 fl = irq_save();
    wq_wake_all(&g_pidfd_wq);
    irq_restore(fl);
}

static WaitQueue *pfd_pollwq(File *f) { (void)f; return &g_pidfd_wq; }

static ssize_t pfd_read(File *f, void *buf, size_t n) { (void)f; (void)buf; (void)n; return -EINVAL; }
static void pfd_close(File *f) { kfree(f->priv); }

static const FileOps pfd_fops = { .read = pfd_read, .close = pfd_close, .poll = pfd_poll, .pollwq = pfd_pollwq };
static const VNodeOps pfd_node_ops = { 0 };

struct ThreadId { int id; bool found; };
static void thread_cb(Process *p, void *ctx) {
    struct ThreadId *a = ctx;
    if (p->state == PROC_ZOMBIE) return;
    for (Task *t = p->threads; t && !a->found; t = t->thread_next) a->found = t->utid == a->id;
}

/* A pidfd for process pid: ESRCH if there is none (a zombie that has not been reaped yet still counts, as on Linux). */
long pidfd_create(int pid, int flags, File **out) {
    u64 fl = irq_save();
    Process *p = process_by_pid(pid);
    bool ok = p && p->pid == pid;
    struct ThreadId a = { pid, false };
    if (!ok) process_for_each(thread_cb, &a);
    irq_restore(fl);
    if (!ok) return a.found ? -ENOENT : -ESRCH;       /* the id of a thread that is not its process's first: ENOENT is what Linux 6.18 answers (the oracle; the manual page's
                                                       * EINVAL is what older kernels said). There is no PIDFD_THREAD here. */
    Pidfd *d = kzalloc(sizeof *d);
    if (!d) return -ENOMEM;
    d->pid = pid;
    VNode *v = vnode_alloc(&pfd_node_ops, NULL, S_IFREG | 0600);
    File *f = v ? file_new(v, O_RDWR | (flags & O_NONBLOCK), &pfd_fops, d) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        kfree(d);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

/* The pid a pidfd names, or -EBADF when f is not a pidfd. */
int pidfd_pid(const File *f) { return f && f->fops == &pfd_fops ? ((const Pidfd *)f->priv)->pid : -EBADF; }
