/*
 * Neighbour discovery and address autoconfiguration for IPv6: the neighbour cache and its states, neighbour solicitation and advertisement, duplicate address detection, router
 * solicitation, router advertisements (default router, on-link prefixes, SLAAC addresses, the MTU, RDNSS name servers).
 *
 * Written from RFC 4861 (neighbour discovery: the message formats, section 6.3.4 for what a host does with a router advertisement, section 7 for the neighbour cache, the unreachability
 * detection and its constants), RFC 4862 (SLAAC, duplicate address detection, the lifetime rules of section 5.5.3) and RFC 8106 (the RDNSS option). A message is accepted only with
 * hop limit 255 (so it cannot have crossed a router), code 0 and well-formed options (an option of length 0 discards the whole message, section 7.1).
 */
#include "net6k.h"
#include "kernel.h"
#include "mm.h"
#include "random.h"
#include "vfs.h"
#include "process.h"

#define NBR6_MAX              64
#define NBR6_QUEUE            4
#define MAX_MULTICAST_SOLICIT 3
#define MAX_UNICAST_SOLICIT   3
#define DELAY_FIRST_PROBE_MS  5000
#define STALE_GC_MS           600000
#define DAD_TRANSMITS         1
#define MAX_RTR_SOLICITATIONS 3
#define RTR_SOLICITATION_MS   4000
#define MAX_RTR_DELAY_MS      1000
#define NA_PER_SECOND         100
#define MAX_RA_ROUTES         16

enum { NS_FREE = 0, NS_INCOMPLETE, NS_REACHABLE, NS_STALE, NS_DELAY, NS_PROBE, NS_PERMANENT };

typedef struct Nbr6 {
    u8      state;
    bool    router;
    u8      probes;
    In6     addr;
    NetDev *dev;
    u8      mac[6];
    u64     timer, used;
    NetBuf *q[NBR6_QUEUE];
} Nbr6;

static Nbr6 g_nbr[NBR6_MAX];

