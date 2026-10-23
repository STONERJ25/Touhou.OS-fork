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
#define AF_INET         2
#define PF_INET         AF_INET
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

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

/* ---- send/recv flags ---- */
#define MSG_PEEK        0x2
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

#define GIFF_UP         0x1
#define GIFF_BROADCAST  0x2
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

#endif /* SHARED_NETABI_H */
