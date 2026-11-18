/*
 * Names and services: /etc/hosts, a DNS resolver over UDP (A and AAAA records, CNAME chains, the search list of /etc/resolv.conf, a small cache), gethostbyname / gethostbyname2 /
 * getaddrinfo / getnameinfo / getservbyname, for IPv4 and IPv6.
 *
 * /etc/resolv.conf understands "nameserver ADDR[#PORT]" (the address may be IPv6; the port is an extension, handy for tests), "search" and "domain", and
 * "options ndots:N timeout:SECONDS attempts:N".
 *
 * getaddrinfo asks for A and AAAA records (one query after the other), honours AI_ADDRCONFIG, AI_V4MAPPED and AI_ALL, understands a scope ("fe80::1%eth0"), and orders what it found by
 * a simple form of RFC 6724 section 6: the source address that a connected datagram socket would use tells whether a destination is reachable and which scope and label it
 * shares, then rules 1, 2, 5, 6, 8 and 9 (reachable first, matching scope, matching label, higher precedence, smaller scope, longer common prefix) and the order the servers gave.
 */
#include <netdb.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <gensokyo.h>

int h_errno;

const char *hstrerror(int err) {
    switch (err) {
    case 0: return "Resolver error 0 (no error)";
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN: return "Host name lookup failure (no answer from the name server)";
    case NO_RECOVERY: return "Unknown server error";
    case NO_DATA: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}

/* An address of either family: AF_INET uses a[0..4), AF_INET6 a[0..16). */
typedef struct {
    int           af;
    unsigned char a[16];
    unsigned      scope;                                                /* IPv6: the scope id of a link-local address ("fe80::1%eth0") */
} HostAddr;

/* Reads an address from text: dotted quad or IPv6, with an optional %scope (an interface name or a number) on IPv6. */
static int parse_addr(const char *text, HostAddr *out) {
    char tmp[64];
    memset(out, 0, sizeof *out);
    if (strlen(text) >= sizeof tmp) return 0;
    strcpy(tmp, text);
    if (strchr(tmp, ':')) {
        char *pct = strchr(tmp, '%');
        unsigned scope = 0;
        if (pct) {
            *pct++ = 0;
            if (!*pct) return 0;
            char *end;
            unsigned long v = strtoul(pct, &end, 10);
            scope = (*end == 0) ? (unsigned)v : if_nametoindex(pct);
            if (!scope) return 0;
        }
        if (inet_pton(AF_INET6, tmp, out->a) != 1) return 0;
        out->af = AF_INET6;
        out->scope = scope;
        return 1;
    }
    struct in_addr a;
    if (!isdigit((unsigned char)tmp[0]) || !inet_aton(tmp, &a)) return 0;
    out->af = AF_INET;
    memcpy(out->a, &a, 4);
    return 1;
}

static int addr_len(int af) { return af == AF_INET6 ? 16 : 4; }

/* ---------------------------------------------------------------- /etc/hosts */

/* The addresses /etc/hosts gives a name (of the family asked: AF_UNSPEC for both). */
static int hosts_lookup(const char *name, int family, HostAddr *out, int max, char *canon, size_t canonlen) {
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL;
        char *addr = strtok_r(line, " \t\r\n", &save);
        HostAddr a;
        if (!addr || !parse_addr(addr, &a)) continue;
        if (family != AF_UNSPEC && family != a.af) continue;
        char *first = NULL;
        for (char *w = strtok_r(NULL, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
            if (!first) first = w;
            if (!strcasecmp(w, name)) {
                out[n++] = a;
                if (canon && !canon[0]) strlcpy(canon, first, canonlen);
                break;
            }
        }
    }
    fclose(f);
    return n;
}

static int hosts_reverse(int af, const unsigned char *addr, char *name, size_t n) {
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL;
        char *a = strtok_r(line, " \t\r\n", &save);
        HostAddr ha;
        if (!a || !parse_addr(a, &ha) || ha.af != af || memcmp(ha.a, addr, (size_t)addr_len(af))) continue;
        char *w = strtok_r(NULL, " \t\r\n", &save);
        if (w) { strlcpy(name, w, n); found = 1; }
    }
    fclose(f);
    return found;
}

/* ---------------------------------------------------------------- /etc/resolv.conf */

typedef struct {
    struct sockaddr_storage ns[3];
    socklen_t nslen[3];
    int nns;
    char search[4][64];
    int nsearch;
    int ndots, timeout_ms, attempts;
} ResConf;

