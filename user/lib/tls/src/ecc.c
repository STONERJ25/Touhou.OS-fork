/*
 * NIST P-256 and P-384: ECDH and ECDSA verification.
 *
 * Points are kept in homogeneous projective coordinates and added with the complete formulas of Renes, Costello and Batina
 * (Eurocrypt 2016, algorithms 1 and 3 for a general `a`): they are correct for every pair of points, including doubling,
 * the point at infinity (0:1:0) and P + (-P), so the scalar multiplication needs no special cases and no branches.
 *
 * ECDH uses a fixed 4-bit window with a masked table lookup: the sequence of field operations and memory accesses does not
 * depend on the secret scalar. ECDSA verification handles only public values and uses the faster Shamir trick.
 * The field arithmetic comes from bn.c (Montgomery, constant time).
 */
#include "tlsint.h"
#include "tables.h"

#define MAXL 6

typedef struct { u64 x[MAXL], y[MAXL], z[MAXL]; } pt;

typedef struct {
    size_t n;                 /* limbs */
    size_t size;              /* bytes in a coordinate */
    tlsc_mont fp, fn;         /* arithmetic modulo the field prime p and the group order n */
    u64 b[MAXL], b3[MAXL];    /* b and 3b, Montgomery form */
    pt g;                     /* the base point, Montgomery form */
} curve;

#define FMUL(r, a, b) tlsc_mont_mul(&e->fp, r, a, b)
#define FADD(r, a, b) tlsc_mont_add(&e->fp, r, a, b)
#define FSUB(r, a, b) tlsc_mont_sub(&e->fp, r, a, b)

static int curve_setup(curve *e, int id) {
    const u64 *p, *b, *n, *gx, *gy;
    memset(e, 0, sizeof *e);
    if (id == TLSC_P256) {
        e->n = 4; e->size = 32;
        p = TLSC_P256_P; b = TLSC_P256_B; n = TLSC_P256_N; gx = TLSC_P256_GX; gy = TLSC_P256_GY;
    } else if (id == TLSC_P384) {
        e->n = 6; e->size = 48;
        p = TLSC_P384_P; b = TLSC_P384_B; n = TLSC_P384_N; gx = TLSC_P384_GX; gy = TLSC_P384_GY;
    } else {
        return -1;
    }
    if (tlsc_mont_init(&e->fp, p, e->n) != 0 || tlsc_mont_init(&e->fn, n, e->n) != 0) return -1;
    tlsc_mont_to(&e->fp, e->b, b);
    FADD(e->b3, e->b, e->b);
    FADD(e->b3, e->b3, e->b);
    tlsc_mont_to(&e->fp, e->g.x, gx);
    tlsc_mont_to(&e->fp, e->g.y, gy);
    memcpy(e->g.z, e->fp.one, e->n * sizeof(u64));
    return 0;
}

size_t tlsc_ec_size(int id) { return id == TLSC_P256 ? 32 : id == TLSC_P384 ? 48 : 0; }

/* r = a * x with a = -3 */
static void mul_a(const curve *e, u64 *r, const u64 *x) {
    u64 t[MAXL];
    FADD(t, x, x);
    FADD(t, t, x);
    tlsc_mont_neg(&e->fp, r, t);
}

static void pt_infinity(const curve *e, pt *p) {
    memset(p, 0, sizeof *p);
    memcpy(p->y, e->fp.one, e->n * sizeof(u64));
}

static int pt_is_infinity(const curve *e, const pt *p) { return tlsc_bn_is_zero(p->z, e->n) != 0; }

static void pt_select(const curve *e, pt *r, const pt *a, const pt *b, u64 mask_b) {
    tlsc_bn_select(r->x, a->x, b->x, e->n, mask_b);
    tlsc_bn_select(r->y, a->y, b->y, e->n, mask_b);
    tlsc_bn_select(r->z, a->z, b->z, e->n, mask_b);
}

