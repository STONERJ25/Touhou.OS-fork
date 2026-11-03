/*
 * Network time at boot (docs/NTP.md): a kernel thread that waits until a network card has an address, asks NTP servers what time it is,
 * and steps the clock. The wire work (packet, checks, DNS messages, time zones) is in ntp_proto.c; this file is the machine half:
 * sockets, the thread, the lease's option 42, /etc/hosts and /etc/resolv.conf, the log, and /proc/ntp.
 *
 * Where it asks, in this order, stopping at the first group that gives an answer:
 *   1. the NTP servers the DHCP lease names (option 42),
 *   2. the servers on the kernel command line (ntp=host[:port][,host[:port]...]),
 *   3. a built-in list of public names (looked up with DNS).
 * Two servers are asked when there are two; if they differ by more than a second neither is believed. A reply that does not echo our
 * request, that comes from the wrong place, says stratum 0 or 16, carries the "unsynchronised" alarm, or took more than a few seconds
 * is thrown away (ntp_proto.c). A failed round backs off (10 s, 20 s, ... up to half an hour) and tries again; a good one is repeated
 * every ntp.interval= seconds (4 hours). The boot never waits for any of it: this is a thread of its own.
 *
 * The clock chip is left alone unless ntp.rtc=1. The system clock is UTC always; the zone only matters for a chip that keeps local
 * time (rtc=local, a PC that also runs Windows), which is what ntp.rtc=1 must know about, so the zone rules are evaluated here too.
 *
 * "init" corrects a local-time chip once, early (setup_clock): if the lease and the first answer come before that, the correction
 * lands on a clock that is already right. A clock set by anyone else during the first five minutes is therefore noticed and the
 * servers are asked again a second later.
 */
#include "ntp.h"
#include "net.h"
#include "netsock.h"
#include "kernel.h"
#include "cpu.h"
#include "cmdline.h"
#include "ktime.h"
#include "irq.h"
#include "task.h"
#include "vfs.h"
#include "process.h"
#include "random.h"
#include "initcall.h"

#define TAG "Sakuya"

int procfs_register_rw(const char *name, size_t (*gen)(char *buf, size_t cap), ssize_t (*wr)(const char *buf, size_t n));

/* The public names, one operator each (two answers from the same operator would not be a second opinion). Replaced by ntp.builtin= in tests. */
#define BUILTIN_SERVERS "time.cloudflare.com,time.google.com,pool.ntp.org,time.nist.gov"

#define LIST_MAX    200
#define MAX_CAND    6                       /* servers asked in one group at most */
#define DNS_TRY_MS  1500                    /* per name server and attempt */
#define NTP_TRY_MS  2000                    /* per NTP request; two tries per server */
#define WATCH_MS    300000                  /* how long after boot a clock set by somebody else makes us ask again */
#define DEFAULT_INTERVAL 14400

enum { ST_OFF, ST_WAITING, ST_SYNCING, ST_SYNCED, ST_FAILING };
static const char *const st_name[] = { "off", "waiting for the network", "syncing", "synced", "failing" };

typedef struct {
    bool rtc_write, rtc_local;
    u32  interval_s;
    char cmdline_list[LIST_MAX];            /* ntp=... as given */
    char list[LIST_MAX];                    /* the group-2 list in force: that, or what "server ..." wrote to /proc/ntp */
    char builtin[LIST_MAX];                 /* group 3 */
} NtpConfig;

typedef struct {
    int  state;
    char server[56];                        /* where the last good answer came from */
    char source[12];                        /* "dhcp", "cmdline", "builtin" */
    int  stratum;
    s64  offset_ms, delay_ms;               /* of the last good answer, before the clock was stepped */
    u64  last_ok_wall_ms, last_ok_up_ms;
    u64  next_up_ms;                        /* uptime at which the next round starts */
    u32  cycles, syncs, failures, streak, external_steps;
    char last_error[128];
    char rtc_note[96];
} NtpState;

