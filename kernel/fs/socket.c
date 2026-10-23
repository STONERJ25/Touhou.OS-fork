/*
 * UNIX-domain stream sockets: the "gaps" Yukari opens between processes. The window server, its clients and anything
 * else that wants a conversation uses them. Bound sockets are special files in tmpfs directories (/BambooForest, /dev).
 *
 * A connection is a Conn: two byte queues (one per direction) and a wait queue shared by both ends. Each end is a Sock
 * owned by one open File. Everything is protected by disabling interrupts (single CPU); blocking uses wait queues.
 */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "initcall.h"

#define SOCK_Q      32768                       /* bytes per direction; power of two */
#define BACKLOG_MAX 32

typedef struct Conn {
    u8  *q[2];                                  /* q[s] is read by side s (written by side 1-s) */
    u32  r[2], w[2];                            /* free-running counters */
    bool closed[2];
    bool shut_wr[2];
    int  refs;                                  /* ends still open */
    WaitQueue wq;
    int  pid[2];
    u32  uid[2], gid[2];
} Conn;

typedef enum { SS_NEW, SS_LISTEN, SS_CONN } SState;

typedef struct Sock {
    SState state;
    Conn  *c;
    int    side;
    Conn  *pending[BACKLOG_MAX];
    int    npending, backlog;
    WaitQueue accept_wq;
    VNode *node;                                /* the bound socket file (listeners) */
    bool   bound;
} Sock;

extern void (*g_signal_self_hook)(int sig);

static const VNodeOps sock_node_ops;            /* identifies socket files; no data operations */
static const FileOps sock_fops;

static Conn *conn_new(void) {
    Conn *c = kzalloc(sizeof *c);
    if (!c) return NULL;
    c->q[0] = kmalloc(SOCK_Q);
    c->q[1] = kmalloc(SOCK_Q);
    if (!c->q[0] || !c->q[1]) { kfree(c->q[0]); kfree(c->q[1]); kfree(c); return NULL; }
    wq_init(&c->wq);
    c->refs = 2;
    return c;
}

static void conn_unref(Conn *c) {                /* IRQs disabled by the caller */
    if (--c->refs > 0) return;
    kfree(c->q[0]);
    kfree(c->q[1]);
    kfree(c);
}

static u32 avail(Conn *c, int s) { return c->w[s] - c->r[s]; }

static File *sock_file(Sock *s, int flags) {
    VNode *v = vnode_alloc(&sock_node_ops, NULL, S_IFSOCK | 0600);
    if (!v) return NULL;
    File *f = file_new(v, O_RDWR | (flags & O_NONBLOCK), &sock_fops, s);
    if (!f) vnode_put(v);
    return f;
}

static Sock *sock_of(int fd, File **fout) {
    File *f = fd_get(current->proc, fd);
    if (!f || f->fops != &sock_fops) return NULL;
    if (fout) *fout = f;
    return f->priv;
}

/* ---------------------------------------------------------------- data path */

static ssize_t sock_read(File *f, void *buf, size_t n) {
    Sock *s = f->priv;
    if (s->state != SS_CONN) return -ENOTCONN;
    if (!n) return 0;
    Conn *c = s->c;
    int me = s->side;
    u64 fl = irq_save();
    while (!avail(c, me)) {
        if (c->closed[1 - me] || c->shut_wr[1 - me]) { irq_restore(fl); return 0; }
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&c->wq);
        current->intr = false;
    }
    size_t got = 0;
    u8 *out = buf;
    while (got < n && avail(c, me)) out[got++] = c->q[me][c->r[me]++ & (SOCK_Q - 1)];
    wq_wake_all(&c->wq);
    irq_restore(fl);
    return (ssize_t)got;
}

static ssize_t sock_write(File *f, const void *buf, size_t n) {
    Sock *s = f->priv;
    if (s->state != SS_CONN) return -ENOTCONN;
    Conn *c = s->c;
    int me = s->side, peer = 1 - me;
    const u8 *in = buf;
    size_t done = 0;
    u64 fl = irq_save();
    while (done < n) {
        if (c->closed[peer] || c->shut_wr[me]) {
            irq_restore(fl);
            if (g_signal_self_hook) g_signal_self_hook(SIGPIPE);
            return done ? (ssize_t)done : -EPIPE;
        }
        if (avail(c, peer) >= SOCK_Q) {
            if (f->flags & O_NONBLOCK) { irq_restore(fl); return done ? (ssize_t)done : -EAGAIN; }
            if (signal_pending()) { irq_restore(fl); return done ? (ssize_t)done : -EINTR; }
            wq_wake_all(&c->wq);
            current->intr = true;
            wq_block(&c->wq);
            current->intr = false;
            continue;
        }
        while (done < n && avail(c, peer) < SOCK_Q) c->q[peer][c->w[peer]++ & (SOCK_Q - 1)] = in[done++];
        wq_wake_all(&c->wq);
    }
    irq_restore(fl);
    return (ssize_t)done;
}

