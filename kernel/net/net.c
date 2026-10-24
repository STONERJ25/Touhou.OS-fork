/*
 * Yukari's gaps between machines: packet buffers, the network lock, the device list, the loopback device, the polling
 * thread, Ethernet and ARP, the routing table, IPv4 (with fragment reassembly) and ICMP.
 */
#include "net.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "random.h"
#include "initcall.h"
#include "vfs.h"
#include "process.h"
#include "cmdline.h"

/* ------------------------------------------------------------------ tunables (all settable through /proc/net/tune) */

int g_net_lo_drop, g_net_lo_dup, g_net_lo_reorder, g_net_lo_corrupt;
int g_net_rto_min_ms = 200;
int g_net_syn_retries = 5;
int g_net_data_retries = 10;
int g_net_timewait_ms = 30000;
u64 g_net_stat_ip_bad, g_net_stat_tcp_bad;
static int g_net_ip_forward_ignored;

u64 net_now(void) { return uptime_ms(); }

/* ------------------------------------------------------------------ text helpers */

size_t net_ip_str(u32 ip, char *buf, size_t n) {
    return (size_t)snprintf(buf, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool net_ip_parse(const char *s, u32 *out) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        u32 part = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { part = part * 10 + (u32)(*s++ - '0'); if (++digits > 3 || part > 255) return false; }
        v = v << 8 | part;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = v;
    return true;
}

/* ------------------------------------------------------------------ packet buffers */

static int g_nb_count;

NetBuf *nb_alloc(void) {
    if (g_nb_count > 16384) return NULL;                    /* 32 MiB of packets in flight is a flood, not traffic */
    NetBuf *nb = kmalloc(sizeof *nb);
    if (!nb) return NULL;
    g_nb_count++;
    nb->next = NULL;
    nb->dev = NULL;
    nb->off = NB_HEAD;
    nb->len = 0;
    nb->pad = 0;
    return nb;
}

void nb_free(NetBuf *nb) {
    if (!nb) return;
    g_nb_count--;
    kfree(nb);
}

/* ------------------------------------------------------------------ the lock */

static Mutex g_mtx;
static WaitQueue g_yukari_wq;
static bool g_kicked;

void net_lock(void) { mutex_lock(&g_mtx); }
void net_unlock(void) { mutex_unlock(&g_mtx); }

int net_wait(WaitQueue *wq, u64 ms) {
    int rc = 0;
    u64 fl = irq_save();
    mutex_unlock(&g_mtx);
    current->intr = true;
    if (signal_pending()) rc = -EINTR;
    else if (ms == NET_FOREVER) wq_block(wq);
    else if (!wq_block_timeout(wq, ms)) rc = -ETIMEDOUT;
    current->intr = false;
    irq_restore(fl);
    mutex_lock(&g_mtx);
    if (!rc && signal_pending()) rc = -EINTR;
    return rc;
}

void net_kick(void) {
    u64 fl = irq_save();
    g_kicked = true;
    wq_wake_one(&g_yukari_wq);
    irq_restore(fl);
}

/* ------------------------------------------------------------------ checksums */

/* Sums 16-bit words as they sit in memory on a little-endian CPU. The ones-complement sum commutes with byte swapping, so
 * the folded result, inverted and stored with a native 16-bit write, is exactly the network-order checksum field. */
u16 net_csum(const void *data, size_t n, u32 init) {
    const u8 *p = data;
    u64 sum = init;
    while (n >= 8) {
        u64 v;
        memcpy(&v, p, 8);
        sum += v;
        if (sum < v) sum++;                                 /* end-around carry */
        p += 8;
        n -= 8;
    }
    if (n >= 4) { u32 v; memcpy(&v, p, 4); sum += v; p += 4; n -= 4; }
    if (n >= 2) { u16 v; memcpy(&v, p, 2); sum += v; p += 2; n -= 2; }
    if (n) sum += *p;
    sum = (sum & 0xFFFFFFFFu) + (sum >> 32);
    sum = (sum & 0xFFFFFFFFu) + (sum >> 32);
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)sum;
}

u32 net_pseudo_csum(u32 src, u32 dst, u8 proto, u16 len) {
    return (u32)hton16((u16)(src >> 16)) + hton16((u16)src) + hton16((u16)(dst >> 16)) + hton16((u16)dst) +
           hton16(proto) + hton16(len);
}

/* ------------------------------------------------------------------ devices */

static NetDev *g_devs;
static int g_ndevs;
static NetDev *g_lo;

NetDev *netdev_find(const char *name) {
    for (NetDev *d = g_devs; d; d = d->next) if (!strcmp(d->name, name)) return d;
    return NULL;
}

NetDev *netdev_by_index(int i) {
    for (NetDev *d = g_devs; d; d = d->next) if (d->index == i) return d;
    return NULL;
}

NetDev *netdev_loopback(void) { return g_lo; }

NetDev *netdev_for_addr(u32 ip) {
    if (!ip) return NULL;
    for (NetDev *d = g_devs; d; d = d->next) if (d->addr == ip) return d;
    return NULL;
}

bool net_is_local(u32 ip) { return (ip >> 24) == 127 || netdev_for_addr(ip) != NULL; }

bool net_have_hardware(void) {
    for (NetDev *d = g_devs; d; d = d->next) if (!(d->flags & NDF_LOOPBACK)) return true;
    return false;
}

/* "nonet" on the kernel command line: the card drivers do not touch any hardware (the escape hatch for a first boot on a real PC whose
 * network chip misbehaves); the loopback still works. Says so once, whichever driver asks first. */
bool net_cards_disabled(void) {
    static bool said;
    if (!cmdline_flag("nonet")) return false;
    if (!said) { said = true; klog("Yukari", "nonet: the network cards are left alone; only the loopback runs"); }
    return true;
}

NetDev *netdev_register(const char *prefix, NetDev *d) {
    net_lock();
    int n = 0;
    for (NetDev *o = g_devs; o; o = o->next) if (!strncmp(o->name, prefix, strlen(prefix))) n++;
    snprintf(d->name, sizeof d->name, "%s%d", prefix, n);
    d->index = g_ndevs++;
    if (!d->mtu) d->mtu = 1500;
    d->flags |= NDF_UP;
    d->next = NULL;
    if (!g_devs) g_devs = d;
    else { NetDev *t = g_devs; while (t->next) t = t->next; t->next = d; }
    if ((d->flags & NDF_LINK) && !(d->flags & NDF_LOOPBACK)) dhcp_start(d);
    net_unlock();
    net_kick();
    return d;
}

