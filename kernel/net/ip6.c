/*
 * IPv6: the addresses of a card, the routes and the path MTU cache, source and next-hop selection, the receive path (header, extension headers, reassembly), the send path
 * (checksums, fragmentation). ICMPv6 is icmp6.c, neighbour discovery and autoconfiguration ndp.c, the parts that need no machine ip6_proto.c; docs/NETWORK.md, "IPv6".
 *
 * Written from RFC 8200 (the protocol, sections 3 to 5; the pseudo-header, section 8.1), RFC 4291 (addresses), RFC 6724 (source address selection, rules 1 to 3 and 8), RFC 5722 and RFC 6946
 * (overlapping fragments; an atomic fragment is an ordinary packet), RFC 4443 (the errors of the receive path) and RFC 1981 (path MTU discovery: only ever lowered, never below 1280).
 */
#include "net6k.h"
#include "kernel.h"
#include "mm.h"
#include "random.h"

Ip6Stats g_ip6_stats;
const u8 *g_rx6_pkt;
size_t g_rx6_len;
NetDev *g_rx6_dev;

/* ------------------------------------------------------------------ the addresses of a card */

Ip6If *ip6_if(NetDev *d) { return d->ip6; }

Ip6If *ip6_if_make(NetDev *d) {
    if (!d->ip6) {
        Ip6If *f = kzalloc(sizeof *f);
        if (!f) return NULL;
        f->hop_limit = 64;
        f->reachable_ms = 30000;
        f->retrans_ms = 1000;
        d->ip6 = f;
    }
    return d->ip6;
}

u32 ip6_dev_scope(NetDev *d) { return (u32)d->index + 1; }

NetDev *ip6_scope_dev(u32 scope) { return scope ? netdev_by_index((int)scope - 1) : NULL; }

u32 ip6_link_mtu(NetDev *d) {
    Ip6If *f = d->ip6;
    u32 mtu = d->mtu;
    if (f && f->mtu && f->mtu < mtu) mtu = f->mtu;
    return mtu;
}

Addr6 *ip6_addr_on(NetDev *d, const In6 *a) {
    Ip6If *f = d->ip6;
    if (!f) return NULL;
    for (int i = 0; i < IP6_MAX_ADDRS; i++)
        if (f->a[i].state && in6_eq(&f->a[i].addr, a)) return &f->a[i];
    return NULL;
}

Addr6 *ip6_addr_find(const In6 *a, NetDev **dev) {
    NetDev *d;
    for (int i = 0; (d = netdev_by_index(i)) != NULL; i++) {
        Addr6 *x = ip6_addr_on(d, a);
        if (x) { if (dev) *dev = d; return x; }
    }
    return NULL;
}

bool ip6_is_ours(const In6 *a) {
    Addr6 *x = ip6_addr_find(a, NULL);
    return x && (x->state == A6_PREFERRED || x->state == A6_DEPRECATED);
}

/* The groups this host listens to on a card: all nodes (link-local and interface-local) and the solicited-node group of each of its addresses (tentative ones too: duplicate address
 * detection must hear the solicitations of others). Sockets cannot join groups and no MLD report is sent. */
