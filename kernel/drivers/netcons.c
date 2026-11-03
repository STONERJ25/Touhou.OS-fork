/*
 * The netconsole: Aya Shameimaru, the crow tengu reporter, carries the kernel log to another machine as it is written (docs/RELIABILITY.md).
 *
 *   netconsole=IP[:PORT]                                  the short form: to IP, port 6666 (default), from port 6665
 *   netconsole=[SPORT]@[SIP]/[DEV],[PORT]@IP/[MAC]        Linux's form: SPORT the source port, DEV the card (eth0); MAC the next hop's
 *                                                         Ethernet address, which skips ARP. SIP is accepted and ignored: datagrams
 *                                                         always leave from the card's own address
 *   netconsole.rate=LINES                                 at most this many lines a second (default 500)
 *
 * Every line of the kernel log becomes one UDP datagram, plain text (the screen's escape sequences taken out), from the moment the network
 * is up; the lines from before are still in the log ring (128 KiB) and go first. tools/netconsole.py on the development PC listens, puts a
 * time on each line and writes it to a file and to the screen.
 *
 * The ordinary path is the "Aya" thread: every 10-50 ms it takes the network lock, waits for a route to the target and for the next hop to
 * answer ARP (no line is sent before: ARP's queue keeps only two packets; a greeting datagram is what makes the stack ask), and sends what
 * is new through ip_output(). In a panic or a fatal exception there is no lock to take and nobody to wait for: netcons_panic_flush() (from
 * panic.c) builds whole Ethernet frames itself, for the card and the next hop's MAC address the ordinary path last used (the broadcast
 * address if ARP never answered), and hands them to the driver's polled transmit entry (NetDev.netpoll_tx: e1000.c, virtio_net.c), one
 * after the other, with interrupts off.
 *
 * Floods: no more than netconsole.rate lines a second leave; when the log runs further ahead than half its ring, the netconsole skips to the
 * newest part and says so in a datagram of its own ("-- netconsole: N lines (B bytes) of the log dropped ... --"); lines the ring overwrote
 * before they could leave are reported the same way. Nothing in the sending path writes to the kernel log: a netconsole that logged its
 * own trouble would send that line too, and log the trouble of sending it. What it is doing is in /proc/netconsole; root can write
 * "flood N" there (N lines into the log at once: the tests' flood).
 */
#include "netcons.h"
#include "panicrec.h"
#include "net.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "ktime.h"
#include "task.h"
#include "cmdline.h"
#include "initcall.h"
#include "fbcon.h"
#include "netabi.h"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

#define SKIP_AT        (64u * 1024)     /* further behind than this (half of printk.c's ring): skip ahead ... */
#define SKIP_KEEP      (8u * 1024)      /* ... to the newest this much */
#define PAYLOAD_MAX    1400             /* the longest datagram: one Ethernet frame, no IP fragments */
#define PANIC_MAX      (32u * 1024)     /* the panic path sends at most the newest this much */
#define DEFAULT_PORT   6666
#define DEFAULT_SPORT  6665
#define DEFAULT_RATE   500

enum { NC_OFF, NC_WAIT_ROUTE, NC_WAIT_ARP, NC_SENDING };

static struct {
    int  state;
    u32  dst;                   /* the target */
    u16  dport, sport;
    char devname[16];           /* "" = the card the route says */
    u8   mac[6];
    bool mac_given;
    u32  rate;
    NetDev *dev;                /* the card the datagrams leave by: the panic path sends through it */
    u32  src, hop;              /* the card's address, the next hop (the target or the gateway) */
    u8   hop_mac[6];
    bool hop_known;             /* hop_mac holds the next hop's address (learned from ARP, or given) */
    u64  sent;                  /* klog offset: everything below it has been sent, or dropped and reported */
    u64  partial_since;         /* a line without its newline has waited since then */
    u64  probe_at;
    u32  probes;
    u32  tokens;
    u64  refill_at;
    u16  ip_id;
    u64  datagrams, bytes, dropped_lines, dropped_bytes, overwritten, errors, full;
    u64  panic_datagrams;
    int  panic_fails;           /* frames in a row the card did not take: at 3 the panic path stops trying */
    bool panic_nocard_said;
} N;