void net_set_link(NetDev *d, bool up) {
    bool was = (d->flags & NDF_LINK) != 0;
    if (up == was) return;
    if (up) d->flags |= NDF_LINK; else d->flags &= ~(u32)NDF_LINK;
    arp_flush_dev(d);                                       /* the cable may lead to another network: what we knew about neighbours is stale */
    klog("Yukari", "%s: link %s", d->name, up ? "up, the gap is open" : "down, the gap closed");
    if (up && !(d->flags & NDF_LOOPBACK) && (!d->addr || (d->flags & NDF_DHCP))) dhcp_start(d);
}

/* ------------------------------------------------------------------ routes */

#define MAX_ROUTES 32
static Route g_routes[MAX_ROUTES];

static int route_count_slots(void) { return MAX_ROUTES; }

int route_add(u32 dst, u32 mask, u32 gw, NetDev *d, u32 metric) {
    Route *free_slot = NULL;
    for (int i = 0; i < route_count_slots(); i++) {
        Route *r = &g_routes[i];
        if (!r->used) { if (!free_slot) free_slot = r; continue; }
        if (r->dst == dst && r->mask == mask && r->dev == d) {          /* replace */
            r->gw = gw;
            r->metric = metric;
            return 0;
        }
    }
    if (!free_slot) return -ENOSPC;
    free_slot->used = true;
    free_slot->dst = dst;
    free_slot->mask = mask;
    free_slot->gw = gw;
    free_slot->dev = d;
    free_slot->metric = metric;
    return 0;
}

int route_del(u32 dst, u32 mask) {
    int n = 0;
    for (int i = 0; i < MAX_ROUTES; i++)
        if (g_routes[i].used && g_routes[i].dst == dst && g_routes[i].mask == mask) { g_routes[i].used = false; n++; }
    return n ? 0 : -ESRCH;
}

void route_flush_dev(NetDev *d) {
    for (int i = 0; i < MAX_ROUTES; i++) if (g_routes[i].used && g_routes[i].dev == d) g_routes[i].used = false;
}

static u32 mask_len(u32 m) { u32 n = 0; while (m) { n++; m <<= 1; } return n; }

Route *ip_route(u32 dst) {
    Route *best = NULL;
    for (int i = 0; i < MAX_ROUTES; i++) {
        Route *r = &g_routes[i];
        if (!r->used || (dst & r->mask) != r->dst) continue;
        if (!(r->dev->flags & NDF_UP)) continue;
        if (!best || mask_len(r->mask) > mask_len(best->mask) || (r->mask == best->mask && r->metric < best->metric)) best = r;
    }
    return best;
}

int route_list(int index, struct gnet_route *out) {
    int n = 0;
    for (int i = 0; i < MAX_ROUTES; i++) {
        Route *r = &g_routes[i];
        if (!r->used) continue;
        if (n++ != index) continue;
        memset(out, 0, sizeof *out);
        out->index = (u32)index;
        strlcpy(out->ifname, r->dev->name, sizeof out->ifname);
        out->dst = r->dst; out->mask = r->mask; out->gw = r->gw; out->metric = r->metric;
        out->flags = GRTF_UP | (r->gw ? GRTF_GATEWAY : 0);
        return 0;
    }
    return -ENOENT;
}

int netdev_set_addr(NetDev *d, u32 addr, u32 mask) {
    if (d->addr && d->mask) route_del(d->addr & d->mask, d->mask);
    d->addr = addr;
    d->mask = mask;
    d->bcast = (addr && mask != 0xFFFFFFFFu) ? (addr | ~mask) : 0;
    if (addr && mask) route_add(addr & mask, mask, 0, d, 0);
    return 0;
}

u32 ip_source_for(u32 dst) {
    if ((dst >> 24) == 127) return INADDR_LOOPBACK;
    if (dst == 0xFFFFFFFFu) {                       /* limited broadcast: the first real interface's address, else the loopback's (as ip_pick does) */
        for (NetDev *d = g_devs; d; d = d->next) if (!(d->flags & NDF_LOOPBACK) && (d->flags & NDF_UP)) return d->addr;
        return INADDR_LOOPBACK;
    }
    if (net_is_local(dst)) return dst;
    Route *r = ip_route(dst);
    return r ? r->dev->addr : 0;
}

/* ------------------------------------------------------------------ the loopback device */

static NetBuf *g_lo_head, *g_lo_tail, *g_lo_held;
static u32 g_lo_rand = 0x2545F491u;

static u32 lo_random(void) {                        /* xorshift: cheap, only used for the fault-injection dice */
    g_lo_rand ^= g_lo_rand << 13;
    g_lo_rand ^= g_lo_rand >> 17;
    g_lo_rand ^= g_lo_rand << 5;
    return g_lo_rand;
}

static bool lo_roll(int percent) { return percent > 0 && (int)(lo_random() % 100) < percent; }

static void lo_enqueue(NetBuf *nb) {
    nb->next = NULL;
    if (g_lo_tail) g_lo_tail->next = nb; else g_lo_head = nb;
    g_lo_tail = nb;
}

static int lo_tx(NetDev *d, NetBuf *nb) {
    d->tx_packets++;
    d->tx_bytes += nb->len;
    if (lo_roll(g_net_lo_drop)) { d->tx_dropped++; nb_free(nb); return 0; }
    if (lo_roll(g_net_lo_corrupt) && nb->len > ETH_HLEN + 20) {
        unsigned pos = ETH_HLEN + lo_random() % (unsigned)(nb->len - ETH_HLEN);
        nb_data(nb)[pos] ^= (u8)(1u << (lo_random() & 7));
    }
    if (lo_roll(g_net_lo_dup)) {
        NetBuf *c = nb_alloc();
        if (c) { c->off = nb->off; c->len = nb->len; memcpy(nb_data(c), nb_data(nb), nb->len); lo_enqueue(c); }
    }
    if (g_lo_held) {                                    /* a held packet goes out after the one that overtook it */
        lo_enqueue(nb);
        lo_enqueue(g_lo_held);
        g_lo_held = NULL;
    } else if (lo_roll(g_net_lo_reorder)) {
        g_lo_held = nb;
    } else {
        lo_enqueue(nb);
    }
    net_kick();
    return 0;
}

static bool lo_pending(void) { return g_lo_head != NULL; }

static void lo_drain(void) {
    int budget = 1024;
    while (g_lo_head && budget-- > 0) {
        NetBuf *nb = g_lo_head;
        g_lo_head = nb->next;
        if (!g_lo_head) g_lo_tail = NULL;
        nb->next = NULL;
        net_rx(g_lo, nb);
    }
    if (!g_lo_head && g_lo_held) {                      /* nothing overtook it: let it go anyway */
        NetBuf *h = g_lo_held;
        g_lo_held = NULL;
        lo_enqueue(h);
    }
}

/* ------------------------------------------------------------------ Ethernet receive */

