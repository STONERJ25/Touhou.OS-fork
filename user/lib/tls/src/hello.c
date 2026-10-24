/* ClientHello construction and ServerHello / HelloRetryRequest processing (RFC 8446 4.1, RFC 5246 7.4.1). */
#include "tlsconn.h"

typedef struct {
    u8 *p;
    size_t n, cap;
    int err;
} bld;

static void b8(bld *b, unsigned v) { if (b->n + 1 > b->cap) { b->err = 1; return; } b->p[b->n++] = (u8)v; }
static void b16(bld *b, unsigned v) { b8(b, v >> 8); b8(b, v); }
static void b24(bld *b, unsigned v) { b8(b, v >> 16); b8(b, v >> 8); b8(b, v); }
static void bn(bld *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->err = 1; return; }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static size_t b_open16(bld *b) { size_t pos = b->n; b16(b, 0); return pos; }
static void b_close16(bld *b, size_t pos) {
    size_t len = b->n - pos - 2;
    if (!b->err) { b->p[pos] = (u8)(len >> 8); b->p[pos + 1] = (u8)len; }
}
static size_t b_open8(bld *b) { size_t pos = b->n; b8(b, 0); return pos; }
static void b_close8(bld *b, size_t pos) {
    size_t len = b->n - pos - 1;
    if (!b->err) b->p[pos] = (u8)len;
}