static char g_line[PAYLOAD_MAX + 64];     /* the thread's line */
static char g_pline[PAYLOAD_MAX + 64];    /* the panic path's */
static u8   g_frame[1600];

/* ---- sending ---- */

/* One datagram through the stack (net lock held). */
static int send_udp(const char *data, size_t len) {
    NetBuf *nb = nb_alloc();
    if (!nb) return -ENOBUFS;
    size_t total = 8 + len;
    if (total > nb_room(nb)) { nb_free(nb); return -EMSGSIZE; }
    u8 *u = nb_put(nb, total);
    wr16(u, N.sport); wr16(u + 2, N.dport); wr16(u + 4, (u16)total); u[6] = u[7] = 0;
    memcpy(u + 8, data, len);
    u16 c = (u16)~net_csum(u, total, net_pseudo_csum(N.src, N.dst, IPPROTO_UDP, (u16)total));
    if (!c) c = 0xFFFF;
    memcpy(u + 6, &c, 2);
    return ip_output(N.src, N.dst, IPPROTO_UDP, 64, 0, false, nb);
}

/* A line of the netconsole's own: on the wire only, never in the kernel log. */
static void note(const char *fmt, ...) {
    char t[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (send_udp(t, MIN((size_t)n, sizeof t - 1)) == 0) N.datagrams++;
}

/* Is there a way to the target, and does the next hop's MAC address answer? Net lock held. */
static bool resolve(u64 now) {
    Route *r = ip_route(N.dst);
    NetDev *d = r ? r->dev : NULL;
    if (d && N.devname[0] && strcmp(d->name, N.devname)) d = NULL;          /* a card was named: through that one only */
    if (!d || (d->flags & NDF_LOOPBACK) || !(d->flags & NDF_UP) || !(d->flags & NDF_LINK) || !d->addr) {
        N.state = NC_WAIT_ROUTE;
        return false;
    }
    u32 hop = r->gw ? r->gw : N.dst;
    if (d != N.dev || hop != N.hop) { N.dev = d; N.hop = hop; N.hop_known = N.mac_given; N.probes = 0; N.probe_at = 0; }
    N.src = d->addr;
    if (N.mac_given) { memcpy(N.hop_mac, N.mac, 6); N.state = NC_SENDING; return true; }
    struct gnet_arp a;
    for (int i = 0; arp_list(i, &a) == 0; i++) {
        if (a.ip != hop || a.state == 0) continue;
        memcpy(N.hop_mac, a.mac, 6);
        N.hop_known = true;
        N.state = NC_SENDING;
        return true;
    }
    N.state = NC_WAIT_ARP;
    u64 gap = N.probes < 10 ? 1000 : 10000;                                /* a target that never answers is asked for every 10 s */
    if (!N.probe_at || now - N.probe_at >= gap) {
        N.probe_at = now;
        N.probes++;
        static const char hello[] = "-- netconsole: the TouhouOS kernel log follows --\n";
        send_udp(hello, sizeof hello - 1);                                 /* ARP keeps it until the next hop answers: asking is what it is for */
    }
    return false;
}

/* The log ran further ahead than the netconsole can follow: skip to its newest part and say how much was left out. */
static void catch_up(void) {
    u64 total = klog_total();
    if (total - N.sent <= SKIP_AT) return;
    u64 to = total - SKIP_KEEP;
    char c = 0;
    for (int i = 0; i < 2048 && to < total && klog_read(to, &c, 1) == 1 && c != '\n'; i++) to++;
    if (to < total) to++;                                                   /* the beginning of a line */
    u64 lines = 0, off = N.sent;
    while (off < to) {
        size_t got = klog_read_clamped(&off, g_line, (size_t)MIN((u64)sizeof g_line, to - off));
        if (!got) break;
        for (size_t i = 0; i < got; i++) lines += g_line[i] == '\n';
        off += got;
    }
    N.dropped_lines += lines;
    N.dropped_bytes += to - N.sent;
    note("-- netconsole: %lu lines (%lu bytes) of the log dropped: it came faster than %u lines a second --\n", (unsigned long)lines,
         (unsigned long)(to - N.sent), N.rate);
    N.sent = to;
    N.partial_since = 0;
}

static void send_lines(u64 now, int budget) {
    while (budget-- > 0 && N.tokens > 0) {
        u64 off = N.sent;
        size_t got = klog_read_clamped(&off, g_line, PAYLOAD_MAX);
        if (off > N.sent) {
            N.overwritten += off - N.sent;
            note("-- netconsole: %lu bytes of the log were overwritten before they could be sent --\n", (unsigned long)(off - N.sent));
            N.sent = off;
        }
        if (!got) { N.partial_since = 0; return; }
        size_t e = 0;
        while (e < got && g_line[e] != '\n') e++;
        if (e < got) e++;                                                   /* the line with its newline */
        else if (got < PAYLOAD_MAX) {                                       /* no newline yet: the rest may still come */
            if (!N.partial_since) N.partial_since = now;
            if (now - N.partial_since < 500) return;
        }
        N.partial_since = 0;
        size_t raw = e, len = klog_plain(g_line, raw);
        int rc = len ? send_udp(g_line, len) : 0;
        if (rc == -ENOBUFS) { N.full++; return; }                           /* the card's ring is full: this line again next time */
        if (rc < 0) N.errors++;
        else if (len) { N.datagrams++; N.bytes += len; }
        N.sent += raw;
        N.tokens--;
    }
}

static void step(void) {
    u64 now = net_now();
    if (!resolve(now)) return;
    u64 add = (now - N.refill_at) * N.rate / 1000;
    if (add) { N.tokens = (u32)MIN((u64)N.rate, N.tokens + add); N.refill_at = now; }
    catch_up();
    send_lines(now, 64);
}

static void aya_main(void *arg) {
    (void)arg;
    for (;;) {
        bool busy = N.state == NC_SENDING && klog_total() != N.sent;
        task_sleep_ms(busy ? 10 : 50);
        net_lock();
        step();
        net_unlock();
    }
}

/* ---- the panic path ---- */

static size_t build_frame(u8 *f, const u8 *dmac, const char *data, size_t len) {
    memcpy(f, dmac, 6);
    memcpy(f + 6, N.dev->mac, 6);
    wr16(f + 12, ETH_P_IP);
    u8 *ip = f + ETH_HLEN, *u = ip + IP_HLEN;
    size_t ulen = 8 + len;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (u16)(IP_HLEN + ulen));
    wr16(ip + 4, ++N.ip_id);
    wr16(ip + 6, 0x4000);                                                   /* don't fragment */
    ip[8] = 64; ip[9] = IPPROTO_UDP; ip[10] = ip[11] = 0;
    wr32(ip + 12, N.src); wr32(ip + 16, N.dst);
    u16 c = (u16)~net_csum(ip, IP_HLEN, 0);
    memcpy(ip + 10, &c, 2);
    wr16(u, N.sport); wr16(u + 2, N.dport); wr16(u + 4, (u16)ulen); u[6] = u[7] = 0;
    memcpy(u + 8, data, len);
    c = (u16)~net_csum(u, ulen, net_pseudo_csum(N.src, N.dst, IPPROTO_UDP, (u16)ulen));
    if (!c) c = 0xFFFF;
    memcpy(u + 6, &c, 2);
    size_t n = ETH_HLEN + IP_HLEN + ulen;
    if (n < 60) { memset(f + n, 0, 60 - n); n = 60; }
    return n;
}

