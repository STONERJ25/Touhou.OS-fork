/* ifconfig, ip and route: look at and change interfaces, addresses and routes (through SYS_netctl). */
#include "netutil.h"

#define MAXIF 16

static int get_ifs(struct gnet_if *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_IF_GET, &v[n]) < 0) break;
    }
    return n;
}

static struct gnet_if *find_if(struct gnet_if *v, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(v[i].name, name)) return &v[i];
    return NULL;
}

#define MAXA6 32

static int get_addrs6(struct gnet_addr6 *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_ADDR6_GET, &v[n]) < 0) break;
    }
    return n;
}

static int get_routes6(struct gnet_route6 *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_ROUTE6_GET, &v[n]) < 0) break;
    }
    return n;
}

static int get_nbrs6(struct gnet_nbr6 *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_NBR6_GET, &v[n]) < 0) break;
    }
    return n;
}

/* An IPv6 address on a card: added (it goes through duplicate address detection before it is used) or removed; the lifetimes are for ever. */
static int addr6_change(int add, const char *dev, const uint8_t addr[16], int plen) {
    struct gnet_addr6 r;
    memset(&r, 0, sizeof r);
    strlcpy(r.ifname, dev, sizeof r.ifname);
    memcpy(r.addr, addr, 16);
    r.plen = (uint8_t)(plen < 0 ? 64 : plen);
    if (gap_netctl(add ? NETCTL_ADDR6_ADD : NETCTL_ADDR6_DEL, &r) < 0) {
        kb_error("%s: %s", dev, (!add && errno == EADDRNOTAVAIL) ? "Cannot assign requested address" : strerror(errno));
        return 1;
    }
    return 0;
}

static int config_if(const char *name, uint32_t addr, uint32_t mask, uint32_t flags, uint32_t mtu) {
    struct gnet_if r;
    memset(&r, 0, sizeof r);
    strlcpy(r.name, name, sizeof r.name);
    r.addr = addr;
    r.mask = mask;
    r.flags = flags;
    r.mtu = mtu;
    if (gap_netctl(NETCTL_IF_CONFIG, &r) < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }
    return 0;
}

/* ---------------------------------------------------------------- ifconfig */

static const char *flag_names(const struct gnet_if *r, char *buf, size_t n) {
    buf[0] = 0;
    if (r->flags & GIFF_UP) strlcat(buf, "UP,", n);
    if (r->flags & GIFF_LOOPBACK) strlcat(buf, "LOOPBACK,", n);
    if (r->flags & GIFF_BROADCAST) strlcat(buf, "BROADCAST,", n);
    if (r->flags & GIFF_RUNNING) strlcat(buf, "RUNNING,", n);
    if (r->flags & GIFF_NOARP) strlcat(buf, "NOARP,", n);
    size_t l = strlen(buf);
    if (l) buf[l - 1] = 0;
    return buf;
}

static void show_ifconfig(const struct gnet_if *r, const struct gnet_addr6 *a6, int na6) {
    char fl[64], b1[16], b2[16];
    unsigned numeric = r->flags & (GIFF_UP | GIFF_BROADCAST | GIFF_LOOPBACK | GIFF_RUNNING | GIFF_NOARP);
    printf("%s: flags=%u<%s>  mtu %u\n", r->name, numeric, flag_names(r, fl, sizeof fl), r->mtu);
    if (r->addr) {
        printf("        inet %s  netmask %s", nu_ip(r->addr), nu_ip(r->mask));
        if (r->bcast) printf("  broadcast %s", nu_ip(r->bcast));
        printf("\n");
    }
    for (int k = 0; k < na6; k++) {                                          /* net-tools' form: inet6 ADDR  prefixlen N  scopeid 0x20<link> */
        if (strcmp(a6[k].ifname, r->name) || (a6[k].flags & G6A_DADFAILED)) continue;
        printf("        inet6 %s  prefixlen %u  scopeid 0x%x<%s>\n", nu_ip6(a6[k].addr), a6[k].plen, a6[k].scope, nu_scope6(a6[k].scope));
    }
    if (r->flags & GIFF_LOOPBACK) printf("        loop  (Local Loopback)\n");
    else printf("        ether %s  (Ethernet)  %s\n", nu_mac(r->mac), r->driver);
    printf("        RX packets %llu  bytes %llu (%s)\n", (unsigned long long)r->rx_packets, (unsigned long long)r->rx_bytes,
           kb_format_size(r->rx_bytes, b1, sizeof b1));
    printf("        RX errors %llu  dropped %llu\n", (unsigned long long)r->rx_errors, (unsigned long long)r->rx_dropped);
    printf("        TX packets %llu  bytes %llu (%s)\n", (unsigned long long)r->tx_packets, (unsigned long long)r->tx_bytes,
           kb_format_size(r->tx_bytes, b2, sizeof b2));
    printf("        TX errors %llu  dropped %llu\n\n", (unsigned long long)r->tx_errors, (unsigned long long)r->tx_dropped);
}

