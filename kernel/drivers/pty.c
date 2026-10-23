/*
 * Pseudo-terminals. SYS_ptyopen creates a pair: the caller gets the master (a file descriptor with read/write/poll), the
 * slave appears as /dev/pts/N and is an ordinary terminal for the programs the terminal emulator starts. Bytes written to
 * the master go through the slave's line discipline (echo, ^C, canonical editing); bytes the programs write to the slave
 * come out of the master's read, with "\n" turned into "\r\n".
 */
#include "tty.h"
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "initcall.h"

#define PTY_MAX 32
#define PTY_BUF 32768                   /* power of two */

typedef struct Pty {
    bool  in_use;
    int   index;
    Tty   slave;
    u8   *buf;                          /* program output waiting for the terminal emulator */
    u32   r, w;
    bool  master_open;
    bool  hup;                          /* master closed */
    bool  slave_seen;                   /* a program has opened the slave at least once */
    int   slave_opens;
    WaitQueue m_wq;                     /* master reader waits for output */
    WaitQueue s_wq;                     /* slave writer waits for ring space */
    char  path[32];                     /* "pts/N" relative to /dev */
} Pty;

static Pty g_ptys[PTY_MAX];
extern void (*g_signal_self_hook)(int sig);

static u32 ring_count(Pty *p) { return p->w - p->r; }

/* Best effort, never blocks: used for echo (interrupt context) and as the tail of a slave write. */
static void pty_tty_out(Tty *t, const u8 *buf, size_t n) {
    Pty *p = t->priv;
    if (!p->buf) return;
    bool onlcr = (t->termios.c_oflag & OPOST) && (t->termios.c_oflag & ONLCR);
    for (size_t i = 0; i < n; i++) {
        if (onlcr && buf[i] == '\n' && ring_count(p) < PTY_BUF) p->buf[p->w++ & (PTY_BUF - 1)] = '\r';
        if (ring_count(p) >= PTY_BUF) break;
        p->buf[p->w++ & (PTY_BUF - 1)] = buf[i];
    }
    wq_wake_all(&p->m_wq);
}

static const TtyOps pty_tty_ops = { pty_tty_out };

static void clear_ctty_cb(Process *proc, void *ctx) {
    if (proc->ctty == ctx) proc->ctty = NULL;
}

static void pty_release(Pty *p) {
    char path[48];
    snprintf(path, sizeof path, "%s", p->path);
    devfs_unregister(path);
    process_for_each(clear_ctty_cb, &p->slave);
    u64 fl = irq_save();
    kfree(p->buf);
    p->buf = NULL;
    p->in_use = false;
    irq_restore(fl);
}

/* ---------------------------------------------------------------- master side (a File with FileOps) */

static ssize_t master_read(File *f, void *buf, size_t n) {
    Pty *p = f->priv;
    if (!n) return 0;
    u64 fl = irq_save();
    while (!ring_count(p)) {
        if (p->slave_seen && p->slave_opens == 0) { irq_restore(fl); return 0; }       /* every program has gone */
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&p->m_wq);
        current->intr = false;
    }
    size_t got = 0;
    u8 *out = buf;
    while (got < n && ring_count(p)) out[got++] = p->buf[p->r++ & (PTY_BUF - 1)];
    wq_wake_all(&p->s_wq);
    irq_restore(fl);
    return (ssize_t)got;
}

static ssize_t master_write(File *f, const void *buf, size_t n) {
    Pty *p = f->priv;
    const u8 *in = buf;
    size_t done = 0;
    while (done < n) {
        size_t chunk = MIN(n - done, (size_t)512);
        tty_receive(&p->slave, in + done, chunk);                 /* line discipline: echo, signals, canonical editing */
        done += chunk;
        if (done < n) task_yield();                               /* let the slave side drain the input buffer */
    }
    return (ssize_t)done;
}

static int master_poll(File *f) {
    Pty *p = f->priv;
    int m = POLLOUT;
    u64 fl = irq_save();
    if (ring_count(p)) m |= POLLIN;
    if (p->slave_seen && p->slave_opens == 0) m |= POLLIN | POLLHUP;
    irq_restore(fl);
    return m;
}

static int master_ioctl(File *f, unsigned long req, void *arg) {
    Pty *p = f->priv;
    if (req == FIONREAD) { *(int *)arg = (int)ring_count(p); return 0; }
    return tty_ioctl(&p->slave, req, arg);                       /* termios and window size live on the slave */
}

static void master_close(File *f) {
    Pty *p = f->priv;
    u64 fl = irq_save();
    p->master_open = false;
    p->hup = true;
    int pg = p->slave.fg_pgid;
    bool free_now = p->slave_opens == 0;
    p->slave.eof = true;
    wq_wake_all(&p->slave.rd_wq);
    wq_wake_all(&p->s_wq);
    irq_restore(fl);
    if (pg && g_tty_signal_hook) g_tty_signal_hook(pg, SIGHUP);   /* the terminal went away: tell the foreground job */
    if (free_now) pty_release(p);
}

static const FileOps master_fops = { .read = master_read, .write = master_write, .ioctl = master_ioctl, .close = master_close, .poll = master_poll };
static const VNodeOps anon_ops = { 0 };

/* ---------------------------------------------------------------- slave side (/dev/pts/N) */

