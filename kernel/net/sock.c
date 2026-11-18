/*
 * AF_INET and AF_INET6 sockets: the socket objects, UDP and the ICMP / ICMPv6 "ping" sockets, and the glue to the system calls. socket(), bind(), listen(), accept(), connect() and
 * shutdown() arrive through kernel/fs/socket.c (which also serves AF_UNIX); the calls that only make sense here (sendto, recvfrom, getsockname, getpeername, setsockopt,
 * getsockopt, netctl) are registered in sockcall.c. How a socket holds either family: netsock.h.
 *
 * IPv6 follows RFC 3493 (the sockets interface: sockaddr_in6, v4-mapped addresses, IPV6_V6ONLY) and RFC 8200 section 8 (UDP over IPv6: the checksum is mandatory); an ICMPv6 ping
 * socket gets the ICMPv6 message without its IP header, as Linux's does.
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
    s->mcast_hops = 1;
    s->mcast_loop = true;
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

int inet_sock_count(void) { return g_nsocks; }

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

/* Two sockets conflict over a port when their bindings overlap in either protocol (an address and the wildcard overlap; so do two equal addresses), and the reuse rules do not allow it. */
bool sock_port_conflict(Sock *me, const SockBind *b, u16 port) {
    for (Sock *o = g_socks; o; o = o->next) {
        if (o == me || o->kind != me->kind || !o->bound || o->lport != port) continue;
        bool overlap = false;
        if (b->m4 && sk_binds4(o) && (!b->a4 || !o->laddr || b->a4 == o->laddr)) overlap = true;
        if (b->m6 && sk_binds6(o) && (in6_is_any(&b->a6) || in6_is_any(&o->laddr6) || in6_eq(&b->a6, &o->laddr6))) overlap = true;
        if (!overlap) continue;
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

void sock_autobind(Sock *s, u16 port) {
    s->lport = port;
    s->bound = true;
    if (s->family == AF_INET6) {
        s->m6 = true;
        s->m4 = !s->v6only;
        s->laddr = 0;
        in6_zero(&s->laddr6);
    }
}

/* ------------------------------------------------------------------ datagram queues */

static void dgram_put(Sock *s, const u8 *data, size_t n, bool v6, u32 saddr4, const In6 *saddr6, u32 scope, u16 sport) {
    Dgram *g = kmalloc(sizeof *g + n);
    if (!g) return;
    g->next = NULL;
    g->saddr = saddr4;
    g->sport = sport;
    g->len = (u32)n;
    g->v6 = v6;
    g->scope = scope;
    if (v6) g->saddr6 = *saddr6; else in6_zero(&g->saddr6);
    memcpy(g->data, data, n);
    if (s->rq_tail) s->rq_tail->next = g; else s->rq = g;
    s->rq_tail = g;
    s->rq_bytes += (u32)n + 32;
    sock_wake(s);
}

/* ------------------------------------------------------------------ UDP */

static void udp_bad(NetDev *d, const char *why, const u8 *p, size_t len, u32 src, u32 dst) {
    g_net_stat_ip_bad++;
    if (g_net_trace && g_net_trace_n < 40) {
        g_net_trace_n++;
        char a[20], b[20];
        net_ip_str(src, a, sizeof a);
        net_ip_str(dst, b, sizeof b);
        klog("Yukari", "%s: UDP datagram thrown away (%s): %s > %s, %u bytes, ports %u>%u", d->name, why, a, b, (unsigned)len, len >= 4 ? rd16(p) : 0, len >= 4 ? rd16(p + 2) : 0);
    }
}

void udp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst) {
    if (len < 8) { udp_bad(d, "shorter than a UDP header", p, len, src, dst); return; }
    size_t ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) { udp_bad(d, "length field", p, len, src, dst); return; }
    len = ulen;
    if (rd16(p + 6) != 0 && net_csum(p, len, net_pseudo_csum(src, dst, IPPROTO_UDP, (u16)len)) != 0xFFFF) { udp_bad(d, "checksum", p, len, src, dst); return; }
    u16 sport = rd16(p), dport = rd16(p + 2);
    if (dport == 68 && sport == 67) { dhcp_input(d, p + 8, len - 8); return; }
    bool bcast = dst == 0xFFFFFFFFu || (dst & 0xFF) == 0xFF;
    bool delivered = false;
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_UDP || !s->bound || s->lport != dport) continue;
        if (!sock_match4(s, dst) && !(bcast && s->broadcast && sk_binds4(s))) continue;
        if (s->connected && (s->wire6 || s->raddr != src || s->rport != sport)) continue;
        if (s->rd_shut_user) continue;
        if (s->rq_bytes + len > 131072) { delivered = true; continue; }                  /* receive buffer full: drop */
        dgram_put(s, p + 8, len - 8, false, src, NULL, 0, sport);
        delivered = true;
        if (!bcast) break;
    }
    if (!delivered && !bcast && g_net_rx_ip) icmp_send_unreachable(g_net_rx_ip, g_net_rx_iplen, 3);
}

