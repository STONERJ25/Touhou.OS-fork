/*
 * X25519 and Ed25519 with the field arithmetic of TweetNaCl (sixteen 16-bit limbs in int64_t; small and easy to check, not fast: a scalar
 * multiplication is a few milliseconds on a PC). The curve constants were recomputed from their definitions (d = -121665/121666, the base point
 * with y = 4/5, sqrt(-1) = 2^((p-1)/4)); the vectors in tests/case_sshcrypto.py come from OpenSSL.
 */
#include <sshcrypto.h>
#include <string.h>

typedef int64_t i64;
typedef uint64_t u64;
typedef i64 gf[16];

static const gf gf0;
static const gf gf1 = { 1 };
static const gf K121665 = { 0xDB41, 1 };
static const gf D = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
static const gf D2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
static const gf GX = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
static const gf GY = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
static const gf SQRTM1 = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };
static const u64 L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

static void set25519(gf r, const gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }

static void car25519(gf o) {
    i64 c;
    for (int i = 0; i < 16; i++) {
        o[i] += (i64)1 << 16;
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;                          /* (c is negative when the limb was: shifting a negative value left is undefined in C17, the product is not) */
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    int i, j, b;
    gf m, t;
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return ct_memcmp(c, d, 32);
}

static uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void pow2523(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

/* ---------------------------------------------------------------- X25519 */

static void scalarmult25519(uint8_t *q, const uint8_t *n, const uint8_t *p) {
    uint8_t z[32];
    i64 x[80], r;
    int i;
    gf a, b, c, d, e, f;
    for (i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, K121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, (int)r);
        sel25519(c, d, (int)r);
    }
    for (i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv25519(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    pack25519(q, x + 16);
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    scalarmult25519(out, scalar, point);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++) zero |= out[i];
    return zero ? 0 : -1;
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t nine[32] = { 9 };
    scalarmult25519(out, scalar, nine);
}

/* ---------------------------------------------------------------- Ed25519 */

static void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    set25519(q[0], GX);
    set25519(q[1], GY);
    set25519(q[2], gf1);
    M(q[3], GX, GY);
    scalarmult(p, q, s);
}

static void modL(uint8_t *r, i64 x[64]) {
    i64 carry, i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (i64)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (i64)L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * (i64)L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t *r) {                          /* r[0..63] as a 512-bit little-endian number -> r[0..31] mod L */
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

static int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], SQRTM1);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

static void expand_seed(uint8_t d[64], const uint8_t seed[32]) {
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, seed, 32);
    sha512_final(&c, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

void ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]) {
    uint8_t d[64];
    gf p[4];
    expand_seed(d, seed);
    scalarbase(p, d);
    pack(pk, p);
    memset(d, 0, sizeof d);
}

void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t seed[32], const uint8_t pk[32]) {
    uint8_t d[64], h[64], r[64];
    i64 x[64];
    gf p[4];
    sha512_ctx c;
    expand_seed(d, seed);
    sha512_init(&c);
    sha512_update(&c, d + 32, 32);
    sha512_update(&c, msg, n);
    sha512_final(&c, r);
    reduce(r);
    scalarbase(p, r);
    pack(sig, p);                                          /* R */
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pk, 32);
    sha512_update(&c, msg, n);
    sha512_final(&c, h);
    reduce(h);
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += (i64)h[i] * (i64)d[j];
    modL(sig + 32, x);                                     /* S = r + h * a mod L */
    memset(d, 0, sizeof d);
    memset(r, 0, sizeof r);
}

int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]) {
    uint8_t t[32], h[64];
    gf p[4], q[4];
    sha512_ctx c;
    /* S must be below the group order (RFC 8032, 5.1.7) */
    int lt = 0;
    for (int i = 31; i >= 0; i--) {
        if (sig[32 + i] < L[i]) { lt = 1; break; }
        if (sig[32 + i] > L[i]) break;
    }
    if (!lt) return -1;
    if (unpackneg(q, pk)) return -1;
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pk, 32);
    sha512_update(&c, msg, n);
    sha512_final(&c, h);
    reduce(h);
    scalarmult(p, q, h);                                   /* h * (-A) */
    scalarbase(q, sig + 32);                               /* S * B */
    add(p, q);
    pack(t, p);
    return ct_memcmp(sig, t, 32) ? -1 : 0;
}
