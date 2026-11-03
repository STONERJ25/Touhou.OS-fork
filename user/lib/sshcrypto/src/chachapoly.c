#include <sshcrypto.h>
#include <string.h>

/* ---------------------------------------------------------------- ChaCha20 (64-bit nonce, 64-bit counter) */

static inline uint32_t rol32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#define QR(a, b, c, d) do { a += b; d ^= a; d = rol32(d, 16); c += d; b ^= c; b = rol32(b, 12); a += b; d ^= a; d = rol32(d, 8); c += d; b ^= c; b = rol32(b, 7); } while (0)

static void chacha_block(uint8_t out[64], const uint32_t in[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + in[i]);
}

void chacha20_xor(uint8_t *out, const uint8_t *in, size_t n, const uint8_t key[32], const uint8_t nonce[8], uint64_t counter) {
    uint32_t st[16] = { 0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u };
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + 4 * i);
    st[14] = le32(nonce);
    st[15] = le32(nonce + 4);
    uint8_t ks[64];
    while (n) {
        st[12] = (uint32_t)counter;
        st[13] = (uint32_t)(counter >> 32);
        chacha_block(ks, st);
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++) out[i] = in[i] ^ ks[i];
        in += take;
        out += take;
        n -= take;
        counter++;
    }
    memset(ks, 0, sizeof ks);
}

/* ---------------------------------------------------------------- Poly1305 (26-bit limbs, after Andrew Moon's public-domain poly1305-donna) */

void poly1305_mac(uint8_t tag[16], const uint8_t *m, size_t bytes, const uint8_t key[32]) {
    uint32_t r0 = le32(key + 0) & 0x3ffffff;
    uint32_t r1 = (le32(key + 3) >> 2) & 0x3ffff03;
    uint32_t r2 = (le32(key + 6) >> 4) & 0x3ffc0ff;
    uint32_t r3 = (le32(key + 9) >> 6) & 0x3f03fff;
    uint32_t r4 = (le32(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (bytes > 0) {
        uint8_t blk[16];
        uint32_t hibit = 1u << 24;
        size_t take = bytes >= 16 ? 16 : bytes;
        memset(blk, 0, sizeof blk);
        memcpy(blk, m, take);
        if (take < 16) { blk[take] = 1; hibit = 0; }
        h0 += le32(blk + 0) & 0x3ffffff;
        h1 += (le32(blk + 3) >> 2) & 0x3ffffff;
        h2 += (le32(blk + 6) >> 4) & 0x3ffffff;
        h3 += (le32(blk + 9) >> 6) & 0x3ffffff;
        h4 += (le32(blk + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += take;
        bytes -= take;
    }
    /* a full carry, then h - p if h >= p */
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;                       /* all ones when h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    /* h mod 2^128, then + the pad (the second half of the key) */
    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));
    uint64_t f = (uint64_t)h0 + le32(key + 16); put_le32(tag + 0, (uint32_t)f);
    f = (uint64_t)h1 + le32(key + 20) + (f >> 32); put_le32(tag + 4, (uint32_t)f);
    f = (uint64_t)h2 + le32(key + 24) + (f >> 32); put_le32(tag + 8, (uint32_t)f);
    f = (uint64_t)h3 + le32(key + 28) + (f >> 32); put_le32(tag + 12, (uint32_t)f);
}

int ct_memcmp(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d;
}
