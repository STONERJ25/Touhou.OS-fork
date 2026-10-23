/* libm.
 *
 * The elementary functions are computed in long double (the x87's 64-bit significand) with plain additions, multiplications,
 * divisions and square roots (all exactly rounded, on real hardware and in an emulator alike): a range reduction followed by a
 * short series, and the result is rounded to double once. They are accurate to well below one unit in the last place and
 * correctly rounded in nearly every case. sin/cos/tan reduce their argument with a Payne-Hanek table of 2/pi, so even 1e300
 * works. pow is exact whenever the result is a double (pow(10, 15) == 1e15, pow(2, -1074) is the smallest subnormal). fma is
 * done with integer arithmetic and one rounding. See the tests in tests/case_libc.py.
 *
 * Nothing sets errno (the library is built with -fno-math-errno): domain errors return NaN, overflow returns +-HUGE_VAL. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "fpconv.h"

typedef long double ld;
typedef unsigned __int128 u128;

static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, sizeof d); return d; }

int signbit_d(double x) { return (int)(d2u(x) >> 63); }

#define SIGN_BIT 0x8000000000000000ULL

double fabs(double x) { return u2d(d2u(x) & ~SIGN_BIT); }
double copysign(double x, double y) { return u2d((d2u(x) & ~SIGN_BIT) | (d2u(y) & SIGN_BIT)); }
double sqrt(double x) { return __builtin_sqrt(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }

/* ---------------------------------------------------------------- long double helpers */

typedef union { ld v; struct { uint64_t m; uint16_t se; } p; } LdBits;

static inline ld ld_pow2(int n) {                       /* 2^n for -16382 <= n <= 16383 */
    LdBits b;
    memset(&b, 0, sizeof b);
    b.p.m = 1ULL << 63;
    b.p.se = (uint16_t)(n + 16383);
    return b.v;
}

static inline ld ld_rint(ld x) {                        /* nearest integer, ties to even; |x| < 2^62 */
    const ld magic = 13835058055282163712.0L;           /* 1.5 * 2^63 */
    return (x + magic) - magic;
}

#define PI_L      3.14159265358979323846264338328L
#define PI_2_L    1.57079632679489661923132169164L
#define LN2_L     0.693147180559945309417232121458L
#define LN2_HI    0.693147180369123816490173339844L     /* 32 significant bits: n * LN2_HI is exact */
#define LN2_LO    1.90821492927058781614426568076e-10L
#define LOG2E_L   1.44269504088896340735992468100L
#define LOG10_2_L 0.301029995663981195213738894724L
#define LOG10E_L  0.434294481903251827651128918917L
#define SQRT2_L   1.41421356237309504880168872421L

static const ld INV_FACT[23] = {
    1.0L, 1.0L, 1.0L / 2.0L, 1.0L / 6.0L, 1.0L / 24.0L, 1.0L / 120.0L, 1.0L / 720.0L, 1.0L / 5040.0L, 1.0L / 40320.0L,
    1.0L / 362880.0L, 1.0L / 3628800.0L, 1.0L / 39916800.0L, 1.0L / 479001600.0L, 1.0L / 6227020800.0L, 1.0L / 87178291200.0L,
    1.0L / 1307674368000.0L, 1.0L / 20922789888000.0L, 1.0L / 355687428096000.0L, 1.0L / 6402373705728000.0L,
    1.0L / 121645100408832000.0L, 1.0L / 2432902008176640000.0L, 1.0L / 51090942171709440000.0L,
    1.0L / 1124000727777607680000.0L,
};

/* 1/(2k+1) for the logarithm and arctangent series */
static const ld ODD_RECIP[17] = {
    1.0L, 1.0L / 3.0L, 1.0L / 5.0L, 1.0L / 7.0L, 1.0L / 9.0L, 1.0L / 11.0L, 1.0L / 13.0L, 1.0L / 15.0L, 1.0L / 17.0L,
    1.0L / 19.0L, 1.0L / 21.0L, 1.0L / 23.0L, 1.0L / 25.0L, 1.0L / 27.0L, 1.0L / 29.0L, 1.0L / 31.0L, 1.0L / 33.0L,
};

/* e^r - 1 for |r| <= 0.36, with full relative accuracy for tiny r. */
static ld ld_expm1_small(ld r) {
    ld p = INV_FACT[17];
    for (int k = 16; k >= 2; k--) p = p * r + INV_FACT[k];
    return r + r * r * p;
}