void net_rx(NetDev *d, NetBuf *nb) {
    d->rx_packets++;
    d->rx_bytes += nb->len;
    if (nb->len < ETH_HLEN) { d->rx_errors++; nb_free(nb); return; }
    u8 *e = nb_data(nb);
    u16 type = rd16(e + 12);
    if (!(d->flags & NDF_LOOPBACK)) {
        static const u8 bc[6] = { 255, 255, 255, 255, 255, 255 };
        if (memcmp(e, bc, 6) && !(e[0] & 1) && memcmp(e, d->mac, 6)) { d->rx_dropped++; nb_free(nb); return; }
    }
    nb->dev = d;
    nb_pull(nb, ETH_HLEN);
    if (type == ETH_P_IP) { ip_input(d, nb); return; }
    if (type == ETH_P_ARP) arp_input(d, nb_data(nb), nb->len);
    nb_free(nb);
}

/* ------------------------------------------------------------------ ARP */

#define ARP_MAX 64
#define ARP_QUEUE 2

typedef struct ArpEnt {
    u32 ip;
    u8  mac[6];
    u8  state;                  /* 0 free, 1 incomplete, 2 reachable, 3 static */
    u8  tries;
    NetDev *dev;
    u64 updated;
    u64 next_try;
    NetBuf *q[ARP_QUEUE];
} ArpEnt;

static ArpEnt g_arp[ARP_MAX];

static ArpEnt *arp_find(u32 ip) {
    for (int i = 0; i < ARP_MAX; i++) if (g_arp[i].state && g_arp[i].ip == ip) return &g_arp[i];
    return NULL;
}

static ArpEnt *arp_slot(void) {
    ArpEnt *oldest = NULL;
    for (int i = 0; i < ARP_MAX; i++) {
        if (!g_arp[i].state) return &g_arp[i];
        if (g_arp[i].state != 3 && (!oldest || g_arp[i].updated < oldest->updated)) oldest = &g_arp[i];
    }
    if (oldest) {
        for (int j = 0; j < ARP_QUEUE; j++) { nb_free(oldest->q[j]); oldest->q[j] = NULL; }
        oldest->state = 0;
    }
    return oldest;
}

static int eth_send(NetDev *d, const u8 *dmac, u16 type, NetBuf *nb) {
    if (nb->len < 46 && nb_room(nb) >= 46 - nb->len) memset(nb_put(nb, 46 - nb->len), 0, 46 - nb->len);   /* pad to the 60 byte minimum */
    u8 *e = nb_push(nb, ETH_HLEN);
    memcpy(e, dmac, 6);
    memcpy(e + 6, d->mac, 6);
    wr16(e + 12, type);
    nb->dev = d;
    int rc = d->tx ? d->tx(d, nb) : -ENODEV;
    if (rc) { d->tx_errors++; nb_free(nb); }
    return rc;
}

static void arp_send(NetDev *d, u16 op, const u8 *tha, u32 tpa, const u8 *dmac) {
    NetBuf *nb = nb_alloc();
    if (!nb) return;
    u8 *p = nb_put(nb, 28);
    wr16(p, 1); wr16(p + 2, ETH_P_IP); p[4] = 6; p[5] = 4; wr16(p + 6, op);
    memcpy(p + 8, d->mac, 6);
    wr32(p + 14, d->addr);
    if (tha) memcpy(p + 18, tha, 6); else memset(p + 18, 0, 6);
    wr32(p + 24, tpa);
    eth_send(d, dmac, ETH_P_ARP, nb);
}

static const u8 g_bcast_mac[6] = { 255, 255, 255, 255, 255, 255 };

void arp_announce(NetDev *d) {
    if (!d->addr || (d->flags & NDF_NOARP)) return;
    arp_send(d, 1, NULL, d->addr, g_bcast_mac);               /* gratuitous: who has my own address? nobody should */
}

static void arp_learn(NetDev *d, u32 ip, const u8 *mac, bool create) {
    ArpEnt *e = arp_find(ip);
    if (!e && !create) return;
    if (e && e->state == 3) return;                            /* static entries stay as they are */
    if (!e) { e = arp_slot(); if (!e) return; memset(e, 0, sizeof *e); e->ip = ip; }
    memcpy(e->mac, mac, 6);
    e->dev = d;
    e->updated = net_now();
    e->tries = 0;
    bool was_incomplete = e->state == 1 || e->state == 0;
    e->state = 2;
    if (was_incomplete) {                                      /* send what was waiting for this answer */
        for (int i = 0; i < ARP_QUEUE; i++) {
            NetBuf *nb = e->q[i];
            e->q[i] = NULL;
            if (nb) eth_send(d, e->mac, ETH_P_IP, nb);
        }
    }
}

void arp_input(NetDev *d, const u8 *p, size_t len) {
    if (len < 28 || rd16(p) != 1 || rd16(p + 2) != ETH_P_IP || p[4] != 6 || p[5] != 4) return;
    u16 op = rd16(p + 6);
    const u8 *sha = p + 8;
    u32 spa = rd32(p + 14), tpa = rd32(p + 24);
    if (spa == 0 || spa == 0xFFFFFFFFu || (sha[0] & 1)) return;
    bool for_me = d->addr && tpa == d->addr;
    arp_learn(d, spa, sha, for_me);                            /* update what we know; only create entries for our own conversations */
    if (op == 1 && for_me && spa != tpa) arp_send(d, 2, sha, spa, sha);
}

int eth_output(NetDev *d, u32 nexthop, u16 type, NetBuf *nb) {
    if (!(d->flags & NDF_UP)) { nb_free(nb); return -ENETDOWN; }
    u8 dmac[6];
    if (d->flags & NDF_NOARP) {
        memset(dmac, 0, 6);
    } else if (nexthop == 0xFFFFFFFFu || (d->bcast && nexthop == d->bcast)) {
        memcpy(dmac, g_bcast_mac, 6);
    } else if ((nexthop >> 28) == 14) {
        dmac[0] = 0x01; dmac[1] = 0x00; dmac[2] = 0x5e; dmac[3] = (u8)((nexthop >> 16) & 0x7f); dmac[4] = (u8)(nexthop >> 8); dmac[5] = (u8)nexthop;
    } else {
        ArpEnt *e = arp_find(nexthop);
        if (e && e->state >= 2) {
            memcpy(dmac, e->mac, 6);
        } else {
            if (!d->addr) { nb_free(nb); return -ENETUNREACH; }
            if (!e) {
                e = arp_slot();
                if (!e) { nb_free(nb); return -ENOBUFS; }
                memset(e, 0, sizeof *e);
                e->ip = nexthop;
                e->state = 1;
                e->dev = d;
                e->updated = net_now();
            }
            if (e->q[ARP_QUEUE - 1]) { nb_free(e->q[0]); for (int i = 0; i < ARP_QUEUE - 1; i++) e->q[i] = e->q[i + 1]; e->q[ARP_QUEUE - 1] = NULL; }
            for (int i = 0; i < ARP_QUEUE; i++) if (!e->q[i]) { e->q[i] = nb; nb = NULL; break; }
            if (nb) nb_free(nb);
            if (e->tries == 0 || net_now() >= e->next_try) {
                e->tries++;
                e->next_try = net_now() + 1000;
                arp_send(d, 1, NULL, nexthop, g_bcast_mac);
            }
            return 0;
        }
    }
    return eth_send(d, dmac, type, nb);
}

