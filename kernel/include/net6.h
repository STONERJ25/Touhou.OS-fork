/*
 * IPv6, the half that needs no machine (kernel/net/ip6_proto.c; docs/NETWORK.md, "IPv6"): addresses and their text and prefix arithmetic, the walker over the
 * extension headers, the parsers of the options of the extension headers and of neighbour discovery, the pseudo-header checksum, and the queue that reassembles a
 * fragmented datagram. This header includes nothing but types.h, so tests/host/test_net6.py compiles the C file with gcc and checks it against Python and the C library;
 * the stateful half (cards, neighbours, routes, sockets) is kernel/include/net6k.h.
 *
 * Written from RFC 8200 (the header, the extension headers, fragments), RFC 4291 (the address architecture and its text forms), RFC 5952 (the text form on output),
 * RFC 4443 (ICMPv6), RFC 4861 (neighbour discovery options), RFC 8106 (RDNSS), RFC 5095 (routing header type 0) and RFC 5722 (overlapping fragments).
 */
#ifndef K_NET6_H
#define K_NET6_H

#include "types.h"

/* ---- protocol numbers, sizes ---- */
#define IP6_HLEN          40
#define IP6_MIN_MTU       1280
#define IP6_MAX_PAYLOAD   65535
#define ETH_P_IPV6        0x86DD

#define IP6P_HOPOPTS      0
#define IP6P_TCP          6
#define IP6P_UDP          17
#define IP6P_ROUTING      43
#define IP6P_FRAGMENT     44
#define IP6P_ESP          50
#define IP6P_AH           51
#define IP6P_ICMPV6       58
#define IP6P_NONE         59
#define IP6P_DSTOPTS      60

#define ICMP6_DST_UNREACH     1
#define ICMP6_PACKET_TOO_BIG  2
#define ICMP6_TIME_EXCEEDED   3
#define ICMP6_PARAM_PROB      4
#define ICMP6_ECHO_REQUEST    128
#define ICMP6_ECHO_REPLY      129
#define ND_ROUTER_SOLICIT     133
#define ND_ROUTER_ADVERT      134
#define ND_NEIGHBOR_SOLICIT   135
#define ND_NEIGHBOR_ADVERT    136
#define ND_REDIRECT           137

/* ---- addresses ---- */
typedef struct In6 { u8 b[16]; } In6;

static inline bool in6_eq(const In6 *a, const In6 *b) { return __builtin_memcmp(a->b, b->b, 16) == 0; }
static inline void in6_zero(In6 *a) { __builtin_memset(a->b, 0, 16); }
static inline bool in6_is_any(const In6 *a) { for (int i = 0; i < 16; i++) if (a->b[i]) return false; return true; }
static inline bool in6_is_loopback(const In6 *a) { for (int i = 0; i < 15; i++) if (a->b[i]) return false; return a->b[15] == 1; }
static inline bool in6_is_mcast(const In6 *a) { return a->b[0] == 0xFF; }
static inline bool in6_is_linklocal(const In6 *a) { return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80; }
static inline bool in6_is_v4mapped(const In6 *a) { for (int i = 0; i < 10; i++) if (a->b[i]) return false; return a->b[10] == 0xFF && a->b[11] == 0xFF; }
static inline u32 in6_v4(const In6 *a) { return (u32)a->b[12] << 24 | (u32)a->b[13] << 16 | (u32)a->b[14] << 8 | a->b[15]; }
static inline void in6_set_v4mapped(In6 *a, u32 v4) {
    __builtin_memset(a->b, 0, 10);
    a->b[10] = a->b[11] = 0xFF;
    a->b[12] = (u8)(v4 >> 24); a->b[13] = (u8)(v4 >> 16); a->b[14] = (u8)(v4 >> 8); a->b[15] = (u8)v4;
}
static inline u16 in6_word(const In6 *a, int i) { return (u16)(a->b[2 * i] << 8 | a->b[2 * i + 1]); }

/* The scope numbers of RFC 4291 section 2.7 (the same numbering for unicast addresses: loopback 1, link-local 2, site-local 5, everything else 14). */
#define IP6_SCOPE_HOST    1
#define IP6_SCOPE_LINK    2
#define IP6_SCOPE_SITE    5
#define IP6_SCOPE_GLOBAL  14
int  in6_scope(const In6 *a);
bool in6_prefix_eq(const In6 *a, const In6 *b, int plen);          /* the first plen bits are the same */
int  in6_common_prefix(const In6 *a, const In6 *b);                /* how many leading bits are the same (0 to 128) */
void in6_mask(In6 *out, const In6 *a, int plen);                   /* a with every bit after the first plen cleared */
void in6_solicited_node(In6 *out, const In6 *a);                   /* ff02::1:ffXX:XXXX of the last 24 bits */
bool in6_is_solicited_node(const In6 *a);                          /* ff02::1:ff00:0/104 */
void in6_mcast_mac(u8 mac[6], const In6 *a);                       /* 33:33 and the last four bytes (RFC 2464 section 7) */
void in6_eui64(In6 *out, const In6 *prefix, const u8 mac[6]);      /* the first 8 bytes of prefix, then the modified EUI-64 of the MAC address (RFC 4291 appendix A) */
size_t in6_str(const In6 *a, char *buf, size_t cap);               /* the text form of RFC 5952 ("::1", "fe80::5054:ff:fe12:3456", "::ffff:10.0.2.2"); returns the length */
void in6_hex(const In6 *a, char out[33]);                          /* 32 lower-case hex digits, the form of /proc/net/if_inet6 */

