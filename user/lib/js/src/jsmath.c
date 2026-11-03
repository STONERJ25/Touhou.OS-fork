/*
 * jsmath.c: the math library behind Math.* (ECMA-262 21.3) and the ** operator, written for the engine so that the
 * results are the same on TouhouOS (whose libm uses x87 instructions with large errors in pow and in large-argument
 * sin) and on the host.
 *
 * Methods: argument reduction to a small interval, then series evaluated with Horner's rule.  exp, log and the
 * trigonometric kernels are within about one unit in the last place.  pow is computed in double-double arithmetic
 * (about 100 bits: log, a product, exp), which makes it correctly rounded in practice: exact cases such as 10 ** 21,
 * 2 ** -1074 or 3 ** 3 come out exact.  sin/cos/tan reduce small arguments with three exact chunks of pi/2
 * (Cody and Waite) and large ones by multiplying with the bits of 2/pi (Payne and Hanek).  The constants come from
 * gen_math_consts.py (decimal arithmetic from Machin's formula and series), see math_consts.h.
 * Double-double products use Veltkamp/Dekker splitting (no fma: the OS's libm has none).
 */
#include "jsint.h"
#include "math_consts.h"

typedef union { double d; uint64_t u; } DU;
static inline double bits(uint64_t u) { DU v; v.u = u; return v.d; }
static inline uint64_t ubits(double d) { DU v; v.d = d; return v.u; }
#define D(x) bits(x)
static const double INF = __builtin_inf();
#define NANV __builtin_nan("")

/* ------------------------------------------------------------------ exact helpers */

