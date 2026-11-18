/*
 * The outside of the IPv6 stack: the netctl commands that the tools use (addresses, routes, neighbours, the name servers routers announced, a card's IPv6 state) and the /proc files
 * that Linux programs read, in Linux's formats: /proc/net/if_inet6, /proc/net/ipv6_route, /proc/net/snmp6 (and tcp6 and udp6, which sock.c fills in), plus /proc/net/ndp (the
 * neighbour cache, for people), and /proc/net/inject_dev (a test hook: a frame delivered as if it had arrived on a named card).
 *
 * Written from the descriptions of the Linux files in proc(5) and the headers <linux/if_addr.h>, <linux/ipv6_route.h> and <linux/neighbour.h> for the numbers.
 */
#include "net6k.h"
#include "kernel.h"
#include "process.h"
#include "vfs.h"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

static u8 g6_scope(const In6 *a) {
    if (in6_is_loopback(a)) return G6S_LOOPBACK;
    if (in6_is_linklocal(a)) return G6S_LINKLOCAL;
    if (in6_scope(a) == IP6_SCOPE_SITE) return G6S_SITELOCAL;
    return G6S_GLOBAL;
}

static u8 g6_flags(const Addr6 *a) {
    u8 f = 0;
    if (a->flags & A6F_PERMANENT) f |= G6A_PERMANENT;
    if (a->state == A6_TENTATIVE) f |= G6A_TENTATIVE;
    if (a->state == A6_DEPRECATED) f |= G6A_DEPRECATED;
    if (a->state == A6_DUPLICATE) f |= G6A_DADFAILED;
    return f;
}

static u32 secs_left(u64 end, u64 now) {
    if (!end) return IP6_INFINITE;
    return end > now ? (u32)((end - now) / 1000) : 0;
}

/* ------------------------------------------------------------------ /proc */

static size_t gen_if_inet6(char *b, size_t cap) {
    size_t n = 0;
    NetDev *d;
    net_lock();
    for (int k = 0; (d = netdev_by_index(k)) != NULL; k++) {
        Ip6If *f = d->ip6;
        for (int i = 0; f && i < IP6_MAX_ADDRS && n + 120 < cap; i++) {
            Addr6 *a = &f->a[i];
            if (!a->state) continue;
            char hex[33];
            in6_hex(&a->addr, hex);
            n += (size_t)snprintf(b + n, cap - n, "%s %02x %02x %02x %02x %8s\n", hex, ip6_dev_scope(d), a->plen, g6_scope(&a->addr), g6_flags(a), d->name);
        }
    }
    net_unlock();
    return n;
}

static size_t gen_ipv6_route(char *b, size_t cap) {
    size_t n = 0;
    Route6 *r;
    net_lock();
    for (int i = 0; route6_list(i, &r) == 0 && n + 200 < cap; i++) {
        char dst[33], gw[33], zero[33];
        in6_hex(&r->dst, dst);
        in6_hex(&r->gw, gw);
        In6 z;
        in6_zero(&z);
        in6_hex(&z, zero);
        u32 flags = G6R_UP | ((r->flags & R6F_GATEWAY) ? G6R_GATEWAY : 0) | (r->plen == 128 ? G6R_HOST : 0);
        if (r->flags & R6F_RA) flags |= G6R_ADDRCONF | G6R_EXPIRES | (r->plen == 0 ? G6R_DEFAULT : 0);
        n += (size_t)snprintf(b + n, cap - n, "%s %02x %s 00 %s %08x %08x %08x %08x %8s\n", dst, r->plen, zero, gw, r->metric, 1u, 0u, flags, r->dev->name);
    }
    net_unlock();
    return n;
}