bool ip6_mcast_for_us(NetDev *d, const In6 *dst) {
    static const u8 all1[16] = { 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    static const u8 all2[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (!memcmp(dst->b, all1, 16) || !memcmp(dst->b, all2, 16)) return true;
    if (!in6_is_solicited_node(dst)) return false;
    Ip6If *f = d->ip6;
    if (!f) return false;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) {
        if (!f->a[i].state || f->a[i].state == A6_DUPLICATE) continue;
        if (f->a[i].addr.b[13] == dst->b[13] && f->a[i].addr.b[14] == dst->b[14] && f->a[i].addr.b[15] == dst->b[15]) return true;
    }
    return false;
}

static void addr_route(NetDev *d, const Addr6 *a, bool add) {
    In6 pfx;
    in6_mask(&pfx, &a->addr, a->plen);
    if (add) {
        route6_add(&pfx, a->plen, NULL, d, 256, R6F_CONNECTED, 0);
        return;
    }
    Ip6If *f = d->ip6;
    for (int i = 0; f && i < IP6_MAX_ADDRS; i++)                         /* another address with the same prefix keeps the route */
        if (&f->a[i] != a && f->a[i].state && f->a[i].plen == a->plen && in6_prefix_eq(&f->a[i].addr, &pfx, a->plen)) return;
    route6_del(&pfx, a->plen, d, NULL);
}

int ip6_addr_add(NetDev *d, const In6 *addr, int plen, u8 flags, u32 valid_s, u32 pref_s) {
    Ip6If *f = ip6_if_make(d);
    if (!f) return -ENOMEM;
    if (plen < 1 || plen > 128) return -EINVAL;
    if (in6_is_any(addr) || in6_is_mcast(addr) || in6_is_v4mapped(addr)) return -EINVAL;
    if (in6_is_loopback(addr) != ((d->flags & NDF_LOOPBACK) != 0)) return -EINVAL;      /* ::1 belongs to lo, and lo has nothing else */
    if (ip6_addr_on(d, addr)) return -EEXIST;
    Addr6 *a = NULL;
    int slaac = 0;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) {
        if (!f->a[i].state) { if (!a) a = &f->a[i]; }
        else if (f->a[i].flags & A6F_SLAAC) slaac++;
    }
    if (!a) return -ENOSPC;
    if ((flags & A6F_SLAAC) && slaac >= IP6_MAX_SLAAC) return -ENOSPC;
    u64 now = net_now();
    memset(a, 0, sizeof *a);
    a->addr = *addr;
    a->plen = (u8)plen;
    a->flags = flags;
    a->valid_end = (valid_s == IP6_INFINITE || valid_s == 0 || (flags & A6F_PERMANENT)) ? 0 : now + (u64)valid_s * 1000;
    a->pref_end = (pref_s == IP6_INFINITE || pref_s == 0 || (flags & A6F_PERMANENT)) ? 0 : now + (u64)pref_s * 1000;
    if (!(flags & A6F_PERMANENT) && valid_s != 0 && pref_s == 0 && (flags & A6F_SLAAC)) a->pref_end = now;       /* a router advertised it as preferred for no time at all */
    a->state = (d->flags & NDF_LOOPBACK) ? A6_PREFERRED : A6_TENTATIVE;
    addr_route(d, a, true);
    if (a->state == A6_TENTATIVE) nd6_addr_added(d, a);
    return 0;
}

int ip6_addr_del(NetDev *d, const In6 *addr) {
    Addr6 *a = ip6_addr_on(d, addr);
    if (!a) return -EADDRNOTAVAIL;
    Addr6 gone = *a;
    a->state = A6_FREE;
    addr_route(d, &gone, false);
    return 0;
}

/* ------------------------------------------------------------------ routes */

#define MAX_ROUTES6 32
static Route6 g_routes6[MAX_ROUTES6];

int route6_add(const In6 *dst, int plen, const In6 *gw, NetDev *d, u32 metric, u8 flags, u64 expires) {
    if (plen < 0 || plen > 128) return -EINVAL;
    In6 pfx, g;
    in6_mask(&pfx, dst, plen);
    if (gw) g = *gw; else in6_zero(&g);
    Route6 *slot = NULL;
    for (int i = 0; i < MAX_ROUTES6; i++) {
        Route6 *r = &g_routes6[i];
        if (!r->used) { if (!slot) slot = r; continue; }
        if (r->plen == plen && r->dev == d && in6_eq(&r->dst, &pfx) && in6_eq(&r->gw, &g)) {
            if ((r->flags & (R6F_STATIC | R6F_CONNECTED)) && (flags & R6F_RA)) return 0;      /* a route set by hand (static6, ip -6 route) or made by an address does not become one that expires */
            r->metric = metric;
            r->flags = (u8)(flags | (in6_is_any(&g) ? 0 : R6F_GATEWAY));
            r->expires = expires;
            return 0;
        }
    }
    if (!slot) return -ENOSPC;
    slot->used = true;
    slot->dst = pfx;
    slot->gw = g;
    slot->plen = (u8)plen;
    slot->dev = d;
    slot->metric = metric;
    slot->flags = (u8)(flags | (in6_is_any(&g) ? 0 : R6F_GATEWAY));
    slot->expires = expires;
    return 0;
}

int route6_del(const In6 *dst, int plen, NetDev *d, const In6 *gw) {
    In6 pfx;
    in6_mask(&pfx, dst, plen);
    int n = 0;
    for (int i = 0; i < MAX_ROUTES6; i++) {
        Route6 *r = &g_routes6[i];
        if (!r->used || r->plen != plen || !in6_eq(&r->dst, &pfx)) continue;
        if (d && r->dev != d) continue;
        if (gw && !in6_eq(&r->gw, gw)) continue;
        r->used = false;
        n++;
    }
    return n ? 0 : -ESRCH;
}