static const In6 g_all_nodes = { { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
static const In6 g_all_routers = { { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } };

/* What happens to the addresses of a card (ready, made from an advertisement) goes into the in-memory log only (dmesg): it comes a few seconds after the boot, at a time nobody
 * chooses, and must not break into the output of a program that is running on the console by then. Trouble (a duplicate address, IPv6 switched off) is on the console. */
static void say_addr(NetDev *d, const char *what, const Addr6 *a) {
    char t[48];
    in6_str(&a->addr, t, sizeof t);
    klog_quiet("Yukari", "%s: IPv6 address %s/%u %s", d->name, t, a->plen, what);
}

/* ------------------------------------------------------------------ the neighbour cache */

static Nbr6 *nbr_find(NetDev *d, const In6 *a) {
    for (int i = 0; i < NBR6_MAX; i++)
        if (g_nbr[i].state && g_nbr[i].dev == d && in6_eq(&g_nbr[i].addr, a)) return &g_nbr[i];
    return NULL;
}

/* A slot for a new entry. A free one; else the entry nobody has used for the longest time, among the STALE ones (an entry that only a received packet made is STALE, so a flood of
 * solicitations from forged addresses can only ever replace what it made itself) and, if the entry is for a packet we want to send, among the REACHABLE ones too. Pending
 * resolutions, probes in progress and permanent entries are never taken. */
static Nbr6 *nbr_slot(bool from_rx) {
    Nbr6 *best = NULL;
    for (int i = 0; i < NBR6_MAX; i++) {
        Nbr6 *e = &g_nbr[i];
        if (!e->state) return e;
        bool ok = e->state == NS_STALE || (!from_rx && e->state == NS_REACHABLE);
        if (ok && (!best || e->used < best->used || (best->state == NS_REACHABLE && e->state == NS_STALE))) best = e;
    }
    if (best) {
        for (int j = 0; j < NBR6_QUEUE; j++) { nb_free(best->q[j]); best->q[j] = NULL; }
        memset(best, 0, sizeof *best);
    }
    return best;
}

static void nbr_unlink(Nbr6 *e) {
    for (int j = 0; j < NBR6_QUEUE; j++) { nb_free(e->q[j]); e->q[j] = NULL; }
    memset(e, 0, sizeof *e);
}

/* Resolution failed: whoever was waiting for the packets that were queued hears "no route to host" (as with ARP), then the entry goes. */
static void nbr_fail(Nbr6 *e) {
    for (int j = 0; j < NBR6_QUEUE; j++) {
        NetBuf *q = e->q[j];
        if (q && q->len >= IP6_HLEN + 4 && (nb_data(q)[0] >> 4) == 6) {
            const u8 *ip = nb_data(q);
            const In6 *src = (const In6 *)(ip + 8), *dst = (const In6 *)(ip + 24);
            u16 sport = rd16(ip + IP6_HLEN), dport = rd16(ip + IP6_HLEN + 2);
            if (ip[6] == IP6P_TCP) tcp6_error(src, sport, dst, dport, EHOSTUNREACH);
            else if (ip[6] == IP6P_UDP) udp6_error(src, sport, dst, dport, EHOSTUNREACH);
        }
        nb_free(q);
        e->q[j] = NULL;
    }
    memset(e, 0, sizeof *e);
}

static u32 reach_ms(NetDev *d) { Ip6If *f = d->ip6; return f && f->reachable_ms ? f->reachable_ms : 30000; }
static u32 retrans_ms(NetDev *d) { Ip6If *f = d->ip6; return f && f->retrans_ms ? f->retrans_ms : 1000; }

/* ------------------------------------------------------------------ sending the messages */

static void send_ns(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, const In6 *target, bool slla) {
    u8 body[4 + 16 + 8];
    size_t n = 20;
    wr32(body, 0);
    memcpy(body + 4, target->b, 16);
    if (slla) { body[n++] = ND_OPT_SLLA; body[n++] = 1; memcpy(body + n, d->mac, 6); n += 6; }
    g_ip6_stats.nd_out_ns++;
    icmp6_send_on(d, src, dst, dmac, ND_NEIGHBOR_SOLICIT, 0, body, n, 255);
}

static bool na_allowed(void) {                                             /* neighbour advertisements are rate limited, so a flood of solicitations cannot make this host a reflector */
    static u64 window;
    static int sent;
    u64 now = net_now();
    if (now - window >= 1000) { window = now; sent = 0; }
    return sent++ < NA_PER_SECOND;
}

static void send_na(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, const In6 *target, u8 flags) {
    u8 body[4 + 16 + 8];
    body[0] = flags; body[1] = body[2] = body[3] = 0;
    memcpy(body + 4, target->b, 16);
    body[20] = ND_OPT_TLLA; body[21] = 1;
    memcpy(body + 22, d->mac, 6);
    g_ip6_stats.nd_out_na++;
    icmp6_send_on(d, src, dst, dmac, ND_NEIGHBOR_ADVERT, 0, body, sizeof body, 255);
}

/* The source for a solicitation that resolves e->addr: the source of the packet that waits for it if that is an address of the card (RFC 4861 section 7.2.2), else the best one. */
static const In6 *ns_source(NetDev *d, const Nbr6 *e) {
    for (int j = 0; j < NBR6_QUEUE; j++) {
        NetBuf *q = e->q[j];
        if (!q || q->len < IP6_HLEN) continue;
        const In6 *s = (const In6 *)(nb_data(q) + 8);
        Addr6 *a = ip6_addr_on(d, s);
        if (a && (a->state == A6_PREFERRED || a->state == A6_DEPRECATED)) return &a->addr;
    }
    Addr6 *a = ip6_src_select(d, &e->addr);
    return a ? &a->addr : NULL;
}

static void nbr_solicit(Nbr6 *e, bool unicast) {
    NetDev *d = e->dev;
    const In6 *src = ns_source(d, e);
    if (!src) return;
    In6 dst;
    if (unicast) dst = e->addr; else in6_solicited_node(&dst, &e->addr);
    send_ns(d, src, &dst, unicast ? e->mac : NULL, &e->addr, true);
}

/* ------------------------------------------------------------------ resolving and sending */

int nd6_output(NetDev *d, const In6 *nexthop, NetBuf *nb) {
    u8 dmac[6];
    if (d->flags & NDF_NOARP) { memset(dmac, 0, 6); return eth_send(d, dmac, ETH_P_IPV6, nb); }
    if (in6_is_mcast(nexthop)) { in6_mcast_mac(dmac, nexthop); return eth_send(d, dmac, ETH_P_IPV6, nb); }
    u64 now = net_now();
    Nbr6 *e = nbr_find(d, nexthop);
    if (e) {
        if (e->state == NS_INCOMPLETE) {
            if (e->q[NBR6_QUEUE - 1]) { nb_free(e->q[0]); for (int i = 0; i < NBR6_QUEUE - 1; i++) e->q[i] = e->q[i + 1]; e->q[NBR6_QUEUE - 1] = NULL; }
            for (int i = 0; i < NBR6_QUEUE; i++) if (!e->q[i]) { e->q[i] = nb; return 0; }
            nb_free(nb);
            return 0;
        }
        if (e->state == NS_STALE) { e->state = NS_DELAY; e->probes = 0; e->timer = now + DELAY_FIRST_PROBE_MS; }      /* used again: if nothing confirms it soon, probe */
        e->used = now;
        memcpy(dmac, e->mac, 6);
        return eth_send(d, dmac, ETH_P_IPV6, nb);
    }
    e = nbr_slot(false);
    if (!e) { nb_free(nb); return -ENOBUFS; }
    memset(e, 0, sizeof *e);
    e->addr = *nexthop;
    e->dev = d;
    e->state = NS_INCOMPLETE;
    e->used = now;
    e->q[0] = nb;
    if (!ns_source(d, e)) { nbr_fail(e); return -EADDRNOTAVAIL; }          /* nothing to send the solicitation from */
    e->probes = 1;
    e->timer = now + retrans_ms(d);
    nbr_solicit(e, false);
    return 0;
}

/* A neighbour's link-layer address is learned from a solicitation or an advertisement: an existing entry with another address is updated and made STALE, a missing one is created
 * STALE (RFC 4861 section 7.2.3), unless the cache has no room that a received packet may take. */
static void nbr_learn(NetDev *d, const In6 *a, const u8 *mac, bool router) {
    Nbr6 *e = nbr_find(d, a);
    u64 now = net_now();
    if (!e) {
        e = nbr_slot(true);
        if (!e) return;
        memset(e, 0, sizeof *e);
        e->addr = *a;
        e->dev = d;
        e->state = NS_STALE;
        e->used = now;
        memcpy(e->mac, mac, 6);
        e->router = router;
        return;
    }
    if (e->state == NS_PERMANENT) return;
    if (e->state == NS_INCOMPLETE) {                                       /* the address we waited for: send what was queued */
        memcpy(e->mac, mac, 6);
        e->state = NS_STALE;
        e->router = router;
        for (int j = 0; j < NBR6_QUEUE; j++) {
            NetBuf *q = e->q[j];
            e->q[j] = NULL;
            if (q) eth_send(d, e->mac, ETH_P_IPV6, q);
        }
        e->state = NS_DELAY;
        e->probes = 0;
        e->timer = now + DELAY_FIRST_PROBE_MS;
        return;
    }
    if (memcmp(e->mac, mac, 6)) { memcpy(e->mac, mac, 6); e->state = NS_STALE; }
    if (router) e->router = true;
}

/* ------------------------------------------------------------------ addresses: duplicate address detection */

void nd6_addr_added(NetDev *d, Addr6 *a) {
    (void)d;
    a->dad_sent = 0;
    a->dad_at = net_now() + random_u64() % MAX_RTR_DELAY_MS;               /* a random delay before the first probe: machines that start together do not probe together */
}

static void dad_failed(NetDev *d, Addr6 *a) {
    Ip6If *f = d->ip6;
    a->state = A6_DUPLICATE;
    In6 pfx;
    in6_mask(&pfx, &a->addr, a->plen);
    bool shared = false;
    for (int i = 0; f && i < IP6_MAX_ADDRS; i++) if (&f->a[i] != a && f->a[i].state && f->a[i].plen == a->plen && in6_prefix_eq(&f->a[i].addr, &pfx, a->plen)) shared = true;
    if (!shared) route6_del(&pfx, a->plen, d, NULL);
    char t[48];
    in6_str(&a->addr, t, sizeof t);
    klog("Yukari", "%s: duplicate address detected for %s: another node on the link has it, so it is not used", d->name, t);
    if ((a->flags & A6F_LINKLOCAL) && f) {                                 /* RFC 4862 section 5.4.5: the interface identifier is not unique, so IPv6 is switched off on the card */
        f->off = true;
        route6_flush_dev(d, 0xFF);
        klog("Yukari", "%s: IPv6 is switched off on this card (its link-local address is a duplicate)", d->name);
    }
}

static void dad_done(NetDev *d, Addr6 *a) {
    Ip6If *f = d->ip6;
    u64 now = net_now();
    a->state = (a->pref_end && now >= a->pref_end) ? A6_DEPRECATED : A6_PREFERRED;
    say_addr(d, a->state == A6_DEPRECATED ? "is ready (deprecated)" : "is ready", a);
    if ((a->flags & A6F_LINKLOCAL) && f && !f->ra_seen && !f->rs_at) {      /* a router may be there: ask for an advertisement after a short random delay (RFC 4861 section 6.3.7) */
        f->rs_sent = 0;
        f->rs_at = now + random_u64() % MAX_RTR_DELAY_MS;
    }
}

/* ------------------------------------------------------------------ the link */

static void flush_dev(NetDev *d) {
    for (int i = 0; i < NBR6_MAX; i++)
        if (g_nbr[i].state && g_nbr[i].dev == d && g_nbr[i].state != NS_PERMANENT) nbr_unlink(&g_nbr[i]);
}

/* What came from the network that is gone: the neighbours, the router's routes, the addresses made from its prefixes, its parameters. */
static void forget_network(NetDev *d) {
    Ip6If *f = d->ip6;
    flush_dev(d);
    route6_flush_dev(d, R6F_RA);
    if (!f) return;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) if (f->a[i].state && (f->a[i].flags & A6F_SLAAC)) ip6_addr_del(d, &f->a[i].addr);
    f->ra_seen = false;
    f->rs_at = 0;
    f->rs_sent = 0;
    f->mtu = 0;
    f->hop_limit = 64;
    f->reachable_ms = 30000;
    f->retrans_ms = 1000;
}

static void make_link_local(NetDev *d) {
    In6 pfx, ll;
    in6_zero(&pfx);
    pfx.b[0] = 0xFE; pfx.b[1] = 0x80;
    in6_eui64(&ll, &pfx, d->mac);
    ip6_addr_add(d, &ll, 64, A6F_LINKLOCAL | A6F_PERMANENT, IP6_INFINITE, IP6_INFINITE);
}

void nd6_link(NetDev *d, bool up) {
    if (d->flags & NDF_LOOPBACK) return;
    Ip6If *f = ip6_if_make(d);
    if (!f) return;
    if (!up) { forget_network(d); return; }
    if (f->off) {                                                          /* a new network may have a different neighbour: try again from the start */
        f->off = false;
        for (int i = 0; i < IP6_MAX_ADDRS; i++) if (f->a[i].state == A6_DUPLICATE) ip6_addr_del(d, &f->a[i].addr);
    }
    bool have_ll = false;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) if (f->a[i].state && (f->a[i].flags & A6F_LINKLOCAL)) have_ll = true;
    if (!have_ll) make_link_local(d);
    for (int i = 0; i < IP6_MAX_ADDRS; i++) {                              /* duplicate address detection runs again for what stayed (RFC 4862 section 5.4.2) */
        Addr6 *a = &f->a[i];
        if (!a->state || a->state == A6_DUPLICATE) continue;
        a->state = A6_TENTATIVE;
        nd6_addr_added(d, a);
    }
}

