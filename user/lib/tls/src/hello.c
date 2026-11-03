/*
 * ClientHello construction and ServerHello / HelloRetryRequest processing (RFC 8446 4.1 and 4.2, RFC 5246 7.4.1, RFC 7627,
 * RFC 5746).
 *
 * Whatever ClientHello goes out (built here, or given by a test script) is parsed back into c->offer, and the server's answer is
 * checked against that: the cipher suite, the version, the key share group, every extension must be one that was offered.
 */
#include "conn.h"

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

static const u8 HRR_RANDOM[32] = {                    /* SHA-256("HelloRetryRequest"), RFC 8446 4.1.3 */
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };

int offered(const unsigned *list, int n, unsigned v) {
    for (int i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

static int group_index(unsigned group) { return group == GROUP_SECP256R1 ? 0 : group == GROUP_SECP384R1 ? 1 : -1; }
static size_t share_len(unsigned group) { return group == GROUP_X25519 ? 32 : group == GROUP_SECP256R1 ? 65 : group == GROUP_SECP384R1 ? 97 : 0; }

int ec_keygen(TlsConn *c, unsigned group) {
    int idx = group_index(group);
    if (idx < 0) return tls_fail(c, TLS_E_ARG, AL_INTERNAL_ERROR, "no key generation for group 0x%04x", group);
    if (c->ec_have[idx]) return 0;
    int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
    size_t size = tlsc_ec_size(curve);
    for (int tries = 0; tries < 8; tries++) {
        u8 rnd[96];
        if (idx == 0 && c->script.have_p256) {                       /* a test gives the private key: hi = 0, lo = k */
            memset(rnd, 0, 32);
            memcpy(rnd + 32, c->script.p256, 32);
        } else if (tls_rand(c, rnd, 2 * size)) {
            return tls_fail(c, TLS_E_RANDOM, AL_INTERNAL_ERROR, "the kernel gave no random numbers");
        }
        int rc = tlsc_ecdh_keypair(curve, rnd, c->ec_priv[idx], c->ec_pub[idx]);
        tlsc_wipe(rnd, sizeof rnd);
        if (rc == 0) {
            c->ec_have[idx] = 1;
            return 0;
        }
    }
    return tls_fail(c, TLS_E_RANDOM, AL_INTERNAL_ERROR, "could not make an EC key");
}

/* The cipher suites to offer, in our order of preference: AES first if the CPU has AES-NI, ChaCha20 first if not. */
static int build_suites(TlsConn *c, unsigned *out, int v13, int v12) {
    static const unsigned ORDER_AES[]  = { 0x1301, 0x1303, 0x1302, 0xc02b, 0xcca9, 0xc02c, 0xc02f, 0xcca8, 0xc030 };
    static const unsigned ORDER_SOFT[] = { 0x1303, 0x1301, 0x1302, 0xcca9, 0xc02b, 0xc02c, 0xcca8, 0xc02f, 0xc030 };
    const unsigned *order = tlsc_aes_hw_available() && !(c->flags & TLS_NO_AESNI) ? ORDER_AES : ORDER_SOFT;
    unsigned mask = c->suites_mask ? c->suites_mask : (TLS_SUITE_AES128_GCM | TLS_SUITE_AES256_GCM | TLS_SUITE_CHACHA20);
    int n = 0;
    for (size_t i = 0; i < 9; i++) {
        const suite_info *s = suite_find(order[i]);
        unsigned bit = s->cipher == CIPHER_AES128 ? TLS_SUITE_AES128_GCM : s->cipher == CIPHER_AES256 ? TLS_SUITE_AES256_GCM : TLS_SUITE_CHACHA20;
        if (!(mask & bit)) continue;
        if ((s->version == 0x0304 && !v13) || (s->version == 0x0303 && !v12)) continue;
        out[n++] = s->id;
    }
    return n;
}

static int build_client_hello(TlsConn *c, u8 *buf, size_t cap, size_t *outlen) {
    int v13 = !(c->flags & TLS_ONLY_TLS12), v12 = !(c->flags & TLS_ONLY_TLS13);
    unsigned suites[16];
    int nsuites = build_suites(c, suites, v13, v12);
    if (nsuites == 0) return tls_fail(c, TLS_E_ARG, -1, "no cipher suite is left to offer (TlsConfig.suites and the version flags exclude them all)");

    bld b = { buf, 0, cap, 0 };
    b8(&b, HS_CLIENT_HELLO);
    size_t len_pos = b.n;
    b24(&b, 0);
    b16(&b, 0x0303);
    bn(&b, c->offer.random, 32);
    b8(&b, 32);                                                      /* a session id: TLS 1.3 middlebox compatibility mode */
    bn(&b, c->offer.sid, 32);
    size_t sp = b_open16(&b);
    for (int i = 0; i < nsuites; i++) b16(&b, suites[i]);
    b_close16(&b, sp);
    b8(&b, 1);
    b8(&b, 0);                                                       /* compression: null only */

    size_t ext = b_open16(&b);
    if (!c->host_is_ip && c->host[0]) {                              /* server_name (an address may not go in it) */
        size_t hostlen = strlen(c->host);
        b16(&b, EXT_SERVER_NAME);
        size_t e = b_open16(&b), l = b_open16(&b);
        b8(&b, 0);
        b16(&b, (unsigned)hostlen);
        bn(&b, c->host, hostlen);
        b_close16(&b, l);
        b_close16(&b, e);
    }
    b16(&b, EXT_SUPPORTED_GROUPS);
    { size_t e = b_open16(&b), l = b_open16(&b); b16(&b, GROUP_X25519); b16(&b, GROUP_SECP256R1); b16(&b, GROUP_SECP384R1); b_close16(&b, l); b_close16(&b, e); }
    if (v12) {
        b16(&b, EXT_EC_POINT_FORMATS);                               /* uncompressed only */
        b16(&b, 2);
        b8(&b, 1);
        b8(&b, 0);
    }
    b16(&b, EXT_SIGNATURE_ALGORITHMS);                               /* handshake signatures, and (rsa_pkcs1) certificates */
    {
        static const unsigned SIGALGS[] = { 0x0403, 0x0503, 0x0807, 0x0804, 0x0805, 0x0806, 0x0809, 0x080a, 0x080b, 0x0401, 0x0501, 0x0601 };
        size_t e = b_open16(&b), l = b_open16(&b);
        for (size_t i = 0; i < TLSC_ARRAY_SIZE(SIGALGS); i++) b16(&b, SIGALGS[i]);
        b_close16(&b, l);
        b_close16(&b, e);
    }
    if (c->alpn[0]) {
        size_t alen = strlen(c->alpn);
        b16(&b, EXT_ALPN);
        size_t e = b_open16(&b), l = b_open16(&b);
        b8(&b, (unsigned)alen);
        bn(&b, c->alpn, alen);
        b_close16(&b, l);
        b_close16(&b, e);
    }
    if (v12) {
        b16(&b, EXT_EXTENDED_MASTER_SECRET);
        b16(&b, 0);
        b16(&b, EXT_RENEGOTIATION_INFO);                             /* empty: "this client knows RFC 5746" (and never renegotiates) */
        b16(&b, 1);
        b8(&b, 0);
    }
    if (v13) {
        b16(&b, EXT_SUPPORTED_VERSIONS);
        size_t e = b_open16(&b), l = b_open8(&b);
        b16(&b, 0x0304);
        if (v12) b16(&b, 0x0303);
        b_close8(&b, l);
        b_close16(&b, e);
        b16(&b, EXT_KEY_SHARE);                                      /* one share: x25519 first, the group of a retry later */
        e = b_open16(&b);
        l = b_open16(&b);
        unsigned g = c->hello_retry && c->retry_group ? c->retry_group : GROUP_X25519;
        if (g == GROUP_X25519) {
            b16(&b, GROUP_X25519);
            b16(&b, 32);
            bn(&b, c->x25519_pub, 32);
        } else {
            int idx = group_index(g);
            b16(&b, g);
            b16(&b, (unsigned)share_len(g));
            bn(&b, c->ec_pub[idx], share_len(g));
        }
        b_close16(&b, l);
        b_close16(&b, e);
        if (c->hello_retry && c->cookie_len) {
            b16(&b, EXT_COOKIE);                                     /* echoed from the HelloRetryRequest */
            e = b_open16(&b);
            l = b_open16(&b);
            bn(&b, c->cookie, c->cookie_len);
            b_close16(&b, l);
            b_close16(&b, e);
        }
    }
    b_close16(&b, ext);
    if (b.err) return tls_fail(c, TLS_E_ARG, -1, "the ClientHello does not fit");
    size_t body = b.n - len_pos - 3;
    buf[len_pos] = (u8)(body >> 16);
    buf[len_pos + 1] = (u8)(body >> 8);
    buf[len_pos + 2] = (u8)body;
    *outlen = b.n;
    return 0;
}

int hello_parse_offer(const u8 *msg, size_t len, offer_t *o) {
    memset(o, 0, sizeof *o);
    rd_t r = { msg, len, 0 };
    if (rd8(&r) != HS_CLIENT_HELLO || rd24(&r) != len - 4) return -1;
    unsigned legacy = rd16(&r);
    const u8 *random = rdn(&r, 32);
    unsigned sidlen = rd8(&r);
    const u8 *sid = rdn(&r, sidlen);
    rd_t suites = rd_sub(&r, rd16(&r));
    rd_t comp = rd_sub(&r, rd8(&r));
    if (r.err || sidlen > 32 || suites.n % 2 || suites.n == 0 || comp.n == 0) return -1;
    memcpy(o->random, random, 32);
    memcpy(o->sid, sid, sidlen);
    o->sid_len = sidlen;
    while (suites.n && o->nsuites < OFFER_MAX) o->suites[o->nsuites++] = rd16(&suites);
    o->v12 = legacy == 0x0303;
    rd_t ext = { NULL, 0, 0 };
    if (r.n) ext = rd_sub(&r, rd16(&r));
    if (ext.err || r.n) return -1;
    int seen_sv = 0;
    while (ext.n > 0) {
        unsigned type = rd16(&ext);
        rd_t d = rd_sub(&ext, rd16(&ext));
        if (ext.err) return -1;
        if (offered(o->exts, o->nexts, type) || o->nexts >= OFFER_MAX) return -1;
        o->exts[o->nexts++] = type;
        if (type == EXT_SUPPORTED_GROUPS) {
            rd_t l = rd_sub(&d, rd16(&d));
            while (l.n >= 2 && o->ngroups < OFFER_MAX) o->groups[o->ngroups++] = rd16(&l);
        } else if (type == EXT_SIGNATURE_ALGORITHMS) {
            rd_t l = rd_sub(&d, rd16(&d));
            while (l.n >= 2 && o->nsigalgs < OFFER_MAX) o->sigalgs[o->nsigalgs++] = rd16(&l);
        } else if (type == EXT_ALPN) {
            rd_t l = rd_sub(&d, rd16(&d));
            while (l.n > 0 && !l.err) {
                unsigned nl = rd8(&l);
                const u8 *name = rdn(&l, nl);
                if (l.err || nl == 0 || nl >= sizeof o->alpn[0]) return -1;
                if (o->nalpn < 4) { memcpy(o->alpn[o->nalpn], name, nl); o->alpn[o->nalpn][nl] = 0; o->nalpn++; }
            }
        } else if (type == EXT_SUPPORTED_VERSIONS) {
            rd_t l = rd_sub(&d, rd8(&d));
            seen_sv = 1;
            o->v12 = 0;
            while (l.n >= 2) {
                unsigned v = rd16(&l);
                if (v == 0x0304) o->v13 = 1;
                if (v == 0x0303) o->v12 = 1;
            }
        } else if (type == EXT_KEY_SHARE) {
            rd_t l = rd_sub(&d, rd16(&d));
            while (l.n > 0 && !l.err) {
                unsigned g = rd16(&l);
                rdn(&l, rd16(&l));
                if (o->nshares < 4) o->shares[o->nshares++] = g;
            }
            if (l.err) return -1;
        }
        if (d.err) return -1;
    }
    (void)seen_sv;
    return 0;
}

int hello_send(TlsConn *c) {
    u8 buf[2048];
    const u8 *msg = buf;
    size_t len = 0;
    int idx = c->hello_retry ? 1 : 0;
    if (!c->hello_retry) {
        if (tls_rand(c, c->offer.random, 32) || tls_rand(c, c->offer.sid, 32))
            return tls_fail(c, TLS_E_RANDOM, -1, "the kernel gave no random numbers");
        if (c->script.have_x25519) {
            memcpy(c->x25519_priv, c->script.x25519, 32);
        } else if (tls_rand(c, c->x25519_priv, 32)) {
            return tls_fail(c, TLS_E_RANDOM, -1, "the kernel gave no random numbers");
        }
        tlsc_x25519_base(c->x25519_pub, c->x25519_priv);
        c->have_x25519 = 1;
    } else if (c->retry_group && c->retry_group != GROUP_X25519) {
        int rc = ec_keygen(c, c->retry_group);
        if (rc) return rc;
    }
    if (c->script.n > idx) {                                         /* a test replays a ClientHello it was given */
        msg = c->script.ch[idx];
        len = c->script.ch_len[idx];
    } else {
        int rc = build_client_hello(c, buf, sizeof buf, &len);
        if (rc) return rc;
    }
    offer_t o;
    if (hello_parse_offer(msg, len, &o) != 0) return tls_fail(c, TLS_E_ARG, -1, "the ClientHello to send does not parse");
    c->offer = o;
    c->legacy_record_version = !c->hello_retry;                      /* the first ClientHello's record says 0x0301, as everybody's does */
    int rc = hs_send(c, msg, len);
    c->legacy_record_version = 0;
    return rc;
}

/* ---------------------------------------------------------------------------------------------------- ServerHello */

static int check_downgrade(TlsConn *c, const u8 *random) {
    if (c->offer.v13 && (memcmp(random + 24, "DOWNGRD\x01", 8) == 0 || memcmp(random + 24, "DOWNGRD\x00", 8) == 0))
        return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER, "the server's random says it can do TLS 1.3 but answered with TLS 1.2: a downgrade attack?");
    return 0;
}

int hello_process_server(TlsConn *c, const u8 *msg, size_t len) {
    rd_t r = { msg + 4, len - 4, 0 };
    unsigned legacy = rd16(&r);
    const u8 *random = rdn(&r, 32);
    unsigned sidlen = rd8(&r);
    const u8 *sid = rdn(&r, sidlen);
    unsigned suite_id = rd16(&r);
    unsigned comp = rd8(&r);
    rd_t ext = { NULL, 0, 0 };
    if (r.n > 0) ext = rd_sub(&r, rd16(&r));
    if (r.err || ext.err || r.n != 0 || sidlen > 32) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed ServerHello");
    int is_hrr = memcmp(random, HRR_RANDOM, 32) == 0;

    unsigned sel_version = 0, key_group = 0, key_len = 0;
    const u8 *key_data = NULL, *cookie = NULL;
    unsigned cookie_len = 0;
    int seen_sv = 0, seen_ks = 0, seen_cookie = 0, ems = 0, reneg = 0;
    char alpn[32] = "";
    unsigned seen[OFFER_MAX];
    int nseen = 0;
    while (ext.n > 0) {
        unsigned type = rd16(&ext);
        rd_t d = rd_sub(&ext, rd16(&ext));
        if (ext.err || d.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed ServerHello extension");
        if (offered(seen, nseen, type) || nseen >= OFFER_MAX) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the ServerHello repeats extension 0x%04x", type);
        seen[nseen++] = type;
        if (!offered(c->offer.exts, c->offer.nexts, type) && !(is_hrr && type == EXT_COOKIE))
            return tls_fail(c, TLS_E_SUITE, AL_UNSUPPORTED_EXTENSION, "the server answered with extension 0x%04x, which was not offered", type);
        switch (type) {
        case EXT_SUPPORTED_VERSIONS:
            seen_sv = 1;
            sel_version = rd16(&d);
            break;
        case EXT_KEY_SHARE:
            seen_ks = 1;
            key_group = rd16(&d);
            if (!is_hrr) {
                key_len = rd16(&d);
                key_data = rdn(&d, key_len);
            }
            break;
        case EXT_COOKIE:
            seen_cookie = 1;
            cookie_len = rd16(&d);
            cookie = rdn(&d, cookie_len);
            if (cookie_len == 0) d.err = 1;
            break;
        case EXT_EXTENDED_MASTER_SECRET:
            ems = 1;
            break;
        case EXT_RENEGOTIATION_INFO:                                 /* RFC 5746: empty in a first handshake */
            reneg = 1;
            if (rd8(&d) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_HANDSHAKE_FAILURE, "renegotiation_info is not empty in a first handshake");
            break;
        case EXT_ALPN: {                                             /* TLS 1.2 only: ALPN in the ServerHello */
            rd_t list = rd_sub(&d, rd16(&d));
            unsigned nl = rd8(&list);
            const u8 *name = rdn(&list, nl);
            if (list.err || list.n != 0 || nl == 0 || nl >= sizeof alpn) d.err = 1;
            else { memcpy(alpn, name, nl); alpn[nl] = 0; }
            break;
        }
        case EXT_SERVER_NAME:                                        /* an empty acknowledgement */
        case EXT_EC_POINT_FORMATS:
        default:
            rdn(&d, d.n);
            break;
        }
        if (d.err || d.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed ServerHello extension 0x%04x", type);
    }
    (void)reneg;

    unsigned version = seen_sv ? sel_version : legacy;
    if (seen_sv && sel_version != 0x0304)
        return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER, "the server's supported_versions picked 0x%04x", sel_version);
    if (version == 0x0304) {
        if (!c->offer.v13) return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER, "the server picked TLS 1.3, which was not offered");
        if (legacy != 0x0303) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "a TLS 1.3 ServerHello with legacy_version 0x%04x", legacy);
        for (int i = 0; i < nseen; i++)                              /* RFC 8446 4.1.3 / 4.1.4: what may be in these two */
            if (seen[i] != EXT_SUPPORTED_VERSIONS && seen[i] != EXT_KEY_SHARE && !(is_hrr && seen[i] == EXT_COOKIE))
                return tls_fail(c, TLS_E_PROTOCOL, AL_UNSUPPORTED_EXTENSION, "extension 0x%04x does not belong in a TLS 1.3 %s", seen[i], is_hrr ? "HelloRetryRequest" : "ServerHello");
    } else if (version == 0x0303) {
        if (!c->offer.v12) return tls_fail(c, TLS_E_VERSION, AL_PROTOCOL_VERSION, "the server picked TLS 1.2, which was not offered");
        if (is_hrr) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "a HelloRetryRequest without TLS 1.3");
        if (c->hello_retry) return tls_fail(c, TLS_E_VERSION, AL_ILLEGAL_PARAMETER, "TLS 1.2 after a HelloRetryRequest");
        int rc = check_downgrade(c, random);
        if (rc) return rc;
    } else {
        return tls_fail(c, TLS_E_VERSION, AL_PROTOCOL_VERSION, "the server wants protocol version 0x%04x; only TLS 1.2 and 1.3 are spoken here", version);
    }
    if (comp != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server picked a compression method");

    const suite_info *s = suite_find(suite_id);
    if (!s || !offered(c->offer.suites, c->offer.nsuites, suite_id) || s->version != (int)version)
        return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server picked cipher suite 0x%04x, which was not offered for this version", suite_id);

    if (version == 0x0304) {
        if (sidlen != c->offer.sid_len || memcmp(sid, c->offer.sid, sidlen) != 0)
            return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the ServerHello does not echo our session id");
        if (is_hrr) {
            if (c->hello_retry) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a second HelloRetryRequest");
            if (seen_ks) {
                if (!offered(c->offer.groups, c->offer.ngroups, key_group) || offered(c->offer.shares, c->offer.nshares, key_group) || !share_len(key_group))
                    return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the HelloRetryRequest asks for group 0x%04x, which cannot be sent", key_group);
            } else if (!seen_cookie) {
                return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "a HelloRetryRequest that changes nothing");
            }
            if (cookie) {
                if (cookie_len > sizeof c->cookie) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "a HelloRetryRequest cookie of %u bytes", cookie_len);
                memcpy(c->cookie, cookie, cookie_len);
                c->cookie_len = cookie_len;
            }
            c->hello_retry = 1;
            c->info.hello_retry = 1;
            c->retry_group = seen_ks ? key_group : 0;
            c->suite = s;
            c->version = 0x0304;                                     /* a dummy ChangeCipherSpec may follow now */
            /* RFC 8446 4.4.1: the first ClientHello is replaced in the transcript by a message_hash message */
            u8 h[64], hdr[4];
            size_t hl = tlsc_hash_len(s->hash);
            transcript_hash(c, s->hash, h);
            tlsc_hash_init(&c->th256, TLSC_SHA256);
            tlsc_hash_init(&c->th384, TLSC_SHA384);
            hdr[0] = HS_MESSAGE_HASH; hdr[1] = 0; hdr[2] = 0; hdr[3] = (u8)hl;
            transcript_add(c, hdr, 4);
            transcript_add(c, h, hl);
            transcript_add(c, msg, len);
            return 1;
        }
        if (!seen_ks) return tls_fail(c, TLS_E_PROTOCOL, AL_MISSING_EXTENSION, "the ServerHello has no key share");
        if (c->hello_retry && s != c->suite) return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the cipher suite changed after the HelloRetryRequest");
        if ((c->hello_retry && c->retry_group) ? key_group != c->retry_group : !offered(c->offer.shares, c->offer.nshares, key_group))
            return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server's key share is for group 0x%04x, for which none was sent", key_group);
        if (!share_len(key_group) || key_len != share_len(key_group) || !key_data)
            return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "a key share of the wrong size");
        c->sh_group = key_group;
        memcpy(c->sh_share, key_data, key_len);
        c->sh_share_len = key_len;
        c->suite = s;
        c->version = 0x0304;
        transcript_add(c, msg, len);
        return 0;
    }

    /* TLS 1.2 */
    if (sidlen && sidlen == c->offer.sid_len && memcmp(sid, c->offer.sid, sidlen) == 0)
        return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server tries to resume a session that does not exist");
    if (alpn[0]) {
        int ok = 0;
        for (int i = 0; i < c->offer.nalpn; i++) ok |= strcmp(alpn, c->offer.alpn[i]) == 0;
        if (!ok) return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server picked application protocol '%s', which was not offered", alpn);
        memcpy(c->info.alpn, alpn, sizeof c->info.alpn);
        c->alpn_from_sh = 1;
    }
    c->version = 0x0303;
    c->suite = s;
    c->ems = ems;                                                    /* without it (old servers) the classic master secret is used; see docs/TLS.md */
    memcpy(c->server_random, random, 32);
    transcript_add(c, msg, len);
    return 0;
}