static void res_load(ResConf *rc) {
    memset(rc, 0, sizeof *rc);
    rc->ndots = 1;
    rc->timeout_ms = 3000;
    rc->attempts = 2;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        for (char *c = line; *c; c++)                                  /* a comment starts at # or ; at the start of a word ("ADDR#PORT" is not one) */
            if ((*c == '#' || *c == ';') && (c == line || c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
        char *save = NULL;
        char *key = strtok_r(line, " \t\r\n", &save);
        if (!key) continue;
        if (!strcmp(key, "nameserver") && rc->nns < 3) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            if (!v) continue;
            unsigned port = 53;
            char *p = strchr(v, '#');
            if (p) { *p++ = 0; port = (unsigned)atoi(p); if (port == 0 || port > 65535) continue; }
            HostAddr a;
            if (!parse_addr(v, &a)) continue;
            struct sockaddr_storage *s = &rc->ns[rc->nns];
            memset(s, 0, sizeof *s);
            if (a.af == AF_INET6) {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)s;
                s6->sin6_family = AF_INET6;
                memcpy(&s6->sin6_addr, a.a, 16);
                s6->sin6_port = htons((unsigned short)port);
                s6->sin6_scope_id = a.scope;
                rc->nslen[rc->nns] = sizeof *s6;
            } else {
                struct sockaddr_in *s4 = (struct sockaddr_in *)s;
                s4->sin_family = AF_INET;
                memcpy(&s4->sin_addr, a.a, 4);
                s4->sin_port = htons((unsigned short)port);
                rc->nslen[rc->nns] = sizeof *s4;
            }
            rc->nns++;
        } else if (!strcmp(key, "search") || !strcmp(key, "domain")) {
            rc->nsearch = 0;
            for (char *w = strtok_r(NULL, " \t\r\n", &save); w && rc->nsearch < 4; w = strtok_r(NULL, " \t\r\n", &save))
                strlcpy(rc->search[rc->nsearch++], w, sizeof rc->search[0]);
        } else if (!strcmp(key, "options")) {
            for (char *w = strtok_r(NULL, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
                if (!strncmp(w, "ndots:", 6)) rc->ndots = atoi(w + 6);
                else if (!strncmp(w, "timeout:", 8)) rc->timeout_ms = atoi(w + 8) * 1000;
                else if (!strncmp(w, "attempts:", 9)) rc->attempts = atoi(w + 9);
            }
        }
    }
    fclose(f);
    if (rc->timeout_ms < 500) rc->timeout_ms = 500;
    if (rc->timeout_ms > 15000) rc->timeout_ms = 15000;
    if (rc->attempts < 1) rc->attempts = 1;
    if (rc->attempts > 5) rc->attempts = 5;
}

/* The IPv4 servers of /etc/resolv.conf (the first interface: IPv6 servers are left out; gap_dns_servers_sa has both). */
int gap_dns_servers(struct sockaddr_in *out, int max) {
    ResConf rc;
    res_load(&rc);
    int n = 0;
    for (int i = 0; i < rc.nns && n < max; i++)
        if (rc.ns[i].ss_family == AF_INET) memcpy(&out[n++], &rc.ns[i], sizeof out[0]);
    return n;
}

/* All the servers, whatever their family; lens[i] is the length of the address structure in out[i]. */
int gap_dns_servers_sa(struct sockaddr_storage *out, socklen_t *lens, int max) {
    ResConf rc;
    res_load(&rc);
    int n = rc.nns < max ? rc.nns : max;
    for (int i = 0; i < n; i++) { out[i] = rc.ns[i]; if (lens) lens[i] = rc.nslen[i]; }
    return n;
}

/* ---------------------------------------------------------------- DNS messages */

static int dns_mkquery(unsigned char *buf, int cap, unsigned short id, const char *name, int qtype) {
    size_t n = strlen(name);
    if (n > 253 || cap < 18 + (int)n) return -1;
    memset(buf, 0, 12);
    buf[0] = (unsigned char)(id >> 8);
    buf[1] = (unsigned char)id;
    buf[2] = 0x01;                                                  /* recursion desired */
    buf[5] = 1;                                                     /* one question */
    int pos = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63) return -1;
        buf[pos++] = (unsigned char)l;
        memcpy(buf + pos, p, l);
        pos += (int)l;
        p += l;
        if (*p == '.') p++;
    }
    buf[pos++] = 0;
    buf[pos++] = (unsigned char)(qtype >> 8);
    buf[pos++] = (unsigned char)qtype;
    buf[pos++] = 0;
    buf[pos++] = 1;                                                 /* class IN */
    return pos;
}

/* Expands the name at `at` (it may use compression pointers into msg). Returns the bytes it occupies at `at`, or -1. */
int gap_dns_name(const unsigned char *msg, int msglen, const unsigned char *at, char *out, int outlen) {
    const unsigned char *p = at;
    const unsigned char *end = msg + msglen;
    int used = -1, o = 0, jumps = 0;
    if (outlen < 2) return -1;
    while (1) {
        if (p >= end) return -1;
        unsigned c = *p;
        if (c == 0) { if (used < 0) used = (int)(p + 1 - at); break; }
        if ((c & 0xC0) == 0xC0) {
            if (p + 1 >= end) return -1;
            if (used < 0) used = (int)(p + 2 - at);
            unsigned off = ((c & 0x3F) << 8) | p[1];
            if ((int)off >= msglen || ++jumps > 32) return -1;
            p = msg + off;
            continue;
        }
        if (c > 63 || p + 1 + c > end) return -1;
        if (o + (int)c + 2 > outlen) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, p + 1, c);
        o += (int)c;
        p += 1 + c;
    }
    out[o] = 0;
    if (o == 0) { out[0] = '.'; out[1] = 0; }
    return used;
}

