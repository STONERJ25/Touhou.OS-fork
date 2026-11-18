/*
 * IPv6 in the Yukari stack, the half that has state (docs/NETWORK.md, "IPv6"): the addresses of a card, the routes, the path MTU cache, the neighbour cache, the receive
 * and send paths, ICMPv6, neighbour discovery and autoconfiguration. The pure half (addresses, header walker, reassembly queue) is net6.h. Everything here runs with the net lock held.
 */
#ifndef K_NET6K_H
#define K_NET6K_H

#include "net.h"
#include "net6.h"

/* ---- the addresses of a card ---- */
#define IP6_MAX_ADDRS    8
#define IP6_MAX_SLAAC    4                  /* addresses a router advertisement may create on one card: a hostile neighbour cannot fill the table */
#define IP6_INFINITE     0xFFFFFFFFu        /* a lifetime in seconds that never ends */

enum { A6_FREE = 0, A6_TENTATIVE, A6_PREFERRED, A6_DEPRECATED, A6_DUPLICATE };
#define A6F_PERMANENT    0x01               /* set by hand, or the loopback's: no lifetime */
#define A6F_LINKLOCAL    0x02               /* the link-local address of the card, made from its MAC address */
#define A6F_SLAAC        0x04               /* made from a prefix of a router advertisement */

typedef struct Addr6 {
    In6  addr;
    u8   plen, state, flags, dad_sent;
    u64  valid_end, pref_end;               /* net_now() deadlines; 0 = never */
    u64  dad_at;                            /* tentative: when the next probe is sent, or when the wait ends */
} Addr6;

typedef struct Ip6If {
    Addr6 a[IP6_MAX_ADDRS];
    bool  off;                              /* the link-local address was found to be a duplicate: IPv6 is switched off on this card (RFC 4862 section 5.4.5) */
    u8    hop_limit;                        /* the default hop limit of packets sent here: 64, or what a router advertisement said */
    u32   mtu;                              /* the link MTU a router advertised (1280 to the card's own); 0 = the card's */
    u32   reachable_ms, retrans_ms;         /* BaseReachableTime and RetransTimer (RFC 4861 section 6.3.2) */
    u8    rs_sent;                          /* router solicitations sent since the link came up */
    u64   rs_at;                            /* when the next one goes (0 = none planned) */
    bool  ra_seen, managed, other;          /* an advertisement came; its M and O flags (there is no DHCPv6: they are only shown) */
    bool  ll_wanted;                        /* the link-local address is to be made as soon as the carrier is there */
} Ip6If;

/* ---- routes ---- */
#define R6F_GATEWAY      0x01
#define R6F_CONNECTED    0x02               /* the prefix of an address of the card */
#define R6F_STATIC       0x04               /* ip -6 route add */
#define R6F_RA           0x08               /* learned from a router advertisement: has a lifetime */

typedef struct Route6 {
    bool    used;
    In6     dst, gw;
    u8      plen, flags;
    NetDev *dev;
    u32     metric;
    u64     expires;                        /* net_now() deadline, 0 = never */
} Route6;

typedef struct Pick6 {                      /* where a packet goes: the answer of ip6_pick */
    NetDev *dev;
    In6     nexthop, src;
    u32     mtu;
} Pick6;

/* ---- counters (/proc/net/snmp6) ---- */
typedef struct Ip6Stats {
    u64 in_receives, in_hdr_errors, in_addr_errors, in_unknown_protos, in_truncated, in_discards, in_delivers;
    u64 out_requests, out_no_routes, out_frag_ok, out_frag_creates, out_frag_fails;
    u64 reasm_reqds, reasm_oks, reasm_fails, reasm_timeouts;
    u64 icmp_in_msgs, icmp_in_errors, icmp_in_echos, icmp_in_echo_replies, icmp_in_dest_unreachs, icmp_in_pkt_too_bigs, icmp_in_time_excds, icmp_in_param_probs;
    u64 icmp_out_msgs, icmp_out_echo_replies, icmp_out_dest_unreachs, icmp_out_pkt_too_bigs, icmp_out_time_excds, icmp_out_param_probs, icmp_out_ratelimit;
    u64 nd_in_rs, nd_in_ra, nd_in_ns, nd_in_na, nd_in_redirects, nd_out_rs, nd_out_ns, nd_out_na, nd_bad;
    u64 udp_in_datagrams, udp_no_ports, udp_in_errors, udp_out_datagrams;
    u64 bad;                                /* every malformed packet thrown away anywhere in the IPv6 code: what /proc/net/tune shows as ip6_bad */
} Ip6Stats;
extern Ip6Stats g_ip6_stats;
extern int g_ip6_reasm_ms;                                                 /* the reassembly timeout, 60000 ms unless /proc/net/tune says less */

