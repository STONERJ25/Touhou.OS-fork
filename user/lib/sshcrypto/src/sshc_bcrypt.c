/*
 * bcrypt_pbkdf: the password-based key derivation OpenSSH uses for passphrase-protected private keys (PROTOCOL.key: kdf "bcrypt", options = salt and number of
 * rounds). It is PBKDF2's shape (a counter, a number of rounds, the outputs XORed) with bcrypt's "hash" as the pseudo-random function, and one deviation: the
 * key bytes are spread over the output blocks (byte i of block c lands at i * stride + c) instead of being concatenated.
 *
 * Written from the algorithm as OpenSSH documents it: the password and each salt are first run through SHA-512 (so any length works); bcrypt's "hash" sets up a
 * Blowfish state from the two digests with the expensive key schedule (64 extra rounds of mixing in the salt and the password alternately), then encrypts the
 * fixed text "OxychromaticBlowfishSwatDynamite" 64 times in ECB mode, and the result is written out with each 32-bit word byte-swapped.
 * Blowfish (Schneier, 1993): 16 Feistel rounds over a P-array and four S-boxes whose initial values are the digits of pi (sshc_blowfish_tab.h; the test
 * recomputes them from pi and compares). The result is checked against an independent implementation (the Python bcrypt package's kdf()).
 */
#include <sshc.h>
#include <string.h>
#include <sshcrypto.h>
#include "sshc_blowfish_tab.h"

typedef struct { uint32_t P[18]; uint32_t S[4][256]; } Blowfish;

static void bf_init(Blowfish *c) {
    memcpy(c->P, BF_INIT_P, sizeof c->P);
    memcpy(c->S, BF_INIT_S, sizeof c->S);
}

static inline uint32_t bf_f(const Blowfish *c, uint32_t x) {
    return ((c->S[0][x >> 24] + c->S[1][(x >> 16) & 255]) ^ c->S[2][(x >> 8) & 255]) + c->S[3][x & 255];
}

static void bf_encipher(const Blowfish *c, uint32_t *xl, uint32_t *xr) {
    uint32_t l = *xl, r = *xr, t;
    for (int i = 0; i < 16; i++) {
        l ^= c->P[i];
        r ^= bf_f(c, l);
        t = l; l = r; r = t;
    }
    t = l; l = r; r = t;                                    /* undo the last swap */
    r ^= c->P[16];
    l ^= c->P[17];
    *xl = l;
    *xr = r;
}

/* The next four bytes of `data` as a big-endian word, wrapping around at its end. */
static uint32_t stream2word(const uint8_t *data, size_t len, size_t *pos) {
    uint32_t w = 0;
    size_t j = *pos;
    for (int i = 0; i < 4; i++, j++) {
        if (j >= len) j = 0;
        w = w << 8 | data[j];
    }
    *pos = j;
    return w;
}

/* Blowfish's key schedule with the salt mixed into every block that is encrypted (bcrypt's "EksBlowfishSetup", first step). */
static void bf_expand_state(Blowfish *c, const uint8_t *salt, size_t slen, const uint8_t *key, size_t klen) {
    size_t j = 0;
    for (int i = 0; i < 18; i++) c->P[i] ^= stream2word(key, klen, &j);
    j = 0;
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        l ^= stream2word(salt, slen, &j);
        r ^= stream2word(salt, slen, &j);
        bf_encipher(c, &l, &r);
        c->P[i] = l;
        c->P[i + 1] = r;
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 256; k += 2) {
            l ^= stream2word(salt, slen, &j);
            r ^= stream2word(salt, slen, &j);
            bf_encipher(c, &l, &r);
            c->S[i][k] = l;
            c->S[i][k + 1] = r;
        }
}

