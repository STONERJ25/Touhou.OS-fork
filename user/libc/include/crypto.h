/* Hashes and password hashing for libgap. */
#ifndef _CRYPTO_H
#define _CRYPTO_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; size_t n; } md5_ctx;
void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t n);
void md5_final(md5_ctx *c, uint8_t out[16]);

uint32_t crc32_update(uint32_t crc, const void *data, size_t n);

/* Password hashes look like "$gap$<salt>$<64 hex digits>". Pass a hash (or "$gap$salt") as `salt` to verify. */
void gap_hash_password(const char *password, const char *salt, char *out, size_t outsz);
int  gap_check_password(const char *password, const char *stored);
void gap_make_salt(char *out, size_t n);

#endif