/* e^x for |x| < 11000 (the result may be far outside the double range). */
static ld ld_exp(ld x) {
    ld n = ld_rint(x * LOG2E_L);
    ld r = (x - n * LN2_HI) - n * LN2_LO;
    ld e = 1.0L + ld_expm1_small(r);
    return e * ld_pow2((int)n);
}

/* ln x for finite x > 0, as e*ln2 + ln m with m in [sqrt(1/2), sqrt(2)); *eo and *lm receive e and ln m. */
static void ld_log_parts(ld x, int *eo, ld *lm) {
    LdBits b;
    b.v = x;
    int e = (int)(b.p.se & 0x7FFF) - 16383;
    b.p.se = 16383;
    ld m = b.v;                                         /* [1, 2) */
    if (m > SQRT2_L) { m *= 0.5L; e++; }
    ld s = (m - 1.0L) / (m + 1.0L), z = s * s;
    ld p = 2.0L * ODD_RECIP[14];
    for (int k = 13; k >= 0; k--) p = p * z + 2.0L * ODD_RECIP[k];
    *eo = e;
    *lm = s * p;
}

static ld ld_log(ld x) {
    int e;
    ld lm;
    ld_log_parts(x, &e, &lm);
    return (e * LN2_HI + lm) + e * LN2_LO;
}

/* ln(1+x) for x > -1 */
static ld ld_log1p(ld x) {
    if (x > 0.35L || x < -0.29L) return ld_log(1.0L + x);
    ld s = x / (2.0L + x), z = s * s;
    ld p = 2.0L * ODD_RECIP[15];
    for (int k = 14; k >= 0; k--) p = p * z + 2.0L * ODD_RECIP[k];
    return s * p;
}

/* atan(t) for t >= 0 */
static ld ld_atan_pos(ld t) {
    int inv = 0;
    if (t > 1.0L) { t = 1.0L / t; inv = 1; }
    t = t / (1.0L + __builtin_sqrtl(1.0L + t * t));     /* atan t = 2 atan(t / (1 + sqrt(1 + t^2))), twice */
    t = t / (1.0L + __builtin_sqrtl(1.0L + t * t));
    ld z = t * t, p = 0;                                /* t - t^3/3 + t^5/5 - ..., Horner in -z */
    for (int k = 16; k >= 0; k--) p = ODD_RECIP[k] - z * p;
    ld a = 4.0L * t * p;
    return inv ? PI_2_L - a : a;
}

/* sin r and cos r for |r| <= 0.8: Taylor series, shorter for small r (the truncation error stays below 2^-70 of the result) */
static ld ld_sin_r(ld r) {
    ld a = __builtin_fabsl(r), z = r * r, s = 0;
    int last = a < 0.1L ? 5 : a < 0.3L ? 7 : 10;
    for (int k = last; k >= 0; k--) s = INV_FACT[2 * k + 1] - z * s;
    return r * s;
}

static ld ld_cos_r(ld r) {
    ld a = __builtin_fabsl(r), z = r * r, c = 0;
    int last = a < 0.1L ? 5 : a < 0.3L ? 8 : 11;
    for (int k = last; k >= 0; k--) c = INV_FACT[2 * k] - z * c;
    return c;
}

/* ---------------------------------------------------------------- argument reduction for sin, cos, tan */

/* The binary digits of 2/pi after the point, 64 bits to a word, most significant word first. */
static const uint64_t TWO_OVER_PI[22] = {
    0xA2F9836E4E441529ULL, 0xFC2757D1F534DDC0ULL, 0xDB6295993C439041ULL,
    0xFE5163ABDEBBC561ULL, 0xB7246E3A424DD2E0ULL, 0x06492EEA09D1921CULL,
    0xFE1DEB1CB129A73EULL, 0xE88235F52EBB4484ULL, 0xE99C7026B45F7E41ULL,
    0x3991D639835339F4ULL, 0x9C845F8BBDF9283BULL, 0x1FF897FFDE05980FULL,
    0xEF2F118B5A0A6D1FULL, 0x6D367ECF27CB09B7ULL, 0x4F463F669E5FEA2DULL,
    0x7527BAC7EBE5F17BULL, 0x3D0739F78A5292EAULL, 0x6BFB5FB11F8D5D08ULL,
    0x56033046FC7B6BABULL, 0xF0CFBC209AF4361DULL, 0xA9E391615EE61B08ULL,
    0x6599855F14A06840ULL,
};