APPLET(ifconfig) {
    struct gnet_if v[MAXIF];
    struct gnet_addr6 a6[MAXA6];
    int n = get_ifs(v, MAXIF);
    int na6 = get_addrs6(a6, MAXA6);
    int all = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-a")) { all = 1; i++; }
    if (i >= argc) {
        for (int k = 0; k < n; k++) if (all || (v[k].flags & GIFF_UP)) show_ifconfig(&v[k], a6, na6);
        return 0;
    }
    const char *name = argv[i++];
    struct gnet_if *cur = find_if(v, n, name);
    if (!cur) { kb_error("%s: no such interface", name); return 1; }
    if (i >= argc) { show_ifconfig(cur, a6, na6); return 0; }
    if (!strcmp(argv[i], "inet6")) i++;                                      /* ifconfig IFACE [inet6] add|del ADDR[/LEN] */
    if (i + 1 < argc && (!strcmp(argv[i], "add") || !strcmp(argv[i], "del"))) {
        uint8_t a[16];
        int prefix = -1;
        if (!nu_parse_cidr6(argv[i + 1], a, &prefix)) { kb_error("bad IPv6 address '%s'", argv[i + 1]); return 1; }
        return addr6_change(!strcmp(argv[i], "add"), name, a, prefix);
    }
    uint32_t addr = 0, mask = 0, flags = 0, mtu = 0;
    int have_addr = 0, want_dhcp = 0, want_nodhcp = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "up")) flags |= GIFF_UP;
        else if (!strcmp(a, "down")) flags |= GIFF_DOWN;
        else if (!strcmp(a, "dhcp")) want_dhcp = 1;
        else if (!strcmp(a, "nodhcp")) want_nodhcp = 1;
        else if (!strcmp(a, "netmask") && i + 1 < argc) {
            struct in_addr m;
            if (!inet_aton(argv[++i], &m)) { kb_error("bad netmask '%s'", argv[i]); return 1; }
            mask = ntohl(m.s_addr);
        } else if (!strcmp(a, "broadcast") && i + 1 < argc) {
            i++;                                           /* the kernel derives it from address and mask */
        } else if (!strcmp(a, "mtu") && i + 1 < argc) {
            mtu = (uint32_t)atoi(argv[++i]);
        } else {
            int prefix = -1;
            uint8_t a6addr[16];
            if (nu_parse_cidr6(a, a6addr, &prefix)) return addr6_change(1, name, a6addr, prefix);     /* an IPv6 address on the line is added */
            if (!nu_parse_cidr(a, &addr, &prefix)) { kb_error("unknown option or bad address '%s'", a); return 1; }
            have_addr = 1;
            if (prefix >= 0) mask = nu_prefix_mask(prefix);
        }
    }
    if (!have_addr && mask && cur->addr) { addr = cur->addr; have_addr = 1; }                  /* "ifconfig eth0 netmask M" keeps the address */
    if (have_addr && !mask) mask = (addr >> 24) < 128 ? 0xFF000000u : (addr >> 24) < 192 ? 0xFFFF0000u : 0xFFFFFF00u;
    if (want_nodhcp && !want_dhcp) {                                 /* stop the DHCP client; the address it set stays, as a static one */
        struct gnet_if r;
        memset(&r, 0, sizeof r);
        strlcpy(r.name, name, sizeof r.name);
        if (gap_netctl(NETCTL_DHCP_STOP, &r) < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }
        if (!have_addr && !flags && !mtu) return 0;
    }
    if (want_dhcp) {
        struct gnet_if r;
        memset(&r, 0, sizeof r);
        strlcpy(r.name, name, sizeof r.name);
        if (gap_netctl(NETCTL_DHCP, &r) < 0) { kb_error("%s: %s", name, strerror(errno)); return 1; }
        return 0;
    }
    if (!have_addr && !flags && !mtu) { kb_error("usage: ifconfig [-a] [IFACE [ADDR[/LEN] [netmask MASK] [mtu N] [up|down|dhcp|nodhcp]]]   |   ifconfig IFACE [inet6] add|del ADDR[/LEN]"); return 2; }
    if (have_addr || mtu || flags) {
        if (!have_addr && mask) { addr = cur->addr; }
        return config_if(name, have_addr ? addr : 0, have_addr ? mask : 0, flags, mtu);
    }
    return 0;
}