static NtpConfig g_cfg;
static NtpState  g_st;
static Semaphore g_kick;
static bool g_kick_pending, g_force;
static bool g_enabled;
static volatile bool g_net_up;              /* what the thread saw at its last look: a card with a carrier and an address */
static s64  g_base;                         /* wall clock minus uptime as of our last step: it moves only when somebody sets the clock */
static bool g_base_valid;

/* ------------------------------------------------------------------ small helpers */

static const char *fmt_s(char *buf, size_t n, s64 ms) {     /* +7200.012 */
    s64 a = ms < 0 ? -ms : ms;
    snprintf(buf, n, "%c%lld.%03lld", ms < 0 ? '-' : '+', (long long)(a / 1000), (long long)(a % 1000));
    return buf;
}

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...) {
    char tmp[sizeof g_st.last_error];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    u64 f = irq_save();
    strlcpy(g_st.last_error, tmp, sizeof g_st.last_error);
    irq_restore(f);
}

static void set_state(int s) { u64 f = irq_save(); g_st.state = s; irq_restore(f); }

static void kick(void) {
    u64 f = irq_save();
    bool post = !g_kick_pending;
    g_kick_pending = true;
    irq_restore(f);
    if (post) sem_post(&g_kick);
}

/* Sleeps up to ms; a kick (a command written to /proc/ntp) ends it early. */
static void nap(u64 ms) {
    if (ms == 0) return;
    sem_wait_timeout(&g_kick, ms);
    u64 f = irq_save();
    g_kick_pending = false;
    irq_restore(f);
}

static bool net_ready(void) {
    bool up = false;
    net_lock();
    NetDev *d;
    for (int i = 0; (d = netdev_by_index(i)) != NULL; i++)
        if (!(d->flags & NDF_LOOPBACK) && (d->flags & NDF_LINK) && d->addr) up = true;
    net_unlock();
    return up;
}

/* ------------------------------------------------------------------ a UDP socket for the kernel's own use */

static File *udp_open(void) {
    net_lock();
    Sock *s = sock_new(SK_UDP);
    if (!s) { net_unlock(); return NULL; }
    s->has_file = true;
    u16 port = sock_alloc_port(SK_UDP);
    VNode *v = port ? vnode_alloc(&g_inet_vn_ops, NULL, S_IFSOCK | 0600) : NULL;
    File *f = v ? file_new(v, O_RDWR, &g_inet_fops, s) : NULL;
    if (!f) {
        if (v) vnode_put(v);
        sock_unlink_free(s);
        net_unlock();
        return NULL;
    }
    s->lport = port;
    s->bound = true;
    net_unlock();
    return f;
}

static void udp_close(File *f) { if (f) file_put(f); }       /* closing the file frees the socket */

static int udp_tx(File *f, u32 ip, u16 port, const void *buf, size_t n) {
    ssize_t rc = sk_send(f, buf, n, 0, true, ip, port);
    return rc < 0 ? (int)rc : 0;
}

/* One datagram, waiting until the uptime `deadline`: its length, or -ETIMEDOUT. */
static ssize_t udp_rx(File *f, u8 *buf, size_t cap, u64 deadline, u32 *from, u16 *from_port) {
    u64 now = uptime_ms();
    if (now >= deadline) return -ETIMEDOUT;
    Sock *s = f->priv;
    s->rcvtimeo = (u32)(deadline - now);
    ssize_t n = sk_recv(f, buf, cap, 0, from, from_port);
    return n == -EAGAIN ? -ETIMEDOUT : n;
}

/* ------------------------------------------------------------------ names: /etc/hosts, /etc/resolv.conf, DNS */

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Calls fn(line) for every line of a small text file (comments and all); fn returns true to stop. */
static bool each_line(const char *path, bool (*fn)(char *line, void *ctx), void *ctx) {
    void *data;
    size_t size;
    if (vfs_read_file(path, &data, &size) != 0) return false;
    bool stopped = false;
    char *s = data;
    while (*s && !stopped) {
        char *line = s, *nl = strchr(s, '\n');
        if (nl) { *nl = 0; s = nl + 1; } else s += strlen(s);
        stopped = fn(line, ctx);
    }
    kfree(data);
    return stopped;
}

