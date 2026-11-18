/*
 * IPv6, the half that needs no machine (see kernel/include/net6.h): addresses, the walker over a packet's extension headers, the option parsers, the pseudo-header
 * checksum and the reassembly queue. Nothing here allocates, reads a clock or touches a card; the kernel (ip6.c, ndp.c, icmp6.c) hands in bytes and tests/host/test_net6.py
 * does the same from Python and the C library.
 *
 * Written from RFC 8200 (the header and its extension headers; section 4.2 for the option actions, 4.4 for the routing header, 4.5 for fragments), RFC 5095 (routing
 * header type 0), RFC 5722 (a datagram with overlapping fragments is thrown away), RFC 4291 (address forms) and RFC 5952 (the text form), RFC 4861 section 4.6 (options)
 * and RFC 8106 (RDNSS).
 */
#include "net6.h"

#define CP(d, s, n)  __builtin_memcpy((d), (s), (n))
#define SET(d, c, n) __builtin_memset((d), (c), (n))

static inline u16 r16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 r32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ------------------------------------------------------------------ addresses */

int in6_scope(const In6 *a) {
    if (in6_is_mcast(a)) return a->b[1] & 0x0F;
    if (in6_is_loopback(a)) return IP6_SCOPE_HOST;
    if (in6_is_linklocal(a)) return IP6_SCOPE_LINK;
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0xC0) return IP6_SCOPE_SITE;           /* fec0::/10, deprecated by RFC 3879 but what QEMU's user network still uses */
    return IP6_SCOPE_GLOBAL;
}

bool in6_prefix_eq(const In6 *a, const In6 *b, int plen) {
    if (plen < 0) plen = 0;
    if (plen > 128) plen = 128;
    int whole = plen / 8, rest = plen % 8;
    for (int i = 0; i < whole; i++) if (a->b[i] != b->b[i]) return false;
    if (rest) {
        u8 m = (u8)(0xFF << (8 - rest));
        if ((a->b[whole] & m) != (b->b[whole] & m)) return false;
    }
    return true;
}

int in6_common_prefix(const In6 *a, const In6 *b) {
    int n = 0;
    for (int i = 0; i < 16; i++) {
        u8 x = a->b[i] ^ b->b[i];
        if (!x) { n += 8; continue; }
        while (!(x & 0x80)) { n++; x <<= 1; }
        break;
    }
    return n;
}

void in6_mask(In6 *out, const In6 *a, int plen) {
    if (plen < 0) plen = 0;
    if (plen > 128) plen = 128;
    for (int i = 0; i < 16; i++) {
        int bits = plen - 8 * i;
        u8 m = bits >= 8 ? 0xFF : bits <= 0 ? 0x00 : (u8)(0xFF << (8 - bits));
        out->b[i] = a->b[i] & m;
    }
}

void in6_solicited_node(In6 *out, const In6 *a) {
    SET(out->b, 0, 16);
    out->b[0] = 0xFF; out->b[1] = 0x02;
    out->b[11] = 0x01; out->b[12] = 0xFF;
    out->b[13] = a->b[13]; out->b[14] = a->b[14]; out->b[15] = a->b[15];
}

bool in6_is_solicited_node(const In6 *a) {
    if (a->b[0] != 0xFF || a->b[1] != 0x02) return false;
    for (int i = 2; i < 11; i++) if (a->b[i]) return false;
    return a->b[11] == 0x01 && a->b[12] == 0xFF;
}

void in6_mcast_mac(u8 mac[6], const In6 *a) {
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = a->b[12]; mac[3] = a->b[13]; mac[4] = a->b[14]; mac[5] = a->b[15];
}

void in6_eui64(In6 *out, const In6 *prefix, const u8 mac[6]) {
    CP(out->b, prefix->b, 8);
    out->b[8] = mac[0] ^ 0x02;                       /* the universal/local bit is inverted (RFC 4291 appendix A) */
    out->b[9] = mac[1]; out->b[10] = mac[2];
    out->b[11] = 0xFF; out->b[12] = 0xFE;
    out->b[13] = mac[3]; out->b[14] = mac[4]; out->b[15] = mac[5];
}