static const u8 HRR_RANDOM[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

static int group_index(unsigned group) { return group == GROUP_SECP256R1 ? 0 : group == GROUP_SECP384R1 ? 1 : -1; }

int tls_ec_keygen(struct tls_conn *c, unsigned group) {
    int idx = group_index(group);
    if (idx < 0) return TLS_E_ARG;
    if (c->ec_have[idx]) return 0;
    int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
    size_t size = tlsc_ec_size(curve);
    for (int tries = 0; tries < 8; tries++) {
        u8 rnd[96];
        int rc = tls_rand(c, rnd, 2 * size);
        if (rc) return tls_fail(c, rc, -1);
        rc = tlsc_ecdh_keypair(curve, rnd, c->ec_priv[idx], c->ec_pub[idx]);
        tlsc_wipe(rnd, sizeof rnd);
        if (rc == 0) {
            c->ec_have[idx] = 1;
            return 0;
        }
    }
    return tls_fail(c, TLS_E_ENTROPY, AL_INTERNAL_ERROR);
}

/* The cipher suites to offer, in our order of preference: AES first if the CPU has AES-NI, ChaCha20 first if not. */
static int build_suites(struct tls_conn *c) {
    static const unsigned ORDER_AES[]  = { 0x1301, 0x1303, 0x1302, 0xc02b, 0xcca9, 0xc02c, 0xc02f, 0xcca8, 0xc030 };
    static const unsigned ORDER_SOFT[] = { 0x1303, 0x1301, 0x1302, 0xcca9, 0xc02b, 0xc02c, 0xcca8, 0xc02f, 0xc030 };
    const unsigned *order = tlsc_aes_hw_available() && !(c->cfg.flags & TLS_F_NO_AESNI) ? ORDER_AES : ORDER_SOFT;
    unsigned mask = c->cfg.suites ? c->cfg.suites : (TLS_S_AES128_GCM | TLS_S_AES256_GCM | TLS_S_CHACHA20);
    c->n_offered = 0;
    for (size_t i = 0; i < 9; i++) {
        const suite_info *s = tls_suite_find(order[i]);
        if (!s) continue;
        unsigned bit = s->cipher == CIPHER_AES128 ? TLS_S_AES128_GCM : s->cipher == CIPHER_AES256 ? TLS_S_AES256_GCM : TLS_S_CHACHA20;
        if (!(mask & bit)) continue;
        if (s->version == 0x0304 && !c->offer_13) continue;
        if (s->version == 0x0303 && !c->offer_12) continue;
        c->offered_suites[c->n_offered++] = s->id;
    }
    return c->n_offered ? 0 : TLS_E_ARG;
}

int tls_send_client_hello(struct tls_conn *c, unsigned retry_group) {
    int rc;
    if (!retry_group) {
        c->offer_13 = !(c->cfg.flags & TLS_F_TLS12_ONLY);
        c->offer_12 = !(c->cfg.flags & TLS_F_TLS13_ONLY);
        if ((rc = tls_rand(c, c->client_random, 32)) || (rc = tls_rand(c, c->session_id, 32))) return tls_fail(c, rc, -1);
        u8 priv[32];
        if ((rc = tls_rand(c, priv, 32))) return tls_fail(c, rc, -1);
        memcpy(c->x25519_priv, priv, 32);
        tlsc_x25519_base(c->x25519_pub, priv);
        tlsc_wipe(priv, sizeof priv);
        if (build_suites(c)) return tls_fail(c, TLS_E_ARG, -1);
    } else if ((rc = tls_ec_keygen(c, retry_group))) {
        return rc;
    }

    u8 buf[1280];
    bld b = { buf, 0, sizeof buf, 0 };
    b8(&b, HS_CLIENT_HELLO);
    size_t len_pos = b.n;
    b24(&b, 0);
    b16(&b, 0x0303);
    bn(&b, c->client_random, 32);
    b8(&b, 32);
    bn(&b, c->session_id, 32);
    size_t sp = b_open16(&b);
    for (int i = 0; i < c->n_offered; i++) b16(&b, c->offered_suites[i]);
    b_close16(&b, sp);
    b8(&b, 1);
    b8(&b, 0);                                                       /* compression: null only */

    size_t ext = b_open16(&b);
    if (c->sni[0] && !x509_host_is_ipv4(c->sni, (u8[4]){ 0 })) {     /* server_name (literal addresses are not allowed in it) */
        size_t hostlen = strlen(c->sni);
        b16(&b, 0x0000);
        size_t e = b_open16(&b);
        size_t l = b_open16(&b);
        b8(&b, 0);
        b16(&b, (unsigned)hostlen);
        bn(&b, c->sni, hostlen);
        b_close16(&b, l);
        b_close16(&b, e);
    }
    b16(&b, 0x000a);                                                 /* supported_groups */
    { size_t e = b_open16(&b), l = b_open16(&b); b16(&b, GROUP_X25519); b16(&b, GROUP_SECP256R1); b16(&b, GROUP_SECP384R1); b_close16(&b, l); b_close16(&b, e); }
    if (c->offer_12) {
        b16(&b, 0x000b);                                             /* ec_point_formats: uncompressed */
        b16(&b, 2);
        b8(&b, 1);
        b8(&b, 0);
    }
    b16(&b, 0x000d);                                                 /* signature_algorithms */
    {
        size_t e = b_open16(&b), l = b_open16(&b);
        b16(&b, 0x0403); b16(&b, 0x0503); b16(&b, 0x0804); b16(&b, 0x0805); b16(&b, 0x0806);
        if (c->offer_12) { b16(&b, 0x0401); b16(&b, 0x0501); b16(&b, 0x0601); }
        b_close16(&b, l);
        b_close16(&b, e);
    }
    b16(&b, 0x0032);                                                 /* signature_algorithms_cert: what we can check on certificates */
    { size_t e = b_open16(&b), l = b_open16(&b); b16(&b, 0x0403); b16(&b, 0x0503); b16(&b, 0x0401); b16(&b, 0x0501); b16(&b, 0x0601); b_close16(&b, l); b_close16(&b, e); }
    if (c->cfg.alpn) {
        size_t alen = strlen(c->cfg.alpn);
        b16(&b, 0x0010);
        size_t e = b_open16(&b), l = b_open16(&b);
        b8(&b, (unsigned)alen);
        bn(&b, c->cfg.alpn, alen);
        b_close16(&b, l);
        b_close16(&b, e);
    }
    if (c->offer_12) {
        b16(&b, 0x0017);                                             /* extended_master_secret */
        b16(&b, 0);
        b16(&b, 0xff01);                                             /* renegotiation_info: an empty one, "I know RFC 5746" */
        b16(&b, 1);
        b8(&b, 0);
    }
    if (c->offer_13) {
        b16(&b, 0x002b);                                             /* supported_versions */
        size_t e = b_open16(&b), l = b_open8(&b);
        b16(&b, 0x0304);
        if (c->offer_12) b16(&b, 0x0303);
        b_close8(&b, l);
        b_close16(&b, e);
        b16(&b, 0x0033);                                             /* key_share: one share, the group of the retry if the server asked for another */
        e = b_open16(&b);
        l = b_open16(&b);
        if (!retry_group || retry_group == GROUP_X25519) {
            b16(&b, GROUP_X25519);
            b16(&b, 32);
            bn(&b, c->x25519_pub, 32);
        } else {
            int idx = group_index(retry_group);
            size_t pl = idx == 0 ? 65 : 97;
            b16(&b, retry_group);
            b16(&b, (unsigned)pl);
            bn(&b, c->ec_pub[idx], pl);
        }
        b_close16(&b, l);
        b_close16(&b, e);
        if (retry_group && c->cookie_len) {
            b16(&b, 0x002c);                                         /* cookie, echoed from the HelloRetryRequest */
            e = b_open16(&b);
            l = b_open16(&b);
            bn(&b, c->cookie, c->cookie_len);
            b_close16(&b, l);
            b_close16(&b, e);
        }
    }
    b_close16(&b, ext);
    if (b.err) return tls_fail(c, TLS_E_ARG, -1);
    size_t body = b.n - len_pos - 3;
    buf[len_pos] = (u8)(body >> 16);
    buf[len_pos + 1] = (u8)(body >> 8);
    buf[len_pos + 2] = (u8)body;
    if (!retry_group) {
        memcpy(c->ch_copy, buf, b.n);
        c->ch_len = b.n;
    }
    return tls_send_handshake(c, buf, b.n);
}

/* ---------------------------------------------------------------------------------------------------- ServerHello */

int tls_process_server_hello(struct tls_conn *c, const u8 *msg, size_t len, unsigned *retry_group) {
    rd_t r = { msg + 4, len - 4, 0 };
    unsigned legacy = rd16(&r);
    const u8 *random = rdn(&r, 32);
    unsigned sidlen = rd8(&r);
    const u8 *sid = rdn(&r, sidlen);
    unsigned suite_id = rd16(&r);
    unsigned comp = rd8(&r);
    rd_t ext = { NULL, 0, 0 };
    if (r.n > 0) {
        unsigned elen = rd16(&r);
        ext = rd_sub(&r, elen);
    }
    if (r.err || ext.err || r.n != 0 || sidlen > 32) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
    int is_hrr = memcmp(random, HRR_RANDOM, 32) == 0;

    unsigned sel_version = 0, key_group = 0, key_len = 0;
    const u8 *key_data = NULL;
    int seen_sv = 0, seen_ks = 0, seen_cookie = 0, ems = 0, seen_reneg = 0, other = 0;
    const u8 *cookie = NULL;
    unsigned cookie_len = 0;
    char alpn[16] = "";
    while (ext.n > 0) {
        unsigned type = rd16(&ext);
        unsigned dlen = rd16(&ext);
        rd_t d = rd_sub(&ext, dlen);
        if (ext.err || d.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        switch (type) {
        case 0x002b:
            if (seen_sv++ || dlen != 2) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
            sel_version = rd16(&d);
            break;
        case 0x0033:
            if (seen_ks++) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
            key_group = rd16(&d);
            if (!is_hrr) {
                key_len = rd16(&d);
                key_data = rdn(&d, key_len);
            }
            if (d.err || d.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
            break;
        case 0x002c: {
            if (seen_cookie++) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
            cookie_len = rd16(&d);
            cookie = rdn(&d, cookie_len);
            if (d.err || d.n != 0 || cookie_len == 0 || cookie_len > sizeof c->cookie) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
            break;
        }
        case 0x0017:
            ems = 1;
            break;
        case 0xff01:
            seen_reneg = 1;
            if (dlen != 1 || d.p[0] != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_HANDSHAKE_FAILURE);
            break;
        case 0x0010: {                                               /* TLS 1.2 only: ALPN in the ServerHello */
            unsigned l = rd16(&d);
            rd_t list = rd_sub(&d, l);
            unsigned nl = rd8(&list);
            const u8 *name = rdn(&list, nl);
            if (d.err || list.err || list.n != 0 || nl == 0 || nl >= sizeof alpn) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
            memcpy(alpn, name, nl);
            alpn[nl] = 0;
            break;
        }
        default:
            other = 1;                                               /* TLS 1.2: ec_point_formats, session_ticket and friends do not concern us */
            break;
        }
        if (type == 0x0010 || type == 0x0017 || type == 0xff01) other = 1;      /* fine in a TLS 1.2 ServerHello, forbidden in a 1.3 one */
    }
    (void)seen_reneg;

    unsigned version = sel_version ? sel_version : legacy;
    if (version == 0x0304) {
        if (!c->offer_13 || legacy != 0x0303) return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER);
        if (other) return tls_fail(c, TLS_E_PROTOCOL, AL_UNSUPPORTED_EXTENSION);
    } else if (version == 0x0303) {
        if (!c->offer_12) return tls_fail(c, TLS_E_VERSION, AL_PROTOCOL_VERSION);
        if (is_hrr || sel_version) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
    } else {
        return tls_fail(c, TLS_E_VERSION, AL_PROTOCOL_VERSION);
    }
    if (comp != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);

    const suite_info *s = tls_suite_find(suite_id);
    int offered = 0;
    for (int i = 0; i < c->n_offered; i++)
        if (c->offered_suites[i] == suite_id) offered = 1;
    if (!s || !offered || s->version != (int)version) return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER);

    if (version == 0x0304) {
        c->version = 0x0304;                                         /* from here on a dummy ChangeCipherSpec from the server is dropped */
        if (sidlen != 32 || memcmp(sid, c->session_id, 32) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        if (is_hrr) {
            if (c->hello_retry) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);                    /* a second one is not allowed */
            if (!seen_ks || (key_group != GROUP_SECP256R1 && key_group != GROUP_SECP384R1)) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
            c->hello_retry = 1;
            c->info.hello_retry = 1;
            c->suite = s;
            if (cookie) {
                memcpy(c->cookie, cookie, cookie_len);
                c->cookie_len = cookie_len;
            }
            /* RFC 8446 4.4.1: the first ClientHello is replaced in the transcript by a message_hash message */
            u8 h[64], hdr[4];
            size_t hl = tlsc_hash_len(s->hash);
            tlsc_hash_once(s->hash, c->ch_copy, c->ch_len, h);
            tlsc_hash_init(&c->th256, TLSC_SHA256);
            tlsc_hash_init(&c->th384, TLSC_SHA384);
            hdr[0] = HS_MESSAGE_HASH; hdr[1] = 0; hdr[2] = 0; hdr[3] = (u8)hl;
            tls_transcript_add(c, hdr, 4);
            tls_transcript_add(c, h, hl);
            tls_transcript_add(c, msg, len);
            tls_hs_consume(c, len);
            *retry_group = key_group;
            return 1;
        }
        if (!seen_ks || !seen_sv) return tls_fail(c, TLS_E_PROTOCOL, AL_MISSING_EXTENSION);
        if (c->hello_retry && (s != c->suite || key_group != *retry_group)) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        size_t want = key_group == GROUP_X25519 ? 32 : key_group == GROUP_SECP256R1 ? 65 : key_group == GROUP_SECP384R1 ? 97 : 0;
        if (!want || key_len != want || !key_data) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        if (!c->hello_retry && key_group != GROUP_X25519) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);   /* we only sent an x25519 share */
        c->sh_group = (int)key_group;
        memcpy(c->sh_share, key_data, key_len);
        c->sh_share_len = key_len;
        c->suite = s;
        c->hash_alg = s->hash;
        memcpy(c->server_random, random, 32);
        tls_transcript_add(c, msg, len);
        tls_hs_consume(c, len);
        return 0;
    }

    /* TLS 1.2 */
    if (c->offer_13 && (memcmp(random + 24, "DOWNGRD\x01", 8) == 0 || memcmp(random + 24, "DOWNGRD\x00", 8) == 0)) return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER);
    if (alpn[0]) {
        if (!c->cfg.alpn || strcmp(alpn, c->cfg.alpn) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNSUPPORTED_EXTENSION);
        memcpy(c->info.alpn, alpn, sizeof c->info.alpn);
        c->sh_alpn_seen = 1;
    }
    c->version = 0x0303;
    c->suite = s;
    c->hash_alg = s->hash;
    c->ems = ems;                                                    /* without it (older servers) the classic master secret is used; see docs/TLS.md */
    memcpy(c->server_random, random, 32);
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);
    return 0;
}