static int sock_poll(File *f) {
    Sock *s = f->priv;
    int m = 0;
    u64 fl = irq_save();
    if (s->state == SS_LISTEN) {
        if (s->npending) m |= POLLIN;
    } else if (s->state == SS_CONN) {
        Conn *c = s->c;
        int me = s->side, peer = 1 - me;
        if (avail(c, me)) m |= POLLIN;
        if (c->closed[peer] || c->shut_wr[peer]) m |= POLLIN | POLLHUP;
        if (!c->closed[peer] && avail(c, peer) < SOCK_Q) m |= POLLOUT;
    }
    irq_restore(fl);
    return m;
}

static int sock_ioctl(File *f, unsigned long req, void *arg) {
    Sock *s = f->priv;
    if (req == FIONREAD) {
        if (s->state != SS_CONN) return -ENOTCONN;
        *(int *)arg = (int)avail(s->c, s->side);
        return 0;
    }
    if (req == SOCK_PEERCRED) {
        if (s->state != SS_CONN) return -ENOTCONN;
        struct gpeercred *pc = arg;
        pc->pid = s->c->pid[1 - s->side];
        pc->uid = s->c->uid[1 - s->side];
        pc->gid = s->c->gid[1 - s->side];
        return 0;
    }
    return -ENOTTY;
}

static void sock_close(File *f) {
    Sock *s = f->priv;
    u64 fl = irq_save();
    if (s->state == SS_LISTEN) {
        for (int i = 0; i < s->npending; i++) {              /* nobody will ever accept these */
            Conn *c = s->pending[i];
            c->closed[1] = true;
            wq_wake_all(&c->wq);
            conn_unref(c);
        }
        s->npending = 0;
        if (s->node) { s->node->priv = NULL; }
        wq_wake_all(&s->accept_wq);
    } else if (s->state == SS_CONN) {
        Conn *c = s->c;
        int me = s->side;
        c->closed[me] = true;
        kfree(c->q[me]);                                      /* the peer can no longer write toward us */
        c->q[me] = NULL;
        wq_wake_all(&c->wq);
        if (c->refs == 1) { kfree(c->q[1 - me]); c->q[1 - me] = NULL; }
        conn_unref(c);
    }
    VNode *node = s->node;
    irq_restore(fl);
    if (node) vnode_put(node);
    kfree(s);
}

static const FileOps sock_fops = { .read = sock_read, .write = sock_write, .ioctl = sock_ioctl, .close = sock_close, .poll = sock_poll };
static const VNodeOps sock_node_ops = { 0 };

/* ---------------------------------------------------------------- syscalls */