static uint64_t get_bits(const uint64_t P[4], int pos, int count) {      /* bits [pos, pos+count) of a 256-bit number */
    int wi = pos >> 6, off = pos & 63;
    uint64_t lo = wi < 4 ? P[wi] : 0, hi = wi + 1 < 4 ? P[wi + 1] : 0;
    uint64_t v = off ? (lo >> off) | (hi << (64 - off)) : lo;
    return count >= 64 ? v : v & ((1ULL << count) - 1);
}

/* Payne-Hanek: ax = n * pi/2 + r with |r| <= pi/4; returns n mod 4 and r (to a relative error near 2^-64) for finite ax >= 0.78.
 * ax = m * 2^e; the digits of 2/pi that matter for x * 2/pi mod 4 are those from position e-1 down to 192 places further. */
static int rem_pio2(double ax, ld *rp) {
    uint64_t bits = d2u(ax);
    int e = (int)(bits >> 52) - 1075;
    uint64_t m = (bits & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    int kmin = e - 1 > 1 ? e - 1 : 1;
    int sh = kmin + 191 - e;                            /* the product has its binary point sh bits from the bottom */
    int b = kmin - 1, wi = b >> 6, off = b & 63;
    uint64_t w[3];
    for (int i = 0; i < 3; i++) {
        uint64_t hi = TWO_OVER_PI[wi + i], lo = TWO_OVER_PI[wi + i + 1];
        w[i] = off ? (hi << off) | (lo >> (64 - off)) : hi;
    }
    uint64_t P[4];
    u128 t = (u128)m * w[2];
    P[0] = (uint64_t)t;
    t = (u128)m * w[1] + (uint64_t)(t >> 64);
    P[1] = (uint64_t)t;
    t = (u128)m * w[0] + (uint64_t)(t >> 64);
    P[2] = (uint64_t)t;
    P[3] = (uint64_t)(t >> 64);
    int n = (int)get_bits(P, sh, 2);
    u128 f = ((u128)get_bits(P, sh - 64, 64) << 64) | get_bits(P, sh - 128, 64);        /* the next 128 bits: the fraction */
    int neg = 0;
    if (f >> 127) { n = (n + 1) & 3; f = (u128)0 - f; neg = 1; }                       /* round to the nearest multiple of pi/2 */
    ld r = 0;
    if (f) {
        int lz = (int)(f >> 64 ? __builtin_clzll((uint64_t)(f >> 64)) : 64 + __builtin_clzll((uint64_t)f));
        uint64_t top = (uint64_t)((f << lz) >> 64);
        r = (ld)top * ld_pow2(-(64 + lz)) * PI_2_L;
    }
    *rp = neg ? -r : r;
    return n;
}

/* ---------------------------------------------------------------- rounding to integers */

double trunc(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;                              /* an integer already, or inf or nan */
    if (e < 0) return u2d(u & SIGN_BIT);                /* |x| < 1: a zero of the same sign */
    return u2d(u & ~((1ULL << (52 - e)) - 1));
}

double floor(double x) {
    double t = trunc(x);
    return (x < 0 && t != x) ? t - 1.0 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return (x > 0 && t != x) ? t + 1.0 : t;
}

double round(double x) {                                /* halves go away from zero */
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += copysign(1.0, x);
    return t;
}

double rint(double x) {                                 /* the current rounding mode: to nearest, ties to even */
    if (fabs(x) >= 4503599627370496.0 || x != x) return x;
    const double c = 4503599627370496.0;
    double y = x < 0 ? (x - c) + c : (x + c) - c;
    return y == 0 ? copysign(0.0, x) : y;
}

double nearbyint(double x) { return rint(x); }
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }

float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float rintf(float x) { return (float)rint(x); }
float nearbyintf(float x) { return (float)rint(x); }
long lroundf(float x) { return (long)round(x); }
long lrintf(float x) { return (long)rint(x); }

/* ---------------------------------------------------------------- fmod, remainder */

static inline ld fprem_ld(ld x, ld y) {                 /* x87 fprem leaves st(1) alone: it is an in/out operand, not a clobber */
    __asm__("1: fprem; fnstsw %%ax; sahf; jp 1b" : "+t"(x), "+u"(y) : : "ax", "cc");
    return x;
}