int gap_dns_walk(const unsigned char *msg, int len, gap_dns_rr_fn fn, void *ctx) {
    if (len < 12) return -1;
    int rcode = msg[3] & 15;
    int counts[4] = { (msg[4] << 8) | msg[5], (msg[6] << 8) | msg[7], (msg[8] << 8) | msg[9], (msg[10] << 8) | msg[11] };
    int pos = 12;
    char name[256];
    for (int i = 0; i < counts[0]; i++) {                           /* questions */
        int n = gap_dns_name(msg, len, msg + pos, name, sizeof name);
        if (n < 0 || pos + n + 4 > len) return -1;
        pos += n + 4;
    }
    for (int sec = 1; sec <= 3; sec++) {
        for (int i = 0; i < counts[sec]; i++) {
            int n = gap_dns_name(msg, len, msg + pos, name, sizeof name);
            if (n < 0 || pos + n + 10 > len) return -1;
            const unsigned char *r = msg + pos + n;
            int type = (r[0] << 8) | r[1], cls = (r[2] << 8) | r[3];
            unsigned ttl = (unsigned)r[4] << 24 | (unsigned)r[5] << 16 | (unsigned)r[6] << 8 | r[7];
            int rdlen = (r[8] << 8) | r[9];
            if (pos + n + 10 + rdlen > len) return -1;
            if (fn) fn(ctx, sec, name, type, cls, ttl, msg, len, r + 10, rdlen);
            pos += n + 10 + rdlen;
        }
    }
    return rcode;
}

/* One query to one server of either family (UDP, retried once). */
int gap_dns_query_sa(const struct sockaddr *server, socklen_t slen, const char *name, int qtype, unsigned char *ans, int cap, int timeout_ms) {
    unsigned char q[300];
    unsigned short id = 0;
    gap_getrandom(&id, sizeof id);
    int qlen = dns_mkquery(q, sizeof q, id, name, qtype);
    if (qlen < 0) { h_errno = NO_RECOVERY; return -1; }
    int fd = socket(server->sa_family, SOCK_DGRAM, 0);
    if (fd < 0) { h_errno = NO_RECOVERY; return -1; }
    if (connect(fd, server, slen) < 0) { close(fd); h_errno = TRY_AGAIN; return -1; }
    int result = -1;
    h_errno = TRY_AGAIN;
    for (int attempt = 0; attempt < 2 && result < 0; attempt++) {
        if (send(fd, q, (size_t)qlen, 0) < 0) break;
        long start = gap_time_ms();
        for (;;) {
            long left = timeout_ms - (gap_time_ms() - start);
            if (left <= 0) break;
            struct gpollfd p = { fd, POLLIN, 0 };
            int r = poll(&p, 1, (int)left);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            ssize_t n = recv(fd, ans, (size_t)cap, 0);
            if (n < 0) { if (errno == ECONNREFUSED) { attempt = 9; } break; }      /* port unreachable: nobody is there */
            if (n < 12 || ((ans[0] << 8) | ans[1]) != id || !(ans[2] & 0x80)) continue;    /* not our answer */
            result = (int)n;
            break;
        }
    }
    close(fd);
    return result;
}

int gap_dns_query(const struct sockaddr_in *server, const char *name, int qtype, unsigned char *ans, int cap, int timeout_ms) {
    return gap_dns_query_sa((const struct sockaddr *)server, sizeof *server, name, qtype, ans, cap, timeout_ms);
}

/* ---------------------------------------------------------------- resolving a name to addresses */

#define MAX_ADDRS 16

typedef struct {
    HostAddr addr[MAX_ADDRS];
    int n;
    char canon[256];
    unsigned ttl;
    int cname_followed;
    char cname_target[256];
    int qtype;                                                      /* the kind of record this answer collects (DNS_A or DNS_AAAA) */
} Answer;

static void collect_rr(void *ctx, int section, const char *name, int type, int cls, unsigned ttl, const unsigned char *msg, int msglen,
                       const unsigned char *rdata, int rdlen) {
    Answer *a = ctx;
    (void)cls;
    if (section != 1) return;
    if (type == DNS_A && a->qtype == DNS_A && rdlen == 4 && a->n < MAX_ADDRS) {
        a->addr[a->n].af = AF_INET;
        memcpy(a->addr[a->n++].a, rdata, 4);
        if (ttl < a->ttl) a->ttl = ttl;
    } else if (type == DNS_AAAA && a->qtype == DNS_AAAA && rdlen == 16 && a->n < MAX_ADDRS) {
        a->addr[a->n].af = AF_INET6;
        memcpy(a->addr[a->n++].a, rdata, 16);
        if (ttl < a->ttl) a->ttl = ttl;
    } else if (type == DNS_CNAME) {
        char target[256];
        if (gap_dns_name(msg, msglen, rdata, target, sizeof target) > 0) {
            strlcpy(a->cname_target, target, sizeof a->cname_target);
            if (!a->canon[0] || !strcasecmp(a->canon, name)) strlcpy(a->canon, target, sizeof a->canon);
            a->cname_followed = 1;
        }
    }
}

