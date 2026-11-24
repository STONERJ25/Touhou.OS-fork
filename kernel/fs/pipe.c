/* Anonymous pipes ("gaps": things go in on one side of the boundary and come out on the other), and named pipes (FIFOs, below: the same object, found through a
 * filesystem node instead of handed out in pairs). */
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
    VNode *fifo;                    /* the named pipe's node (not referenced: the open files hold it), NULL for an anonymous pipe */
    u32 ropens, wopens;             /* FIFO: opens for reading / writing so far, so that a blocked open sees a partner that came and went at once */
    WaitQueue open_wq;              /* FIFO: opens waiting for their partner */
    struct Pipe *fifo_next;
} Pipe;

static Pipe *g_fifos;                   /* the named pipes that have openers (guarded by irq_save) */

/* Which counts a file of this pipe holds: O_RDWR (only for a FIFO) holds both. */
static bool is_reader(const File *f) { return (f->flags & O_ACCMODE) != O_WRONLY; }
static bool is_writer(const File *f) { return (f->flags & O_ACCMODE) != O_RDONLY; }

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
    if (p->fifo) wq_wake_all(&p->rd_wq);            /* a FIFO opened O_RDWR is watched on rd_wq (pipe_pollwq) and may be waiting for this room (POLLOUT) */
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
    if (is_reader(f)) p->readers--;
    if (is_writer(f)) p->writers--;
    bool dead = p->readers == 0 && p->writers == 0;
    wq_wake_all(&p->rd_wq);
    wq_wake_all(&p->wr_wq);
    if (dead) g_npipes--;
    if (dead && p->fifo)                            /* the last opener of a FIFO: the node forgets its pipe, and unread data goes with it (fifo(7)) */
        for (Pipe **pp = &g_fifos; *pp; pp = &(*pp)->fifo_next) if (*pp == p) { *pp = p->fifo_next; break; }
    irq_restore(fl);
    if (dead) kfree(p);
}

static int pipe_poll(File *f) {
    Pipe *p = f->priv;
    int m = 0;
    if (is_reader(f)) {
        if (pipe_count(p)) m |= POLLIN;
        /* POLLHUP: the last writer has gone. A FIFO that no writer has opened yet (a reader that opened with O_NONBLOCK) does not report it: Linux compares the
         * count of writing opens with what it was at this reader's open; here: none since the pipe was made (it is freed with its last opener). */
        if (p->writers == 0 && (f->flags & O_ACCMODE) == O_RDONLY && (!p->fifo || p->wopens)) m |= POLLHUP;
    }
    if (is_writer(f)) {
        if (pipe_count(p) < PIPE_SIZE) m |= POLLOUT;
        if (p->readers == 0) m |= POLLERR;
    }
    return m;
}

/* A reader waits for data (or for the last writer to leave) on rd_wq, a writer for room (or for the last reader to leave) on wr_wq: those are the queues to watch.
 * A FIFO opened for both is watched on rd_wq (what a program waits for on such a descriptor is data). */
static WaitQueue *pipe_pollwq(File *f) {
    Pipe *p = f->priv;
    return is_reader(f) ? &p->rd_wq : &p->wr_wq;
}

static const FileOps pipe_fops = { .read = pipe_read, .write = pipe_write, .ioctl = pipe_ioctl, .close = pipe_close, .poll = pipe_poll, .pollwq = pipe_pollwq };
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

/* ------------------------------------------------------------------ named pipes (FIFOs)
 *
 * Written from fifo(7), open(2) and POSIX.1-2017 (open, "O_NONBLOCK" for FIFOs). A FIFO is a filesystem node (mknod with S_IFIFO; only tmpfs makes them, see
 * kernel/linux/l3.c) whose opens meet in one pipe: the pipe is made by the first open and freed when the last open file goes, so the node itself holds no data.
 *   - O_RDONLY waits until a writer has opened (one that opened and closed meanwhile counts), O_WRONLY until a reader has; O_RDWR never waits.
 *   - O_RDONLY | O_NONBLOCK succeeds at once (and reads end of file while there is no writer); O_WRONLY | O_NONBLOCK without a reader fails with ENXIO.
 *   - A wait is ended by a signal (EINTR), and the open is undone.
 * From there on the files are a pipe's: reads, writes, SIGPIPE/EPIPE, FIONREAD, poll (POLLHUP when the last writer has gone), lseek's ESPIPE (vfs_lseek).
 * Takes over the caller's reference to v. */
static void fifo_unlist(Pipe *p) {
    for (Pipe **pp = &g_fifos; *pp; pp = &(*pp)->fifo_next) if (*pp == p) { *pp = p->fifo_next; break; }
    g_npipes--;
}

int fifo_open(VNode *v, int flags, File **out) {
    int acc = flags & O_ACCMODE;
    Pipe *fresh = kzalloc(sizeof *fresh);
    if (!fresh) { vnode_put(v); return -ENOMEM; }
    u64 fl = irq_save();
    Pipe *p = g_fifos;
    while (p && p->fifo != v) p = p->fifo_next;
    if (!p) {
        p = fresh;
        fresh = NULL;
        wq_init(&p->rd_wq);
        wq_init(&p->wr_wq);
        wq_init(&p->open_wq);
        p->fifo = v;
        p->fifo_next = g_fifos;
        g_fifos = p;
        g_npipes++;
    }
    if (acc == O_WRONLY && !p->readers && (flags & O_NONBLOCK)) {
        bool unused = !p->writers;
        if (unused) fifo_unlist(p);
        irq_restore(fl);
        if (unused) kfree(p);
        if (fresh) kfree(fresh);
        vnode_put(v);
        return -ENXIO;
    }
    if (acc != O_WRONLY) { p->readers++; p->ropens++; }
    if (acc != O_RDONLY) { p->writers++; p->wopens++; }
    wq_wake_all(&p->open_wq);
    wq_wake_all(&p->rd_wq);                         /* a poll on the other end may answer differently now */
    wq_wake_all(&p->wr_wq);
    int rc = 0;
    if (acc != O_RDWR && !(flags & O_NONBLOCK)) {
        u32 start = acc == O_RDONLY ? p->wopens : p->ropens;
        for (;;) {
            if (acc == O_RDONLY ? (p->writers || p->wopens != start) : (p->readers || p->ropens != start)) break;
            if (signal_pending()) { rc = -EINTR; break; }
            current->intr = true;
            wq_block(&p->open_wq);
            current->intr = false;
        }
    }
    irq_restore(fl);
    File *f = rc ? NULL : file_new(v, flags, &pipe_fops, p);       /* takes over the reference to the node */
    if (!rc && !f) rc = -ENOMEM;
    if (fresh) kfree(fresh);
    if (rc) {                                       /* undo this open */
        fl = irq_save();
        if (acc != O_WRONLY) p->readers--;
        if (acc != O_RDONLY) p->writers--;
        bool dead = !p->readers && !p->writers;
        if (dead) fifo_unlist(p);
        wq_wake_all(&p->rd_wq);
        wq_wake_all(&p->wr_wq);
        irq_restore(fl);
        if (dead) kfree(p);
        vnode_put(v);
        return rc;
    }
    *out = f;
    return 0;
}
