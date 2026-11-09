/*
 * UNIX-domain sockets: the "gaps" Yukari opens between processes. The window server, its clients and anything else that wants a conversation uses them.
 * Bound sockets are special files in tmpfs directories (/BambooForest, /dev, /tmp), or live in the abstract namespace (a name that starts with a NUL byte,
 * kept in a table here, not in the file system). The same system calls also serve Internet sockets (AF_INET): when the descriptor (or the domain) says so they
 * are handed to kernel/net/sockcall.c.
 *
 * Streams. A connection is a Conn: two byte queues (one per direction) and a wait queue shared by both ends. Each end is a Sock owned by one open File.
 * Datagrams. A datagram Sock has a queue of whole messages (UMsg); a message is added by any socket that knows the name or was connected to it. A pair made by
 * socketpair() is two datagram sockets connected to each other. A datagram Sock can be pointed at by others (their default destination), so it is counted.
 * Descriptors. A message may carry open files (SCM_RIGHTS): the message holds a reference to each until a receiver turns them into descriptors of its own or the
 * message is thrown away. On a stream the files travel with the first byte of the data they were sent with (an Anc entry with the byte position), and a read
 * stops at that position so that the next read starts with them.
 *
 * Everything is protected by disabling interrupts (single CPU); blocking uses wait queues. Nothing that can sleep (file_put closes sockets, which takes the
 * network lock) is called with interrupts off: the code collects what has to be dropped and drops it afterwards.
 *
 * The Linux personality (kernel/linux/net.c) reaches the same code through unixsock.h: the names of the two ends (getsockname, getpeername, the address accept
 * stores), the flags of send and recv (MSG_PEEK, MSG_DONTWAIT, MSG_WAITALL, MSG_NOSIGNAL), shutdown of either direction, descriptors and credentials in a
 * message, and the options a program reads (SO_PEERCRED, SO_TYPE). Written from unix(7), socket(2), bind(2), connect(2), accept(2), shutdown(2), recv(2),
 * send(2), cmsg(3) and scm_rights; the checks are tests/linux/src/net_unix.c and net_unix2.c.
 */
#include "vfs.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "initcall.h"
#include "unixsock.h"
#include "pollq.h"

#define SOCK_Q      32768                       /* bytes per direction of a stream; power of two */
#define BACKLOG_MAX 32
#define DGRAM_BUDGET 262144                     /* bytes of datagrams a socket holds before senders wait */
#define DGRAM_MAX    130000                     /* the longest datagram */
#define DGRAM_OVERHEAD 64                       /* what a queued datagram costs besides its bytes, so that empty ones cannot fill memory */

/* Descriptors on their way to one side of a stream. pos is the stream position of the first byte they were sent with. */
typedef struct Anc {
    struct Anc *next;
    u32   pos;
    int   nfds;
    File **fds;
    int   pid;
    u32   uid, gid;
} Anc;

/* A datagram in a receive queue. */
typedef struct UMsg {
    struct UMsg *next;
    u32   len;
    u8    nlen;                                 /* the sender's name, if it had one */
    char  nm[UNIX_PATH_MAX];
    int   pid;
    u32   uid, gid;
    int   nfds;
    File **fds;
    u8    data[];
} UMsg;

typedef struct Conn {
    u8  *q[2];                                  /* q[s] is read by side s (written by side 1-s) */
    u32  r[2], w[2];                            /* free-running counters */
    bool closed[2];
    bool shut_wr[2];                            /* side s will send no more: the other side reads end of file once the queue is empty */
    bool shut_rd[2];                            /* side s will read no more: the other side's sends fail with EPIPE */
    int  refs;                                  /* ends still open */
    WaitQueue wq;
    int  pid[2];
    u32  uid[2], gid[2];
    u8   nlen[2];                               /* the names the two ends had when the connection was made (what getsockname and getpeername show) */
    char nm[2][UNIX_PATH_MAX];
    Anc *anc[2];                                /* anc[s]: descriptors on their way to side s, in stream order */
} Conn;

typedef enum { SS_NEW, SS_LISTEN, SS_CONN } SState;

struct Sock;

typedef struct AbsName {                        /* a socket in the abstract namespace */
    struct AbsName *next;
    struct Sock *owner;
    u8   nlen;
    char nm[UNIX_PATH_MAX];
} AbsName;

typedef struct Sock {
    int    type;                                /* SOCK_STREAM or SOCK_DGRAM */
    int    refs;                                /* the File's reference, and one for each datagram socket whose destination this is (and for a sender that waits) */
    bool   dead;                                /* the File is closed; the structure stays only for those that still point at it */
    SState state;
    Conn  *c;
    int    side;
    Conn  *pending[BACKLOG_MAX];
    int    npending, backlog;
    WaitQueue accept_wq;                        /* a listener's accept queue; a datagram socket's receive queue (readers wait for a message, senders for room) */
    VNode *node;                                /* the bound socket file */
    bool   bound;
    int    lpid;                                /* who called listen(): what a client's getpeercred() says until the server accepts */
    u32    luid, lgid;
    int    cpid;                                /* who made the socket (datagram sockets report this as the credentials of their peer) */
    u32    cuid, cgid;
    u8     nlen;                                /* the name bind() gave this socket: a path with its NUL, or an abstract name with its leading NUL; 0 = none */
    char   nm[UNIX_PATH_MAX];
    AbsName *abs;                               /* its entry in the abstract table, if it is bound there */
    u32    opts;                                /* the on/off options a program set (bit n for SO_n), so that getsockopt answers what was set */
    UMsg  *rq, *rq_tail;                        /* datagram sockets: messages to read */
    u32    rq_bytes;
    struct Sock *peer;                          /* datagram sockets: where plain sends go (connect, socketpair); holds a reference */
    bool   rd_shut;                             /* datagram sockets: shutdown(SHUT_RD) */
} Sock;

extern void (*g_signal_self_hook)(int sig);

/* kernel/net/sockcall.c */
extern const FileOps g_inet_fops;
long inet_socket(int type, int protocol);
long inet_bind(File *f, u64 uaddr, u64 len);
long inet_listen(File *f, int backlog);
long inet_accept(File *f, u64 uaddr, u64 ulenp);
long inet_connect(File *f, u64 uaddr, u64 len);
long inet_shutdown(File *f, int how);

static int g_nunix;                             /* AF_UNIX sockets alive (statistics) */
int unix_sock_count(void) { return g_nunix; }