/* One name and one kind of record, tried against the servers in order. Returns 1 with addresses, 0 for "no such name / no data", -1 if nobody answered. */
static int ask_servers(const ResConf *rc, const char *name, int qtype, Answer *out, int depth) {
    unsigned char buf[1200];
    int silent = 1;
    for (int s = 0; s < rc->nns; s++) {
        int n = gap_dns_query_sa((const struct sockaddr *)&rc->ns[s], rc->nslen[s], name, qtype, buf, sizeof buf, rc->timeout_ms);
        if (n < 0) continue;
        Answer a;
        memset(&a, 0, sizeof a);
        a.ttl = 0xFFFFFFFFu;
        a.qtype = qtype;
        int rcode = gap_dns_walk(buf, n, collect_rr, &a);
        if (rcode < 0) continue;
        silent = 0;
        if (rcode == 3) return 0;                                   /* NXDOMAIN: the name does not exist */
        if (rcode != 0) continue;                                   /* SERVFAIL and friends: try the next server */
        if (a.n == 0 && a.cname_followed && depth < 5) return ask_servers(rc, a.cname_target, qtype, out, depth + 1);
        if (a.n == 0) return 0;
        *out = a;
        return 1;
    }
    return silent ? -1 : 0;
}

/* The records of the kinds the family asks for (A for AF_INET, AAAA for AF_INET6, both for AF_UNSPEC), merged. 1 with addresses, 0 none, -1 nobody answered. */
static int ask_name(const ResConf *rc, const char *name, int family, Answer *out) {
    Answer a4, a6;
    int r4 = -2, r6 = -2;
    memset(&a4, 0, sizeof a4);
    memset(&a6, 0, sizeof a6);
    if (family != AF_INET6) r4 = ask_servers(rc, name, DNS_A, &a4, 0);
    if (family != AF_INET) r6 = ask_servers(rc, name, DNS_AAAA, &a6, 0);
    if (r4 != 1 && r6 != 1) return (r4 < 0 && r6 < 0) ? -1 : 0;
    memset(out, 0, sizeof *out);
    out->ttl = 0xFFFFFFFFu;
    for (int i = 0; i < a6.n && out->n < MAX_ADDRS; i++) if (r6 == 1) out->addr[out->n++] = a6.addr[i];
    for (int i = 0; i < a4.n && out->n < MAX_ADDRS; i++) if (r4 == 1) out->addr[out->n++] = a4.addr[i];
    if (r6 == 1 && a6.ttl < out->ttl) out->ttl = a6.ttl;
    if (r4 == 1 && a4.ttl < out->ttl) out->ttl = a4.ttl;
    strlcpy(out->canon, (r4 == 1 && a4.canon[0]) ? a4.canon : a6.canon, sizeof out->canon);
    return 1;
}

#define CACHE_SLOTS 16
static struct { char name[96]; int family; Answer a; time_t expire; } g_cache[CACHE_SLOTS];
static int g_cache_next;

static int cache_get(const char *name, int family, Answer *out) {
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (g_cache[i].name[0] && g_cache[i].family == family && now < g_cache[i].expire && !strcasecmp(g_cache[i].name, name)) { *out = g_cache[i].a; return 1; }
    return 0;
}

static void cache_put(const char *name, int family, const Answer *a) {
    if (strlen(name) >= sizeof g_cache[0].name) return;
    unsigned ttl = a->ttl > 300 ? 300 : a->ttl;
    if (ttl == 0) return;
    int i = g_cache_next++ % CACHE_SLOTS;
    strlcpy(g_cache[i].name, name, sizeof g_cache[i].name);
    g_cache[i].family = family;
    g_cache[i].a = *a;
    g_cache[i].expire = time(NULL) + ttl;
}

/* hosts file, then the cache, then the name servers (search list applied). 0 on success, -1 with h_errno set. */
static int resolve_name(const char *name, int family, Answer *out) {
    memset(out, 0, sizeof *out);
    out->ttl = 0xFFFFFFFFu;
    if (!name[0]) { h_errno = HOST_NOT_FOUND; return -1; }
    char canon[256] = "";
    int n = hosts_lookup(name, family, out->addr, MAX_ADDRS, canon, sizeof canon);
    if (n > 0) { out->n = n; strlcpy(out->canon, canon, sizeof out->canon); return 0; }
    if (cache_get(name, family, out)) return 0;
    ResConf rc;
    res_load(&rc);
    if (rc.nns == 0) { h_errno = NO_RECOVERY; return -1; }
    size_t len = strlen(name);
    int absolute = len > 0 && name[len - 1] == '.';
    int dots = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dots++;
    char cand[300];
    char list[6][300];
    int nlist = 0;
    if (absolute) {
        snprintf(list[nlist++], sizeof list[0], "%.*s", (int)len - 1, name);
    } else {
        if (dots >= rc.ndots) strlcpy(list[nlist++], name, sizeof list[0]);
        for (int i = 0; i < rc.nsearch && nlist < 5; i++) {
            snprintf(cand, sizeof cand, "%s.%s", name, rc.search[i]);
            strlcpy(list[nlist++], cand, sizeof list[0]);
        }
        if (dots < rc.ndots) strlcpy(list[nlist++], name, sizeof list[0]);
    }
    int heard = 0;
    for (int i = 0; i < nlist; i++) {
        Answer a;
        int r = ask_name(&rc, list[i], family, &a);
        if (r < 0) continue;
        heard = 1;
        if (r == 1) {
            if (!a.canon[0]) strlcpy(a.canon, list[i], sizeof a.canon);
            *out = a;
            cache_put(name, family, &a);
            return 0;
        }
    }
    h_errno = heard ? HOST_NOT_FOUND : TRY_AGAIN;
    return -1;
}

/* ---------------------------------------------------------------- gethostbyname and friends */