/* ---------------------------------------------------------------- route */

static int get_routes(struct gnet_route *v, int max) {
    int n = 0;
    for (; n < max; n++) {
        memset(&v[n], 0, sizeof v[n]);
        v[n].index = (uint32_t)n;
        if (gap_netctl(NETCTL_ROUTE_GET, &v[n]) < 0) break;
    }
    return n;
}

static void show_route_table(void) {
    struct gnet_route v[32];
    int n = get_routes(v, 32);
    printf("Kernel IP routing table\nDestination     Gateway         Genmask         Flags Metric Ref    Use Iface\n");
    for (int i = 0; i < n; i++)
        printf("%-15s %-15s %-15s %-5s %-6u %-3d %6d %s\n", nu_ip(v[i].dst), nu_ip(v[i].gw), nu_ip(v[i].mask),
               (v[i].flags & GRTF_GATEWAY) ? "UG" : "U", v[i].metric, 0, 0, v[i].ifname);
}

static int do_route_change(int add, uint32_t dst, uint32_t mask, uint32_t gw, const char *dev, uint32_t metric) {
    struct gnet_route r;
    memset(&r, 0, sizeof r);
    r.dst = dst & mask;
    r.mask = mask;
    r.gw = gw;
    r.metric = metric;
    if (dev) strlcpy(r.ifname, dev, sizeof r.ifname);
    if (gap_netctl(add ? NETCTL_ROUTE_ADD : NETCTL_ROUTE_DEL, &r) < 0) {
        kb_error("%s route: %s", add ? "adding" : "deleting", strerror(errno));
        return 1;
    }
    return 0;
}

static void show_route6_table(void) {
    struct gnet_route6 v[32];
    int n = get_routes6(v, 32);
    printf("Kernel IPv6 routing table\nDestination                    Next Hop                   Flag Met Ref Use If\n");
    for (int i = 0; i < n; i++) {
        char dst[64];
        snprintf(dst, sizeof dst, "%s/%u", nu_ip6(v[i].dst), v[i].plen);
        printf("%-30s %-26s %-4s %-3u %-3d %5d %s\n", dst, nu_ip6(v[i].gw), (v[i].flags & G6R_GATEWAY) ? "UG" : "U", v[i].metric, 0, 0, v[i].ifname);
    }
}

/* An IPv6 route added or removed. gw may be NULL (an on-link route); dev may be NULL when the gateway tells the card. */
static int route6_change(int add, const uint8_t dst[16], int plen, const uint8_t *gw, const char *dev, uint32_t metric) {
    struct gnet_route6 r;
    memset(&r, 0, sizeof r);
    memcpy(r.dst, dst, 16);
    if (gw) memcpy(r.gw, gw, 16);
    r.plen = (uint8_t)plen;
    r.metric = metric;
    if (dev) strlcpy(r.ifname, dev, sizeof r.ifname);
    if (gap_netctl(add ? NETCTL_ROUTE6_ADD : NETCTL_ROUTE6_DEL, &r) < 0) {
        kb_error("%s route: %s", add ? "adding" : "deleting", strerror(errno));
        return 1;
    }
    return 0;
}

/* "route -A inet6 add|del TARGET[/LEN]|default [gw GATEWAY] [dev IFACE] [metric N]" */
static int route6_cmd(int argc, char **argv, int i) {
    if (i >= argc) { show_route6_table(); return 0; }
    int add = !strcmp(argv[i], "add");
    if (!add && strcmp(argv[i], "del") && strcmp(argv[i], "delete")) { kb_error("usage: route -A inet6 [-n] | route -A inet6 add|del TARGET[/LEN]|default [gw GATEWAY] [metric N] [dev IFACE]"); return 2; }
    i++;
    uint8_t dst[16] = { 0 }, gw[16] = { 0 };
    int plen = 128, have = 0, have_gw = 0;
    uint32_t metric = 0;
    const char *dev = NULL;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-net") || !strcmp(a, "-host")) continue;
        if (!strcmp(a, "default")) { memset(dst, 0, 16); plen = 0; have = 1; }
        else if (!strcmp(a, "gw") && i + 1 < argc) {
            if (!nu_parse_cidr6(argv[++i], gw, NULL)) { kb_error("bad gateway '%s'", argv[i]); return 1; }
            have_gw = 1;
        } else if (!strcmp(a, "metric") && i + 1 < argc) metric = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "dev") && i + 1 < argc) dev = argv[++i];
        else if (!have) {
            int prefix = -1;
            if (!nu_parse_cidr6(a, dst, &prefix)) { kb_error("bad target '%s'", a); return 2; }
            plen = prefix < 0 ? 128 : prefix;
            have = 1;
        } else { kb_error("unknown argument '%s'", a); return 2; }
    }
    if (!have) { kb_error("missing target"); return 2; }
    return route6_change(add, dst, plen, have_gw ? gw : NULL, dev, metric);
}