void arp_tick(u64 now) {
    for (int i = 0; i < ARP_MAX; i++) {
        ArpEnt *e = &g_arp[i];
        if (e->state == 1 && now >= e->next_try) {
            if (e->tries >= 3) {                              /* nobody answered: "no route to host" for whoever was waiting */
                for (int j = 0; j < ARP_QUEUE; j++) {
                    NetBuf *q = e->q[j];
                    if (q && q->len >= IP_HLEN + 4 && (nb_data(q)[0] >> 4) == 4) {
                        const u8 *ip = nb_data(q);
                        size_t ihl = (size_t)(ip[0] & 15) * 4;
                        if (ihl >= IP_HLEN && q->len >= ihl + 4) {
                            u32 src = rd32(ip + 12), dst = rd32(ip + 16);
                            u16 sport = rd16(ip + ihl), dport = rd16(ip + ihl + 2);
                            if (ip[9] == IPPROTO_TCP) tcp_error(src, sport, dst, dport, EHOSTUNREACH);
                            else if (ip[9] == IPPROTO_UDP) udp_error(src, sport, dst, dport, EHOSTUNREACH);
                        }
                    }
                    nb_free(q);
                    e->q[j] = NULL;
                }
                e->state = 0;
            } else {
                e->tries++;
                e->next_try = now + 1000;
                if (e->dev) arp_send(e->dev, 1, NULL, e->ip, g_bcast_mac);
            }
        } else if (e->state == 2 && now - e->updated > 300000) {
            e->state = 0;                                      /* five minutes without a word: ask again when needed */
        }
    }
}

void arp_flush_dev(NetDev *d) {
    for (int i = 0; i < ARP_MAX; i++) {
        ArpEnt *e = &g_arp[i];
        if (!e->state || e->state == 3 || e->dev != d) continue;
        for (int j = 0; j < ARP_QUEUE; j++) { nb_free(e->q[j]); e->q[j] = NULL; }
        e->state = 0;
    }
}

int arp_list(int index, struct gnet_arp *out) {
    int n = 0;
    for (int i = 0; i < ARP_MAX; i++) {
        ArpEnt *e = &g_arp[i];
        if (!e->state) continue;
        if (n++ != index) continue;
        memset(out, 0, sizeof *out);
        out->index = (u32)index;
        out->ip = e->ip;
        memcpy(out->mac, e->mac, 6);
        out->state = e->state == 1 ? 0 : e->state == 2 ? 1 : 2;
        if (e->dev) strlcpy(out->ifname, e->dev->name, sizeof out->ifname);
        return 0;
    }
    return -ENOENT;
}

int arp_set(u32 ip, const u8 *mac) {
    ArpEnt *e = arp_find(ip);
    if (!e) { e = arp_slot(); if (!e) return -ENOSPC; memset(e, 0, sizeof *e); e->ip = ip; }
    Route *r = ip_route(ip);
    e->dev = r ? r->dev : NULL;
    memcpy(e->mac, mac, 6);
    e->state = 3;
    e->updated = net_now();
    return 0;
}

int arp_del(u32 ip) {
    ArpEnt *e = arp_find(ip);
    if (!e) return -ENOENT;
    for (int j = 0; j < ARP_QUEUE; j++) { nb_free(e->q[j]); e->q[j] = NULL; }
    e->state = 0;
    return 0;
}

/* ------------------------------------------------------------------ IPv4: reassembly */

#define REASM_MAX    4
#define REASM_BYTES  16384
#define REASM_UNITS  (REASM_BYTES / 8)

typedef struct Reasm {
    bool used;
    u32 src, dst;
    u16 id;
    u8  proto;
    u64 deadline;
    u8  *data;                  /* payload so far */
    u8  got[REASM_UNITS / 8];   /* one bit per 8-byte unit that has arrived */
    u32 total;                  /* payload length once the last fragment is seen, else 0 */
    u32 nunits_total;
} Reasm;

static Reasm g_reasm[REASM_MAX];
static u16 g_ip_id;

static void reasm_free(Reasm *r) {
    kfree(r->data);
    memset(r, 0, sizeof *r);
}

/* Takes one fragment (payload only). Returns the whole payload (valid until the next call into reassembly) once complete. */
static const u8 *reasm_add(u32 src, u32 dst, u16 id, u8 proto, u32 off, bool more, const u8 *p, size_t len, size_t *out_len) {
    u64 now = net_now();
    Reasm *r = NULL, *spare = NULL;
    for (int i = 0; i < REASM_MAX; i++) {
        Reasm *c = &g_reasm[i];
        if (c->used && now > c->deadline) reasm_free(c);
        if (c->used && c->src == src && c->dst == dst && c->id == id && c->proto == proto) r = c;
        else if (!c->used && !spare) spare = c;
    }
    if (!r) {
        if (!spare) return NULL;                                  /* table full: a flood of fragments gets no memory */
        r = spare;
        r->data = kzalloc(REASM_BYTES);
        if (!r->data) return NULL;
        r->used = true;
        r->src = src; r->dst = dst; r->id = id; r->proto = proto;
        r->deadline = now + 30000;
    }
    if (off + len > REASM_BYTES || (more && (len & 7)) || len == 0) { reasm_free(r); return NULL; }   /* absurd or misaligned: drop the datagram */
    memcpy(r->data + off, p, len);
    for (u32 u = off / 8; u < (off + len + 7) / 8; u++) r->got[u >> 3] |= (u8)(1u << (u & 7));
    if (!more) {
        r->total = (u32)(off + len);
        r->nunits_total = (r->total + 7) / 8;
    }
    if (!r->total) return NULL;
    for (u32 u = 0; u < r->nunits_total; u++) if (!(r->got[u >> 3] & (1u << (u & 7)))) return NULL;
    *out_len = r->total;
    return r->data;                                              /* caller must call reasm_done() */
}

static void reasm_done(u32 src, u32 dst, u16 id, u8 proto) {
    for (int i = 0; i < REASM_MAX; i++) {
        Reasm *c = &g_reasm[i];
        if (c->used && c->src == src && c->dst == dst && c->id == id && c->proto == proto) reasm_free(c);
    }
}

