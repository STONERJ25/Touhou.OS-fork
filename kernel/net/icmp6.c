/*
 * ICMPv6: the checksum, echo request and reply, the four error messages (sent with the restrictions and the rate limit of RFC 4443 section 2.4, received and handed to the
 * sockets they belong to), the path MTU from "packet too big", and the plumbing for the ICMPv6 "ping" sockets. Neighbour discovery messages are ICMPv6 too; they are handed to ndp.c.
 *
 * Written from RFC 4443 (ICMPv6) and RFC 8200 section 8.1 (the pseudo-header checksum).
 */
#include "net6k.h"
#include "netsock.h"
#include "kernel.h"
#include "mm.h"

#ifndef EPROTO
#define EPROTO 71
#endif

/* ------------------------------------------------------------------ the rate limit */

static u64 g_window, g_echo_window;
static int g_sent, g_echo_sent;

/* At most 200 error messages a second that this host sends because of what it received: nobody can use it to flood a third party (RFC 4443 section 2.4 (f), which limits the error messages). */
bool icmp6_allowed(void) {
    u64 now = net_now();
    if (now - g_window >= 1000) { g_window = now; g_sent = 0; }
    if (g_sent >= 200) { g_ip6_stats.icmp_out_ratelimit++; return false; }
    g_sent++;
    return true;
}

/* Echo replies are not errors and have a budget of their own (1000 a second): a storm of malformed packets, whose errors use up the budget above, must not make a ping go unanswered,
 * and an echo reply is no bigger than its request, so it cannot amplify anything. */
static bool echo_allowed(void) {
    u64 now = net_now();
    if (now - g_echo_window >= 1000) { g_echo_window = now; g_echo_sent = 0; }
    if (g_echo_sent >= 1000) { g_ip6_stats.icmp_out_ratelimit++; return false; }
    g_echo_sent++;
    return true;
}

/* ------------------------------------------------------------------ sending */

/* One message out of one card to one destination with no routing: the neighbour discovery messages (the source may be ::, the hop limit is 255). body is everything after the checksum. */
int icmp6_send_on(NetDev *d, const In6 *src, const In6 *dst, const u8 *dmac, u8 type, u8 code, const u8 *body, size_t blen, u8 hlim) {
    NetBuf *nb = nb_alloc();
    if (!nb || blen + 4 > nb_room(nb) - 64) { nb_free(nb); return -ENOBUFS; }
    u8 *m = nb_put(nb, 4 + blen);
    m[0] = type; m[1] = code; m[2] = m[3] = 0;
    memcpy(m + 4, body, blen);
    u16 c = ip6_l4_csum(src, dst, IP6P_ICMPV6, m, 4 + blen);
    memcpy(m + 2, &c, 2);
    g_ip6_stats.icmp_out_msgs++;
    return ip6_send_on(d, src, dst, dmac, IP6P_ICMPV6, hlim, nb);
}

/* A message through the routing table, from the source ip6_pick chooses (or src_hint). `body` follows the 4-byte header (so it starts with the 4 bytes "rest of header" or the identifier). */
static int icmp6_send_msg(const In6 *src_hint, const In6 *dst, u32 scope, u8 type, u8 code, const u8 *body, size_t blen) {
    Pick6 p;
    int rc = ip6_pick(src_hint, dst, scope, &p);
    if (rc) { g_ip6_stats.out_no_routes++; return rc; }
    size_t len = 4 + blen;
    u8 *m = kmalloc(len);
    if (!m) return -ENOMEM;
    m[0] = type; m[1] = code; m[2] = m[3] = 0;
    memcpy(m + 4, body, blen);
    u16 c = ip6_l4_csum(&p.src, dst, IP6P_ICMPV6, m, len);
    memcpy(m + 2, &c, 2);
    g_ip6_stats.icmp_out_msgs++;
    rc = ip6_send_data(&p, dst, IP6P_ICMPV6, 0, 0, true, m, len);
    kfree(m);
    return rc;
}

/* The upper-layer protocol of a quoted packet (an error message holds as much of the packet that caused it as fits): the extension headers are skipped by their length fields alone,
 * without the checks that the receive path makes (the packet may be quoted because one of them was wrong), and only the first fragment of a fragmented packet has the transport header. */
static bool quote_upper(const u8 *q, size_t qlen, u8 *proto, u32 *uoff) {
    u8 nh = q[6];
    u32 off = IP6_HLEN;
    for (int i = 0; i <= IP6_MAX_EXT; i++) {
        if (nh == IP6P_HOPOPTS || nh == IP6P_DSTOPTS || nh == IP6P_ROUTING) {
            if (off + 8 > qlen) return false;
            u32 hl = ((u32)q[off + 1] + 1) * 8;
            if (off + hl > qlen) return false;
            nh = q[off];
            off += hl;
            continue;
        }
        if (nh == IP6P_FRAGMENT) {
            if (off + 8 > qlen || (rd16(q + off + 2) & 0xFFF8u)) return false;
            nh = q[off];
            off += 8;
            continue;
        }
        *proto = nh;
        *uoff = off;
        return true;
    }
    return false;
}