static size_t put_hex(char *o, u32 v) {              /* lower-case hex, no leading zeros */
    char t[8];
    size_t n = 0;
    do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    for (size_t i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}

static size_t put_dec(char *o, u32 v) {
    char t[10];
    size_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}

/* The text form of RFC 5952 section 4: lower case, no leading zeros in a group, the longest run of two or more zero groups (the first of equal ones) written "::", and an
 * address of the forms ::a.b.c.d or ::ffff:a.b.c.d (RFC 4291 section 2.2 item 3) with its last 32 bits in dotted decimal, as the C library's inet_ntop does. */
size_t in6_str(const In6 *a, char *buf, size_t cap) {
    u16 w[8];
    for (int i = 0; i < 8; i++) w[i] = in6_word(a, i);
    int best_base = -1, best_len = 0, cur_base = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (w[i] == 0) {
            if (cur_base < 0) { cur_base = i; cur_len = 1; } else cur_len++;
        } else if (cur_base >= 0) {
            if (best_base < 0 || cur_len > best_len) { best_base = cur_base; best_len = cur_len; }
            cur_base = -1;
        }
    }
    if (cur_base >= 0 && (best_base < 0 || cur_len > best_len)) { best_base = cur_base; best_len = cur_len; }
    if (best_base >= 0 && best_len < 2) best_base = -1;
    char t[48];
    size_t n = 0;
    for (int i = 0; i < 8; i++) {
        if (best_base >= 0 && i >= best_base && i < best_base + best_len) {
            if (i == best_base) t[n++] = ':';
            continue;
        }
        if (i != 0) t[n++] = ':';
        if (i == 6 && best_base == 0 && (best_len == 6 || (best_len == 5 && w[5] == 0xFFFF))) {
            for (int k = 12; k < 16; k++) {
                n += put_dec(t + n, a->b[k]);
                if (k < 15) t[n++] = '.';
            }
            goto done;
        }
        n += put_hex(t + n, w[i]);
    }
    if (best_base >= 0 && best_base + best_len == 8) t[n++] = ':';
done:
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        CP(buf, t, c);
        buf[c] = 0;
    }
    return n;
}