double kz_math_trunc(double x) {
    uint64_t u = ubits(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;                      /* integral, infinite or NaN */
    if (e < 0) return bits(u & 0x8000000000000000ULL);    /* |x| < 1: a signed zero */
    uint64_t mask = (1ULL << (52 - e)) - 1;
    return bits(u & ~mask);
}

double kz_math_floor(double x) {
    double t = kz_math_trunc(x);
    if (t == x) return x;                       /* keeps -0 and NaN */
    if (x < 0) return t - 1.0;
    return t;
}

double kz_math_ceil(double x) {
    double t = kz_math_trunc(x);
    if (t == x) return x;
    if (x > 0) return t + 1.0;
    return t == 0 ? -0.0 : t;                   /* ceil(-0.5) is -0 */
}

double kz_math_round(double x) {
    if (x != x || x == INF || x == -INF) return x;
    double f = kz_math_floor(x);
    if (f == x) return x;
    double r = (x - f >= 0.5) ? f + 1.0 : f;    /* x - f is exact below 2^52 */
    if (r == 0 && (x < 0 || __builtin_signbit(x))) return -0.0;
    return r;
}

double kz_frexp(double x, int *e) {
    uint64_t u = ubits(x);
    int be = (int)((u >> 52) & 0x7FF);
    if (be == 0x7FF || x == 0) { *e = 0; return x; }
    if (be == 0) {                              /* subnormal: scale up first */
        x *= 18014398509481984.0;               /* 2^54 */
        u = ubits(x);
        be = (int)((u >> 52) & 0x7FF) - 54;
    }
    *e = be - 1022;
    return bits((u & ~(0x7FFULL << 52)) | (1022ULL << 52));
}

/* x * 2^e, correctly rounded (also into the subnormal range) */
double kz_ldexp(double x, int e) {
    if (x == 0 || x != x || x == INF || x == -INF) return x;
    int ex;
    double m = kz_frexp(x, &ex);                /* x = m * 2^ex, 0.5 <= |m| < 1 */
    long ne = (long)ex + e;                     /* result = m * 2^ne */
    uint64_t sign = ubits(x) & 0x8000000000000000ULL;
    if (ne > 1024) return bits(sign | 0x7FF0000000000000ULL);
    if (ne >= -1021) return bits(sign | ((uint64_t)(ne + 1022) << 52) | (ubits(m) & ((1ULL << 52) - 1)));
    /* subnormal: the 53-bit mantissa shifted right with rounding half to even */
    uint64_t mant = (ubits(m) & ((1ULL << 52) - 1)) | (1ULL << 52);   /* value = mant * 2^(ne - 53) */
    long sh = -1021 - ne;                       /* bits to drop so the unit becomes 2^-1074 (ne = -1022: 1) */
    if (sh > 53) return bits(sign);
    uint64_t q = mant >> sh, rem = mant & ((1ULL << sh) - 1), half = 1ULL << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return bits(sign | q);
}

/* x % y with the sign of x, exactly (ECMA-262 Number::remainder) */
double kz_math_fmod(double x, double y) {
    if (x != x || y != y || x == INF || x == -INF || y == 0) return NANV;
    if (y == INF || y == -INF || x == 0) return x;
    double ax = __builtin_fabs(x), ay = __builtin_fabs(y);
    if (ax < ay) return x;
    if (ax < 9007199254740992.0 && ay < 9007199254740992.0 && ax == (double)(uint64_t)ax && ay == (double)(uint64_t)ay) {
        double r = (double)((uint64_t)ax % (uint64_t)ay);
        return x < 0 ? -r : r;
    }
    int ex, ey;
    double mx = kz_frexp(ax, &ex), my = kz_frexp(ay, &ey);
    uint64_t ix = (uint64_t)kz_ldexp(mx, 53), iy = (uint64_t)kz_ldexp(my, 53);   /* 53-bit integers */
    /* ax = ix * 2^(ex-53), ay = iy * 2^(ey-53); reduce ix * 2^(ex-ey) modulo iy */
    uint64_t r = ix % iy;
    for (int k = ex - ey; k > 0; k--) {
        r <<= 1;
        if (r >= iy) r -= iy;
    }
    double res = kz_ldexp((double)r, ey - 53);
    return x < 0 ? -res : res;
}

/* ------------------------------------------------------------------ double-double arithmetic */

typedef struct { double hi, lo; } dd;

static inline dd two_sum(double a, double b) {
    double s = a + b, bb = s - a;
    dd r = { s, (a - (s - bb)) + (b - bb) };
    return r;
}
static inline dd quick_two_sum(double a, double b) {
    double s = a + b;
    dd r = { s, b - (s - a) };
    return r;
}
static inline void split(double a, double *hi, double *lo) {
    double t = 134217729.0 * a;                 /* 2^27 + 1 */
    *hi = t - (t - a);
    *lo = a - *hi;
}
static inline dd two_prod(double a, double b) {
    double p = a * b, ah, al, bh, bl;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    dd r = { p, ((ah * bh - p) + ah * bl + al * bh) + al * bl };
    return r;
}
static inline dd dd_add(dd a, dd b) {
    dd s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}
static inline dd dd_mul(dd a, dd b) {
    dd p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd dd_mul_d(dd a, double b) {
    dd p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd dd_div(dd a, dd b) {
    double q1 = a.hi / b.hi;
    dd r = dd_add(a, dd_mul_d(b, -q1));
    double q2 = r.hi / b.hi;
    r = dd_add(r, dd_mul_d(b, -q2));
    double q3 = r.hi / b.hi;
    dd q = quick_two_sum(q1, q2);
    return dd_add(q, (dd){ q3, 0 });
}
static inline dd dd_from(double a) { dd r = { a, 0 }; return r; }

/* ------------------------------------------------------------------ exp and log */

double kz_math_exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return INF;
    if (x < -745.1332191019412) return 0.0;
    double ax = __builtin_fabs(x);
    if (ax < 1.0 / 18014398509481984.0) return 1.0 + x;
    double k = 0, hi = x, lo = 0;
    if (ax > 0.34657359027997264) {             /* ln2 / 2 */
        k = kz_math_round(x * D(KZ_INV_LN2_HI));
        /* x - k ln2 with ln2 in two parts: k * (the first 32 bits of ln2) is exact for |k| < 2^21 */
        hi = x - k * D(KZ_LN2_C_1);
        lo = -k * D(KZ_LN2_C_2);
    }
    double r = hi + lo;
    /* exp(r) - 1 = r + r^2/2! + ... + r^13/13! */
    double p = 1.0 / 6227020800.0;
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    double em1 = r + r * r * p;
    /* add the rounding error of r = hi + lo back: d/dr exp(r) = exp(r) */
    double err = (hi - r) + lo;
    double y = 1.0 + (em1 + err * (1.0 + em1));
    return k ? kz_ldexp(y, (int)k) : y;
}

double kz_math_expm1(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return INF;
    if (x < -40) return -1.0;
    double ax = __builtin_fabs(x);
    if (ax < 1.0 / 18014398509481984.0) return x;
    if (ax <= 0.34657359027997264) {
        double p = 1.0 / 6227020800.0;
        p = p * x + 1.0 / 479001600.0;
        p = p * x + 1.0 / 39916800.0;
        p = p * x + 1.0 / 3628800.0;
        p = p * x + 1.0 / 362880.0;
        p = p * x + 1.0 / 40320.0;
        p = p * x + 1.0 / 5040.0;
        p = p * x + 1.0 / 720.0;
        p = p * x + 1.0 / 120.0;
        p = p * x + 1.0 / 24.0;
        p = p * x + 1.0 / 6.0;
        p = p * x + 0.5;
        return x + x * x * p;
    }
    double k = kz_math_round(x * D(KZ_INV_LN2_HI));
    double r = (x - k * D(KZ_LN2_C_1)) - k * D(KZ_LN2_C_2);
    double e = kz_math_expm1(r);                /* |r| <= ln2/2: the series above */
    if (k > 56) return kz_ldexp(e + 1.0, (int)k) - 1.0;
    double two_k = kz_ldexp(1.0, (int)k);
    return two_k * e + (two_k - 1.0);           /* both terms exact apart from the final rounding */
}

/* log(1 + f) for f = m - 1 with m in [sqrt(1/2), sqrt(2)), via s = f / (2 + f) and the atanh series */
static double log1p_kernel(double f) {
    double s = f / (2.0 + f), z = s * s;
    double R = 2.0 / 23;
    R = R * z + 2.0 / 21;
    R = R * z + 2.0 / 19;
    R = R * z + 2.0 / 17;
    R = R * z + 2.0 / 15;
    R = R * z + 2.0 / 13;
    R = R * z + 2.0 / 11;
    R = R * z + 2.0 / 9;
    R = R * z + 2.0 / 7;
    R = R * z + 2.0 / 5;
    R = R * z + 2.0 / 3;
    R *= z;
    double hfsq = 0.5 * f * f;
    return f - (hfsq - s * (hfsq + R));
}

double kz_math_log(double x) {
    if (x != x || x == INF) return x;
    if (x < 0) return NANV;
    if (x == 0) return -INF;
    if (x == 1) return 0.0;
    int k;
    double m = kz_frexp(x, &k);                 /* x = m * 2^k, m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2; k--; }
    double f = m - 1.0;
    double l = log1p_kernel(f);
    if (!k) return l;
    return k * D(KZ_LN2_HI) + (l + k * D(KZ_LN2_LO));
}

double kz_math_log1p(double x) {
    if (x != x || x == INF) return x;
    if (x < -1) return NANV;
    if (x == -1) return -INF;
    if (__builtin_fabs(x) < 1.0 / 18014398509481984.0) return x;
    if (x > -0.2928932188134524 && x < 0.41421356237309503) return log1p_kernel(x);
    double u = 1.0 + x;
    double c = (u > 2) ? 1.0 - (u - x) : x - (u - 1.0);    /* the part of x lost in 1 + x */
    return kz_math_log(u) + c / u;
}

/* log(x) as a double-double, x > 0 finite */
static dd log_dd(double x) {
    int k;
    double m = kz_frexp(x, &k);
    if (m < 0.70710678118654752440) { m *= 2; k--; }
    double f = m - 1.0;                         /* exact */
    dd den = two_sum(2.0, f);
    dd s = dd_div(dd_from(f), den);
    dd z = dd_mul(s, s);
    /* S = sum z^n / (2n+1), n = 0..23 */
    dd S = { kz_inv_odd[23][0], kz_inv_odd[23][1] };
    for (int n = 22; n >= 0; n--) S = dd_add(dd_mul(S, z), (dd){ kz_inv_odd[n][0], kz_inv_odd[n][1] });
    dd l = dd_mul_d(dd_mul(s, S), 2.0);
    if (k) {
        dd ln2 = { D(KZ_LN2_HI), D(KZ_LN2_LO) };
        l = dd_add(l, dd_mul_d(ln2, (double)k));
    }
    return l;
}

/* exp of a double-double t; |t.hi| < 709.8 */
static double exp_dd(dd t) {
    dd ln2 = { D(KZ_LN2_HI), D(KZ_LN2_LO) };
    double k = kz_math_round(t.hi * D(KZ_INV_LN2_HI));
    dd r = dd_add(t, dd_mul_d(ln2, -k));
    /* reduce further by 2^-8 and square back up */
    r.hi = kz_ldexp(r.hi, -8);
    r.lo = kz_ldexp(r.lo, -8);
    /* e = exp(r) - 1 = r (1/1! + r/2! + ... + r^10/11!) */
    dd e = { kz_inv_fact[11][0], kz_inv_fact[11][1] };
    for (int n = 10; n >= 1; n--) e = dd_add(dd_mul(e, r), (dd){ kz_inv_fact[n][0], kz_inv_fact[n][1] });
    e = dd_mul(e, r);
    /* (1 + e)^256: e' = 2e + e^2, eight times */
    for (int i = 0; i < 8; i++) e = dd_add(dd_mul_d(e, 2.0), dd_mul(e, e));
    dd y = dd_add(dd_from(1.0), e);
    int ki = (int)k;
    if (ki > -1021) return kz_ldexp(y.hi + y.lo, ki);
    /* subnormal result: scale the parts separately so the final rounding happens once */
    double h = kz_ldexp(y.hi, ki + 60), l = kz_ldexp(y.lo, ki + 60);
    return kz_ldexp(h + l, -60);
}

static bool is_odd_integer(double y) {
    if (__builtin_fabs(y) >= 9007199254740992.0) return false;   /* large doubles are even */
    double t = kz_math_trunc(y);
    if (t != y) return false;
    return kz_math_fmod(t, 2.0) != 0;
}

static bool is_integer(double y) { return y == kz_math_trunc(y) && y != INF && y != -INF; }

double kz_math_pow(double x, double y) {
    if (y != y) return NANV;
    if (y == 0) return 1.0;
    if (x != x) return NANV;
    double ax = __builtin_fabs(x);
    if (y == INF) return ax > 1 ? INF : ax == 1 ? NANV : 0.0;
    if (y == -INF) return ax > 1 ? 0.0 : ax == 1 ? NANV : INF;
    if (x == INF) return y > 0 ? INF : 0.0;
    if (x == -INF) {
        bool odd = is_odd_integer(y);
        if (y > 0) return odd ? -INF : INF;
        return odd ? -0.0 : 0.0;
    }
    if (x == 0) {
        bool odd = is_odd_integer(y), neg = __builtin_signbit(x);
        if (y > 0) return (neg && odd) ? -0.0 : 0.0;
        return (neg && odd) ? -INF : INF;
    }
    bool negate = false;
    if (x < 0) {
        if (!is_integer(y)) return NANV;
        negate = is_odd_integer(y);
    }
    if (y == 1) return x;
    if (y == 2) return x * x;
    if (y == -1) return 1.0 / x;
    if (y == 0.5) return __builtin_sqrt(x);     /* x > 0 here */
    if (ax == 1) return negate ? -1.0 : 1.0;
    dd l = log_dd(ax);
    dd t = dd_add(two_prod(l.hi, y), dd_from(l.lo * y));
    double r;
    if (t.hi > 709.8) r = INF;
    else if (t.hi < -745.2) r = 0.0;
    else r = exp_dd(t);
    return negate ? -r : r;
}

double kz_math_log2(double x) {
    if (x != x || x == INF) return x;
    if (x < 0) return NANV;
    if (x == 0) return -INF;
    int k;
    double m = kz_frexp(x, &k);
    if (m == 0.5) return (double)(k - 1);      /* exact powers of two */
    dd l = log_dd(x);
    dd il2 = { D(KZ_INV_LN2_HI), D(KZ_INV_LN2_LO) };
    dd r = dd_mul(l, il2);
    return r.hi + r.lo;
}

double kz_math_log10(double x) {
    if (x != x || x == INF) return x;
    if (x < 0) return NANV;
    if (x == 0) return -INF;
    if (x == 1) return 0.0;
    dd l = log_dd(x);
    dd il10 = { D(KZ_INV_LN10_HI), D(KZ_INV_LN10_LO) };
    dd r = dd_mul(l, il10);
    return r.hi + r.lo;
}

/* ------------------------------------------------------------------ trigonometry */

/* sin and cos on [-pi/4, pi/4] with a correction term lo (the argument is hi + lo) */
static double ksin(double x, double lo) {
    double z = x * x;
    double p = -1.0 / 121645100408832000.0;     /* -1/19! */
    p = p * z + 1.0 / 355687428096000.0;        /* 1/17! */
    p = p * z - 1.0 / 1307674368000.0;
    p = p * z + 1.0 / 6227020800.0;
    p = p * z - 1.0 / 39916800.0;
    p = p * z + 1.0 / 362880.0;
    p = p * z - 1.0 / 5040.0;
    p = p * z + 1.0 / 120.0;
    p = p * z - 1.0 / 6.0;
    return x + (x * z * p + lo * (1.0 - 0.5 * z));
}

static double kcos(double x, double lo) {
    double z = x * x;
    double p = 1.0 / 2432902008176640000.0;     /* 1/20! */
    p = p * z - 1.0 / 6402373705728000.0;
    p = p * z + 1.0 / 20922789888000.0;
    p = p * z - 1.0 / 87178291200.0;
    p = p * z + 1.0 / 479001600.0;
    p = p * z - 1.0 / 3628800.0;
    p = p * z + 1.0 / 40320.0;
    p = p * z - 1.0 / 720.0;
    p = p * z + 1.0 / 24.0;
    double hz = 0.5 * z, w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * z * p - x * lo));
}

