/*
 * AF_INET sockets: the socket objects, UDP and the ICMP "ping" sockets, and the glue to the system calls. socket(), bind(),
 * listen(), accept(), connect() and shutdown() arrive through kernel/fs/socket.c (which also serves AF_UNIX); the calls that
 * only make sense here (sendto, recvfrom, getsockname, getpeername, setsockopt, getsockopt, netctl) are registered below.
 */
#include "netsock.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "process.h"
#include "random.h"
#include "initcall.h"

extern void (*g_signal_self_hook)(int sig);

Sock *g_socks;
static u32 g_next_ino = 3000;
static int g_nsocks;
#define MAX_SOCKS 4096

/* ------------------------------------------------------------------ socket objects */

Sock *sock_new(int kind) {
    if (g_nsocks >= MAX_SOCKS) return NULL;
    Sock *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->kind = (u8)kind;
    s->ttl = 64;
    s->ino = g_next_ino++;
    wq_init(&s->wq);
    if (kind == SK_TCP) tcp_sock_init(s);
    s->next = g_socks;
    if (g_socks) g_socks->prev = s;
    g_socks = s;
    g_nsocks++;
    return s;
}

static void dgrams_free(Sock *s) {
    while (s->rq) {
        Dgram *d = s->rq;
        s->rq = d->next;
        kfree(d);
    }
    s->rq_tail = NULL;
    s->rq_bytes = 0;
}

void sock_unlink_free(Sock *s) {
    if (s->prev) s->prev->next = s->next; else g_socks = s->next;
    if (s->next) s->next->prev = s->prev;
    g_nsocks--;
    dgrams_free(s);
    kfree(s->sbuf);
    kfree(s->rbuf);
    kfree(s);
}

void sock_wake(Sock *s) { wq_wake_all(&s->wq); }

bool sock_exists(Sock *s) {
    for (Sock *o = g_socks; o; o = o->next) if (o == s) return true;
    return false;
}

u16 sock_alloc_port(int kind) {
    for (int tries = 0; tries < 2000; tries++) {
        u16 p = (u16)(49152 + random_u64() % 16384);
        bool used = false;
        for (Sock *o = g_socks; o && !used; o = o->next) if (o->kind == kind && o->bound && o->lport == p) used = true;
        if (!used) return p;
    }
    return 0;
}

bool sock_port_conflict(Sock *me, u32 addr, u16 port) {
    for (Sock *o = g_socks; o; o = o->next) {
        if (o == me || o->kind != me->kind || !o->bound || o->lport != port) continue;
        if (!(o->laddr == 0 || addr == 0 || o->laddr == addr)) continue;
        if (me->kind == SK_TCP) {
            if (o->state == TCPS_LISTEN) return true;
            if (!me->reuseaddr) { if (o->state != TCPS_CLOSED || o->has_file) return true; continue; }
            if (!o->reuseaddr && o->state != TCPS_TIME_WAIT) return true;
        } else if (!(me->reuseaddr && o->reuseaddr)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ UDP */

void udp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst) {
    (void)d;
    if (len < 8) { g_net_stat_ip_bad++; return; }
    size_t ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) { g_net_stat_ip_bad++; return; }
    len = ulen;
    if (rd16(p + 6) != 0 && net_csum(p, len, net_pseudo_csum(src, dst, IPPROTO_UDP, (u16)len)) != 0xFFFF) { g_net_stat_ip_bad++; return; }
    u16 sport = rd16(p), dport = rd16(p + 2);
    bool bcast = dst == 0xFFFFFFFFu || (dst & 0xFF) == 0xFF;
    bool delivered = false;
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_UDP || !s->bound || s->lport != dport) continue;
        if (s->laddr && s->laddr != dst && !(bcast && s->broadcast)) continue;
        if (s->connected && (s->raddr != src || s->rport != sport)) continue;
        if (s->rd_shut_user) continue;
        if (s->rq_bytes + len > 131072) { delivered = true; continue; }                  /* receive buffer full: drop */
        Dgram *g = kmalloc(sizeof *g + len - 8);
        if (!g) continue;
        g->next = NULL; g->saddr = src; g->sport = sport; g->len = (u32)(len - 8);
        memcpy(g->data, p + 8, len - 8);
        if (s->rq_tail) s->rq_tail->next = g; else s->rq = g;
        s->rq_tail = g;
        s->rq_bytes += g->len + 32;
        sock_wake(s);
        delivered = true;
        if (!bcast) break;
    }
    if (!delivered && !bcast && g_net_rx_ip) icmp_send_unreachable(g_net_rx_ip, g_net_rx_iplen, 3);
}