static void poll_send(const u8 *dmac, const char *data, size_t len) {
    if (N.panic_fails >= 3) return;
    if (N.dev->netpoll_tx(N.dev, g_frame, build_frame(g_frame, dmac, data, len)) == 0) { N.panic_datagrams++; N.datagrams++; N.panic_fails = 0; }
    else if (++N.panic_fails == 3) {
        static const char t[] = "[Aya] netconsole: the card does not take the frames (a stuck transmit unit?): the rest of the panic is not sent\n";
        console_write(t, sizeof t - 1);                                     /* the screen and the serial port: not the log, not the wire */
    }
}

void netcons_panic_flush(void) {
    if (N.state == NC_OFF || !N.dev || !N.src) return;                      /* not configured, or the network was never up */
    if (!N.dev->netpoll_tx) {
        if (!N.panic_nocard_said) {
            N.panic_nocard_said = true;
            static const char t[] = "[Aya] netconsole: this card's driver has no polled transmit, so the panic is not sent\n";
            console_write(t, sizeof t - 1);
        }
        return;
    }
    if (N.panic_fails >= 3) return;
    static const u8 bcast[6] = { 255, 255, 255, 255, 255, 255 };
    const u8 *dmac = N.hop_known ? N.hop_mac : bcast;
    u64 total = klog_total();
    if (total - N.sent > PANIC_MAX) {
        u64 off = total - PANIC_MAX;
        char c = 0;
        for (int i = 0; i < 2048 && off < total && klog_read(off, &c, 1) == 1 && c != '\n'; i++) off++;
        if (off < total) off++;
        static const char t[] = "-- netconsole: older lines left out, the panic follows --\n";
        poll_send(dmac, t, sizeof t - 1);
        N.dropped_bytes += off - N.sent;
        N.sent = off;
    }
    for (int guard = 0; guard < 2048; guard++) {
        u64 off = N.sent;
        size_t got = klog_read_clamped(&off, g_pline, PAYLOAD_MAX);
        N.sent = off;
        if (!got) break;
        size_t e = 0;
        while (e < got && g_pline[e] != '\n') e++;
        if (e < got) e++;
        size_t len = klog_plain(g_pline, e);
        if (len) poll_send(dmac, g_pline, len);
        N.sent += e;
    }
}

