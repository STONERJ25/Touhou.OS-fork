/*
 * DHCP client (RFC 2131), run from the Yukari thread for every NIC that has a carrier: DISCOVER, OFFER, REQUEST, ACK, then
 * renewal at T1 and a fresh start when the lease runs out. It sets the address, the default route and /etc/resolv.conf.
 * Nothing else in the stack ever speaks first on the network.
 */
#include "net.h"
#include "kernel.h"
#include "vfs.h"
#include "process.h"
#include "random.h"

enum { D_SELECTING = 1, D_REQUESTING, D_BOUND, D_RENEWING };

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_NAK      6
#define DHCP_ACK      5

typedef struct Dhcp {
    int  state;
    u32  xid;
    u32  server, offer, mask, router;
    u32  dns[3];
    int  ndns;
    u32  lease_s;
    u64  next_send, t1, expire, started;
    int  tries;
    bool resolv_dirty;
} Dhcp;

static u32 g_dns[3];
static size_t g_ndns;

size_t dhcp_dns(u32 *servers, size_t max) {
    size_t n = MIN(max, g_ndns);
    for (size_t i = 0; i < n; i++) servers[i] = g_dns[i];
    return n;
}

static void dhcp_unconfigure(NetDev *d) {
    if (d->flags & NDF_DHCP) {
        route_flush_dev(d);
        d->addr = d->mask = d->bcast = 0;
        d->flags &= ~(u32)NDF_DHCP;
    }
}

void dhcp_start(NetDev *d) {
    if (d->flags & NDF_LOOPBACK) return;
    Dhcp *c = d->dhcp;
    if (!c) { c = kzalloc(sizeof *c); if (!c) return; d->dhcp = c; }
    dhcp_unconfigure(d);
    c->state = D_SELECTING;
    c->xid = (u32)random_u64();
    c->tries = 0;
    c->next_send = 0;                       /* now */
    c->started = net_now();
    net_kick();
}

static void dhcp_send(NetDev *d, Dhcp *c, u8 type) {
    NetBuf *nb = nb_alloc();
    if (!nb) return;
    const size_t total = 8 + 300;
    u8 *u = nb_put(nb, total);
    memset(u, 0, total);
    u8 *m = u + 8;
    m[0] = 1; m[1] = 1; m[2] = 6;
    wr32(m + 4, c->xid);
    wr16(m + 8, (u16)((net_now() - c->started) / 1000));
    wr16(m + 10, 0x8000);                                            /* ask for broadcast replies: we have no address yet */
    bool renewing = c->state == D_RENEWING;
    if (renewing) wr32(m + 12, d->addr);
    memcpy(m + 28, d->mac, 6);
    wr32(m + 236, 0x63825363);
    u8 *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, d->mac, 6); o += 6;       /* client identifier: hardware type + MAC */
    *o++ = 12; *o++ = 8; memcpy(o, "gensokyo", 8); o += 8;
    if (type == DHCP_REQUEST && !renewing) {
        *o++ = 50; *o++ = 4; wr32(o, c->offer); o += 4;
        *o++ = 54; *o++ = 4; wr32(o, c->server); o += 4;
    }
    *o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 28; *o++ = 51;
    *o++ = 255;
    wr16(u, 68); wr16(u + 2, 67); wr16(u + 4, (u16)total);
    u32 src = renewing ? d->addr : 0;
    u16 ck = (u16)~net_csum(u, total, net_pseudo_csum(src, 0xFFFFFFFFu, IPPROTO_UDP, (u16)total));
    if (ck == 0) ck = 0xFFFF;
    memcpy(u + 6, &ck, 2);
    ip_send_on(d, src, 0xFFFFFFFFu, IPPROTO_UDP, nb);
}

static void dhcp_bind(NetDev *d, Dhcp *c) {
    route_flush_dev(d);
    netdev_set_addr(d, c->offer, c->mask);
    d->flags |= NDF_DHCP;
    if (c->router) route_add(0, 0, c->router, d, 0);
    c->state = D_BOUND;
    u64 now = net_now();
    u32 lease = c->lease_s ? c->lease_s : 3600;
    c->t1 = now + (u64)lease * 500;
    c->expire = now + (u64)lease * 1000;
    g_ndns = 0;
    for (int i = 0; i < c->ndns; i++) g_dns[g_ndns++] = c->dns[i];
    c->resolv_dirty = true;
    char a[20], m[20], r[20], s[20];
    net_ip_str(d->addr, a, sizeof a);
    net_ip_str(d->mask, m, sizeof m);
    net_ip_str(c->router, r, sizeof r);
    net_ip_str(c->server, s, sizeof s);
    klog("Yukari", "%s: lease %s/%s from %s, gateway %s, %d name server%s, %u s", d->name, a, m, s, r, c->ndns, c->ndns == 1 ? "" : "s", lease);
    arp_announce(d);
}