/* r = p + q, complete (RCB 2016, algorithm 1) */
static void pt_add(const curve *e, pt *r, const pt *p, const pt *q) {
    u64 t0[MAXL], t1[MAXL], t2[MAXL], t3[MAXL], t4[MAXL], t5[MAXL], X3[MAXL], Y3[MAXL], Z3[MAXL];
    FMUL(t0, p->x, q->x);
    FMUL(t1, p->y, q->y);
    FMUL(t2, p->z, q->z);
    FADD(t3, p->x, p->y);
    FADD(t4, q->x, q->y);
    FMUL(t3, t3, t4);
    FADD(t4, t0, t1);
    FSUB(t3, t3, t4);
    FADD(t4, p->x, p->z);
    FADD(t5, q->x, q->z);
    FMUL(t4, t4, t5);
    FADD(t5, t0, t2);
    FSUB(t4, t4, t5);
    FADD(t5, p->y, p->z);
    FADD(X3, q->y, q->z);
    FMUL(t5, t5, X3);
    FADD(X3, t1, t2);
    FSUB(t5, t5, X3);
    mul_a(e, Z3, t4);
    FMUL(X3, e->b3, t2);
    FADD(Z3, X3, Z3);
    FSUB(X3, t1, Z3);
    FADD(Z3, t1, Z3);
    FMUL(Y3, X3, Z3);
    FADD(t1, t0, t0);
    FADD(t1, t1, t0);
    mul_a(e, t2, t2);
    FMUL(t4, e->b3, t4);
    FADD(t1, t1, t2);
    FSUB(t2, t0, t2);
    mul_a(e, t2, t2);
    FADD(t4, t4, t2);
    FMUL(t0, t1, t4);
    FADD(Y3, Y3, t0);
    FMUL(t0, t5, t4);
    FMUL(X3, t3, X3);
    FSUB(X3, X3, t0);
    FMUL(t0, t3, t1);
    FMUL(Z3, t5, Z3);
    FADD(Z3, Z3, t0);
    memcpy(r->x, X3, sizeof X3);
    memcpy(r->y, Y3, sizeof Y3);
    memcpy(r->z, Z3, sizeof Z3);
}

/* r = 2p, complete (RCB 2016, algorithm 3) */
static void pt_dbl(const curve *e, pt *r, const pt *p) {
    u64 t0[MAXL], t1[MAXL], t2[MAXL], t3[MAXL], X3[MAXL], Y3[MAXL], Z3[MAXL];
    FMUL(t0, p->x, p->x);
    FMUL(t1, p->y, p->y);
    FMUL(t2, p->z, p->z);
    FMUL(t3, p->x, p->y);
    FADD(t3, t3, t3);
    FMUL(Z3, p->x, p->z);
    FADD(Z3, Z3, Z3);
    mul_a(e, X3, Z3);
    FMUL(Y3, e->b3, t2);
    FADD(Y3, X3, Y3);
    FSUB(X3, t1, Y3);
    FADD(Y3, t1, Y3);
    FMUL(Y3, X3, Y3);
    FMUL(X3, t3, X3);
    FMUL(Z3, e->b3, Z3);
    mul_a(e, t2, t2);
    FSUB(t3, t0, t2);
    mul_a(e, t3, t3);
    FADD(t3, t3, Z3);
    FADD(Z3, t0, t0);
    FADD(t0, Z3, t0);
    FADD(t0, t0, t2);
    FMUL(t0, t0, t3);
    FADD(Y3, Y3, t0);
    FMUL(t2, p->y, p->z);
    FADD(t2, t2, t2);
    FMUL(t0, t2, t3);
    FSUB(X3, X3, t0);
    FMUL(Z3, t2, t1);
    FADD(Z3, Z3, Z3);
    FADD(Z3, Z3, Z3);
    memcpy(r->x, X3, sizeof X3);
    memcpy(r->y, Y3, sizeof Y3);
    memcpy(r->z, Z3, sizeof Z3);
}

static u64 eq_mask(u64 a, u64 b) {
    u64 x = a ^ b;
    return ((x | (0 - x)) >> 63) - 1;
}

