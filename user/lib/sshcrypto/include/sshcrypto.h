/*
 * sshcrypto: the primitives of the SSH server (user/sbin/sshd) and nothing else: SHA-512, X25519, Ed25519, ChaCha20 and Poly1305 (the
 * original ChaCha20 with a 64-bit nonce and a 64-bit block counter, which is what OpenSSH's chacha20-poly1305@openssh.com uses).
 * SHA-256 is in libgap (<crypto.h>). The field arithmetic follows TweetNaCl (compact, not fast); the known-answer tests are
 * tests/case_sshcrypto.py (RFC 7748, RFC 8032, RFC 8439 style vectors, generated with an independent library).
 */
#ifndef SSHCRYPTO_H
#define SSHCRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; size_t n; } sha512_ctx;
void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t n);
void sha512_final(sha512_ctx *c, uint8_t out[64]);

/* X25519 (RFC 7748). scalar and point are 32 bytes. x25519() returns -1 when the shared secret is all zero (a point of small order). */
int  x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Ed25519 (RFC 8032). The private key is the 32-byte seed. verify returns 0 for a good signature. */
void ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t seed[32], const uint8_t pk[32]);
int  ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]);

/* ChaCha20 with a 64-bit nonce and a 64-bit counter; out may equal in. */
void chacha20_xor(uint8_t *out, const uint8_t *in, size_t n, const uint8_t key[32], const uint8_t nonce[8], uint64_t counter);

/* Poly1305 (RFC 8439) over a whole message with a one-time key. */
void poly1305_mac(uint8_t tag[16], const uint8_t *msg, size_t n, const uint8_t key[32]);

/* Constant-time comparison: 0 when the n bytes are equal. */
int  ct_memcmp(const void *a, const void *b, size_t n);

#endif