void ip_tick(u64 now) {
    for (int i = 0; i < REASM_MAX; i++) if (g_reasm[i].used && now > g_reasm[i].deadline) reasm_free(&g_reasm[i]);
}

/* ------------------------------------------------------------------ IPv4: input */

static bool ip_for_us(NetDev *d, u32 dst) {
    if (dst == 0xFFFFFFFFu || (dst >> 24) == 127 || net_is_local(dst)) return true;
    for (NetDev *o = g_devs; o; o = o->next) if (o->bcast && dst == o->bcast) return true;
    if (!d->addr && !(dst >> 28 == 14)) return true;               /* no address yet (DHCP): unicast to our MAC is for us */
    return false;
}

const u8 *g_net_rx_ip;
size_t g_net_rx_iplen;

static void ip_deliver(NetDev *d, u8 proto, const u8 *p, size_t len, u32 src, u32 dst, const u8 *iphdr, size_t iplen) {
    g_net_rx_ip = iphdr;
    g_net_rx_iplen = iplen;
    switch (proto) {
    case IPPROTO_ICMP: icmp_input(d, p, len, src, dst); break;
    case IPPROTO_UDP: udp_input(d, p, len, src, dst); break;
    case IPPROTO_TCP: tcp_input(d, p, len, src, dst); break;
    default:
        if (iphdr && dst != 0xFFFFFFFFu) icmp_send_unreachable(iphdr, iplen, 2);       /* protocol unreachable */
        break;
    }
    g_net_rx_ip = NULL;
    g_net_rx_iplen = 0;
}

void ip_input(NetDev *d, NetBuf *nb) {
    u8 *ip = nb_data(nb);
    size_t len = nb->len;
    if (len < IP_HLEN || (ip[0] >> 4) != 4) goto bad;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if (ihl < IP_HLEN || ihl > len) goto bad;
    size_t tot = rd16(ip + 2);
    if (tot < ihl || tot > len) goto bad;
    if (net_csum(ip, ihl, 0) != 0xFFFF) goto bad;                  /* header checksum */
    u32 src = rd32(ip + 12), dst = rd32(ip + 16);
    if (ip[8] == 0) goto bad;                                       /* TTL 0 never leaves a router */
    if (src == 0xFFFFFFFFu || (src >> 28) == 14 || ((src >> 24) == 127 && !(d->flags & NDF_LOOPBACK))) goto bad;
    if (!ip_for_us(d, dst)) { g_net_ip_forward_ignored++; nb_free(nb); return; }      /* we are a host, not a router */
    u8 proto = ip[9];
    const u8 *payload = ip + ihl;
    size_t plen = tot - ihl;
    u16 fo = rd16(ip + 6);
    if (fo & 0x3FFF) {                                              /* a fragment (offset or more-fragments set) */
        size_t full = 0;
        u32 off = (u32)(fo & 0x1FFF) * 8;
        bool more = (fo & 0x2000) != 0;
        if (more && (plen & 7)) goto bad;
        const u8 *whole = reasm_add(src, dst, rd16(ip + 4), proto, off, more, payload, plen, &full);
        if (whole) {
            u8 hdr[IP_HLEN + 8];
            size_t hl = MIN(tot, sizeof hdr);
            memcpy(hdr, ip, hl);
            ip_deliver(d, proto, whole, full, src, dst, hdr, hl);
            reasm_done(src, dst, rd16(ip + 4), proto);
        }
        nb_free(nb);
        return;
    }
    ip_deliver(d, proto, payload, plen, src, dst, ip, MIN(tot, ihl + 8));
    nb_free(nb);
    return;
bad:
    g_net_stat_ip_bad++;
    d->rx_errors++;
    nb_free(nb);
}

/* ------------------------------------------------------------------ IPv4: output */

static int ip_pick(u32 *src, u32 dst, NetDev **dev, u32 *nexthop) {
    *nexthop = dst;
    *dev = NULL;
    if (dst == 0xFFFFFFFFu) {
        for (NetDev *d = g_devs; d && !*dev; d = d->next)
            if (!(d->flags & NDF_LOOPBACK) && (d->flags & NDF_UP) && (!*src || d->addr == *src)) *dev = d;
        if (!*dev) *dev = g_lo;
    } else if ((dst >> 24) == 127 || net_is_local(dst)) {
        *dev = g_lo;
    } else {
        Route *r = ip_route(dst);
        if (!r) return -ENETUNREACH;
        *dev = r->dev;
        if (r->gw) *nexthop = r->gw;
    }
    if (!*dev || !((*dev)->flags & NDF_UP)) return -ENETDOWN;
    if (!*src) {
        if (dst == 0xFFFFFFFFu) *src = (*dev == g_lo) ? INADDR_LOOPBACK : (*dev)->addr;
        else *src = ((dst >> 24) == 127) ? INADDR_LOOPBACK : (*dev == g_lo ? dst : (*dev)->addr);
    }
    return 0;
}

static void ip_header(u8 *h, u8 tos, u16 totlen, u16 id, u16 fragfield, u8 ttl, u8 proto, u32 src, u32 dst) {
    h[0] = 0x45; h[1] = tos;
    wr16(h + 2, totlen);
    wr16(h + 4, id);
    wr16(h + 6, fragfield);
    h[8] = ttl; h[9] = proto;
    h[10] = h[11] = 0;
    wr32(h + 12, src); wr32(h + 16, dst);
    u16 c = (u16)~net_csum(h, IP_HLEN, 0);
    memcpy(h + 10, &c, 2);
}

/* Sends one packet out of a given device, bypassing the routing table (DHCP, before the interface has an address). */
int ip_send_on(NetDev *dev, u32 src, u32 dst, u8 proto, NetBuf *nb) {
    size_t plen = nb->len;
    if (plen + IP_HLEN > dev->mtu) { nb_free(nb); return -EMSGSIZE; }
    ip_header(nb_push(nb, IP_HLEN), 0, (u16)(plen + IP_HLEN), ++g_ip_id, 0, 64, proto, src, dst);
    return eth_output(dev, dst, ETH_P_IP, nb);
}