static struct hostent g_he;
static char g_hname[256];
static char *g_haliases[1];
static char *g_hlist[MAX_ADDRS + 1];
static unsigned char g_haddrs[MAX_ADDRS][16];

static struct hostent *fill_hostent(const char *name, int af, const Answer *a) {
    strlcpy(g_hname, a->canon[0] ? a->canon : name, sizeof g_hname);
    int k = 0;
    for (int i = 0; i < a->n; i++) {
        if (a->addr[i].af != af) continue;
        memcpy(g_haddrs[k], a->addr[i].a, (size_t)addr_len(af));
        g_hlist[k] = (char *)g_haddrs[k];
        k++;
    }
    g_hlist[k] = NULL;
    g_haliases[0] = NULL;
    g_he.h_name = g_hname;
    g_he.h_aliases = g_haliases;
    g_he.h_addrtype = af;
    g_he.h_length = addr_len(af);
    g_he.h_addr_list = g_hlist;
    return &g_he;
}

struct hostent *gethostbyname2(const char *name, int af) {
    Answer a;
    HostAddr lit;
    if (!name || (af != AF_INET && af != AF_INET6)) { h_errno = HOST_NOT_FOUND; return NULL; }
    if (parse_addr(name, &lit) && lit.af == af) {
        memset(&a, 0, sizeof a);
        a.addr[0] = lit;
        a.n = 1;
        strlcpy(a.canon, name, sizeof a.canon);
        return fill_hostent(name, af, &a);
    }
    if (resolve_name(name, af, &a) < 0) return NULL;
    return fill_hostent(name, af, &a);
}

struct hostent *gethostbyname(const char *name) { return gethostbyname2(name, AF_INET); }

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    if ((type != AF_INET || len != 4) && (type != AF_INET6 || len != 16)) { h_errno = NO_RECOVERY; return NULL; }
    Answer a;
    memset(&a, 0, sizeof a);
    if (!hosts_reverse(type, addr, a.canon, sizeof a.canon)) { h_errno = HOST_NOT_FOUND; return NULL; }
    a.addr[0].af = type;
    memcpy(a.addr[0].a, addr, len);
    a.n = 1;
    return fill_hostent(a.canon, type, &a);
}

/* ---------------------------------------------------------------- services */

static const struct { const char *name; int port; const char *proto; } g_services[] = {
    { "echo", 7, "tcp" }, { "echo", 7, "udp" }, { "discard", 9, "tcp" }, { "daytime", 13, "tcp" }, { "ftp-data", 20, "tcp" }, { "ftp", 21, "tcp" },
    { "ssh", 22, "tcp" }, { "telnet", 23, "tcp" }, { "smtp", 25, "tcp" }, { "time", 37, "tcp" }, { "domain", 53, "udp" }, { "domain", 53, "tcp" },
    { "bootps", 67, "udp" }, { "bootpc", 68, "udp" }, { "tftp", 69, "udp" }, { "http", 80, "tcp" }, { "www", 80, "tcp" }, { "pop3", 110, "tcp" },
    { "ntp", 123, "udp" }, { "imap", 143, "tcp" }, { "snmp", 161, "udp" }, { "ldap", 389, "tcp" }, { "https", 443, "tcp" }, { "syslog", 514, "udp" },
    { "submission", 587, "tcp" }, { "imaps", 993, "tcp" }, { "pop3s", 995, "tcp" }, { "http-alt", 8080, "tcp" },
};

static struct servent g_se;
static char g_sname[64], g_sproto[8];
static char *g_saliases[1];

static struct servent *serv_fill(const char *name, int port, const char *proto) {
    strlcpy(g_sname, name, sizeof g_sname);
    strlcpy(g_sproto, proto, sizeof g_sproto);
    g_saliases[0] = NULL;
    g_se.s_name = g_sname;
    g_se.s_aliases = g_saliases;
    g_se.s_port = htons((unsigned short)port);
    g_se.s_proto = g_sproto;
    return &g_se;
}

static struct servent *serv_search(const char *name, int port, const char *proto) {      /* name != NULL: by name, else by port (host order) */
    FILE *f = fopen("/etc/services", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = 0;
            char *save = NULL;
            char *n = strtok_r(line, " \t\r\n", &save);
            char *pp = n ? strtok_r(NULL, " \t\r\n", &save) : NULL;
            if (!pp) continue;
            char *slash = strchr(pp, '/');
            if (!slash) continue;
            *slash++ = 0;
            int p = atoi(pp);
            if (proto && strcmp(proto, slash)) continue;
            bool match = name ? !strcasecmp(n, name) : p == port;
            for (char *al = strtok_r(NULL, " \t\r\n", &save); al && name && !match; al = strtok_r(NULL, " \t\r\n", &save)) match = !strcasecmp(al, name);
            if (match) { struct servent *s = serv_fill(n, p, slash); fclose(f); return s; }
        }
        fclose(f);
    }
    for (unsigned i = 0; i < sizeof g_services / sizeof g_services[0]; i++) {
        if (proto && strcmp(proto, g_services[i].proto)) continue;
        if (name ? !strcasecmp(name, g_services[i].name) : port == g_services[i].port) return serv_fill(g_services[i].name, g_services[i].port, g_services[i].proto);
    }
    return NULL;
}