void route6_flush_dev(NetDev *d, u8 flags_mask) {
    for (int i = 0; i < MAX_ROUTES6; i++)
        if (g_routes6[i].used && g_routes6[i].dev == d && (g_routes6[i].flags & flags_mask)) g_routes6[i].used = false;
}

Route6 *route6_lookup(const In6 *dst) {
    Route6 *best = NULL;
    for (int i = 0; i < MAX_ROUTES6; i++) {
        Route6 *r = &g_routes6[i];
        if (!r->used || !(r->dev->flags & NDF_UP)) continue;
        Ip6If *f = r->dev->ip6;
        if (f && f->off) continue;
        if (!in6_prefix_eq(dst, &r->dst, r->plen)) continue;
        if (!best || r->plen > best->plen || (r->plen == best->plen && r->metric < best->metric)) best = r;
    }
    return best;
}

int route6_list(int index, Route6 **out) {
    int n = 0;
    for (int i = 0; i < MAX_ROUTES6; i++) {
        if (!g_routes6[i].used) continue;
        if (n++ == index) { *out = &g_routes6[i]; return 0; }
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ the path MTU cache (RFC 1981) */

#define PMTU_MAX 16
#define PMTU_LIFE_MS 600000

static struct { In6 dst; u32 mtu; u64 expires; bool used; } g_pmtu[PMTU_MAX];

void pmtu6_set(const In6 *dst, u32 mtu) {
    if (mtu < IP6_MIN_MTU) mtu = IP6_MIN_MTU;                 /* a router that claims less is asking for fragments of 1280 */
    u64 now = net_now();
    int slot = -1;
    for (int i = 0; i < PMTU_MAX; i++) {
        if (g_pmtu[i].used && now >= g_pmtu[i].expires) g_pmtu[i].used = false;
        if (g_pmtu[i].used && in6_eq(&g_pmtu[i].dst, dst)) { slot = i; break; }
    }
    if (slot < 0) {
        u64 oldest = ~0ULL;
        for (int i = 0; i < PMTU_MAX; i++) {
            if (!g_pmtu[i].used) { slot = i; break; }
            if (g_pmtu[i].expires < oldest) { oldest = g_pmtu[i].expires; slot = i; }
        }
    } else if (g_pmtu[slot].mtu <= mtu) {
        return;                                                /* only ever lowered */
    }
    g_pmtu[slot].used = true;
    g_pmtu[slot].dst = *dst;
    g_pmtu[slot].mtu = mtu;
    g_pmtu[slot].expires = now + PMTU_LIFE_MS;
}

u32 pmtu6_get(const In6 *dst) {
    u64 now = net_now();
    for (int i = 0; i < PMTU_MAX; i++)
        if (g_pmtu[i].used && now < g_pmtu[i].expires && in6_eq(&g_pmtu[i].dst, dst)) return g_pmtu[i].mtu;
    return 0;
}

/* ------------------------------------------------------------------ source and next-hop selection */

/* Is a better than b as the source for dst? RFC 6724 section 5: rule 2 (appropriate scope: the smaller scope that is still at least the destination's), rule 3 (not deprecated),
 * rule 8 (the longest common prefix). Rule 1 (the destination itself) and the choice of the card come first, in ip6_pick. */
static bool src_better(const Addr6 *a, const Addr6 *b, const In6 *dst) {
    int sa = in6_scope(&a->addr), sb = in6_scope(&b->addr), sd = in6_scope(dst);
    if (sa < sb) return sa >= sd;
    if (sb < sa) return sb < sd;
    bool da = a->state == A6_DEPRECATED, db = b->state == A6_DEPRECATED;
    if (da != db) return db;
    return in6_common_prefix(&a->addr, dst) > in6_common_prefix(&b->addr, dst);
}

Addr6 *ip6_src_select(NetDev *d, const In6 *dst) {
    Ip6If *f = d->ip6;
    if (!f) return NULL;
    Addr6 *best = NULL;
    for (int i = 0; i < IP6_MAX_ADDRS; i++) {
        Addr6 *a = &f->a[i];
        if (a->state != A6_PREFERRED && a->state != A6_DEPRECATED) continue;
        if (!best || src_better(a, best, dst)) best = a;
    }
    return best;
}

/* The one card that a link-scoped destination without a scope id can mean: exactly one card that is up, has a carrier and runs IPv6. *count says how many there were. */
static NetDev *single_card(int *count) {
    NetDev *found = NULL, *d;
    int n = 0;
    for (int i = 0; (d = netdev_by_index(i)) != NULL; i++) {
        if ((d->flags & NDF_LOOPBACK) || !(d->flags & NDF_UP) || !(d->flags & NDF_LINK)) continue;
        Ip6If *f = d->ip6;
        if (!f || f->off) continue;
        found = d;
        n++;
    }
    *count = n;
    return n == 1 ? found : NULL;
}

int ip6_pick(const In6 *src_hint, const In6 *dst, u32 scope, Pick6 *p) {
    memset(p, 0, sizeof *p);
    NetDev *lo = netdev_loopback(), *dev = NULL;
    bool have_hint = src_hint && !in6_is_any(src_hint);
    if (in6_is_any(dst)) return -EINVAL;
    if (in6_is_loopback(dst) || ip6_is_ours(dst)) {                        /* a destination on this host goes through the loopback, from the address it was sent to */
        if (!lo || !(lo->flags & NDF_UP)) return -ENETDOWN;
        p->dev = lo;
        p->nexthop = *dst;
        p->src = have_hint ? *src_hint : *dst;
        p->mtu = ip6_link_mtu(lo);
        return 0;
    }
    if (in6_is_mcast(dst)) {
        int sc = in6_scope(dst);
        p->nexthop = *dst;
        if (sc == IP6_SCOPE_HOST) {
            dev = lo;
        } else if (sc == IP6_SCOPE_LINK) {
            dev = ip6_scope_dev(scope);
            if (!dev) { int n; dev = single_card(&n); if (!dev) return n ? -EINVAL : -ENETUNREACH; }
        } else {
            Route6 *r = route6_lookup(dst);
            if (r) dev = r->dev;
            else { int n; dev = single_card(&n); if (!dev) return n ? -EINVAL : -ENETUNREACH; }
        }
    } else if (in6_is_linklocal(dst)) {
        dev = ip6_scope_dev(scope);
        if (!dev) { int n; dev = single_card(&n); if (!dev) return n ? -EINVAL : -ENETUNREACH; }
        p->nexthop = *dst;
    } else {
        Route6 *r = route6_lookup(dst);
        if (!r) return -ENETUNREACH;
        dev = r->dev;
        p->nexthop = (r->flags & R6F_GATEWAY) ? r->gw : *dst;
    }
    if (!(dev->flags & NDF_UP)) return -ENETDOWN;
    Ip6If *f = dev->ip6;
    if (!f || f->off) return -ENETUNREACH;
    if (have_hint) {
        p->src = *src_hint;
    } else {
        Addr6 *s = ip6_src_select(dev, dst);
        if (!s) return -EADDRNOTAVAIL;
        p->src = s->addr;
    }
    u32 mtu = ip6_link_mtu(dev);
    u32 pm = pmtu6_get(dst);
    if (pm && pm < mtu) mtu = pm;
    if (mtu < IP6_MIN_MTU) mtu = IP6_MIN_MTU;
    p->dev = dev;
    p->mtu = mtu;
    return 0;
}

/* ------------------------------------------------------------------ sending */

static void put_header(u8 *h, u8 tclass, u16 plen, u8 nh, u8 hlim, const In6 *src, const In6 *dst) {
    h[0] = (u8)(0x60 | (tclass >> 4));
    h[1] = (u8)(tclass << 4);                                              /* the flow label is 0 */
    h[2] = h[3] = 0;
    wr16(h + 4, plen);
    h[6] = nh;
    h[7] = hlim;
    memcpy(h + 8, src->b, 16);
    memcpy(h + 24, dst->b, 16);
}

static int ip6_xmit(NetDev *d, const In6 *nexthop, const u8 *dmac, NetBuf *nb) {
    g_ip6_stats.out_requests++;
    if (!(d->flags & NDF_UP)) { nb_free(nb); return -ENETDOWN; }
    nb->dev = d;
    if (dmac) return eth_send(d, dmac, ETH_P_IPV6, nb);
    return nd6_output(d, nexthop, nb);
}

u16 ip6_l4_csum(const In6 *src, const In6 *dst, u8 proto, const u8 *msg, size_t len) {
    return (u16)~net_csum(msg, len, net6_pseudo_csum(src, dst, proto, (u32)len));
}

int ip6_send_on(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, u8 proto, u8 hlim, NetBuf *nb) {
    size_t plen = nb->len;
    if (plen + IP6_HLEN > ip6_link_mtu(d) || plen + IP6_HLEN > NB_SIZE - 128) { nb_free(nb); return -EMSGSIZE; }
    put_header(nb_push(nb, IP6_HLEN), 0, (u16)plen, proto, hlim, src, dst);
    return ip6_xmit(d, dst, dmac, nb);
}

static u8 hlim_for(const Pick6 *p, u8 hlim) {
    if (hlim) return hlim;
    Ip6If *f = p->dev ? p->dev->ip6 : NULL;
    return f && f->hop_limit ? f->hop_limit : 64;
}

int ip6_send_data(const Pick6 *p, const In6 *dst, u8 proto, u8 hlim, u8 tclass, bool frag_ok, const u8 *data, size_t len) {
    hlim = hlim_for(p, hlim);
    if (len + IP6_HLEN <= p->mtu) {
        NetBuf *nb = nb_alloc();
        if (!nb) return -ENOBUFS;
        memcpy(nb_put(nb, len), data, len);
        put_header(nb_push(nb, IP6_HLEN), tclass, (u16)len, proto, hlim, &p->src, dst);
        return ip6_xmit(p->dev, &p->nexthop, NULL, nb);
    }
    if (!frag_ok || len > IP6_MAX_PAYLOAD) return -EMSGSIZE;
    size_t per = (p->mtu - IP6_HLEN - 8) & ~(size_t)7;                      /* every fragment but the last carries a multiple of 8 bytes */
    u32 id = (u32)random_u64();
    int rc = 0;
    for (size_t off = 0; off < len && !rc; off += per) {
        size_t n = MIN(per, len - off);
        NetBuf *f = nb_alloc();
        if (!f) { g_ip6_stats.out_frag_fails++; return -ENOBUFS; }
        u8 *fh = nb_put(f, 8 + n);
        fh[0] = proto;
        fh[1] = 0;
        wr16(fh + 2, (u16)(off | (off + n < len ? 1 : 0)));                 /* the offset is a multiple of 8, so its bits sit where the field wants them; bit 0 is "more" */
        wr32(fh + 4, id);
        memcpy(fh + 8, data + off, n);
        put_header(nb_push(f, IP6_HLEN), tclass, (u16)(8 + n), IP6P_FRAGMENT, hlim, &p->src, dst);
        g_ip6_stats.out_frag_creates++;
        rc = ip6_xmit(p->dev, &p->nexthop, NULL, f);
    }
    if (rc) g_ip6_stats.out_frag_fails++; else g_ip6_stats.out_frag_ok++;
    return rc;
}

int ip6_send_nb(const Pick6 *p, const In6 *dst, u8 proto, u8 hlim, u8 tclass, bool frag_ok, NetBuf *nb) {
    size_t plen = nb->len;
    if (plen + IP6_HLEN <= p->mtu) {
        put_header(nb_push(nb, IP6_HLEN), tclass, (u16)plen, proto, hlim_for(p, hlim), &p->src, dst);
        return ip6_xmit(p->dev, &p->nexthop, NULL, nb);
    }
    int rc = ip6_send_data(p, dst, proto, hlim, tclass, frag_ok, nb_data(nb), plen);
    nb_free(nb);
    return rc;
}

int ip6_send_csum(const In6 *src_hint, const In6 *dst, u32 scope, u8 proto, u8 hlim, u8 tclass, bool frag_ok, NetBuf *nb, int csum_off) {
    Pick6 p;
    int rc = ip6_pick(src_hint, dst, scope, &p);
    if (rc) { g_ip6_stats.out_no_routes++; nb_free(nb); return rc; }
    if (csum_off >= 0 && (size_t)csum_off + 2 <= nb->len) {
        u8 *m = nb_data(nb);
        u16 c = ip6_l4_csum(&p.src, dst, proto, m, nb->len);
        if (c == 0 && proto == IP6P_UDP) c = 0xFFFF;                         /* a zero checksum means "none" in IPv4 and is not allowed in IPv6 (RFC 8200 section 8.1) */
        memcpy(m + csum_off, &c, 2);
    }
    return ip6_send_nb(&p, dst, proto, hlim, tclass, frag_ok, nb);
}

/* ------------------------------------------------------------------ reassembly (RFC 8200 section 4.5) */

#define FRAG6_MAX      4
#define FRAG6_PER_SRC  2
int g_ip6_reasm_ms = 60000;                   /* how long a datagram's fragments are kept (RFC 8200 says 60 seconds; /proc/net/tune can shorten it for tests) */

typedef struct Frag6Q {
    bool    used;
    In6     src, dst;
    u32     id;
    u64     born, deadline;
    NetDev *dev;
    bool    have_quote;
    u8      quote[IP6_HLEN + 8];                    /* the first fragment's header and fragment header, for the "time exceeded" message */
    Reasm6  r;
} Frag6Q;

static Frag6Q g_frag[FRAG6_MAX];

static void frag_free(Frag6Q *q) {
    kfree(q->r.buf);
    memset(q, 0, sizeof *q);
}

static Frag6Q *frag_find(const In6 *src, const In6 *dst, u32 id) {
    for (int i = 0; i < FRAG6_MAX; i++)
        if (g_frag[i].used && g_frag[i].id == id && in6_eq(&g_frag[i].src, src) && in6_eq(&g_frag[i].dst, dst)) return &g_frag[i];
    return NULL;
}

static Frag6Q *frag_new(const In6 *src, const In6 *dst, u32 id, NetDev *d) {
    Frag6Q *slot = NULL, *oldest = NULL, *oldest_src = NULL;
    int from_src = 0;
    for (int i = 0; i < FRAG6_MAX; i++) {
        Frag6Q *q = &g_frag[i];
        if (!q->used) { if (!slot) slot = q; continue; }
        if (!oldest || q->born < oldest->born) oldest = q;
        if (in6_eq(&q->src, src)) { from_src++; if (!oldest_src || q->born < oldest_src->born) oldest_src = q; }
    }
    if (from_src >= FRAG6_PER_SRC) { frag_free(oldest_src); g_ip6_stats.reasm_fails++; slot = oldest_src; }      /* one source cannot take every queue */
    else if (!slot) { frag_free(oldest); g_ip6_stats.reasm_fails++; slot = oldest; }                               /* full: the oldest datagram loses */
    u8 *buf = kmalloc(IP6_HLEN + IP6_MAX_PAYLOAD);
    if (!buf) return NULL;
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    slot->src = *src;
    slot->dst = *dst;
    slot->id = id;
    slot->dev = d;
    slot->born = net_now();
    slot->deadline = slot->born + (u64)g_ip6_reasm_ms;
    slot->r.buf = buf;
    return slot;
}

static void ip6_deliver(NetDev *d, const u8 *pkt, size_t len, const Ip6Walk *w, bool tentative);

/* The walk over what follows the headers a fragment shared: the verdict is acted on here, the packet delivered if it passed. */
static void ip6_after_walk(NetDev *d, const u8 *pkt, size_t len, Ip6Walk *w, bool tentative) {
    if (w->verdict == IP6W_DROP) { g_ip6_stats.bad++; g_ip6_stats.in_hdr_errors++; return; }
    if (w->verdict == IP6W_ICMP) { g_ip6_stats.bad++; g_ip6_stats.in_hdr_errors++; icmp6_error(ICMP6_PARAM_PROB, w->pp_code, w->pp_ptr, pkt, len, d); return; }
    ip6_deliver(d, pkt, len, w, tentative);
}

static void ip6_reasm_input(NetDev *d, const u8 *p, size_t len, const Ip6Walk *w, bool tentative) {
    const In6 *src = (const In6 *)(p + 8), *dst = (const In6 *)(p + 24);
    u32 dlen = (u32)(len - w->uoff);
    g_ip6_stats.reasm_reqds++;
    if (w->frag_more && (dlen & 7)) {                                      /* RFC 8200: a fragment that is not the last must be a multiple of 8 bytes */
        g_ip6_stats.bad++; g_ip6_stats.in_hdr_errors++;
        icmp6_error(ICMP6_PARAM_PROB, 0, 4, p, len, d);
        return;
    }
    if ((u64)w->frag_off + dlen > IP6_MAX_PAYLOAD) {
        g_ip6_stats.bad++; g_ip6_stats.in_hdr_errors++;
        icmp6_error(ICMP6_PARAM_PROB, 0, w->frag_hoff + 2, p, len, d);
        return;
    }
    if (w->frag_off == 0 && !w->frag_more) {                               /* an atomic fragment (RFC 6946): no other fragment will come; it is an ordinary packet */
        Ip6Walk w2;
        ip6_walk(p, len, w->frag_nh, w->uoff, false, false, in6_is_mcast(dst), &w2);
        if (w2.verdict == IP6W_OK && w2.nexthdrs == 0) w2.nh_ptr = w->frag_hoff;
        ip6_after_walk(d, p, len, &w2, tentative);
        return;
    }
    if (dlen == 0) { g_ip6_stats.bad++; g_ip6_stats.in_hdr_errors++; return; }
    Frag6Q *q = frag_find(src, dst, w->frag_id);
    if (!q) {
        q = frag_new(src, dst, w->frag_id, d);
        if (!q) { g_ip6_stats.reasm_fails++; return; }
    }
    if (w->frag_off == 0 && !q->have_quote) {
        q->have_quote = true;
        memcpy(q->quote, p, MIN(len, sizeof q->quote));
        q->dev = d;
    }
    int r = reasm6_add(&q->r, w->frag_off, w->frag_more, w->frag_nh, p + w->uoff, dlen);
    if (r == R6_BAD) { g_ip6_stats.reasm_fails++; g_ip6_stats.bad++; frag_free(q); return; }
    if (r != R6_DONE) return;
    u8 *buf = q->r.buf;                                                    /* the whole datagram: a copy of this fragment's header, the lengths and next header made right, then the data */
    u32 total = q->r.total;
    u8 first_nh = q->r.first_nh;
    memcpy(buf, p, IP6_HLEN);
    wr16(buf + 4, (u16)total);
    buf[6] = first_nh;
    Ip6Walk w2;
    ip6_walk(buf, IP6_HLEN + total, first_nh, IP6_HLEN, false, false, in6_is_mcast(dst), &w2);
    g_ip6_stats.reasm_oks++;
    ip6_after_walk(d, buf, IP6_HLEN + total, &w2, tentative);
    frag_free(q);
}

/* ------------------------------------------------------------------ the receive path */

static void ip6_deliver(NetDev *d, const u8 *pkt, size_t len, const Ip6Walk *w, bool tentative) {
    const In6 *src = (const In6 *)(pkt + 8), *dst = (const In6 *)(pkt + 24);
    g_rx6_pkt = pkt;
    g_rx6_len = len;
    g_rx6_dev = d;
    switch (w->proto) {
    case IP6P_ICMPV6:
        g_ip6_stats.in_delivers++;
        icmp6_input(d, pkt, len, w->uoff, tentative);
        break;
    case IP6P_UDP:
        if (tentative || in6_is_any(src)) { g_ip6_stats.in_addr_errors++; break; }
        g_ip6_stats.in_delivers++;
        udp6_input(d, pkt + w->uoff, len - w->uoff, src, dst);
        break;
    case IP6P_TCP:
        if (tentative || in6_is_any(src)) { g_ip6_stats.in_addr_errors++; break; }
        g_ip6_stats.in_delivers++;
        tcp6_input(d, pkt + w->uoff, len - w->uoff, src, dst);
        break;
    case IP6P_NONE:
        break;
    default:                                                               /* a protocol nobody here speaks: RFC 8200 says a parameter problem pointing at the Next Header byte */
        g_ip6_stats.in_unknown_protos++;
        icmp6_error(ICMP6_PARAM_PROB, 1, w->nh_ptr, pkt, len, d);
        break;
    }
    g_rx6_pkt = NULL;
    g_rx6_len = 0;
    g_rx6_dev = NULL;
}

void ip6_input(NetDev *d, NetBuf *nb) {
    u8 *p = nb_data(nb);
    size_t len = nb->len;
    const char *why = "too short or not IPv6";
    g_ip6_stats.in_receives++;
    Ip6If *f = d->ip6;
    if (f && f->off) { g_ip6_stats.in_discards++; goto drop; }              /* IPv6 is off on this card */
    if (len < IP6_HLEN || (p[0] >> 4) != 6) goto bad_hdr;
    size_t plen = rd16(p + 4);
    why = "payload length";
    if (IP6_HLEN + plen > len) { g_ip6_stats.in_truncated++; goto bad; }
    len = IP6_HLEN + plen;                                                  /* the rest is Ethernet padding */
    const In6 *src = (const In6 *)(p + 8), *dst = (const In6 *)(p + 24);
    why = "address";
    if (in6_is_mcast(src) || in6_is_any(dst) || in6_is_v4mapped(src) || in6_is_v4mapped(dst)) { g_ip6_stats.in_addr_errors++; goto bad; }
    if (!(d->flags & NDF_LOOPBACK) && (in6_is_loopback(src) || in6_is_loopback(dst))) { g_ip6_stats.in_addr_errors++; goto bad; }     /* ::1 never crosses a link */
    bool tentative = false;
    if (in6_is_mcast(dst)) {
        if (!ip6_mcast_for_us(d, dst)) goto drop;
    } else {
        Addr6 *a = ip6_addr_find(dst, NULL);
        if (!a || a->state == A6_DUPLICATE) { g_ip6_stats.in_addr_errors++; goto drop; }       /* a host, not a router: what is not ours is not ours */
        tentative = a->state == A6_TENTATIVE;
    }
    why = "extension headers";
    if (plen == 0 && p[6] == IP6P_HOPOPTS) goto bad;                       /* a jumbogram: not supported */
    Ip6Walk w;
    ip6_walk(p, len, p[6], IP6_HLEN, true, true, in6_is_mcast(dst), &w);
    if (w.verdict == IP6W_DROP) goto bad_hdr;
    if (w.verdict == IP6W_ICMP) {
        g_ip6_stats.bad++;
        g_ip6_stats.in_hdr_errors++;
        icmp6_error(ICMP6_PARAM_PROB, w.pp_code, w.pp_ptr, p, len, d);
        goto drop;
    }
    if (w.proto == IP6P_FRAGMENT) ip6_reasm_input(d, p, len, &w, tentative);
    else ip6_deliver(d, p, len, &w, tentative);
drop:
    nb_free(nb);
    return;
bad_hdr:
    g_ip6_stats.in_hdr_errors++;
bad:
    g_ip6_stats.bad++;
    d->rx_errors++;
    if (g_net_trace && g_net_trace_n < 40) {
        g_net_trace_n++;
        klog("Yukari", "%s: IPv6 packet thrown away (%s): %u bytes, first bytes %02x %02x %02x %02x", d->name, why, (unsigned)len, len > 0 ? p[0] : 0, len > 1 ? p[1] : 0, len > 6 ? p[6] : 0, len > 7 ? p[7] : 0);
    }
    nb_free(nb);
}

/* ------------------------------------------------------------------ timers */

void ip6_tick(u64 now) {
    for (int i = 0; i < FRAG6_MAX; i++) {
        Frag6Q *q = &g_frag[i];
        if (!q->used || now < q->deadline) continue;
        g_ip6_stats.reasm_timeouts++;
        g_ip6_stats.reasm_fails++;
        if (q->have_quote) icmp6_error(ICMP6_TIME_EXCEEDED, 1, 0, q->quote, sizeof q->quote, q->dev);     /* only if the first fragment came (RFC 8200) */
        frag_free(q);
    }
    for (int i = 0; i < MAX_ROUTES6; i++)
        if (g_routes6[i].used && g_routes6[i].expires && now >= g_routes6[i].expires) g_routes6[i].used = false;
    NetDev *d;
    for (int k = 0; (d = netdev_by_index(k)) != NULL; k++) {                 /* the lifetimes of the addresses */
        Ip6If *f = d->ip6;
        for (int i = 0; f && i < IP6_MAX_ADDRS; i++) {
            Addr6 *a = &f->a[i];
            if (!a->state) continue;
            if (a->valid_end && now >= a->valid_end) {
                char t[48];
                in6_str(&a->addr, t, sizeof t);
                klog_quiet("Yukari", "%s: the address %s/%u ran out of lifetime", d->name, t, a->plen);
                ip6_addr_del(d, &a->addr);
                continue;
            }
            if (a->state == A6_PREFERRED && a->pref_end && now >= a->pref_end) a->state = A6_DEPRECATED;
        }
    }
}

void ip6_dev_added(NetDev *d) {
    Ip6If *f = ip6_if_make(d);
    if (!f) return;
    if (d->flags & NDF_LOOPBACK) {
        In6 lo;
        in6_zero(&lo);
        lo.b[15] = 1;
        ip6_addr_add(d, &lo, 128, A6F_PERMANENT, IP6_INFINITE, IP6_INFINITE);
    }
}
