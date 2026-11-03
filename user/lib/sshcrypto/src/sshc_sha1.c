/*
 * SHA-1 (FIPS 180-4) and HMAC (RFC 2104), written for one purpose: known_hosts files that were written with HashKnownHosts store each name as
 * "|1|salt|HMAC-SHA1(salt, name)", so the client needs SHA-1 to find itself in them. It is not used to sign or to authenticate anything.
 * The SHA-256 wrapper is here as well because libgap's has a context API and the callers want a one-shot.
 */
#include <sshc.h>
#include <string.h>
#include <crypto.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(SshcSha1 *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                      k = 0xCA62C1D6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sshc_sha1_init(SshcSha1 *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->n = 0;
}

void sshc_sha1_update(SshcSha1 *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { block(c, c->buf); c->n = 0; }
    }
}

void sshc_sha1_final(SshcSha1 *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sshc_sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sshc_sha1_update(c, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sshc_sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}

void sshc_hmac_sha1(uint8_t out[20], const void *key, size_t klen, const void *data, size_t n) {
    uint8_t k[64], pad[64], inner[20];
    memset(k, 0, sizeof k);
    if (klen > sizeof k) {                                   /* a key longer than the block is hashed first */
        SshcSha1 h;
        sshc_sha1_init(&h);
        sshc_sha1_update(&h, key, klen);
        sshc_sha1_final(&h, k);
    } else {
        memcpy(k, key, klen);
    }
    SshcSha1 h;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sshc_sha1_init(&h);
    sshc_sha1_update(&h, pad, 64);
    sshc_sha1_update(&h, data, n);
    sshc_sha1_final(&h, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sshc_sha1_init(&h);
    sshc_sha1_update(&h, pad, 64);
    sshc_sha1_update(&h, inner, 20);
    sshc_sha1_final(&h, out);
    memset(k, 0, sizeof k);
}

void sshc_sha256(uint8_t out[32], const void *data, size_t n) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, out);
}

void sshc_md5(uint8_t out[16], const void *data, size_t n) {
    md5_ctx c;
    md5_init(&c);
    md5_update(&c, data, n);
    md5_final(&c, out);
}
