/*
 * libtls internals: types, the hash/MAC/KDF interface and the primitives the protocol code is built from.
 * Nothing in here knows about the operating system: the same sources are compiled into a host DLL by tools/tlshost.py so
 * that every primitive can be checked against Python's cryptography package.
 *
 * Naming: tlsc_* are the crypto primitives, tls_* the protocol layer, x509_* / asn1_* the certificate code.
 */
#ifndef TLSINT_H
#define TLSINT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned __int128 u128;

#define TLSC_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- constant-time helpers ---- */
int  tlsc_ct_memeq(const void *a, const void *b, size_t n);          /* 1 if equal; the time does not depend on the data */
void tlsc_wipe(void *p, size_t n);                                    /* memset that the compiler may not drop */

/* ---- hashes ---- */
enum { TLSC_SHA256 = 1, TLSC_SHA384 = 2, TLSC_SHA512 = 3 };

typedef struct { u32 h[8]; u64 len; u8 buf[64]; size_t n; } tlsc_sha256_ctx;
typedef struct { u64 h[8]; u64 len; u8 buf[128]; size_t n; } tlsc_sha512_ctx;      /* also used for SHA-384 */

typedef struct {
    int alg;
    union { tlsc_sha256_ctx s256; tlsc_sha512_ctx s512; } u;
} tlsc_hash;

size_t tlsc_hash_len(int alg);                                        /* 32, 48, 64 (0 for an unknown algorithm) */
size_t tlsc_hash_block(int alg);                                      /* 64 or 128 */
void   tlsc_hash_init(tlsc_hash *h, int alg);
void   tlsc_hash_update(tlsc_hash *h, const void *data, size_t n);
void   tlsc_hash_final(tlsc_hash *h, u8 *out);                        /* out has tlsc_hash_len() bytes; the context is spent */
void   tlsc_hash_once(int alg, const void *data, size_t n, u8 *out);

/* ---- HMAC and HKDF (RFC 2104, RFC 5869) ---- */
typedef struct { tlsc_hash inner, outer; } tlsc_hmac_ctx;
void tlsc_hmac_init(tlsc_hmac_ctx *c, int alg, const void *key, size_t keylen);
void tlsc_hmac_update(tlsc_hmac_ctx *c, const void *data, size_t n);
void tlsc_hmac_final(tlsc_hmac_ctx *c, u8 *out);
void tlsc_hmac(int alg, const void *key, size_t keylen, const void *data, size_t n, u8 *out);

void tlsc_hkdf_extract(int alg, const void *salt, size_t saltlen, const void *ikm, size_t ikmlen, u8 *prk);
int  tlsc_hkdf_expand(int alg, const u8 *prk, size_t prklen, const void *info, size_t infolen, u8 *out, size_t outlen);   /* 0 ok, -1 if outlen is too big */
/* TLS 1.3: HKDF-Expand-Label(secret, label, context, length), label without the "tls13 " prefix */
int  tlsc_hkdf_expand_label(int alg, const u8 *secret, const char *label, const u8 *ctx, size_t ctxlen, u8 *out, size_t outlen);
/* TLS 1.2: the PRF of RFC 5246 (P_hash with HMAC-SHA-256 or -384) */
void tlsc_prf(int alg, const u8 *secret, size_t secretlen, const char *label, const u8 *seed1, size_t seed1len, const u8 *seed2, size_t seed2len,
              u8 *out, size_t outlen);