void dhcp_input(NetDev *d, const u8 *p, size_t len) {
    Dhcp *c = d->dhcp;
    if (!c || len < 241 || p[0] != 2 || p[1] != 1 || p[2] != 6) return;
    if (rd32(p + 4) != c->xid || memcmp(p + 28, d->mac, 6) || rd32(p + 236) != 0x63825363) return;
    u32 yiaddr = rd32(p + 16);
    u8 type = 0;
    u32 server = 0, mask = 0, router = 0, lease = 0, dns[3];
    int ndns = 0;
    size_t i = 240;
    while (i < len) {
        u8 code = p[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= len) break;
        u8 l = p[i++];
        if (i + l > len) break;
        const u8 *v = p + i;
        switch (code) {
        case 53: if (l == 1) type = v[0]; break;
        case 54: if (l == 4) server = rd32(v); break;
        case 1:  if (l == 4) mask = rd32(v); break;
        case 3:  if (l >= 4) router = rd32(v); break;
        case 6:  for (unsigned k = 0; k + 4 <= l && ndns < 3; k += 4) dns[ndns++] = rd32(v + k); break;
        case 51: if (l == 4) lease = rd32(v); break;
        default: break;
        }
        i += l;
    }
    if (type == DHCP_OFFER && c->state == D_SELECTING && yiaddr && server) {
        c->offer = yiaddr;
        c->server = server;
        c->state = D_REQUESTING;
        c->tries = 0;
        dhcp_send(d, c, DHCP_REQUEST);
        c->tries = 1;
        c->next_send = net_now() + 2000;
    } else if (type == DHCP_ACK && (c->state == D_REQUESTING || c->state == D_RENEWING) && yiaddr) {
        c->offer = yiaddr;
        if (server) c->server = server;
        c->mask = mask ? mask : (yiaddr >> 24) < 128 ? 0xFF000000u : (yiaddr >> 24) < 192 ? 0xFFFF0000u : 0xFFFFFF00u;
        c->router = router;
        c->lease_s = lease;
        c->ndns = ndns;
        for (int k = 0; k < ndns; k++) c->dns[k] = dns[k];
        dhcp_bind(d, c);
    } else if (type == DHCP_NAK) {
        klog("Yukari", "%s: the DHCP server refused the request; starting over", d->name);
        dhcp_start(d);
    }
}

static void write_resolv(Dhcp *c, NetDev *d) {
    char buf[256];
    size_t n = (size_t)snprintf(buf, sizeof buf, "# written by Yukari's DHCP client (%s)\n", d->name);
    for (int i = 0; i < c->ndns; i++) {
        char a[20];
        net_ip_str(c->dns[i], a, sizeof a);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "nameserver %s\n", a);
    }
    if (vfs_write_file("/etc/resolv.conf", buf, n, 0644) == 0) c->resolv_dirty = false;
}

void dhcp_tick(u64 now) {
    NetDev *d;
    for (int idx = 0; (d = netdev_by_index(idx)) != NULL; idx++) {
        Dhcp *c = d->dhcp;
        if (!c || !c->state) continue;
        if (!(d->flags & NDF_LINK)) continue;
        switch (c->state) {
        case D_SELECTING:
        case D_REQUESTING:
            if (now < c->next_send) break;
            if (c->tries >= 6) {                                      /* nobody answers: try again in a minute */
                if (c->state == D_REQUESTING) c->state = D_SELECTING;
                c->tries = 0;
                c->next_send = now + 60000;
                break;
            }
            dhcp_send(d, c, c->state == D_SELECTING ? DHCP_DISCOVER : DHCP_REQUEST);
            c->next_send = now + (u64)1000 * (1u << MIN(c->tries, 3));
            c->tries++;
            break;
        case D_BOUND:
            if (now >= c->t1) { c->state = D_RENEWING; c->tries = 0; c->next_send = now; }
            break;
        case D_RENEWING:
            if (now >= c->expire) { klog("Yukari", "%s: the lease ran out", d->name); dhcp_start(d); break; }
            if (now >= c->next_send) { dhcp_send(d, c, DHCP_REQUEST); c->next_send = now + 10000; }
            break;
        }
        if (c->resolv_dirty && g_init_proc) write_resolv(c, d);
    }
}
