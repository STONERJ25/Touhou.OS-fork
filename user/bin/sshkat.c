/* sshkat: known-answer tests of the SSH crypto library (user/lib/sshcrypto) inside the OS. The vectors (sshkat_vectors.h) were made with an independent
 * library; tests/case_sshcrypto.py runs this and wants the last line "SSHKAT PASS". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gensokyo.h>
#include <sshcrypto.h>

#include "sshkat_vectors.h"

static int g_fail, g_checks;
static uint8_t g_big[8192], g_big2[8192];

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = strlen(s) / 2;
    if (n > cap) { printf("sshkat: vector longer than %zu bytes\n", cap); exit(2); }
    for (size_t i = 0; i < n; i++) {
        int hi = s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10, lo = s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return n;
}

static void check(int ok, const char *what, int index) {
    g_checks++;
    if (!ok) { g_fail++; printf("sshkat: FAIL %s #%d\n", what, index); }
}

static int same_hex(const uint8_t *got, size_t n, const char *want) {
    uint8_t w[8192];
    return unhex(want, w, sizeof w) == n && !memcmp(got, w, n);
}

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* getrandom(2) hands out at most 4096 bytes per call. */
static int rng_fill(uint8_t *p, size_t n) {
    while (n) {
        size_t c = n < 4096 ? n : 4096;
        int got = gap_getrandom(p, c);
        if (got <= 0) return -1;
        p += got;
        n -= (size_t)got;
    }
    return 0;
}

/* The kernel's generator is not something known answers can test; these are the cheap statistical sanity checks that catch a broken one (stuck, repeating,
 * biased, the same after a restart of the counter): bit balance, byte histogram, no repeated blocks, two reads that differ, short and odd-sized reads. */
static void rng_checks(void) {
    enum { N = 1 << 16 };
    static uint8_t a[N], b[N];
    check(rng_fill(a, N) == 0 && rng_fill(b, N) == 0, "getrandom works", 0);
    check(memcmp(a, b, N) != 0, "two reads differ", 0);
    long ones = 0, hist[256] = { 0 };
    for (int i = 0; i < N; i++) {
        ones += __builtin_popcount(a[i]);
        hist[a[i]]++;
    }
    long expect = (long)N * 4, dev = ones > expect ? ones - expect : expect - ones;
    check(dev < 1600, "bit balance (4 sigma is 1450)", (int)dev);               /* sigma = sqrt(N * 8 / 4) = 362 */
    long chi = 0;                                                                /* the expected count per byte value is N / 256 = 256 */
    for (int i = 0; i < 256; i++) chi += (hist[i] - 256) * (hist[i] - 256);
    chi /= 256;
    check(chi > 170 && chi < 360, "byte histogram (chi-square about 255)", (int)chi);
    int repeats = 0;
    for (int i = 0; i < 512; i++)
        for (int j = i + 1; j < 512; j++)
            if (!memcmp(a + 16 * i, a + 16 * j, 16)) repeats++;
    check(repeats == 0, "no repeated 16-byte blocks", repeats);
    int stuck = 0;
    for (int i = 1; i < N; i++) stuck += a[i] == a[i - 1];
    check(stuck < 400, "no runs of equal bytes (expect 256)", stuck);
    uint8_t odd[33];
    int same = 0;
    for (int round = 0; round < 8; round++) {
        uint8_t prev[33];
        memcpy(prev, odd, sizeof prev);
        memset(odd, 0, sizeof odd);
        check(gap_getrandom(odd, 33) == 33, "a 33-byte read", round);
        if (round && !memcmp(prev, odd, 33)) same++;
    }
    check(same == 0, "short reads differ", same);
    uint8_t tiny[1];
    int seen[256] = { 0 }, distinct = 0;
    for (int i = 0; i < 400; i++) {
        if (gap_getrandom(tiny, 1) != 1) { check(0, "a 1-byte read", i); break; }
        if (!seen[tiny[0]]++) distinct++;
    }
    check(distinct > 150, "1-byte reads vary (expect about 200 of 256)", distinct);
}