static ssize_t slave_read(VNode *v, File *f, void *buf, size_t n, u64 off) {
    (void)off;
    Pty *p = v->priv;
    if (p->hup && !tty_pending(&p->slave)) return 0;
    ssize_t r = tty_read(&p->slave, buf, n, f && (f->flags & O_NONBLOCK));
    if (r == 0 && !p->hup) return 0;
    return r;
}

static ssize_t slave_write(VNode *v, File *f, const void *buf, size_t n, u64 off) {
    (void)off;
    Pty *p = v->priv;
    const u8 *in = buf;
    size_t done = 0;
    while (done < n) {
        if (p->hup) { if (g_signal_self_hook) g_signal_self_hook(SIGHUP); return done ? (ssize_t)done : -EIO; }
        size_t chunk = MIN(n - done, (size_t)1024);
        u64 fl = irq_save();
        while (PTY_BUF - ring_count(p) < chunk * 2 && !p->hup) {   /* worst case every byte is a newline */
            if (f && (f->flags & O_NONBLOCK)) { irq_restore(fl); return done ? (ssize_t)done : -EAGAIN; }
            if (signal_pending()) { irq_restore(fl); return done ? (ssize_t)done : -EINTR; }
            wq_wake_all(&p->m_wq);
            current->intr = true;
            wq_block(&p->s_wq);
            current->intr = false;
        }
        irq_restore(fl);
        tty_write(&p->slave, in + done, chunk);
        done += chunk;
    }
    return (ssize_t)done;
}

static int slave_ioctl(VNode *v, File *f, unsigned long req, void *arg) {
    (void)f;
    Pty *p = v->priv;
    return tty_ioctl(&p->slave, req, arg);
}

static int slave_poll(VNode *v, File *f) {
    (void)f;
    Pty *p = v->priv;
    return POLLOUT | (tty_pending(&p->slave) || p->slave.eof || p->hup ? POLLIN : 0);
}

static void slave_opened(VNode *v, File *f) {
    (void)f;
    Pty *p = v->priv;
    u64 fl = irq_save();
    p->slave_opens++;
    p->slave_seen = true;
    irq_restore(fl);
    Process *proc = current ? current->proc : NULL;
    if (proc && proc->sid == proc->pid && !proc->ctty) {            /* a session leader adopts its first terminal */
        proc->ctty = &p->slave;
        p->slave.session = proc->sid;
        p->slave.fg_pgid = proc->pgid;
    }
}

static void slave_closed(VNode *v, File *f) {
    (void)f;
    Pty *p = v->priv;
    u64 fl = irq_save();
    p->slave_opens--;
    bool gone = p->slave_opens == 0;
    bool free_now = gone && !p->master_open;
    if (gone) wq_wake_all(&p->m_wq);                                 /* master readers see end of file */
    irq_restore(fl);
    if (free_now) pty_release(p);
}

static const VNodeOps slave_ops = { .read = slave_read, .write = slave_write, .ioctl = slave_ioctl, .poll = slave_poll,
                                    .opened = slave_opened, .closed = slave_closed };

/* ---------------------------------------------------------------- allocation */

static long sys_ptyopen(TrapFrame *tf) {
    Pty *p = NULL;
    u64 fl = irq_save();
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].in_use) { p = &g_ptys[i]; p->in_use = true; p->index = i; break; }
    }
    irq_restore(fl);
    if (!p) return -ENOSPC;
    p->buf = kmalloc(PTY_BUF);
    if (!p->buf) { p->in_use = false; return -ENOMEM; }
    p->r = p->w = 0;
    p->master_open = true;
    p->hup = false;
    p->slave_seen = false;
    p->slave_opens = 0;
    wq_init(&p->m_wq);
    wq_init(&p->s_wq);
    snprintf(p->path, sizeof p->path, "pts/%d", p->index);
    tty_init(&p->slave, &pty_tty_ops, p, p->path);
    p->slave.winsize.ws_row = 24;
    p->slave.winsize.ws_col = 80;

    int rc = devfs_register(p->path, S_IFCHR | 0620, &slave_ops, p, 0x8800 + (u64)p->index);
    if (rc) { kfree(p->buf); p->buf = NULL; p->in_use = false; return rc; }
    /* the slave node belongs to whoever opened the pair, so their programs (and only they) can open it */
    VNode *n = NULL, *dir = devfs_root();
    vnode_get(dir);
    VNode *pts = NULL;
    if (dir->ops->lookup(dir, "pts", &pts) == 0) {
        if (pts->ops->lookup(pts, strrchr(p->path, '/') + 1, &n) == 0) {
            n->uid = current->proc->cred.uid;
            n->gid = current->proc->cred.gid;
            vnode_put(n);
        }
        vnode_put(pts);
    }
    vnode_put(dir);

    VNode *v = vnode_alloc(&anon_ops, NULL, S_IFCHR | 0600);
    File *m = v ? file_new(v, O_RDWR, &master_fops, p) : NULL;
    if (!m) { devfs_unregister(p->path); kfree(p->buf); p->buf = NULL; p->in_use = false; return -ENOMEM; }
    long fd = fd_install(current->proc, m, 0, 0);
    if (fd < 0) { file_put(m); return fd; }

    char path[40];
    int len = snprintf(path, sizeof path, "/dev/%s", p->path);
    if (A0(tf) && A1(tf) > (u64)len && copy_to_user((void *)A0(tf), path, (size_t)len + 1)) {
        fd_close(current->proc, (int)fd);
        return -EFAULT;
    }
    return fd;
}

static void pty_init(void) {
    syscall_register(SYS_ptyopen, sys_ptyopen);
}
INITCALL(pty_init);