void nd6_if_down(NetDev *d) {
    if (d->flags & NDF_LOOPBACK) return;
    forget_network(d);
}

void nd6_dev_added(NetDev *d) {
    ip6_dev_added(d);
    if (!(d->flags & NDF_LOOPBACK) && (d->flags & NDF_LINK)) nd6_link(d, true);
}

/* ------------------------------------------------------------------ RDNSS: the name servers the routers announce (RFC 8106) */

#define RDNSS_MAX 3
static struct { bool used; In6 addr; u64 expires; NetDev *dev; } g_rdnss[RDNSS_MAX];
static bool g_resolv_dirty;
static u64 g_resolv_check;

static void rdnss_set(NetDev *d, const In6 *a, u32 lifetime) {
    int slot = -1;
    u64 now = net_now();
    for (int i = 0; i < RDNSS_MAX; i++) {
        if (g_rdnss[i].used && now >= g_rdnss[i].expires) g_rdnss[i].used = false;
        if (g_rdnss[i].used && g_rdnss[i].dev == d && in6_eq(&g_rdnss[i].addr, a)) { slot = i; break; }
    }
    if (lifetime == 0) { if (slot >= 0) { g_rdnss[slot].used = false; g_resolv_dirty = true; } return; }
    if (slot < 0) {
        for (int i = 0; i < RDNSS_MAX && slot < 0; i++) if (!g_rdnss[i].used) slot = i;
        if (slot < 0) return;                                              /* three servers are enough; a fourth waits until one expires */
        g_resolv_dirty = true;
    }
    g_rdnss[slot].used = true;
    g_rdnss[slot].addr = *a;
    g_rdnss[slot].dev = d;
    g_rdnss[slot].expires = lifetime == IP6_INFINITE ? ~0ULL : now + (u64)lifetime * 1000;
}