typedef struct { const char *name; u32 ip; } HostsQuery;

static bool hosts_line(char *line, void *ctx) {
    HostsQuery *q = ctx;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *p = line;
    while (is_space(*p)) p++;
    char *addr = p;
    while (*p && !is_space(*p)) p++;
    if (!*p) return false;
    *p++ = 0;
    u32 ip;
    if (!ntp_parse_ip4(addr, &ip)) return false;                   /* (an IPv6 line, or noise) */
    for (;;) {
        while (is_space(*p)) p++;
        if (!*p) return false;
        char *name = p;
        while (*p && !is_space(*p)) p++;
        if (*p) *p++ = 0;
        if (!strcasecmp(name, q->name)) { q->ip = ip; return true; }
    }
}

typedef struct { u32 ip[3]; u16 port[3]; int n; } Resolvers;

static bool resolv_line(char *line, void *ctx) {
    Resolvers *r = ctx;
    char *p = line;
    while (is_space(*p)) p++;
    if (strncmp(p, "nameserver", 10) || !is_space(p[10])) return false;
    p += 10;
    while (is_space(*p)) p++;
    char *end = p;
    while (*end && !is_space(*end) && *end != '#') end++;
    u16 port = 53;
    if (*end == '#') {                                              /* "10.0.2.2#5353": the port, as the C library's resolver reads it too */
        u32 v = 0;
        char *q = end + 1;
        while (*q >= '0' && *q <= '9' && v < 65536) v = v * 10 + (u32)(*q++ - '0');
        if (v > 0 && v < 65536) port = (u16)v;
    }
    *end = 0;
    u32 ip;
    if (ntp_parse_ip4(p, &ip) && r->n < 3) { r->ip[r->n] = ip; r->port[r->n] = port; r->n++; }
    return false;
}

static void resolvers(Resolvers *r) {
    r->n = 0;
    each_line("/etc/resolv.conf", resolv_line, r);
    if (r->n == 0) {                                                /* no file yet (before init) or none written: the lease's servers */
        u32 d[3];
        net_lock();
        size_t n = dhcp_dns(d, 3);
        net_unlock();
        for (size_t i = 0; i < n; i++) { r->ip[r->n] = d[i]; r->port[r->n] = 53; r->n++; }
    }
}

/* An address for a name: a dotted number as it is, then /etc/hosts, then the name servers. 0, or -ENOENT / -ETIMEDOUT. */
static int resolve(File *f, const char *name, u32 *ip) {
    if (ntp_parse_ip4(name, ip)) return 0;
    HostsQuery hq = { name, 0 };
    if (each_line("/etc/hosts", hosts_line, &hq)) { *ip = hq.ip; return 0; }
    Resolvers rs;
    resolvers(&rs);
    if (rs.n == 0) return -ENOENT;
    int rc = -ETIMEDOUT;
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < rs.n; i++) {
            u16 id = (u16)random_u64();
            u8 q[NTP_DNS_MAX], a[NTP_DNS_MAX];
            size_t ql = ntp_dns_query(q, sizeof q, id, name);
            if (!ql) return -ENOENT;
            if (udp_tx(f, rs.ip[i], rs.port[i], q, ql) != 0) continue;
            u64 deadline = uptime_ms() + DNS_TRY_MS;
            for (;;) {
                u32 from;
                u16 fport;
                ssize_t n = udp_rx(f, a, sizeof a, deadline, &from, &fport);
                if (n < 0) break;
                if (from != rs.ip[i] || fport != rs.port[i]) continue;
                u32 ips[4];
                int got = ntp_dns_answers(a, (size_t)n, id, ips, 4);
                if (got == -1) continue;                            /* another transaction's answer: keep waiting for ours */
                if (got > 0) { *ip = ips[0]; return 0; }
                rc = -ENOENT;                                       /* NXDOMAIN, or a name with no address: this server has spoken */
                break;
            }
        }
        if (rc == -ENOENT) break;
    }
    return rc;
}