/* x = n * pi/2 + (hi + lo), |hi| <= pi/4; returns n mod 4 */
static int reduce(double x, double *hi, double *lo) {
    double ax = __builtin_fabs(x);
    if (ax <= D(KZ_PIO4_HI)) { *hi = x; *lo = 0; return 0; }
    if (ax < 823550.0) {                        /* 2^19 * pi/2: n has at most 20 bits */
        double n = kz_math_round(x * (2.0 / D(KZ_PI_HI)));
        double t = x - n * D(KZ_PIO2_C_1);      /* exact */
        dd a = two_sum(t, -n * D(KZ_PIO2_C_2));  /* n * C2 is exact */
        dd b = two_sum(a.hi, -n * D(KZ_PIO2_C_3));
        b.lo += a.lo;
        dd r = quick_two_sum(b.hi, b.lo);
        /* the rounded n can leave |r| just above pi/4: harmless for the kernels */
        *hi = r.hi;
        *lo = r.lo;
        return (int)((int64_t)n & 3);
    }
    /* Payne-Hanek: x = m * 2^e with m a 53-bit integer; x * 2/pi mod 4 from a window of the bits of 2/pi */
    int ex;
    double fm = kz_frexp(ax, &ex);
    uint64_t m = (uint64_t)kz_ldexp(fm, 53);    /* ax = m * 2^(ex - 53) */
    int e = ex - 53;                            /* ax = m * 2^e, e >= 0 here */
    /* bits of 2/pi numbered 1, 2, ... after the point; bit i has weight 2^-i.  Terms with e - i >= 2 are multiples of
       4: start the window at bit i0 = e - 1 (weight 2^(e - i0) = 2, together with m that is the bit worth 2). */
    int i0 = e - 1;
    /* collect 192 bits of the window [i0, i0 + 192) into w[0..5] (w[0] the most significant) */
    uint32_t w[6];
    for (int j = 0; j < 6; j++) {
        uint32_t v = 0;
        for (int b = 0; b < 32; b++) {
            int i = i0 + j * 32 + b;            /* bit index, 1-based */
            uint32_t bit = 0;
            if (i >= 1) {
                int wi = (i - 1) / 32, bi = 31 - (i - 1) % 32;
                bit = wi < 48 ? (kz_two_over_pi[wi] >> bi) & 1 : 0;
            }
            v = (v << 1) | bit;
        }
        w[j] = v;
    }
    /* P = m * W (W the 192-bit integer); keep it mod 2^192: then x * 2/pi mod 4 = P * 2^-190 */
    uint32_t p[7] = { 0 };
    uint64_t mlo = m & 0xFFFFFFFFu, mhi = m >> 32;
    for (int j = 5; j >= 0; j--) {              /* w[5] is the least significant word */
        int pos = 5 - j;                        /* word position from the bottom */
        uint64_t t1 = (uint64_t)w[j] * mlo, t2 = (uint64_t)w[j] * mhi;
        uint64_t carry = 0;
        uint64_t add[3] = { t1 & 0xFFFFFFFFu, (t1 >> 32) + (t2 & 0xFFFFFFFFu), t2 >> 32 };
        for (int k = 0; k < 3 && pos + k < 7; k++) {
            uint64_t s = (uint64_t)p[pos + k] + add[k] + carry;
            p[pos + k] = (uint32_t)s;
            carry = s >> 32;
        }
        for (int k = pos + 3; carry && k < 7; k++) {
            uint64_t s = (uint64_t)p[k] + carry;
            p[k] = (uint32_t)s;
            carry = s >> 32;
        }
    }
    /* p[5] holds bits 160..191: the top 2 bits of the 192-bit value are the quadrant */
    int n = (int)(p[5] >> 30);
    /* the fraction: the remaining 190 bits, as a fixed-point number in [0, 1) */
    uint64_t f_hi = ((uint64_t)(p[5] & 0x3FFFFFFFu) << 34) | ((uint64_t)p[4] << 2) | (p[3] >> 30);
    uint64_t f_lo = ((uint64_t)(p[3] & 0x3FFFFFFFu) << 34) | ((uint64_t)p[2] << 2) | (p[1] >> 30);
    double fh = kz_ldexp((double)f_hi, -64), fl = kz_ldexp((double)f_lo, -128);
    dd frac = two_sum(fh, fl);
    if (frac.hi >= 0.5) { frac = dd_add(frac, dd_from(-1.0)); n++; }
    dd pio2 = { D(KZ_PIO2_HI), D(KZ_PIO2_LO) };
    dd r = dd_mul(frac, pio2);
    if (x < 0) { r.hi = -r.hi; r.lo = -r.lo; n = -n; }
    *hi = r.hi;
    *lo = r.lo;
    return n & 3;
}

