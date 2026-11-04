/* Anonymous pipes ("gaps": things go in on one side of the boundary and come out on the other). */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"

#define PIPE_SIZE 16384

typedef struct Pipe {
    u8  buf[PIPE_SIZE];
    u32 head, tail;                 /* free-running indices */
    int readers, writers;
    WaitQueue rd_wq, wr_wq;
} Pipe;

void (*g_signal_self_hook)(int sig);

static int g_npipes;                    /* pipes alive (statistics) */
int pipes_alive(void) { return g_npipes; }

static u32 pipe_count(Pipe *p) { return p->head - p->tail; }

static ssize_t pipe_read(File *f, void *buf, size_t n) {
    Pipe *p = f->priv;
    if (!n) return 0;
    u64 fl = irq_save();
    while (pipe_count(p) == 0) {
        if (p->writers == 0) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&p->rd_wq);
        current->intr = false;
    }
    size_t got = 0;
    u8 *out = buf;
    while (got < n && pipe_count(p)) out[got++] = p->buf[p->tail++ % PIPE_SIZE];
    wq_wake_all(&p->wr_wq);
    irq_restore(fl);
    return (ssize_t)got;
}

static ssize_t pipe_write(File *f, const void *buf, size_t n) {
    Pipe *p = f->priv;
    const u8 *in = buf;
    size_t done = 0;
    u64 fl = irq_save();
    while (done < n) {
        if (p->readers == 0) {
            irq_restore(fl);
            if (g_signal_self_hook) g_signal_self_hook(SIGPIPE);
            return done ? (ssize_t)done : -EPIPE;
        }
        if (pipe_count(p) == PIPE_SIZE) {
            if (f->flags & O_NONBLOCK) { irq_restore(fl); return done ? (ssize_t)done : -EAGAIN; }
            if (signal_pending()) { irq_restore(fl); return done ? (ssize_t)done : -EINTR; }
            wq_wake_all(&p->rd_wq);
            current->intr = true;
            wq_block(&p->wr_wq);
            current->intr = false;
            continue;
        }
        while (done < n && pipe_count(p) < PIPE_SIZE) p->buf[p->head++ % PIPE_SIZE] = in[done++];
        wq_wake_all(&p->rd_wq);
    }
    irq_restore(fl);
    return (ssize_t)done;
}

static int pipe_ioctl(File *f, unsigned long req, void *arg) {
    Pipe *p = f->priv;
    if (req == FIONREAD) { *(int *)arg = (int)pipe_count(p); return 0; }
    return -ENOTTY;
}

static void pipe_close(File *f) {
    Pipe *p = f->priv;
    u64 fl = irq_save();
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else p->writers--;
    bool dead = p->readers == 0 && p->writers == 0;
    wq_wake_all(&p->rd_wq);
    wq_wake_all(&p->wr_wq);
    if (dead) g_npipes--;
    irq_restore(fl);
    if (dead) kfree(p);
}

static int pipe_poll(File *f) {
    Pipe *p = f->priv;
    int m = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        if (pipe_count(p)) m |= POLLIN;
        if (p->writers == 0) m |= POLLHUP;
    } else {
        if (pipe_count(p) < PIPE_SIZE) m |= POLLOUT;
        if (p->readers == 0) m |= POLLERR;
    }
    return m;
}

static const FileOps pipe_fops = { .read = pipe_read, .write = pipe_write, .ioctl = pipe_ioctl, .close = pipe_close, .poll = pipe_poll };
static const VNodeOps pipe_vnode_ops = { 0 };

int vfs_pipe(File **rd, File **wr) {
    Pipe *p = kzalloc(sizeof *p);
    VNode *v = vnode_alloc(&pipe_vnode_ops, NULL, S_IFIFO | 0600);
    if (!p || !v) { kfree(p); if (v) vnode_put(v); return -ENOMEM; }
    wq_init(&p->rd_wq);
    wq_init(&p->wr_wq);
    p->readers = p->writers = 1;
    vnode_get(v);                                           /* one reference per file */
    File *r = file_new(v, O_RDONLY, &pipe_fops, p);
    File *w = r ? file_new(v, O_WRONLY, &pipe_fops, p) : NULL;
    if (!r || !w) {                                         /* out of memory: give back what was made (the files' close would count readers) */
        if (r) { r->fops = NULL; file_put(r); } else vnode_put(v);
        vnode_put(v);
        kfree(p);
        return -ENOMEM;
    }
    u64 fl = irq_save();
    g_npipes++;
    irq_restore(fl);
    *rd = r;
    *wr = w;
    return 0;
}