static long fd_for(File *f, int type) {
    long fd = fd_install(current->proc, f, 0, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_socket(TrapFrame *tf) {
    int type = (int)A1(tf);
    if (A0(tf) != AF_UNIX) return -EAFNOSUPPORT;
    if ((type & 0xFF) != SOCK_STREAM) return -EPROTONOSUPPORT;
    Sock *s = kzalloc(sizeof *s);
    if (!s) return -ENOMEM;
    wq_init(&s->accept_wq);
    File *f = sock_file(s, (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    if (!f) { kfree(s); return -ENOMEM; }
    return fd_for(f, type);
}

static void fill_cred(Conn *c, int side) {
    c->pid[side] = current->proc ? current->proc->pid : 0;
    c->uid[side] = current->proc ? current->proc->cred.uid : 0;
    c->gid[side] = current->proc ? current->proc->cred.gid : 0;
}

static long sys_socketpair(TrapFrame *tf) {
    int type = (int)A0(tf);
    if ((type & 0xFF) != SOCK_STREAM) return -EPROTONOSUPPORT;
    Conn *c = conn_new();
    Sock *a = kzalloc(sizeof *a), *b = kzalloc(sizeof *b);
    if (!c || !a || !b) { if (c) { kfree(c->q[0]); kfree(c->q[1]); kfree(c); } kfree(a); kfree(b); return -ENOMEM; }
    a->state = b->state = SS_CONN;
    a->c = b->c = c;
    a->side = 0;
    b->side = 1;
    wq_init(&a->accept_wq);
    wq_init(&b->accept_wq);
    fill_cred(c, 0);
    fill_cred(c, 1);
    File *fa = sock_file(a, type & SOCK_NONBLOCK ? O_NONBLOCK : 0), *fb = sock_file(b, type & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    if (!fa || !fb) return -ENOMEM;
    int sv[2];
    sv[0] = (int)fd_for(fa, type);
    sv[1] = sv[0] >= 0 ? (int)fd_for(fb, type) : -EMFILE;
    if (sv[0] < 0 || sv[1] < 0) { if (sv[0] >= 0) fd_close(current->proc, sv[0]); return sv[0] < 0 ? sv[0] : sv[1]; }
    if (copy_to_user((void *)A1(tf), sv, sizeof sv)) return -EFAULT;
    return 0;
}

static long sys_bind(TrapFrame *tf) {
    File *f;
    Sock *s = sock_of((int)A0(tf), &f);
    if (!s) return -ENOTSOCK;
    if (s->state != SS_NEW || s->bound) return -EINVAL;
    char path[PATH_MAX];
    int n = strncpy_from_user(path, (const char *)A1(tf), sizeof path);
    if (n < 0) return n;
    Process *p = current->proc;
    VNode *dir;
    char name[GNAME_MAX + 1];
    int rc = vfs_lookup_parent(path, p->cwd, &p->cred, &dir, name);
    if (rc) return rc;
    if (dir->mount->type != &g_tmpfs_type && dir->mount->type != &g_devfs_type) { vnode_put(dir); return -ENOTSUP; }
    rc = vfs_access(dir, &p->cred, W_OK | X_OK);
    VNode *node = NULL;
    if (!rc) {
        rc = tmpfs_add_special(dir, name, S_IFSOCK | (0777 & ~p->umask), &sock_node_ops, s, 0, &node);
        if (rc == -EEXIST) rc = -EADDRINUSE;
    }
    vnode_put(dir);
    if (rc) return rc;
    node->uid = p->cred.uid;
    node->gid = p->cred.gid;
    s->node = node;                                           /* keep our reference until the socket closes */
    s->bound = true;
    return 0;
}

static long sys_listen(TrapFrame *tf) {
    Sock *s = sock_of((int)A0(tf), NULL);
    if (!s) return -ENOTSOCK;
    if (s->state == SS_CONN || !s->bound) return -EINVAL;
    int backlog = (int)A1(tf);
    s->backlog = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
    s->state = SS_LISTEN;
    return 0;
}

static long sys_connect(TrapFrame *tf) {
    File *f;
    Sock *s = sock_of((int)A0(tf), &f);
    if (!s) return -ENOTSOCK;
    if (s->state == SS_CONN) return -EISCONN;
    if (s->state != SS_NEW) return -EINVAL;
    char path[PATH_MAX];
    int n = strncpy_from_user(path, (const char *)A1(tf), sizeof path);
    if (n < 0) return n;
    Process *p = current->proc;
    VNode *v;
    int rc = vfs_lookup(path, p->cwd, &p->cred, 0, &v);
    if (rc) return rc == -ENOENT ? -ECONNREFUSED : rc;
    if (v->ops != &sock_node_ops) { vnode_put(v); return -ECONNREFUSED; }
    rc = vfs_access(v, &p->cred, W_OK);
    if (rc) { vnode_put(v); return rc; }
    Conn *c = conn_new();
    if (!c) { vnode_put(v); return -ENOMEM; }
    u64 fl = irq_save();
    Sock *l = v->priv;
    if (!l || l->state != SS_LISTEN || l->npending >= l->backlog) {
        irq_restore(fl);
        kfree(c->q[0]); kfree(c->q[1]); kfree(c);
        vnode_put(v);
        return -ECONNREFUSED;
    }
    fill_cred(c, 0);
    s->state = SS_CONN;
    s->c = c;
    s->side = 0;
    l->pending[l->npending++] = c;
    wq_wake_all(&l->accept_wq);
    irq_restore(fl);
    vnode_put(v);
    return 0;
}

static long sys_accept(TrapFrame *tf) {
    File *f;
    Sock *l = sock_of((int)A0(tf), &f);
    if (!l) return -ENOTSOCK;
    if (l->state != SS_LISTEN) return -EINVAL;
    u64 fl = irq_save();
    while (!l->npending) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&l->accept_wq);
        current->intr = false;
    }
    Conn *c = l->pending[0];
    memmove(l->pending, l->pending + 1, (size_t)(--l->npending) * sizeof(Conn *));
    fill_cred(c, 1);
    irq_restore(fl);
    Sock *ns = kzalloc(sizeof *ns);
    File *nf = ns ? sock_file(ns, 0) : NULL;
    if (!nf) {
        u64 f2 = irq_save();
        c->closed[1] = true;
        wq_wake_all(&c->wq);
        conn_unref(c);
        irq_restore(f2);
        kfree(ns);
        return -ENOMEM;
    }
    ns->state = SS_CONN;
    ns->c = c;
    ns->side = 1;
    wq_init(&ns->accept_wq);
    return fd_for(nf, 0);
}

static long sys_shutdown(TrapFrame *tf) {
    Sock *s = sock_of((int)A0(tf), NULL);
    if (!s) return -ENOTSOCK;
    if (s->state != SS_CONN) return -ENOTCONN;
    int how = (int)A1(tf);
    if (how == SHUT_WR || how == SHUT_RDWR) {
        u64 fl = irq_save();
        s->c->shut_wr[s->side] = true;
        wq_wake_all(&s->c->wq);
        irq_restore(fl);
    }
    return 0;
}

static void socket_init(void) {
    syscall_register(SYS_socket, sys_socket);
    syscall_register(SYS_bind, sys_bind);
    syscall_register(SYS_listen, sys_listen);
    syscall_register(SYS_accept, sys_accept);
    syscall_register(SYS_connect, sys_connect);
    syscall_register(SYS_shutdown, sys_shutdown);
    syscall_register(SYS_socketpair, sys_socketpair);
}
INITCALL(socket_init);