size_t net6_dns_servers(In6 *out, size_t max) {
    size_t n = 0;
    u64 now = net_now();
    for (int i = 0; i < RDNSS_MAX && n < max; i++)
        if (g_rdnss[i].used && now < g_rdnss[i].expires && !in6_is_linklocal(&g_rdnss[i].addr)) out[n++] = g_rdnss[i].addr;      /* (the resolver has no scope for a link-local server) */
    return n;
}

void net6_resolv_changed(void) { g_resolv_dirty = true; }

#define RESOLV_MARK " # RDNSS"

/* /etc/resolv.conf with the IPv6 servers in it: whatever else the file says stays (the DHCP client's or netconf's lines), the lines this code wrote before are replaced. */
static void resolv_update(void) {
    In6 srv[RDNSS_MAX];
    size_t ns = net6_dns_servers(srv, RDNSS_MAX);
    char *old = NULL;
    size_t olen = 0;
    void *data = NULL;
    if (vfs_read_file("/etc/resolv.conf", &data, &olen) == 0) old = data;
    char *out = kmalloc(olen + 3 * 80 + 8);
    if (!out) { kfree(old); return; }
    size_t n = 0;
    bool had_ours = false;
    for (size_t pos = 0; pos < olen;) {                                     /* the old lines, except ours */
        size_t e = pos;
        while (e < olen && old[e] != '\n') e++;
        size_t ll = e - pos;
        bool ours = false;
        for (size_t k = 0; k + sizeof RESOLV_MARK - 1 <= ll; k++) if (!memcmp(old + pos + k, RESOLV_MARK, sizeof RESOLV_MARK - 1)) ours = true;
        if (ours) had_ours = true;
        else { memcpy(out + n, old + pos, ll); n += ll; out[n++] = '\n'; }
        pos = e < olen ? e + 1 : e;
    }
    for (size_t i = 0; i < ns; i++) {
        char t[48];
        in6_str(&srv[i], t, sizeof t);
        n += (size_t)snprintf(out + n, 80, "nameserver %s%s\n", t, RESOLV_MARK);
    }
    bool changed = ns > 0 || had_ours;
    if (ns && old && n == olen && !memcmp(out, old, n)) changed = false;   /* the file already says exactly this */
    if (changed && (ns || old)) vfs_write_file("/etc/resolv.conf", out, n, 0644);
    kfree(out);
    kfree(old);
}