/* ------------------------------------------------------------------ asking one server */

typedef struct {
    u32  ip;
    u16  port;
    int  stratum;
    s64  offset, delay;
    char label[56];                                                 /* "10.0.2.2", "10.0.2.2:12345" or "pool.ntp.org (203.0.113.4)" */
} Answer;

static void label_for(char *out, size_t n, const char *host, u32 ip, u16 port) {
    char a[20];
    net_ip_str(ip, a, sizeof a);
    bool literal = !strcmp(host, a);
    if (literal && port == NTP_PORT) snprintf(out, n, "%s", a);
    else if (literal) snprintf(out, n, "%s:%u", a, port);
    else if (port == NTP_PORT) snprintf(out, n, "%s (%s)", host, a);
    else snprintf(out, n, "%s (%s:%u)", host, a, port);
}

/* Two requests of two seconds each. 0 with the answer filled in, or -ETIMEDOUT (nothing usable came); `why` says what was wrong with
 * what did come, if anything did. */
static int ask_server(File *f, u32 ip, u16 port, Answer *out, char *why, size_t whycap) {
    why[0] = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        u8 req[NTP_PKT_LEN], rep[128];
        u16 nonce = (u16)random_u64();
        s64 t1 = (s64)time_now_ms();                                /* (taken as late as possible: the ARP lookup for the first packet is the network's time, not ours) */
        ntp_make_request(req, t1, nonce);
        if (udp_tx(f, ip, port, req, sizeof req) != 0) { strlcpy(why, "the request could not be sent", whycap); continue; }
        u64 deadline = uptime_ms() + NTP_TRY_MS;
        for (;;) {
            u32 from;
            u16 fport;
            ssize_t n = udp_rx(f, rep, sizeof rep, deadline, &from, &fport);
            s64 t4 = (s64)time_now_ms();
            if (n < 0) break;
            if (from != ip || fport != port) continue;              /* somebody else's packet on our port */
            NtpReply r;
            int rc = ntp_parse_reply(rep, (size_t)n, req + 40, &r);
            if (rc == NTP_E_ORIGIN || rc == NTP_E_SHORT || rc == NTP_E_MODE) {
                snprintf(why, whycap, "%s", ntp_strerror(rc));      /* not an answer to this request (a late one, or noise): keep waiting for the real one */
                continue;
            }
            if (rc != NTP_OK) {
                if (rc == NTP_E_KISS) snprintf(why, whycap, "%s%s%s", ntp_strerror(rc), r.kiss[0] ? ": " : "", r.kiss);
                else snprintf(why, whycap, "%s", ntp_strerror(rc));
                return -ETIMEDOUT;                                  /* a server that answers like this is not asked again this round */
            }
            s64 offset, delay;
            ntp_offset_delay(t1, r.t2_ms, r.t3_ms, t4, &offset, &delay);
            if (!ntp_delay_ok(delay)) {
                snprintf(why, whycap, "a round trip of %lld ms", (long long)delay);
                return -ETIMEDOUT;
            }
            out->ip = ip;
            out->port = port;
            out->stratum = r.stratum;
            out->offset = offset;
            out->delay = delay < 0 ? 0 : delay;
            return 0;
        }
    }
    if (!why[0]) strlcpy(why, "no answer", whycap);
    return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ the zone, for a clock chip that keeps local time */

typedef struct { char tz[96]; bool found; } ZoneText;

static bool timezone_line(char *line, void *ctx) {
    ZoneText *z = ctx;
    char *cr = strchr(line, '\r');
    if (cr) *cr = 0;
    strlcpy(z->tz, line, sizeof z->tz);
    z->found = true;
    return true;                                                    /* the first line only */
}

static NtpZone load_zone(void) {
    ZoneText zt;
    zt.tz[0] = 0;
    zt.found = false;
    NtpZone z;
    if (!cmdline_value("tz", zt.tz, sizeof zt.tz) || !zt.tz[0]) {   /* tz= on the command line wins over /etc/timezone, as init arranges for the C library */
        zt.tz[0] = 0;
        each_line("/etc/timezone", timezone_line, &zt);
    }
    ntp_zone_parse(zt.tz, &z);                                      /* (a zone that is not understood is UTC, as in the C library) */
    return z;
}