/* ---- /proc/netconsole ---- */

static size_t gen_proc(char *b, size_t cap) {
    size_t n = 0;
#define P(...) do { if (n < cap) n += (size_t)snprintf(b + n, cap - n, __VA_ARGS__); } while (0)
    if (N.state == NC_OFF) { P("state:    off (netconsole=IP[:PORT] on the kernel command line)\n"); return MIN(n, cap); }
    char dst[20], src[20], hop[20];
    net_ip_str(N.dst, dst, sizeof dst);
    net_ip_str(N.src, src, sizeof src);
    net_ip_str(N.hop, hop, sizeof hop);
    P("target:   %s:%u from port %u%s%s\n", dst, N.dport, N.sport, N.devname[0] ? " through " : "", N.devname);
    if (N.state == NC_WAIT_ROUTE) P("state:    waiting for a route to %s (the network is not up)\n", dst);
    else if (N.state == NC_WAIT_ARP) P("state:    waiting for %s to answer ARP (asked %u times)\n", hop, N.probes);
    else P("state:    sending\n");
    if (N.dev) {
        P("card:     %s (%s) from %s, next hop %s", N.dev->name, N.dev->driver, src, hop);
        if (N.hop_known) P(" at %02x:%02x:%02x:%02x:%02x:%02x", N.hop_mac[0], N.hop_mac[1], N.hop_mac[2], N.hop_mac[3], N.hop_mac[4], N.hop_mac[5]);
        P("; polled transmit for a panic: %s\n", N.dev->netpoll_tx ? "yes" : "no (this driver has none)");
    }
    P("sent:     %lu datagrams, %lu bytes of log lines\n", (unsigned long)N.datagrams, (unsigned long)N.bytes);
    P("dropped:  %lu lines (%lu bytes) skipped in floods, %lu bytes overwritten before they could be sent, %lu send errors, %lu times the card was full\n",
      (unsigned long)N.dropped_lines, (unsigned long)N.dropped_bytes, (unsigned long)N.overwritten, (unsigned long)N.errors, (unsigned long)N.full);
    P("backlog:  %lu bytes of the log not sent yet\n", (unsigned long)(klog_total() - N.sent));
    P("rate:     at most %u lines a second\n", N.rate);
    P("panic:    %lu datagrams sent through the polled path\n", (unsigned long)N.panic_datagrams);
#undef P
    return MIN(n, cap);
}

