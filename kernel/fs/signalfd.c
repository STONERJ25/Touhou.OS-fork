/*
 * signalfd: the signals of a set, read from a descriptor instead of delivered to a handler. Written from signalfd(2) (the call, the flags, the record layout and
 * what a read and poll do) and signal(7).
 *
 * A read takes pending signals of the descriptor's set out of the calling thread's and its process's pending sets (kernel/sys/signal.c, signal_wait: the same
 * path as sigtimedwait, so a queued real-time signal brings its value and one record comes out per instance) and writes one 128-byte struct signalfd_siginfo for
 * each, as many as the buffer holds. It waits while none is pending (unless O_NONBLOCK: EAGAIN); a buffer smaller than one record is EINVAL. The descriptor polls
 * readable while one of its signals is pending for the thread that polls. The program is expected to block the signals (otherwise they may be delivered first).
 *
 * Readiness: every signalfd shares one wait queue, g_sigfd_wq, which the one place where a signal becomes pending (deposit, kernel/sys/signal.c) wakes through
 * signalfd_notify. One queue for all of them, and not one per process, because the queue a file names must stay valid until the file is released (include/pollq.h)
 * and a signalfd outlives the process that made it (fork, SCM_RIGHTS, an epoll set that holds it): a waiter woken for somebody else's signal polls again and finds
 * nothing, which is harmless.
 */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "pollq.h"

typedef struct Sigfd {
    u64 mask;                                   /* the kernel's signal set (siginfo.h), never SIGKILL or SIGSTOP */
} Sigfd;

/* struct signalfd_siginfo of signalfd(2): 128 bytes */
struct sfd_info {
    u32 ssi_signo;
    s32 ssi_errno, ssi_code;
    u32 ssi_pid, ssi_uid;
    s32 ssi_fd;
    u32 ssi_tid, ssi_band, ssi_overrun, ssi_trapno;
    s32 ssi_status, ssi_int;
    u64 ssi_ptr, ssi_utime, ssi_stime, ssi_addr;
    u16 ssi_addr_lsb;
    u8  pad[46];
};
_Static_assert(sizeof(struct sfd_info) == 128, "struct signalfd_siginfo is 128 bytes");

static u64 u64_at(const SigInfo *i, int off) { u64 v; memcpy(&v, i->u + off, 8); return v; }

/* Which fields a siginfo fills depends on where the signal came from (signal(7), "siginfo_t"): a child's state change, a POSIX timer, a fault, or a process (kill
 * gives the sender; sigqueue and the other negative codes add the value). */
static void convert(const SigInfo *in, struct sfd_info *o) {
    memset(o, 0, sizeof *o);
    o->ssi_signo = (u32)in->si_signo;
    o->ssi_errno = in->si_errno;
    o->ssi_code = in->si_code;
    int s = in->si_signo, c = in->si_code;
    bool fault = s == SIGSEGV || s == SIGBUS || s == SIGILL || s == SIGFPE || s == SIGTRAP;
    if (s == SIGCHLD && c > 0) {
        o->ssi_pid = (u32)siginfo_get_s32(in, 0);
        o->ssi_uid = (u32)siginfo_get_s32(in, 4);
        o->ssi_status = siginfo_get_s32(in, 8);
        o->ssi_utime = u64_at(in, 16);
        o->ssi_stime = u64_at(in, 24);
    } else if (c == SI_TIMER) {
        o->ssi_tid = (u32)siginfo_get_s32(in, 0);
        o->ssi_overrun = (u32)siginfo_get_s32(in, 4);
        o->ssi_ptr = u64_at(in, 8);
        o->ssi_int = (s32)o->ssi_ptr;
    } else if (c > 0 && fault) {
        o->ssi_addr = u64_at(in, 0);
    } else {
        o->ssi_pid = (u32)siginfo_get_s32(in, 0);
        o->ssi_uid = (u32)siginfo_get_s32(in, 4);
        if (c < 0) {                                /* sigqueue, tgkill, a message queue ...: they carry a value */
            o->ssi_ptr = u64_at(in, 8);
            o->ssi_int = (s32)o->ssi_ptr;
        }
    }
}

static ssize_t sfd_read(File *f, void *buf, size_t n) {
    Sigfd *s = f->priv;
    if (n < sizeof(struct sfd_info)) return -EINVAL;
    size_t got = 0;
    while (got + sizeof(struct sfd_info) <= n) {
        SigInfo info;
        bool wait = got == 0 && !(f->flags & O_NONBLOCK);
        long rc = signal_wait(s->mask, wait ? ~0ULL : 0, &info);       /* deadline 0: only what is pending now */
        if (rc < 0) {
            if (got) break;
            return rc;                              /* -EAGAIN (nothing pending) or -EINTR (another signal wants its handler) */
        }
        struct sfd_info o;
        convert(&info, &o);
        memcpy((u8 *)buf + got, &o, sizeof o);
        got += sizeof o;
    }
    return (ssize_t)got;
}

static int sfd_poll(File *f) {
    Sigfd *s = f->priv;
    Task *t = current;
    u64 fl = irq_save();
    bool any = ((t->sig_pending | (t->proc ? t->proc->sig_pending : 0)) & s->mask) != 0;
    irq_restore(fl);
    return any ? POLLIN : 0;
}

static WaitQueue g_sigfd_wq;                    /* all zero is an empty queue (wq_init) */
static int g_sigfds;                            /* signalfds alive: the signal path does nothing while there is none */

/* A signal became pending somewhere (kernel/sys/signal.c, deposit; irq off, possibly in an interrupt handler). */
void signalfd_notify(void) { if (g_sigfds) wq_wake_all(&g_sigfd_wq); }

static WaitQueue *sfd_pollwq(File *f) { (void)f; return &g_sigfd_wq; }

static void sfd_close(File *f) {
    u64 fl = irq_save();
    g_sigfds--;
    irq_restore(fl);
    kfree(f->priv);
}

static const FileOps sfd_fops = { .read = sfd_read, .close = sfd_close, .poll = sfd_poll, .pollwq = sfd_pollwq };
static const VNodeOps sfd_node_ops = { 0 };

long signalfd_create(u64 mask, int flags, File **out) {
    Sigfd *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    s->mask = mask & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
    VNode *v = vnode_alloc(&sfd_node_ops, NULL, S_IFREG | 0600);
    File *f = v ? file_new(v, O_RDONLY | (flags & O_NONBLOCK), &sfd_fops, s) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        kfree(s);
        return -ENOMEM;
    }
    u64 fl = irq_save();
    g_sigfds++;
    irq_restore(fl);
    *out = f;
    return 0;
}

/* signalfd with an existing descriptor: the set is replaced. EINVAL when the file is not a signalfd. */
long signalfd_set(File *f, u64 mask) {
    if (f->fops != &sfd_fops) return -EINVAL;
    ((Sigfd *)f->priv)->mask = mask & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
    u64 fl = irq_save();
    wq_wake_all(&g_sigfd_wq);                       /* a waiter's answer may be different under the new set */
    irq_restore(fl);
    return 0;
}

bool signalfd_is(const File *f) { return f->fops == &sfd_fops; }