static inline ld fprem1_step(ld x, ld y, unsigned *sw) {
    unsigned short s;
    __asm__("fprem1; fnstsw %1" : "+t"(x), "=m"(s), "+u"(y));
    *sw = s;
    return x;
}

double fmod(double x, double y) {
    if (y == 0 || x != x || y != y || __builtin_isinf(x)) return NAN;
    if (__builtin_isinf(y) || x == 0) return x;
    if (fabs(x) < fabs(y)) return x;
    return (double)fprem_ld(x, y);
}

double remquo(double x, double y, int *quo) {
    if (y == 0 || x != x || y != y || __builtin_isinf(x)) { *quo = 0; return NAN; }
    if (__builtin_isinf(y)) { *quo = 0; return x; }
    unsigned sw;
    ld r = x, yy = y;
    do r = fprem1_step(r, yy, &sw); while (sw & 0x400);
    int q = (int)(((sw >> 9) & 1) | (((sw >> 14) & 1) << 1) | (((sw >> 8) & 1) << 2));       /* the low three bits of |x/y| */
    *quo = (signbit_d(x) != signbit_d(y)) ? -q : q;
    double res = (double)r;
    return res == 0 ? copysign(0.0, x) : res;
}

double remainder(double x, double y) { int q; return remquo(x, y, &q); }

float fmodf(float x, float y) { return (float)fmod(x, y); }
float remainderf(float x, float y) { return (float)remainder(x, y); }

/* ---------------------------------------------------------------- scaling and exponents */

double scalbn(double x, int n) {
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            x *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {                             /* scale in two steps through 2^-969 so that only the last one rounds */
        x *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    return x * u2d((uint64_t)(0x3FF + n) << 52);
}

double ldexp(double x, int e) { return scalbn(x, e); }
double scalbln(double x, long n) { return scalbn(x, n > 100000 ? 100000 : n < -100000 ? -100000 : (int)n); }
float scalbnf(float x, int n) { return (float)scalbn(x, n); }
float ldexpf(float x, int n) { return (float)scalbn(x, n); }

double frexp(double x, int *e) {
    uint64_t u = d2u(x);
    int ex = (int)((u >> 52) & 0x7FF);
    if (ex == 0x7FF || x == 0) { *e = 0; return x; }
    int adj = 0;
    if (ex == 0) {                                      /* subnormal */
        x *= 0x1p54;
        u = d2u(x);
        ex = (int)((u >> 52) & 0x7FF);
        adj = -54;
    }
    *e = ex - 1022 + adj;
    return u2d((u & ~(0x7FFULL << 52)) | (1022ULL << 52));
}

float frexpf(float x, int *e) { return (float)frexp(x, e); }

int ilogb(double x) {
    uint64_t u = d2u(x);
    int ex = (int)((u >> 52) & 0x7FF);
    if (ex == 0x7FF) return (u << 12) ? FP_ILOGBNAN : INT_MAX;
    if (ex == 0) {
        if (!(u << 1)) return FP_ILOGB0;
        return 63 - __builtin_clzll(u & 0xFFFFFFFFFFFFFULL) - 1074;
    }
    return ex - 1023;
}

double logb(double x) {
    uint64_t u = d2u(x);
    int ex = (int)((u >> 52) & 0x7FF);
    if (ex == 0x7FF) return x * x;                      /* nan, or +inf for +-inf */
    if (x == 0) return -HUGE_VAL;
    return (double)ilogb(x);
}

double modf(double x, double *ip) {
    if (__builtin_isinf(x)) { *ip = x; return copysign(0.0, x); }
    double t = trunc(x);
    *ip = t;
    return copysign(x - t, x);
}

float modff(float x, float *ip) { double d; double r = modf(x, &d); *ip = (float)d; return (float)r; }

double nextafter(double x, double y) {
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    if (x == 0) return copysign(0x1p-1074, y);
    uint64_t u = d2u(x);
    if ((x < y) == (x > 0)) u++; else u--;
    return u2d(u);
}

double nexttoward(double x, long double y) { return nextafter(x, (double)y); }
float nextafterf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    uint32_t u;
    if (x == 0) { u = 1; memcpy(&x, &u, 4); return copysignf(x, y); }
    memcpy(&u, &x, 4);
    if ((x < y) == (x > 0)) u++; else u--;
    memcpy(&x, &u, 4);
    return x;
}

