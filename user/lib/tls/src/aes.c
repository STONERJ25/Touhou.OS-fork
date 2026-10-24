/*
 * AES (FIPS 197) and GCM (NIST SP 800-38D).
 *
 * Two implementations behind one interface:
 *
 *   - AES-NI and PCLMULQDQ, used when CPUID says both exist. These instructions touch no tables, so this path has no
 *     secret-dependent memory accesses or branches.
 *   - A portable one for CPUs (and emulators) without them. Its AES core reads a 256-byte S-box at secret-dependent
 *     indexes, so it is NOT safe against an attacker who can watch the cache of the same CPU. libtls therefore offers
 *     ChaCha20-Poly1305 first when the CPU has no AES-NI (see tls13.c). The portable GHASH is bit-serial and constant time.
 */
#include "tlsint.h"
#include "tables.h"

#if defined(__x86_64__)
#include <immintrin.h>
#define TLSC_HAVE_X86 1
#endif

static int g_force_sw;
static int g_hw_cache = -1;

#ifdef TLSC_HAVE_X86
static void cpuid1(u32 *ecx) {
    u32 a = 1, b, c = 0, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    *ecx = c;
}
#endif

int tlsc_aes_hw_available(void) {
    if (g_force_sw) return 0;
    if (g_hw_cache < 0) {
        g_hw_cache = 0;
#ifdef TLSC_HAVE_X86
        u32 ecx;
        cpuid1(&ecx);
        /* SSSE3 (bit 9), SSE4.1 (19), AES (25), PCLMULQDQ (1) */
        if ((ecx & (1u << 1)) && (ecx & (1u << 9)) && (ecx & (1u << 19)) && (ecx & (1u << 25))) g_hw_cache = 1;
#endif
    }
    return g_hw_cache;
}

void tlsc_aes_force_software(int on) { g_force_sw = on; }

/* ------------------------------------------------------------------------------------------------ portable AES */

static inline u8 xtime(u8 x) { return (u8)((x << 1) ^ ((u8)(0 - (x >> 7)) & 0x1b)); }     /* no branch */

static void sw_expand(tlsc_aes_key *k, const u8 *key, size_t keylen) {
    u8 *w = (u8 *)k->rk;
    size_t nk = keylen / 4, total = 4 * (size_t)(k->rounds + 1);
    memcpy(w, key, keylen);
    u8 rcon = 1;
    for (size_t i = nk; i < total; i++) {
        u8 t[4];
        memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {
            u8 t0 = t[0];
            t[0] = (u8)(TLSC_SBOX[t[1]] ^ rcon);
            t[1] = TLSC_SBOX[t[2]];
            t[2] = TLSC_SBOX[t[3]];
            t[3] = TLSC_SBOX[t0];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = TLSC_SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) w[4 * i + j] = (u8)(w[4 * (i - nk) + j] ^ t[j]);
    }
}

static void sw_encrypt(const tlsc_aes_key *k, const u8 in[16], u8 out[16]) {
    const u8 *rk = (const u8 *)k->rk;
    u8 s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= k->rounds; r++) {
        for (int c = 0; c < 4; c++)                                  /* SubBytes + ShiftRows */
            for (int row = 0; row < 4; row++) t[4 * c + row] = TLSC_SBOX[s[4 * ((c + row) & 3) + row]];
        if (r < k->rounds) {
            for (int c = 0; c < 4; c++) {                            /* MixColumns */
                u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                u8 x0 = xtime(a0), x1 = xtime(a1), x2 = xtime(a2), x3 = xtime(a3);
                s[4 * c]     = (u8)(x0 ^ x1 ^ a1 ^ a2 ^ a3);
                s[4 * c + 1] = (u8)(a0 ^ x1 ^ x2 ^ a2 ^ a3);
                s[4 * c + 2] = (u8)(a0 ^ a1 ^ x2 ^ x3 ^ a3);
                s[4 * c + 3] = (u8)(x0 ^ a0 ^ a1 ^ a2 ^ x3);
            }
        } else {
            memcpy(s, t, 16);
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * r + i];
    }
    memcpy(out, s, 16);
    tlsc_wipe(s, sizeof s);
    tlsc_wipe(t, sizeof t);
}

/* ------------------------------------------------------------------------------------------------ AES-NI */

#ifdef TLSC_HAVE_X86
#define HW __attribute__((target("aes,pclmul,ssse3,sse4.1")))

HW static inline __m128i ks_mix(__m128i k, __m128i gen) {
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    return _mm_xor_si128(k, gen);
}

