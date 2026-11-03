/* nslookup and host: ask a name server (the one in /etc/resolv.conf unless one is named) about a name. */
#include "netutil.h"

typedef struct {
    int host_style;                         /* host(1) wording instead of nslookup(1) */
    int found;
} Printer;

static const struct { const char *name; int type; } g_types[] = {
    { "A", DNS_A }, { "NS", DNS_NS }, { "CNAME", DNS_CNAME }, { "SOA", DNS_SOA }, { "PTR", DNS_PTR }, { "MX", DNS_MX },
    { "TXT", DNS_TXT }, { "AAAA", DNS_AAAA }, { "ANY", DNS_ANY },
};

static int parse_type(const char *s) {
    for (unsigned i = 0; i < sizeof g_types / sizeof g_types[0]; i++) if (!strcasecmp(s, g_types[i].name)) return g_types[i].type;
    return -1;
}

static void rr_print(void *ctx, int section, const char *name, int type, int cls, unsigned ttl, const unsigned char *msg, int msglen,
                     const unsigned char *rd, int rdlen) {
    Printer *p = ctx;
    char tgt[256];
    (void)cls; (void)ttl;
    if (section != 1) return;
    switch (type) {
    case DNS_A:
        if (rdlen != 4) return;
        if (p->host_style) printf("%s has address %u.%u.%u.%u\n", name, rd[0], rd[1], rd[2], rd[3]);
        else printf("Name:\t%s\nAddress: %u.%u.%u.%u\n", name, rd[0], rd[1], rd[2], rd[3]);
        break;
    case DNS_AAAA:
        if (rdlen != 16) return;
        printf(p->host_style ? "%s has IPv6 address " : "Name:\t%s\nAddress: ", name);
        for (int i = 0; i < 16; i += 2) printf("%x%s", (rd[i] << 8) | rd[i + 1], i < 14 ? ":" : "\n");
        break;
    case DNS_CNAME:
        if (gap_dns_name(msg, msglen, rd, tgt, sizeof tgt) < 0) return;
        if (p->host_style) printf("%s is an alias for %s.\n", name, tgt); else printf("%s\tcanonical name = %s.\n", name, tgt);
        break;
    case DNS_NS:
        if (gap_dns_name(msg, msglen, rd, tgt, sizeof tgt) < 0) return;
        if (p->host_style) printf("%s name server %s.\n", name, tgt); else printf("%s\tnameserver = %s.\n", name, tgt);
        break;
    case DNS_PTR:
        if (gap_dns_name(msg, msglen, rd, tgt, sizeof tgt) < 0) return;
        if (p->host_style) printf("%s domain name pointer %s.\n", name, tgt); else printf("%s\tname = %s.\n", name, tgt);
        break;
    case DNS_MX:
        if (rdlen < 3 || gap_dns_name(msg, msglen, rd + 2, tgt, sizeof tgt) < 0) return;
        if (p->host_style) printf("%s mail is handled by %d %s.\n", name, (rd[0] << 8) | rd[1], tgt);
        else printf("%s\tmail exchanger = %d %s.\n", name, (rd[0] << 8) | rd[1], tgt);
        break;
    case DNS_TXT: {
        printf(p->host_style ? "%s descriptive text \"" : "%s\ttext = \"", name);
        for (int i = 0; i < rdlen;) {
            int l = rd[i++];
            for (int k = 0; k < l && i < rdlen; k++, i++) putchar(rd[i]);
        }
        printf("\"\n");
        break;
    }
    case DNS_SOA: {
        char mname[256], rname[256];
        int a = gap_dns_name(msg, msglen, rd, mname, sizeof mname);
        if (a < 0) return;
        int b = gap_dns_name(msg, msglen, rd + a, rname, sizeof rname);
        if (b < 0 || a + b + 20 > rdlen) return;
        const unsigned char *t = rd + a + b;
        unsigned serial = (unsigned)t[0] << 24 | (unsigned)t[1] << 16 | (unsigned)t[2] << 8 | t[3];
        if (p->host_style) printf("%s has SOA record %s. %s. %u\n", name, mname, rname, serial);
        else printf("%s\n\torigin = %s\n\tmail addr = %s\n\tserial = %u\n", name, mname, rname, serial);
        break;
    }
    default: return;
    }
    p->found++;
}