/* ------------------------------------------------------------------ messages in */

static void handle_ns(NetDev *d, const In6 *src, const In6 *dst, const u8 *m, const NdOpts *o) {
    (void)dst;
    const In6 *target = (const In6 *)(m + 8);
    Addr6 *a = ip6_addr_on(d, target);                                      /* the target must be an address of the card the message came in on */
    if (!a || a->state == A6_DUPLICATE) return;
    g_ip6_stats.nd_in_ns++;
    if (a->state == A6_TENTATIVE) {
        if (in6_is_any(src)) dad_failed(d, a);                             /* another node probes for the address we are probing for: neither may have it */
        return;                                                            /* a solicitation for a tentative address is never answered (RFC 4862 section 5.4.3) */
    }
    if (in6_is_any(src)) {                                                 /* somebody else's probe for an address we hold: tell everybody that it is taken */
        if (na_allowed()) send_na(d, &a->addr, &g_all_nodes, NULL, target, 0x20);
        return;
    }
    if (o->slla) nbr_learn(d, src, o->slla, false);
    if (!na_allowed()) return;
    send_na(d, &a->addr, src, o->slla, target, 0x60);                      /* solicited, override */
}

static void handle_na(NetDev *d, const In6 *src, const In6 *dst, const u8 *m, const NdOpts *o) {
    (void)src; (void)dst;
    const In6 *target = (const In6 *)(m + 8);
    u8 flags = m[4];
    Addr6 *a = ip6_addr_on(d, target);
    if (a && a->state == A6_TENTATIVE) { dad_failed(d, a); return; }       /* somebody answers for the address we are probing: it is taken */
    if (a) return;
    g_ip6_stats.nd_in_na++;
    Nbr6 *e = nbr_find(d, target);
    if (!e || e->state == NS_PERMANENT) return;
    bool S = (flags & 0x40) != 0, R = (flags & 0x80) != 0, O = (flags & 0x20) != 0;
    u64 now = net_now();
    if (e->state == NS_INCOMPLETE) {
        if (!o->tlla) return;
        memcpy(e->mac, o->tlla, 6);
        e->router = R;
        e->state = S ? NS_REACHABLE : NS_STALE;
        e->timer = S ? now + reach_ms(d) : 0;
        e->probes = 0;
        for (int j = 0; j < NBR6_QUEUE; j++) {
            NetBuf *q = e->q[j];
            e->q[j] = NULL;
            if (q) eth_send(d, e->mac, ETH_P_IPV6, q);
        }
        return;
    }
    bool differs = o->tlla && memcmp(e->mac, o->tlla, 6);
    if (!O && differs) {                                                   /* an advertisement that does not override: only a reachable entry is affected */
        if (e->state == NS_REACHABLE) { e->state = NS_STALE; e->timer = 0; }
        return;
    }
    if (differs) memcpy(e->mac, o->tlla, 6);
    if (S) { e->state = NS_REACHABLE; e->timer = now + reach_ms(d); e->probes = 0; }
    else if (differs) { e->state = NS_STALE; e->timer = 0; }
    if (e->router && !R) route6_del(&(In6){ { 0 } }, 0, d, target);        /* it stopped being a router: its default route goes */
    e->router = R;
}