double fdim(double x, double y) {
    if (x != x || y != y) return x + y;
    return x > y ? x - y : 0.0;
}

double fmin(double a, double b) { if (a != a) return b; if (b != b) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (a != a) return b; if (b != b) return a; return a > b ? a : b; }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }
float fdimf(float a, float b) { return (float)fdim(a, b); }

/* ---------------------------------------------------------------- exp, log, pow */

double exp(double x) {
    if (x != x) return x;
    if (x > 710.0) return HUGE_VAL;
    if (x < -746.0) return 0.0;
    return (double)ld_exp(x);
}

double exp2(double x) {
    if (x != x) return x;
    if (x > 1025.0) return HUGE_VAL;
    if (x < -1080.0) return 0.0;
    ld n = ld_rint(x), f = x - n;
    return (double)((1.0L + ld_expm1_small(f * LN2_L)) * ld_pow2((int)n));
}

double expm1(double x) {
    if (x != x) return x;
    if (x > 710.0) return HUGE_VAL;
    if (x < -46.0) return -1.0;                         /* e^x < 2^-66: -1 + e^x rounds to -1 */
    if (fabs(x) < 0x1p-60) return x;
    if (fabs(x) < 0.35) return (double)ld_expm1_small(x);
    return (double)(ld_exp(x) - 1.0L);
}

double log(double x) {
    if (x != x) return x;
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (__builtin_isinf(x)) return x;
    return (double)ld_log(x);
}

double log2(double x) {
    if (x != x) return x;
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (__builtin_isinf(x)) return x;
    int e;
    ld lm;
    ld_log_parts(x, &e, &lm);
    return (double)((ld)e + lm * LOG2E_L);
}

double log10(double x) {
    if (x != x) return x;
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (__builtin_isinf(x)) return x;
    int e;
    ld lm;
    ld_log_parts(x, &e, &lm);
    return (double)(e * LOG10_2_L + lm * LOG10E_L);
}

double log1p(double x) {
    if (x != x) return x;
    if (x < -1.0) return NAN;
    if (x == -1.0) return -HUGE_VAL;
    if (__builtin_isinf(x)) return x;
    if (fabs(x) < 0x1p-60) return x;
    return (double)ld_log1p(x);
}

static int is_odd_integer(double y) {                   /* y finite */
    double ay = fabs(y);
    if (ay >= 9007199254740992.0) return 0;
    uint64_t k = (uint64_t)ay;
    return (double)k == ay && (k & 1);
}

