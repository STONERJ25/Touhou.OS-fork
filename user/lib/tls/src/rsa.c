/*
 * RSA signature verification (RFC 8017): PKCS#1 v1.5 and PSS. Public-key operations only, so nothing here handles a secret.
 *
 * PKCS#1 v1.5 is checked by rebuilding the whole expected encoded message (0x00 0x01 FF..FF 0x00 DigestInfo hash) and comparing
 * it byte for byte, which is immune to the "lenient parsing" forgeries (Bleichenbacher 2006) that hit implementations that
 * parse the padding instead.
 */
#include "tlsint.h"

#define RSA_MIN_BITS 1024
#define RSA_MAX_BITS (TLSC_BN_MAXLIMBS * 64)

static const u8 DI_SHA256[19] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const u8 DI_SHA384[19] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const u8 DI_SHA512[19] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

static const u8 *strip(const u8 *b, size_t *len) {
    while (*len > 0 && b[0] == 0) { b++; (*len)--; }
    return b;
}

/* em = sig^e mod n as k bytes (k = length of n). Returns k, or 0 if the key or the signature is unacceptable. */
static size_t rsa_public(const u8 *nb, size_t nlen, const u8 *eb, size_t elen, const u8 *sig, size_t siglen, u8 *em) {
    nb = strip(nb, &nlen);
    eb = strip(eb, &elen);
    if (nlen == 0 || elen == 0 || elen > 8) return 0;
    size_t bits = nlen * 8 - (size_t)__builtin_clz((unsigned)nb[0]) + 24;      /* exact bit length of the modulus */
    if (bits < RSA_MIN_BITS || bits > RSA_MAX_BITS || !(nb[nlen - 1] & 1)) return 0;
    if (siglen != nlen) return 0;                                              /* RFC 8017: the signature has exactly k bytes */
    size_t limbs = (nlen + 7) / 8;
    u64 n[TLSC_BN_MAXLIMBS], s[TLSC_BN_MAXLIMBS], e[1], r[TLSC_BN_MAXLIMBS];
    tlsc_bn_from_bytes(n, limbs, nb, nlen);
    tlsc_bn_from_bytes(s, limbs, sig, siglen);
    tlsc_bn_from_bytes(e, 1, eb, elen);
    if (!(e[0] & 1) || e[0] < 3) return 0;
    if (!tlsc_bn_lt_mask(s, n, limbs)) return 0;                               /* s < n */
    tlsc_mont c;
    if (tlsc_mont_init(&c, n, limbs) != 0) return 0;
    tlsc_mont_to(&c, s, s);
    tlsc_mont_pow(&c, r, s, e, 1);
    tlsc_mont_from(&c, r, r);
    tlsc_bn_to_bytes(em, nlen, r, limbs);
    return nlen;
}

int tlsc_rsa_verify_pkcs1(const u8 *n, size_t nlen, const u8 *e, size_t elen, int hash_alg, const u8 *hash, const u8 *sig, size_t siglen) {
    const u8 *prefix;
    switch (hash_alg) {
    case TLSC_SHA256: prefix = DI_SHA256; break;
    case TLSC_SHA384: prefix = DI_SHA384; break;
    case TLSC_SHA512: prefix = DI_SHA512; break;
    default: return -1;
    }
    size_t hl = tlsc_hash_len(hash_alg);
    u8 em[TLSC_BN_MAXLIMBS * 8], want[TLSC_BN_MAXLIMBS * 8];
    size_t k = rsa_public(n, nlen, e, elen, sig, siglen, em);
    if (k == 0) return -1;
    size_t tlen = 19 + hl;
    if (k < tlen + 11) return -1;
    want[0] = 0x00;
    want[1] = 0x01;
    memset(want + 2, 0xff, k - tlen - 3);
    want[k - tlen - 1] = 0x00;
    memcpy(want + k - tlen, prefix, 19);
    memcpy(want + k - hl, hash, hl);
    return tlsc_ct_memeq(em, want, k) ? 0 : -1;
}

/* MGF1 (RFC 8017 B.2) */
static void mgf1(int alg, const u8 *seed, size_t seedlen, u8 *out, size_t outlen) {
    size_t hl = tlsc_hash_len(alg);
    u8 digest[64];
    for (u32 counter = 0; outlen > 0; counter++) {
        tlsc_hash h;
        u8 c[4] = { (u8)(counter >> 24), (u8)(counter >> 16), (u8)(counter >> 8), (u8)counter };
        tlsc_hash_init(&h, alg);
        tlsc_hash_update(&h, seed, seedlen);
        tlsc_hash_update(&h, c, 4);
        tlsc_hash_final(&h, digest);
        size_t take = outlen < hl ? outlen : hl;
        memcpy(out, digest, take);
        out += take;
        outlen -= take;
    }
}

int tlsc_rsa_verify_pss(const u8 *n, size_t nlen, const u8 *e, size_t elen, int hash_alg, const u8 *hash, const u8 *sig, size_t siglen, size_t saltlen) {
    size_t hl = tlsc_hash_len(hash_alg);
    if (hl == 0) return -1;
    u8 em_full[TLSC_BN_MAXLIMBS * 8];
    size_t k = rsa_public(n, nlen, e, elen, sig, siglen, em_full);
    if (k == 0) return -1;
    const u8 *nstrip = strip(n, &nlen);
    size_t modbits = nlen * 8 - (size_t)__builtin_clz((unsigned)nstrip[0]) + 24;
    size_t embits = modbits - 1;
    size_t emlen = (embits + 7) / 8;
    const u8 *em = em_full + (k - emlen);                                      /* emlen is k or k-1 */
    if (emlen == k - 1 && em_full[0] != 0) return -1;
    if (emlen < hl + saltlen + 2) return -1;
    if (em[emlen - 1] != 0xbc) return -1;
    size_t dblen = emlen - hl - 1;
    const u8 *masked = em;
    const u8 *h = em + dblen;
    unsigned topbits = (unsigned)(8 * emlen - embits);
    if (topbits && (masked[0] >> (8 - topbits))) return -1;
    u8 db[TLSC_BN_MAXLIMBS * 8];
    mgf1(hash_alg, h, hl, db, dblen);
    for (size_t i = 0; i < dblen; i++) db[i] ^= masked[i];
    if (topbits) db[0] &= (u8)(0xff >> topbits);
    size_t pad = dblen - saltlen - 1;
    for (size_t i = 0; i < pad; i++)
        if (db[i] != 0) return -1;
    if (db[pad] != 0x01) return -1;
    u8 mprime[8 + 64 + 64], hh[64];
    memset(mprime, 0, 8);
    memcpy(mprime + 8, hash, hl);
    if (saltlen > 64) return -1;
    memcpy(mprime + 8 + hl, db + pad + 1, saltlen);
    tlsc_hash_once(hash_alg, mprime, 8 + hl + saltlen, hh);
    return tlsc_ct_memeq(hh, h, hl) ? 0 : -1;
}