int main(void) {
    for (int i = 0; i < COUNT(sha512_v); i++) {
        size_t n = unhex(sha512_v[i].msg, g_big, sizeof g_big);
        sha512_ctx c;
        uint8_t d[64];
        sha512_init(&c);
        sha512_update(&c, g_big, n / 3);                      /* three pieces: the buffering is tested too */
        sha512_update(&c, g_big + n / 3, n / 2 - n / 3);
        sha512_update(&c, g_big + n / 2, n - n / 2);
        sha512_final(&c, d);
        check(same_hex(d, 64, sha512_v[i].digest), "sha512", i);
    }
    for (int i = 0; i < COUNT(x25519_v); i++) {
        uint8_t s[32], p[32], o[32];
        unhex(x25519_v[i].scalar, s, 32);
        unhex(x25519_v[i].point, p, 32);
        check(x25519(o, s, p) == 0 && same_hex(o, 32, x25519_v[i].out), "x25519", i);
    }
    for (int i = 0; i < COUNT(x25519_base_v); i++) {
        uint8_t s[32], o[32];
        unhex(x25519_base_v[i].scalar, s, 32);
        x25519_base(o, s);
        check(same_hex(o, 32, x25519_base_v[i].pub), "x25519_base", i);
    }
    {
        uint8_t s[32], zero[32] = { 0 }, o[32];
        memset(s, 0x42, sizeof s);
        check(x25519(o, s, zero) != 0, "x25519 refuses a point of small order", 0);
    }
    for (int i = 0; i < COUNT(ed25519_v); i++) {
        uint8_t seed[32], pk[32], sig[64], pkw[32], sigw[64];
        unhex(ed25519_v[i].seed, seed, 32);
        unhex(ed25519_v[i].pk, pkw, 32);
        unhex(ed25519_v[i].sig, sigw, 64);
        size_t n = unhex(ed25519_v[i].msg, g_big, sizeof g_big);
        ed25519_pubkey(pk, seed);
        check(!memcmp(pk, pkw, 32), "ed25519 public key", i);
        ed25519_sign(sig, g_big, n, seed, pk);
        check(!memcmp(sig, sigw, 64), "ed25519 signature", i);
        check(ed25519_verify(sigw, g_big, n, pkw) == 0, "ed25519 verify", i);
        uint8_t bad[64];
        memcpy(bad, sigw, 64);
        bad[(i * 7) % 64] ^= (uint8_t)(1 << (i % 8));
        check(ed25519_verify(bad, g_big, n, pkw) != 0, "ed25519 refuses a corrupted signature", i);
        if (n) {
            g_big[n - 1] ^= 1;
            check(ed25519_verify(sigw, g_big, n, pkw) != 0, "ed25519 refuses another message", i);
        }
    }
    {
        uint8_t pk[32], sig[64];
        size_t n = unhex(ed25519_twin.msg, g_big, sizeof g_big);
        unhex(ed25519_twin.pk, pk, 32);
        unhex(ed25519_twin.sig, sig, 64);
        check(ed25519_verify(sig, g_big, n, pk) != 0, "ed25519 refuses S + L", 0);
    }
    for (int i = 0; i < COUNT(chacha_v); i++) {
        uint8_t key[32], nonce[8];
        unhex(chacha_v[i].key, key, 32);
        unhex(chacha_v[i].nonce, nonce, 8);
        size_t n = unhex(chacha_v[i].in, g_big, sizeof g_big);
        chacha20_xor(g_big2, g_big, n, key, nonce, chacha_v[i].counter);
        check(same_hex(g_big2, n, chacha_v[i].out), "chacha20", i);
        chacha20_xor(g_big2, g_big2, n, key, nonce, chacha_v[i].counter);          /* in place, and back again */
        check(!memcmp(g_big2, g_big, n), "chacha20 round trip", i);
    }
    for (int i = 0; i < COUNT(poly_v); i++) {
        uint8_t key[32], tag[16];
        unhex(poly_v[i].key, key, 32);
        size_t n = unhex(poly_v[i].msg, g_big, sizeof g_big);
        poly1305_mac(tag, g_big, n, key);
        check(same_hex(tag, 16, poly_v[i].tag), "poly1305", i);
    }
    rng_checks();
    printf("sshkat: %d checks, %d failed\n", g_checks, g_fail);
    printf(g_fail ? "SSHKAT FAIL\n" : "SSHKAT PASS\n");
    return g_fail ? 1 : 0;
}
