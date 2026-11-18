/*
 * Networking ABI shared by the kernel and libgap: constants and structures of the socket calls and of the netctl call.
 * Numbers follow Linux where that costs nothing, so existing C code ports without surprises. Included from abi.h.
 */
#ifndef SHARED_NETABI_H
#define SHARED_NETABI_H

#include <stdint.h>

#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & 0170000) == 0140000)
#endif

/* ---- errno (the rest of the table is in abi.h) ---- */
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EOPNOTSUPP      95
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ENOBUFS         105
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#define EWOULDBLOCK     EAGAIN

/* ---- address families, socket types, protocols ---- */
#define AF_UNSPEC       0
#define AF_INET         2
#define AF_INET6        10
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7F000001u
#define INADDR_BROADCAST 0xFFFFFFFFu

/* ---- socket options ---- */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30
#define IP_TOS          1
#define IP_TTL          2
#define TCP_NODELAY     1
#define TCP_MAXSEG      2
/* level IPPROTO_IPV6 (the numbers of Linux's <linux/in6.h>, which glibc's netinet/in.h repeats) */
#define IPV6_CHECKSUM       7
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_MTU_DISCOVER   23
#define IPV6_MTU            24
#define IPV6_RECVERR        25
#define IPV6_V6ONLY         26
#define IPV6_RECVPKTINFO    49
#define IPV6_PKTINFO        50
#define IPV6_RECVHOPLIMIT   51
#define IPV6_HOPLIMIT       52
#define IPV6_DONTFRAG       62
#define IPV6_RECVTCLASS     66
#define IPV6_TCLASS         67

/* ---- send/recv flags ---- */
#define MSG_PEEK        0x2
#define MSG_TRUNC       0x20
#define MSG_WAITALL     0x100
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

struct gtimeval { int64_t tv_sec; int64_t tv_usec; };       /* SO_RCVTIMEO / SO_SNDTIMEO value */

/* sockaddr_in as the system calls see it (all fields network byte order except the family) */
struct gsockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t  zero[8];
};

/* sockaddr_in6 as the system calls see it: Linux's layout (28 bytes). The port and the flow label are network byte order, the scope id is host order: for a link-local
 * address it names the card (its index + 1: if_nametoindex says which; lo is 1 only when no card was registered before it), for the others it is 0 and ignored. */
struct gsockaddr_in6 {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    uint8_t  addr[16];
    uint32_t scope_id;
};

/* ---- netctl: interface, route and neighbour configuration (SYS_netctl) ---- */
#define NETCTL_IF_GET       1       /* arg: struct gnet_if with .index set (0, 1, ...) -> filled; -ENODEV past the last */
#define NETCTL_IF_CONFIG    2       /* arg: struct gnet_if (.name, .addr, .mask, .flags, .mtu): root only */
#define NETCTL_ROUTE_GET    3       /* arg: struct gnet_route with .index set -> filled; -ENOENT past the last */
#define NETCTL_ROUTE_ADD    4       /* arg: struct gnet_route: root only */
#define NETCTL_ROUTE_DEL    5       /* arg: struct gnet_route (.dst, .mask): root only */
#define NETCTL_ARP_GET      6       /* arg: struct gnet_arp with .index set -> filled; -ENOENT past the last */
#define NETCTL_ARP_SET      7       /* arg: struct gnet_arp (.ip, .mac): root only */
#define NETCTL_ARP_DEL      8       /* arg: struct gnet_arp (.ip): root only */
#define NETCTL_DHCP         9       /* arg: struct gnet_if (.name): restart DHCP on that interface; root only */
#define NETCTL_DHCP_STOP    10      /* arg: struct gnet_if (.name): stop the DHCP client on that interface (an address it set stays, as a static one); root only */
#define NETCTL_NETCONF_DONE 11      /* arg: struct gnet_if (unused): init has run netconf; the DHCP clients that waited for it (the kept network.conf has a static line) may ask now; root only */
#define NETCTL_ADDR6_GET    12      /* arg: struct gnet_addr6 with .index set (0, 1, ... over all cards) -> filled; -ENOENT past the last */
#define NETCTL_ADDR6_ADD    13      /* arg: struct gnet_addr6 (.ifname, .addr, .plen, .valid, .pref): root only; duplicate address detection runs before it is used */
#define NETCTL_ADDR6_DEL    14      /* arg: struct gnet_addr6 (.ifname, .addr): root only */
#define NETCTL_ROUTE6_GET   15      /* arg: struct gnet_route6 with .index set -> filled; -ENOENT past the last */
#define NETCTL_ROUTE6_ADD   16      /* arg: struct gnet_route6 (.dst, .plen, .gw, .ifname, .metric): root only */
#define NETCTL_ROUTE6_DEL   17      /* arg: struct gnet_route6 (.dst, .plen; .ifname and .gw narrow it down): root only */
#define NETCTL_NBR6_GET     18      /* arg: struct gnet_nbr6 with .index set -> filled; -ENOENT past the last */
#define NETCTL_NBR6_SET     19      /* arg: struct gnet_nbr6 (.ifname, .addr, .mac): a permanent entry; root only */
#define NETCTL_NBR6_DEL     20      /* arg: struct gnet_nbr6 (.ifname, .addr): root only */
#define NETCTL_DNS6_GET     21      /* arg: struct gnet_dns6 with .index set -> filled (the RDNSS servers the routers announced) */
#define NETCTL_IF6_GET      22      /* arg: struct gnet_if6 with .index set (a card's index) -> filled */