APPLET(route) {
    int i = 1, v6 = 0;
    while (i < argc) {                                                        /* -n, -4, -6 and -A inet|inet6 */
        if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-4")) i++;
        else if (!strcmp(argv[i], "-6")) { v6 = 1; i++; }
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) { v6 = !strcmp(argv[i + 1], "inet6"); i += 2; }
        else break;
    }
    if (v6) return route6_cmd(argc, argv, i);
    if (i >= argc) { show_route_table(); return 0; }
    int add = !strcmp(argv[i], "add");
    if (!add && strcmp(argv[i], "del") && strcmp(argv[i], "delete")) { kb_error("usage: route [-n] | route add|del [-net|-host] TARGET [netmask MASK] [gw GATEWAY] [metric N] [dev IFACE]"); return 2; }
    i++;
    uint32_t dst = 0, mask = 0xFFFFFFFFu, gw = 0, metric = 0;
    const char *dev = NULL;
    int have_target = 0, host = 0, have_mask = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-net")) continue;
        if (!strcmp(a, "-host")) { host = 1; continue; }
        if (!strcmp(a, "default")) { dst = 0; mask = 0; have_target = 1; have_mask = 1; continue; }
        if (!strcmp(a, "netmask") && i + 1 < argc) {
            struct in_addr m;
            if (!inet_aton(argv[++i], &m)) { kb_error("bad netmask"); return 1; }
            mask = ntohl(m.s_addr);
            have_mask = 1;
        } else if (!strcmp(a, "gw") && i + 1 < argc) {
            if (!nu_resolve(argv[++i], &gw)) return 1;
        } else if (!strcmp(a, "metric") && i + 1 < argc) {
            metric = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(a, "dev") && i + 1 < argc) {
            dev = argv[++i];
        } else if (!have_target) {
            int prefix = -1;
            if (!nu_parse_cidr(a, &dst, &prefix)) { if (!nu_resolve(a, &dst)) return 1; }
            if (prefix >= 0) { mask = nu_prefix_mask(prefix); have_mask = 1; }
            have_target = 1;
        } else {
            kb_error("unknown argument '%s'", a);
            return 2;
        }
    }
    if (!have_target) { kb_error("missing target"); return 2; }
    if (!have_mask && !host) mask = (dst & 0xFF) ? 0xFFFFFFFFu : (dst & 0xFFFF00) ? 0xFFFFFF00u : (dst & 0xFF0000) ? 0xFFFF0000u : 0xFF000000u;
    return do_route_change(add, dst, mask, gw, dev, metric);
}

/* ---------------------------------------------------------------- ip */