static void slaac_prefix(NetDev *d, Ip6If *f, const In6 *pfx, u32 valid, u32 pref) {
    In6 addr;
    in6_eui64(&addr, pfx, d->mac);
    Addr6 *a = ip6_addr_on(d, &addr);
    u64 now = net_now();
    if (!a) {
        if (valid == 0) return;
        if (ip6_addr_add(d, &addr, 64, A6F_SLAAC, valid, pref) == 0) {
            Addr6 *n = ip6_addr_on(d, &addr);
            if (n) say_addr(d, "was made from a router advertisement (tentative)", n);
        }
        return;
    }
    if (!(a->flags & A6F_SLAAC)) return;
    (void)f;
    u64 remaining = a->valid_end ? (a->valid_end > now ? (a->valid_end - now) / 1000 : 0) : IP6_INFINITE;
    if (valid > 7200 || valid > remaining) a->valid_end = valid == IP6_INFINITE ? 0 : now + (u64)valid * 1000;       /* RFC 4862 section 5.5.3 (e): an advertisement may lengthen the lifetime, and may only shorten it to two hours */
    else if (remaining > 7200) a->valid_end = now + 7200000ULL;
    a->pref_end = pref == IP6_INFINITE ? 0 : now + (u64)pref * 1000;
    if (a->state == A6_DEPRECATED && pref > 0) a->state = A6_PREFERRED;
}

static int count_ra_routes(void) {
    int n = 0;
    Route6 *r;
    for (int i = 0; route6_list(i, &r) == 0; i++) if (r->flags & R6F_RA) n++;
    return n;
}

static void handle_ra(NetDev *d, const In6 *src, const u8 *m, const NdOpts *o) {
    Ip6If *f = ip6_if_make(d);
    if (!f) return;
    g_ip6_stats.nd_in_ra++;
    u64 now = net_now();
    u8 flags = m[5];
    u16 lifetime = rd16(m + 6);
    u32 reach = rd32(m + 8), retrans = rd32(m + 12);
    f->ra_seen = true;
    f->rs_at = 0;
    f->managed = (flags & 0x80) != 0;
    f->other = (flags & 0x40) != 0;
    if (m[4]) f->hop_limit = m[4];
    if (reach) f->reachable_ms = reach < 100 ? 100 : reach > 3600000 ? 3600000 : reach;
    if (retrans) f->retrans_ms = retrans < 100 ? 100 : retrans > 60000 ? 60000 : retrans;
    In6 zero;
    in6_zero(&zero);
    u32 prf = (flags >> 3) & 3;
    u32 metric = prf == 1 ? 512 : prf == 3 ? 2048 : 1024;                  /* the router's preference (RFC 4191): high, low, medium (and the reserved value, which counts as medium) */
    if (lifetime) {
        if (count_ra_routes() < MAX_RA_ROUTES) route6_add(&zero, 0, src, d, metric, R6F_RA, now + (u64)lifetime * 1000);
    } else {
        route6_del(&zero, 0, d, src);
    }
    if (o->slla) nbr_learn(d, src, o->slla, true);
    else { Nbr6 *e = nbr_find(d, src); if (e) e->router = true; }
    if (o->mtu) {
        u32 mtu = rd32(o->mtu + 4);
        if (mtu >= IP6_MIN_MTU && mtu <= d->mtu) f->mtu = mtu;
    }
    for (int i = 0; i < o->nprefix; i++) {
        const u8 *po = o->prefix[i];
        u8 plen = po[2], pflags = po[3];
        u32 valid = rd32(po + 4), pref = rd32(po + 8);
        const In6 *pfx_raw = (const In6 *)(po + 16);
        if (plen > 128 || in6_is_linklocal(pfx_raw) || in6_is_mcast(pfx_raw) || pref > valid) continue;
        In6 pfx;
        in6_mask(&pfx, pfx_raw, plen);
        if (pflags & 0x80) {                                               /* on-link: a route (the prefix of an address of ours has its own) */
            bool ours = false;
            for (int k = 0; k < IP6_MAX_ADDRS; k++) if (f->a[k].state && f->a[k].plen == plen && in6_prefix_eq(&f->a[k].addr, &pfx, plen)) ours = true;
            if (!ours) {
                if (valid == 0) route6_del(&pfx, plen, d, NULL);
                else if (count_ra_routes() < MAX_RA_ROUTES) route6_add(&pfx, plen, NULL, d, 256, R6F_RA, valid == IP6_INFINITE ? 0 : now + (u64)valid * 1000);
            }
        }
        if ((pflags & 0x40) && plen == 64) slaac_prefix(d, f, &pfx, valid, pref);          /* autonomous: an address (only for /64: the interface identifier is 64 bits) */
    }
    for (int i = 0; i < o->nrdnss; i++) {
        const u8 *ro = o->rdnss[i];
        u32 life = rd32(ro + 4);
        int n = (ro[1] - 1) / 2;
        for (int k = 0; k < n; k++) {
            const In6 *srv = (const In6 *)(ro + 8 + 16 * k);
            if (in6_is_any(srv) || in6_is_mcast(srv)) continue;
            rdnss_set(d, srv, life);
        }
    }
}

