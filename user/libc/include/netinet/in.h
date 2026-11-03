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

#endif
