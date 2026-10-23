/* ChaCha20, Poly1305 and the combination of the two (RFC 8439). Constant time: no table lookups, no secret-dependent branches. */
#include "tlsint.h"

static inline u32 rotl32(u32 x, int k) { return (x << k) | (x >> (32 - k)); }
static inline u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline void put_le32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

#define QR(a, b, c, d) do {                                                                              \
        a += b; d ^= a; d = rotl32(d, 16); c += d; b ^= c; b = rotl32(b, 12);                             \
        a += b; d ^= a; d = rotl32(d, 8);  c += d; b ^= c; b = rotl32(b, 7);                              \
    } while (0)

static void chacha_block(u8 out[64], const u32 s[16]) {
    u32 x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
}

void tlsc_chacha20_xor(const u8 key[32], const u8 nonce[12], u32 counter, const u8 *in, u8 *out, size_t n) {
    u32 s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    u8 blk[64];
    while (n) {
        chacha_block(blk, s);
        s[12]++;
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++) out[i] = in[i] ^ blk[i];
        in += take;
        out += take;
        n -= take;
    }
    tlsc_wipe(blk, sizeof blk);
    tlsc_wipe(s, sizeof s);
}

/* ---------------------------------------------------------------- Poly1305 (26-bit limbs) */

typedef struct { u32 r[5], h[5], pad[4]; u8 buf[16]; size_t n; } poly_ctx;

static void poly_init(poly_ctx *p, const u8 key[32]) {
    p->r[0] = le32(key) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
    p->n = 0;
}

static void poly_blocks(poly_ctx *p, const u8 *m, size_t n, u32 hibit) {
    const u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (n >= 16) {
        h0 += le32(m) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
        u32 c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff;
        d1 += c; c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff;
        d2 += c; c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff;
        d3 += c; c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff;
        d4 += c; c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        n -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(poly_ctx *p, const u8 *m, size_t n) {
    if (p->n) {
        size_t take = 16 - p->n < n ? 16 - p->n : n;
        memcpy(p->buf + p->n, m, take);
        p->n += take; m += take; n -= take;
        if (p->n < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->n = 0;
    }
    if (n >= 16) {
        size_t whole = n & ~(size_t)15;
        poly_blocks(p, m, whole, 1u << 24);
        m += whole;
        n -= whole;
    }
    if (n) { memcpy(p->buf, m, n); p->n = n; }
}

static void poly_final(poly_ctx *p, u8 tag[16]) {
    if (p->n) {
        p->buf[p->n++] = 1;
        while (p->n < 16) p->buf[p->n++] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1;                                       /* all ones if h >= p (no borrow), else 0 */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));
    u64 f = (u64)h0 + p->pad[0]; put_le32(tag, (u32)f);
    f = (u64)h1 + p->pad[1] + (f >> 32); put_le32(tag + 4, (u32)f);
    f = (u64)h2 + p->pad[2] + (f >> 32); put_le32(tag + 8, (u32)f);
    f = (u64)h3 + p->pad[3] + (f >> 32); put_le32(tag + 12, (u32)f);
    tlsc_wipe(p, sizeof *p);
}

void tlsc_poly1305(const u8 key[32], const u8 *data, size_t n, u8 tag[16]) {
    poly_ctx p;
    poly_init(&p, key);
    poly_update(&p, data, n);
    poly_final(&p, tag);
}

/* ---------------------------------------------------------------- the AEAD */

static void aead_tag(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aadlen, const u8 *ct, size_t n, u8 tag[16]) {
    u8 otk[64], zeros[64];
    memset(zeros, 0, sizeof zeros);
    tlsc_chacha20_xor(key, nonce, 0, zeros, otk, 64);                /* the first block's first 32 bytes are the one-time Poly1305 key */
    poly_ctx p;
    poly_init(&p, otk);
    static const u8 pad[16] = { 0 };
    poly_update(&p, aad, aadlen);
    if (aadlen % 16) poly_update(&p, pad, 16 - aadlen % 16);
    poly_update(&p, ct, n);
    if (n % 16) poly_update(&p, pad, 16 - n % 16);
    u8 lens[16];
    for (int i = 0; i < 8; i++) { lens[i] = (u8)((u64)aadlen >> (8 * i)); lens[8 + i] = (u8)((u64)n >> (8 * i)); }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    tlsc_wipe(otk, sizeof otk);
}

void tlsc_chacha20poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, u8 tag[16]) {
    tlsc_chacha20_xor(key, nonce, 1, in, out, n);
    aead_tag(key, nonce, aad, aadlen, out, n, tag);
}

int tlsc_chacha20poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, const u8 tag[16]) {
    u8 mine[16];
    aead_tag(key, nonce, aad, aadlen, in, n, mine);
    if (!tlsc_ct_memeq(mine, tag, 16)) return -1;
    tlsc_chacha20_xor(key, nonce, 1, in, out, n);
    return 0;
}