static size_t gen_snmp6(char *b, size_t cap) {
    const Ip6Stats *s = &g_ip6_stats;
    static const struct { const char *name; size_t off; } rows[] = {
#define C(n, f) { n, offsetof(Ip6Stats, f) }
        C("Ip6InReceives", in_receives), C("Ip6InHdrErrors", in_hdr_errors), C("Ip6InTruncatedPkts", in_truncated), C("Ip6InAddrErrors", in_addr_errors),
        C("Ip6InUnknownProtos", in_unknown_protos), C("Ip6InDiscards", in_discards), C("Ip6InDelivers", in_delivers), C("Ip6OutRequests", out_requests),
        C("Ip6OutNoRoutes", out_no_routes), C("Ip6ReasmReqds", reasm_reqds), C("Ip6ReasmOKs", reasm_oks), C("Ip6ReasmFails", reasm_fails), C("Ip6ReasmTimeout", reasm_timeouts),
        C("Ip6FragOKs", out_frag_ok), C("Ip6FragFails", out_frag_fails), C("Ip6FragCreates", out_frag_creates),
        C("Icmp6InMsgs", icmp_in_msgs), C("Icmp6InErrors", icmp_in_errors), C("Icmp6OutMsgs", icmp_out_msgs), C("Icmp6OutRateLimitHost", icmp_out_ratelimit),
        C("Icmp6InDestUnreachs", icmp_in_dest_unreachs), C("Icmp6InPktTooBigs", icmp_in_pkt_too_bigs), C("Icmp6InTimeExcds", icmp_in_time_excds), C("Icmp6InParmProblems", icmp_in_param_probs),
        C("Icmp6InEchos", icmp_in_echos), C("Icmp6InEchoReplies", icmp_in_echo_replies), C("Icmp6OutDestUnreachs", icmp_out_dest_unreachs), C("Icmp6OutPktTooBigs", icmp_out_pkt_too_bigs),
        C("Icmp6OutTimeExcds", icmp_out_time_excds), C("Icmp6OutParmProblems", icmp_out_param_probs), C("Icmp6OutEchoReplies", icmp_out_echo_replies),
        C("Icmp6InRouterSolicits", nd_in_rs), C("Icmp6InRouterAdvertisements", nd_in_ra), C("Icmp6InNeighborSolicits", nd_in_ns), C("Icmp6InNeighborAdvertisements", nd_in_na),
        C("Icmp6InRedirects", nd_in_redirects), C("Icmp6OutRouterSolicits", nd_out_rs), C("Icmp6OutNeighborSolicits", nd_out_ns), C("Icmp6OutNeighborAdvertisements", nd_out_na),
        C("Udp6InDatagrams", udp_in_datagrams), C("Udp6NoPorts", udp_no_ports), C("Udp6InErrors", udp_in_errors), C("Udp6OutDatagrams", udp_out_datagrams),
        C("Yukari6Rejected", bad), C("Yukari6NdRejected", nd_bad),
#undef C
    };
    size_t n = 0;
    net_lock();
    for (size_t i = 0; i < sizeof rows / sizeof rows[0] && n + 80 < cap; i++)
        n += (size_t)snprintf(b + n, cap - n, "%-32s\t%lu\n", rows[i].name, (unsigned long)*(const u64 *)((const u8 *)s + rows[i].off));
    net_unlock();
    return n;
}

static size_t gen_tcp6(char *b, size_t cap) { return net_proc_tcp6(b, cap); }
static size_t gen_udp6(char *b, size_t cap) { return net_proc_udp6(b, cap); }

/* /proc/net/inject_dev: the first 16 bytes name a card (NUL padded), the rest is one Ethernet frame, delivered as if that card had received it. Root only. */
static size_t gen_inject_help(char *b, size_t cap) { return (size_t)snprintf(b, cap, "write: 16 bytes of card name (NUL padded), then one raw Ethernet frame\n"); }

