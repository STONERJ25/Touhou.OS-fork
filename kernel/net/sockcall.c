/*
 * The system call layer of the Internet sockets: socket/bind/listen/accept/connect/shutdown for AF_INET descriptors (called
 * from kernel/fs/socket.c), plus sendto, recvfrom, getsockname, getpeername, setsockopt, getsockopt and netctl.
 */
#include "netsock.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "initcall.h"
#include "random.h"
#include "unixsock.h"
#include "pollq.h"

#ifndef MSG_TRUNC
#define MSG_TRUNC 0x20
#endif

static inline bool is_inet(File *f) { return f && f->fops == &g_inet_fops; }

/* A file that is the end of a socket, Internet or UNIX. A file can have a socket for its vnode without being one: open() on the socket file of a bound UNIX socket
 * makes an ordinary File of the file system with nothing behind it, and the UNIX socket code would find no socket in it (the system call fuzzer found that out). */
static inline bool is_sock(File *f) { return f && (f->fops == &g_inet_fops || unix_is_sock(f)); }

static File *inet_file_new(Sock *s, int flags) {
    VNode *v = vnode_alloc(&g_inet_vn_ops, NULL, S_IFSOCK | 0600);
    if (!v) return NULL;
    File *f = file_new(v, O_RDWR | (flags & O_NONBLOCK), &g_inet_fops, s);
    if (!f) vnode_put(v);
    return f;
}

/* ------------------------------------------------------------------ socket, bind, listen, connect, accept, shutdown */