struct servent *getservbyname(const char *name, const char *proto) { return name ? serv_search(name, 0, proto) : NULL; }
struct servent *getservbyport(int port, const char *proto) { return serv_search(NULL, ntohs((unsigned short)port), proto); }

/* ---------------------------------------------------------------- getaddrinfo */

const char *gai_strerror(int code) {
    switch (code) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
    }
}

void freeaddrinfo(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = next;
    }
}

static struct addrinfo *ai_new(const HostAddr *addr, int port, int socktype, int protocol, int flags, const char *canon) {
    struct addrinfo *ai = calloc(1, sizeof *ai);
    size_t sl = addr->af == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    void *sa = calloc(1, sl);
    if (!ai || !sa) { free(ai); free(sa); return NULL; }
    if (addr->af == AF_INET6) {
        struct sockaddr_in6 *s6 = sa;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((unsigned short)port);
        memcpy(&s6->sin6_addr, addr->a, 16);
        s6->sin6_scope_id = addr->scope;
    } else {
        struct sockaddr_in *s4 = sa;
        s4->sin_family = AF_INET;
        s4->sin_port = htons((unsigned short)port);
        memcpy(&s4->sin_addr, addr->a, 4);
    }
    ai->ai_flags = flags;
    ai->ai_family = addr->af;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = (socklen_t)sl;
    ai->ai_addr = sa;
    if (canon) ai->ai_canonname = strdup(canon);
    return ai;
}

/* Is there an address of this family on this machine that is not the loopback's (and, for IPv6, not link-local or still being checked)? The test of AI_ADDRCONFIG. */
static int have_family(int af) {
    if (af == AF_INET) {
        for (unsigned i = 0; i < 64; i++) {
            struct gnet_if r;
            memset(&r, 0, sizeof r);
            r.index = i;
            if (gap_netctl(NETCTL_IF_GET, &r) < 0) break;
            if (!(r.flags & GIFF_LOOPBACK) && (r.flags & GIFF_UP) && r.addr) return 1;
        }
        return 0;
    }
    for (unsigned i = 0; i < 64; i++) {
        struct gnet_addr6 r;
        memset(&r, 0, sizeof r);
        r.index = i;
        if (gap_netctl(NETCTL_ADDR6_GET, &r) < 0) break;
        if (r.scope == G6S_GLOBAL || r.scope == G6S_SITELOCAL) { if (!(r.flags & (G6A_TENTATIVE | G6A_DADFAILED))) return 1; }
    }
    return 0;
}

/* ---- RFC 6724 ---- */

static void to_v6(const HostAddr *a, unsigned char out[16]) {
    if (a->af == AF_INET6) { memcpy(out, a->a, 16); return; }
    memset(out, 0, 10);
    out[10] = out[11] = 0xFF;
    memcpy(out + 12, a->a, 4);
}

static int prefix_match(const unsigned char *a, const unsigned char *b, int bits) {
    int whole = bits / 8, rest = bits % 8;
    if (memcmp(a, b, (size_t)whole)) return 0;
    if (rest && ((a[whole] ^ b[whole]) & (0xFF << (8 - rest)) & 0xFF)) return 0;
    return 1;
}

/* The default policy table of RFC 6724 section 2.1: precedence and label of the longest prefix that matches. */
static void policy_of(const unsigned char a[16], int *prec, int *label) {
    static const struct { unsigned char p[16]; int bits, prec, label; } table[] = {
        { { 0 }, 128, 50, 0 },                                                                                     /* ::1 */
        { { 0xFE, 0xC0 }, 10, 1, 11 },                                                                             /* fec0::/10 */
        { { 0xFC }, 7, 3, 13 },                                                                                    /* fc00::/7 */
        { { 0x20, 0x02 }, 16, 30, 2 },                                                                             /* 2002::/16 */
        { { 0x20, 0x01 }, 32, 5, 5 },                                                                              /* 2001::/32 */
        { { 0x3F, 0xFE }, 16, 1, 12 },                                                                             /* 3ffe::/16 */
        { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF }, 96, 35, 4 },                                               /* ::ffff:0:0/96 */
        { { 0 }, 96, 1, 3 },                                                                                       /* ::/96 */
        { { 0 }, 0, 40, 1 },                                                                                       /* ::/0 */
    };
    unsigned char lo[16] = { 0 };
    lo[15] = 1;
    int best = -1, best_bits = -1;
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++) {
        const unsigned char *p = table[i].p;
        if (i == 0) p = lo;
        if (prefix_match(a, p, table[i].bits) && table[i].bits > best_bits) { best = (int)i; best_bits = table[i].bits; }
    }
    *prec = table[best].prec;
    *label = table[best].label;
}

static int scope_of(const unsigned char a[16]) {
    static const unsigned char lo[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (a[0] == 0xFF) return a[1] & 15;
    if (!memcmp(a, lo, 16)) return 2;
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return 2;
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) return 5;
    if (prefix_match(a, (const unsigned char[16]){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF }, 96))
        return (a[12] == 127 || (a[12] == 169 && a[13] == 254)) ? 2 : 14;
    return 14;
}

typedef struct { HostAddr a; int usable, dscope, sscope, dlabel, slabel, prec, common, index; unsigned char dst[16], src[16]; } Cand;