double kz_math_sin(double x) {
    if (x != x || x == INF || x == -INF) return NANV;
    if (x == 0) return x;
    double hi, lo;
    int n = reduce(x, &hi, &lo);
    switch (n) {
    case 0: return ksin(hi, lo);
    case 1: return kcos(hi, lo);
    case 2: return -ksin(hi, lo);
    default: return -kcos(hi, lo);
    }
}

double kz_math_cos(double x) {
    if (x != x || x == INF || x == -INF) return NANV;
    double hi, lo;
    int n = reduce(x, &hi, &lo);
    switch (n) {
    case 0: return kcos(hi, lo);
    case 1: return -ksin(hi, lo);
    case 2: return -kcos(hi, lo);
    default: return ksin(hi, lo);
    }
}

double kz_math_tan(double x) {
    if (x != x || x == INF || x == -INF) return NANV;
    if (x == 0) return x;
    double hi, lo;
    int n = reduce(x, &hi, &lo);
    double s = ksin(hi, lo), c = kcos(hi, lo);
    return (n & 1) ? -c / s : s / c;
}

/* atan for 0 <= x, by breakpoints: atan(x) = atan(c) + atan((x - c) / (1 + x c)) */
static double atan_pos(double x) {
    if (x > 7.3786976294838206e19) return D(KZ_PIO2_HI) + D(KZ_PIO2_LO);     /* 2^66 */
    double base_hi = 0, base_lo = 0, t;
    if (x < 0.4375) t = x;
    else if (x < 0.6875) { t = (2.0 * x - 1.0) / (2.0 + x); base_hi = D(KZ_ATAN_HALF_HI); base_lo = D(KZ_ATAN_HALF_LO); }
    else if (x < 1.1875) { t = (x - 1.0) / (x + 1.0); base_hi = D(KZ_ATAN_ONE_HI); base_lo = D(KZ_ATAN_ONE_LO); }
    else if (x < 2.4375) { t = (x - 1.5) / (1.0 + 1.5 * x); base_hi = D(KZ_ATAN_THREEHALF_HI); base_lo = D(KZ_ATAN_THREEHALF_LO); }
    else { t = -1.0 / x; base_hi = D(KZ_PIO2_HI); base_lo = D(KZ_PIO2_LO); }
    /* atan(t) = t - t^3/3 + ... for |t| <= 0.4375: 27 terms */
    double z = t * t;
    double p = 0;
    for (int k = 27; k >= 1; k--) p = p * z + ((k & 1) ? -1.0 : 1.0) / (2 * k + 1);
    double a = t + t * z * p;
    if (base_hi == 0) return a;
    return base_hi + (a + base_lo);
}