void udp6_input(NetDev *d, const u8 *p, size_t len, const In6 *src, const In6 *dst) {
    if (len < 8) goto bad;
    size_t ulen = rd16(p + 4);
    if (ulen < 8 || ulen > len) goto bad;
    len = ulen;
    if (rd16(p + 6) == 0) goto bad;                                           /* RFC 8200 section 8.1: an IPv6 receiver discards a UDP datagram with a zero checksum */
    if (net_csum(p, len, net6_pseudo_csum(src, dst, IP6P_UDP, (u32)len)) != 0xFFFF) goto bad;
    u16 sport = rd16(p), dport = rd16(p + 2);
    bool mcast = in6_is_mcast(dst);
    u32 scope = in6_is_linklocal(src) ? ip6_dev_scope(d) : 0;
    bool delivered = false;
    g_ip6_stats.udp_in_datagrams++;
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_UDP || !s->bound || s->lport != dport || !sock_match6(s, dst, d)) continue;
        if (mcast && !in6_is_any(&s->laddr6)) continue;                       /* a group is heard by the sockets bound to :: only (sockets cannot join groups) */
        if (s->connected && (!s->wire6 || !in6_eq(&s->raddr6, src) || s->rport != sport)) continue;
        if (s->rd_shut_user) continue;
        if (s->rq_bytes + len > 131072) { delivered = true; continue; }
        dgram_put(s, p + 8, len - 8, true, 0, src, scope, sport);
        delivered = true;
        if (!mcast) break;
    }
    if (!delivered && !mcast) {
        g_ip6_stats.udp_no_ports++;
        if (g_rx6_pkt) icmp6_error(ICMP6_DST_UNREACH, 4, 0, g_rx6_pkt, g_rx6_len, d);
    }
    return;
bad:
    g_ip6_stats.udp_in_errors++;
    g_ip6_stats.bad++;
}

void udp_error(u32 laddr, u16 lport, u32 raddr, u16 rport, int err) {
    for (Sock *s = g_socks; s; s = s->next)
        if (s->kind == SK_UDP && s->connected && !s->wire6 && s->lport == lport && (!s->laddr || s->laddr == laddr) && s->raddr == raddr && s->rport == rport) {
            s->err = err;
            sock_wake(s);
        }
}