static AbsName *g_abs;                          /* the abstract namespace; guarded by irq_save */

static const VNodeOps sock_node_ops;            /* identifies socket files; no data operations */
static const FileOps sock_fops;

#define SO_PASSCRED_BIT 16

static inline bool linux_caller(void) { return current->proc && current->proc->abi == ABI_LINUX; }

bool unix_is_sock(const File *f) { return f && f->fops == &sock_fops; }
bool unix_is_dgram(const File *f) { return f && f->fops == &sock_fops && ((Sock *)f->priv)->type == SOCK_DGRAM; }

/* ---------------------------------------------------------------- objects */

static Sock *sock_alloc(int type) {
    Sock *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->type = type;
    s->refs = 1;
    wq_init(&s->accept_wq);
    s->cpid = current->proc ? current->proc->pid : 0;
    s->cuid = current->proc ? current->proc->cred.uid : 0;
    s->cgid = current->proc ? current->proc->cred.gid : 0;
    u64 fl = irq_save();
    g_nunix++;
    irq_restore(fl);
    return s;
}

/* Drops a reference; the last one frees the socket. Any context. */
static void sock_unref(Sock *s) {
    u64 fl = irq_save();
    int left = --s->refs;
    if (left == 0) g_nunix--;
    irq_restore(fl);
    if (left == 0) kfree(s);
}

/* Descriptors carried by a message or an Anc entry that nobody is going to receive: let them go (IRQs on). */
static void files_drop(File **fds, int n) {
    for (int i = 0; i < n; i++) file_put(fds[i]);
    kfree(fds);
}

static void anc_drop_list(Anc *a) {
    while (a) {
        Anc *next = a->next;
        files_drop(a->fds, a->nfds);
        kfree(a);
        a = next;
    }
}

static void msg_drop(UMsg *m) {
    files_drop(m->fds, m->nfds);
    kfree(m);
}

static void msg_drop_list(UMsg *m) {
    while (m) {
        UMsg *next = m->next;
        msg_drop(m);
        m = next;
    }
}

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

