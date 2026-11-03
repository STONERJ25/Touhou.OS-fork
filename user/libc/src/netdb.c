/*
 * Names and services: /etc/hosts, a DNS resolver over UDP (A records, CNAME chains, the search list of /etc/resolv.conf, a
 * small cache), gethostbyname / getaddrinfo / getnameinfo / getservbyname. IPv4 only.
 *
 * /etc/resolv.conf understands "nameserver ADDR[#PORT]" (the port is an extension, handy for tests), "search" and "domain",
 * and "options ndots:N timeout:SECONDS attempts:N".
 */
#include <netdb.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/socket.h>
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

/* ---------------------------------------------------------------- /etc/hosts */

static int hosts_lookup(const char *name, unsigned char addrs[][4], int max, char *canon, size_t canonlen) {
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL;
        char *addr = strtok_r(line, " \t\r\n", &save);
        if (!addr || strchr(addr, ':')) continue;
        struct in_addr a;
        if (!inet_aton(addr, &a)) continue;
        char *first = NULL;
        for (char *w = strtok_r(NULL, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
            if (!first) first = w;
            if (!strcasecmp(w, name)) {
                memcpy(addrs[n++], &a, 4);
                if (canon && !canon[0]) strlcpy(canon, first, canonlen);
                break;
            }
        }
    }
    fclose(f);
    return n;
}

static int hosts_reverse(const unsigned char addr[4], char *name, size_t n) {
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL;
        char *a = strtok_r(line, " \t\r\n", &save);
        struct in_addr ia;
        if (!a || !inet_aton(a, &ia) || memcmp(&ia, addr, 4)) continue;
        char *w = strtok_r(NULL, " \t\r\n", &save);
        if (w) { strlcpy(name, w, n); found = 1; }
    }
    fclose(f);
    return found;
}

/* ---------------------------------------------------------------- /etc/resolv.conf */