void udp_error(u32 laddr, u16 lport, u32 raddr, u16 rport, int err) {
    for (Sock *s = g_socks; s; s = s->next)
        if (s->kind == SK_UDP && s->connected && s->lport == lport && (!s->laddr || s->laddr == laddr) && s->raddr == raddr && s->rport == rport) {
            s->err = err;
            sock_wake(s);
        }
}

static int udp_send(Sock *s, const u8 *data, size_t n, u32 daddr, u16 dport) {
    if (n > 65507) return -EMSGSIZE;
    if (!daddr) daddr = INADDR_LOOPBACK;                             /* sending to 0.0.0.0 means "this host" */
    bool bcast = daddr == 0xFFFFFFFFu;
    if (!bcast) for (NetDev *d = netdev_by_index(0); d; d = netdev_by_index(d->index + 1)) if (d->bcast && d->bcast == daddr) bcast = true;
    if (bcast && !s->broadcast) return -EACCES;
    if (!s->bound) {
        u16 p = sock_alloc_port(SK_UDP);
        if (!p) return -EADDRINUSE;
        s->lport = p;
        s->bound = true;
    }
    u32 src = s->laddr;
    if (!src && !bcast) { src = ip_source_for(daddr); if (!src) return -ENETUNREACH; }
    size_t total = 8 + n;
    NetBuf *nb = nb_alloc();
    if (nb && total <= nb_room(nb)) {
        u8 *u = nb_put(nb, total);
        wr16(u, s->lport); wr16(u + 2, dport); wr16(u + 4, (u16)total); u[6] = u[7] = 0;
        memcpy(u + 8, data, n);
        u32 psrc = src ? src : ip_source_for(daddr);
        u16 c = (u16)~net_csum(u, total, net_pseudo_csum(psrc, daddr, IPPROTO_UDP, (u16)total));
        if (c == 0) c = 0xFFFF;
        memcpy(u + 6, &c, 2);
        return ip_output(src, daddr, IPPROTO_UDP, s->ttl, s->tos, false, nb);
    }
    nb_free(nb);
    u8 *big = kmalloc(total);
    if (!big) return -ENOMEM;
    wr16(big, s->lport); wr16(big + 2, dport); wr16(big + 4, (u16)total); big[6] = big[7] = 0;
    memcpy(big + 8, data, n);
    u32 psrc = src ? src : ip_source_for(daddr);
    u16 c = (u16)~net_csum(big, total, net_pseudo_csum(psrc, daddr, IPPROTO_UDP, (u16)total));
    if (c == 0) c = 0xFFFF;
    memcpy(big + 6, &c, 2);
    int rc = ip_output_buf(src, daddr, IPPROTO_UDP, s->ttl, s->tos, false, big, total);
    kfree(big);
    return rc;
}

/* ------------------------------------------------------------------ ICMP sockets */

void icmp_sock_deliver(const u8 *ip, size_t len) {
    if (len < IP_HLEN + 8) return;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if (ihl < IP_HLEN || len < ihl + 8) return;
    const u8 *m = ip + ihl;
    u16 ident;
    if (m[0] == 0) ident = rd16(m + 4);                              /* echo reply */
    else if (len >= ihl + 8 + IP_HLEN + 8) {                         /* error quoting our echo request */
        const u8 *o = m + 8;
        size_t oihl = (size_t)(o[0] & 15) * 4;
        if ((o[0] >> 4) != 4 || o[9] != IPPROTO_ICMP || len < ihl + 8 + oihl + 8 || o[oihl] != 8) return;
        ident = rd16(o + oihl + 4);
    } else return;
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_ICMP || s->icmp_ident != ident || s->rq_bytes + len > 65536) continue;
        Dgram *g = kmalloc(sizeof *g + len);
        if (!g) continue;
        g->next = NULL; g->saddr = rd32(ip + 12); g->sport = 0; g->len = (u32)len;
        memcpy(g->data, ip, len);
        if (s->rq_tail) s->rq_tail->next = g; else s->rq = g;
        s->rq_tail = g;
        s->rq_bytes += (u32)len + 32;
        sock_wake(s);
    }
}