static void ip_link_line(const struct gnet_if *r, int stats) {
    char fl[64];
    fl[0] = 0;
    if (r->flags & GIFF_LOOPBACK) strlcat(fl, "LOOPBACK,", sizeof fl);
    if (r->flags & GIFF_BROADCAST) strlcat(fl, "BROADCAST,", sizeof fl);
    if (r->flags & GIFF_NOARP) strlcat(fl, "NOARP,", sizeof fl);
    if (r->flags & GIFF_UP) strlcat(fl, "UP,", sizeof fl);
    if (r->flags & GIFF_RUNNING) strlcat(fl, "LOWER_UP,", sizeof fl);
    size_t l = strlen(fl);
    if (l) fl[l - 1] = 0;
    printf("%u: %s: <%s> mtu %u state %s\n", r->index + 1, r->name, fl, r->mtu,
           (r->flags & GIFF_LOOPBACK) ? "UNKNOWN" : (r->flags & GIFF_UP) ? ((r->flags & GIFF_RUNNING) ? "UP" : "DOWN") : "DOWN");
    if (r->flags & GIFF_LOOPBACK) printf("    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
    else printf("    link/ether %s brd ff:ff:ff:ff:ff:ff\n", nu_mac(r->mac));
    if (stats) {
        printf("    RX: bytes %llu packets %llu errors %llu dropped %llu\n", (unsigned long long)r->rx_bytes, (unsigned long long)r->rx_packets,
               (unsigned long long)r->rx_errors, (unsigned long long)r->rx_dropped);
        printf("    TX: bytes %llu packets %llu errors %llu dropped %llu\n", (unsigned long long)r->tx_bytes, (unsigned long long)r->tx_packets,
               (unsigned long long)r->tx_errors, (unsigned long long)r->tx_dropped);
    }
}

static int g_fam;                                           /* ip -4 (4) and ip -6 (6): one family only; 0: both where Linux shows both */

static void ip_addr_line(const struct gnet_if *r) {
    if (!r->addr || g_fam == 6) return;
    int len = nu_mask_len(r->mask);
    printf("    inet %s/%d", nu_ip(r->addr), len < 0 ? 32 : len);
    if (r->bcast && !(r->flags & GIFF_LOOPBACK)) printf(" brd %s", nu_ip(r->bcast));
    printf(" scope %s %s\n", (r->flags & GIFF_LOOPBACK) ? "host" : "global", r->name);
}

/* The IPv6 addresses of a card in iproute2's form: "inet6 ADDR/LEN scope S [tentative] [dadfailed] [deprecated] [dynamic]" and a line with the lifetimes. */
static void ip_addr6_lines(const struct gnet_if *r, const struct gnet_addr6 *a, int n) {
    if (g_fam == 4) return;
    for (int k = 0; k < n; k++) {
        if (strcmp(a[k].ifname, r->name)) continue;
        printf("    inet6 %s/%u scope %s", nu_ip6(a[k].addr), a[k].plen, nu_scope6(a[k].scope));
        if (a[k].flags & G6A_TENTATIVE) printf(" tentative");
        if (a[k].flags & G6A_DADFAILED) printf(" dadfailed");
        if (a[k].flags & G6A_DEPRECATED) printf(" deprecated");
        if (!(a[k].flags & G6A_PERMANENT)) printf(" dynamic");
        printf("\n       valid_lft ");
        if (a[k].valid == 0xFFFFFFFFu) printf("forever"); else printf("%usec", a[k].valid);
        printf(" preferred_lft ");
        if (a[k].pref == 0xFFFFFFFFu) printf("forever\n"); else printf("%usec\n", a[k].pref);
    }
}

static const char *ip_dev_arg(int argc, char **argv, int from) {
    for (int i = from; i < argc; i++) if (!strcmp(argv[i], "dev") && i + 1 < argc) return argv[i + 1];
    return NULL;
}

static int ip_do_addr(int argc, char **argv, int i, int stats) {
    struct gnet_if v[MAXIF];
    struct gnet_addr6 a6[MAXA6];
    int n = get_ifs(v, MAXIF);
    int na6 = get_addrs6(a6, MAXA6);
    const char *sub = i < argc ? argv[i] : "show";
    if (!strcmp(sub, "show") || !strcmp(sub, "list") || !strcmp(sub, "ls") || !strcmp(sub, "sh")) {
        const char *dev = ip_dev_arg(argc, argv, i + 1);
        if (!dev && i + 1 < argc) dev = argv[i + 1];
        if (dev && !find_if(v, n, dev)) { kb_error("Device \"%s\" does not exist.", dev); return 1; }
        for (int k = 0; k < n; k++) {
            if (dev && strcmp(dev, v[k].name)) continue;
            ip_link_line(&v[k], stats);
            ip_addr_line(&v[k]);
            ip_addr6_lines(&v[k], a6, na6);
        }
        return 0;
    }
    int add = !strcmp(sub, "add"), del = !strcmp(sub, "del") || !strcmp(sub, "delete"), flush = !strcmp(sub, "flush");
    if (!add && !del && !flush) { kb_error("usage: ip addr [show [dev IFACE]] | add ADDR/LEN dev IFACE | del ADDR/LEN dev IFACE | flush dev IFACE"); return 2; }
    const char *dev = ip_dev_arg(argc, argv, i + 1);
    if (!dev) { kb_error("missing \"dev\""); return 2; }
    struct gnet_if *cur = find_if(v, n, dev);
    if (!cur) { kb_error("Cannot find device \"%s\"", dev); return 1; }
    if (flush && g_fam == 6) {                                              /* ip -6 addr flush dev IFACE: every IPv6 address of the card */
        int rc = 0;
        for (int k = 0; k < na6; k++) if (!strcmp(a6[k].ifname, dev)) rc |= addr6_change(0, dev, a6[k].addr, a6[k].plen);
        return rc;
    }
    if (flush) return config_if(dev, 0, 0, GIFF_CLEARADDR, 0);
    uint8_t a6addr[16];
    int p6 = -1;
    if (i + 1 < argc && nu_parse_cidr6(argv[i + 1], a6addr, &p6)) return addr6_change(add, dev, a6addr, p6);       /* an IPv6 address */
    uint32_t addr;
    int prefix = -1;
    if (i + 1 >= argc || !nu_parse_cidr(argv[i + 1], &addr, &prefix)) { kb_error("bad address"); return 2; }
    if (add) return config_if(dev, addr, nu_prefix_mask(prefix < 0 ? 32 : prefix), GIFF_UP, 0);
    if (cur->addr != addr) { kb_error("Cannot assign requested address"); return 1; }
    return config_if(dev, 0, 0, GIFF_CLEARADDR, 0);
}

static int ip_do_link(int argc, char **argv, int i, int stats) {
    struct gnet_if v[MAXIF];
    int n = get_ifs(v, MAXIF);
    const char *sub = i < argc ? argv[i] : "show";
    if (!strcmp(sub, "show") || !strcmp(sub, "list") || !strcmp(sub, "ls")) {
        const char *dev = ip_dev_arg(argc, argv, i + 1);
        for (int k = 0; k < n; k++) if (!dev || !strcmp(dev, v[k].name)) ip_link_line(&v[k], stats);
        return 0;
    }
    if (strcmp(sub, "set")) { kb_error("usage: ip link [show [dev IFACE]] | set [dev] IFACE [up|down] [mtu N]"); return 2; }
    const char *dev = NULL;
    uint32_t flags = 0, mtu = 0;
    for (int k = i + 1; k < argc; k++) {
        if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k];
        else if (!strcmp(argv[k], "up")) flags |= GIFF_UP;
        else if (!strcmp(argv[k], "down")) flags |= GIFF_DOWN;
        else if (!strcmp(argv[k], "mtu") && k + 1 < argc) mtu = (uint32_t)atoi(argv[++k]);
        else if (!dev) dev = argv[k];
        else { kb_error("unknown argument '%s'", argv[k]); return 2; }
    }
    if (!dev) { kb_error("missing device"); return 2; }
    return config_if(dev, 0, 0, flags, mtu);
}

