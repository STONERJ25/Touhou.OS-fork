/* netstat and ss: sockets, interfaces and routes, read from /proc/net. */
#include "netutil.h"

int applet_route(int argc, char **argv);

typedef struct {
    char proto[5];                                          /* tcp, udp, tcp6, udp6 */
    int v6;
    uint32_t laddr, raddr;
    uint8_t laddr6[16], raddr6[16];
    unsigned lport, rport, state;
    unsigned long txq, rxq;
} SockLine;

static int is_udp(const char *proto) { return !strncmp(proto, "udp", 3); }

static const char *state_name(const char *proto, unsigned st) {
    if (is_udp(proto)) return st == 1 ? "ESTABLISHED" : "";
    static const char *const names[] = { "", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING" };
    return st < sizeof names / sizeof names[0] ? names[st] : "UNKNOWN";
}

static uint32_t swab_hex(unsigned long v) {                 /* /proc prints the address as the little-endian view of the network-order bytes */
    return (uint32_t)((v & 255) << 24 | ((v >> 8) & 255) << 16 | ((v >> 16) & 255) << 8 | ((v >> 24) & 255));
}

/* The 32 hex digits of an IPv6 address in /proc/net/tcp6: four 32-bit words, each the little-endian view of four network-order bytes. */
static int parse_hex6(const char *h, uint8_t out[16]) {
    if (strlen(h) != 32) return 0;
    for (int w = 0; w < 4; w++) {
        char word[9];
        memcpy(word, h + 8 * w, 8);
        word[8] = 0;
        char *end;
        unsigned long v = strtoul(word, &end, 16);
        if (*end) return 0;
        out[4 * w] = (uint8_t)v; out[4 * w + 1] = (uint8_t)(v >> 8); out[4 * w + 2] = (uint8_t)(v >> 16); out[4 * w + 3] = (uint8_t)(v >> 24);
    }
    return 1;
}

static int read_socks(const char *proto, SockLine *v, int max) {
    char path[32];
    snprintf(path, sizeof path, "/proc/net/%s", proto);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[320];
    int n = 0, six = proto[strlen(proto) - 1] == '6';
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }          /* header */
    while (n < max && fgets(line, sizeof line, f)) {
        int sl;
        unsigned long la, ra, tx, rx;
        unsigned lp, rp, st;
        char lh[40] = "", rh[40] = "";
        if (six) {
            if (sscanf(line, "%d: %32[0-9A-Fa-f]:%X %32[0-9A-Fa-f]:%X %X %lX:%lX", &sl, lh, &lp, rh, &rp, &st, &tx, &rx) != 8) continue;
            if (!parse_hex6(lh, v[n].laddr6) || !parse_hex6(rh, v[n].raddr6)) continue;
            la = ra = 0;
        } else if (sscanf(line, "%d: %lX:%X %lX:%X %X %lX:%lX", &sl, &la, &lp, &ra, &rp, &st, &tx, &rx) != 8) continue;
        strlcpy(v[n].proto, proto, sizeof v[n].proto);
        v[n].v6 = six;
        v[n].laddr = swab_hex(la);
        v[n].raddr = swab_hex(ra);
        v[n].lport = lp;
        v[n].rport = rp;
        v[n].state = st;
        v[n].txq = tx;
        v[n].rxq = rx;
        n++;
    }
    fclose(f);
    return n;
}

static void addr_port(char *out, size_t n, uint32_t ip, unsigned port, int star) {
    if (ip == 0 && star) snprintf(out, n, "*:%u", port); else snprintf(out, n, "%s:%u", nu_ip(ip), port);
}

/* An IPv6 end in netstat's form (":::22", "::1:631") or, with brackets, ss's ("[::]:22") */
static void addr_port6(char *out, size_t n, const uint8_t a[16], unsigned port, int brackets) {
    if (brackets) snprintf(out, n, "[%s]:%u", nu_ip6(a), port); else snprintf(out, n, "%s:%u", nu_ip6(a), port);
}

static int any6(const uint8_t a[16]) { for (int i = 0; i < 16; i++) if (a[i]) return 0; return 1; }

static void line_ends(const SockLine *s, char *l, size_t ln, char *r, size_t rn, int brackets, const char *none_v4, const char *none_v6) {
    if (s->v6) {
        addr_port6(l, ln, s->laddr6, s->lport, brackets);
        if (any6(s->raddr6) && s->rport == 0) strlcpy(r, none_v6, rn); else addr_port6(r, rn, s->raddr6, s->rport, brackets);
        return;
    }
    addr_port(l, ln, s->laddr, s->lport, 1);
    if (s->raddr == 0 && s->rport == 0) strlcpy(r, none_v4, rn); else addr_port(r, rn, s->raddr, s->rport, 1);
}