static int lookup(int host_style, const char *name, int qtype, const char *server_arg, int verbose_server) {
    struct sockaddr_in servers[3];
    int ns = 0;
    if (server_arg) {
        uint32_t ip;
        unsigned port = 53;
        char tmp[64];
        strlcpy(tmp, server_arg, sizeof tmp);
        char *hash = strchr(tmp, '#');
        if (hash) { *hash++ = 0; port = (unsigned)atoi(hash); }
        if (!nu_resolve(tmp, &ip)) return 1;
        memset(&servers[0], 0, sizeof servers[0]);
        servers[0].sin_family = AF_INET;
        servers[0].sin_addr.s_addr = htonl(ip);
        servers[0].sin_port = htons((unsigned short)port);
        ns = 1;
    } else {
        ns = gap_dns_servers(servers, 3);
    }
    if (ns == 0) { kb_error("no name server is configured (/etc/resolv.conf is empty; has DHCP finished?)"); return 1; }

    char qname[256];
    uint32_t lit;
    if (qtype == DNS_PTR && nu_parse_cidr(name, &lit, NULL)) snprintf(qname, sizeof qname, "%u.%u.%u.%u.in-addr.arpa", lit & 255, (lit >> 8) & 255, (lit >> 16) & 255, lit >> 24);
    else strlcpy(qname, name, sizeof qname);

    static unsigned char ans[1500];
    int n = -1, used = 0;
    for (; used < ns && n < 0; used++) n = gap_dns_query(&servers[used], qname, qtype, ans, sizeof ans, 3000);
    if (n < 0) {
        if (host_style) printf(";; connection timed out; no servers could be reached\n");
        else printf(";; connection timed out; no servers could be reached\n\n");
        return 1;
    }
    used--;
    if (!host_style && verbose_server) {
        printf("Server:\t\t%s\nAddress:\t%s#%u\n\n", nu_ip(ntohl(servers[used].sin_addr.s_addr)), nu_ip(ntohl(servers[used].sin_addr.s_addr)), ntohs(servers[used].sin_port));
    }
    Printer p = { host_style, 0 };
    if (!host_style) printf("Non-authoritative answer:\n");
    int rcode = gap_dns_walk(ans, n, rr_print, &p);
    if (rcode < 0) { printf("%s: malformed answer from the name server\n", host_style ? "Host" : "** server"); return 1; }
    if (rcode == 3) {
        if (host_style) printf("Host %s not found: 3(NXDOMAIN)\n", name); else printf("** server can't find %s: NXDOMAIN\n", name);
        return 1;
    }
    if (rcode != 0) {
        if (host_style) printf("Host %s not found: %d(%s)\n", name, rcode, rcode == 2 ? "SERVFAIL" : rcode == 5 ? "REFUSED" : "ERROR");
        else printf("** server can't find %s: %s\n", name, rcode == 2 ? "SERVFAIL" : rcode == 5 ? "REFUSED" : "ERROR");
        return 1;
    }
    if (!p.found) {
        if (host_style) printf("%s has no %s record\n", name, qtype == DNS_A ? "A" : "such");
        else printf("*** Can't find %s: No answer\n", name);
        return 1;
    }
    return 0;
}

APPLET(nslookup) {
    int type = DNS_A, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i] + 1;
        if (!strncmp(a, "type=", 5) || !strncmp(a, "ty=", 3) || !strncmp(a, "q=", 2) || !strncmp(a, "query=", 6)) {
            type = parse_type(strchr(a, '=') + 1);
            if (type < 0) { kb_error("unknown query type"); return 2; }
        } else if (strncmp(a, "port=", 5) && strncmp(a, "timeout=", 8) && strncmp(a, "retry=", 6)) {
            kb_error("usage: nslookup [-type=TYPE] NAME [SERVER]");
            return 2;
        }
    }
    if (i >= argc) { kb_error("usage: nslookup [-type=TYPE] NAME [SERVER]"); return 2; }
    const char *name = argv[i];
    const char *server = i + 1 < argc ? argv[i + 1] : NULL;
    uint32_t lit;
    if (type == DNS_A && nu_parse_cidr(name, &lit, NULL) && strchr(name, '.')) type = DNS_PTR;       /* nslookup 10.0.2.2 asks for the name */
    return lookup(0, name, type, server, 1);
}

APPLET(host) {
    int type = -1, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            type = parse_type(argv[++i]);
            if (type < 0) { kb_error("unknown query type '%s'", argv[i]); return 2; }
        } else if (strcmp(argv[i], "-a") && strcmp(argv[i], "-v") && strcmp(argv[i], "-4")) {
            kb_error("usage: host [-t TYPE] NAME [SERVER]");
            return 2;
        }
    }
    if (i >= argc) { kb_error("usage: host [-t TYPE] NAME [SERVER]"); return 2; }
    const char *name = argv[i];
    const char *server = i + 1 < argc ? argv[i + 1] : NULL;
    uint32_t lit;
    if (type < 0) type = (nu_parse_cidr(name, &lit, NULL) && strchr(name, '.')) ? DNS_PTR : DNS_A;
    return lookup(1, name, type, server, 0);
}