static int icmp_send_echo(Sock *s, const u8 *data, size_t n, u32 daddr) {
    if (n < 8 || n > 1400) return -EMSGSIZE;
    if (data[0] != 8 || data[1] != 0) return -EINVAL;                /* only echo requests */
    u32 src = s->laddr ? s->laddr : ip_source_for(daddr);
    if (!src) return -ENETUNREACH;
    NetBuf *nb = nb_alloc();
    if (!nb) return -ENOMEM;
    u8 *m = nb_put(nb, n);
    memcpy(m, data, n);
    wr16(m + 4, s->icmp_ident);
    m[2] = m[3] = 0;
    u16 c = (u16)~net_csum(m, n, 0);
    memcpy(m + 2, &c, 2);
    return ip_output(src, daddr, IPPROTO_ICMP, s->ttl, s->tos, false, nb);
}

/* ------------------------------------------------------------------ the file side */

const VNodeOps g_inet_vn_ops = { 0 };

static int inet_poll(File *f) {
    Sock *s = f->priv;
    int m = 0;
    net_lock();
    if (s->kind == SK_TCP) m = tcp_poll(s);
    else {
        if (s->rq || s->rd_shut_user) m |= POLLIN;
        if (s->err) m |= POLLERR;
        m |= POLLOUT;
    }
    net_unlock();
    return m;
}


static ssize_t inet_read(File *f, void *buf, size_t n) { return sk_recv(f, buf, n, 0, NULL, NULL); }
static ssize_t inet_write(File *f, const void *buf, size_t n) { return sk_send(f, buf, n, 0, false, 0, 0); }

static int inet_ioctl(File *f, unsigned long req, void *arg) {
    Sock *s = f->priv;
    if (req == FIONREAD) {
        net_lock();
        *(int *)arg = s->kind == SK_TCP ? (int)tcp_readable(s) : s->rq ? (int)s->rq->len : 0;
        net_unlock();
        return 0;
    }
    return -ENOTTY;
}

static void inet_close(File *f) {
    Sock *s = f->priv;
    net_lock();
    if (s->kind == SK_TCP) tcp_close(s);
    else sock_unlink_free(s);
    net_unlock();
    net_kick();
}

const FileOps g_inet_fops = { .read = inet_read, .write = inet_write, .ioctl = inet_ioctl, .close = inet_close, .poll = inet_poll };

/* ------------------------------------------------------------------ address helpers */

int sk_get_sockaddr(u64 uaddr, u64 len, u32 *addr, u16 *port) {
    struct gsockaddr_in sa;
    if (len < sizeof sa) return -EINVAL;
    if (copy_from_user(&sa, (const void *)uaddr, sizeof sa)) return -EFAULT;
    if (sa.family != AF_INET) return -EAFNOSUPPORT;
    *addr = ntoh32(sa.addr);
    *port = ntoh16(sa.port);
    return 0;
}