HW static void hw_expand(tlsc_aes_key *key, const u8 *in, size_t keylen) {
    u32 *rk = key->rk;
    if (keylen == 16) {
        __m128i k = _mm_loadu_si128((const __m128i *)in);
        _mm_storeu_si128((__m128i *)rk, k);
#define S128(i, rc) do { k = ks_mix(k, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(k, rc), 0xff)); _mm_storeu_si128((__m128i *)(rk + 4 * (i)), k); } while (0)
        S128(1, 0x01); S128(2, 0x02); S128(3, 0x04); S128(4, 0x08); S128(5, 0x10);
        S128(6, 0x20); S128(7, 0x40); S128(8, 0x80); S128(9, 0x1b); S128(10, 0x36);
#undef S128
    } else {
        __m128i a = _mm_loadu_si128((const __m128i *)in), b = _mm_loadu_si128((const __m128i *)(in + 16));
        _mm_storeu_si128((__m128i *)rk, a);
        _mm_storeu_si128((__m128i *)(rk + 4), b);
#define EA(i, rc) do { a = ks_mix(a, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(b, rc), 0xff)); _mm_storeu_si128((__m128i *)(rk + 4 * (i)), a); } while (0)
#define EB(i)     do { b = ks_mix(b, _mm_shuffle_epi32(_mm_aeskeygenassist_si128(a, 0), 0xaa));  _mm_storeu_si128((__m128i *)(rk + 4 * (i)), b); } while (0)
        EA(2, 0x01); EB(3); EA(4, 0x02); EB(5); EA(6, 0x04); EB(7); EA(8, 0x08); EB(9);
        EA(10, 0x10); EB(11); EA(12, 0x20); EB(13); EA(14, 0x40);
#undef EA
#undef EB
    }
}

HW static void hw_encrypt(const tlsc_aes_key *k, const u8 in[16], u8 out[16]) {
    const u32 *rk = k->rk;
    __m128i s = _mm_xor_si128(_mm_loadu_si128((const __m128i *)in), _mm_loadu_si128((const __m128i *)rk));
    for (int r = 1; r < k->rounds; r++) s = _mm_aesenc_si128(s, _mm_loadu_si128((const __m128i *)(rk + 4 * r)));
    s = _mm_aesenclast_si128(s, _mm_loadu_si128((const __m128i *)(rk + 4 * k->rounds)));
    _mm_storeu_si128((__m128i *)out, s);
}

/* x * h in GF(2^128), both byte-reversed ("reflected") as in Intel's carry-less multiplication paper, algorithm 5 */
HW static inline __m128i hw_gfmul(__m128i a, __m128i b) {
    __m128i t3 = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i t4 = _mm_clmulepi64_si128(a, b, 0x10);
    __m128i t5 = _mm_clmulepi64_si128(a, b, 0x01);
    __m128i t6 = _mm_clmulepi64_si128(a, b, 0x11);
    t4 = _mm_xor_si128(t4, t5);
    t5 = _mm_slli_si128(t4, 8);
    t4 = _mm_srli_si128(t4, 8);
    t3 = _mm_xor_si128(t3, t5);
    t6 = _mm_xor_si128(t6, t4);
    __m128i t7 = _mm_srli_epi32(t3, 31);
    __m128i t8 = _mm_srli_epi32(t6, 31);
    t3 = _mm_slli_epi32(t3, 1);
    t6 = _mm_slli_epi32(t6, 1);
    __m128i t9 = _mm_srli_si128(t7, 12);
    t8 = _mm_slli_si128(t8, 4);
    t7 = _mm_slli_si128(t7, 4);
    t3 = _mm_or_si128(t3, t7);
    t6 = _mm_or_si128(t6, t8);
    t6 = _mm_or_si128(t6, t9);
    t7 = _mm_slli_epi32(t3, 31);
    t8 = _mm_slli_epi32(t3, 30);
    t9 = _mm_slli_epi32(t3, 25);
    t7 = _mm_xor_si128(t7, t8);
    t7 = _mm_xor_si128(t7, t9);
    t8 = _mm_srli_si128(t7, 4);
    t7 = _mm_slli_si128(t7, 12);
    t3 = _mm_xor_si128(t3, t7);
    __m128i t2 = _mm_srli_epi32(t3, 1);
    t4 = _mm_srli_epi32(t3, 2);
    t5 = _mm_srli_epi32(t3, 7);
    t2 = _mm_xor_si128(t2, t4);
    t2 = _mm_xor_si128(t2, t5);
    t2 = _mm_xor_si128(t2, t8);
    t3 = _mm_xor_si128(t3, t2);
    return _mm_xor_si128(t6, t3);
}

HW static void hw_ghash(const u8 hkey[16], u8 y[16], const u8 *data, size_t n) {
    const __m128i rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i h = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)hkey), rev);
    __m128i acc = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)y), rev);
    while (n >= 16) {
        acc = _mm_xor_si128(acc, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)data), rev));
        acc = hw_gfmul(acc, h);
        data += 16;
        n -= 16;
    }
    if (n) {
        u8 blk[16] = { 0 };
        memcpy(blk, data, n);
        acc = _mm_xor_si128(acc, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)blk), rev));
        acc = hw_gfmul(acc, h);
    }
    _mm_storeu_si128((__m128i *)y, _mm_shuffle_epi8(acc, rev));
}
#endif

