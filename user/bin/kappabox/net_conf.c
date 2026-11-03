/*
 * netconf: applies /etc/network.conf (network.conf(5)). init runs it before the first session. A card whose line says "static" gets that address,
 * default route and name servers instead of asking DHCP; a card that has no line, or a "dhcp" line, is left to the DHCP client as before.
 *
 *   netconf [-n] [FILE]      -n: only say what would be done
 */
#include "netutil.h"
#include <netabi.h>

#define MAXIF 16
#define MAXTOK 16

typedef struct {
    char iface[16];
    bool is_static, has_mac;
    uint32_t addr, mask, gw, dns[3];
    int ndns;
    uint8_t mac[6];
} Line;

static int get_ifs(struct gnet_if *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_IF_GET, &v[n]) < 0) break;
    }
    return n;
}

static bool parse_mac(const char *s, uint8_t mac[6]) {
    unsigned b[6];
    char junk;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &junk) != 6) return false;
    for (int i = 0; i < 6; i++) { if (b[i] > 255) return false; mac[i] = (uint8_t)b[i]; }
    return true;
}

/* One line of the file; false when it is blank, a comment, or not understood (the file was checked when it was kept, so that is rare). */
static bool parse_line(char *text, Line *l) {
    char *hash = strchr(text, '#');
    if (hash) *hash = 0;
    char *tok[MAXTOK];
    int n = 0;
    for (char *p = strtok(text, " \t\r\n"); p && n < MAXTOK; p = strtok(NULL, " \t\r\n")) tok[n++] = p;
    if (n < 2) return false;
    memset(l, 0, sizeof *l);
    strlcpy(l->iface, tok[0], sizeof l->iface);
    if (!strcmp(tok[1], "dhcp")) return true;
    if (strcmp(tok[1], "static") || n < 3) return false;
    int prefix = -1;
    if (!nu_parse_cidr(tok[2], &l->addr, &prefix) || prefix < 1 || prefix > 30) return false;
    l->mask = nu_prefix_mask(prefix);
    l->is_static = true;
    for (int i = 3; i < n; i++) {
        if (!strcmp(tok[i], "gateway") && i + 1 < n) { if (!nu_parse_cidr(tok[++i], &l->gw, NULL)) return false; }
        else if (!strcmp(tok[i], "mac") && i + 1 < n) { if (!parse_mac(tok[++i], l->mac)) return false; l->has_mac = true; }
        else if (!strcmp(tok[i], "dns")) {
            while (i + 1 < n && l->ndns < 3 && isdigit((unsigned char)tok[i + 1][0])) { if (!nu_parse_cidr(tok[++i], &l->dns[l->ndns], NULL)) return false; l->ndns++; }
        } else return false;
    }
    return true;
}

APPLET(netconf) {
    bool dry = false;
    const char *file = "/etc/network.conf";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) dry = true;
        else if (argv[i][0] != '-') file = argv[i];
        else { kb_error("usage: netconf [-n] [FILE]"); return 2; }
    }
    FILE *f = fopen(file, "r");
    if (!f) return 0;                                           /* no file: every card uses DHCP */
    Line lines[16];
    int nl = 0;
    char buf[256];
    while (nl < 16 && fgets(buf, sizeof buf, f)) {
        Line l;
        if (parse_line(buf, &l)) lines[nl++] = l;
    }
    fclose(f);

    struct gnet_if ifs[MAXIF];
    int nif = get_ifs(ifs, MAXIF), rc = 0;
    for (int k = 0; k < nif; k++) {
        const struct gnet_if *c = &ifs[k];
        if (c->flags & GIFF_LOOPBACK) continue;
        const Line *best = NULL;
        for (int i = 0; i < nl; i++) {                          /* a line for this card; one that names its hardware address beats one that does not */
            if (strcmp(lines[i].iface, c->name)) continue;
            if (lines[i].has_mac && memcmp(lines[i].mac, c->mac, 6)) continue;
            if (!best || (lines[i].has_mac && !best->has_mac)) best = &lines[i];
        }
        if (!best || !best->is_static) continue;
        char msg[200];
        int o = snprintf(msg, sizeof msg, "%s: static %s/%d", c->name, nu_ip(best->addr), nu_mask_len(best->mask));
        if (best->gw) o += snprintf(msg + o, sizeof msg - (size_t)o, ", gateway %s", nu_ip(best->gw));
        for (int d = 0; d < best->ndns; d++) o += snprintf(msg + o, sizeof msg - (size_t)o, "%s %s", d ? "," : ", dns", nu_ip(best->dns[d]));
        if (dry) { printf("netconf: would set %s\n", msg); continue; }
        struct gnet_if r;
        memset(&r, 0, sizeof r);
        strlcpy(r.name, c->name, sizeof r.name);
        r.addr = best->addr;
        r.mask = best->mask;
        r.flags = GIFF_UP;
        if (gap_netctl(NETCTL_IF_CONFIG, &r) < 0) { kb_error("%s: %s", c->name, strerror(errno)); rc = 1; continue; }
        if (best->gw) {
            struct gnet_route rt;
            memset(&rt, 0, sizeof rt);
            strlcpy(rt.ifname, c->name, sizeof rt.ifname);
            rt.gw = best->gw;
            if (gap_netctl(NETCTL_ROUTE_ADD, &rt) < 0) { kb_error("%s: default route: %s", c->name, strerror(errno)); rc = 1; }
        }
        if (best->ndns) {
            FILE *rf = fopen("/etc/resolv.conf", "w");
            if (rf) {
                fprintf(rf, "# written by netconf from %s (%s)\n", file, c->name);
                for (int d = 0; d < best->ndns; d++) fprintf(rf, "nameserver %s\n", nu_ip(best->dns[d]));
                fclose(rf);
            }
        }
        gap_setname("netconf");
        gap_log(msg);
        printf("netconf: %s\n", msg);
    }
    return rc;
}
