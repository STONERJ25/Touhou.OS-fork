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
#include "cmdline.h"
#include "ntp.h"

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
    u32  ntp[4];                        /* option 42: the NTP servers the lease names (kernel/net/ntp.c asks them first) */
    int  nntp;
    u32  lease_s;
    u64  next_send, t1, expire, started;
    int  tries;
    bool resolv_dirty;
    bool silence_logged;                /* "nobody answers" was said once; do not repeat it every minute */
    bool held;                          /* an automatic start is waiting for netconf (see "The hold" below) */
} Dhcp;

static u32 g_dns[3];
static size_t g_ndns;
static u32 g_ntp[4];
static size_t g_nntp;
static bool g_trace;                    /* "nicdebug": every DHCP message sent and every reply seen is logged, and why a reply was not used */

size_t dhcp_dns(u32 *servers, size_t max) {
    size_t n = MIN(max, g_ndns);
    for (size_t i = 0; i < n; i++) servers[i] = g_dns[i];
    return n;
}

size_t dhcp_ntp(u32 *servers, size_t max) {
    size_t n = MIN(max, g_nntp);
    for (size_t i = 0; i < n; i++) servers[i] = g_ntp[i];
    return n;
}

static void dhcp_unconfigure(NetDev *d) {
    if (d->flags & NDF_DHCP) {
        route_flush_dev(d);
        d->addr = d->mask = d->bcast = 0;
        d->flags &= ~(u32)NDF_DHCP;
    }
}

/* An address set by hand (netconf, ifconfig) ends whatever DHCP was doing: a lease that arrived later would silently replace it. A start that was waiting for netconf is
 * cancelled the same way. */
void dhcp_stop(NetDev *d) {
    Dhcp *c = d->dhcp;
    if (c) { c->state = 0; c->held = false; }
}

static Dhcp *dhcp_get(NetDev *d) {
    Dhcp *c = d->dhcp;
    if (!c) { c = kzalloc(sizeof *c); if (c) d->dhcp = c; }
    return c;
}

static void dhcp_begin(NetDev *d) {
    Dhcp *c = dhcp_get(d);
    if (!c) return;
    c->held = false;
    dhcp_unconfigure(d);
    g_trace = cmdline_flag("nicdebug");
    c->state = D_SELECTING;
    c->xid = (u32)random_u64();
    c->tries = 0;
    c->next_send = 0;                       /* now */
    c->started = net_now();
    net_kick();
}

/* A start the user asked for (`ifconfig eth0 dhcp`) never waits. */
void dhcp_start_now(NetDev *d) {
    if (d->flags & NDF_LOOPBACK) return;
    dhcp_begin(d);
}

/*
 * The hold. A card that network.conf gives a static address must never ask DHCP, but the client starts the moment there is a carrier (the boot, long before init), while
 * netconf, a user-space program, applies the static line only after init has started: a lease could arrive first (QEMU answers within milliseconds), and a real DHCP server
 * would have heard a machine that was told to be static. So the first card to register (netdev_register: the card drivers run before net_init, after the volume is mounted)
 * looks at the network.conf that is kept on the Human Village volume (/HumanVillage/.system/etc: the file init puts into /etc before it runs netconf). When that has a static
 * line, the automatic starts only note that a card wants an address (Dhcp.held), and the client waits for one of three things: init says that netconf has run
 * (NETCTL_NETCONF_DONE), the boot finds that there is no init to run it (the kernel shell, or init did not start), or netconf.wait seconds (20 unless the kernel command line
 * says otherwise, 0 turns the wait off) pass after init started without it. Then every card that waited and still has no address of its own starts. A boot whose kept file
 * has no static line, or that has no such file, is not delayed at all.
 */
static bool g_hold;
static u64 g_hold_wait_ms;
static u64 g_hold_deadline;             /* 0 until init is running: the limit counts from there, so a long repair of the volume at boot does not use it up */

#define NETCONF_KEPT "/HumanVillage/.system/etc/network.conf"

void dhcp_start(NetDev *d) {
    if (d->flags & NDF_LOOPBACK) return;
    if (g_hold) {
        Dhcp *c = dhcp_get(d);
        if (c && !c->state) c->held = true;             /* (a client that is already running, because the user asked, is left alone) */
        return;
    }
    dhcp_begin(d);
}

void dhcp_hold_release(const char *why) {
    if (!g_hold) return;
    g_hold = false;
    klog("Yukari", "DHCP may ask now: %s", why);
    NetDev *d;
    for (int idx = 0; (d = netdev_by_index(idx)) != NULL; idx++) {
        Dhcp *c = d->dhcp;
        if (!c || !c->held) continue;
        c->held = false;
        /* a card without a carrier asks when the carrier comes (net_set_link); one that netconf gave an address had its wish cancelled by dhcp_stop */
        if ((d->flags & NDF_LINK) && !(d->flags & NDF_LOOPBACK) && (!d->addr || (d->flags & NDF_DHCP))) dhcp_begin(d);
    }
}

/* A line of network.conf that names a card and says "static" (network.conf(5)). A rough reading is enough here: netconf does the real one, and init says when it is done. */
static bool has_static_line(char *s) {
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        char *hash = strchr(s, '#');
        if (hash) *hash = 0;
        char *p = s;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;                  /* the card's name */
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!strncmp(p, "static", 6) && (!p[6] || p[6] == ' ' || p[6] == '\t' || p[6] == '\r')) return true;
        if (!nl) break;
        s = nl + 1;
    }
    return false;
}