static ssize_t put_inject_dev(const char *buf, size_t n) {
    if (n < 16 + ETH_HLEN || n > 9000 + 16) return -EINVAL;
    if (current->proc && current->proc->cred.uid != 0) return -EPERM;
    char name[17];
    memcpy(name, buf, 16);
    name[16] = 0;
    NetBuf *nb = nb_alloc();
    if (!nb) return -ENOMEM;
    size_t take = MIN(n - 16, nb_room(nb));
    memcpy(nb_put(nb, take), buf + 16, take);
    net_lock();
    NetDev *d = netdev_find(name);
    if (d) net_rx(d, nb); else nb_free(nb);
    net_unlock();
    return d ? (ssize_t)n : -ENODEV;
}

/* ------------------------------------------------------------------ netctl */

static void addr_fill(NetDev *d, const Addr6 *a, struct gnet_addr6 *o, int index) {
    u64 now = net_now();
    memset(o, 0, sizeof *o);
    o->index = (u32)index;
    strlcpy(o->ifname, d->name, sizeof o->ifname);
    o->ifindex = ip6_dev_scope(d);
    memcpy(o->addr, a->addr.b, 16);
    o->plen = a->plen;
    o->scope = g6_scope(&a->addr);
    o->flags = g6_flags(a);
    o->valid = secs_left(a->valid_end, now);
    o->pref = secs_left(a->pref_end, now);
}