void udp6_error(const In6 *laddr, u16 lport, const In6 *raddr, u16 rport, int err) {
    for (Sock *s = g_socks; s; s = s->next)
        if (s->kind == SK_UDP && s->connected && s->wire6 && s->lport == lport && (in6_is_any(&s->laddr6) || in6_eq(&s->laddr6, laddr)) && in6_eq(&s->raddr6, raddr) && s->rport == rport) {
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
        sock_autobind(s, p);
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

static int udp6_send(Sock *s, const u8 *data, size_t n, const SockAddr *to) {
    if (n > IP6_MAX_PAYLOAD - 8) return -EMSGSIZE;
    In6 dst = to->a6;
    if (in6_is_any(&dst)) { in6_zero(&dst); dst.b[15] = 1; }                 /* sending to :: means "this host" */
    if (!s->bound) {
        u16 p = sock_alloc_port(SK_UDP);
        if (!p) return -EADDRINUSE;
        sock_autobind(s, p);
    }
    const In6 *hint = (s->m6 && !in6_is_any(&s->laddr6)) ? &s->laddr6 : NULL;
    u32 scope = to->scope ? to->scope : s->scope;
    Pick6 pk;
    int rc = ip6_pick(hint, &dst, scope, &pk);
    if (rc) { g_ip6_stats.out_no_routes++; return rc; }
    size_t total = 8 + n;
    u8 *big = NULL;
    NetBuf *nb = nb_alloc();
    u8 *u;
    if (nb && total + IP6_HLEN <= pk.mtu && total <= nb_room(nb)) {
        u = nb_put(nb, total);
    } else {
        nb_free(nb);
        nb = NULL;
        big = kmalloc(total);
        if (!big) return -ENOMEM;
        u = big;
    }
    wr16(u, s->lport); wr16(u + 2, to->port); wr16(u + 4, (u16)total); u[6] = u[7] = 0;
    memcpy(u + 8, data, n);
    u16 c = ip6_l4_csum(&pk.src, &dst, IP6P_UDP, u, total);
    if (c == 0) c = 0xFFFF;
    memcpy(u + 6, &c, 2);
    g_ip6_stats.udp_out_datagrams++;
    if (nb) return ip6_send_nb(&pk, &dst, IP6P_UDP, s->ttl, s->tos, true, nb);
    rc = ip6_send_data(&pk, &dst, IP6P_UDP, s->ttl, s->tos, true, big, total);
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
        dgram_put(s, ip, len, false, rd32(ip + 12), NULL, 0, 0);
    }
}

void icmp6_sock_deliver(const In6 *src, u32 scope, u16 ident, const u8 *msg, size_t len) {
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_ICMP6 || s->icmp_ident != ident || s->rq_bytes + len > 65536) continue;
        dgram_put(s, msg, len, true, 0, src, scope, 0);
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

/* Everything that can happen to an Internet socket wakes its one queue (sock_wake). */
static WaitQueue *inet_pollwq(File *f) { return &((Sock *)f->priv)->wq; }

const FileOps g_inet_fops = { .read = inet_read, .write = inet_write, .ioctl = inet_ioctl, .close = inet_close, .poll = inet_poll, .pollwq = inet_pollwq };

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

/* An address from a program, as the socket's family allows it: an AF_INET socket takes a sockaddr_in; an AF_INET6 socket a sockaddr_in6, whose v4-mapped addresses become IPv4
 * (RFC 3493 section 3.7). Anything else is EAFNOSUPPORT, as on Linux. */
int sk_get_addr_fam(u8 family, u64 uaddr, u64 len, SockAddr *out) {
    memset(out, 0, sizeof *out);
    if (family != AF_INET6) {
        u32 a;
        u16 p;
        int rc = sk_get_sockaddr(uaddr, len, &a, &p);
        if (rc) return rc;
        out->fam = AF_INET;
        out->a4 = a;
        out->port = p;
        return 0;
    }
    struct gsockaddr_in6 sa;
    if (len < 24) return -EINVAL;
    memset(&sa, 0, sizeof sa);
    if (copy_from_user(&sa, (const void *)uaddr, MIN((size_t)len, sizeof sa))) return -EFAULT;
    if (sa.family != AF_INET6) return -EAFNOSUPPORT;
    In6 a;
    memcpy(a.b, sa.addr, 16);
    out->port = ntoh16(sa.port);
    if (in6_is_v4mapped(&a)) {
        out->fam = AF_INET;
        out->a4 = in6_v4(&a);
    } else {
        out->fam = AF_INET6;
        out->a6 = a;
        out->scope = len >= sizeof sa ? sa.scope_id : 0;
    }
    return 0;
}

int sk_get_addr(const Sock *s, u64 uaddr, u64 len, SockAddr *out) { return sk_get_addr_fam(s->family, uaddr, len, out); }

/* The bytes of an address as the socket's family shows it: a sockaddr_in (16 bytes), or a sockaddr_in6 (28) in which an IPv4 address is v4-mapped. Returns the length; out holds 28 bytes. */
size_t sk_addr_bytes(u8 family, const SockAddr *a, void *out) {
    if (family != AF_INET6) {
        struct gsockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.family = AF_INET;
        sa.port = hton16(a->port);
        sa.addr = hton32(a->a4);
        memcpy(out, &sa, sizeof sa);
        return sizeof sa;
    }
    struct gsockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.family = AF_INET6;
    sa.port = hton16(a->port);
    if (a->fam == AF_INET) {
        In6 m;
        in6_set_v4mapped(&m, a->a4);
        memcpy(sa.addr, m.b, 16);
    } else {
        memcpy(sa.addr, a->a6.b, 16);
        if (in6_is_linklocal(&a->a6) || (in6_is_mcast(&a->a6) && in6_scope(&a->a6) == IP6_SCOPE_LINK)) sa.scope_id = a->scope;
    }
    memcpy(out, &sa, sizeof sa);
    return sizeof sa;
}

int sk_put_addr(u8 family, u64 uaddr, u64 ulenp, const SockAddr *a) {
    if (!uaddr || !ulenp) return 0;
    u32 have;
    if (copy_from_user(&have, (const void *)ulenp, sizeof have)) return -EFAULT;
    u8 sa[sizeof(struct gsockaddr_in6)];
    u32 want = (u32)sk_addr_bytes(family, a, sa);
    u32 n = have < want ? have : want;
    if (n && copy_to_user((void *)uaddr, sa, n)) return -EFAULT;
    if (copy_to_user((void *)ulenp, &want, sizeof want)) return -EFAULT;
    return 0;
}

static bool sk_connected_now(const Sock *s) {
    return s->kind == SK_TCP ? (s->state != TCPS_CLOSED && s->state != TCPS_LISTEN) : s->connected;
}

void sk_local_addr(Sock *s, SockAddr *a) {
    memset(a, 0, sizeof *a);
    a->port = s->lport;
    if (s->family != AF_INET6) {
        a->fam = AF_INET;
        a->a4 = s->laddr;
        if (!a->a4 && s->kind != SK_ICMP && s->raddr) a->a4 = ip_source_for(s->raddr);
        return;
    }
    bool conn = sk_connected_now(s);
    if (conn && s->wire6) { a->fam = AF_INET6; a->a6 = s->laddr6; a->scope = s->scope; return; }
    if (conn) {
        a->fam = AF_INET;
        a->a4 = s->laddr ? s->laddr : (s->raddr ? ip_source_for(s->raddr) : 0);
        return;
    }
    if (s->m6 || !s->bound) { a->fam = AF_INET6; a->a6 = s->laddr6; a->scope = s->scope; return; }
    a->fam = AF_INET;                                                       /* bound to an IPv4 address (::ffff:a.b.c.d) */
    a->a4 = s->laddr;
}

bool sk_peer_addr(Sock *s, SockAddr *a) {
    bool ok = s->kind == SK_TCP ? (s->state >= TCPS_ESTABLISHED && s->state != TCPS_CLOSED && s->state != TCPS_LISTEN && s->state != TCPS_SYN_SENT) : s->connected;
    if (!ok) return false;
    memset(a, 0, sizeof *a);
    a->port = s->rport;
    if (s->wire6) { a->fam = AF_INET6; a->a6 = s->raddr6; a->scope = s->scope; }
    else { a->fam = AF_INET; a->a4 = s->raddr; }
    return true;
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
    return sk_recv_ex(f, buf, n, mflags, from_addr, from_port, NULL);
}

ssize_t sk_recv_ex(File *f, u8 *buf, size_t n, int mflags, u32 *from_addr, u16 *from_port, size_t *full) {
    SockAddr from;
    memset(&from, 0, sizeof from);
    ssize_t rc = sk_recv_from(f, buf, n, mflags, (from_addr || from_port) ? &from : NULL, full);
    if (from_addr) *from_addr = from.fam == AF_INET ? from.a4 : 0;
    if (from_port) *from_port = from.port;
    return rc;
}

/* The same, and for a datagram socket *full (if given) becomes the length of the datagram, which is more than the result when the buffer was too small and the
 * rest was thrown away: recvmsg reports that as MSG_TRUNC. For a stream it is the byte count. */
ssize_t sk_recv_from(File *f, u8 *buf, size_t n, int mflags, SockAddr *from, size_t *full) {
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
                if (full) *full = g->len;
                if (from) {
                    memset(from, 0, sizeof *from);
                    from->port = g->sport;
                    if (g->v6) { from->fam = AF_INET6; from->a6 = g->saddr6; from->scope = g->scope; }
                    else { from->fam = AF_INET; from->a4 = g->saddr; }
                }
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
    if (full && s->kind == SK_TCP) *full = rc > 0 ? (size_t)rc : 0;
    net_unlock();
    file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ send */

ssize_t sk_send(File *f, const u8 *buf, size_t n, int mflags, bool have_addr, u32 daddr, u16 dport) {
    SockAddr to;
    memset(&to, 0, sizeof to);
    to.fam = AF_INET;
    to.a4 = daddr;
    to.port = dport;
    return sk_send_to(f, buf, n, mflags, have_addr ? &to : NULL);
}

ssize_t sk_send_to(File *f, const u8 *buf, size_t n, int mflags, const SockAddr *to) {
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
        SockAddr dst;
        bool have = to != NULL;
        if (have) dst = *to;
        if (!have) {
            if (!s->connected) rc = -EDESTADDRREQ;
            else {
                memset(&dst, 0, sizeof dst);
                dst.port = s->rport;
                if (s->wire6) { dst.fam = AF_INET6; dst.a6 = s->raddr6; dst.scope = s->scope; } else { dst.fam = AF_INET; dst.a4 = s->raddr; }
                have = true;
            }
        }
        if (have) {
            if (s->err) { rc = -s->err; s->err = 0; }
            else if (s->wr_shut) { rc = -EPIPE; sigpipe = true; }
            else if (dst.fam == AF_INET6) { int r = udp6_send(s, buf, n, &dst); rc = r ? r : (ssize_t)n; }
            else if (s->family == AF_INET6 && s->v6only) rc = -ENETUNREACH;      /* an IPv4 destination from a socket that takes no IPv4 */
            else if (s->family == AF_INET6 && s->bound && !s->m4) rc = -ENETUNREACH;
            else { int r = udp_send(s, buf, n, dst.a4, dst.port); rc = r ? r : (ssize_t)n; }
        }
    } else if (s->kind == SK_ICMP6) {
        SockAddr dst;
        bool have = to != NULL;
        if (have) dst = *to;
        else if (s->connected) { memset(&dst, 0, sizeof dst); dst.fam = AF_INET6; dst.a6 = s->raddr6; dst.scope = s->scope; have = true; }
        if (!have) rc = -EDESTADDRREQ;
        else if (dst.fam != AF_INET6) rc = -EAFNOSUPPORT;
        else {
            const In6 *hint = (s->m6 && !in6_is_any(&s->laddr6)) ? &s->laddr6 : NULL;
            int r = icmp6_send_echo_request(hint, &dst.a6, dst.scope ? dst.scope : s->scope, buf, n, s->icmp_ident);
            rc = r ? r : (ssize_t)n;
        }
    } else {
        u32 daddr = to ? to->a4 : 0;
        bool have_addr = to != NULL;
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