double kz_math_atan(double x) {
    if (x != x) return x;
    if (__builtin_fabs(x) < 1.0 / 134217728.0) return x;
    return x < 0 ? -atan_pos(-x) : atan_pos(x);
}

double kz_math_atan2(double y, double x) {
    if (x != x || y != y) return NANV;
    bool sy = __builtin_signbit(y), sx = __builtin_signbit(x);
    double pi = D(KZ_PI_HI), pio2 = D(KZ_PIO2_HI);
    if (y == 0) {
        if (x > 0 || (x == 0 && !sx)) return y;            /* +-0 */
        return sy ? -pi : pi;
    }
    if (x == 0) return sy ? -pio2 : pio2;
    if (x == INF) {
        if (y == INF) return D(KZ_PIO4_HI);
        if (y == -INF) return -D(KZ_PIO4_HI);
        return sy ? -0.0 : 0.0;
    }
    if (x == -INF) {
        if (y == INF) return 3 * D(KZ_PIO4_HI);
        if (y == -INF) return -3 * D(KZ_PIO4_HI);
        return sy ? -pi : pi;
    }
    if (y == INF) return pio2;
    if (y == -INF) return -pio2;
    double ay = __builtin_fabs(y), ax = __builtin_fabs(x);
    double a;
    if (ay / 7.3786976294838206e19 > ax) a = D(KZ_PIO2_HI) + D(KZ_PIO2_LO);
    else if (ax / 7.3786976294838206e19 > ay && sx) a = 0;
    else a = atan_pos(ay / ax);
    if (sx) a = (D(KZ_PI_HI) - a) + D(KZ_PI_LO);
    return sy ? -a : a;
}