long net6_ctl(unsigned long cmd, void *arg, bool root, bool *handled) {
    *handled = true;
    switch (cmd) {
    case NETCTL_ADDR6_GET: {
        struct gnet_addr6 *r = arg;
        int want = (int)r->index, n = 0;
        NetDev *d;
        for (int k = 0; (d = netdev_by_index(k)) != NULL; k++) {
            Ip6If *f = d->ip6;
            for (int i = 0; f && i < IP6_MAX_ADDRS; i++) {
                if (!f->a[i].state) continue;
                if (n++ == want) { addr_fill(d, &f->a[i], r, want); return 0; }
            }
        }
        return -ENOENT;
    }
    case NETCTL_ADDR6_ADD: {
        struct gnet_addr6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        NetDev *d = netdev_find(r->ifname);
        if (!d) return -ENODEV;
        In6 a;
        memcpy(a.b, r->addr, 16);
        bool forever = !r->valid || r->valid == IP6_INFINITE;
        return ip6_addr_add(d, &a, r->plen ? r->plen : 64, forever ? A6F_PERMANENT : 0, forever ? IP6_INFINITE : r->valid, r->pref ? r->pref : IP6_INFINITE);
    }
    case NETCTL_ADDR6_DEL: {
        struct gnet_addr6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        NetDev *d = netdev_find(r->ifname);
        if (!d) return -ENODEV;
        In6 a;
        memcpy(a.b, r->addr, 16);
        return ip6_addr_del(d, &a);
    }
    case NETCTL_ROUTE6_GET: {
        struct gnet_route6 *r = arg;
        Route6 *x;
        int want = (int)r->index;
        if (route6_list(want, &x)) return -ENOENT;
        u64 now = net_now();
        memset(r, 0, sizeof *r);
        r->index = (u32)want;
        strlcpy(r->ifname, x->dev->name, sizeof r->ifname);
        memcpy(r->dst, x->dst.b, 16);
        memcpy(r->gw, x->gw.b, 16);
        r->plen = x->plen;
        r->proto = (x->flags & R6F_RA) ? 9 : (x->flags & R6F_CONNECTED) ? 2 : 3;
        r->metric = x->metric;
        r->flags = G6R_UP | ((x->flags & R6F_GATEWAY) ? G6R_GATEWAY : 0) | ((x->flags & R6F_RA) ? G6R_ADDRCONF | G6R_EXPIRES : 0) | (x->plen == 0 ? G6R_DEFAULT : 0);
        r->expires = x->expires ? (x->expires > now ? (u32)((x->expires - now) / 1000) : 0) : 0;
        return 0;
    }
    case NETCTL_ROUTE6_ADD: {
        struct gnet_route6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        In6 dst, gw;
        memcpy(dst.b, r->dst, 16);
        memcpy(gw.b, r->gw, 16);
        NetDev *d = r->ifname[0] ? netdev_find(r->ifname) : NULL;
        if (!d) {
            if (r->ifname[0]) return -ENODEV;
            Route6 *via = route6_lookup(in6_is_any(&gw) ? &dst : &gw);          /* the card that can reach the gateway */
            d = via ? via->dev : NULL;
            if (!d && in6_is_linklocal(&gw)) { int n = 0; for (NetDev *x = netdev_by_index(0); x; x = netdev_by_index(x->index + 1)) if (!(x->flags & NDF_LOOPBACK)) { d = x; n++; } if (n != 1) d = NULL; }
        }
        if (!d) return -ENETUNREACH;
        if (r->plen > 128) return -EINVAL;
        return route6_add(&dst, r->plen, in6_is_any(&gw) ? NULL : &gw, d, r->metric ? r->metric : 1024, R6F_STATIC, 0);
    }
    case NETCTL_ROUTE6_DEL: {
        struct gnet_route6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        In6 dst, gw;
        memcpy(dst.b, r->dst, 16);
        memcpy(gw.b, r->gw, 16);
        NetDev *d = r->ifname[0] ? netdev_find(r->ifname) : NULL;
        if (r->ifname[0] && !d) return -ENODEV;
        if (r->plen > 128) return -EINVAL;
        return route6_del(&dst, r->plen, d, in6_is_any(&gw) ? NULL : &gw);
    }
    case NETCTL_NBR6_GET: {
        struct gnet_nbr6 *r = arg;
        return nbr6_list((int)r->index, r);
    }
    case NETCTL_NBR6_SET: {
        struct gnet_nbr6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        NetDev *d = netdev_find(r->ifname);
        if (!d) return -ENODEV;
        In6 a;
        memcpy(a.b, r->addr, 16);
        return nbr6_set(d, &a, r->mac);
    }
    case NETCTL_NBR6_DEL: {
        struct gnet_nbr6 *r = arg;
        r->ifname[sizeof r->ifname - 1] = 0;
        if (!root) return -EPERM;
        NetDev *d = netdev_find(r->ifname);
        if (!d) return -ENODEV;
        In6 a;
        memcpy(a.b, r->addr, 16);
        return nbr6_del(d, &a);
    }
    case NETCTL_DNS6_GET: {
        struct gnet_dns6 *r = arg;
        In6 srv[4];
        size_t n = net6_dns_servers(srv, 4);
        u32 idx = r->index;
        if (idx >= n) return -ENOENT;
        memset(r, 0, sizeof *r);
        r->index = idx;
        memcpy(r->addr, srv[idx].b, 16);
        return 0;
    }
    case NETCTL_IF6_GET: {
        struct gnet_if6 *r = arg;
        NetDev *d = netdev_by_index((int)r->index);
        if (!d) return -ENODEV;
        Ip6If *f = d->ip6;
        memset(r, 0, sizeof *r);
        r->index = (u32)d->index;
        if (!f) return 0;
        r->mtu = f->off ? 0 : ip6_link_mtu(d);
        r->hop_limit = f->hop_limit;
        r->disabled = f->off;
        r->ra_seen = f->ra_seen;
        r->managed = f->managed;
        r->other = f->other;
        return 0;
    }
    default:
        *handled = false;
        return 0;
    }
}

/* ------------------------------------------------------------------ registration */

void ip6_netctl_init(void) {
    procfs_register("net/if_inet6", gen_if_inet6);
    procfs_register("net/ipv6_route", gen_ipv6_route);
    procfs_register("net/snmp6", gen_snmp6);
    procfs_register("net/tcp6", gen_tcp6);
    procfs_register("net/udp6", gen_udp6);
    procfs_register("net/ndp", nd6_proc);
    procfs_register_rw("net/inject_dev", gen_inject_help, put_inject_dev);
}
