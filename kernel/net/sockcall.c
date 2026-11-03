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

static inline bool is_inet(File *f) { return f && f->fops == &g_inet_fops; }

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

static long unix_like_io(File *f, bool is_send, u64 ubuf, u64 len) {
    size_t chunk = MIN((size_t)len, (size_t)SEND_CHUNK);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    long rc;
    file_get(f);
    if (is_send) {
        if (copy_from_user(k, (const void *)ubuf, chunk)) rc = -EFAULT;
        else rc = vfs_write(f, k, chunk);
    } else {
        rc = vfs_read(f, k, chunk);
        if (rc > 0 && copy_to_user((void *)ubuf, k, (size_t)rc)) rc = -EFAULT;
    }
    file_put(f);
    kfree(k);
    return rc;
}

static long sys_sendto(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    u64 ubuf = A1(tf), len = A2(tf);
    int mflags = (int)A3(tf);
    if (!is_inet(f)) return unix_like_io(f, true, ubuf, len);
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
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    u64 ubuf = A1(tf), len = A2(tf);
    int mflags = (int)A3(tf);
    if (!is_inet(f)) return unix_like_io(f, false, ubuf, len);
    if (!user_range_ok((void *)ubuf, len)) return -EFAULT;
    Sock *s = f->priv;
    size_t chunk = s->kind == SK_TCP ? MIN((size_t)len, (size_t)SEND_CHUNK) : (size_t)MIN(len, (u64)65536);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) return -ENOMEM;
    u32 from = 0;
    u16 fport = 0;
    ssize_t r = sk_recv(f, k, chunk, mflags, &from, &fport);
    long rc = r;
    if (r > 0 && copy_to_user((void *)ubuf, k, (size_t)r)) rc = -EFAULT;
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
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    u32 have;
    if (!is_inet(f)) {                                                   /* UNIX sockets: just the family */
        if (A2(tf) && !copy_from_user(&have, (void *)A2(tf), sizeof have) && have >= 2) { u16 fam = AF_UNIX; copy_to_user((void *)A1(tf), &fam, 2); have = 2; copy_to_user((void *)A2(tf), &have, sizeof have); }
        return 0;
    }
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
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    if (!is_inet(f)) return -ENOTCONN;
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
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    int level = (int)A1(tf), name = (int)A2(tf);
    u64 len = A4(tf);
    u8 val[16];
    memset(val, 0, sizeof val);
    if (len > sizeof val) len = sizeof val;
    if (A3(tf) && len && copy_from_user(val, (const void *)A3(tf), (size_t)len)) return -EFAULT;
    if (!is_inet(f)) return level == SOL_SOCKET ? 0 : -ENOPROTOOPT;      /* UNIX sockets accept and ignore the generic options */
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
        case SO_SNDBUF: case SO_RCVBUF: case SO_LINGER: break;            /* the buffers are fixed-size rings; accepted and ignored */
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
        else rc = -ENOPROTOOPT;
    } else if (level == IPPROTO_TCP && s->kind == SK_TCP) {
        if (name == TCP_NODELAY) { s->nodelay = iv != 0; if (s->nodelay) net_kick(); }
        else if (name == TCP_MAXSEG) { if (iv >= 88 && iv < s->mss) s->mss = (u16)iv; }
        else rc = -ENOPROTOOPT;
    } else {
        rc = -ENOPROTOOPT;
    }
    net_unlock();
    return rc;
}

static long sys_getsockopt(TrapFrame *tf) {
    File *f = fd_get(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    if (!(f->vn && S_ISSOCK(f->vn->mode))) return -ENOTSOCK;
    int level = (int)A1(tf), name = (int)A2(tf);
    u32 ulen;
    if (!A4(tf) || copy_from_user(&ulen, (const void *)A4(tf), sizeof ulen)) return -EFAULT;
    u8 out[16];
    memset(out, 0, sizeof out);
    size_t olen = 4;
    int iv = 0;
    long rc = 0;
    if (!is_inet(f)) {
        if (level != SOL_SOCKET) return -ENOPROTOOPT;
        iv = name == SO_TYPE ? SOCK_STREAM : 0;
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
            if (name == TCP_NODELAY) iv = s->nodelay; else if (name == TCP_MAXSEG) iv = s->mss; else rc = -ENOPROTOOPT;
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
    case NETCTL_IF_GET: case NETCTL_IF_CONFIG: case NETCTL_DHCP: size = sizeof(struct gnet_if); break;
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