void in6_hex(const In6 *a, char out[33]) {
    for (int i = 0; i < 16; i++) {
        out[2 * i] = "0123456789abcdef"[a->b[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[a->b[i] & 15];
    }
    out[32] = 0;
}

/* ------------------------------------------------------------------ checksums */

u32 net6_pseudo_csum(const In6 *src, const In6 *dst, u8 proto, u32 len) {
    u64 sum = 0;
    for (int i = 0; i < 16; i += 2) sum += (u32)src->b[i] | (u32)src->b[i + 1] << 8;       /* little-endian words of the memory image, as net_csum reads them */
    for (int i = 0; i < 16; i += 2) sum += (u32)dst->b[i] | (u32)dst->b[i + 1] << 8;
    sum += (u32)((len >> 24) & 0xFF) | (u32)((len >> 16) & 0xFF) << 8;                      /* the 32-bit length, network order */
    sum += (u32)((len >> 8) & 0xFF) | (u32)(len & 0xFF) << 8;
    sum += (u32)proto << 8;                                                                  /* three zero bytes, then the next header */
    while (sum >> 32) sum = (sum & 0xFFFFFFFFu) + (sum >> 32);
    return (u32)sum;
}

/* ------------------------------------------------------------------ options of the hop-by-hop and destination options headers */

int ip6_opts_check(const u8 *p, size_t len, bool dst_mcast, u8 *code, u32 *ptr) {
    size_t i = 0;
    while (i < len) {
        u8 t = p[i];
        if (t == 0) { i++; continue; }                                  /* Pad1 */
        if (i + 1 >= len) { *code = 0; *ptr = (u32)i; return IP6W_ICMP; }       /* a type with no length byte */
        size_t l = p[i + 1];
        if (i + 2 + l > len) { *code = 0; *ptr = (u32)(i + 1); return IP6W_ICMP; }
        switch (t >> 6) {                                                /* what to do with an option nobody here knows (PadN, router alert and the tunnel limit are 00: skipped too) */
        case 0: break;
        case 1: return IP6W_DROP;
        case 2: *code = 2; *ptr = (u32)i; return IP6W_ICMP;
        default:
            if (dst_mcast) return IP6W_DROP;
            *code = 2; *ptr = (u32)i;
            return IP6W_ICMP;
        }
        i += 2 + l;
    }
    return IP6W_OK;
}

/* ------------------------------------------------------------------ the walker */

static void w_drop(Ip6Walk *w) { w->verdict = IP6W_DROP; }
static void w_pp(Ip6Walk *w, u8 code, u32 ptr) { w->verdict = IP6W_ICMP; w->pp_code = code; w->pp_ptr = ptr; }

void ip6_walk(const u8 *pkt, size_t len, u8 nh, u32 off, bool first, bool allow_frag, bool dst_mcast, Ip6Walk *w) {
    SET(w, 0, sizeof *w);
    w->verdict = IP6W_OK;
    u32 prev_nh = 6;                                                     /* the offset of the byte that holds `nh` (the IPv6 header's own field to start with) */
    if (off > len) { w_drop(w); return; }
    for (;;) {
        switch (nh) {
        case IP6P_HOPOPTS:
        case IP6P_DSTOPTS: {
            if (nh == IP6P_HOPOPTS && !(first && off == IP6_HLEN)) { w_pp(w, 1, prev_nh); return; }       /* RFC 8200 section 4.1: only straight after the IPv6 header */
            if (++w->nexthdrs > IP6_MAX_EXT) { w_drop(w); return; }
            if (off + 8 > len) { w_drop(w); return; }
            u32 hl = ((u32)pkt[off + 1] + 1) * 8;
            if (off + hl > len) { w_drop(w); return; }
            u8 code = 0;
            u32 ptr = 0;
            int v = ip6_opts_check(pkt + off + 2, hl - 2, dst_mcast, &code, &ptr);
            if (v == IP6W_DROP) { w_drop(w); return; }
            if (v == IP6W_ICMP) { w_pp(w, code, off + 2 + ptr); return; }
            prev_nh = off;
            nh = pkt[off];
            off += hl;
            break;
        }
        case IP6P_ROUTING: {
            if (++w->nexthdrs > IP6_MAX_EXT) { w_drop(w); return; }
            if (off + 8 > len) { w_drop(w); return; }
            u32 hl = ((u32)pkt[off + 1] + 1) * 8;
            if (off + hl > len) { w_drop(w); return; }
            if (pkt[off + 3] != 0) { w_pp(w, 0, off + 2); return; }      /* segments left: type 0 is refused (RFC 5095) and no other type is implemented, so the routing type is "unrecognized" */
            prev_nh = off;
            nh = pkt[off];
            off += hl;
            break;
        }
        case IP6P_FRAGMENT: {
            if (!allow_frag) { w_drop(w); return; }                      /* a fragment header inside reassembled data, or a second one */
            if (++w->nexthdrs > IP6_MAX_EXT) { w_drop(w); return; }
            if (off + 8 > len) { w_drop(w); return; }
            u16 fo = r16(pkt + off + 2);
            w->frag_off = (u32)(fo & 0xFFF8u);
            w->frag_more = (fo & 1) != 0;
            w->frag_id = r32(pkt + off + 4);
            w->frag_nh = pkt[off];
            w->frag_hoff = off;
            w->proto = IP6P_FRAGMENT;
            w->uoff = off + 8;
            w->nh_ptr = prev_nh;
            return;
        }
        case IP6P_NONE:
            w->proto = IP6P_NONE;
            w->uoff = off;
            w->nh_ptr = prev_nh;
            return;
        default:                                                         /* an upper layer (or something nobody here knows: the caller says which) */
            w->proto = nh;
            w->uoff = off;
            w->nh_ptr = prev_nh;
            return;
        }
    }
}

/* ------------------------------------------------------------------ neighbour discovery options */

bool nd_opts_parse(const u8 *p, size_t len, NdOpts *o) {
    SET(o, 0, sizeof *o);
    size_t i = 0;
    while (i < len) {
        if (i + 2 > len) return false;
        u8 t = p[i];
        size_t l = (size_t)p[i + 1] * 8;
        if (l == 0 || i + l > len) return false;
        switch (t) {
        case ND_OPT_SLLA: if (!o->slla && l >= 8) o->slla = p + i + 2; break;
        case ND_OPT_TLLA: if (!o->tlla && l >= 8) o->tlla = p + i + 2; break;
        case ND_OPT_PREFIX: if (l == 32 && o->nprefix < ND_MAX_PREFIX) o->prefix[o->nprefix++] = p + i; break;
        case ND_OPT_MTU: if (l == 8 && !o->mtu) o->mtu = p + i; break;
        case ND_OPT_RDNSS: if (l >= 24 && ((l - 8) % 16) == 0 && o->nrdnss < ND_MAX_RDNSS) o->rdnss[o->nrdnss++] = p + i; break;
        default: break;                                                  /* options nobody here knows are skipped (RFC 4861 section 4.6) */
        }
        i += l;
    }
    return true;
}

/* ------------------------------------------------------------------ reassembly */

int reasm6_add(Reasm6 *q, u32 off, bool more, u8 nh, const u8 *data, u32 len) {
    if (len == 0 || (off & 7) || (more && (len & 7))) return R6_BAD;
    if ((u64)off + len > IP6_MAX_PAYLOAD) return R6_BAD;
    u32 end = off + len;
    if (!more) {                                                         /* the last fragment: it fixes the length, and nothing we hold may reach beyond it */
        if (q->total && q->total != end) return R6_BAD;
        for (int i = 0; i < q->nfrag; i++) if (q->end[i] > end) return R6_BAD;
    } else if (q->total && end > q->total) {
        return R6_BAD;
    }
    for (int i = 0; i < q->nfrag; i++) {
        if (q->start[i] == off && q->end[i] == end) return R6_DUP;       /* the network may duplicate a fragment: RFC 8200 lets that one go while the others stay */
        if (off < q->end[i] && q->start[i] < end) return R6_BAD;         /* any other overlap: the whole datagram is thrown away (RFC 5722) */
    }
    if (q->nfrag >= R6_MAX_FRAGS) return R6_BAD;
    CP(q->buf + IP6_HLEN + off, data, len);
    q->start[q->nfrag] = off;
    q->end[q->nfrag] = end;
    q->nfrag++;
    q->got += len;
    if (off == 0) { q->have_first = true; q->first_nh = nh; }
    if (!more) q->total = end;
    return (q->total && q->got == q->total && q->have_first) ? R6_DONE : R6_MORE;
}