static void print_ip_route(const struct gnet_route *r, const struct gnet_if *ifs, int nif) {
    if (r->dst == 0 && r->mask == 0) printf("default via %s dev %s\n", nu_ip(r->gw), r->ifname);
    else {
        int len = nu_mask_len(r->mask);
        if (len < 0) len = 32;
        if (r->gw) printf("%s/%d via %s dev %s\n", nu_ip(r->dst), len, nu_ip(r->gw), r->ifname);
        else {
            printf("%s/%d dev %s scope link", nu_ip(r->dst), len, r->ifname);
            for (int k = 0; k < nif; k++) if (!strcmp(ifs[k].name, r->ifname) && ifs[k].addr) printf("  src %s", nu_ip(ifs[k].addr));
            printf("\n");
        }
    }
}

/* ip -6 route: "default via GW dev IF proto ra metric 1024 expires 1796sec pref medium", "fe80::/64 dev eth0 proto kernel metric 256 pref medium" */
static void print_ip_route6(const struct gnet_route6 *r) {
    static const char *protos[] = { "", "", "kernel", "boot", "", "", "", "", "", "ra" };
    const char *proto = r->proto < 10 && protos[r->proto][0] ? protos[r->proto] : "boot";
    if (r->plen == 0) printf("default via %s dev %s", nu_ip6(r->gw), r->ifname);
    else {
        printf("%s", nu_ip6(r->dst));
        if (r->plen != 128) printf("/%u", r->plen);
        if (r->flags & G6R_GATEWAY) printf(" via %s", nu_ip6(r->gw));
        printf(" dev %s", r->ifname);
    }
    printf(" proto %s metric %u", proto, r->metric);
    if (r->expires) printf(" expires %usec", r->expires);
    printf(" pref medium\n");
}