int ip_output_buf(u32 src, u32 dst, u8 proto, u8 ttl, u8 tos, bool df, const u8 *data, size_t plen) {
    NetDev *dev;
    u32 nexthop;
    int rc = ip_pick(&src, dst, &dev, &nexthop);
    if (rc) return rc;
    size_t mtu = dev->mtu;
    if (plen + IP_HLEN <= mtu) {
        NetBuf *nb = nb_alloc();
        if (!nb) return -ENOBUFS;
        memcpy(nb_put(nb, plen), data, plen);
        ip_header(nb_push(nb, IP_HLEN), tos, (u16)(plen + IP_HLEN), df ? 0 : ++g_ip_id, df ? 0x4000 : 0, ttl, proto, src, dst);
        return eth_output(dev, nexthop, ETH_P_IP, nb);
    }
    if (df) return -EMSGSIZE;
    size_t per = (mtu - IP_HLEN) & ~(size_t)7;                        /* every fragment but the last carries a multiple of 8 bytes */
    u16 id = ++g_ip_id;
    for (size_t off = 0; off < plen && !rc; off += per) {
        size_t n = MIN(per, plen - off);
        NetBuf *f = nb_alloc();
        if (!f) return -ENOBUFS;
        memcpy(nb_put(f, n), data + off, n);
        ip_header(nb_push(f, IP_HLEN), tos, (u16)(n + IP_HLEN), id, (u16)((off / 8) | (off + n < plen ? 0x2000 : 0)), ttl, proto, src, dst);
        rc = eth_output(dev, nexthop, ETH_P_IP, f);
    }
    return rc;
}

int ip_output(u32 src, u32 dst, u8 proto, u8 ttl, u8 tos, bool df, NetBuf *nb) {
    NetDev *dev;
    u32 nexthop;
    int rc = ip_pick(&src, dst, &dev, &nexthop);
    if (rc) { nb_free(nb); return rc; }
    size_t plen = nb->len;
    if (plen + IP_HLEN <= dev->mtu) {
        ip_header(nb_push(nb, IP_HLEN), tos, (u16)(plen + IP_HLEN), df ? 0 : ++g_ip_id, df ? 0x4000 : 0, ttl, proto, src, dst);
        return eth_output(dev, nexthop, ETH_P_IP, nb);
    }
    rc = ip_output_buf(src, dst, proto, ttl, tos, df, nb_data(nb), plen);
    nb_free(nb);
    return rc;
}

/* ------------------------------------------------------------------ ICMP */

static u64 g_icmp_window;
static int g_icmp_sent;

static bool icmp_allowed(void) {                                  /* at most 200 error/echo messages per second: no amplification */
    u64 now = net_now();
    if (now - g_icmp_window >= 1000) { g_icmp_window = now; g_icmp_sent = 0; }
    return g_icmp_sent++ < 200;
}

static void icmp_send(u32 src, u32 dst, u8 type, u8 code, u32 rest, const u8 *data, size_t dlen) {
    NetBuf *nb = nb_alloc();
    if (!nb || dlen > 1400) { nb_free(nb); return; }
    u8 *m = nb_put(nb, 8 + dlen);
    m[0] = type; m[1] = code; m[2] = m[3] = 0;
    wr32(m + 4, rest);
    memcpy(m + 8, data, dlen);
    u16 c = (u16)~net_csum(m, 8 + dlen, 0);
    memcpy(m + 2, &c, 2);
    ip_output(src, dst, IPPROTO_ICMP, 64, 0, false, nb);
}

void icmp_send_unreachable(const u8 *orig, size_t len, u8 code) {
    if (len < IP_HLEN) return;
    u32 osrc = rd32(orig + 12), odst = rd32(orig + 16);
    if (osrc == 0 || osrc == 0xFFFFFFFFu || (osrc >> 28) == 14 || odst == 0xFFFFFFFFu || (odst >> 28) == 14) return;
    NetDev *dd = netdev_for_addr(odst);
    if (!dd && (odst >> 24) != 127) return;                         /* only for packets that really were addressed to us */
    if ((rd16(orig + 6) & 0x1FFF) != 0) return;                      /* not for non-first fragments */
    if (orig[9] == IPPROTO_ICMP && len >= (size_t)(orig[0] & 15) * 4 + 1) {
        u8 t = orig[(orig[0] & 15) * 4];
        if (t != 8 && t != 0) return;                               /* never answer an ICMP error with an ICMP error */
    }
    if (!icmp_allowed()) return;
    size_t n = MIN(len, (size_t)(orig[0] & 15) * 4 + 8);
    icmp_send(odst, osrc, 3, code, 0, orig, n);
}

static int icmp_errno(u8 type, u8 code) {
    if (type == 3) {
        switch (code) {
        case 0: return ENETUNREACH;
        case 1: return EHOSTUNREACH;
        case 2: case 3: return ECONNREFUSED;
        case 4: return EMSGSIZE;
        default: return EHOSTUNREACH;
        }
    }
    if (type == 11) return EHOSTUNREACH;
    return 0;
}

void icmp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst) {
    (void)d;
    if (len < 8 || net_csum(p, len, 0) != 0xFFFF) { g_net_stat_ip_bad++; return; }
    u8 type = p[0], code = p[1];
    if (type == 8) {                                                /* echo request */
        if (dst == 0xFFFFFFFFu || (dst >> 28) == 14 || !icmp_allowed()) return;
        NetBuf *nb = nb_alloc();
        if (!nb || len > 1400) { nb_free(nb); return; }
        u8 *m = nb_put(nb, len);
        memcpy(m, p, len);
        m[0] = 0;
        m[2] = m[3] = 0;
        u16 c = (u16)~net_csum(m, len, 0);
        memcpy(m + 2, &c, 2);
        ip_output(dst, src, IPPROTO_ICMP, 64, 0, false, nb);
        return;
    }
    if (type == 0) {                                                /* echo reply: for the ping sockets */
        /* rebuild nothing: the socket layer wants the whole IP packet, which is gone by now; give it a synthetic header */
        u8 buf[IP_HLEN + 1500];
        if (len > 1500) return;
        memset(buf, 0, IP_HLEN);
        buf[0] = 0x45; buf[8] = 64; buf[9] = IPPROTO_ICMP;
        wr16(buf + 2, (u16)(IP_HLEN + len));
        wr32(buf + 12, src); wr32(buf + 16, dst);
        memcpy(buf + IP_HLEN, p, len);
        icmp_sock_deliver(buf, IP_HLEN + len);
        return;
    }
    if ((type == 3 || type == 11 || type == 4 || type == 12) && len >= 8 + IP_HLEN) {
        const u8 *o = p + 8;
        size_t olen = len - 8;
        if ((o[0] >> 4) != 4) return;
        size_t oihl = (size_t)(o[0] & 15) * 4;
        if (oihl < IP_HLEN || olen < oihl + 8) return;
        u32 osrc = rd32(o + 12), odst = rd32(o + 16);
        u8 oproto = o[9];
        const u8 *t = o + oihl;
        int err = icmp_errno(type, code);
        if (!net_is_local(osrc)) return;                             /* a quoted packet we never sent */
        if (oproto == IPPROTO_UDP && err) udp_error(osrc, rd16(t), odst, rd16(t + 2), err);
        else if (oproto == IPPROTO_TCP && err) tcp_error(osrc, rd16(t), odst, rd16(t + 2), err);
        else if (oproto == IPPROTO_ICMP) {
            u8 buf[IP_HLEN + 1500];
            if (len > 1500) return;
            memset(buf, 0, IP_HLEN);
            buf[0] = 0x45; buf[8] = 64; buf[9] = IPPROTO_ICMP;
            wr16(buf + 2, (u16)(IP_HLEN + len));
            wr32(buf + 12, src); wr32(buf + 16, dst);
            memcpy(buf + IP_HLEN, p, len);
            icmp_sock_deliver(buf, IP_HLEN + len);
        }
    }
}