#define GIFF_UP         0x1
#define GIFF_BROADCAST  0x2
#define GIFF_DOWN       0x100       /* NETCTL_IF_CONFIG only: take the interface down */
#define GIFF_CLEARADDR  0x200       /* NETCTL_IF_CONFIG only: remove the address (and its routes) */
#define GIFF_LOOPBACK   0x8
#define GIFF_RUNNING    0x40        /* link detected */
#define GIFF_NOARP      0x80
#define GIFF_DHCP       0x10000     /* address came from DHCP */

struct gnet_if {
    uint32_t index;
    char     name[16];
    char     driver[16];
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t addr, mask, bcast;     /* host byte order */
    uint32_t flags;                 /* GIFF_* */
    uint32_t mtu;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped, tx_dropped;
};

struct gnet_route {
    uint32_t index;
    char     ifname[16];
    uint32_t dst, mask, gw;         /* host byte order */
    uint32_t metric;
    uint32_t flags;                 /* GRTF_* */
};
#define GRTF_UP        1
#define GRTF_GATEWAY   2

struct gnet_arp {
    uint32_t index;
    char     ifname[16];
    uint32_t ip;                    /* host byte order */
    uint8_t  mac[6];
    uint8_t  state;                 /* 0 incomplete, 1 reachable/complete, 2 static */
    uint8_t  pad;
};

/* IPv6. Flag and state values are Linux's (<linux/if_addr.h>, <linux/neighbour.h>, <linux/ipv6_route.h>), which is also what /proc/net/if_inet6 and ipv6_route show. */
#define G6A_TEMPORARY   0x01
#define G6A_DADFAILED   0x08
#define G6A_DEPRECATED  0x20
#define G6A_TENTATIVE   0x40
#define G6A_PERMANENT   0x80
#define G6S_GLOBAL      0x00        /* the "scope" of /proc/net/if_inet6 (IPV6_ADDR_*) */
#define G6S_LOOPBACK    0x10
#define G6S_LINKLOCAL   0x20
#define G6S_SITELOCAL   0x40

struct gnet_addr6 {
    uint32_t index;
    char     ifname[16];
    uint32_t ifindex;               /* the card's scope id (index + 1) */
    uint8_t  addr[16];
    uint8_t  plen;
    uint8_t  scope;                 /* G6S_* */
    uint8_t  flags;                 /* G6A_* */
    uint8_t  pad;
    uint32_t valid, pref;           /* seconds left; IP6_INFINITE (0xFFFFFFFF) for ever. On ADD: the lifetimes to give it (0: for ever) */
};

#define G6R_UP          0x0001      /* RTF_* */
#define G6R_GATEWAY     0x0002
#define G6R_HOST        0x0004
#define G6R_DEFAULT     0x10000
#define G6R_ADDRCONF    0x40000
#define G6R_EXPIRES     0x400000

struct gnet_route6 {
    uint32_t index;
    char     ifname[16];
    uint8_t  dst[16], gw[16];
    uint8_t  plen;
    uint8_t  proto;                 /* who made it: 2 the kernel (the prefix of an address), 3 by hand, 9 a router advertisement (Linux's RTPROT_*) */
    uint8_t  pad[2];
    uint32_t metric;
    uint32_t flags;                 /* G6R_* */
    uint32_t expires;               /* seconds left, 0 = never */
};

#define G6N_INCOMPLETE  0x01        /* NUD_* */
#define G6N_REACHABLE   0x02
#define G6N_STALE       0x04
#define G6N_DELAY       0x08
#define G6N_PROBE       0x10
#define G6N_PERMANENT   0x80

struct gnet_nbr6 {
    uint32_t index;
    char     ifname[16];
    uint8_t  addr[16];
    uint8_t  mac[6];
    uint8_t  state;                 /* G6N_* */
    uint8_t  router;                /* the neighbour said it is a router */
};

struct gnet_dns6 {
    uint32_t index;
    uint8_t  addr[16];
};

struct gnet_if6 {
    uint32_t index;                 /* the card's index (as in gnet_if) */
    uint32_t mtu;                   /* the IPv6 link MTU (0: IPv6 is off on this card) */
    uint8_t  hop_limit;
    uint8_t  disabled;              /* duplicate address detection switched IPv6 off */
    uint8_t  ra_seen, managed;
    uint8_t  other;
    uint8_t  pad[3];
};

#endif /* SHARED_NETABI_H */