double pow(double x, double y) {
    if (y == 0.0 || x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    double ax = fabs(x), ay = fabs(y);
    int yint = ay >= 9007199254740992.0 || trunc(y) == y;
    int yodd = !__builtin_isinf(y) && is_odd_integer(y);
    if (__builtin_isinf(y)) {
        if (ax == 1.0) return 1.0;
        return ((ax > 1.0) == (y > 0)) ? HUGE_VAL : 0.0;
    }
    if (ax == 0.0) {
        if (y < 0) return yodd ? copysign(HUGE_VAL, x) : HUGE_VAL;
        return yodd ? x : 0.0;
    }
    if (__builtin_isinf(ax)) {
        if (x > 0) return y > 0 ? HUGE_VAL : 0.0;
        if (y > 0) return yodd ? -HUGE_VAL : HUGE_VAL;
        return yodd ? -0.0 : 0.0;
    }
    int neg = 0;
    if (x < 0) {
        if (!yint) return NAN;
        neg = yodd;
    }
    ld res;
    if (yint && ay <= 2147483647.0) {
        /* ax = m * 2^e with m odd; m^k is exact when it fits in 64 bits */
        uint64_t bits = d2u(ax);
        int be = (int)(bits >> 52);
        uint64_t m = bits & 0xFFFFFFFFFFFFFULL;
        long long e;
        if (be == 0) { e = -1074; }
        else { m |= 1ULL << 52; e = be - 1075; }
        int tz = __builtin_ctzll(m);
        m >>= tz;
        e += tz;
        long long k = (long long)ay;
        u128 p = 1;
        int exact = 1;
        if (m > 1) {
            for (long long i = 0; i < k; i++) {
                if (__builtin_mul_overflow(p, (u128)m, &p)) { exact = 0; break; }
            }
        }
        int bl = exact ? (p >> 64 ? 128 - __builtin_clzll((uint64_t)(p >> 64)) : 64 - __builtin_clzll((uint64_t)p)) : 0;
        if (exact && y > 0) {                           /* ax^k = p * 2^E exactly: round that integer once */
            long long E = e * k;
            int drop = bl > 64 ? bl - 64 : 0;
            if (E + drop > 1100) return neg ? -HUGE_VAL : HUGE_VAL;
            if (E + drop < -1300) return neg ? -0.0 : 0.0;
            int sticky = drop && (p & (((u128)1 << drop) - 1)) != 0;
            double r = __fp_round_double((uint64_t)(p >> drop), E + drop, sticky);
            return neg ? -r : r;
        }
        if (exact && y < 0 && bl <= 120) {              /* ax^-k = 2^-E / p: long division gives 62 bits and a sticky bit */
            long long E = e * k;
            int sh = bl + 62;                           /* the quotient floor(2^sh / p) has 62 or 63 bits */
            if (-E - sh > 1100) return neg ? -HUGE_VAL : HUGE_VAL;
            if (-E - sh < -1300) return neg ? -0.0 : 0.0;
            u128 rem = 1;
            uint64_t q = 0;
            for (int i = 0; i < sh; i++) {
                rem <<= 1;
                q <<= 1;
                if (rem >= p) { rem -= p; q |= 1; }
            }
            double r = __fp_round_double(q, -E - sh, rem != 0);
            return neg ? -r : r;
        }
    }
    int e;
    ld lm;
    ld_log_parts(ax, &e, &lm);
    ld l = (ld)y * ((ld)e + lm * LOG2E_L);              /* y * log2(ax) */
    if (l > 1100.0L) return neg ? -HUGE_VAL : HUGE_VAL;
    if (l < -1200.0L) return neg ? -0.0 : 0.0;
    ld n = ld_rint(l), f = l - n;
    res = (1.0L + ld_expm1_small(f * LN2_L)) * ld_pow2((int)n);
    double r = (double)res;
    return neg ? -r : r;
}

double cbrt(double x) {
    if (x != x || x == 0 || __builtin_isinf(x)) return x;
    ld a = fabs(x);
    ld y = ld_exp(ld_log(a) / 3.0L);
    y = y - (y * y * y - a) / (3.0L * y * y);           /* one Newton step in extended precision */
    double r = (double)y;
    return x < 0 ? -r : r;
}

double hypot(double x, double y) {
    if (__builtin_isinf(x) || __builtin_isinf(y)) return HUGE_VAL;
    if (x != x || y != y) return NAN;
    ld a = x, b = y;
    return (double)__builtin_sqrtl(a * a + b * b);
}

/* ---------------------------------------------------------------- hyperbolic functions */

double sinh(double x) {
    if (x != x || __builtin_isinf(x) || x == 0) return x;
    ld a = fabs(x), r;
    if (a > 711.0L) return x < 0 ? -HUGE_VAL : HUGE_VAL;
    if (a < 0.35L) {
        if (a < 0x1p-60) return x;
        ld t = ld_expm1_small(a);
        r = 0.5L * (t + t / (1.0L + t));
    } else {
        ld w = ld_exp(a);
        r = 0.5L * (w - 1.0L / w);
    }
    double d = (double)r;
    return x < 0 ? -d : d;
}

double cosh(double x) {
    if (x != x) return x;
    if (fabs(x) > 711.0) return HUGE_VAL;
    ld w = ld_exp(fabs(x));
    return (double)(0.5L * (w + 1.0L / w));
}

double tanh(double x) {
    if (x != x || x == 0) return x;
    ld a = fabs(x), r;
    if (a > 22.0L) r = 1.0L;
    else if (a < 0.35L) {
        if (a < 0x1p-60) return x;
        ld t = ld_expm1_small(2.0L * a);
        r = t / (t + 2.0L);
    } else {
        ld e2 = ld_exp(2.0L * a);
        r = (e2 - 1.0L) / (e2 + 1.0L);
    }
    double d = (double)r;
    return x < 0 ? -d : d;
}

double asinh(double x) {
    if (x != x || __builtin_isinf(x) || x == 0) return x;
    ld a = fabs(x), r;
    if (a < 0x1p-28L) return x;
    if (a > 0x1p60L) r = ld_log(a) + LN2_L;
    else r = ld_log1p(a + a * a / (1.0L + __builtin_sqrtl(1.0L + a * a)));
    double d = (double)r;
    return x < 0 ? -d : d;
}

double acosh(double x) {
    if (x != x) return x;
    if (x < 1.0) return NAN;
    if (x == 1.0) return 0.0;
    if (__builtin_isinf(x)) return x;
    ld t = (ld)x - 1.0L;
    if (x > 0x1p60) return (double)(ld_log((ld)x) + LN2_L);
    return (double)ld_log1p(t + __builtin_sqrtl(2.0L * t + t * t));
}

double atanh(double x) {
    if (x != x || x == 0) return x;
    double a = fabs(x);
    if (a > 1.0) return NAN;
    if (a == 1.0) return copysign(HUGE_VAL, x);
    if (a < 0x1p-28) return x;
    ld r = 0.5L * ld_log1p(2.0L * a / (1.0L - a));
    double d = (double)r;
    return x < 0 ? -d : d;
}

/* ---------------------------------------------------------------- trigonometry */

double sin(double x) {
    uint64_t u = d2u(x);
    int neg = (int)(u >> 63);
    double ax = fabs(x);
    if (ax < 0x1p-26) return x;                         /* includes +-0; sin x = x - x^3/6 */
    if (!(ax < HUGE_VAL)) return NAN;
    ld r;
    int n = 0;
    if (ax <= 0.7853981633974483) r = ax;
    else n = rem_pio2(ax, &r);
    ld v = (n & 1) ? ld_cos_r(r) : ld_sin_r(r);                       /* n = 0: sin r, 1: cos r, 2: -sin r, 3: -cos r */
    if (n & 2) v = -v;
    double d = (double)v;
    return neg ? -d : d;
}

double cos(double x) {
    double ax = fabs(x);
    if (ax < 0x1p-27) return 1.0;
    if (!(ax < HUGE_VAL)) return NAN;
    ld r;
    int n = 0;
    if (ax <= 0.7853981633974483) r = ax;
    else n = rem_pio2(ax, &r);
    ld v = n == 0 ? ld_cos_r(r) : n == 1 ? -ld_sin_r(r) : n == 2 ? -ld_cos_r(r) : ld_sin_r(r);
    return (double)v;
}

double tan(double x) {
    uint64_t u = d2u(x);
    int neg = (int)(u >> 63);
    double ax = fabs(x);
    if (ax < 0x1p-27) return x;                         /* tan x = x + x^3/3 */
    if (!(ax < HUGE_VAL)) return NAN;
    ld r;
    int n = 0;
    if (ax <= 0.7853981633974483) r = ax;
    else n = rem_pio2(ax, &r);
    ld sn = ld_sin_r(r), cs = ld_cos_r(r);
    ld v = (n & 1) ? -cs / sn : sn / cs;
    double d = (double)v;
    return neg ? -d : d;
}

double atan(double x) {
    if (x != x || x == 0) return x;
    if (__builtin_isinf(x)) return copysign(M_PI_2, x);
    double d = (double)ld_atan_pos(fabs(x));
    return x < 0 ? -d : d;
}

double atan2(double y, double x) {
    if (x != x || y != y) return x + y;
    if (y == 0) {
        if (x > 0 || (x == 0 && !signbit_d(x))) return y;
        return copysign(M_PI, y);
    }
    if (x == 0) return copysign(M_PI_2, y);
    if (__builtin_isinf(x)) {
        if (__builtin_isinf(y)) return copysign(x > 0 ? M_PI_4 : 3 * M_PI_4, y);
        return x > 0 ? copysign(0.0, y) : copysign(M_PI, y);
    }
    if (__builtin_isinf(y)) return copysign(M_PI_2, y);
    ld a = ld_atan_pos((ld)fabs(y) / (ld)fabs(x));
    ld r = x > 0 ? a : PI_L - a;
    double d = (double)r;
    return y < 0 ? -d : d;
}

double asin(double x) {
    if (x != x || x == 0) return x;
    double a = fabs(x);
    if (a > 1.0) return NAN;
    double d;
    if (a == 1.0) d = M_PI_2;
    else {
        ld ax = a;
        d = (double)ld_atan_pos(ax / __builtin_sqrtl((1.0L - ax) * (1.0L + ax)));
    }
    return x < 0 ? -d : d;
}

double acos(double x) {
    if (x != x) return x;
    if (fabs(x) > 1.0) return NAN;
    if (x == 1.0) return 0.0;
    if (x == -1.0) return M_PI;
    if (x == 0) return M_PI_2;
    ld ax = fabs(x);
    ld a = ld_atan_pos(__builtin_sqrtl((1.0L - ax) * (1.0L + ax)) / ax);          /* acos |x| */
    return (double)(x > 0 ? a : PI_L - a);
}

/* ---------------------------------------------------------------- fma */

double fma(double a, double b, double c) {
    if (a != a || b != b || c != c || __builtin_isinf(a) || __builtin_isinf(b) || __builtin_isinf(c) || a == 0 || b == 0)
        return a * b + c;
    if (c == 0) return a * b;
    uint64_t ua = d2u(a), ub = d2u(b), uc = d2u(c);
    int sp = (int)((ua ^ ub) >> 63), sc = (int)(uc >> 63);
    uint64_t ma, mb, mc;
    long long ea, eb, ec;
#define DECOMPOSE(u, m, e) do {                                                                  \
        int be_ = (int)(((u) >> 52) & 0x7FF);                                                    \
        (m) = (u) & 0xFFFFFFFFFFFFFULL;                                                          \
        if (be_) { (m) |= 1ULL << 52; (e) = be_ - 1075; } else { (e) = -1074; }                  \
        int lz_ = __builtin_clzll(m) - 11;                                                       \
        (m) <<= lz_;                                                                             \
        (e) -= lz_;                                                                              \
    } while (0)
    DECOMPOSE(ua, ma, ea);
    DECOMPOSE(ub, mb, eb);
    DECOMPOSE(uc, mc, ec);
#undef DECOMPOSE
    u128 P = (u128)ma * mb;                             /* a * b = P * 2^ep exactly, P has 105 or 106 bits */
    long long ep = ea + eb;
    int bitsP = P >> 64 ? 128 - __builtin_clzll((uint64_t)(P >> 64)) : 64 - __builtin_clzll((uint64_t)P);
    long long tp = bitsP - 1 + ep, tc = 52 + ec;        /* exponents of the leading bits */
    long long top = tp > tc ? tp : tc;
    long long e0 = top - 125;                           /* weight of bit 0 of the 128-bit window */
    u128 X, Y;
    int sticky_x = 0, sticky_y = 0;
    long long shp = ep - e0, shc = ec - e0;
    if (shp >= 0) X = P << shp;
    else if (-shp >= 128) { X = 0; sticky_x = 1; }
    else { X = P >> -shp; sticky_x = (P & (((u128)1 << -shp) - 1)) != 0; }
    if (shc >= 0) Y = (u128)mc << shc;
    else if (-shc >= 128) { Y = 0; sticky_y = 1; }
    else { Y = (u128)mc >> -shc; sticky_y = (((u128)mc) & (((u128)1 << -shc) - 1)) != 0; }
    u128 S;
    int sticky = sticky_x | sticky_y, rneg;
    if (sp == sc) {
        S = X + Y;
        rneg = sp;
    } else if (X > Y || (X == Y && sticky_x)) {
        S = X - Y;
        if (sticky_y) S -= 1;                           /* Y's lost fraction: subtracting Y + 1 leaves a positive remainder */
        rneg = sp;
    } else {
        S = Y - X;
        if (sticky_x) S -= 1;
        rneg = sc;
    }
    if (S == 0 && !sticky) return 0.0;                  /* an exact zero from cancellation is +0 */
    int bl = S >> 64 ? 128 - __builtin_clzll((uint64_t)(S >> 64)) : 64 - __builtin_clzll((uint64_t)S);
    uint64_t q;
    long long e2 = e0;
    if (bl > 64) {
        int drop = bl - 64;
        sticky |= (S & (((u128)1 << drop) - 1)) != 0;
        q = (uint64_t)(S >> drop);
        e2 += drop;
    } else {
        q = (uint64_t)S;
    }
    double r = __fp_round_double(q, e2, sticky);
    return rneg ? -r : r;
}

/* ---------------------------------------------------------------- float versions */

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float sinhf(float x) { return (float)sinh(x); }
float coshf(float x) { return (float)cosh(x); }
float tanhf(float x) { return (float)tanh(x); }
float expf(float x) { return (float)exp(x); }
float exp2f(float x) { return (float)exp2(x); }
float expm1f(float x) { return (float)expm1(x); }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }
float log1pf(float x) { return (float)log1p(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float cbrtf(float x) { return (float)cbrt(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float fmaf(float a, float b, float c) { return (float)fma(a, b, c); }