/* The plain key schedule (no salt): the "expand0state" of the 64 extra rounds. */
static void bf_expand0_state(Blowfish *c, const uint8_t *key, size_t klen) {
    size_t j = 0;
    for (int i = 0; i < 18; i++) c->P[i] ^= stream2word(key, klen, &j);
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
        bf_encipher(c, &l, &r);
        c->P[i] = l;
        c->P[i + 1] = r;
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 256; k += 2) {
            bf_encipher(c, &l, &r);
            c->S[i][k] = l;
            c->S[i][k + 1] = r;
        }
}

static void bcrypt_hash(const uint8_t sha2pass[64], const uint8_t sha2salt[64], uint8_t out[32]) {
    static const char text[] = "OxychromaticBlowfishSwatDynamite";            /* 32 characters and a NUL we do not use */
    static Blowfish state;                                  /* 4 KB: static, so that a small stack is no problem */
    uint32_t cdata[8];
    bf_init(&state);
    bf_expand_state(&state, sha2salt, 64, sha2pass, 64);
    for (int i = 0; i < 64; i++) {
        bf_expand0_state(&state, sha2salt, 64);
        bf_expand0_state(&state, sha2pass, 64);
    }
    size_t j = 0;
    for (int i = 0; i < 8; i++) cdata[i] = stream2word((const uint8_t *)text, sizeof text - 1, &j);
    for (int round = 0; round < 64; round++)
        for (int blk = 0; blk < 4; blk++) bf_encipher(&state, &cdata[2 * blk], &cdata[2 * blk + 1]);
    for (int i = 0; i < 8; i++) {                           /* written little-endian, unlike the cipher's own big-endian words */
        out[4 * i + 3] = (uint8_t)(cdata[i] >> 24);
        out[4 * i + 2] = (uint8_t)(cdata[i] >> 16);
        out[4 * i + 1] = (uint8_t)(cdata[i] >> 8);
        out[4 * i + 0] = (uint8_t)cdata[i];
    }
    memset(&state, 0, sizeof state);
}

static void sha512_of(uint8_t out[64], const uint8_t *a, size_t an) {
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, a, an);
    sha512_final(&c, out);
}

void sshc_bcrypt_pbkdf(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *key, size_t klen, unsigned rounds) {
    uint8_t sha2pass[64], sha2salt[64], out[32], tmp[32];
    uint8_t countsalt[64 + 4];
    if (rounds < 1 || klen == 0 || klen > sizeof out * sizeof out || slen > 64) { memset(key, 0, klen); return; }       /* callers pass sane values: 16 rounds, a 16-byte salt */
    size_t stride = (klen + sizeof out - 1) / sizeof out;
    size_t amt = (klen + stride - 1) / stride;
    size_t origlen = klen;
    sha512_of(sha2pass, pass, plen);
    memcpy(countsalt, salt, slen);
    for (uint32_t count = 1; klen > 0; count++) {
        countsalt[slen + 0] = (uint8_t)(count >> 24);
        countsalt[slen + 1] = (uint8_t)(count >> 16);
        countsalt[slen + 2] = (uint8_t)(count >> 8);
        countsalt[slen + 3] = (uint8_t)count;
        sha512_of(sha2salt, countsalt, slen + 4);
        bcrypt_hash(sha2pass, sha2salt, tmp);
        memcpy(out, tmp, sizeof out);
        for (unsigned i = 1; i < rounds; i++) {
            sha512_of(sha2salt, tmp, sizeof tmp);
            bcrypt_hash(sha2pass, sha2salt, tmp);
            for (size_t j = 0; j < sizeof out; j++) out[j] ^= tmp[j];
        }
        if (amt > klen) amt = klen;
        size_t i;
        for (i = 0; i < amt; i++) {
            size_t dest = i * stride + (count - 1);
            if (dest >= origlen) break;
            key[dest] = out[i];
        }
        klen -= i;
    }
    memset(sha2pass, 0, sizeof sha2pass);
    memset(sha2salt, 0, sizeof sha2salt);
    memset(out, 0, sizeof out);
    memset(tmp, 0, sizeof tmp);
}