static int ip_do_route6(int argc, char **argv, int i) {
    const char *sub = i < argc ? argv[i] : "show";
    if (!strcmp(sub, "show") || !strcmp(sub, "list") || !strcmp(sub, "ls")) {
        struct gnet_route6 v[32];
        int n = get_routes6(v, 32);
        for (int k = 0; k < n; k++) print_ip_route6(&v[k]);
        return 0;
    }
    int add = !strcmp(sub, "add") || !strcmp(sub, "replace") || !strcmp(sub, "append");
    int del = !strcmp(sub, "del") || !strcmp(sub, "delete");
    if (!add && !del) { kb_error("usage: ip -6 route [show] | add|del TARGET[/LEN]|default [via GATEWAY] [dev IFACE] [metric N]"); return 2; }
    uint8_t dst[16] = { 0 }, gw[16] = { 0 };
    int plen = 128, have = 0, have_gw = 0;
    uint32_t metric = 0;
    const char *dev = NULL;
    for (int k = i + 1; k < argc; k++) {
        if (!strcmp(argv[k], "via") && k + 1 < argc) {
            if (!nu_parse_cidr6(argv[++k], gw, NULL)) { kb_error("bad gateway '%s'", argv[k]); return 1; }
            have_gw = 1;
        } else if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k];
        else if (!strcmp(argv[k], "metric") && k + 1 < argc) metric = (uint32_t)atoi(argv[++k]);
        else if (!strcmp(argv[k], "scope") || !strcmp(argv[k], "proto") || !strcmp(argv[k], "src") || !strcmp(argv[k], "pref")) k++;
        else if (!have) {
            if (!strcmp(argv[k], "default")) { memset(dst, 0, 16); plen = 0; }
            else {
                int prefix = -1;
                if (!nu_parse_cidr6(argv[k], dst, &prefix)) { kb_error("bad target '%s'", argv[k]); return 2; }
                plen = prefix < 0 ? 128 : prefix;
            }
            have = 1;
        } else { kb_error("unknown argument '%s'", argv[k]); return 2; }
    }
    if (!have) { kb_error("missing target"); return 2; }
    return route6_change(add, dst, plen, have_gw ? gw : NULL, dev, metric);
}

static int ip_do_route(int argc, char **argv, int i) {
    const char *sub = i < argc ? argv[i] : "show";
    int target6 = 0;                                                         /* "ip route add fec0::/64 ..." names an IPv6 target even without -6 */
    for (int k = i + 1; k < argc; k++) if (strchr(argv[k], ':')) target6 = 1;
    if (g_fam == 6 || (target6 && g_fam != 4)) return ip_do_route6(argc, argv, i);
    if (!strcmp(sub, "show") || !strcmp(sub, "list") || !strcmp(sub, "ls")) {
        struct gnet_route v[32];
        struct gnet_if ifs[MAXIF];
        int n = get_routes(v, 32), nif = get_ifs(ifs, MAXIF);
        for (int k = 0; k < n; k++) print_ip_route(&v[k], ifs, nif);
        return 0;
    }
    int add = !strcmp(sub, "add") || !strcmp(sub, "replace") || !strcmp(sub, "append");
    int del = !strcmp(sub, "del") || !strcmp(sub, "delete");
    if (!add && !del) { kb_error("usage: ip route [show] | add|del TARGET[/LEN]|default [via GATEWAY] [dev IFACE] [metric N]"); return 2; }
    uint32_t dst = 0, mask = 0xFFFFFFFFu, gw = 0, metric = 0;
    const char *dev = NULL;
    int have = 0;
    for (int k = i + 1; k < argc; k++) {
        if (!strcmp(argv[k], "via") && k + 1 < argc) { if (!nu_resolve(argv[++k], &gw)) return 1; }
        else if (!strcmp(argv[k], "dev") && k + 1 < argc) dev = argv[++k];
        else if (!strcmp(argv[k], "metric") && k + 1 < argc) metric = (uint32_t)atoi(argv[++k]);
        else if (!strcmp(argv[k], "scope") || !strcmp(argv[k], "proto") || !strcmp(argv[k], "src")) k++;
        else if (!have) {
            if (!strcmp(argv[k], "default")) { dst = 0; mask = 0; }
            else {
                int prefix = -1;
                if (!nu_parse_cidr(argv[k], &dst, &prefix)) { kb_error("bad target '%s'", argv[k]); return 2; }
                mask = nu_prefix_mask(prefix < 0 ? 32 : prefix);
            }
            have = 1;
        } else { kb_error("unknown argument '%s'", argv[k]); return 2; }
    }
    if (!have) { kb_error("missing target"); return 2; }
    return do_route_change(add, dst, mask, gw, dev, metric);
}