long inet_socket(int type, int protocol) {
    int t = type & 0xFF;
    int kind;
    if (t == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) kind = SK_TCP;
    else if (t == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) kind = SK_UDP;
    else if (t == SOCK_DGRAM && protocol == IPPROTO_ICMP) kind = SK_ICMP;
    else return t == SOCK_STREAM || t == SOCK_DGRAM ? -EPROTONOSUPPORT : -EPROTONOSUPPORT;
    net_lock();
    Sock *s = sock_new(kind);
    if (!s) { net_unlock(); return -ENOMEM; }
    s->has_file = true;
    s->uid = current->proc ? current->proc->cred.uid : 0;
    if (kind == SK_ICMP) s->icmp_ident = (u16)(random_u64() | 1);
    File *f = inet_file_new(s, (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
    if (!f) { sock_unlink_free(s); net_unlock(); return -ENOMEM; }
    net_unlock();
    long fd = fd_install(current->proc, f, 0, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) file_put(f);                                           /* closing the file frees the socket */
    return fd;
}

long inet_bind(File *f, u64 uaddr, u64 len) {
    Sock *s = f->priv;
    u32 addr;
    u16 port;
    int rc = sk_get_sockaddr(uaddr, len, &addr, &port);
    if (rc) return rc;
    net_lock();
    if (s->bound || (s->kind == SK_TCP && s->state != TCPS_CLOSED)) { net_unlock(); return -EINVAL; }
    if (s->kind == SK_ICMP) { net_unlock(); return -EOPNOTSUPP; }
    if (addr && !net_is_local(addr) && !(s->kind == SK_UDP && (addr == 0xFFFFFFFFu || (addr & 0xFF) == 0xFF))) { net_unlock(); return -EADDRNOTAVAIL; }
    if (port && port < 1024 && current->proc && current->proc->cred.uid != 0) { net_unlock(); return -EACCES; }
    if (!port) {
        port = sock_alloc_port(s->kind);
        if (!port) { net_unlock(); return -EADDRINUSE; }
    } else if (sock_port_conflict(s, addr, port)) {
        net_unlock();
        return -EADDRINUSE;
    }
    s->laddr = addr;
    s->lport = port;
    s->bound = true;
    net_unlock();
    return 0;
}

long inet_listen(File *f, int backlog) {
    Sock *s = f->priv;
    if (s->kind != SK_TCP) return -EOPNOTSUPP;
    net_lock();
    long rc = 0;
    if (s->state == TCPS_LISTEN) { s->backlog = backlog < 1 ? 1 : backlog > 128 ? 128 : backlog; }
    else if (s->state != TCPS_CLOSED) rc = -EINVAL;
    else {
        if (!s->bound) {
            u16 p = sock_alloc_port(SK_TCP);
            if (!p) rc = -EADDRINUSE; else { s->lport = p; s->bound = true; }
        }
        if (!rc) rc = tcp_listen(s, backlog);
    }
    net_unlock();
    net_kick();
    return rc;
}

long inet_accept(File *f, u64 uaddr, u64 ulenp) {
    Sock *l = f->priv;
    if (l->kind != SK_TCP) return -EOPNOTSUPP;
    bool nb = (f->flags & O_NONBLOCK) != 0;
    u64 deadline = 0;
    long rc = 0;
    Sock *c = NULL;
    file_get(f);
    net_lock();
    for (;;) {
        if (l->state != TCPS_LISTEN) { rc = -EINVAL; break; }
        if (l->naccept) { c = tcp_accept_dequeue(l); break; }
        if (nb) { rc = -EAGAIN; break; }
        int w = sk_block(l, l->rcvtimeo, &deadline);
        if (w && !l->naccept && l->state == TCPS_LISTEN) { rc = w; break; }
    }
    if (c) {
        c->has_file = true;
        File *nf = inet_file_new(c, 0);
        if (!nf) {
            c->has_file = false;
            tcp_close(c);
            rc = -ENOMEM;
        } else {
            u32 ra = c->raddr;
            u16 rp = c->rport;
            net_unlock();
            long fd = fd_install(current->proc, nf, 0, 0);
            if (fd < 0) file_put(nf);
            else sk_put_sockaddr(uaddr, ulenp, ra, rp);
            file_put(f);
            net_kick();
            return fd;
        }
    }
    net_unlock();
    file_put(f);
    return rc;
}

long inet_connect(File *f, u64 uaddr, u64 len) {
    Sock *s = f->priv;
    u32 addr;
    u16 port;
    int rc = sk_get_sockaddr(uaddr, len, &addr, &port);
    if (rc) return rc;
    bool nb = (f->flags & O_NONBLOCK) != 0;
    if (s->kind == SK_UDP || s->kind == SK_ICMP) {
        net_lock();
        if (s->kind == SK_UDP && !port && addr) { net_unlock(); return -EINVAL; }
        if (!addr) addr = INADDR_LOOPBACK;
        if (s->kind == SK_UDP && !s->bound) {
            u16 p = sock_alloc_port(SK_UDP);
            if (!p) { net_unlock(); return -EADDRINUSE; }
            s->lport = p;
            s->bound = true;
        }
        if (!ip_source_for(addr)) { net_unlock(); return -ENETUNREACH; }
        s->raddr = addr;
        s->rport = port;
        s->connected = true;
        s->err = 0;
        net_unlock();
        return 0;
    }
    if (!addr) addr = INADDR_LOOPBACK;                                  /* connecting to 0.0.0.0 means "this host" */
    u64 deadline = 0;
    long r = 0;
    file_get(f);
    net_lock();
    if (s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD) r = nb ? -EALREADY : 0;
    else if (s->state != TCPS_CLOSED) r = -EISCONN;
    else if (s->state == TCPS_LISTEN) r = -EOPNOTSUPP;
    else if (!port) r = -ECONNREFUSED;
    else r = tcp_connect(s, addr, port);
    if (r == 0 && nb && s->state == TCPS_SYN_SENT) r = -EINPROGRESS;
    if (r == 0) {
        while (s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD) {
            int w = sk_block(s, s->sndtimeo, &deadline);
            if (w == -EAGAIN) {                                          /* SO_SNDTIMEO ran out: give up on this attempt */
                r = -ETIMEDOUT;
                s->state = TCPS_CLOSED;
                s->t_rexmit = 0;
                s->raddr = 0;
                s->rport = 0;
                break;
            }
            if (w && (s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD)) { r = w; break; }
        }
        if (r == 0) {
            if (s->err) { r = -s->err; s->err = 0; }
            else if (s->state == TCPS_CLOSED) r = -ECONNREFUSED;
        }
    }
    net_unlock();
    net_kick();
    file_put(f);
    return r;
}

long inet_shutdown(File *f, int how) {
    Sock *s = f->priv;
    if (how < SHUT_RD || how > SHUT_RDWR) return -EINVAL;
    net_lock();
    long rc = 0;
    if (s->kind == SK_TCP) {
        if (s->state == TCPS_CLOSED || s->state == TCPS_LISTEN || s->state == TCPS_SYN_SENT) rc = -ENOTCONN;
        else tcp_shutdown(s, how);
    } else {
        if (how == SHUT_RD || how == SHUT_RDWR) s->rd_shut_user = true;
        if (how == SHUT_WR || how == SHUT_RDWR) s->wr_shut = true;
        sock_wake(s);
    }
    net_unlock();
    net_kick();
    return rc;
}

/* ------------------------------------------------------------------ sendto / recvfrom */

#define SEND_CHUNK 65536

/* send and recv on a UNIX-domain socket: the same system calls serve both families. A send goes out completely (or until an error or a signal); a receive is
 * one read, except that MSG_WAITALL asks for the whole length. */
static long unix_msg_io(File *f, bool is_send, u64 ubuf, u64 len, int mflags) {
    size_t chunk = MIN((size_t)len, (size_t)SEND_CHUNK);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    long total = 0, rc = 0;
    file_get(f);
    do {
        size_t take = MIN((size_t)len - (size_t)total, chunk);
        ssize_t w;
        if (is_send) {
            if (copy_from_user(k, (const u8 *)ubuf + total, take)) { rc = total ? total : -EFAULT; break; }
            w = unix_send(f, k, take, mflags);
        } else {
            w = unix_recv(f, k, take, mflags);
            if (w > 0 && copy_to_user((u8 *)ubuf + total, k, (size_t)w)) { rc = total ? total : -EFAULT; break; }
        }
        if (w < 0) { rc = total ? total : w; break; }
        total += w;
        rc = total;
        if ((size_t)w < take || (!is_send && !(mflags & MSG_WAITALL))) break;
    } while ((size_t)total < len);
    file_put(f);
    kfree(k);
    return rc;
}

/* A receive that has no sender's address to report (a stream, or a datagram from a socket that never had a name) says so by making the length 0. */
static void unix_no_name(u64 ufromlen) {
    u32 zero = 0;
    copy_to_user((void *)ufromlen, &zero, sizeof zero);
}

/* sendto and recvfrom on a UNIX datagram socket: one message, with the destination or the sender's name. */
#define UDGRAM_MAX 130000

static long unix_dgram_to(File *f, u64 ubuf, u64 len, int mflags, u64 uto, u64 utolen) {
    if (len > UDGRAM_MAX) return -EMSGSIZE;
    u8 *k = kmalloc(len ? (size_t)len : 1);
    if (!k) return -ENOMEM;
    long rc;
    if (len && copy_from_user(k, (const void *)ubuf, (size_t)len)) {
        rc = -EFAULT;
    } else {
        file_get(f);
        rc = unix_send_msg(f, k, (size_t)len, mflags, uto, utolen, NULL, 0);
        file_put(f);
    }
    kfree(k);
    return rc;
}

static long unix_dgram_from(File *f, u64 ubuf, u64 len, int mflags, u64 ufrom, u64 ufromlen) {
    size_t chunk = (size_t)MIN(len, (u64)UDGRAM_MAX);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    UnixCtl ctl;
    ctl.want_fds = false;
    file_get(f);
    ssize_t r = unix_recv_msg(f, k, chunk, mflags, &ctl);
    file_put(f);
    long rc = r;
    if (r > 0 && copy_to_user((void *)ubuf, k, (size_t)r)) rc = -EFAULT;
    if (rc >= 0 && (mflags & MSG_TRUNC) && ctl.dgram) rc = (long)ctl.full;
    kfree(k);
    if (rc >= 0 && ufrom && ufromlen) {
        if (ctl.fromlen) unix_store_name(ufrom, ufromlen, ctl.from, ctl.fromlen);
        else unix_no_name(ufromlen);
    }
    return rc;
}

static long sys_sendto(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    u64 ubuf = A1(tf), len = A2(tf);
    int mflags = (int)A3(tf);
    if (!is_inet(f)) return unix_is_dgram(f) ? unix_dgram_to(f, ubuf, len, mflags, A4(tf), A5(tf)) : unix_msg_io(f, true, ubuf, len, mflags);
    bool have = A4(tf) != 0;
    u32 addr = 0;
    u16 port = 0;
    if (have) { int rc = sk_get_sockaddr(A4(tf), A5(tf), &addr, &port); if (rc) return rc; }
    if (!user_range_ok((const void *)ubuf, len)) return -EFAULT;
    Sock *s = f->priv;
    if (s->kind != SK_TCP && len > 65507) return -EMSGSIZE;
    size_t chunk = s->kind == SK_TCP ? MIN((size_t)len, (size_t)SEND_CHUNK) : (size_t)len;
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    long total = 0, rc = 0;
    file_get(f);
    do {
        size_t take = MIN((size_t)len - (size_t)total, chunk);
        if (copy_from_user(k, (const u8 *)ubuf + total, take)) { rc = total ? total : -EFAULT; break; }
        ssize_t w = sk_send(f, k, take, mflags, have, addr, port);
        if (w < 0) { rc = total ? total : w; break; }
        total += w;
        rc = total;
        if ((size_t)w < take) break;
    } while ((size_t)total < len);
    file_put(f);
    kfree(k);
    return rc;
}

static long sys_recvfrom(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    u64 ubuf = A1(tf), len = A2(tf);
    int mflags = (int)A3(tf);
    if (!is_inet(f)) {
        if (unix_is_dgram(f)) return unix_dgram_from(f, ubuf, len, mflags, A4(tf), A5(tf));
        long rc = unix_msg_io(f, false, ubuf, len, mflags);
        if (rc >= 0 && A4(tf) && A5(tf)) unix_no_name(A5(tf));
        return rc;
    }
    if (!user_range_ok((void *)ubuf, len)) return -EFAULT;
    Sock *s = f->priv;
    size_t chunk = s->kind == SK_TCP ? MIN((size_t)len, (size_t)SEND_CHUNK) : (size_t)MIN(len, (u64)65536);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    u32 from = 0;
    u16 fport = 0;
    size_t full = 0;
    ssize_t r = sk_recv_ex(f, k, chunk, mflags, &from, &fport, &full);
    long rc = r;
    if (r > 0 && copy_to_user((void *)ubuf, k, (size_t)r)) rc = -EFAULT;
    if (rc >= 0 && (mflags & MSG_TRUNC) && s->kind != SK_TCP) rc = (long)full;       /* MSG_TRUNC: the real length of the datagram, not what fitted */
    kfree(k);
    if (rc >= 0 && A4(tf)) {
        if (s->kind == SK_TCP) { net_lock(); from = s->raddr; fport = s->rport; net_unlock(); }
        sk_put_sockaddr(A4(tf), A5(tf), from, fport);
    }
    return rc;
}

/* ------------------------------------------------------------------ names and options */

static long sys_getsockname(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    if (!is_inet(f)) return unix_getname(f, false, A1(tf), A2(tf));
    Sock *s = f->priv;
    net_lock();
    u32 a = s->laddr;
    u16 p = s->lport;
    if (!a && s->kind != SK_ICMP && s->raddr) a = ip_source_for(s->raddr);
    net_unlock();
    return sk_put_sockaddr(A1(tf), A2(tf), a, p);
}

static long sys_getpeername(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    if (!is_inet(f)) return unix_getname(f, true, A1(tf), A2(tf));
    Sock *s = f->priv;
    net_lock();
    bool ok = s->kind == SK_TCP ? (s->state >= TCPS_ESTABLISHED && s->state != TCPS_CLOSED && s->state != TCPS_LISTEN && s->state != TCPS_SYN_SENT)
                                : s->connected;
    u32 a = s->raddr;
    u16 p = s->rport;
    net_unlock();
    if (!ok) return -ENOTCONN;
    return sk_put_sockaddr(A1(tf), A2(tf), a, p);
}

static long sys_setsockopt(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    int level = (int)A1(tf), name = (int)A2(tf);
    u64 len = A4(tf);
    u8 val[16];
    memset(val, 0, sizeof val);
    if (len > sizeof val) len = sizeof val;
    if (A3(tf) && len && copy_from_user(val, (const void *)A3(tf), (size_t)len)) return -EFAULT;
    if (!is_inet(f)) return unix_setsockopt(f, level, name, val, (size_t)len);
    Sock *s = f->priv;
    int iv = 0;
    memcpy(&iv, val, MIN((u64)4, len));
    long rc = 0;
    net_lock();
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR: case SO_REUSEPORT: s->reuseaddr = iv != 0; break;
        case SO_BROADCAST: s->broadcast = iv != 0; break;
        case SO_KEEPALIVE: s->keepalive = iv != 0; break;
        case SO_SNDBUF: case SO_RCVBUF: break;                            /* the buffers are fixed-size rings; accepted and ignored */
        case SO_LINGER:
            if (len < 8) { rc = -EINVAL; break; }
            memcpy(&s->linger_on, val, 4);
            memcpy(&s->linger_sec, val + 4, 4);
            break;
        case 5: case 10: case 11: case 12: case 16: case 18: case 19: case 29: case 32: case 33:
            break;                                                         /* DONTROUTE, OOBINLINE, NO_CHECK, PRIORITY, PASSCRED, RCVLOWAT, SNDLOWAT, TIMESTAMP, SNDBUFFORCE, RCVBUFFORCE: accepted, no effect */
        case SO_RCVTIMEO: case SO_SNDTIMEO: {
            if (len < sizeof(struct gtimeval)) { rc = -EINVAL; break; }
            struct gtimeval tv;
            memcpy(&tv, val, sizeof tv);
            if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) { rc = -EINVAL; break; }
            u64 ms = (u64)tv.tv_sec * 1000 + (u64)tv.tv_usec / 1000 + ((tv.tv_usec % 1000) ? 1 : 0);
            if (ms > 0xFFFFFFFFu) ms = 0xFFFFFFFFu;
            if (name == SO_RCVTIMEO) s->rcvtimeo = (u32)ms; else s->sndtimeo = (u32)ms;
            break;
        }
        default: rc = -ENOPROTOOPT;
        }
    } else if (level == IPPROTO_IP) {
        if (name == IP_TTL) { if (iv < 1 || iv > 255) rc = -EINVAL; else s->ttl = (u8)iv; }
        else if (name == IP_TOS) s->tos = (u8)iv;
        else if (name == 10 || name == 11) { }                              /* IP_MTU_DISCOVER, IP_RECVERR: accepted, no effect */
        else rc = -ENOPROTOOPT;
    } else if (level == IPPROTO_TCP && s->kind == SK_TCP) {
        switch (name) {
        case TCP_NODELAY: s->nodelay = iv != 0; if (s->nodelay) net_kick(); break;
        case TCP_MAXSEG: if (iv >= 88 && iv < s->mss) s->mss = (u16)iv; break;
        case 4: if (iv < 1 || iv > 32767) rc = -EINVAL; else s->ka_idle = (u16)iv; break;       /* TCP_KEEPIDLE */
        case 5: if (iv < 1 || iv > 32767) rc = -EINVAL; else s->ka_intvl = (u16)iv; break;      /* TCP_KEEPINTVL */
        case 6: if (iv < 1 || iv > 127) rc = -EINVAL; else s->ka_cnt = (u8)iv; break;           /* TCP_KEEPCNT */
        case 3: case 7: case 8: case 9: case 12: case 18: break;           /* CORK, SYNCNT, LINGER2, DEFER_ACCEPT, QUICKACK, USER_TIMEOUT: accepted, no effect */
        default: rc = -ENOPROTOOPT;
        }
    } else {
        rc = -ENOPROTOOPT;
    }
    net_unlock();
    return rc;
}