void icmp6_error(u8 type, u8 code, u32 info, const u8 *pkt, size_t pktlen, NetDev *indev) {
    if (pktlen < IP6_HLEN || (pkt[0] >> 4) != 6) return;
    const In6 *osrc = (const In6 *)(pkt + 8), *odst = (const In6 *)(pkt + 24);
    if (in6_is_any(osrc) || in6_is_mcast(osrc)) return;                    /* the source must be one node to answer to */
    if (in6_is_mcast(odst) && !(type == ICMP6_PACKET_TOO_BIG || (type == ICMP6_PARAM_PROB && code == 2))) return;
    u8 oproto;
    u32 ouoff;
    if (quote_upper(pkt, pktlen, &oproto, &ouoff) && oproto == IP6P_ICMPV6 && ouoff < pktlen) {      /* never an error about an error (or a redirect) */
        u8 t = pkt[ouoff];
        if (t < 128 || t == ND_REDIRECT) return;
    }
    if (!icmp6_allowed()) return;
    size_t q = MIN(pktlen, (size_t)(IP6_MIN_MTU - IP6_HLEN - 8));          /* as much of the packet as fits in 1280 bytes */
    u8 body[4 + IP6_MIN_MTU];
    wr32(body, info);
    memcpy(body + 4, pkt, q);
    u32 scope = indev && (in6_is_linklocal(osrc) || in6_is_mcast(osrc)) ? ip6_dev_scope(indev) : 0;
    const In6 *hint = in6_is_mcast(odst) ? NULL : odst;
    if (hint && !ip6_is_ours(hint)) hint = NULL;
    switch (type) {
    case ICMP6_DST_UNREACH: g_ip6_stats.icmp_out_dest_unreachs++; break;
    case ICMP6_PACKET_TOO_BIG: g_ip6_stats.icmp_out_pkt_too_bigs++; break;
    case ICMP6_TIME_EXCEEDED: g_ip6_stats.icmp_out_time_excds++; break;
    case ICMP6_PARAM_PROB: g_ip6_stats.icmp_out_param_probs++; break;
    default: break;
    }
    icmp6_send_msg(hint, osrc, scope, type, code, body, 4 + q);
}

/* A ping socket's echo request: the identifier is the socket's, the checksum ours. msg is the whole ICMPv6 message (type 128 first). */
int icmp6_send_echo_request(const In6 *src_hint, const In6 *dst, u32 scope, const u8 *msg, size_t len, u16 ident) {
    if (len < 8 || len > IP6_MAX_PAYLOAD) return -EMSGSIZE;
    if (msg[0] != ICMP6_ECHO_REQUEST || msg[1] != 0) return -EINVAL;
    Pick6 p;
    int rc = ip6_pick(src_hint, dst, scope, &p);
    if (rc) { g_ip6_stats.out_no_routes++; return rc; }
    u8 *m = kmalloc(len);
    if (!m) return -ENOMEM;
    memcpy(m, msg, len);
    wr16(m + 4, ident);
    m[2] = m[3] = 0;
    u16 c = ip6_l4_csum(&p.src, dst, IP6P_ICMPV6, m, len);
    memcpy(m + 2, &c, 2);
    g_ip6_stats.icmp_out_msgs++;
    rc = ip6_send_data(&p, dst, IP6P_ICMPV6, 0, 0, true, m, len);
    kfree(m);
    return rc;
}

/* ------------------------------------------------------------------ receiving */

static int icmp6_errno(u8 type, u8 code) {
    switch (type) {
    case ICMP6_DST_UNREACH:
        switch (code) {
        case 0: case 2: return ENETUNREACH;
        case 1: case 5: case 6: return EACCES;
        case 3: return EHOSTUNREACH;
        case 4: return ECONNREFUSED;
        default: return EHOSTUNREACH;
        }
    case ICMP6_PACKET_TOO_BIG: return EMSGSIZE;
    case ICMP6_TIME_EXCEEDED: return EHOSTUNREACH;
    case ICMP6_PARAM_PROB: return EPROTO;
    default: return 0;
    }
}