/* r = k * p for a big-endian scalar of klen bytes; constant time with respect to k and p */
static void scalar_mult(const curve *e, pt *r, const u8 *k, size_t klen, const pt *p) {
    pt table[16], acc, sel;
    pt_infinity(e, &table[0]);
    table[1] = *p;
    for (int i = 2; i < 16; i += 2) {
        pt_dbl(e, &table[i], &table[i / 2]);
        pt_add(e, &table[i + 1], &table[i], p);
    }
    pt_infinity(e, &acc);
    for (size_t i = 0; i < klen * 2; i++) {
        for (int j = 0; j < 4; j++) pt_dbl(e, &acc, &acc);
        u64 w = (k[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
        sel = table[0];
        for (u64 j = 1; j < 16; j++) pt_select(e, &sel, &sel, &table[j], eq_mask(j, w));
        pt_add(e, &acc, &acc, &sel);
    }
    *r = acc;
    tlsc_wipe(table, sizeof table);
    tlsc_wipe(&acc, sizeof acc);
    tlsc_wipe(&sel, sizeof sel);
}

/* r = u1 * G + u2 * q for public little-endian limb scalars (variable time) */
static void double_scalar_mult(const curve *e, pt *r, const u64 *u1, const u64 *u2, const pt *q) {
    pt gq, acc;
    pt_add(e, &gq, &e->g, q);
    pt_infinity(e, &acc);
    for (int i = (int)(e->n * 64) - 1; i >= 0; i--) {
        pt_dbl(e, &acc, &acc);
        unsigned b1 = (unsigned)((u1[i / 64] >> (i % 64)) & 1), b2 = (unsigned)((u2[i / 64] >> (i % 64)) & 1);
        if (b1 && b2) pt_add(e, &acc, &acc, &gq);
        else if (b1) pt_add(e, &acc, &acc, &e->g);
        else if (b2) pt_add(e, &acc, &acc, q);
    }
    *r = acc;
}

/* Montgomery-form affine x (and y, if given) of a non-infinite point, as plain big-endian bytes */
static int pt_affine_bytes(const curve *e, const pt *p, u8 *xb, u8 *yb) {
    if (pt_is_infinity(e, p)) return -1;
    u64 zi[MAXL], v[MAXL];
    tlsc_mont_inv(&e->fp, zi, p->z);
    FMUL(v, p->x, zi);
    tlsc_mont_from(&e->fp, v, v);
    tlsc_bn_to_bytes(xb, e->size, v, e->n);
    if (yb) {
        FMUL(v, p->y, zi);
        tlsc_mont_from(&e->fp, v, v);
        tlsc_bn_to_bytes(yb, e->size, v, e->n);
    }
    return 0;
}

/* parses a coordinate (must be < p) into Montgomery form */
static int load_fe(const curve *e, u64 *out, const u8 *b) {
    u64 v[MAXL];
    tlsc_bn_from_bytes(v, e->n, b, e->size);
    if (!tlsc_bn_lt_mask(v, e->fp.m, e->n)) return -1;
    tlsc_mont_to(&e->fp, out, v);
    return 0;
}

static int on_curve(const curve *e, const u64 *x, const u64 *y) {
    u64 y2[MAXL], x2[MAXL], rhs[MAXL], t[MAXL];
    FMUL(y2, y, y);
    FMUL(x2, x, x);
    FMUL(rhs, x2, x);
    mul_a(e, t, x);
    FADD(rhs, rhs, t);
    FADD(rhs, rhs, e->b);
    return tlsc_bn_cmp(y2, rhs, e->n) == 0;
}

static int load_point(const curve *e, pt *p, const u8 *x, const u8 *y) {
    if (load_fe(e, p->x, x) != 0 || load_fe(e, p->y, y) != 0) return -1;
    if (!on_curve(e, p->x, p->y)) return -1;
    memset(p->z, 0, sizeof p->z);
    memcpy(p->z, e->fp.one, e->n * sizeof(u64));
    return 0;
}

int tlsc_ec_point_valid(int id, const u8 *x, const u8 *y) {
    curve e;
    pt p;
    if (curve_setup(&e, id) != 0) return 0;
    return load_point(&e, &p, x, y) == 0;
}

int tlsc_ecdh_keypair(int id, const u8 *random_bytes, u8 *priv, u8 *pub) {
    curve e;
    if (curve_setup(&e, id) != 0) return -1;
    /* k = (hi * 2^(64n) + lo) mod n from 2*size random bytes: the bias is below 2^-(8*size) */
    u64 hi[MAXL], lo[MAXL], d[MAXL], k[MAXL];
    tlsc_bn_from_bytes(hi, e.n, random_bytes, e.size);
    tlsc_bn_from_bytes(lo, e.n, random_bytes + e.size, e.size);
    tlsc_mont_mul(&e.fn, hi, hi, e.fn.rr);                           /* hi * R mod n */
    u64 borrow = tlsc_bn_sub(d, lo, e.fn.m, e.n);
    tlsc_bn_select(lo, d, lo, e.n, 0 - borrow);                      /* lo mod n (R < 2n) */
    tlsc_mont_add(&e.fn, k, hi, lo);
    if (tlsc_bn_is_zero(k, e.n)) {
        tlsc_wipe(&e, sizeof e);
        return -1;
    }
    tlsc_bn_to_bytes(priv, e.size, k, e.n);
    pt r;
    scalar_mult(&e, &r, priv, e.size, &e.g);
    pub[0] = 4;
    int rc = pt_affine_bytes(&e, &r, pub + 1, pub + 1 + e.size);
    tlsc_wipe(&e, sizeof e);
    tlsc_wipe(&r, sizeof r);
    tlsc_wipe(hi, sizeof hi);
    tlsc_wipe(lo, sizeof lo);
    tlsc_wipe(k, sizeof k);
    return rc;
}

int tlsc_ecdh(int id, const u8 *priv, const u8 *peer_pub, size_t peer_len, u8 *shared) {
    curve e;
    if (curve_setup(&e, id) != 0) return -1;
    if (peer_len != 1 + 2 * e.size || peer_pub[0] != 4) return -1;
    pt q, r;
    if (load_point(&e, &q, peer_pub + 1, peer_pub + 1 + e.size) != 0) return -1;
    scalar_mult(&e, &r, priv, e.size, &q);
    int rc = pt_affine_bytes(&e, &r, shared, NULL);
    tlsc_wipe(&e, sizeof e);
    tlsc_wipe(&r, sizeof r);
    return rc;
}

/* plain big-endian bytes (any length, leading zeros allowed) -> n limbs; -1 if the value does not fit */
static int load_scalar(u64 *out, size_t n, const u8 *b, size_t len) {
    while (len > 0 && b[0] == 0) { b++; len--; }
    if (len > 8 * n) return -1;
    tlsc_bn_from_bytes(out, n, b, len);
    return 0;
}

int tlsc_ecdsa_verify(int id, const u8 *pub, size_t publen, const u8 *hash, size_t hashlen, const u8 *rb, size_t rlen, const u8 *sb, size_t slen) {
    curve e;
    if (curve_setup(&e, id) != 0) return -1;
    if (publen != 1 + 2 * e.size || pub[0] != 4) return -1;
    pt q;
    if (load_point(&e, &q, pub + 1, pub + 1 + e.size) != 0) return -1;

    u64 r[MAXL], s[MAXL], h[MAXL];
    if (load_scalar(r, e.n, rb, rlen) != 0 || load_scalar(s, e.n, sb, slen) != 0) return -1;
    if (tlsc_bn_is_zero(r, e.n) || tlsc_bn_is_zero(s, e.n)) return -1;
    if (!tlsc_bn_lt_mask(r, e.fn.m, e.n) || !tlsc_bn_lt_mask(s, e.fn.m, e.n)) return -1;

    /* the leftmost min(bits(n), 8*hashlen) bits of the hash, as an integer reduced modulo n */
    size_t use = hashlen < e.size ? hashlen : e.size;
    tlsc_bn_from_bytes(h, e.n, hash, use);
    u64 d[MAXL];
    u64 borrow = tlsc_bn_sub(d, h, e.fn.m, e.n);
    tlsc_bn_select(h, d, h, e.n, 0 - borrow);

    u64 sm[MAXL], w[MAXL], hm[MAXL], rm[MAXL], u1[MAXL], u2[MAXL];
    tlsc_mont_to(&e.fn, sm, s);
    tlsc_mont_inv(&e.fn, w, sm);                                     /* s^-1 (Montgomery form) */
    tlsc_mont_to(&e.fn, hm, h);
    tlsc_mont_to(&e.fn, rm, r);
    tlsc_mont_mul(&e.fn, u1, hm, w);
    tlsc_mont_mul(&e.fn, u2, rm, w);
    tlsc_mont_from(&e.fn, u1, u1);
    tlsc_mont_from(&e.fn, u2, u2);

    pt res;
    double_scalar_mult(&e, &res, u1, u2, &q);
    if (pt_is_infinity(&e, &res)) return -1;
    u8 xb[48];
    if (pt_affine_bytes(&e, &res, xb, NULL) != 0) return -1;
    u64 x[MAXL];
    tlsc_bn_from_bytes(x, e.n, xb, e.size);
    borrow = tlsc_bn_sub(d, x, e.fn.m, e.n);                         /* x mod n: p < 2n for both curves */
    tlsc_bn_select(x, d, x, e.n, 0 - borrow);
    return tlsc_bn_cmp(x, r, e.n) == 0 ? 0 : -1;
}