static const char *nud6_name(unsigned s) {
    switch (s) {
    case G6N_INCOMPLETE: return "INCOMPLETE";
    case G6N_REACHABLE: return "REACHABLE";
    case G6N_STALE: return "STALE";
    case G6N_DELAY: return "DELAY";
    case G6N_PROBE: return "PROBE";
    default: return "PERMANENT";
    }
}

/* ip neigh [-4|-6] [show] | add|del ADDR [lladdr MAC] dev IFACE (an IPv6 address on the line, or -6, means the IPv6 table) */
static int ip_do_neigh(int argc, char **argv, int i) {
    const char *sub = i < argc ? argv[i] : "show";
    if (!strcmp(sub, "add") || !strcmp(sub, "del") || !strcmp(sub, "delete") || !strcmp(sub, "replace")) {
        int add = strcmp(sub, "del") && strcmp(sub, "delete");
        uint8_t a[16];
        const char *dev = ip_dev_arg(argc, argv, i + 1), *mac = NULL;
        for (int k = i + 1; k + 1 < argc; k++) if (!strcmp(argv[k], "lladdr")) mac = argv[k + 1];
        if (i + 1 >= argc || !nu_parse_cidr6(argv[i + 1], a, NULL)) { kb_error("usage: ip -6 neigh add|del ADDR [lladdr MAC] dev IFACE"); return 2; }
        if (!dev) { kb_error("missing \"dev\""); return 2; }
        struct gnet_nbr6 r;
        memset(&r, 0, sizeof r);
        strlcpy(r.ifname, dev, sizeof r.ifname);
        memcpy(r.addr, a, 16);
        if (add) {
            unsigned b[6];
            char junk;
            if (!mac || sscanf(mac, "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &junk) != 6) { kb_error("an added neighbour needs a lladdr"); return 2; }
            for (int k = 0; k < 6; k++) r.mac[k] = (uint8_t)b[k];
        }
        if (gap_netctl(add ? NETCTL_NBR6_SET : NETCTL_NBR6_DEL, &r) < 0) { kb_error("neighbour: %s", strerror(errno)); return 1; }
        return 0;
    }
    if (g_fam != 6) {
        for (int k = 0;; k++) {
            struct gnet_arp a;
            memset(&a, 0, sizeof a);
            a.index = (uint32_t)k;
            if (gap_netctl(NETCTL_ARP_GET, &a) < 0) break;
            printf("%s dev %s", nu_ip(a.ip), a.ifname[0] ? a.ifname : "?");
            if (a.state != 0) printf(" lladdr %s", nu_mac(a.mac));
            printf(" %s\n", a.state == 0 ? "INCOMPLETE" : a.state == 2 ? "PERMANENT" : "REACHABLE");
        }
    }
    if (g_fam != 4) {
        struct gnet_nbr6 v[64];
        int n = get_nbrs6(v, 64);
        for (int k = 0; k < n; k++) {
            printf("%s dev %s", nu_ip6(v[k].addr), v[k].ifname);
            if (v[k].state != G6N_INCOMPLETE) printf(" lladdr %s", nu_mac(v[k].mac));
            printf("%s %s\n", v[k].router ? " router" : "", nud6_name(v[k].state));
        }
    }
    return 0;
}

APPLET(ip) {
    int i = 1, stats = 0;
    g_fam = 0;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-stats")) stats = 1;
        else if (!strcmp(argv[i], "-4")) g_fam = 4;
        else if (!strcmp(argv[i], "-6")) g_fam = 6;
        i++;                                                 /* -o, -br and friends are accepted and ignored */
    }
    if (i >= argc) { kb_error("usage: ip [-4|-6] [-s] addr | link | route | neigh ..."); return 2; }
    const char *obj = argv[i++];
    if (!strcmp(obj, "addr") || !strcmp(obj, "address") || !strcmp(obj, "a")) return ip_do_addr(argc, argv, i, stats);
    if (!strcmp(obj, "link") || !strcmp(obj, "l")) return ip_do_link(argc, argv, i, stats);
    if (!strcmp(obj, "route") || !strcmp(obj, "r")) return ip_do_route(argc, argv, i);
    if (!strcmp(obj, "neigh") || !strcmp(obj, "neighbour") || !strcmp(obj, "neighbor") || !strcmp(obj, "n")) return ip_do_neigh(argc, argv, i);
    kb_error("unknown object '%s' (try addr, link, route or neigh)", obj);
    return 2;
}
