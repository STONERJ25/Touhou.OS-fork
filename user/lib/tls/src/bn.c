/*
 * Fixed-size big numbers on 64-bit limbs and Montgomery arithmetic (CIOS method).
 *
 * Everything that handles secret values (tlsc_mont_mul/add/sub/neg, the select and compare-mask helpers) runs in a time that
 * depends only on the number of limbs. tlsc_mont_pow and tlsc_mont_inv skip the leading zero bits of the exponent and branch
 * on its bits, so they may only be given public exponents (RSA's e, and p-2 for inversion by Fermat's little theorem).
 */
#include "tlsint.h"

void tlsc_bn_from_bytes(u64 *r, size_t n, const u8 *b, size_t len) {
    for (size_t i = 0; i < n; i++) r[i] = 0;
    for (size_t i = 0; i < len && i < 8 * n; i++) r[i / 8] |= (u64)b[len - 1 - i] << (8 * (i % 8));
}

void tlsc_bn_to_bytes(u8 *b, size_t len, const u64 *a, size_t n) {
    for (size_t i = 0; i < len; i++) {
        size_t limb = i / 8;
        b[len - 1 - i] = limb < n ? (u8)(a[limb] >> (8 * (i % 8))) : 0;
    }
}

int tlsc_bn_cmp(const u64 *a, const u64 *b, size_t n) {
    for (size_t i = n; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

u64 tlsc_bn_is_zero(const u64 *a, size_t n) {
    u64 acc = 0;
    for (size_t i = 0; i < n; i++) acc |= a[i];
    return ((acc | (0 - acc)) >> 63) - 1;
}

u64 tlsc_bn_lt_mask(const u64 *a, const u64 *b, size_t n) {
    u64 borrow = 0;
    for (size_t i = 0; i < n; i++) {
        u128 d = (u128)a[i] - b[i] - borrow;
        borrow = (u64)(d >> 64) & 1;
    }
    return 0 - borrow;
}

u64 tlsc_bn_add(u64 *r, const u64 *a, const u64 *b, size_t n) {
    u64 carry = 0;
    for (size_t i = 0; i < n; i++) {
        u128 s = (u128)a[i] + b[i] + carry;
        r[i] = (u64)s;
        carry = (u64)(s >> 64);
    }
    return carry;
}

u64 tlsc_bn_sub(u64 *r, const u64 *a, const u64 *b, size_t n) {
    u64 borrow = 0;
    for (size_t i = 0; i < n; i++) {
        u128 d = (u128)a[i] - b[i] - borrow;
        r[i] = (u64)d;
        borrow = (u64)(d >> 64) & 1;
    }
    return borrow;
}

void tlsc_bn_select(u64 *r, const u64 *a, const u64 *b, size_t n, u64 mask_b) {
    for (size_t i = 0; i < n; i++) r[i] = (a[i] & ~mask_b) | (b[i] & mask_b);
}

/* x = 2x mod m for x < m */
static void double_mod(u64 *x, const u64 *m, size_t n) {
    u64 d[TLSC_BN_MAXLIMBS];
    u64 carry = tlsc_bn_add(x, x, x, n);
    u64 borrow = tlsc_bn_sub(d, x, m, n);
    tlsc_bn_select(x, x, d, n, (0 - carry) | ~(0 - borrow));
}

int tlsc_mont_init(tlsc_mont *c, const u64 *m, size_t n) {
    if (n == 0 || n > TLSC_BN_MAXLIMBS || !(m[0] & 1) || m[n - 1] == 0) return -1;
    if (n == 1 && m[0] == 1) return -1;
    memset(c, 0, sizeof *c);
    c->n = n;
    memcpy(c->m, m, n * sizeof(u64));
    u64 inv = m[0];                                   /* Newton: each step doubles the number of correct low bits (3 -> 96) */
    for (int i = 0; i < 5; i++) inv *= 2 - m[0] * inv;
    c->m0inv = 0 - inv;
    u64 x[TLSC_BN_MAXLIMBS] = { 1 };
    for (size_t i = 0; i < 64 * n; i++) double_mod(x, c->m, n);       /* x = 2^(64n) mod m = R mod m */
    memcpy(c->one, x, n * sizeof(u64));
    for (size_t i = 0; i < 64 * n; i++) double_mod(x, c->m, n);       /* x = R^2 mod m */
    memcpy(c->rr, x, n * sizeof(u64));
    return 0;
}

void tlsc_mont_mul(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b) {
    const size_t n = c->n;
    u64 t[TLSC_BN_MAXLIMBS + 2];
    for (size_t i = 0; i < n + 2; i++) t[i] = 0;
    for (size_t i = 0; i < n; i++) {
        u64 carry = 0, bi = b[i];
        for (size_t j = 0; j < n; j++) {
            u128 x = (u128)a[j] * bi + t[j] + carry;
            t[j] = (u64)x;
            carry = (u64)(x >> 64);
        }
        u128 x = (u128)t[n] + carry;
        t[n] = (u64)x;
        t[n + 1] = (u64)(x >> 64);
        u64 mm = t[0] * c->m0inv;
        x = (u128)mm * c->m[0] + t[0];
        carry = (u64)(x >> 64);
        for (size_t j = 1; j < n; j++) {
            x = (u128)mm * c->m[j] + t[j] + carry;
            t[j - 1] = (u64)x;
            carry = (u64)(x >> 64);
        }
        x = (u128)t[n] + carry;
        t[n - 1] = (u64)x;
        t[n] = t[n + 1] + (u64)(x >> 64);
    }
    /* t < 2m: subtract m once if t >= m */
    u64 d[TLSC_BN_MAXLIMBS];
    u64 borrow = 0;
    for (size_t j = 0; j < n; j++) {
        u128 s = (u128)t[j] - c->m[j] - borrow;
        d[j] = (u64)s;
        borrow = (u64)(s >> 64) & 1;
    }
    u64 top = 0 - ((t[n] | (0 - t[n])) >> 63);        /* all ones if the (n+1)th limb is non-zero */
    u64 use_d = top | ~(0 - borrow);
    tlsc_bn_select(r, t, d, n, use_d);
}

void tlsc_mont_to(const tlsc_mont *c, u64 *r, const u64 *a) { tlsc_mont_mul(c, r, a, c->rr); }

void tlsc_mont_from(const tlsc_mont *c, u64 *r, const u64 *a) {
    u64 one[TLSC_BN_MAXLIMBS] = { 1 };
    tlsc_mont_mul(c, r, a, one);
}

void tlsc_mont_add(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b) {
    const size_t n = c->n;
    u64 s[TLSC_BN_MAXLIMBS], d[TLSC_BN_MAXLIMBS];
    u64 carry = tlsc_bn_add(s, a, b, n);
    u64 borrow = tlsc_bn_sub(d, s, c->m, n);
    tlsc_bn_select(r, s, d, n, (0 - carry) | ~(0 - borrow));
}

void tlsc_mont_sub(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *b) {
    const size_t n = c->n;
    u64 d[TLSC_BN_MAXLIMBS], e[TLSC_BN_MAXLIMBS];
    u64 borrow = tlsc_bn_sub(d, a, b, n);
    tlsc_bn_add(e, d, c->m, n);
    tlsc_bn_select(r, d, e, n, 0 - borrow);
}

void tlsc_mont_neg(const tlsc_mont *c, u64 *r, const u64 *a) {
    u64 zero[TLSC_BN_MAXLIMBS] = { 0 };
    tlsc_mont_sub(c, r, zero, a);
}

void tlsc_mont_pow(const tlsc_mont *c, u64 *r, const u64 *a, const u64 *e, size_t elimbs) {
    const size_t n = c->n;
    u64 acc[TLSC_BN_MAXLIMBS], base[TLSC_BN_MAXLIMBS];
    memcpy(base, a, n * sizeof(u64));
    memcpy(acc, c->one, n * sizeof(u64));
    size_t top = elimbs * 64;
    while (top > 0 && !((e[(top - 1) / 64] >> ((top - 1) % 64)) & 1)) top--;
    for (size_t i = top; i-- > 0;) {
        tlsc_mont_mul(c, acc, acc, acc);
        if ((e[i / 64] >> (i % 64)) & 1) tlsc_mont_mul(c, acc, acc, base);
    }
    memcpy(r, acc, n * sizeof(u64));
}

void tlsc_mont_inv(const tlsc_mont *c, u64 *r, const u64 *a) {
    const size_t n = c->n;
    u64 e[TLSC_BN_MAXLIMBS], two[TLSC_BN_MAXLIMBS] = { 2 };
    tlsc_bn_sub(e, c->m, two, n);                     /* a^(m-2) = a^-1 for prime m */
    tlsc_mont_pow(c, r, a, e, n);
}