/* ------------------------------------------------------------------ the Yukari thread */

static void net_poll_once(void) {
    u64 now = net_now();
    for (NetDev *d = g_devs; d; d = d->next) if (d->poll) d->poll(d);
    lo_drain();
    tcp_tick(now);
    arp_tick(now);
    ip_tick(now);
    dhcp_tick(now);
}

static void yukari_main(void *arg) {
    (void)arg;
    for (;;) {
        net_lock();
        net_poll_once();
        bool busy = lo_pending();
        bool hw = net_have_hardware();
        net_unlock();
        if (busy) { task_yield(); continue; }
        u64 fl = irq_save();
        if (!g_kicked) wq_block_timeout(&g_yukari_wq, hw ? 1 : 20);     /* polling at 1 kHz only when a real card is there */
        g_kicked = false;
        irq_restore(fl);
    }
}

/* ------------------------------------------------------------------ /proc/net */

static size_t gen_dev(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "Inter-|   Receive                                                |  Transmit\n"
                                        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    net_lock();
    for (NetDev *d = g_devs; d && n + 200 < cap; d = d->next)
        n += (size_t)snprintf(b + n, cap - n, "%6s: %lu %lu %lu %lu 0 0 0 0 %lu %lu %lu %lu 0 0 0 0\n", d->name, (unsigned long)d->rx_bytes,
                              (unsigned long)d->rx_packets, (unsigned long)d->rx_errors, (unsigned long)d->rx_dropped, (unsigned long)d->tx_bytes,
                              (unsigned long)d->tx_packets, (unsigned long)d->tx_errors, (unsigned long)d->tx_dropped);
    net_unlock();
    return n;
}

static size_t gen_dhcp(char *b, size_t cap) {
    net_lock();
    size_t n = dhcp_proc(b, cap);
    net_unlock();
    return n;
}

static size_t gen_route(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    net_lock();
    for (int i = 0; i < MAX_ROUTES && n + 160 < cap; i++) {
        Route *r = &g_routes[i];
        if (!r->used) continue;
        n += (size_t)snprintf(b + n, cap - n, "%s\t%08X\t%08X\t%04X\t0\t0\t%u\t%08X\t0\t0\t0\n", r->dev->name, hton32(r->dst), hton32(r->gw),
                              1 | (r->gw ? 2 : 0), r->metric, hton32(r->mask));
    }
    net_unlock();
    return n;
}

static size_t gen_arp(char *b, size_t cap) {
    size_t n = (size_t)snprintf(b, cap, "IP address       HW type     Flags       HW address            Mask     Device\n");
    net_lock();
    for (int i = 0; i < ARP_MAX && n + 160 < cap; i++) {
        ArpEnt *e = &g_arp[i];
        if (!e->state) continue;
        char ip[20];
        net_ip_str(e->ip, ip, sizeof ip);
        n += (size_t)snprintf(b + n, cap - n, "%-15s  0x1         0x%x         %02x:%02x:%02x:%02x:%02x:%02x     *        %s\n", ip,
                              e->state == 1 ? 0 : e->state == 3 ? 6 : 2, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                              e->dev ? e->dev->name : "?");
    }
    net_unlock();
    return n;
}

static size_t gen_tcp(char *b, size_t cap) { return net_proc_tcp(b, cap); }
static size_t gen_udp(char *b, size_t cap) { return net_proc_udp(b, cap); }

static size_t gen_tune(char *b, size_t cap) {
    return (size_t)snprintf(b, cap,
        "lo_drop %d\nlo_dup %d\nlo_reorder %d\nlo_corrupt %d\nrto_min_ms %d\nsyn_retries %d\ndata_retries %d\ntimewait_ms %d\n"
        "ip_bad %lu\ntcp_bad %lu\nbuffers %d\n",
        g_net_lo_drop, g_net_lo_dup, g_net_lo_reorder, g_net_lo_corrupt, g_net_rto_min_ms, g_net_syn_retries, g_net_data_retries,
        g_net_timewait_ms, (unsigned long)g_net_stat_ip_bad, (unsigned long)g_net_stat_tcp_bad, g_nb_count);
}

static ssize_t put_tune(const char *buf, size_t n) {
    char line[64];
    size_t pos = 0;
    while (pos < n) {
        size_t l = 0;
        while (pos < n && buf[pos] != '\n' && l < sizeof line - 1) line[l++] = buf[pos++];
        while (pos < n && buf[pos] != '\n') pos++;                    /* overlong line: ignore the rest of it */
        if (pos < n) pos++;
        line[l] = 0;
        char *sp = line;
        while (*sp && *sp != ' ' && *sp != '=') sp++;
        if (!*sp) continue;
        *sp++ = 0;
        while (*sp == ' ' || *sp == '=') sp++;
        int v = 0;
        bool neg = false;
        const char *q = sp;
        if (*q == '-') { neg = true; q++; }
        if (*q < '0' || *q > '9') continue;
        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
        if (neg) v = -v;
        net_lock();
        if (!strcmp(line, "lo_drop")) g_net_lo_drop = MAX(0, MIN(v, 100));
        else if (!strcmp(line, "lo_dup")) g_net_lo_dup = MAX(0, MIN(v, 100));
        else if (!strcmp(line, "lo_reorder")) g_net_lo_reorder = MAX(0, MIN(v, 100));
        else if (!strcmp(line, "lo_corrupt")) g_net_lo_corrupt = MAX(0, MIN(v, 100));
        else if (!strcmp(line, "rto_min_ms")) g_net_rto_min_ms = MAX(1, MIN(v, 5000));
        else if (!strcmp(line, "syn_retries")) g_net_syn_retries = MAX(0, MIN(v, 12));
        else if (!strcmp(line, "data_retries")) g_net_data_retries = MAX(1, MIN(v, 30));
        else if (!strcmp(line, "timewait_ms")) g_net_timewait_ms = MAX(100, MIN(v, 120000));
        net_unlock();
    }
    return (ssize_t)n;
}

/* Test hook: one raw Ethernet frame per write(), delivered to the stack as if it had arrived on lo. Root only. */
static ssize_t put_inject(const char *buf, size_t n) {
    if (n < ETH_HLEN || n > 9000) return -EINVAL;
    NetBuf *nb = nb_alloc();
    if (!nb) return -ENOMEM;
    size_t take = MIN(n, nb_room(nb));
    memcpy(nb_put(nb, take), buf, take);
    net_lock();
    net_rx(g_lo, nb);
    net_unlock();
    return (ssize_t)n;
}