/* The source address a datagram socket would use to reach the destination (a connected UDP socket picks one and does not send anything). 0 if there is no way. */
static int source_for(const HostAddr *d, unsigned char src[16]) {
    int fd = socket(d->af, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    if (d->af == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(9);
        memcpy(&s6->sin6_addr, d->a, 16);
        s6->sin6_scope_id = d->scope;
        sl = sizeof *s6;
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(9);
        memcpy(&s4->sin_addr, d->a, 4);
        sl = sizeof *s4;
    }
    int ok = 0;
    if (connect(fd, (struct sockaddr *)&ss, sl) == 0) {
        struct sockaddr_storage me;
        socklen_t ml = sizeof me;
        if (getsockname(fd, (struct sockaddr *)&me, &ml) == 0) {
            HostAddr s;
            memset(&s, 0, sizeof s);
            s.af = me.ss_family;
            if (me.ss_family == AF_INET6) memcpy(s.a, &((struct sockaddr_in6 *)&me)->sin6_addr, 16);
            else memcpy(s.a, &((struct sockaddr_in *)&me)->sin_addr, 4);
            to_v6(&s, src);
            ok = 1;
        }
    }
    close(fd);
    return ok;
}

/* True if a should come before b (RFC 6724 section 6, rules 1, 2, 5, 6, 8 and 9; the rest keeps the order the servers gave). */
static int comes_first(const Cand *a, const Cand *b) {
    if (a->usable != b->usable) return a->usable;                                    /* rule 1: avoid unusable destinations */
    if (!a->usable) return a->index < b->index;
    int am = a->dscope == a->sscope, bm = b->dscope == b->sscope;
    if (am != bm) return am;                                                         /* rule 2: prefer matching scope */
    int al = a->dlabel == a->slabel, bl = b->dlabel == b->slabel;
    if (al != bl) return al;                                                         /* rule 5: prefer matching label */
    if (a->prec != b->prec) return a->prec > b->prec;                                /* rule 6: prefer higher precedence */
    if (a->dscope != b->dscope) return a->dscope < b->dscope;                        /* rule 8: prefer smaller scope */
    if (a->a.af == b->a.af && a->common != b->common) return a->common > b->common; /* rule 9: prefer the longer common prefix */
    return a->index < b->index;                                                      /* rule 10: leave the order alone */
}

static void sort_addrs(HostAddr *v, int n) {
    if (n < 2) return;
    Cand c[MAX_ADDRS];
    for (int i = 0; i < n; i++) {
        memset(&c[i], 0, sizeof c[i]);
        c[i].a = v[i];
        c[i].index = i;
        to_v6(&v[i], c[i].dst);
        c[i].usable = source_for(&v[i], c[i].src);
        if (c[i].usable) {
            c[i].sscope = scope_of(c[i].src);
            int sp, sl;
            policy_of(c[i].src, &sp, &sl);
            c[i].slabel = sl;
            int common = 0;
            for (int k = 0; k < 16 && c[i].src[k] == c[i].dst[k]; k++) common += 8;
            if (common < 128) { int k = common / 8; unsigned char x = c[i].src[k] ^ c[i].dst[k]; while (!(x & 0x80)) { common++; x <<= 1; } }
            c[i].common = common;
        }
        c[i].dscope = scope_of(c[i].dst);
        policy_of(c[i].dst, &c[i].prec, &c[i].dlabel);
    }
    for (int i = 1; i < n; i++) {                                                    /* an insertion sort: stable, and n is tiny */
        Cand t = c[i];
        int j = i - 1;
        while (j >= 0 && comes_first(&t, &c[j])) { c[j + 1] = c[j]; j--; }
        c[j + 1] = t;
    }
    for (int i = 0; i < n; i++) v[i] = c[i].a;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    int flags = hints ? hints->ai_flags : 0, family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0, protocol = hints ? hints->ai_protocol : 0;
    *res = NULL;
    if (!node && !service) return EAI_NONAME;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV)) return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM && socktype != SOCK_RAW) return EAI_SOCKTYPE;

    int port = 0;
    if (service && service[0]) {
        char *end;
        long v = strtol(service, &end, 10);
        if (*end == 0 && end != service) {
            if (v < 0 || v > 65535) return EAI_SERVICE;
            port = (int)v;
        } else {
            if (flags & AI_NUMERICSERV) return EAI_NONAME;
            struct servent *se = getservbyname(service, socktype == SOCK_DGRAM ? "udp" : socktype == SOCK_STREAM ? "tcp" : NULL);
            if (!se) return EAI_SERVICE;
            port = ntohs((unsigned short)se->s_port);
        }
    }

    HostAddr list[MAX_ADDRS];
    int n = 0;
    char canon[256] = "";
    bool literal = false;
    if (!node) {                                                                      /* no name: the wildcard (AI_PASSIVE) or the loopback, IPv6 first as the C library of Linux does */
        HostAddr a6, a4;
        memset(&a6, 0, sizeof a6);
        memset(&a4, 0, sizeof a4);
        a6.af = AF_INET6;
        a4.af = AF_INET;
        if (!(flags & AI_PASSIVE)) { a6.a[15] = 1; a4.a[0] = 127; a4.a[3] = 1; }
        if (family != AF_INET) list[n++] = a6;
        if (family != AF_INET6) list[n++] = a4;
        literal = true;
    } else {
        HostAddr lit;
        if (parse_addr(node, &lit)) {
            literal = true;
            if (lit.af == AF_INET && family == AF_INET6) {
                if (!(flags & AI_V4MAPPED)) return EAI_NONAME;
                HostAddr m;
                memset(&m, 0, sizeof m);
                m.af = AF_INET6;
                to_v6(&lit, m.a);
                lit = m;
            } else if (lit.af == AF_INET6 && family == AF_INET) {
                return EAI_NONAME;
            }
            list[n++] = lit;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else {
            Answer a;
            int want = family;
            if (family == AF_INET6 && (flags & AI_V4MAPPED)) want = AF_UNSPEC;         /* the A records are needed for the mapped addresses */
            if ((flags & AI_ADDRCONFIG) && want == AF_UNSPEC) {                       /* a family this machine cannot use is not even asked for */
                int h4 = have_family(AF_INET), h6 = have_family(AF_INET6);
                if (h4 && !h6) want = AF_INET;
                else if (h6 && !h4) want = AF_INET6;
            }
            if (resolve_name(node, want, &a) < 0) return h_errno == TRY_AGAIN ? EAI_AGAIN : h_errno == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
            bool any6 = false;
            for (int i = 0; i < a.n; i++) if (a.addr[i].af == AF_INET6) any6 = true;
            for (int i = 0; i < a.n && n < MAX_ADDRS; i++) {
                HostAddr x = a.addr[i];
                if (family == AF_INET6 && x.af == AF_INET) {
                    if (any6 && !(flags & AI_ALL)) continue;                           /* mapped addresses only when there is no IPv6 one (or AI_ALL) */
                    HostAddr m;
                    memset(&m, 0, sizeof m);
                    m.af = AF_INET6;
                    to_v6(&x, m.a);
                    x = m;
                }
                list[n++] = x;
            }
            strlcpy(canon, a.canon[0] ? a.canon : node, sizeof canon);
        }
        if ((flags & AI_ADDRCONFIG) && !literal && n) {                                /* only the families this machine can use */
            int h4 = have_family(AF_INET), h6 = have_family(AF_INET6), k = 0;
            for (int i = 0; i < n; i++) if ((list[i].af == AF_INET && h4) || (list[i].af == AF_INET6 && h6)) list[k++] = list[i];
            n = k;
            if (!n) return EAI_NONAME;
        }
        if (!literal) sort_addrs(list, n);                                            /* RFC 6724 */
    }

    struct addrinfo *head = NULL, **tail = &head;
    static const struct { int type, proto; } kinds[] = { { SOCK_STREAM, IPPROTO_TCP }, { SOCK_DGRAM, IPPROTO_UDP } };
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            if (socktype && socktype != kinds[k].type) continue;
            if (protocol && protocol != kinds[k].proto) continue;
            const char *cn = ((flags & AI_CANONNAME) && !head) ? (canon[0] ? canon : node) : NULL;
            struct addrinfo *ai = ai_new(&list[i], port, kinds[k].type, kinds[k].proto, flags, cn);
            if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
            *tail = ai;
            tail = &ai->ai_next;
        }
        if (socktype == SOCK_RAW) break;
    }
    if (!head) return EAI_SOCKTYPE;
    *res = head;
    return 0;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
    if (!sa || salen < sizeof(sa_family_t)) return EAI_FAMILY;
    unsigned short port;
    int af = sa->sa_family;
    const unsigned char *addr;
    unsigned scope = 0;
    if (af == AF_INET && salen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        addr = (const unsigned char *)&in->sin_addr;
        port = in->sin_port;
    } else if (af == AF_INET6 && salen >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        addr = (const unsigned char *)&in6->sin6_addr;
        port = in6->sin6_port;
        scope = in6->sin6_scope_id;
    } else {
        return EAI_FAMILY;
    }
    if (host && hostlen) {
        char name[256];
        if (!(flags & NI_NUMERICHOST) && hosts_reverse(af, addr, name, sizeof name)) {
            if (strlen(name) >= hostlen) return EAI_OVERFLOW;
            strcpy(host, name);
        } else {
            if (flags & NI_NAMEREQD) return EAI_NONAME;
            char num[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
            if (!inet_ntop(af, addr, num, sizeof num)) return EAI_OVERFLOW;
            if (af == AF_INET6 && scope && (addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80)) {      /* a link-local address says which link: fe80::1%eth0 */
                char ifn[IF_NAMESIZE + 1];
                size_t l = strlen(num);
                if (if_indextoname(scope, ifn)) snprintf(num + l, sizeof num - l, "%%%s", ifn);
                else snprintf(num + l, sizeof num - l, "%%%u", scope);
            }
            if (strlen(num) >= hostlen) return EAI_OVERFLOW;
            strcpy(host, num);
        }
    }
    if (serv && servlen) {
        struct servent *se = (flags & NI_NUMERICSERV) ? NULL : getservbyport(port, (flags & NI_DGRAM) ? "udp" : "tcp");
        char tmp[32];
        if (se) snprintf(tmp, sizeof tmp, "%s", se->s_name); else snprintf(tmp, sizeof tmp, "%u", ntohs(port));
        if (strlen(tmp) >= servlen) return EAI_OVERFLOW;
        strcpy(serv, tmp);
    }
    return 0;
}
