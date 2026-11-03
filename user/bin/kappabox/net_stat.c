/* netstat and ss: sockets, interfaces and routes, read from /proc/net. */
#include "netutil.h"

int applet_route(int argc, char **argv);

typedef struct {
    char proto[4];
    uint32_t laddr, raddr;
    unsigned lport, rport, state;
    unsigned long txq, rxq;
} SockLine;

static const char *state_name(const char *proto, unsigned st) {
    if (!strcmp(proto, "udp")) return st == 1 ? "ESTABLISHED" : "";
    static const char *const names[] = { "", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING" };
    return st < sizeof names / sizeof names[0] ? names[st] : "UNKNOWN";
}

static uint32_t swab_hex(unsigned long v) {                 /* /proc prints the address as the little-endian view of the network-order bytes */
    return (uint32_t)((v & 255) << 24 | ((v >> 8) & 255) << 16 | ((v >> 16) & 255) << 8 | ((v >> 24) & 255));
}

static int read_socks(const char *proto, SockLine *v, int max) {
    char path[32];
    snprintf(path, sizeof path, "/proc/net/%s", proto);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }          /* header */
    while (n < max && fgets(line, sizeof line, f)) {
        int sl;
        unsigned long la, ra, tx, rx;
        unsigned lp, rp, st;
        if (sscanf(line, "%d: %lX:%X %lX:%X %X %lX:%lX", &sl, &la, &lp, &ra, &rp, &st, &tx, &rx) != 8) continue;
        strlcpy(v[n].proto, proto, sizeof v[n].proto);
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
    int all = 0, listening = 0, want_t = 0, want_u = 0, routes = 0, ifaces = 0, opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "altunrip")) != -1) {
        switch (opt) {
        case 'a': all = 1; break;
        case 'l': listening = 1; break;
        case 't': want_t = 1; break;
        case 'u': want_u = 1; break;
        case 'r': routes = 1; break;
        case 'i': ifaces = 1; break;
        case 'n': case 'p': break;
        default: kb_error("usage: netstat [-a] [-l] [-t] [-u] [-n] [-r] [-i]"); return 2;
        }
    }
    if (routes) { char *args[] = { "route", "-n", NULL }; return applet_route(2, args); }
    if (ifaces) return show_interfaces();
    if (!want_t && !want_u) want_t = want_u = 1;
    static SockLine v[256];
    int n = 0;
    if (want_t) n += read_socks("tcp", v + n, 128);
    if (want_u) n += read_socks("udp", v + n, 128);
    printf("Active Internet connections (%s)\n", all ? "servers and established" : listening ? "only servers" : "w/o servers");
    printf("Proto Recv-Q Send-Q Local Address           Foreign Address         State\n");
    for (int i = 0; i < n; i++) {
        int is_listen = !strcmp(v[i].proto, "tcp") ? v[i].state == 10 : v[i].state == 7;
        if (!all && listening != is_listen) continue;
        char l[40], r[40];
        addr_port(l, sizeof l, v[i].laddr, v[i].lport, 1);
        addr_port(r, sizeof r, v[i].raddr, v[i].rport, 1);
        if (v[i].raddr == 0 && v[i].rport == 0) strlcpy(r, "0.0.0.0:*", sizeof r);
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
    if (want_t) n += read_socks("tcp", v + n, 128);
    if (want_u) n += read_socks("udp", v + n, 128);
    printf("%-6s %-10s %-7s %-7s %-24s %s\n", "Netid", "State", "Recv-Q", "Send-Q", "Local Address:Port", "Peer Address:Port");
    for (int i = 0; i < n; i++) {
        int is_listen = !strcmp(v[i].proto, "tcp") ? v[i].state == 10 : v[i].state == 7;
        if (!all && listening != is_listen) continue;
        char l[40], r[40];
        addr_port(l, sizeof l, v[i].laddr, v[i].lport, 1);
        addr_port(r, sizeof r, v[i].raddr, v[i].rport, 1);
        if (v[i].raddr == 0 && v[i].rport == 0) strlcpy(r, "*:*", sizeof r);
        const char *st = !strcmp(v[i].proto, "udp") ? (v[i].state == 1 ? "ESTAB" : "UNCONN") : (v[i].state == 1 ? "ESTAB" : v[i].state == 10 ? "LISTEN" : state_name("tcp", v[i].state));
        printf("%-6s %-10s %-7lu %-7lu %-24s %s\n", v[i].proto, st, v[i].rxq, v[i].txq, l, r);
    }
    return 0;
}