double kz_math_asin(double x) {
    if (x != x || x > 1 || x < -1) return NANV;
    if (__builtin_fabs(x) < 1.0 / 134217728.0) return x;
    double ax = __builtin_fabs(x);
    double r = kz_math_atan2(ax, __builtin_sqrt((1.0 - ax) * (1.0 + ax)));
    return x < 0 ? -r : r;
}

double kz_math_acos(double x) {
    if (x != x || x > 1 || x < -1) return NANV;
    if (x == 1) return 0.0;
    return kz_math_atan2(__builtin_sqrt((1.0 - x) * (1.0 + x)), x);
}

/* ------------------------------------------------------------------ hyperbolic functions */

double kz_math_sinh(double x) {
    if (x != x || x == INF || x == -INF) return x;
    double ax = __builtin_fabs(x);
    if (ax < 1.0 / 268435456.0) return x;
    double r;
    if (ax < 22) {
        double t = kz_math_expm1(ax);
        r = 0.5 * (t + t / (t + 1.0));
    } else if (ax < 709.78) r = 0.5 * kz_math_exp(ax);
    else {
        double w = kz_math_exp(0.5 * ax);
        r = (0.5 * w) * w;
    }
    return x < 0 ? -r : r;
}

double kz_math_cosh(double x) {
    if (x != x) return x;
    double ax = __builtin_fabs(x);
    if (ax == INF) return INF;
    if (ax < 0.34657359027997264) {
        double t = kz_math_expm1(ax);
        return 1.0 + (t * t) / (2.0 * (1.0 + t));
    }
    if (ax < 22) {
        double t = kz_math_exp(ax);
        return 0.5 * t + 0.5 / t;
    }
    if (ax < 709.78) return 0.5 * kz_math_exp(ax);
    double w = kz_math_exp(0.5 * ax);
    return (0.5 * w) * w;
}

