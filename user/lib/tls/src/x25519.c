/*
 * X25519 (RFC 7748): the Montgomery ladder over GF(2^255 - 19), on top of the generic Montgomery code in bn.c.
 * The ladder does the same operations for every scalar bit and swaps with masks, so it has no secret-dependent branches
 * or memory accesses. It is slower than a hand-tuned field implementation, which does not matter for one handshake.
 */
#include "tlsint.h"

static const u64 P25519[4] = { 0xffffffffffffffedULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL, 0x7fffffffffffffffULL };

static void le_to_limbs(u64 *r, const u8 *b) {
    for (int i = 0; i < 4; i++) {
        u64 v = 0;
        for (int j = 7; j >= 0; j--) v = (v << 8) | b[8 * i + j];
        r[i] = v;
    }
}

static void limbs_to_le(u8 *b, const u64 *a) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) b[8 * i + j] = (u8)(a[i] >> (8 * j));
}

static void cswap(u64 mask, u64 *a, u64 *b) {
    for (int i = 0; i < 4; i++) {
        u64 t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

int tlsc_x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    tlsc_mont c;
    tlsc_mont_init(&c, P25519, 4);
    u8 k[32], ub[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;
    memcpy(ub, point, 32);
    ub[31] &= 127;                                     /* RFC 7748: the top bit of the u-coordinate is ignored */

    u64 u[4], d[4];
    le_to_limbs(u, ub);
    u64 borrow = tlsc_bn_sub(d, u, P25519, 4);         /* non-canonical u in [p, 2^255) is reduced once */
    tlsc_bn_select(u, d, u, 4, 0 - borrow);            /* borrow set: u < p, keep u; else use d = u - p */

    u64 x1[4], x2[4], z2[4], x3[4], z3[4], a24[4], t[4];
    tlsc_mont_to(&c, x1, u);
    memcpy(x2, c.one, sizeof x2);
    memset(z2, 0, sizeof z2);
    memcpy(x3, x1, sizeof x3);
    memcpy(z3, c.one, sizeof z3);
    t[0] = 121665; t[1] = t[2] = t[3] = 0;
    tlsc_mont_to(&c, a24, t);

    u64 swap = 0;
    for (int bit = 254; bit >= 0; bit--) {
        u64 kt = (k[bit >> 3] >> (bit & 7)) & 1;
        swap ^= kt;
        cswap(0 - swap, x2, x3);
        cswap(0 - swap, z2, z3);
        swap = kt;

        u64 A[4], AA[4], B[4], BB[4], E[4], C[4], D[4], DA[4], CB[4], s1[4], s2[4];
        tlsc_mont_add(&c, A, x2, z2);
        tlsc_mont_mul(&c, AA, A, A);
        tlsc_mont_sub(&c, B, x2, z2);
        tlsc_mont_mul(&c, BB, B, B);
        tlsc_mont_sub(&c, E, AA, BB);
        tlsc_mont_add(&c, C, x3, z3);
        tlsc_mont_sub(&c, D, x3, z3);
        tlsc_mont_mul(&c, DA, D, A);
        tlsc_mont_mul(&c, CB, C, B);
        tlsc_mont_add(&c, s1, DA, CB);
        tlsc_mont_mul(&c, x3, s1, s1);
        tlsc_mont_sub(&c, s2, DA, CB);
        tlsc_mont_mul(&c, s2, s2, s2);
        tlsc_mont_mul(&c, z3, x1, s2);
        tlsc_mont_mul(&c, x2, AA, BB);
        tlsc_mont_mul(&c, s1, a24, E);
        tlsc_mont_add(&c, s1, AA, s1);
        tlsc_mont_mul(&c, z2, E, s1);
    }
    cswap(0 - swap, x2, x3);
    cswap(0 - swap, z2, z3);

    u64 zi[4], res[4];
    tlsc_mont_inv(&c, zi, z2);
    tlsc_mont_mul(&c, res, x2, zi);
    tlsc_mont_from(&c, res, res);
    limbs_to_le(out, res);

    u8 acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    tlsc_wipe(k, sizeof k);
    tlsc_wipe(&c, sizeof c);
    tlsc_wipe(x2, sizeof x2);
    tlsc_wipe(x3, sizeof x3);
    tlsc_wipe(z2, sizeof z2);
    tlsc_wipe(z3, sizeof z3);
    return acc == 0 ? -1 : 0;
}

void tlsc_x25519_base(u8 out[32], const u8 scalar[32]) {
    u8 nine[32] = { 9 };
    tlsc_x25519(out, scalar, nine);
}