int sk_put_sockaddr(u64 uaddr, u64 ulenp, u32 addr, u16 port) {
    if (!uaddr || !ulenp) return 0;
    u32 have;
    if (copy_from_user(&have, (const void *)ulenp, sizeof have)) return -EFAULT;
    struct gsockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.family = AF_INET;
    sa.port = hton16(port);
    sa.addr = hton32(addr);
    u32 n = have < sizeof sa ? have : (u32)sizeof sa;
    if (n && copy_to_user((void *)uaddr, &sa, n)) return -EFAULT;
    u32 want = sizeof sa;
    if (copy_to_user((void *)ulenp, &want, sizeof want)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ blocking helper */

int sk_block(Sock *s, u32 tmo, u64 *deadline) {
    u64 ms = NET_FOREVER;
    if (tmo) {
        u64 now = net_now();
        if (!*deadline) *deadline = now + tmo;
        if (now >= *deadline) return -EAGAIN;
        ms = *deadline - now;
    }
    int rc = net_wait(&s->wq, ms);
    return rc == -ETIMEDOUT ? -EAGAIN : rc;
}

/* ------------------------------------------------------------------ receive */

ssize_t sk_recv(File *f, u8 *buf, size_t n, int mflags, u32 *from_addr, u16 *from_port) {
    Sock *s = f->priv;
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    bool peek = (mflags & MSG_PEEK) != 0;
    u64 deadline = 0;
    ssize_t rc = 0;
    size_t got = 0;
    file_get(f);
    net_lock();
    if (s->kind == SK_TCP) {
        for (;;) {
            if (s->state == TCPS_LISTEN) { rc = -ENOTCONN; break; }
            size_t avail = tcp_readable(s);
            if (avail && got < n) {
                got += tcp_read(s, buf + got, n - got, peek);
                if (peek || !(mflags & MSG_WAITALL) || got >= n) { rc = (ssize_t)got; break; }
                continue;
            }
            if (got) { rc = (ssize_t)got; break; }
            if (s->err) { rc = -s->err; s->err = 0; break; }
            if (s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD) { rc = -ENOTCONN; break; }
            if (s->state == TCPS_CLOSED) { rc = (s->rd_shut || s->wr_shut) ? 0 : -ENOTCONN; break; }
            if (s->rd_shut || s->rd_shut_user || n == 0) { rc = 0; break; }
            if (nb) { rc = -EAGAIN; break; }
            int w = sk_block(s, s->rcvtimeo, &deadline);
            if (w && !tcp_readable(s) && !s->err && !s->rd_shut) { rc = w; break; }
        }
    } else {
        for (;;) {
            if (s->rq && !s->rd_shut_user) {
                Dgram *g = s->rq;
                size_t k = MIN(n, (size_t)g->len);
                memcpy(buf, g->data, k);
                if (from_addr) *from_addr = g->saddr;
                if (from_port) *from_port = g->sport;
                if (!peek) {
                    s->rq = g->next;
                    if (!s->rq) s->rq_tail = NULL;
                    s->rq_bytes -= MIN(s->rq_bytes, g->len + 32);
                    kfree(g);
                }
                rc = (ssize_t)k;
                break;
            }
            if (s->err) { rc = -s->err; s->err = 0; break; }
            if (s->rd_shut_user) { rc = 0; break; }
            if (nb) { rc = -EAGAIN; break; }
            int w = sk_block(s, s->rcvtimeo, &deadline);
            if (w && !s->rq && !s->err) { rc = w; break; }
        }
    }
    net_unlock();
    file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ send */

ssize_t sk_send(File *f, const u8 *buf, size_t n, int mflags, bool have_addr, u32 daddr, u16 dport) {
    Sock *s = f->priv;
    bool nb = (f->flags & O_NONBLOCK) || (mflags & MSG_DONTWAIT);
    u64 deadline = 0;
    ssize_t rc = 0;
    size_t sent = 0;
    bool sigpipe = false;
    file_get(f);
    net_lock();
    if (s->kind == SK_TCP) {
        for (;;) {
            if (s->err) { rc = sent ? (ssize_t)sent : -s->err; if (!sent) s->err = 0; break; }
            bool can = s->state == TCPS_ESTABLISHED || s->state == TCPS_CLOSE_WAIT;
            if (!can) {
                if (s->state == TCPS_LISTEN || s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD) rc = -ENOTCONN;
                else if (s->state == TCPS_CLOSED && !s->wr_shut && !s->rd_shut) rc = -ENOTCONN;
                else { rc = sent ? (ssize_t)sent : -EPIPE; sigpipe = !sent; }
                break;
            }
            if (s->wr_shut || s->fin_pending) { rc = sent ? (ssize_t)sent : -EPIPE; sigpipe = !sent; break; }
            if (sent >= n) { rc = (ssize_t)sent; break; }
            size_t k = tcp_queue(s, buf + sent, n - sent);
            sent += k;
            if (sent >= n) { rc = (ssize_t)sent; break; }
            if (k) continue;
            if (nb) { rc = sent ? (ssize_t)sent : -EAGAIN; break; }
            int w = sk_block(s, s->sndtimeo, &deadline);
            if (w && !tcp_send_space(s) && !s->err) { rc = sent ? (ssize_t)sent : w; break; }
        }
    } else if (s->kind == SK_UDP) {
        if (!have_addr) {
            if (!s->connected) rc = -EDESTADDRREQ;
            else { daddr = s->raddr; dport = s->rport; have_addr = true; }
        }
        if (have_addr) {
            if (s->err) { rc = -s->err; s->err = 0; }
            else if (s->wr_shut) { rc = -EPIPE; sigpipe = true; }
            else { int r = udp_send(s, buf, n, daddr, dport); rc = r ? r : (ssize_t)n; }
        }
    } else {
        if (!have_addr && s->raddr) { daddr = s->raddr; have_addr = true; }
        if (!have_addr) rc = -EDESTADDRREQ;
        else { int r = icmp_send_echo(s, buf, n, daddr); rc = r ? r : (ssize_t)n; }
    }
    net_unlock();
    net_kick();
    file_put(f);
    if (sigpipe && !(mflags & MSG_NOSIGNAL) && g_signal_self_hook) g_signal_self_hook(SIGPIPE);
    return rc;
}
