#include <crypto.h>
#include <string.h>
#include <stdio.h>
#include <gensokyo.h>

/* ------------------------------------------------------------------ SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, init, sizeof init);
    c->len = 0;
    c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha256_update(c, &pad, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenbuf, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = (uint8_t)(c->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8); out[i * 4 + 3] = (uint8_t)c->h[i]; }
}

/* ------------------------------------------------------------------ MD5 */

static const uint32_t K5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static void md5_block(md5_ctx *c, const uint8_t *p) {
    static const uint8_t S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t tmp = d;
        d = cc; cc = b;
        uint32_t x = a + f + K5[i] + m[g];
        b = b + ((x << S[i]) | (x >> (32 - S[i])));
        a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
}

void md5_init(md5_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe; c->h[3] = 0x10325476;
    c->len = 0;
    c->n = 0;
}

void md5_update(md5_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; n -= take;
        if (c->n == 64) { md5_block(c, c->buf); c->n = 0; }
    }
}

void md5_final(md5_ctx *c, uint8_t out[16]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    md5_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) md5_update(c, &pad, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, lenbuf, 8);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(c->h[i] >> (8 * j));
}

/* ------------------------------------------------------------------ CRC-32 (the zlib/PNG polynomial) */

uint32_t crc32_update(uint32_t crc, const void *data, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------ passwords */

#define HASH_ROUNDS 3000

static void to_hex(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = hex[in[i] >> 4]; out[i * 2 + 1] = hex[in[i] & 15]; }
    out[n * 2] = 0;
}

void gap_hash_password(const char *password, const char *salt, char *out, size_t outsz) {
    char s[24];
    if (!strncmp(salt, "$gap$", 5)) salt += 5;
    size_t sl = strcspn(salt, "$");
    if (sl > sizeof s - 1) sl = sizeof s - 1;
    memcpy(s, salt, sl);
    s[sl] = 0;
    uint8_t digest[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, s, sl);
    sha256_update(&c, password, strlen(password));
    sha256_final(&c, digest);
    for (int i = 0; i < HASH_ROUNDS; i++) {
        sha256_init(&c);
        sha256_update(&c, digest, 32);
        sha256_update(&c, s, sl);
        sha256_update(&c, password, strlen(password));
        sha256_final(&c, digest);
    }
    char hex[65];
    to_hex(digest, 32, hex);
    snprintf(out, outsz, "$gap$%s$%s", s, hex);
}

int gap_check_password(const char *password, const char *stored) {
    if (!stored || !*stored) return 0;
    if (!strncmp(stored, "$gap$", 5)) {
        char out[128];
        gap_hash_password(password, stored, out, sizeof out);
        return strcmp(out, stored) == 0;
    }
    return 0;                                   /* "x", "*" and "!" mean no password login */
}

void gap_make_salt(char *out, size_t n) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[16];
    if (n > 17) n = 17;
    gap_getrandom(r, n - 1);
    for (size_t i = 0; i + 1 < n; i++) out[i] = alphabet[r[i] % 62];
    out[n - 1] = 0;
}
