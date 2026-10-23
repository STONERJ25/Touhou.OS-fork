/* bit twiddling: CRC32, SHA-256, base64, xorshift, FNV, run-length coding */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[128];
    size_t total = len + 1 + 8;
    size_t padded = (total + 63) / 64 * 64;
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    (void)block;
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)buf[off + i * 4] << 24 | (uint32_t)buf[off + i * 4 + 1] << 16 | (uint32_t)buf[off + i * 4 + 2] << 8 | buf[off + i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static void print_hex(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int base64_decode(const char *in, uint8_t *out) {
    int o = 0, bits = 0;
    uint32_t acc = 0;
    for (; *in && *in != '='; in++) {
        const char *p = strchr(b64, *in);
        if (!p) continue;
        acc = acc << 6 | (uint32_t)(p - b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

static uint64_t xs = 88172645463325252ULL;
static uint64_t xorshift(void) {
    xs ^= xs << 13;
    xs ^= xs >> 7;
    xs ^= xs << 17;
    return xs;
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static int rle(const uint8_t *in, int n, uint8_t *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && in[j] == in[i] && j - i < 255) j++;
        out[o++] = (uint8_t)(j - i);
        out[o++] = in[i];
        i = j;
    }
    return o;
}

static int unrle(const uint8_t *in, int n, uint8_t *out) {
    int o = 0;
    for (int i = 0; i + 1 < n; i += 2)
        for (int k = 0; k < in[i]; k++) out[o++] = in[i + 1];
    return o;
}

int main(void) {
    crc_init();
    printf("crc32(\"123456789\")=%08x crc32(\"\")=%08x\n", crc32("123456789", 9), crc32("", 0));
    uint8_t digest[32];
    sha256((const uint8_t *)"abc", 3, digest);
    print_hex(digest, 32);
    sha256((const uint8_t *)"", 0, digest);
    print_hex(digest, 32);
    sha256((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43, digest);
    print_hex(digest, 32);
    char enc[64];
    uint8_t dec[64];
    const char *samples[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    for (int i = 0; i < 7; i++) {
        base64_encode((const uint8_t *)samples[i], (int)strlen(samples[i]), enc);
        int n = base64_decode(enc, dec);
        dec[n] = 0;
        printf("[%s]->[%s]->[%s] ", samples[i], enc, dec);
    }
    printf("\n");
    uint64_t acc = 0;
    for (int i = 0; i < 1000; i++) acc ^= xorshift() + (uint64_t)i;
    printf("xorshift acc=%llx next=%llx\n", (unsigned long long)acc, (unsigned long long)xorshift());
    printf("fnv1a: %08x %08x\n", fnv1a("hello"), fnv1a("Gensokyo"));
    uint8_t data[] = "aaaabbbcccccccccd";
    uint8_t packed[64], unpacked[64];
    int pn = rle(data, 17, packed);
    int un = unrle(packed, pn, unpacked);
    unpacked[un] = 0;
    printf("rle: %d -> %d -> %d (%s)\n", 17, pn, un, (char *)unpacked);
    uint32_t x = 0x80000001u;
    printf("rotl/rotr: %08x %08x popcnt=", (x << 3) | (x >> 29), (x >> 5) | (x << 27));
    int pc = 0;
    for (uint32_t t = x; t; t &= t - 1) pc++;
    printf("%d\n", pc);
    int64_t s64 = -9223372036854775807LL - 1;
    printf("int64 min=%lld max=%lld uint64 max=%llu\n", (long long)s64, (long long)(s64 - 1), (unsigned long long)-1LL);
    return 0;
}