/* ---- AEAD ciphers ---- */
void tlsc_chacha20_xor(const u8 key[32], const u8 nonce[12], u32 counter, const u8 *in, u8 *out, size_t n);
void tlsc_chacha20poly1305_seal(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, u8 tag[16]);
int  tlsc_chacha20poly1305_open(const u8 key[32], const u8 nonce[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, const u8 tag[16]);   /* 0 ok, -1 bad tag */
void tlsc_poly1305(const u8 key[32], const u8 *data, size_t n, u8 tag[16]);

typedef struct { u32 rk[60]; int rounds; int hw; } tlsc_aes_key;      /* hw: the round keys were expanded for AES-NI */
void tlsc_aes_set_key(tlsc_aes_key *k, const u8 *key, size_t keylen); /* keylen 16 or 32 */
void tlsc_aes_encrypt_block(const tlsc_aes_key *k, const u8 in[16], u8 out[16]);
int  tlsc_aes_hw_available(void);                                     /* AES-NI and PCLMULQDQ are both present */
void tlsc_aes_force_software(int on);                                 /* for tests: pretend the CPU has no AES-NI */

typedef struct { tlsc_aes_key aes; u8 h[16]; } tlsc_gcm_ctx;
void tlsc_gcm_init(tlsc_gcm_ctx *g, const u8 *key, size_t keylen);
void tlsc_gcm_seal(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, u8 tag[16]);
int  tlsc_gcm_open(const tlsc_gcm_ctx *g, const u8 iv[12], const u8 *aad, size_t aadlen, const u8 *in, size_t n, u8 *out, const u8 tag[16]);          /* 0 ok, -1 bad tag */

/* ---- big numbers (Montgomery arithmetic on 64-bit limbs) ---- */
#define TLSC_BN_MAXLIMBS 64                                           /* 4096 bits */

typedef struct {
    size_t n;                                                         /* limbs in the modulus */
    u64 m[TLSC_BN_MAXLIMBS];
    u64 rr[TLSC_BN_MAXLIMBS];                                         /* R^2 mod m */
    u64 one[TLSC_BN_MAXLIMBS];                                        /* R mod m: 1 in Montgomery form */
    u64 m0inv;                                                        /* -m^-1 mod 2^64 */
} tlsc_mont;

int  tlsc_mont_init(tlsc_mont *c, const u64 *m, size_t n);            /* m odd, top limb non-zero; 0 ok, -1 if not */
void tlsc_mont_mul(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b);
void tlsc_mont_to(const tlsc_mont *c, u64 *r, const u64 *a);          /* a < m, plain -> Montgomery */
void tlsc_mont_from(const tlsc_mont *c, u64 *r, const u64 *a);        /* Montgomery -> plain */
void tlsc_mont_add(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b);   /* modular, constant time */
void tlsc_mont_sub(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b);
void tlsc_mont_neg(const tlsc_mont *c, u64 *r, const u64 *a);
void tlsc_mont_pow(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *e, size_t elimbs);     /* a, r in Montgomery form; e plain (a public or fixed exponent) */
void tlsc_mont_inv(const tlsc_mont *c, u64 *r, const u64 *a);          /* a^(m-2): m must be prime */
void tlsc_bn_from_bytes(u64 *r, size_t n, const u8 *b, size_t len);   /* big endian bytes -> n little-endian limbs (len <= 8n) */
void tlsc_bn_to_bytes(u8 *b, size_t len, const u64 *a, size_t n);     /* the low len bytes, big endian */
int  tlsc_bn_cmp(const u64 *a, const u64 *b, size_t n);               /* -1, 0, 1 (not constant time) */
u64  tlsc_bn_is_zero(const u64 *a, size_t n);                         /* all ones if zero, else 0 (constant time) */
u64  tlsc_bn_lt_mask(const u64 *a, const u64 *b, size_t n);           /* all ones if a < b (constant time) */
u64  tlsc_bn_add(u64 *r, const u64 *a, const u64 *b, size_t n);       /* returns the carry */
u64  tlsc_bn_sub(u64 *r, const u64 *a, const u64 *b, size_t n);       /* returns the borrow */
void tlsc_bn_select(u64 *r, const u64 *a, const u64 *b, size_t n, u64 mask_b);    /* r = mask_b ? b : a */

/* ---- elliptic curves ---- */
enum { TLSC_P256 = 1, TLSC_P384 = 2 };
int  tlsc_x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);                 /* 0 ok, -1 if the result is all zero (a bad peer value) */
void tlsc_x25519_base(u8 out[32], const u8 scalar[32]);
size_t tlsc_ec_size(int curve);                                                         /* bytes in a coordinate: 32 or 48 */
int  tlsc_ec_point_valid(int curve, const u8 *x, const u8 *y);                          /* on the curve and not the point at infinity */
int  tlsc_ecdh_keypair(int curve, const u8 *random_bytes, u8 *priv, u8 *pub);           /* random_bytes: 2*size bytes; pub = 04 || X || Y; 0 ok */
int  tlsc_ecdh(int curve, const u8 *priv, const u8 *peer_pub, size_t peer_len, u8 *shared);   /* peer_pub as in TLS: 04 || X || Y; 0 ok, -1 bad point */
int  tlsc_ecdsa_verify(int curve, const u8 *pub, size_t publen, const u8 *hash, size_t hashlen, const u8 *r, size_t rlen, const u8 *s, size_t slen);   /* 0 valid */

/* ---- RSA (public operations only) ---- */
int  tlsc_rsa_verify_pkcs1(const u8 *n, size_t nlen, const u8 *e, size_t elen, int hash_alg, const u8 *hash, const u8 *sig, size_t siglen);   /* 0 valid */
int  tlsc_rsa_verify_pss(const u8 *n, size_t nlen, const u8 *e, size_t elen, int hash_alg, const u8 *hash, const u8 *sig, size_t siglen, size_t saltlen);

#endif