/* ------------------------------------------------------------------------------------------------ portable GHASH */

static inline u64 be64(const u8 *p) {
    return (u64)p[0] << 56 | (u64)p[1] << 48 | (u64)p[2] << 40 | (u64)p[3] << 32 | (u64)p[4] << 24 | (u64)p[5] << 16 | (u64)p[6] << 8 | p[7];
}
static inline void put_be64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (56 - 8 * i)); }

/* y = y * h: the bit-serial algorithm of SP 800-38D 6.3, with masks instead of branches */
static void sw_gfmul(u8 y[16], u64 hh, u64 hl) {
    u64 zh = 0, zl = 0, vh = hh, vl = hl;
    for (int i = 0; i < 128; i++) {
        u64 bit = (y[i >> 3] >> (7 - (i & 7))) & 1;
        u64 m = 0 - bit;
        zh ^= vh & m;
        zl ^= vl & m;
        u64 lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ ((0 - lsb) & 0xe100000000000000ull);
    }
    put_be64(y, zh);
    put_be64(y + 8, zl);
}

static void sw_ghash(const u8 hkey[16], u8 y[16], const u8 *data, size_t n) {
    u64 hh = be64(hkey), hl = be64(hkey + 8);
    while (n) {
        size_t take = n < 16 ? n : 16;
        for (size_t i = 0; i < take; i++) y[i] ^= data[i];
        sw_gfmul(y, hh, hl);
        data += take;
        n -= take;
    }
}

/* ------------------------------------------------------------------------------------------------ interface */

void tlsc_aes_set_key(tlsc_aes_key *k, const u8 *key, size_t keylen) {
    memset(k, 0, sizeof *k);
    k->rounds = keylen == 32 ? 14 : 10;
    k->hw = 0;
#ifdef TLSC_HAVE_X86
    if (tlsc_aes_hw_available()) {
        hw_expand(k, key, keylen);
        k->hw = 1;
        return;
    }
#endif
    sw_expand(k, key, keylen);
}

void tlsc_aes_encrypt_block(const tlsc_aes_key *k, const u8 in[16], u8 out[16]) {
#ifdef TLSC_HAVE_X86
    if (k->hw) {
        hw_encrypt(k, in, out);
        return;
    }
#endif
    sw_encrypt(k, in, out);
}

static void ghash(const tlsc_gcm_ctx *g, u8 y[16], const u8 *data, size_t n) {
#ifdef TLSC_HAVE_X86
    if (g->aes.hw) {
        hw_ghash(g->h, y, data, n);
        return;
    }
#endif
    sw_ghash(g->h, y, data, n);
}

void tlsc_gcm_init(tlsc_gcm_ctx *g, const u8 *key, size_t keylen) {
    static const u8 zero[16] = { 0 };
    tlsc_aes_set_key(&g->aes, key, keylen);
    tlsc_aes_encrypt_block(&g->aes, zero, g->h);
}

static inline void inc32(u8 ctr[16]) {
    for (int i = 15; i >= 12; i--)
        if (++ctr[i] != 0) break;
}

static void gcm_ctr(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *in, u8 *out, size_t n) {
    u8 ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    for (size_t off = 0; off < n; off += 16) {
        inc32(ctr);
        tlsc_aes_encrypt_block(&g->aes, ctr, ks);
        size_t take = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < take; i++) out[off + i] = in[off + i] ^ ks[i];
    }
    tlsc_wipe(ks, sizeof ks);
}

static void gcm_tag(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *aad, size_t aadlen, const u8 *ct, size_t n, u8 tag[16]) {
    u8 y[16] = { 0 }, j0[16], lens[16], ek[16];
    ghash(g, y, aad, aadlen);
    ghash(g, y, ct, n);
    put_be64(lens, (u64)aadlen * 8);
    put_be64(lens + 8, (u64)n * 8);
    ghash(g, y, lens, 16);
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    tlsc_aes_encrypt_block(&g->aes, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

void tlsc_gcm_seal(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, u8 tag[16]) {
    gcm_ctr(g, iv, in, out, n);
    gcm_tag(g, iv, aad, aadlen, out, n, tag);
}

int tlsc_gcm_open(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, const u8 tag[16]) {
    u8 mine[16];
    gcm_tag(g, iv, aad, aadlen, in, n, mine);
    if (!tlsc_ct_memeq(mine, tag, 16)) return -1;
    gcm_ctr(g, iv, in, out, n);
    return 0;
}