static int show_interfaces(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) { kb_error("/proc/net/dev: %s", strerror(errno)); return 1; }
    char line[256];
    printf("Kernel Interface table\nIface      MTU    RX-OK RX-ERR RX-DRP    TX-OK TX-ERR TX-DRP Flg\n");
    int skip = 2;
    while (fgets(line, sizeof line, f)) {
        if (skip-- > 0) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = line;
        while (*name == ' ') name++;
        unsigned long long rxb, rxp, rxe, rxd, f1, f2, f3, f4, txb, txp, txe, txd;
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &rxb, &rxp, &rxe, &rxd, &f1, &f2, &f3, &f4, &txb, &txp, &txe, &txd) < 12) continue;
        printf("%-8s %5d %8llu %6llu %6llu %8llu %6llu %6llu BMRU\n", name, 1500, rxp, rxe, rxd, txp, txe, txd);
    }
    fclose(f);
    return 0;
}

APPLET(netstat) {
    int all = 0, listening = 0, want_t = 0, want_u = 0, routes = 0, ifaces = 0, six = 0, opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "altunripA:46")) != -1) {
        switch (opt) {
        case 'a': all = 1; break;
        case 'l': listening = 1; break;
        case 't': want_t = 1; break;
        case 'u': want_u = 1; break;
        case 'r': routes = 1; break;
        case 'i': ifaces = 1; break;
        case '6': six = 1; break;
        case 'A': six = !strcmp(optarg, "inet6"); break;
        case 'n': case 'p': case '4': break;
        default: kb_error("usage: netstat [-a] [-l] [-t] [-u] [-n] [-r] [-i] [-A inet|inet6]"); return 2;
        }
    }
    if (routes && six) { char *args[] = { "route", "-A", "inet6", "-n", NULL }; return applet_route(4, args); }
    if (routes) { char *args[] = { "route", "-n", NULL }; return applet_route(2, args); }
    if (ifaces) return show_interfaces();
    if (!want_t && !want_u) want_t = want_u = 1;
    static SockLine v[256];
    int n = 0;
    if (want_t) { n += read_socks("tcp", v + n, 64); n += read_socks("tcp6", v + n, 64); }
    if (want_u) { n += read_socks("udp", v + n, 64); n += read_socks("udp6", v + n, 64); }
    printf("Active Internet connections (%s)\n", all ? "servers and established" : listening ? "only servers" : "w/o servers");
    printf("Proto Recv-Q Send-Q Local Address           Foreign Address         State\n");
    for (int i = 0; i < n; i++) {
        int is_listen = !is_udp(v[i].proto) ? v[i].state == 10 : v[i].state == 7;
        if (!all && listening != is_listen) continue;
        char l[80], r[80];
        line_ends(&v[i], l, sizeof l, r, sizeof r, 0, "0.0.0.0:*", ":::*");
        printf("%-5s %6lu %6lu %-23s %-23s %s\n", v[i].proto, v[i].rxq, v[i].txq, l, r, state_name(v[i].proto, v[i].state));
    }
    return 0;
}

APPLET(ss) {
    int all = 0, listening = 0, want_t = 0, want_u = 0, opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "altunpH")) != -1) {
        switch (opt) {
        case 'a': all = 1; break;
        case 'l': listening = 1; break;
        case 't': want_t = 1; break;
        case 'u': want_u = 1; break;
        case 'n': case 'p': case 'H': break;
        default: kb_error("usage: ss [-a] [-l] [-t] [-u] [-n]"); return 2;
        }
    }
    if (!want_t && !want_u) want_t = want_u = 1;
    static SockLine v[256];
    int n = 0;
    if (want_t) { n += read_socks("tcp", v + n, 64); n += read_socks("tcp6", v + n, 64); }
    if (want_u) { n += read_socks("udp", v + n, 64); n += read_socks("udp6", v + n, 64); }
    printf("%-6s %-10s %-7s %-7s %-24s %s\n", "Netid", "State", "Recv-Q", "Send-Q", "Local Address:Port", "Peer Address:Port");
    for (int i = 0; i < n; i++) {
        int is_listen = !is_udp(v[i].proto) ? v[i].state == 10 : v[i].state == 7;
        if (!all && listening != is_listen) continue;
        char l[80], r[80];
        line_ends(&v[i], l, sizeof l, r, sizeof r, 1, "*:*", "*:*");
        const char *st = is_udp(v[i].proto) ? (v[i].state == 1 ? "ESTAB" : "UNCONN") : (v[i].state == 1 ? "ESTAB" : v[i].state == 10 ? "LISTEN" : state_name("tcp", v[i].state));
        printf("%-6s %-10s %-7lu %-7lu %-24s %s\n", is_udp(v[i].proto) ? "udp" : "tcp", st, v[i].rxq, v[i].txq, l, r);
    }
    return 0;
}