/* IRQs disabled by the caller. The last reference frees the connection; descriptors still queued on it are put on *drop, for the caller to drop with IRQs on. */
static void conn_unref(Conn *c, Anc **drop) {
    if (--c->refs > 0) return;
    for (int s = 0; s < 2; s++) {
        Anc *a = c->anc[s];
        while (a) {
            Anc *next = a->next;
            a->next = *drop;
            *drop = a;
            a = next;
        }
    }
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

/* The socket behind descriptor fd, with a reference on its File (file_put it): another thread may close the descriptor while the
 * call sleeps. -EBADF for no such descriptor, -ENOTSOCK for a file that is not a socket. */
static long socket_ref(u64 fd, File **out) {
    File *f = fd_get_ref(current->proc, (int)fd);
    if (!f) return -EBADF;
    if (f->fops != &sock_fops && f->fops != &g_inet_fops) { file_put(f); return -ENOTSOCK; }
    *out = f;
    return 0;
}

/* ---------------------------------------------------------------- names */

/* A sockaddr_un as a program passed it. The length decides where an abstract name ends (it may hold NUL bytes); a path ends at its NUL. */
typedef struct UName {
    bool abstract;
    u8   len;                                    /* bytes in b that are the name: a path counts its NUL, an abstract name its leading NUL */
    char b[UNIX_PATH_MAX + 1];
} UName;

/* unix(7): the length must be more than the family field and at most the size of the whole structure.
 *
 * A native program gets what the native calls always gave: the path is the string at the end of the family field, up to its NUL, whatever length the program
 * passed (and whatever family: nothing looked at either), and there are no abstract names. The system call fuzzer depends on that (docs/HARDENING.md): it passes
 * lengths of a few bytes with a full path, and a name cut to that length would put socket files into /tmp that nothing sweeps away. */
static long uname_from_user(u64 uaddr, u64 ulen, UName *u) {
    if (!linux_caller()) {
        memset(u, 0, sizeof *u);
        int n = strncpy_from_user(u->b, (const char *)uaddr + sizeof(u16), sizeof u->b);
        if (n < 0) return n;
        u->len = (u8)(n + 1);
        return 0;
    }
    if (ulen <= sizeof(u16) || ulen > sizeof(u16) + UNIX_PATH_MAX) return -EINVAL;
    u16 fam;
    if (copy_from_user(&fam, (const void *)uaddr, sizeof fam)) return -EFAULT;
    if (fam != AF_UNIX) return -EAFNOSUPPORT;
    size_t n = (size_t)ulen - sizeof(u16);
    memset(u, 0, sizeof *u);
    if (copy_from_user(u->b, (const char *)uaddr + sizeof(u16), n)) return -EFAULT;
    if (u->b[0] == 0) {
        u->abstract = true;
        u->len = (u8)n;
    } else {
        size_t l = 0;
        while (l < n && u->b[l]) l++;
        u->b[l] = 0;
        u->len = (u8)(l + 1);
    }
    return 0;
}

static AbsName *abs_find(const UName *u) {       /* IRQs disabled by the caller */
    for (AbsName *a = g_abs; a; a = a->next)
        if (a->nlen == u->len && !memcmp(a->nm, u->b, u->len)) return a;
    return NULL;
}

/* The file system half of finding the socket a name refers to (process context): for a path, the vnode of the socket file, with the checks connect(2) makes
 * (it exists, it is a socket, the caller may write it). Linux says "no such file" for a path that is not there; the native calls always said "refused". An
 * abstract name needs no lookup (*vn stays NULL). */
static long name_vnode(const UName *un, VNode **vn) {
    *vn = NULL;
    if (un->abstract) return 0;
    Process *p = current->proc;
    VNode *v;
    long rc = vfs_lookup(un->b, p->cwd, &p->cred, 0, &v);
    if (rc) return (rc == -ENOENT && !linux_caller()) ? -ECONNREFUSED : rc;
    if (v->ops != &sock_node_ops) { vnode_put(v); return -ECONNREFUSED; }
    rc = vfs_access(v, &p->cred, W_OK);
    if (rc) { vnode_put(v); return rc; }
    *vn = v;
    return 0;
}

/* The socket behind a name (IRQs off): the owner of the socket file, or the entry in the abstract table. NULL if there is none (any more). */
static Sock *name_sock(const UName *un, VNode *vn) {
    if (un->abstract) {
        AbsName *a = abs_find(un);
        return a ? a->owner : NULL;
    }
    return vn ? vn->priv : NULL;
}

/* Stores a sockaddr_un made of the family and the name at uaddr, getsockname(2) style: at most *ulenp bytes, and *ulenp becomes the full length. */
long unix_store_name(u64 uaddr, u64 ulenp, const char *nm, u8 nlen) {
    u32 have;
    if (!ulenp || copy_from_user(&have, (const void *)ulenp, sizeof have)) return -EFAULT;
    struct { u16 family; char path[UNIX_PATH_MAX]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.family = AF_UNIX;
    memcpy(sa.path, nm, nlen);
    u32 full = (u32)sizeof(u16) + nlen, n = MIN(have, full);
    if (n && uaddr && copy_to_user((void *)uaddr, &sa, n)) return -EFAULT;
    if (copy_to_user((void *)ulenp, &full, sizeof full)) return -EFAULT;
    return 0;
}

long unix_getname(File *f, bool peer, u64 uaddr, u64 ulenp) {
    Sock *s = f->priv;
    char nm[UNIX_PATH_MAX];
    u8 nlen = 0;
    u64 fl = irq_save();
    if (s->type == SOCK_DGRAM) {
        Sock *src = peer ? s->peer : s;
        if (!src || src->dead) { irq_restore(fl); return peer ? -ENOTCONN : 0; }
        nlen = src->nlen;
        memcpy(nm, src->nm, nlen);
    } else {
        if (peer && s->state != SS_CONN) { irq_restore(fl); return -ENOTCONN; }
        if (s->state == SS_CONN) {
            int side = peer ? 1 - s->side : s->side;
            nlen = s->c->nlen[side];
            memcpy(nm, s->c->nm[side], nlen);
        } else {
            nlen = s->nlen;
            memcpy(nm, s->nm, nlen);
        }
    }
    irq_restore(fl);
    return unix_store_name(uaddr, ulenp, nm, nlen);
}

/* ---------------------------------------------------------------- streams */

/* How many bytes may be read from r0 on before the next group of descriptors starts (the position it was sent at). IRQs off. skip_here: a group that starts exactly at
 * r0 is not an obstacle (it is the one this call takes, or the one a peek leaves alone). */
static u32 stream_room(Conn *c, int me, u32 r0, bool skip_here) {
    u32 room = avail(c, me);
    for (Anc *a = c->anc[me]; a; a = a->next) {
        u32 d = a->pos - r0;
        if (d == 0 && skip_here) { skip_here = false; continue; }
        if (d < room) room = d;
        break;
    }
    return room;
}

static ssize_t stream_recv(File *f, void *buf, size_t n, int mflags, UnixCtl *ctl) {
    Sock *s = f->priv;
    if (s->state != SS_CONN) return -ENOTCONN;
    if (!n) return 0;
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    bool peek = (mflags & MSG_PEEK) != 0;
    Conn *c = s->c;
    int me = s->side, peer = 1 - me;
    u8 *out = buf;
    size_t got = 0;
    ssize_t rc = 0;
    Anc *take = NULL;
    u64 fl = irq_save();
    for (;;) {
        u32 r0 = c->r[me];
        if (!peek && !got && !take && c->anc[me] && c->anc[me]->pos == r0) {                  /* descriptors were sent with the byte we are about to read */
            take = c->anc[me];
            c->anc[me] = take->next;
            take->next = NULL;
        }
        u32 room = stream_room(c, me, r0, peek);
        if (peek) {
            if (room) {
                size_t k = MIN((size_t)room, n);
                for (size_t i = 0; i < k; i++) out[i] = c->q[me][(r0 + (u32)i) & (SOCK_Q - 1)];
                rc = (ssize_t)k;
                break;
            }
        } else {
            while (got < n && room) {
                out[got++] = c->q[me][c->r[me]++ & (SOCK_Q - 1)];
                room--;
            }
            if (got) wq_wake_all(&c->wq);                                                   /* the writer may have been waiting for room */
            if (got && (!(mflags & MSG_WAITALL) || got >= n)) break;
            if (got && c->anc[me] && c->anc[me]->pos == c->r[me]) break;                     /* the next bytes come with descriptors: another call */
            if (got && avail(c, me)) continue;
        }
        if (c->shut_rd[me] || c->closed[peer] || c->shut_wr[peer]) {                         /* end of file once what is queued has been read */
            if (!avail(c, me)) break;
        }
        if (nb) { if (!got && !rc) rc = -EAGAIN; break; }
        if (signal_pending()) { if (!got && !rc) rc = -EINTR; break; }
        current->intr = true;
        wq_block(&c->wq);
        current->intr = false;
    }
    bool pass = s->opts & (1u << SO_PASSCRED_BIT);
    int ppid = c->pid[peer];
    u32 puid = c->uid[peer], pgid = c->gid[peer];
    irq_restore(fl);
    if (take) {
        if (ctl) {
            ctl->nfds = take->nfds;                                                          /* the caller learns that there were some, even if it did not ask for them */
            ctl->pid = take->pid;
            ctl->uid = take->uid;
            ctl->gid = take->gid;
            if (ctl->want_fds) {
                ctl->fds = take->fds;
                take->fds = NULL;
                take->nfds = 0;
            }
        }
        anc_drop_list(take);                                                                 /* frees the entry, and the descriptors if the caller did not want them */
    }
    if (ctl) {
        ctl->dgram = false;
        ctl->passcred = pass;
        if (pass && (got || rc > 0)) { ctl->have_cred = true; ctl->pid = ppid; ctl->uid = puid; ctl->gid = pgid; }
    }
    return rc ? rc : (ssize_t)got;
}

/* send on a stream: blocks until everything is queued (or a signal comes). A broken connection is EPIPE, and SIGPIPE too unless the call said MSG_NOSIGNAL; if
 * some bytes went out first, they are the result. The descriptors (fds, handed over) are queued with the first byte. */
static ssize_t stream_send(File *f, const void *buf, size_t n, int mflags, File **fds, int nfds) {
    Sock *s = f->priv;
    if (s->state != SS_CONN) { if (nfds) files_drop(fds, nfds); return -ENOTCONN; }
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    Conn *c = s->c;
    int me = s->side, peer = 1 - me;
    const u8 *in = buf;
    size_t done = 0;
    Anc *anc = NULL;
    bool queued = false;
    if (!n && nfds) {                                       /* a send without data carries nothing on a stream, descriptors included (the same on Linux) */
        files_drop(fds, nfds);
        fds = NULL;
        nfds = 0;
    }
    if (nfds) {
        anc = kzalloc(sizeof *anc);
        if (!anc) { files_drop(fds, nfds); return -ENOMEM; }
        anc->fds = fds;
        anc->nfds = nfds;
        anc->pid = current->proc ? current->proc->pid : 0;
        anc->uid = current->proc ? current->proc->cred.uid : 0;
        anc->gid = current->proc ? current->proc->cred.gid : 0;
    }
    ssize_t rc = 0;
    bool sigpipe = false;
    u64 fl = irq_save();
    while (done < n || (anc && !queued)) {
        if (c->closed[peer] || c->shut_wr[me] || c->shut_rd[peer]) {
            sigpipe = !done;
            rc = done ? (ssize_t)done : -EPIPE;
            break;
        }
        if (avail(c, peer) >= SOCK_Q && done < n) {
            if (nb) { rc = done ? (ssize_t)done : -EAGAIN; break; }
            if (signal_pending()) { rc = done ? (ssize_t)done : -EINTR; break; }
            wq_wake_all(&c->wq);
            current->intr = true;
            wq_block(&c->wq);
            current->intr = false;
            continue;
        }
        if (anc && !queued) {                                                                /* the descriptors start where the first byte of this send goes */
            anc->pos = c->w[peer];
            Anc **pp = &c->anc[peer];
            while (*pp) pp = &(*pp)->next;
            *pp = anc;
            queued = true;
        }
        while (done < n && avail(c, peer) < SOCK_Q) c->q[peer][c->w[peer]++ & (SOCK_Q - 1)] = in[done++];
        wq_wake_all(&c->wq);
        rc = (ssize_t)done;
    }
    irq_restore(fl);
    if (anc && !queued) anc_drop_list(anc);
    if (sigpipe && !(mflags & MSG_NOSIGNAL) && g_signal_self_hook) g_signal_self_hook(SIGPIPE);
    return rc;
}

/* ---------------------------------------------------------------- datagrams */

/* Looks the destination up, makes a message of the bytes (and takes over the descriptors) and adds it to the destination's queue. `to` is a name in user memory
 * (0: the socket's default destination). */
static ssize_t dgram_send(File *f, const void *buf, size_t n, int mflags, u64 uto, u64 utolen, File **fds, int nfds) {
    Sock *s = f->priv;
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    UName un;
    VNode *vn = NULL;
    ssize_t rc = 0;
    UMsg *m = NULL;
    if (n > DGRAM_MAX) { rc = -EMSGSIZE; goto fail; }
    if (uto) {
        rc = uname_from_user(uto, utolen, &un);
        if (!rc) rc = name_vnode(&un, &vn);
        if (rc) goto fail;
    } else {
        u64 fl = irq_save();
        bool have = s->peer != NULL;
        irq_restore(fl);
        if (!have) { rc = -ENOTCONN; goto fail; }
    }
    m = kmalloc(sizeof *m + n);
    if (!m) { rc = -ENOMEM; goto fail; }
    memset(m, 0, sizeof *m);
    m->len = (u32)n;
    memcpy(m->data, buf, n);
    m->fds = fds;
    m->nfds = nfds;
    m->pid = current->proc ? current->proc->pid : 0;
    m->uid = current->proc ? current->proc->cred.uid : 0;
    m->gid = current->proc ? current->proc->cred.gid : 0;

    u64 fl = irq_save();
    m->nlen = s->nlen;
    memcpy(m->nm, s->nm, s->nlen);
    Sock *dst = uto ? name_sock(&un, vn) : s->peer;
    if (!dst || dst->dead || dst->type != SOCK_DGRAM) {
        irq_restore(fl);
        rc = -ECONNREFUSED;
        goto fail;
    }
    dst->refs++;                                                                             /* it must not go away while we wait for room in it */
    while (dst->rq_bytes + m->len + DGRAM_OVERHEAD > DGRAM_BUDGET && dst->rq) {
        if (nb) { rc = -EAGAIN; break; }
        if (signal_pending()) { rc = -EINTR; break; }
        current->intr = true;
        wq_block(&dst->accept_wq);
        current->intr = false;
        if (dst->dead) { rc = -ECONNREFUSED; break; }
    }
    if (!rc) {
        if (dst->dead) {
            rc = -ECONNREFUSED;
        } else if (dst->rd_shut) {
            rc = (ssize_t)n;                                                                 /* a receiver that shut down reading: the datagram is dropped without a word */
        } else {
            if (dst->rq_tail) dst->rq_tail->next = m; else dst->rq = m;
            dst->rq_tail = m;
            dst->rq_bytes += m->len + DGRAM_OVERHEAD;
            wq_wake_all(&dst->accept_wq);
            rc = (ssize_t)n;
            m = NULL;                                                                        /* the queue owns it now; so do its descriptors */
        }
    }
    irq_restore(fl);
    sock_unref(dst);
    if (m) msg_drop(m);                                                                      /* not queued: the descriptors go with it */
    if (vn) vnode_put(vn);
    return rc;

fail:
    if (m) msg_drop(m);
    else if (fds) files_drop(fds, nfds);
    if (vn) vnode_put(vn);
    return rc;
}

static ssize_t dgram_recv(File *f, void *buf, size_t n, int mflags, UnixCtl *ctl) {
    Sock *s = f->priv;
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    bool peek = (mflags & MSG_PEEK) != 0;
    u64 fl = irq_save();
    UMsg *m;
    for (;;) {
        m = s->rq;
        if (m) break;
        if (s->rd_shut) { irq_restore(fl); return 0; }
        if (nb) { irq_restore(fl); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); return -EINTR; }
        current->intr = true;
        wq_block(&s->accept_wq);
        current->intr = false;
    }
    size_t k = MIN(n, (size_t)m->len);
    memcpy(buf, m->data, k);
    bool pass = s->opts & (1u << SO_PASSCRED_BIT);
    if (ctl) {
        ctl->dgram = true;
        ctl->full = m->len;
        ctl->fromlen = m->nlen;
        memcpy(ctl->from, m->nm, m->nlen);
        ctl->passcred = pass;
        if (pass) { ctl->have_cred = true; ctl->pid = m->pid; ctl->uid = m->uid; ctl->gid = m->gid; }
    }
    if (!peek) {
        s->rq = m->next;
        if (!s->rq) s->rq_tail = NULL;
        s->rq_bytes -= MIN(s->rq_bytes, m->len + DGRAM_OVERHEAD);
        if (ctl) {
            ctl->nfds = m->nfds;
            if (ctl->want_fds) { ctl->fds = m->fds; m->fds = NULL; m->nfds = 0; }
        }
        wq_wake_all(&s->accept_wq);                                                          /* senders may have been waiting for room */
    }
    irq_restore(fl);
    if (!peek) msg_drop(m);
    return (ssize_t)k;
}

/* ---------------------------------------------------------------- the public face of the data path */

ssize_t unix_send_msg(File *f, const void *buf, size_t n, int mflags, u64 to, u64 tolen, File **fds, int nfds) {
    Sock *s = f->priv;
    if (s->type == SOCK_DGRAM) return dgram_send(f, buf, n, mflags, to, tolen, fds, nfds);
    if (to) {
        if (nfds) files_drop(fds, nfds);
        return s->state == SS_CONN ? -EISCONN : -EOPNOTSUPP;
    }
    return stream_send(f, buf, n, mflags, fds, nfds);
}

ssize_t unix_recv_msg(File *f, void *buf, size_t n, int mflags, UnixCtl *ctl) {
    Sock *s = f->priv;
    if (ctl) {
        bool want = ctl->want_fds;
        memset(ctl, 0, sizeof *ctl);
        ctl->want_fds = want;
    }
    return s->type == SOCK_DGRAM ? dgram_recv(f, buf, n, mflags, ctl) : stream_recv(f, buf, n, mflags, ctl);
}

ssize_t unix_send(File *f, const void *buf, size_t n, int mflags) { return unix_send_msg(f, buf, n, mflags, 0, 0, NULL, 0); }
ssize_t unix_recv(File *f, void *buf, size_t n, int mflags) { return unix_recv_msg(f, buf, n, mflags, NULL); }

static ssize_t sock_read(File *f, void *buf, size_t n) { return unix_recv(f, buf, n, 0); }
static ssize_t sock_write(File *f, const void *buf, size_t n) { return unix_send(f, buf, n, 0); }

static int sock_poll(File *f) {
    Sock *s = f->priv;
    int m = 0;
    bool lx = linux_caller();
    u64 fl = irq_save();
    if (s->type == SOCK_DGRAM) {
        if (s->rq) m |= POLLIN;
        if (s->rd_shut) m |= POLLIN | POLLRDHUP;
        if (!s->peer || s->peer->dead || s->peer->rq_bytes + DGRAM_OVERHEAD < DGRAM_BUDGET) m |= POLLOUT;
    } else if (s->state == SS_LISTEN) {
        if (s->npending) m |= POLLIN;
    } else if (s->state == SS_CONN) {
        Conn *c = s->c;
        int me = s->side, peer = 1 - me;
        if (avail(c, me)) m |= POLLIN;
        if (lx) {
            /* unix_poll: reading is shut down when we did it or the peer sent its last byte (or went away); sending when we did it or the peer stopped reading;
             * both at once is a hang-up. A socket is writable as long as the queue toward the peer has room, even if the peer has gone: the send then fails. */
            bool rcv_sd = c->shut_rd[me] || c->shut_wr[peer] || c->closed[peer];
            bool snd_sd = c->shut_wr[me] || c->shut_rd[peer] || c->closed[peer];
            if (rcv_sd) m |= POLLIN | POLLRDHUP;
            if (rcv_sd && snd_sd) m |= POLLHUP;
            if (avail(c, peer) < SOCK_Q) m |= POLLOUT;
        } else {
            if (c->closed[peer] || c->shut_wr[peer]) m |= POLLIN | POLLHUP | POLLRDHUP;
            if (!c->closed[peer] && avail(c, peer) < SOCK_Q) m |= POLLOUT;
        }
    } else if (lx) {
        m |= POLLOUT | POLLHUP;                                                            /* a stream socket that is not connected (unix_poll) */
    }
    irq_restore(fl);
    return m;
}

static int sock_ioctl(File *f, unsigned long req, void *arg) {
    Sock *s = f->priv;
    if (req == FIONREAD) {
        if (s->type == SOCK_DGRAM) {                                                        /* the length of the next datagram */
            u64 fl = irq_save();
            *(int *)arg = s->rq ? (int)s->rq->len : 0;
            irq_restore(fl);
            return 0;
        }
        if (s->state != SS_CONN) return -ENOTCONN;
        *(int *)arg = (int)avail(s->c, s->side);
        return 0;
    }
    if (req == SOCK_PEERCRED) {
        struct gpeercred *pc = arg;
        if (s->type == SOCK_DGRAM) {
            if (!s->peer) return -ENOTCONN;
            pc->pid = s->peer->cpid;
            pc->uid = s->peer->cuid;
            pc->gid = s->peer->cgid;
            return 0;
        }
        if (s->state != SS_CONN) return -ENOTCONN;
        pc->pid = s->c->pid[1 - s->side];
        pc->uid = s->c->uid[1 - s->side];
        pc->gid = s->c->gid[1 - s->side];
        return 0;
    }
    return -ENOTTY;
}

static void sock_close(File *f) {
    Sock *s = f->priv;
    Anc *drop = NULL;
    UMsg *queued = NULL;
    Sock *peer_sock = NULL;
    u64 fl = irq_save();
    s->dead = true;
    if (s->abs) {                                                                          /* the name is free again */
        for (AbsName **pp = &g_abs; *pp; pp = &(*pp)->next)
            if (*pp == s->abs) { *pp = s->abs->next; break; }
        kfree(s->abs);
        s->abs = NULL;
    }
    if (s->type == SOCK_DGRAM) {
        queued = s->rq;
        s->rq = s->rq_tail = NULL;
        s->rq_bytes = 0;
        peer_sock = s->peer;
        s->peer = NULL;
        wq_wake_all(&s->accept_wq);                                                        /* senders waiting for room in it must see that it is gone */
    } else if (s->state == SS_LISTEN) {
        for (int i = 0; i < s->npending; i++) {              /* nobody will ever accept these */
            Conn *c = s->pending[i];
            c->closed[1] = true;
            wq_wake_all(&c->wq);
            conn_unref(c, &drop);
        }
        s->npending = 0;
        wq_wake_all(&s->accept_wq);
    } else if (s->state == SS_CONN) {
        Conn *c = s->c;
        int me = s->side;
        c->closed[me] = true;
        kfree(c->q[me]);                                      /* the peer can no longer write toward us */
        c->q[me] = NULL;
        drop = c->anc[me];                                    /* descriptors on their way to us never arrive */
        c->anc[me] = NULL;
        wq_wake_all(&c->wq);
        if (c->refs == 1) { kfree(c->q[1 - me]); c->q[1 - me] = NULL; }
        conn_unref(c, &drop);
    }
    VNode *node = s->node;
    if (node) node->priv = NULL;                              /* the socket file stays in the directory, but nothing is behind it any more */
    irq_restore(fl);
    if (node) vnode_put(node);
    anc_drop_list(drop);
    msg_drop_list(queued);
    if (peer_sock) sock_unref(peer_sock);
    sock_unref(s);
}

/* A connected socket's readiness changes with its Conn (data, room, the peer's shutdown or close: one wait queue for both ends); a socket that is new or listening
 * is woken through its own accept queue, which connect() and listen() wake too when the socket changes state, so that a watcher moves to the right queue. A
 * datagram socket is woken by its own queue when a message arrives; room appearing in the queue it sends to is nobody's wake-up for it, so while that queue is
 * nearly full it asks to be looked at every few milliseconds instead. */
static WaitQueue *sock_pollwq(File *f) {
    Sock *s = f->priv;
    if (s->type == SOCK_DGRAM) {
        u64 fl = irq_save();
        Sock *p = s->peer;
        bool full = p && !p->dead && p->rq_bytes + DGRAM_OVERHEAD >= DGRAM_BUDGET;
        irq_restore(fl);
        return full ? NULL : &s->accept_wq;
    }
    return s->state == SS_CONN ? &s->c->wq : &s->accept_wq;
}

static const FileOps sock_fops = { .read = sock_read, .write = sock_write, .ioctl = sock_ioctl, .close = sock_close, .poll = sock_poll, .pollwq = sock_pollwq };
static const VNodeOps sock_node_ops = { 0 };

/* ---------------------------------------------------------------- options */

/* The SOL_SOCKET options a program may set on a UNIX socket and then read back, as bits of Sock.opts; the sizes and timeouts are accepted and not kept. */
#define OPTS_KEPT ((1u << SO_REUSEADDR) | (1u << SO_BROADCAST) | (1u << SO_KEEPALIVE) | (1u << 10) | (1u << 5) | (1u << SO_REUSEPORT) | (1u << SO_PASSCRED_BIT))     /* + OOBINLINE, DONTROUTE, PASSCRED */

long unix_setsockopt(File *f, int level, int name, const u8 *val, size_t len) {
    Sock *s = f->priv;
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    int iv = 0;
    memcpy(&iv, val, MIN(len, sizeof iv));
    if (name >= 0 && name < 32 && (OPTS_KEPT & (1u << name))) {
        u64 fl = irq_save();
        if (iv) s->opts |= 1u << name; else s->opts &= ~(1u << name);
        irq_restore(fl);
        return 0;
    }
    switch (name) {
    case SO_SNDBUF: case SO_RCVBUF: case 32: case 33: /* SNDBUFFORCE, RCVBUFFORCE */
    case SO_LINGER: case 12: /* PRIORITY */ case 18: case 19: /* RCVLOWAT, SNDLOWAT */
    case SO_RCVTIMEO: case SO_SNDTIMEO:
        return 0;                                            /* accepted; the queues have a fixed size and a UNIX socket does not time out */
    default:
        return -ENOPROTOOPT;
    }
}

long unix_getsockopt(File *f, int level, int name, u8 out[16], size_t *olen) {
    Sock *s = f->priv;
    int iv = 0;
    *olen = 4;
    memset(out, 0, 16);
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (name >= 0 && name < 32 && (OPTS_KEPT & (1u << name))) {
        iv = (s->opts >> name) & 1;
    } else {
        switch (name) {
        case SO_TYPE: iv = s->type; break;
        case SO_ERROR: iv = 0; break;
        case SO_ACCEPTCONN: iv = s->state == SS_LISTEN; break;
        case SO_SNDBUF: case SO_RCVBUF: iv = 212992; break;                                 /* Linux's default; ours are 32 KiB queues */
        case 18: case 19: iv = 1; break;                                                    /* RCVLOWAT, SNDLOWAT */
        case 12: case 38: iv = 0; break;                                                    /* PRIORITY, PROTOCOL */
        case 39: iv = AF_UNIX; break;                                                       /* DOMAIN */
        case SO_LINGER: *olen = 8; break;                                                   /* off, zero seconds */
        case SO_RCVTIMEO: case SO_SNDTIMEO: *olen = sizeof(struct gtimeval); break;         /* no timeout */
        case 17: {                                                                          /* SO_PEERCRED: pid, uid, gid of the process at the other end */
            struct gpeercred pc = { 0, (uint32_t)-1, (uint32_t)-1 };                        /* not connected: nobody (Linux says pid 0 and the overflow ids) */
            u64 fl = irq_save();
            if (s->type == SOCK_DGRAM) {
                if (s->peer) { pc.pid = s->peer->cpid; pc.uid = s->peer->cuid; pc.gid = s->peer->cgid; }
            } else if (s->state == SS_CONN) {
                pc.pid = s->c->pid[1 - s->side];
                pc.uid = s->c->uid[1 - s->side];
                pc.gid = s->c->gid[1 - s->side];
            }
            irq_restore(fl);
            memcpy(out, &pc, sizeof pc);
            *olen = sizeof pc;
            return 0;
        }
        default: return -ENOPROTOOPT;
        }
    }
    if (*olen == 4) memcpy(out, &iv, 4);
    return 0;
}

/* ---------------------------------------------------------------- syscalls */

static long fd_for(File *f, int type) {
    long fd = fd_install(current->proc, f, 0, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_socket(TrapFrame *tf) {
    int type = (int)A1(tf);
    if (A0(tf) == AF_INET) return inet_socket(type, (int)A2(tf));
    if (A0(tf) != AF_UNIX) return -EAFNOSUPPORT;
    int t = type & 0xFF;
    if (t != SOCK_STREAM && t != SOCK_DGRAM) return -EPROTONOSUPPORT;
    Sock *s = sock_alloc(t);
    if (!s) return -ENOMEM;
    File *f = sock_file(s, (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    if (!f) { sock_unref(s); return -ENOMEM; }
    return fd_for(f, type);                                  /* closing the file frees the socket */
}

static void fill_cred(Conn *c, int side) {
    c->pid[side] = current->proc ? current->proc->pid : 0;
    c->uid[side] = current->proc ? current->proc->cred.uid : 0;
    c->gid[side] = current->proc ? current->proc->cred.gid : 0;
}

static long sys_socketpair(TrapFrame *tf) {
    int type = (int)A0(tf);
    int t = type & 0xFF;
    if (t != SOCK_STREAM && t != SOCK_DGRAM) return -EPROTONOSUPPORT;
    Conn *c = t == SOCK_STREAM ? conn_new() : NULL;
    Sock *a = sock_alloc(t), *b = sock_alloc(t);
    if ((t == SOCK_STREAM && !c) || !a || !b) {
        if (c) { kfree(c->q[0]); kfree(c->q[1]); kfree(c); }
        if (a) sock_unref(a);
        if (b) sock_unref(b);
        return -ENOMEM;
    }
    if (t == SOCK_STREAM) {
        a->state = b->state = SS_CONN;
        a->c = b->c = c;
        a->side = 0;
        b->side = 1;
        fill_cred(c, 0);
        fill_cred(c, 1);
    } else {                                                 /* two datagram sockets, each the default destination of the other */
        a->peer = b;
        b->peer = a;
        a->refs++;
        b->refs++;
    }
    File *fa = sock_file(a, type & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    File *fb = fa ? sock_file(b, type & SOCK_NONBLOCK ? O_NONBLOCK : 0) : NULL;
    if (!fa || !fb) {                                        /* closing a File releases its end; for an end that has no File do it by hand */
        if (fa) file_put(fa);
        if (t == SOCK_STREAM) {
            Anc *d = NULL;
            u64 fl = irq_save();
            if (!fa) conn_unref(c, &d);                      /* the end that never got a File: its share of the connection */
            conn_unref(c, &d);
            irq_restore(fl);
            anc_drop_list(d);
            if (!fa) sock_unref(a);
            sock_unref(b);
        } else if (!fa) {                                    /* no File at all: undo the two references each datagram socket holds on the other */
            a->peer = b->peer = NULL;
            sock_unref(a);
            sock_unref(a);
            sock_unref(b);
            sock_unref(b);
        } else {                                             /* closing a released its pointer to b; b still points at a */
            Sock *ap = b->peer;
            b->peer = NULL;
            sock_unref(ap);
            sock_unref(b);
        }
        return -ENOMEM;
    }
    int sv[2];
    sv[0] = (int)fd_for(fa, type);
    sv[1] = sv[0] >= 0 ? (int)fd_for(fb, type) : -EMFILE;
    if (sv[0] < 0 || sv[1] < 0) {
        if (sv[0] >= 0) fd_close(current->proc, sv[0]);
        else file_put(fb);
        return sv[0] < 0 ? sv[0] : sv[1];
    }
    if (copy_to_user((void *)A1(tf), sv, sizeof sv)) {
        fd_close(current->proc, sv[0]);
        fd_close(current->proc, sv[1]);
        return -EFAULT;
    }
    return 0;
}

static long sys_bind(TrapFrame *tf) {
    File *f;
    long rc = socket_ref(A0(tf), &f);
    if (rc) return rc;
    if (f->fops == &g_inet_fops) { rc = inet_bind(f, A1(tf), A2(tf)); file_put(f); return rc; }
    Sock *s = f->priv;
    if (s->state != SS_NEW || s->bound) { file_put(f); return -EINVAL; }
    UName un;
    rc = uname_from_user(A1(tf), A2(tf), &un);
    if (rc) { file_put(f); return rc; }
    if (un.abstract) {                                        /* a name in the table: no file, no directory to look at */
        AbsName *a = kzalloc(sizeof *a);
        if (!a) { file_put(f); return -ENOMEM; }
        a->owner = s;
        a->nlen = un.len;
        memcpy(a->nm, un.b, un.len);
        u64 fl = irq_save();
        if (abs_find(&un) || s->bound) {
            irq_restore(fl);
            kfree(a);
            file_put(f);
            return -EADDRINUSE;
        }
        a->next = g_abs;
        g_abs = a;
        s->abs = a;
        s->bound = true;
        s->nlen = un.len;
        memcpy(s->nm, un.b, un.len);
        irq_restore(fl);
        file_put(f);
        return 0;
    }
    Process *p = current->proc;
    VNode *dir;
    char name[GNAME_MAX + 1];
    rc = vfs_lookup_parent(un.b, p->cwd, &p->cred, &dir, name);
    if (rc) { file_put(f); return rc; }
    if (!dir->mount || (dir->mount->type != &g_tmpfs_type && dir->mount->type != &g_devfs_type)) { vnode_put(dir); file_put(f); return -ENOTSUP; }
    rc = vfs_access(dir, &p->cred, W_OK | X_OK);
    VNode *node = NULL;
    if (!rc) {
        rc = tmpfs_add_special(dir, name, S_IFSOCK | (0777 & ~p->umask), &sock_node_ops, s, 0, &node);
        if (rc == -EEXIST) rc = -EADDRINUSE;
    }
    vnode_put(dir);
    if (rc) { file_put(f); return rc; }
    node->uid = p->cred.uid;
    node->gid = p->cred.gid;
    s->node = node;                                           /* keep our reference until the socket closes */
    s->bound = true;
    s->nlen = un.len;
    memcpy(s->nm, un.b, un.len);
    file_put(f);
    return 0;
}

static long sys_listen(TrapFrame *tf) {
    File *f;
    long rc = socket_ref(A0(tf), &f);
    if (rc) return rc;
    if (f->fops == &g_inet_fops) { rc = inet_listen(f, (int)A1(tf)); file_put(f); return rc; }
    Sock *s = f->priv;
    if (s->type == SOCK_DGRAM) rc = -EOPNOTSUPP;
    else if (s->state == SS_CONN || !s->bound) rc = -EINVAL;
    else {
        int backlog = (int)A1(tf);
        s->backlog = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
        s->state = SS_LISTEN;
        s->lpid = current->proc ? current->proc->pid : 0;
        s->luid = current->proc ? current->proc->cred.uid : 0;
        s->lgid = current->proc ? current->proc->cred.gid : 0;
        wq_wake_all(&s->accept_wq);                           /* whoever watches a socket that has just become a listener must look again */
    }
    file_put(f);
    return rc;
}

/* connect() on a datagram socket only names the default destination (and can be repeated, or undone with an address family of AF_UNSPEC). */
static long dgram_connect(File *f, u64 uaddr, u64 ulen) {
    Sock *s = f->priv;
    u16 fam = AF_UNIX;
    if (ulen >= sizeof fam && copy_from_user(&fam, (const void *)uaddr, sizeof fam)) return -EFAULT;
    Sock *old = NULL;
    if (fam == 0) {                                           /* AF_UNSPEC: forget the destination */
        u64 fl = irq_save();
        old = s->peer;
        s->peer = NULL;
        irq_restore(fl);
        if (old) sock_unref(old);
        return 0;
    }
    UName un;
    VNode *vn;
    long rc = uname_from_user(uaddr, ulen, &un);
    if (!rc) rc = name_vnode(&un, &vn);
    if (rc) return rc;
    u64 fl = irq_save();
    Sock *dst = name_sock(&un, vn);
    if (!dst || dst->dead) rc = -ECONNREFUSED;
    else if (dst->type != SOCK_DGRAM) rc = -EPROTOTYPE;
    else {
        dst->refs++;
        old = s->peer;
        s->peer = dst;
    }
    irq_restore(fl);
    if (vn) vnode_put(vn);
    if (old) sock_unref(old);
    return rc;
}

static long sys_connect(TrapFrame *tf) {
    File *f;
    long rc = socket_ref(A0(tf), &f);
    if (rc) return rc;
    if (f->fops == &g_inet_fops) { rc = inet_connect(f, A1(tf), A2(tf)); file_put(f); return rc; }
    Sock *s = f->priv;
    if (s->type == SOCK_DGRAM) { rc = dgram_connect(f, A1(tf), A2(tf)); file_put(f); return rc; }
    if (s->state == SS_CONN) { file_put(f); return -EISCONN; }
    if (s->state != SS_NEW) { file_put(f); return -EINVAL; }
    UName un;
    rc = uname_from_user(A1(tf), A2(tf), &un);
    if (rc) { file_put(f); return rc; }
    VNode *v;
    rc = name_vnode(&un, &v);
    if (rc) { file_put(f); return rc; }
    Conn *c = conn_new();
    if (!c) { if (v) vnode_put(v); file_put(f); return -ENOMEM; }
    u64 fl = irq_save();
    Sock *l = name_sock(&un, v);
    if (!l || l->state != SS_LISTEN || l->npending >= l->backlog || s->state != SS_NEW) {
        bool raced = s->state != SS_NEW;                       /* another thread connected this socket meanwhile */
        irq_restore(fl);
        kfree(c->q[0]); kfree(c->q[1]); kfree(c);
        if (v) vnode_put(v);
        file_put(f);
        return raced ? -EISCONN : -ECONNREFUSED;
    }
    fill_cred(c, 0);
    c->pid[1] = l->lpid;                                      /* the client may ask who the server is before accept() ran: the listener */
    c->uid[1] = l->luid;
    c->gid[1] = l->lgid;
    c->nlen[0] = s->nlen;                                     /* the client's own name, if it bound one; the server end is known by the listener's name */
    memcpy(c->nm[0], s->nm, s->nlen);
    c->nlen[1] = l->nlen;
    memcpy(c->nm[1], l->nm, l->nlen);
    s->state = SS_CONN;
    s->c = c;
    s->side = 0;
    l->pending[l->npending++] = c;
    wq_wake_all(&l->accept_wq);
    wq_wake_all(&s->accept_wq);                               /* a watcher of this socket (attached while it was new) must look again */
    irq_restore(fl);
    if (v) vnode_put(v);
    file_put(f);
    return 0;
}

static long sys_accept(TrapFrame *tf) {
    File *f;
    long rc = socket_ref(A0(tf), &f);
    if (rc) return rc;
    if (f->fops == &g_inet_fops) { rc = inet_accept(f, A1(tf), A2(tf)); file_put(f); return rc; }
    Sock *l = f->priv;
    if (l->state != SS_LISTEN) { file_put(f); return l->type == SOCK_DGRAM ? -EOPNOTSUPP : -EINVAL; }
    u64 fl = irq_save();
    while (!l->npending) {
        if (f->flags & O_NONBLOCK) { irq_restore(fl); file_put(f); return -EAGAIN; }
        if (signal_pending()) { irq_restore(fl); file_put(f); return -EINTR; }
        current->intr = true;
        wq_block(&l->accept_wq);
        current->intr = false;
    }
    Conn *c = l->pending[0];
    memmove(l->pending, l->pending + 1, (size_t)(--l->npending) * sizeof(Conn *));
    fill_cred(c, 1);
    char peer_nm[UNIX_PATH_MAX];
    u8 peer_len = c->nlen[0];
    memcpy(peer_nm, c->nm[0], peer_len);
    irq_restore(fl);
    file_put(f);
    Sock *ns = sock_alloc(SOCK_STREAM);
    File *nf = ns ? sock_file(ns, 0) : NULL;
    if (!nf) {
        Anc *d = NULL;
        u64 f2 = irq_save();
        c->closed[1] = true;
        wq_wake_all(&c->wq);
        conn_unref(c, &d);
        irq_restore(f2);
        anc_drop_list(d);
        if (ns) sock_unref(ns);
        return -ENOMEM;
    }
    ns->state = SS_CONN;
    ns->c = c;
    ns->side = 1;
    long fd = fd_for(nf, 0);
    if (fd >= 0 && A1(tf) && A2(tf)) unix_store_name(A1(tf), A2(tf), peer_nm, peer_len);        /* the client's name (family only if it never bound one) */
    return fd;
}

static long sys_shutdown(TrapFrame *tf) {
    File *f;
    long rc = socket_ref(A0(tf), &f);
    if (rc) return rc;
    if (f->fops == &g_inet_fops) { rc = inet_shutdown(f, (int)A1(tf)); file_put(f); return rc; }
    Sock *s = f->priv;
    int how = (int)A1(tf);
    if (how < SHUT_RD || how > SHUT_RDWR) rc = -EINVAL;
    else if (s->type == SOCK_DGRAM) {
        u64 fl = irq_save();
        if (how == SHUT_RD || how == SHUT_RDWR) s->rd_shut = true;
        wq_wake_all(&s->accept_wq);
        irq_restore(fl);
    } else if (s->state != SS_CONN) rc = -ENOTCONN;
    else {
        u64 fl = irq_save();
        if (how == SHUT_WR || how == SHUT_RDWR) s->c->shut_wr[s->side] = true;
        if (how == SHUT_RD || how == SHUT_RDWR) s->c->shut_rd[s->side] = true;
        wq_wake_all(&s->c->wq);
        irq_restore(fl);
    }
    file_put(f);
    return rc;
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