/* ------------------------------------------------------------------ netctl */

static void if_fill(NetDev *d, struct gnet_if *o) {
    memset(o, 0, sizeof *o);
    o->index = (u32)d->index;
    strlcpy(o->name, d->name, sizeof o->name);
    strlcpy(o->driver, d->driver, sizeof o->driver);
    memcpy(o->mac, d->mac, 6);
    o->addr = d->addr; o->mask = d->mask; o->bcast = d->bcast;
    o->mtu = d->mtu;
    o->flags = (d->flags & NDF_UP ? GIFF_UP : 0) | (d->flags & NDF_BROADCAST ? GIFF_BROADCAST : 0) | (d->flags & NDF_LOOPBACK ? GIFF_LOOPBACK : 0) |
               (d->flags & NDF_LINK ? GIFF_RUNNING : 0) | (d->flags & NDF_NOARP ? GIFF_NOARP : 0) | (d->flags & NDF_DHCP ? GIFF_DHCP : 0);
    o->rx_packets = d->rx_packets; o->tx_packets = d->tx_packets; o->rx_bytes = d->rx_bytes; o->tx_bytes = d->tx_bytes;
    o->rx_errors = d->rx_errors; o->tx_errors = d->tx_errors; o->rx_dropped = d->rx_dropped; o->tx_dropped = d->tx_dropped;
}

long net_ctl(unsigned long cmd, void *arg) {
    Process *p = current->proc;
    bool root = !p || p->cred.uid == 0;
    long rc = 0;
    net_lock();
    switch (cmd) {
    case NETCTL_IF_GET: {
        struct gnet_if *r = arg;
        NetDev *d = netdev_by_index((int)r->index);
        if (!d) rc = -ENODEV; else if_fill(d, r);
        break;
    }
    case NETCTL_IF_CONFIG: {
        struct gnet_if *r = arg;
        r->name[sizeof r->name - 1] = 0;
        NetDev *d = netdev_find(r->name);
        if (!root) { rc = -EPERM; break; }
        if (!d) { rc = -ENODEV; break; }
        if (r->mtu) {
            if (r->mtu < 68 || r->mtu > 1500) { rc = -EINVAL; break; }
            d->mtu = r->mtu;
        }
        if (r->flags & GIFF_UP) d->flags |= NDF_UP;
        else if (r->flags & GIFF_DOWN) { d->flags &= ~(u32)NDF_UP; route_flush_dev(d); }
        if (r->flags & GIFF_CLEARADDR) {
            route_flush_dev(d);
            d->flags &= ~(u32)NDF_DHCP;
            netdev_set_addr(d, 0, 0);
        } else if (r->mask || r->addr) {
            if (!r->mask) r->mask = 0xFFFFFF00u;
            dhcp_stop(d);                                           /* an address set by hand is final: a late DHCP offer must not replace it */
            route_flush_dev(d);
            d->flags &= ~(u32)NDF_DHCP;
            netdev_set_addr(d, r->addr, r->mask);
            if (d->flags & NDF_UP) arp_announce(d);
        }
        break;
    }
    case NETCTL_ROUTE_GET: {
        struct gnet_route *r = arg;
        rc = route_list((int)r->index, r);
        break;
    }
    case NETCTL_ROUTE_ADD: {
        struct gnet_route *r = arg;
        if (!root) { rc = -EPERM; break; }
        r->ifname[sizeof r->ifname - 1] = 0;
        NetDev *d = r->ifname[0] ? netdev_find(r->ifname) : NULL;
        if (!d) {                                                   /* find the device that can reach the gateway */
            Route *rt = ip_route(r->gw ? r->gw : r->dst);
            d = rt ? rt->dev : NULL;
        }
        if (!d) { rc = -ENETUNREACH; break; }
        rc = route_add(r->dst & r->mask, r->mask, r->gw, d, r->metric);
        break;
    }
    case NETCTL_ROUTE_DEL: {
        struct gnet_route *r = arg;
        if (!root) { rc = -EPERM; break; }
        rc = route_del(r->dst & r->mask, r->mask);
        break;
    }
    case NETCTL_ARP_GET: { struct gnet_arp *a = arg; rc = arp_list((int)a->index, a); break; }
    case NETCTL_ARP_SET: { struct gnet_arp *a = arg; rc = root ? arp_set(a->ip, a->mac) : -EPERM; break; }
    case NETCTL_ARP_DEL: { struct gnet_arp *a = arg; rc = root ? arp_del(a->ip) : -EPERM; break; }
    case NETCTL_DHCP: {
        struct gnet_if *r = arg;
        r->name[sizeof r->name - 1] = 0;
        NetDev *d = netdev_find(r->name);
        if (!root) rc = -EPERM; else if (!d || (d->flags & NDF_LOOPBACK)) rc = -ENODEV; else dhcp_start(d);
        break;
    }
    case NETCTL_DHCP_STOP: {
        struct gnet_if *r = arg;
        r->name[sizeof r->name - 1] = 0;
        NetDev *d = netdev_find(r->name);
        if (!root) rc = -EPERM; else if (!d || (d->flags & NDF_LOOPBACK)) rc = -ENODEV; else dhcp_stop(d);
        break;
    }
    default: rc = -EINVAL;
    }
    net_unlock();
    net_kick();
    return rc;
}

/* ------------------------------------------------------------------ setup */

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

static void net_init(void) {
    mutex_init(&g_mtx);
    wq_init(&g_yukari_wq);
    g_ip_id = (u16)random_u64();
    NetDev *lo = kzalloc(sizeof *lo);
    if (!lo) return;
    strlcpy(lo->driver, "loopback", sizeof lo->driver);
    lo->mtu = 1500;
    lo->flags = NDF_LOOPBACK | NDF_NOARP | NDF_LINK;
    lo->tx = lo_tx;
    g_lo = netdev_register("lo", lo);
    snprintf(lo->name, sizeof lo->name, "lo");
    netdev_set_addr(lo, INADDR_LOOPBACK, 0xFF000000u);
    procfs_register("net/dev", gen_dev);
    procfs_register("net/route", gen_route);
    procfs_register("net/arp", gen_arp);
    procfs_register("net/dhcp", gen_dhcp);
    procfs_register("net/tcp", gen_tcp);
    procfs_register("net/udp", gen_udp);
    procfs_register_rw("net/tune", gen_tune, put_tune);
    procfs_register_rw("net/inject", gen_tune, put_inject);
    task_create_detached("Yukari", yukari_main, NULL);
    klog("Yukari", "the gap-opener is awake: loopback up, 127.0.0.1/8");
}
INITCALL(net_init);