/* ntp.rtc=1: the chip gets the time the way hwclock -w would write it: UTC, or the local time of the zone when the chip keeps local time. */
static void write_chip(s64 utc_sec) {
    s64 value = utc_sec;
    s32 off = 0;
    if (g_cfg.rtc_local) {
        NtpZone z = load_zone();
        off = ntp_zone_offset(&z, utc_sec);
        value += off;
    }
    if (value < 0) return;
    rtc_write_raw((u64)value);
    u64 f = irq_save();
    snprintf(g_st.rtc_note, sizeof g_st.rtc_note, "written at %lld (%s)", (long long)utc_sec, g_cfg.rtc_local ? "local time" : "UTC");
    irq_restore(f);
    if (g_cfg.rtc_local) klog(TAG, "clock chip written too: local time, %+d h %02d min from UTC", off / 3600, (off < 0 ? -off : off) % 3600 / 60);
    else klog(TAG, "clock chip written too (UTC)");
}

/* ------------------------------------------------------------------ one round */

static void note_rejection(const char *host, const char *why) {
    klog(TAG, "network time: %s: %s", host, why);
}

/* Fills `list` with the servers of group 0, 1 or 2. */
static void group_list(int group, char *list, size_t cap) {
    list[0] = 0;
    if (group == 0) {
        u32 ips[4];
        net_lock();
        size_t n = dhcp_ntp(ips, 4);
        net_unlock();
        for (size_t i = 0; i < n; i++) {
            char a[20];
            net_ip_str(ips[i], a, sizeof a);
            strlcat(list, a, cap);
            strlcat(list, ",", cap);
        }
    } else {
        u64 f = irq_save();
        strlcpy(list, group == 1 ? g_cfg.list : g_cfg.builtin, cap);
        irq_restore(f);
    }
}

static const char *const group_name[] = { "dhcp", "cmdline", "builtin" };

/* Asks the servers of one group until two have answered. Returns how many answered (and in *tried how many it asked). */
static int ask_group(File *f, const char *list, Answer ans[2], int *tried) {
    NtpSpec sp;
    const char *p = list;
    int got = 0;
    *tried = 0;
    while (got < 2 && *tried < MAX_CAND && ntp_next_spec(&p, &sp)) {
        u32 ip;
        if (resolve(f, sp.host, &ip) != 0) { note_rejection(sp.host, "the name could not be looked up"); (*tried)++; continue; }
        if (ip == 0 || ip == 0xFFFFFFFFu || (ip >> 24) == 127 || (ip >> 28) == 14) {        /* "this host" (a send to 0.0.0.0 goes to the loopback), broadcast, loopback, multicast */
            note_rejection(sp.host, "not an address a time server can have");
            (*tried)++;
            continue;
        }
        bool twice = false;
        for (int i = 0; i < got; i++) if (ans[i].ip == ip && ans[i].port == sp.port) twice = true;
        if (twice) continue;                                        /* the same server under another name is not a second opinion */
        (*tried)++;
        char why[96];
        Answer a;
        if (ask_server(f, ip, sp.port, &a, why, sizeof why) != 0) {
            char label[56];
            label_for(label, sizeof label, sp.host, ip, sp.port);
            note_rejection(label, why);
            continue;
        }
        label_for(a.label, sizeof a.label, sp.host, ip, sp.port);
        ans[got++] = a;
    }
    return got;
}