double kz_math_tanh(double x) {
    if (x != x) return x;
    double ax = __builtin_fabs(x), r;
    if (ax == INF) return x < 0 ? -1.0 : 1.0;
    if (ax < 1.0 / 268435456.0) return x;
    if (ax > 22) r = 1.0;
    else if (ax >= 1) {
        double t = kz_math_expm1(2 * ax);
        r = 1.0 - 2.0 / (t + 2.0);
    } else {
        double t = kz_math_expm1(-2 * ax);
        r = -t / (t + 2.0);
    }
    return x < 0 ? -r : r;
}

double kz_math_asinh(double x) {
    if (x != x || x == INF || x == -INF) return x;
    double ax = __builtin_fabs(x), r;
    if (ax < 1.0 / 268435456.0) return x;
    if (ax > 268435456.0) r = kz_math_log(ax) + D(KZ_LN2_HI);
    else if (ax > 2) r = kz_math_log(2 * ax + 1.0 / (__builtin_sqrt(x * x + 1.0) + ax));
    else {
        double t = x * x;
        r = kz_math_log1p(ax + t / (1.0 + __builtin_sqrt(1.0 + t)));
    }
    return x < 0 ? -r : r;
}

double kz_math_acosh(double x) {
    if (x != x) return x;
    if (x < 1) return NANV;
    if (x == 1) return 0.0;
    if (x == INF) return INF;
    if (x > 268435456.0) return kz_math_log(x) + D(KZ_LN2_HI);
    if (x > 2) return kz_math_log(2 * x - 1.0 / (x + __builtin_sqrt(x * x - 1.0)));
    double t = x - 1.0;
    return kz_math_log1p(t + __builtin_sqrt(2.0 * t + t * t));
}