void nd6_input(NetDev *d, const u8 *pkt, size_t pktlen, u32 uoff, bool to_tentative) {
    (void)to_tentative;
    if (d->flags & NDF_LOOPBACK) return;                                   /* there is no neighbour discovery on the loopback */
    const In6 *src = (const In6 *)(pkt + 8), *dst = (const In6 *)(pkt + 24);
    const u8 *m = pkt + uoff;
    size_t len = pktlen - uoff;
    NdOpts o;
    if (pkt[7] != 255 || m[1] != 0) goto bad;                               /* it must have been sent on this link: a router decrements the hop limit */
    switch (m[0]) {
    case ND_NEIGHBOR_SOLICIT:
        if (len < 24 || in6_is_mcast((const In6 *)(m + 8)) || !nd_opts_parse(m + 24, len - 24, &o)) goto bad;
        if (in6_is_any(src) && (!in6_is_solicited_node(dst) || o.slla)) goto bad;
        handle_ns(d, src, dst, m, &o);
        return;
    case ND_NEIGHBOR_ADVERT:
        if (len < 24 || in6_is_mcast((const In6 *)(m + 8)) || !nd_opts_parse(m + 24, len - 24, &o)) goto bad;
        if (in6_is_mcast(dst) && (m[4] & 0x40)) goto bad;                  /* a solicited advertisement to a multicast address makes no sense */
        handle_na(d, src, dst, m, &o);
        return;
    case ND_ROUTER_ADVERT:
        if (len < 16 || !in6_is_linklocal(src) || !nd_opts_parse(m + 16, len - 16, &o)) goto bad;
        handle_ra(d, src, m, &o);
        return;
    case ND_ROUTER_SOLICIT:                                                 /* this host is not a router */
        g_ip6_stats.nd_in_rs++;
        return;
    default:                                                                /* redirects: not followed (the default router is used) */
        g_ip6_stats.nd_in_redirects++;
        return;
    }
bad:
    g_ip6_stats.nd_bad++;
    g_ip6_stats.bad++;
}

/* ------------------------------------------------------------------ timers */

static void send_rs(NetDev *d, Ip6If *f) {
    In6 src;
    bool have = false;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) {
        Addr6 *a = &f->a[i];
        if ((a->flags & A6F_LINKLOCAL) && (a->state == A6_PREFERRED || a->state == A6_DEPRECATED)) { src = a->addr; have = true; break; }
    }
    if (!have) in6_zero(&src);
    u8 body[4 + 8];
    size_t n = 4;
    wr32(body, 0);
    if (have) { body[n++] = ND_OPT_SLLA; body[n++] = 1; memcpy(body + n, d->mac, 6); n += 6; }
    g_ip6_stats.nd_out_rs++;
    icmp6_send_on(d, &src, &g_all_routers, NULL, ND_ROUTER_SOLICIT, 0, body, n, 255);
}