static void apply(const Answer *a, const char *source, const Answer *second) {
    u64 f = irq_save();
    s64 now = (s64)time_now_ms();
    s64 target = now + a->offset;
    if (target < 0) target = 0;
    time_set_ms((u64)target);
    g_base = (s64)time_now_ms() - (s64)uptime_ms();                 /* (interrupts are off: the tick cannot move between the two) */
    g_base_valid = true;
    irq_restore(f);

    char off[24], extra[96] = "";
    fmt_s(off, sizeof off, a->offset);
    if (second) {
        s64 d = a->offset - second->offset;
        snprintf(extra, sizeof extra, "; %s agrees within %lld ms", second->label, (long long)(d < 0 ? -d : d));
    }
    klog(TAG, "clock set from %s: %s s (round trip %lld ms, stratum %d%s)", a->label, off, (long long)a->delay, a->stratum, extra);

    u64 f2 = irq_save();
    strlcpy(g_st.server, a->label, sizeof g_st.server);
    strlcpy(g_st.source, source, sizeof g_st.source);
    g_st.stratum = a->stratum;
    g_st.offset_ms = a->offset;
    g_st.delay_ms = a->delay;
    g_st.last_ok_wall_ms = time_now_ms();
    g_st.last_ok_up_ms = uptime_ms();
    g_st.syncs++;
    irq_restore(f2);

    if (g_cfg.rtc_write) write_chip(target / 1000);
}

/* One round over the three groups. true: the clock now agrees with the servers. */
static bool run_round(void) {
    File *f = udp_open();
    if (!f) { set_error("no memory for a socket"); return false; }
    bool ok = false, spoke = false;
    char list[LIST_MAX];
    for (int group = 0; group < 3 && !ok && !spoke; group++) {
        group_list(group, list, sizeof list);
        if (!list[0]) continue;
        Answer ans[2];
        int tried = 0, got = ask_group(f, list, ans, &tried);
        if (got == 0) continue;                                     /* nobody in this group answered: the next group */
        spoke = true;
        if (got == 2 && !ntp_agree(ans[0].offset, ans[1].offset)) {
            char a[24], b[24];
            fmt_s(a, sizeof a, ans[0].offset);
            fmt_s(b, sizeof b, ans[1].offset);
            klog(TAG, "network time: the servers disagree (%s says %s s, %s says %s s): neither is believed, the clock stays as it is", ans[0].label, a, ans[1].label, b);
            set_error("the servers disagree: %s says %s s, %s says %s s", ans[0].label, a, ans[1].label, b);
            break;
        }
        if (got == 1 && tried > 1) klog(TAG, "network time: only %s of %d servers answered", ans[0].label, tried);
        const Answer *best = &ans[0];
        if (got == 2 && ans[1].delay < ans[0].delay) best = &ans[1];
        apply(best, group_name[group], got == 2 ? (best == &ans[0] ? &ans[1] : &ans[0]) : NULL);
        ok = true;
    }
    udp_close(f);
    if (!ok && !spoke) {
        klog(TAG, "network time: no server answered");
        set_error("no server answered");
    }
    return ok;
}

/* ------------------------------------------------------------------ the thread */

static u32 backoff_s(u32 streak) {
    static const u32 steps[] = { 10, 20, 40, 80, 160, 320, 640, 1280, 1800 };
    u32 s = steps[streak > 0 ? MIN(streak - 1, (u32)ARRAY_SIZE(steps) - 1) : 0];
    return s < g_cfg.interval_s ? s : g_cfg.interval_s;
}

/* Has somebody else set the clock since we did? During the first minutes after boot that means init's rtc=local correction (or a person
 * in a hurry): ask again at once, so that the answer from the servers has the last word. */
static void watch_clock(u64 up) {
    if (!g_base_valid) return;
    u64 f = irq_save();
    s64 base = (s64)time_now_ms() - (s64)uptime_ms();
    irq_restore(f);
    s64 d = base - g_base;
    if (d > -50 && d < 50) return;
    char off[24];
    klog(TAG, "the clock was set by somebody else (%s s)%s", fmt_s(off, sizeof off, d), up < WATCH_MS ? ": asking the servers again" : "");
    g_base = base;
    u64 f2 = irq_save();
    g_st.external_steps++;
    if (up < WATCH_MS) g_st.next_up_ms = up + 1000;
    irq_restore(f2);
}

