/* SHA-256, SHA-384, SHA-512, HMAC, HKDF and the TLS 1.2 PRF. */
#include "tlsint.h"
#include "tables.h"

int tlsc_ct_memeq(const void *a, const void *b, size_t n) {
    const volatile u8 *x = a, *y = b;
    u8 d = 0;
    for (size_t i = 0; i < n; i++) d |= (u8)(x[i] ^ y[i]);
    return d == 0;
}

void tlsc_wipe(void *p, size_t n) {
    volatile u8 *v = p;
    while (n--) *v++ = 0;
}

/* ---------------------------------------------------------------- SHA-256 */

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha256_block(tlsc_sha256_ctx *c, const u8 *p) {
    u32 w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + TLSC_K256[i] + w[i];
        u32 t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(tlsc_sha256_ctx *c) {
    memcpy(c->h, TLSC_H256, sizeof c->h);
    c->len = 0;
    c->n = 0;
}

static void sha256_update(tlsc_sha256_ctx *c, const u8 *p, size_t n) {
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(tlsc_sha256_ctx *c, u8 *out) {
    u64 bits = c->len * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) { memset(c->buf + c->n, 0, 64 - c->n); sha256_block(c, c->buf); c->n = 0; }
    memset(c->buf + c->n, 0, 56 - c->n);
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (u8)(bits >> (56 - 8 * i));
    sha256_block(c, c->buf);
    for (int i = 0; i < 8; i++) { out[i * 4] = (u8)(c->h[i] >> 24); out[i * 4 + 1] = (u8)(c->h[i] >> 16); out[i * 4 + 2] = (u8)(c->h[i] >> 8); out[i * 4 + 3] = (u8)c->h[i]; }
}

/* ---------------------------------------------------------------- SHA-512 / SHA-384 */

static void sha512_block(tlsc_sha512_ctx *c, const u8 *p) {
    u64 w[80], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        u64 v = 0;
        for (int k = 0; k < 8; k++) v = v << 8 | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        u64 s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = h + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + TLSC_K512[i] + w[i];
        u64 t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha512_init(tlsc_sha512_ctx *c, const u64 *iv) {
    memcpy(c->h, iv, sizeof c->h);
    c->len = 0;
    c->n = 0;
}

static void sha512_update(tlsc_sha512_ctx *c, const u8 *p, size_t n) {
    c->len += n;
    while (n) {
        size_t take = 128 - c->n < n ? 128 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}

static void sha512_final(tlsc_sha512_ctx *c, u8 *out, size_t outlen) {
    u64 bits = c->len * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 112) { memset(c->buf + c->n, 0, 128 - c->n); sha512_block(c, c->buf); c->n = 0; }
    memset(c->buf + c->n, 0, 120 - c->n);                          /* the upper 64 bits of the 128-bit length are zero */
    for (int i = 0; i < 8; i++) c->buf[120 + i] = (u8)(bits >> (56 - 8 * i));
    sha512_block(c, c->buf);
    u8 full[64];
    for (int i = 0; i < 8; i++) for (int k = 0; k < 8; k++) full[i * 8 + k] = (u8)(c->h[i] >> (56 - 8 * k));
    memcpy(out, full, outlen);
}

/* ---------------------------------------------------------------- the generic interface */

size_t tlsc_hash_len(int alg) { return alg == TLSC_SHA256 ? 32 : alg == TLSC_SHA384 ? 48 : alg == TLSC_SHA512 ? 64 : 0; }
size_t tlsc_hash_block(int alg) { return alg == TLSC_SHA256 ? 64 : 128; }

void tlsc_hash_init(tlsc_hash *h, int alg) {
    h->alg = alg;
    if (alg == TLSC_SHA256) sha256_init(&h->u.s256);
    else sha512_init(&h->u.s512, alg == TLSC_SHA384 ? TLSC_H384 : TLSC_H512);
}

void tlsc_hash_update(tlsc_hash *h, const void *data, size_t n) {
    if (h->alg == TLSC_SHA256) sha256_update(&h->u.s256, data, n);
    else sha512_update(&h->u.s512, data, n);
}

void tlsc_hash_final(tlsc_hash *h, u8 *out) {
    if (h->alg == TLSC_SHA256) sha256_final(&h->u.s256, out);
    else sha512_final(&h->u.s512, out, h->alg == TLSC_SHA384 ? 48 : 64);
}

void tlsc_hash_once(int alg, const void *data, size_t n, u8 *out) {
    tlsc_hash h;
    tlsc_hash_init(&h, alg);
    tlsc_hash_update(&h, data, n);
    tlsc_hash_final(&h, out);
}

/* ---------------------------------------------------------------- HMAC */

void tlsc_hmac_init(tlsc_hmac_ctx *c, int alg, const void *key, size_t keylen) {
    size_t block = tlsc_hash_block(alg);
    u8 k[128], pad[128];
    memset(k, 0, sizeof k);
    if (keylen > block) tlsc_hash_once(alg, key, keylen, k);
    else memcpy(k, key, keylen);
    for (size_t i = 0; i < block; i++) pad[i] = k[i] ^ 0x36;
    tlsc_hash_init(&c->inner, alg);
    tlsc_hash_update(&c->inner, pad, block);
    for (size_t i = 0; i < block; i++) pad[i] = k[i] ^ 0x5c;
    tlsc_hash_init(&c->outer, alg);
    tlsc_hash_update(&c->outer, pad, block);
    tlsc_wipe(k, sizeof k);
    tlsc_wipe(pad, sizeof pad);
}

void tlsc_hmac_update(tlsc_hmac_ctx *c, const void *data, size_t n) { tlsc_hash_update(&c->inner, data, n); }

void tlsc_hmac_final(tlsc_hmac_ctx *c, u8 *out) {
    u8 ih[64];
    tlsc_hash_final(&c->inner, ih);
    tlsc_hash_update(&c->outer, ih, tlsc_hash_len(c->outer.alg));
    tlsc_hash_final(&c->outer, out);
}

void tlsc_hmac(int alg, const void *key, size_t keylen, const void *data, size_t n, u8 *out) {
    tlsc_hmac_ctx c;
    tlsc_hmac_init(&c, alg, key, keylen);
    tlsc_hmac_update(&c, data, n);
    tlsc_hmac_final(&c, out);
}

/* ---------------------------------------------------------------- HKDF and the TLS PRF */

void tlsc_hkdf_extract(int alg, const void *salt, size_t saltlen, const void *ikm, size_t ikmlen, u8 *prk) {
    u8 zeros[64];
    if (!salt || !saltlen) { memset(zeros, 0, sizeof zeros); salt = zeros; saltlen = tlsc_hash_len(alg); }
    tlsc_hmac(alg, salt, saltlen, ikm, ikmlen, prk);
}

int tlsc_hkdf_expand(int alg, const u8 *prk, size_t prklen, const void *info, size_t infolen, u8 *out, size_t outlen) {
    size_t hl = tlsc_hash_len(alg);
    if (outlen > 255 * hl) return -1;
    u8 t[64];
    size_t tlen = 0;
    for (unsigned i = 1; outlen; i++) {
        tlsc_hmac_ctx c;
        tlsc_hmac_init(&c, alg, prk, prklen);
        tlsc_hmac_update(&c, t, tlen);
        tlsc_hmac_update(&c, info, infolen);
        u8 ctr = (u8)i;
        tlsc_hmac_update(&c, &ctr, 1);
        tlsc_hmac_final(&c, t);
        tlen = hl;
        size_t take = outlen < hl ? outlen : hl;
        memcpy(out, t, take);
        out += take;
        outlen -= take;
    }
    tlsc_wipe(t, sizeof t);
    return 0;
}

int tlsc_hkdf_expand_label(int alg, const u8 *secret, const char *label, const u8 *ctx, size_t ctxlen, u8 *out, size_t outlen) {
    u8 info[2 + 1 + 6 + 255 + 1 + 255];
    size_t ll = strlen(label);
    if (ll > 249 || ctxlen > 255 || outlen > 0xFFFF) return -1;
    size_t p = 0;
    info[p++] = (u8)(outlen >> 8);
    info[p++] = (u8)outlen;
    info[p++] = (u8)(6 + ll);
    memcpy(info + p, "tls13 ", 6); p += 6;
    memcpy(info + p, label, ll); p += ll;
    info[p++] = (u8)ctxlen;
    if (ctxlen) { memcpy(info + p, ctx, ctxlen); p += ctxlen; }
    return tlsc_hkdf_expand(alg, secret, tlsc_hash_len(alg), info, p, out, outlen);
}

void tlsc_prf(int alg, const u8 *secret, size_t secretlen, const char *label, const u8 *seed1, size_t seed1len, const u8 *seed2, size_t seed2len,
              u8 *out, size_t outlen) {
    /* P_hash(secret, label || seed1 || seed2): A(0) = seed, A(i) = HMAC(secret, A(i-1)); output HMAC(secret, A(i) || seed) */
    u8 seed[256], a[64];
    size_t ll = strlen(label), sl = 0;
    if (ll + seed1len + seed2len > sizeof seed) return;                            /* callers stay far below this */
    memcpy(seed, label, ll); sl = ll;
    memcpy(seed + sl, seed1, seed1len); sl += seed1len;
    if (seed2len) { memcpy(seed + sl, seed2, seed2len); sl += seed2len; }
    size_t hl = tlsc_hash_len(alg);
    tlsc_hmac(alg, secret, secretlen, seed, sl, a);
    while (outlen) {
        tlsc_hmac_ctx c;
        u8 blk[64];
        tlsc_hmac_init(&c, alg, secret, secretlen);
        tlsc_hmac_update(&c, a, hl);
        tlsc_hmac_update(&c, seed, sl);
        tlsc_hmac_final(&c, blk);
        size_t take = outlen < hl ? outlen : hl;
        memcpy(out, blk, take);
        out += take;
        outlen -= take;
        tlsc_hmac(alg, secret, secretlen, a, hl, a);
    }
    tlsc_wipe(a, sizeof a);
}