/* ---- ip6.c ---- */
void   ip6_input(NetDev *d, NetBuf *nb);
Ip6If *ip6_if(NetDev *d);                                                  /* NULL if the card has none yet */
Ip6If *ip6_if_make(NetDev *d);
Addr6 *ip6_addr_find(const In6 *a, NetDev **dev);                          /* an address of this host on any card (any state), or NULL */
Addr6 *ip6_addr_on(NetDev *d, const In6 *a);
bool   ip6_is_ours(const In6 *a);                                          /* ::1 or an address of ours that may be used (preferred or deprecated) */
int    ip6_addr_add(NetDev *d, const In6 *addr, int plen, u8 flags, u32 valid_s, u32 pref_s);   /* 0, -EEXIST, -ENOSPC...; starts DAD */
int    ip6_addr_del(NetDev *d, const In6 *addr);
bool   ip6_mcast_for_us(NetDev *d, const In6 *dst);                        /* a group this host listens to on this card */
Addr6 *ip6_src_select(NetDev *d, const In6 *dst);                          /* the address of the card to send to dst from (RFC 6724, simplified), or NULL */
int    ip6_pick(const In6 *src_hint, const In6 *dst, u32 scope, Pick6 *p); /* scope: sin6_scope_id (card index + 1) or 0 */
NetDev *ip6_scope_dev(u32 scope);                                          /* the card a scope id names, or NULL */
u32    ip6_dev_scope(NetDev *d);                                           /* the scope id of a card (index + 1) */
u32    ip6_link_mtu(NetDev *d);
u16    ip6_l4_csum(const In6 *src, const In6 *dst, u8 proto, const u8 *msg, size_t len);   /* the value to store, with a native 16-bit write */
/* Sends the finished upper-layer message in nb (checksum field at csum_off already zero; -1: none) from the address src_hint (or one that is chosen) to dst. */
int    ip6_send_csum(const In6 *src_hint, const In6 *dst, u32 scope, u8 proto, u8 hlim, u8 tclass, bool frag_ok, NetBuf *nb, int csum_off);
int    ip6_send_data(const Pick6 *p, const In6 *dst, u8 proto, u8 hlim, u8 tclass, bool frag_ok, const u8 *data, size_t len);   /* any size; fragments */
int    ip6_send_nb(const Pick6 *p, const In6 *dst, u8 proto, u8 hlim, u8 tclass, bool frag_ok, NetBuf *nb);
int    ip6_send_on(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, u8 proto, u8 hlim, NetBuf *nb);   /* one device, no routing (neighbour discovery); dmac NULL: resolve dst */
int    route6_add(const In6 *dst, int plen, const In6 *gw, NetDev *d, u32 metric, u8 flags, u64 expires);
int    route6_del(const In6 *dst, int plen, NetDev *d, const In6 *gw);
Route6 *route6_lookup(const In6 *dst);
int    route6_list(int index, Route6 **out);
void   route6_flush_dev(NetDev *d, u8 flags_mask);
void   pmtu6_set(const In6 *dst, u32 mtu);
u32    pmtu6_get(const In6 *dst);
void   ip6_tick(u64 now);
extern const u8 *g_rx6_pkt;                                                /* the IPv6 packet being delivered, for the ICMPv6 errors the transports send */
extern size_t g_rx6_len;
extern NetDev *g_rx6_dev;
void   ip6_dev_added(NetDev *d);
void   ip6_netctl_init(void);                                              /* /proc/net/{if_inet6,ipv6_route,snmp6} and the netctl commands (net6ctl.c) */
long   net6_ctl(unsigned long cmd, void *arg, bool root, bool *handled);

/* ---- icmp6.c ---- */
void   icmp6_input(NetDev *d, const u8 *pkt, size_t pktlen, u32 uoff, bool to_tentative);
/* An error about the packet pkt (IPv6 header first; as much of it as there is) that arrived on indev. Rules of RFC 4443 section 2.4 apply; rate limited. */
void   icmp6_error(u8 type, u8 code, u32 info, const u8 *pkt, size_t pktlen, NetDev *indev);
int    icmp6_send_on(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, u8 type, u8 code, const u8 *body, size_t blen, u8 hlim);
int    icmp6_send_echo_request(const In6 *src_hint, const In6 *dst, u32 scope, const u8 *msg, size_t len, u16 ident);   /* a ping socket's request */
bool   icmp6_allowed(void);                                                /* the rate limit of the messages we send unasked */

/* ---- ndp.c ---- */
void   nd6_input(NetDev *d, const u8 *pkt, size_t pktlen, u32 uoff, bool to_tentative);
int    nd6_output(NetDev *d, const In6 *nexthop, NetBuf *nb);              /* a finished IPv6 packet at nb->off, resolved and sent (or queued) */
void   nd6_tick(u64 now);
void   nd6_dev_added(NetDev *d);
void   nd6_link(NetDev *d, bool up);
void   nd6_if_down(NetDev *d);                                             /* ifconfig down: addresses stay, the learned state goes */
void   nd6_addr_added(NetDev *d, Addr6 *a);                                /* a manual address: DAD starts */
int    nbr6_list(int index, struct gnet_nbr6 *out);
int    nbr6_set(NetDev *d, const In6 *addr, const u8 *mac);
int    nbr6_del(NetDev *d, const In6 *addr);
size_t net6_dns_servers(In6 *out, size_t max);                             /* the RDNSS servers that are valid now */
void   net6_resolv_changed(void);                                          /* /etc/resolv.conf was rewritten (DHCP): the IPv6 servers go back in */
size_t nd6_proc(char *buf, size_t cap);

/* ---- sockets (sock.c) ---- */
void   udp6_input(NetDev *d, const u8 *p, size_t len, const In6 *src, const In6 *dst);
void   udp6_error(const In6 *laddr, u16 lport, const In6 *raddr, u16 rport, int err);
void   tcp6_input(NetDev *d, const u8 *p, size_t len, const In6 *src, const In6 *dst);
void   tcp6_error(const In6 *laddr, u16 lport, const In6 *raddr, u16 rport, int err);
void   tcp6_pmtu(const In6 *raddr, u32 mtu);                               /* a "packet too big" for connections to raddr */
void   icmp6_sock_deliver(const In6 *src, u32 scope, u16 ident, const u8 *msg, size_t len);   /* echo replies and the errors about echo requests (ident: the request's identifier), for the ICMPv6 sockets */
size_t net_proc_tcp6(char *buf, size_t cap);
size_t net_proc_udp6(char *buf, size_t cap);

#endif