static void ntp_thread(void *arg) {
    (void)arg;
    bool was_up = false;
    for (;;) {
        u64 up = uptime_ms();
        bool ready = net_ready();
        g_net_up = ready;
        if (!ready) {
            was_up = false;
            if (!g_st.syncs && g_st.state != ST_FAILING) set_state(ST_WAITING);
            nap(500);
            continue;
        }
        if (!was_up) {                                              /* the network is back after being gone (a cable pulled and plugged in, a lease that ran out) */
            was_up = true;
            u64 f = irq_save();
            if (g_st.next_up_ms > up + 10000) g_st.next_up_ms = up + 5000;      /* the clock has not been checked while it was away, and this may be another network: look soon */
            irq_restore(f);
        }
        watch_clock(up);
        u64 f = irq_save();
        bool due = g_force || up >= g_st.next_up_ms;
        g_force = false;
        irq_restore(f);
        if (due) {
            set_state(ST_SYNCING);
            bool ok = run_round();
            u64 f2 = irq_save();
            g_st.cycles++;
            if (ok) {
                g_st.streak = 0;
                g_st.next_up_ms = uptime_ms() + (u64)g_cfg.interval_s * 1000;
                g_st.last_error[0] = 0;
                g_st.state = ST_SYNCED;
            } else {
                g_st.failures++;
                g_st.streak++;
                g_st.next_up_ms = uptime_ms() + (u64)backoff_s(g_st.streak) * 1000;
                g_st.state = ST_FAILING;
            }
            irq_restore(f2);
            continue;
        }
        u64 now = uptime_ms();
        u64 left = g_st.next_up_ms > now ? g_st.next_up_ms - now : 0;
        u64 step = now < WATCH_MS ? 1000 : 30000;                   /* (a quick look for a clock set by somebody else only matters early) */
        nap(left < step ? left : step);
    }
}

/* ------------------------------------------------------------------ /proc/ntp */