/* ---- checksums ---- */
/* The unfolded sum of the pseudo-header of RFC 8200 section 8.1, in the form net_csum() takes as its start (the sum of little-endian words of the memory image). */
u32  net6_pseudo_csum(const In6 *src, const In6 *dst, u8 proto, u32 len);

/* ---- the walker over a packet's extension headers ---- */
#define IP6_MAX_EXT 8                       /* extension headers followed in one packet (RFC 8200 does not set a limit; a bound keeps the work bounded) */

enum { IP6W_OK = 0, IP6W_DROP = 1, IP6W_ICMP = 2 };    /* a verdict: carry on, throw the packet away silently, throw it away and send a parameter problem */

typedef struct Ip6Walk {
    int  verdict;
    u8   pp_code;           /* IP6W_ICMP: 0 erroneous header field, 1 unrecognized next header type, 2 unrecognized option (ICMPv6 parameter problem codes) */
    u32  pp_ptr;            /* and the pointer: the offset in the packet of the offending byte */
    u8   proto;             /* the protocol the walk ended at: an upper layer (or IP6P_NONE), or IP6P_FRAGMENT when stopped at a fragment header */
    u32  uoff;              /* where that header starts (from the first byte of the IPv6 header); for a fragment: the first byte after the fragment header */
    u32  nh_ptr;            /* the offset of the Next Header byte that named `proto` (the pointer for "unrecognized next header" if the caller does not know the protocol) */
    int  nexthdrs;          /* extension headers passed */
    u32  frag_off;          /* IP6P_FRAGMENT: the offset of this fragment's data in the fragmentable part, in bytes */
    bool frag_more;
    u32  frag_id;
    u8   frag_nh;           /* the Next Header of the fragment header: what the fragmentable part starts with */
    u32  frag_hoff;         /* where the fragment header is */
} Ip6Walk;

/* Walks pkt[0..len) (an IPv6 packet from its first header byte; len = 40 + the payload length) from the header `nh` that starts at offset `off` (the first call: nh = pkt[6], off = 40;
 * after reassembly: the first Next Header of the fragmentable part, with the data placed after a copy of the header). `first` is true when the walk starts at the IPv6 header (the
 * hop-by-hop options header is allowed only there), `allow_frag` false for data that was reassembled (a fragment header inside it is an error). */
void ip6_walk(const u8 *pkt, size_t len, u8 nh, u32 off, bool first, bool allow_frag, bool dst_mcast, Ip6Walk *w);

/* ---- neighbour discovery options (RFC 4861 section 4.6, RFC 8106) ---- */
#define ND_OPT_SLLA     1
#define ND_OPT_TLLA     2
#define ND_OPT_PREFIX   3
#define ND_OPT_REDIRECT 4
#define ND_OPT_MTU      5
#define ND_OPT_RDNSS    25
#define ND_MAX_PREFIX   8
#define ND_MAX_RDNSS    4

typedef struct NdOpts {
    const u8 *slla, *tlla;                  /* the six address bytes, or NULL */
    const u8 *prefix[ND_MAX_PREFIX];        /* each: the option from its type byte (32 bytes) */
    int       nprefix;
    const u8 *mtu;                          /* the option from its type byte (8 bytes) */
    const u8 *rdnss[ND_MAX_RDNSS];          /* each: the option from its type byte (8 + 16 n bytes) */
    int       nrdnss;
} NdOpts;

/* Walks the options in p[0..len). False if the area is malformed (an option of length 0 or one that runs past the end): RFC 4861 section 7.1 says the whole message is discarded. */
bool nd_opts_parse(const u8 *p, size_t len, NdOpts *o);

/* ---- the options of hop-by-hop and destination options headers (RFC 8200 section 4.2) ---- */
/* Checks the options in p[0..len) (the area after the two header bytes). Returns IP6W_OK, IP6W_DROP or IP6W_ICMP with *code and *ptr (the offset in p of the option type) set. */
int ip6_opts_check(const u8 *p, size_t len, bool dst_mcast, u8 *code, u32 *ptr);

/* ---- fragment reassembly (RFC 8200 section 4.5, RFC 5722) ---- */
#define R6_MAX_FRAGS 64                     /* fragments of one datagram; the data of all of them is at most IP6_MAX_PAYLOAD bytes */

typedef struct Reasm6 {
    u8  *buf;                               /* IP6_HLEN + IP6_MAX_PAYLOAD bytes; the data goes to buf + IP6_HLEN (the caller allocates and fills the header at the end) */
    u32  total;                             /* the length of the datagram's fragmentable part once the last fragment has arrived, else 0 */
    u32  got;                               /* bytes received so far (the fragments do not overlap, so the sum is exact) */
    int  nfrag;
    u32  start[R6_MAX_FRAGS], end[R6_MAX_FRAGS];
    bool have_first;
    u8   first_nh;                          /* the fragment header's Next Header in the fragment at offset 0 */
} Reasm6;

enum { R6_MORE = 0, R6_DONE = 1, R6_DUP = 2, R6_BAD = 3 };     /* the datagram is incomplete / complete / this fragment repeats one we have (drop it only) / abandon the datagram */
int reasm6_add(Reasm6 *q, u32 off, bool more, u8 nh, const u8 *data, u32 len);

#endif