void dhcp_hold_init(void) {
    static bool done;                                                           /* the first card to register decides, once (the card drivers run before net_init) */
    if (done) return;
    done = true;
    long secs = cmdline_int("netconf.wait", 20);
    if (secs <= 0 || cmdline_flag("nonet")) return;
    void *data;
    size_t size;
    if (vfs_read_file(NETCONF_KEPT, &data, &size) != 0) return;                 /* no volume, or no kept file: nothing is static */
    bool wait = has_static_line(data);
    kfree(data);
    if (!wait) return;
    g_hold = true;
    g_hold_wait_ms = (u64)secs * 1000;
    klog("Yukari", "network.conf on the volume has a static line: DHCP waits until netconf has run (at most %ld s after init starts; netconf.wait=0 turns the wait off)", secs);
}

static const char *state_name(int s) {
    switch (s) {
    case D_SELECTING: return "selecting";
    case D_REQUESTING: return "requesting";
    case D_BOUND: return "bound";
    case D_RENEWING: return "renewing";
    default: return "off";
    }
}

/* One line per card: name, state, address, mask, server, gateway, name servers (comma separated), lease length and seconds left. */
size_t dhcp_proc(char *buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "# interface state address mask server gateway dns lease_s left_s\n");
    u64 now = net_now();
    NetDev *d;
    for (int idx = 0; (d = netdev_by_index(idx)) != NULL && n + 200 < cap; idx++) {
        if (d->flags & NDF_LOOPBACK) continue;
        Dhcp *c = d->dhcp;
        int st = c ? c->state : 0;
        bool leased = st == D_BOUND || st == D_RENEWING;
        char a[20] = "-", m[20] = "-", s[20] = "-", r[20] = "-", dns[64] = "-";
        if (leased) {
            net_ip_str(c->offer, a, sizeof a);
            net_ip_str(c->mask, m, sizeof m);
            net_ip_str(c->server, s, sizeof s);
            if (c->router) net_ip_str(c->router, r, sizeof r);
            size_t k = 0;
            dns[0] = 0;
            for (int i = 0; i < c->ndns && k + 18 < sizeof dns; i++) {
                char one[20];
                net_ip_str(c->dns[i], one, sizeof one);
                k += (size_t)snprintf(dns + k, sizeof dns - k, "%s%s", i ? "," : "", one);
            }
            if (!c->ndns) strlcpy(dns, "-", sizeof dns);
        }
        unsigned long left = leased && c->expire > now ? (unsigned long)((c->expire - now) / 1000) : 0;
        n += (size_t)snprintf(buf + n, cap - n, "%s %s %s %s %s %s %s %u %lu\n", d->name, c && c->held ? "held" : state_name(st), a, m, s, r, dns, leased ? c->lease_s : 0, left);
    }
    return n;
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
    if (g_trace) klog("Yukari", "%s: DHCP %s sent: xid %08x, state %d, try %d", d->name, type == DHCP_DISCOVER ? "DISCOVER" : type == DHCP_REQUEST ? "REQUEST" : "message", c->xid, c->state, c->tries);
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
    *o++ = 55; *o++ = 7; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 28; *o++ = 51; *o++ = 42;
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
    c->silence_logged = false;
    u64 now = net_now();
    u32 lease = c->lease_s ? c->lease_s : 3600;
    c->t1 = now + (u64)lease * 500;
    c->expire = now + (u64)lease * 1000;
    g_ndns = 0;
    for (int i = 0; i < c->ndns; i++) g_dns[g_ndns++] = c->dns[i];
    g_nntp = 0;
    for (int i = 0; i < c->nntp; i++) g_ntp[g_nntp++] = c->ntp[i];
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
    if (g_trace) klog("Yukari", "%s: DHCP reply seen: %u bytes, op %u, xid %08x (ours %08x), state %d", d->name, (unsigned)len, len ? p[0] : 0, len >= 8 ? rd32(p + 4) : 0, c ? c->xid : 0, c ? c->state : -1);
    if (!c || len < 241 || p[0] != 2 || p[1] != 1 || p[2] != 6) { if (g_trace) klog("Yukari", "%s: DHCP reply ignored: not a BOOTREPLY for an Ethernet client", d->name); return; }
    if (rd32(p + 4) != c->xid || memcmp(p + 28, d->mac, 6) || rd32(p + 236) != 0x63825363) {
        if (g_trace) klog("Yukari", "%s: DHCP reply ignored: xid %s, hardware address %s, cookie %s", d->name, rd32(p + 4) == c->xid ? "ok" : "not ours",
                          memcmp(p + 28, d->mac, 6) ? "not ours" : "ok", rd32(p + 236) == 0x63825363 ? "ok" : "bad");
        return;
    }
    u32 yiaddr = rd32(p + 16);
    u8 type = 0;
    u32 server = 0, mask = 0, router = 0, lease = 0, dns[3], ntp[4];
    int ndns = 0, nntp = 0;
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
        case 42: for (unsigned k = 0; k + 4 <= l && nntp < 4; k += 4) ntp[nntp++] = rd32(v + k); break;
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
        c->nntp = nntp;
        for (int k = 0; k < nntp; k++) c->ntp[k] = ntp[k];
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
    if (g_hold && g_init_proc) {                                              /* the limit counts from the moment init exists */
        if (!g_hold_deadline) g_hold_deadline = now + g_hold_wait_ms;
        else if (now >= g_hold_deadline) {
            char why[64];
            snprintf(why, sizeof why, "init did not run netconf within %u s", (unsigned)(g_hold_wait_ms / 1000));
            dhcp_hold_release(why);
        }
    }
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
                if (!c->silence_logged) {
                    c->silence_logged = true;
                    klog("Yukari", "%s: no DHCP server answers (yet); the address stays unset and DHCP keeps trying in the background", d->name);
                }
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