typedef struct { char *b; size_t cap, n; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out *o, const char *fmt, ...) {
    if (o->n >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->b + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += MIN((size_t)k, o->cap - o->n - 1);
}

static size_t gen_ntp(char *b, size_t cap) {
    NtpState s;
    NtpConfig c;
    u64 f = irq_save();
    s = g_st;
    c = g_cfg;
    irq_restore(f);
    Out o = { b, cap, 0 };
    u64 up = uptime_ms();
    outf(&o, "ntp: %s\n", st_name[s.state]);
    if (g_enabled) outf(&o, "network: %s\n", g_net_up ? "up" : "down");     /* as the client last saw it: a card with a carrier and an address */
    if (!g_enabled) {
        outf(&o, "reason: ntp=off on the kernel command line\n");
        return o.n;
    }
    outf(&o, "server: %s\n", s.syncs ? s.server : "-");
    outf(&o, "source: %s\n", s.syncs ? s.source : "-");
    outf(&o, "stratum: %d\n", s.syncs ? s.stratum : 0);
    if (s.syncs) {
        char off[24];
        int y, mo, d, h, mi, se, wd;
        civil_from_epoch(s.last_ok_wall_ms / 1000, &y, &mo, &d, &h, &mi, &se, &wd);
        outf(&o, "offset_ms: %lld\n", (long long)s.offset_ms);
        outf(&o, "offset: %s s\n", fmt_s(off, sizeof off, s.offset_ms));
        outf(&o, "delay_ms: %lld\n", (long long)s.delay_ms);
        outf(&o, "last_sync: %04d-%02d-%02d %02d:%02d:%02d UTC\n", y, mo, d, h, mi, se);
        outf(&o, "last_sync_age_s: %lu\n", (unsigned long)((up - s.last_ok_up_ms) / 1000));
    } else {
        outf(&o, "offset_ms: -\noffset: -\ndelay_ms: -\nlast_sync: never\nlast_sync_age_s: -\n");
    }
    outf(&o, "cycles: %u\nsyncs: %u\nfailures: %u\nfail_streak: %u\n", s.cycles, s.syncs, s.failures, s.streak);
    outf(&o, "external_steps: %u\n", s.external_steps);
    outf(&o, "next_sync_in_s: %lu\n", (unsigned long)(s.next_up_ms > up ? (s.next_up_ms - up) / 1000 : 0));
    outf(&o, "interval_s: %u\n", c.interval_s);
    outf(&o, "rtc: %s\n", c.rtc_write ? (s.rtc_note[0] ? s.rtc_note : "will be written (ntp.rtc=1)") : "untouched (ntp.rtc=0)");
    outf(&o, "rtc_local: %s\n", c.rtc_local ? "yes (rtc=local)" : "no");
    u32 ips[4];
    net_lock();
    size_t nd = dhcp_ntp(ips, 4);
    net_unlock();
    outf(&o, "dhcp_servers: %u\n", (unsigned)nd);
    outf(&o, "cmdline_servers: %s\n", c.list[0] ? c.list : "-");
    outf(&o, "builtin_servers: %s\n", c.builtin);
    outf(&o, "last_error: %s\n", s.last_error[0] ? s.last_error : "-");
    return o.n;
}

/* Root writes: "sync" (ask now), "server LIST" (replace the command-line servers until the next start; no list puts the command line's back),
 * "builtin LIST" (replace the built-in names). The lists are written as on the command line: host[:port],host[:port]. */
static ssize_t put_ntp(const char *buf, size_t n) {
    char line[LIST_MAX + 16];
    size_t l = 0;
    while (l < n && l < sizeof line - 1 && buf[l] != '\n') { line[l] = buf[l]; l++; }
    line[l] = 0;
    while (l > 0 && is_space(line[l - 1])) line[--l] = 0;
    if (!g_enabled) return -ENODEV;
    char *arg = line;
    while (*arg && !is_space(*arg)) arg++;
    if (*arg) *arg++ = 0;
    while (is_space(*arg)) arg++;
    if (!strcmp(line, "sync")) {
        u64 f = irq_save();
        g_force = true;
        irq_restore(f);
        kick();
    } else if (!strcmp(line, "server") || !strcmp(line, "servers")) {
        u64 f = irq_save();
        strlcpy(g_cfg.list, *arg ? arg : g_cfg.cmdline_list, sizeof g_cfg.list);
        irq_restore(f);
    } else if (!strcmp(line, "builtin")) {
        u64 f = irq_save();
        strlcpy(g_cfg.builtin, *arg ? arg : BUILTIN_SERVERS, sizeof g_cfg.builtin);
        irq_restore(f);
    } else {
        return -EINVAL;
    }
    return (ssize_t)n;
}

/* ------------------------------------------------------------------ setup */

static void ntp_init(void) {
    char v[LIST_MAX];
    g_enabled = true;
    if (cmdline_value("ntp", v, sizeof v)) {
        if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "0") || !strcmp(v, "no")) g_enabled = false;
        else if (strcmp(v, "on") && strcmp(v, "auto")) strlcpy(g_cfg.cmdline_list, v, sizeof g_cfg.cmdline_list);
    }
    strlcpy(g_cfg.list, g_cfg.cmdline_list, sizeof g_cfg.list);
    strlcpy(g_cfg.builtin, cmdline_value("ntp.builtin", v, sizeof v) && v[0] ? v : BUILTIN_SERVERS, sizeof g_cfg.builtin);
    long iv = cmdline_int("ntp.interval", DEFAULT_INTERVAL);
    g_cfg.interval_s = (u32)(iv < 16 ? 16 : iv > 604800 ? 604800 : iv);       /* RFC 4330: no client asks more often than every 15 seconds */
    g_cfg.rtc_write = cmdline_int("ntp.rtc", 0) != 0;
    g_cfg.rtc_local = cmdline_value("rtc", v, sizeof v) && !strcmp(v, "local");

    procfs_register_rw("ntp", gen_ntp, put_ntp);
    if (!g_enabled) {
        g_st.state = ST_OFF;
        klog(TAG, "network time is off (ntp=off)");
        return;
    }
    sem_init(&g_kick, 0);
    g_st.state = ST_WAITING;
    task_create_detached("ntp", ntp_thread, NULL);
}
INITCALL(ntp_init);