typedef struct {
    struct sockaddr_in ns[3];
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
            struct in_addr a;
            if (!inet_aton(v, &a)) continue;
            struct sockaddr_in *s = &rc->ns[rc->nns++];
            memset(s, 0, sizeof *s);
            s->sin_family = AF_INET;
            s->sin_addr = a;
            s->sin_port = htons((unsigned short)port);
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

int gap_dns_servers(struct sockaddr_in *out, int max) {
    ResConf rc;
    res_load(&rc);
    int n = rc.nns < max ? rc.nns : max;
    for (int i = 0; i < n; i++) out[i] = rc.ns[i];
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

int gap_dns_query(const struct sockaddr_in *server, const char *name, int qtype, unsigned char *ans, int cap, int timeout_ms) {
    unsigned char q[300];
    unsigned short id = 0;
    gap_getrandom(&id, sizeof id);
    int qlen = dns_mkquery(q, sizeof q, id, name, qtype);
    if (qlen < 0) { h_errno = NO_RECOVERY; return -1; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { h_errno = NO_RECOVERY; return -1; }
    if (connect(fd, (const struct sockaddr *)server, sizeof *server) < 0) { close(fd); h_errno = TRY_AGAIN; return -1; }
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

/* ---------------------------------------------------------------- resolving a name to addresses */

typedef struct {
    unsigned char addr[8][4];
    int n;
    char canon[256];
    unsigned ttl;
    int cname_followed;
    char cname_target[256];
} Answer;

static void collect_rr(void *ctx, int section, const char *name, int type, int cls, unsigned ttl, const unsigned char *msg, int msglen,
                       const unsigned char *rdata, int rdlen) {
    Answer *a = ctx;
    (void)cls;
    if (section != 1) return;
    if (type == DNS_A && rdlen == 4 && a->n < 8) {
        memcpy(a->addr[a->n++], rdata, 4);
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

/* One name, tried against the servers in order. Returns 1 with addresses, 0 for "no such name / no data", -1 if nobody answered. */
static int ask_servers(const ResConf *rc, const char *name, Answer *out, int depth) {
    unsigned char buf[1200];
    int silent = 1;
    for (int s = 0; s < rc->nns; s++) {
        int n = gap_dns_query(&rc->ns[s], name, DNS_A, buf, sizeof buf, rc->timeout_ms);
        if (n < 0) continue;
        Answer a;
        memset(&a, 0, sizeof a);
        a.ttl = 0xFFFFFFFFu;
        int rcode = gap_dns_walk(buf, n, collect_rr, &a);
        if (rcode < 0) continue;
        silent = 0;
        if (rcode == 3) return 0;                                   /* NXDOMAIN: the name does not exist */
        if (rcode != 0) continue;                                   /* SERVFAIL and friends: try the next server */
        if (a.n == 0 && a.cname_followed && depth < 5) return ask_servers(rc, a.cname_target, out, depth + 1);
        if (a.n == 0) return 0;
        *out = a;
        return 1;
    }
    return silent ? -1 : 0;
}

#define CACHE_SLOTS 16
static struct { char name[96]; Answer a; time_t expire; } g_cache[CACHE_SLOTS];
static int g_cache_next;

static int cache_get(const char *name, Answer *out) {
    time_t now = time(NULL);
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (g_cache[i].name[0] && now < g_cache[i].expire && !strcasecmp(g_cache[i].name, name)) { *out = g_cache[i].a; return 1; }
    return 0;
}

static void cache_put(const char *name, const Answer *a) {
    if (strlen(name) >= sizeof g_cache[0].name) return;
    unsigned ttl = a->ttl > 300 ? 300 : a->ttl;
    if (ttl == 0) return;
    int i = g_cache_next++ % CACHE_SLOTS;
    strlcpy(g_cache[i].name, name, sizeof g_cache[i].name);
    g_cache[i].a = *a;
    g_cache[i].expire = time(NULL) + ttl;
}

/* hosts file, then the cache, then the name servers (search list applied). 0 on success, -1 with h_errno set. */
static int resolve_name(const char *name, Answer *out) {
    memset(out, 0, sizeof *out);
    out->ttl = 0xFFFFFFFFu;
    if (!name[0]) { h_errno = HOST_NOT_FOUND; return -1; }
    char canon[256] = "";
    int n = hosts_lookup(name, out->addr, 8, canon, sizeof canon);
    if (n > 0) { out->n = n; strlcpy(out->canon, canon, sizeof out->canon); return 0; }
    if (cache_get(name, out)) return 0;
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
        int r = ask_servers(&rc, list[i], &a, 0);
        if (r < 0) continue;
        heard = 1;
        if (r == 1) {
            if (!a.canon[0]) strlcpy(a.canon, list[i], sizeof a.canon);
            *out = a;
            cache_put(name, &a);
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
static char *g_hlist[9];
static unsigned char g_haddrs[8][4];

static struct hostent *fill_hostent(const char *name, const Answer *a) {
    strlcpy(g_hname, a->canon[0] ? a->canon : name, sizeof g_hname);
    for (int i = 0; i < a->n; i++) { memcpy(g_haddrs[i], a->addr[i], 4); g_hlist[i] = (char *)g_haddrs[i]; }
    g_hlist[a->n] = NULL;
    g_haliases[0] = NULL;
    g_he.h_name = g_hname;
    g_he.h_aliases = g_haliases;
    g_he.h_addrtype = AF_INET;
    g_he.h_length = 4;
    g_he.h_addr_list = g_hlist;
    return &g_he;
}

struct hostent *gethostbyname(const char *name) {
    struct in_addr lit;
    Answer a;
    if (!name) { h_errno = HOST_NOT_FOUND; return NULL; }
    if (isdigit((unsigned char)name[0]) && inet_aton(name, &lit)) {
        memset(&a, 0, sizeof a);
        memcpy(a.addr[0], &lit, 4);
        a.n = 1;
        strlcpy(a.canon, name, sizeof a.canon);
        return fill_hostent(name, &a);
    }
    if (resolve_name(name, &a) < 0) return NULL;
    return fill_hostent(name, &a);
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    if (type != AF_INET || len != 4) { h_errno = NO_RECOVERY; return NULL; }
    Answer a;
    memset(&a, 0, sizeof a);
    if (!hosts_reverse(addr, a.canon, sizeof a.canon)) { h_errno = HOST_NOT_FOUND; return NULL; }
    memcpy(a.addr[0], addr, 4);
    a.n = 1;
    return fill_hostent(a.canon, &a);
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

static struct addrinfo *ai_new(const unsigned char addr[4], int port, int socktype, int protocol, int flags, const char *canon) {
    struct addrinfo *ai = calloc(1, sizeof *ai);
    struct sockaddr_in *sa = calloc(1, sizeof *sa);
    if (!ai || !sa) { free(ai); free(sa); return NULL; }
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    memcpy(&sa->sin_addr, addr, 4);
    ai->ai_flags = flags;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof *sa;
    ai->ai_addr = (struct sockaddr *)sa;
    if (canon) ai->ai_canonname = strdup(canon);
    return ai;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    int flags = hints ? hints->ai_flags : 0, family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0, protocol = hints ? hints->ai_protocol : 0;
    *res = NULL;
    if (!node && !service) return EAI_NONAME;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV)) return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
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

    Answer a;
    memset(&a, 0, sizeof a);
    if (!node) {
        static const unsigned char any[4] = { 0, 0, 0, 0 }, lo[4] = { 127, 0, 0, 1 };
        memcpy(a.addr[0], (flags & AI_PASSIVE) ? any : lo, 4);
        a.n = 1;
    } else {
        struct in_addr lit;
        if (isdigit((unsigned char)node[0]) && inet_aton(node, &lit)) {
            memcpy(a.addr[0], &lit, 4);
            a.n = 1;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (resolve_name(node, &a) < 0) {
            return h_errno == TRY_AGAIN ? EAI_AGAIN : h_errno == NO_RECOVERY ? EAI_FAIL : EAI_NONAME;
        }
    }

    struct addrinfo *head = NULL, **tail = &head;
    static const struct { int type, proto; } kinds[] = { { SOCK_STREAM, IPPROTO_TCP }, { SOCK_DGRAM, IPPROTO_UDP } };
    for (int i = 0; i < a.n; i++) {
        for (int k = 0; k < 2; k++) {
            if (socktype && socktype != kinds[k].type) continue;
            if (protocol && protocol != kinds[k].proto) continue;
            const char *canon = ((flags & AI_CANONNAME) && !head) ? (a.canon[0] ? a.canon : node) : NULL;
            struct addrinfo *ai = ai_new(a.addr[i], port, kinds[k].type, kinds[k].proto, flags, canon);
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
    if (salen < sizeof(struct sockaddr_in) || sa->sa_family != AF_INET) return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    if (host && hostlen) {
        char name[256];
        if (!(flags & NI_NUMERICHOST) && hosts_reverse((const unsigned char *)&in->sin_addr, name, sizeof name)) {
            if (strlen(name) >= hostlen) return EAI_OVERFLOW;
            strcpy(host, name);
        } else {
            if (flags & NI_NAMEREQD) return EAI_NONAME;
            if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) return EAI_OVERFLOW;
        }
    }
    if (serv && servlen) {
        struct servent *se = (flags & NI_NUMERICSERV) ? NULL : getservbyport(in->sin_port, (flags & NI_DGRAM) ? "udp" : "tcp");
        char tmp[32];
        if (se) snprintf(tmp, sizeof tmp, "%s", se->s_name); else snprintf(tmp, sizeof tmp, "%u", ntohs(in->sin_port));
        if (strlen(tmp) >= servlen) return EAI_OVERFLOW;
        strcpy(serv, tmp);
    }
    return 0;
}