/* An error message about a packet we sent: the quoted packet says which socket. */
static void icmp6_got_error(NetDev *d, const In6 *esrc, const u8 *m, size_t len) {
    u8 type = m[0], code = m[1];
    u32 info = rd32(m + 4);
    const u8 *q = m + 8;
    size_t qlen = len - 8;
    if (qlen < IP6_HLEN || (q[0] >> 4) != 6) return;
    const In6 *qsrc = (const In6 *)(q + 8), *qdst = (const In6 *)(q + 24);
    if (!ip6_is_ours(qsrc)) return;                                         /* a packet that never left from here */
    int err = icmp6_errno(type, code);
    if (type == ICMP6_PACKET_TOO_BIG) {                                     /* the path to qdst is narrower than we thought (only ever lowered, never below 1280) */
        pmtu6_set(qdst, info);
        tcp6_pmtu(qdst, info < IP6_MIN_MTU ? IP6_MIN_MTU : info);
    }
    u8 proto;
    u32 uoff;
    if (!quote_upper(q, qlen, &proto, &uoff) || qlen < uoff + 4) return;
    u16 sport = rd16(q + uoff), dport = rd16(q + uoff + 2);
    if (proto == IP6P_UDP && err) udp6_error(qsrc, sport, qdst, dport, err);
    else if (proto == IP6P_TCP && err) tcp6_error(qsrc, sport, qdst, dport, err);
    else if (proto == IP6P_ICMPV6 && qlen >= uoff + 8 && q[uoff] == ICMP6_ECHO_REQUEST) icmp6_sock_deliver(esrc, in6_is_linklocal(esrc) ? ip6_dev_scope(d) : 0, rd16(q + uoff + 4), m, len);
}

void icmp6_input(NetDev *d, const u8 *pkt, size_t pktlen, u32 uoff, bool to_tentative) {
    const In6 *src = (const In6 *)(pkt + 8), *dst = (const In6 *)(pkt + 24);
    const u8 *m = pkt + uoff;
    size_t len = pktlen - uoff;
    g_ip6_stats.icmp_in_msgs++;
    if (len < 4) goto bad;
    if (net_csum(m, len, net6_pseudo_csum(src, dst, IP6P_ICMPV6, (u32)len)) != 0xFFFF) goto bad;
    u8 type = m[0];
    if (to_tentative && type != ND_NEIGHBOR_SOLICIT && type != ND_NEIGHBOR_ADVERT) return;
    switch (type) {
    case ICMP6_ECHO_REQUEST: {
        g_ip6_stats.icmp_in_echos++;
        if (len < 8 || in6_is_any(src) || !echo_allowed()) return;
        u32 scope = in6_is_linklocal(src) ? ip6_dev_scope(d) : 0;
        const In6 *hint = in6_is_mcast(dst) ? NULL : dst;                    /* an echo reply to a unicast request comes from the address it was sent to (RFC 4443 section 4.2) */
        Pick6 p;
        if (ip6_pick(hint, src, scope, &p)) return;
        u8 *r = kmalloc(len);
        if (!r) return;
        memcpy(r, m, len);
        r[0] = ICMP6_ECHO_REPLY;
        r[2] = r[3] = 0;
        u16 c = ip6_l4_csum(&p.src, src, IP6P_ICMPV6, r, len);
        memcpy(r + 2, &c, 2);
        g_ip6_stats.icmp_out_msgs++;
        g_ip6_stats.icmp_out_echo_replies++;
        ip6_send_data(&p, src, IP6P_ICMPV6, 0, 0, true, r, len);
        kfree(r);
        return;
    }
    case ICMP6_ECHO_REPLY:
        g_ip6_stats.icmp_in_echo_replies++;
        if (len >= 8) icmp6_sock_deliver(src, in6_is_linklocal(src) ? ip6_dev_scope(d) : 0, rd16(m + 4), m, len);
        return;
    case ICMP6_DST_UNREACH: g_ip6_stats.icmp_in_dest_unreachs++; goto error;
    case ICMP6_PACKET_TOO_BIG: g_ip6_stats.icmp_in_pkt_too_bigs++; goto error;
    case ICMP6_TIME_EXCEEDED: g_ip6_stats.icmp_in_time_excds++; goto error;
    case ICMP6_PARAM_PROB: g_ip6_stats.icmp_in_param_probs++; goto error;
    case ND_ROUTER_SOLICIT: case ND_ROUTER_ADVERT: case ND_NEIGHBOR_SOLICIT: case ND_NEIGHBOR_ADVERT: case ND_REDIRECT:
        nd6_input(d, pkt, pktlen, uoff, to_tentative);
        return;
    default:                                                                 /* MLD queries and the like: nobody here sends reports, so there is nothing to answer */
        return;
    }
error:
    if (len >= 8 + IP6_HLEN && !in6_is_any(src)) icmp6_got_error(d, src, m, len);
    return;
bad:
    g_ip6_stats.icmp_in_errors++;
    g_ip6_stats.bad++;
}
