#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <stdint.h>
#include <sys/socket.h>

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr { in_addr_t s_addr; };               /* network byte order */

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;                         /* network byte order */
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

/* IPv6 (RFC 3493): the layout of Linux's, 28 bytes */
struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32

struct sockaddr_in6 {
    sa_family_t     sin6_family;                     /* AF_INET6 */
    in_port_t       sin6_port;                       /* network byte order */
    uint32_t        sin6_flowinfo;                   /* network byte order */
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;                   /* host byte order: the card of a link-local address (index + 1, as if_nametoindex says) */
};

extern const struct in6_addr in6addr_any;            /* :: */
extern const struct in6_addr in6addr_loopback;       /* ::1 */
#define IN6ADDR_ANY_INIT      { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } }

struct ipv6_mreq {                                   /* declared for programs that name it; joining a group is not supported (setsockopt answers ENOPROTOOPT) */
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#undef INADDR_ANY
#undef INADDR_LOOPBACK
#undef INADDR_BROADCAST
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)     /* host byte order, like glibc: use htonl() */
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

/* The address classes of RFC 4291 (the macros of RFC 3493 section 6.7) */
#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && ((const uint32_t *)(a))[2] == 0 && ((const uint32_t *)(a))[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && ((const uint32_t *)(a))[2] == 0 && ((const uint32_t *)(a))[3] == htonl(1))
#define IN6_IS_ADDR_MULTICAST(a)   (((const uint8_t *)(a))[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a)   ((((const uint8_t *)(a))[0] == 0xfe) && ((((const uint8_t *)(a))[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_SITELOCAL(a)   ((((const uint8_t *)(a))[0] == 0xfe) && ((((const uint8_t *)(a))[1] & 0xc0) == 0xc0))
#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && ((const uint32_t *)(a))[2] == htonl(0xffff))
#define IN6_IS_ADDR_V4COMPAT(a) \
    (((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && ((const uint32_t *)(a))[2] == 0 && ntohl(((const uint32_t *)(a))[3]) > 1)
#define IN6_IS_ADDR_MC_NODELOCAL(a)  (IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x1))
#define IN6_IS_ADDR_MC_LINKLOCAL(a)  (IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x2))
#define IN6_IS_ADDR_MC_SITELOCAL(a)  (IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x5))
#define IN6_IS_ADDR_MC_ORGLOCAL(a)   (IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x8))
#define IN6_IS_ADDR_MC_GLOBAL(a)     (IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0xe))

#endif