static long sys_getsockopt(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!is_sock(f)) return -ENOTSOCK;
    int level = (int)A1(tf), name = (int)A2(tf);
    u32 ulen;
    if (!A4(tf) || copy_from_user(&ulen, (const void *)A4(tf), sizeof ulen)) return -EFAULT;
    u8 out[16];
    memset(out, 0, sizeof out);
    size_t olen = 4;
    int iv = 0;
    long rc = 0;
    if (!is_inet(f)) {
        rc = unix_getsockopt(f, level, name, out, &olen);
        if (olen == 4) memcpy(&iv, out, 4);
    } else {
        Sock *s = f->priv;
        net_lock();
        if (level == SOL_SOCKET) {
            switch (name) {
            case SO_REUSEADDR: case SO_REUSEPORT: iv = s->reuseaddr; break;
            case SO_BROADCAST: iv = s->broadcast; break;
            case SO_KEEPALIVE: iv = s->keepalive; break;
            case SO_TYPE: iv = s->kind == SK_TCP ? SOCK_STREAM : SOCK_DGRAM; break;
            case SO_ERROR: iv = s->err; s->err = 0; break;
            case SO_ACCEPTCONN: iv = s->state == TCPS_LISTEN; break;
            case SO_SNDBUF: case SO_RCVBUF: iv = s->kind == SK_TCP ? TCP_BUF : 131072; break;
            case 38: iv = s->kind == SK_TCP ? IPPROTO_TCP : s->kind == SK_UDP ? IPPROTO_UDP : IPPROTO_ICMP; break;       /* SO_PROTOCOL */
            case 39: iv = AF_INET; break;                                                                               /* SO_DOMAIN */
            case SO_LINGER:
                memcpy(out, &s->linger_on, 4);
                memcpy(out + 4, &s->linger_sec, 4);
                olen = 8;
                break;
            case SO_RCVTIMEO: case SO_SNDTIMEO: {
                u32 ms = name == SO_RCVTIMEO ? s->rcvtimeo : s->sndtimeo;
                struct gtimeval tv = { ms / 1000, (ms % 1000) * 1000 };
                memcpy(out, &tv, sizeof tv);
                olen = sizeof tv;
                break;
            }
            default: rc = -ENOPROTOOPT;
            }
        } else if (level == IPPROTO_IP) {
            if (name == IP_TTL) iv = s->ttl; else if (name == IP_TOS) iv = s->tos; else rc = -ENOPROTOOPT;
        } else if (level == IPPROTO_TCP && s->kind == SK_TCP) {
            switch (name) {
            case TCP_NODELAY: iv = s->nodelay; break;
            case TCP_MAXSEG: iv = s->mss; break;
            case 4: iv = s->ka_idle ? s->ka_idle : 7200; break;
            case 5: iv = s->ka_intvl ? s->ka_intvl : 75; break;
            case 6: iv = s->ka_cnt ? s->ka_cnt : 9; break;
            default: rc = -ENOPROTOOPT;
            }
        } else {
            rc = -ENOPROTOOPT;
        }
        net_unlock();
    }
    if (rc) return rc;
    if (olen == 4) memcpy(out, &iv, 4);
    size_t n = MIN((size_t)ulen, olen);
    if (n && copy_to_user((void *)A3(tf), out, n)) return -EFAULT;
    u32 outlen = (u32)olen;
    if (copy_to_user((void *)A4(tf), &outlen, sizeof outlen)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ netctl */

static long sys_netctl(TrapFrame *tf) {
    unsigned long cmd = A0(tf);
    size_t size;
    switch (cmd) {
    case NETCTL_IF_GET: case NETCTL_IF_CONFIG: case NETCTL_DHCP: case NETCTL_DHCP_STOP: size = sizeof(struct gnet_if); break;
    case NETCTL_ROUTE_GET: case NETCTL_ROUTE_ADD: case NETCTL_ROUTE_DEL: size = sizeof(struct gnet_route); break;
    case NETCTL_ARP_GET: case NETCTL_ARP_SET: case NETCTL_ARP_DEL: size = sizeof(struct gnet_arp); break;
    default: return -EINVAL;
    }
    u8 *k = kzalloc(size);
    if (!k) return -ENOMEM;
    long rc = copy_from_user(k, (const void *)A1(tf), size) ? -EFAULT : 0;
    if (!rc) rc = net_ctl(cmd, k);
    if (rc >= 0 && copy_to_user((void *)A1(tf), k, size)) rc = -EFAULT;
    kfree(k);
    return rc;
}

/* ------------------------------------------------------------------ /proc/net/tcp and /proc/net/udp */

size_t net_proc_tcp(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int i = 0;
    net_lock();
    for (Sock *s = g_socks; s && n + 160 < cap; s = s->next) {
        if (s->kind != SK_TCP || !s->bound) continue;
        u32 txq = (s32)(s->snd_end - s->snd_una) > 0 ? s->snd_end - s->snd_una : 0;
        n += (size_t)snprintf(b + n, cap - n, "%4d: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %5u %8d %u\n", i++, hton32(s->laddr), s->lport,
                              hton32(s->raddr), s->rport, s->state, txq, (u32)tcp_readable(s), s->t_rexmit ? 1 : 0, 0u, s->nrexmit, s->uid, 0, s->ino);
    }
    net_unlock();
    return n;
}

size_t net_proc_udp(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    int i = 0;
    net_lock();
    for (Sock *s = g_socks; s && n + 160 < cap; s = s->next) {
        if (s->kind != SK_UDP || !s->bound) continue;
        n += (size_t)snprintf(b + n, cap - n, "%4d: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %5u %8d %u\n", i++, hton32(s->laddr), s->lport,
                              hton32(s->raddr), s->rport, s->connected ? 1 : 7, 0u, s->rq_bytes, 0, 0u, 0u, s->uid, 0, s->ino);
    }
    net_unlock();
    return n;
}

/* ------------------------------------------------------------------ registration */

static void sockcall_init(void) {
    syscall_register(SYS_sendto, sys_sendto);
    syscall_register(SYS_recvfrom, sys_recvfrom);
    syscall_register(SYS_getsockname, sys_getsockname);
    syscall_register(SYS_getpeername, sys_getpeername);
    syscall_register(SYS_setsockopt, sys_setsockopt);
    syscall_register(SYS_getsockopt, sys_getsockopt);
    syscall_register(SYS_netctl, sys_netctl);
}
INITCALL(sockcall_init);