void nd6_tick(u64 now) {
    for (int i = 0; i < NBR6_MAX; i++) {
        Nbr6 *e = &g_nbr[i];
        if (!e->state || e->state == NS_PERMANENT) continue;
        switch (e->state) {
        case NS_INCOMPLETE:
            if (now < e->timer) break;
            if (e->probes >= MAX_MULTICAST_SOLICIT) { nbr_fail(e); break; }
            e->probes++;
            e->timer = now + retrans_ms(e->dev);
            nbr_solicit(e, false);
            break;
        case NS_REACHABLE:
            if (e->timer && now >= e->timer) e->state = NS_STALE;
            break;
        case NS_DELAY:
            if (now < e->timer) break;
            e->state = NS_PROBE;
            e->probes = 1;
            e->timer = now + retrans_ms(e->dev);
            nbr_solicit(e, true);
            break;
        case NS_PROBE:
            if (now < e->timer) break;
            if (e->probes >= MAX_UNICAST_SOLICIT) {                        /* it does not answer: the entry goes (a router's default route stays until its lifetime ends) */
                nbr_unlink(e);
                break;
            }
            e->probes++;
            e->timer = now + retrans_ms(e->dev);
            nbr_solicit(e, true);
            break;
        case NS_STALE:
            if (now - e->used > STALE_GC_MS) nbr_unlink(e);
            break;
        default: break;
        }
    }
    NetDev *d;
    for (int k = 0; (d = netdev_by_index(k)) != NULL; k++) {
        Ip6If *f = d->ip6;
        if (!f || (d->flags & NDF_LOOPBACK) || f->off) continue;
        if (!(d->flags & NDF_UP) || !(d->flags & NDF_LINK)) continue;
        for (int i = 0; i < IP6_MAX_ADDRS; i++) {                           /* duplicate address detection */
            Addr6 *a = &f->a[i];
            if (a->state != A6_TENTATIVE || now < a->dad_at) continue;
            if (a->dad_sent < DAD_TRANSMITS) {
                In6 dst, unspec;
                in6_zero(&unspec);
                in6_solicited_node(&dst, &a->addr);
                a->dad_sent++;
                a->dad_at = now + retrans_ms(d);
                send_ns(d, &unspec, &dst, NULL, &a->addr, false);
            } else {
                dad_done(d, a);
            }
        }
        if (f->rs_at && now >= f->rs_at && !f->ra_seen) {                   /* router solicitations */
            if (f->rs_sent >= MAX_RTR_SOLICITATIONS) {
                f->rs_at = 0;
            } else {
                f->rs_sent++;
                f->rs_at = now + RTR_SOLICITATION_MS;
                send_rs(d, f);
            }
        }
    }
    for (int i = 0; i < RDNSS_MAX; i++)
        if (g_rdnss[i].used && now >= g_rdnss[i].expires) { g_rdnss[i].used = false; g_resolv_dirty = true; }
    bool any = false;
    for (int i = 0; i < RDNSS_MAX; i++) if (g_rdnss[i].used) any = true;
    if (g_init_proc && (g_resolv_dirty || (any && now >= g_resolv_check))) {          /* the file is looked at every half minute while servers are known: netconf may have rewritten it */
        g_resolv_dirty = false;
        g_resolv_check = now + 30000;
        resolv_update();
    }
}

/* ------------------------------------------------------------------ the cache for the tools and /proc */

int nbr6_list(int index, struct gnet_nbr6 *out) {
    int n = 0;
    for (int i = 0; i < NBR6_MAX; i++) {
        Nbr6 *e = &g_nbr[i];
        if (!e->state) continue;
        if (n++ != index) continue;
        memset(out, 0, sizeof *out);
        out->index = (u32)index;
        strlcpy(out->ifname, e->dev->name, sizeof out->ifname);
        memcpy(out->addr, e->addr.b, 16);
        memcpy(out->mac, e->mac, 6);
        out->state = e->state == NS_INCOMPLETE ? G6N_INCOMPLETE : e->state == NS_REACHABLE ? G6N_REACHABLE : e->state == NS_STALE ? G6N_STALE : e->state == NS_DELAY ? G6N_DELAY
                     : e->state == NS_PROBE ? G6N_PROBE : G6N_PERMANENT;
        out->router = e->router;
        return 0;
    }
    return -ENOENT;
}

int nbr6_set(NetDev *d, const In6 *addr, const u8 *mac) {
    if (in6_is_mcast(addr) || in6_is_any(addr)) return -EINVAL;
    Nbr6 *e = nbr_find(d, addr);
    if (!e) {
        e = nbr_slot(false);
        if (!e) return -ENOSPC;
        memset(e, 0, sizeof *e);
        e->addr = *addr;
        e->dev = d;
    } else {
        for (int j = 0; j < NBR6_QUEUE; j++) { nb_free(e->q[j]); e->q[j] = NULL; }
    }
    memcpy(e->mac, mac, 6);
    e->state = NS_PERMANENT;
    e->used = net_now();
    return 0;
}

int nbr6_del(NetDev *d, const In6 *addr) {
    Nbr6 *e = nbr_find(d, addr);
    if (!e) return -ENOENT;
    nbr_unlink(e);
    return 0;
}

size_t nd6_proc(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "address                                  lladdr             dev    state      router\n");
    static const char *names[] = { "", "INCOMPLETE", "REACHABLE", "STALE", "DELAY", "PROBE", "PERMANENT" };
    net_lock();
    for (int i = 0; i < NBR6_MAX && n + 120 < cap; i++) {
        Nbr6 *e = &g_nbr[i];
        if (!e->state) continue;
        char t[48];
        in6_str(&e->addr, t, sizeof t);
        n += (size_t)snprintf(b + n, cap - n, "%-40s %02x:%02x:%02x:%02x:%02x:%02x  %-6s %-10s %d\n", t, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->dev->name,
                              names[e->state], e->router);
    }
    net_unlock();
    return n;
}