double kz_math_atanh(double x) {
    if (x != x) return x;
    double ax = __builtin_fabs(x);
    if (ax > 1) return NANV;
    if (ax == 1) return x > 0 ? INF : -INF;
    if (ax < 1.0 / 268435456.0) return x;
    double r;
    if (ax < 0.5) {
        double t = ax + ax;
        r = 0.5 * kz_math_log1p(t + t * ax / (1.0 - ax));
    } else r = 0.5 * kz_math_log1p((ax + ax) / (1.0 - ax));
    return x < 0 ? -r : r;
}

/* ------------------------------------------------------------------ roots */

double kz_math_cbrt(double x) {
    if (x != x || x == 0 || x == INF || x == -INF) return x;
    double ax = __builtin_fabs(x);
    int e;
    double m = kz_frexp(ax, &e);                /* ax = m * 2^e */
    int r = ((e % 3) + 3) % 3;
    int q = (e - r) / 3;
    m = kz_ldexp(m, r);                         /* m in [0.5, 4) and ax = m * 2^(3q) */
    double t = kz_math_exp(kz_math_log(m) / 3.0);
    for (int i = 0; i < 3; i++) t = t - (t * t * t - m) / (3.0 * t * t);
    /* final rounding check: pick the neighbour whose cube is closest (in double-double) */
    double best = t;
    dd c = dd_mul(two_prod(t, t), dd_from(t));
    double besterr = __builtin_fabs((c.hi - m) + c.lo);
    for (int d = -1; d <= 1; d += 2) {
        double u = bits(ubits(t) + (uint64_t)(int64_t)d);
        dd cu = dd_mul(two_prod(u, u), dd_from(u));
        double err = __builtin_fabs((cu.hi - m) + cu.lo);
        if (err < besterr) { besterr = err; best = u; }
    }
    double res = kz_ldexp(best, q);
    return x < 0 ? -res : res;
}

double kz_math_hypot2(double x, double y) {
    double ax = __builtin_fabs(x), ay = __builtin_fabs(y);
    if (ax == INF || ay == INF) return INF;
    if (ax != ax || ay != ay) return NANV;
    if (ax < ay) { double t = ax; ax = ay; ay = t; }
    if (ax == 0) return 0.0;
    double r = ay / ax;
    dd s = dd_add(dd_from(1.0), two_prod(r, r));
    double q = __builtin_sqrt(s.hi);
    /* one Newton correction in double-double: sqrt(s) ~ q + (s - q^2) / (2q) */
    dd q2 = two_prod(q, q);
    double corr = ((s.hi - q2.hi) - q2.lo + s.lo) / (2.0 * q);
    return ax * (q + corr);
}