static ssize_t put_proc(const char *buf, size_t n) {
    char w[32];
    size_t l = MIN(n, sizeof w - 1);
    memcpy(w, buf, l);
    w[l] = 0;
    if (strncmp(w, "flood ", 6)) return -EINVAL;
    long count = 0;
    for (const char *p = w + 6; *p >= '0' && *p <= '9'; p++) count = count * 10 + (*p - '0');
    if (count <= 0 || count > 200000) return -EINVAL;
    bool drawing = fbcon_enabled();
    fbcon_set_enabled(false);                                               /* a screen repaint per line would make the flood a trickle */
    for (long i = 1; i <= count; i++) klog("flood", "line %ld of %ld: the quick brown fox jumps over the lazy dog", i, count);
    fbcon_set_enabled(drawing);
    return (ssize_t)n;
}

/* ---- the command line ---- */

static bool parse_port(const char *s, const char *e, u16 *out) {
    u32 v = 0;
    if (s >= e) return false;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (u32)(*s - '0');
        if (v > 65535) return false;
    }
    if (!v) return false;
    *out = (u16)v;
    return true;
}

static bool parse_ip(const char *s, const char *e, u32 *out) {
    char t[20];
    if (e - s <= 0 || e - s >= (long)sizeof t) return false;
    memcpy(t, s, (size_t)(e - s));
    t[e - s] = 0;
    return net_ip_parse(t, out);
}

static bool parse_mac(const char *s, u8 *mac) {
    for (int i = 0; i < 6; i++) {
        u32 v = 0;
        for (int k = 0; k < 2; k++, s++) {
            char c = *s;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + (u32)d;
        }
        mac[i] = (u8)v;
        if (i < 5 && *s++ != ':') return false;
    }
    return *s == 0;
}

static bool parse_target(const char *v) {
    const char *end = v + strlen(v), *comma = strchr(v, ',');
    if (comma) {                                                            /* SPORT@SIP/DEV,PORT@IP/MAC */
        const char *at = memchr(v, '@', (size_t)(comma - v)), *slash = memchr(v, '/', (size_t)(comma - v));
        if (!at) return false;
        if (at > v && !parse_port(v, at, &N.sport)) return false;
        if (slash && slash + 1 < comma) {
            size_t l = MIN((size_t)(comma - slash - 1), sizeof N.devname - 1);
            memcpy(N.devname, slash + 1, l);
            N.devname[l] = 0;
        }
        const char *t = comma + 1, *at2 = strchr(t, '@');
        if (!at2) return false;
        if (at2 > t && !parse_port(t, at2, &N.dport)) return false;
        const char *ip = at2 + 1, *slash2 = strchr(ip, '/');
        if (!parse_ip(ip, slash2 ? slash2 : end, &N.dst)) return false;
        if (slash2 && slash2[1]) {
            if (!parse_mac(slash2 + 1, N.mac)) return false;
            N.mac_given = true;
        }
        return true;
    }
    const char *colon = strchr(v, ':');                                     /* IP[:PORT] */
    if (!parse_ip(v, colon ? colon : end, &N.dst)) return false;
    return !colon || parse_port(colon + 1, end, &N.dport);
}

static void netcons_init(void) {
    procfs_register_rw("netconsole", gen_proc, put_proc);
    char v[96];
    if (!cmdline_value("netconsole", v, sizeof v)) return;
    N.dport = DEFAULT_PORT;
    N.sport = DEFAULT_SPORT;
    long rate = cmdline_int("netconsole.rate", DEFAULT_RATE);
    N.rate = (u32)(rate < 10 ? 10 : rate > 100000 ? 100000 : rate);
    if (!parse_target(v) || !N.dst) {
        klog("Aya", "netconsole=%s is not a target (netconsole=IP[:PORT], or SPORT@/DEV,PORT@IP/MAC): no netconsole", v);
        return;
    }
    N.tokens = N.rate;
    N.refill_at = net_now();
    N.ip_id = (u16)rdtsc();
    N.state = NC_WAIT_ROUTE;
    char ip[20];
    net_ip_str(N.dst, ip, sizeof ip);
    klog("Aya", "netconsole: the kernel log goes to %s:%u (from port %u%s%s) once the network is up, at most %u lines a second", ip, N.dport, N.sport,
         N.devname[0] ? " through " : "", N.devname, N.rate);
    task_create_detached("Aya", aya_main, NULL);
}
INITCALL(netcons_init);
