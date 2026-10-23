/* Exact decimal <-> binary floating point conversion.
 *
 *   strtod, strtof, strtold, atof   correctly rounded (round to nearest, ties to even) for every input: any number of
 *                                   digits, any exponent, hex floats, inf/nan, subnormals, overflow and underflow.
 *   __fp_digits                     the exact decimal expansion of a double, for printf's %f %e %g.
 *
 * How strtod stays exact: the decimal string is the integer D times 10 ^ e10. Three ways to find the nearest binary value:
 *   1. D < 2^53 and |e10| <= 22 (about 99% of real inputs): D and 10^|e10| are exact doubles, so one multiplication or
 *      division is correctly rounded by the hardware (Clinger's fast path).
 *   2. At most 19 digits and |e10| <= 27: 5^|e10| fits in 64 bits, so D * 5^e10 or (D << s) / 5^-e10 is done exactly in
 *      128-bit arithmetic and rounded from the top 64 bits plus a "sticky" bit that says whether anything non-zero lies
 *      below them.
 *   3. Anything else: the same computation with big integers (32-bit limbs). Digits past the 800th only matter as a
 *      sticky digit, because no double or midpoint between doubles has more than 768 significant digits.
 * The quotient is always scaled to 63..64 bits, so one final rounding step (pack) is exact: no double rounding.
 * Powers of ten are 5^k * 2^k; the 2^k only shifts the binary exponent, which keeps the big integers small. */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include "fpconv.h"

typedef unsigned __int128 u128;

/* ---------------------------------------------------------------- big integers */

#define BN_LIMBS 132            /* 4224 bits: the largest values are about 2700 bits, see the bounds in fp_parse */
#define MAXDIG 800              /* decimal digits kept from the input; the rest only decides the sticky digit */

typedef struct {
    uint32_t w[BN_LIMBS];       /* little endian; w[n-1] != 0 */
    int n;
} Big;

static void big_set64(Big *b, uint64_t v) {
    b->n = 0;
    while (v) { b->w[b->n++] = (uint32_t)v; v >>= 32; }
}

static void big_mul32(Big *b, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) b->w[b->n++] = (uint32_t)carry;
}

static void big_add32(Big *b, uint32_t a) {
    uint64_t carry = a;
    for (int i = 0; i < b->n && carry; i++) {
        uint64_t t = (uint64_t)b->w[i] + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) b->w[b->n++] = (uint32_t)carry;
}

static int big_bitlen(const Big *b) {
    return b->n ? 32 * (b->n - 1) + (32 - __builtin_clz(b->w[b->n - 1])) : 0;
}

static void big_shl(Big *b, int s) {
    if (!b->n || s <= 0) return;
    int ls = s >> 5, bs = s & 31, n = b->n;
    if (bs) {
        uint32_t carry = 0;
        for (int i = 0; i < n; i++) {
            uint32_t v = b->w[i];
            b->w[i] = (v << bs) | carry;
            carry = v >> (32 - bs);
        }
        if (carry) b->w[n++] = carry;
    }
    if (ls) {
        for (int i = n - 1; i >= 0; i--) b->w[i + ls] = b->w[i];
        for (int i = 0; i < ls; i++) b->w[i] = 0;
        n += ls;
    }
    b->n = n;
}

static void big_shr1(Big *b) {
    for (int i = 0; i < b->n; i++)
        b->w[i] = (b->w[i] >> 1) | (i + 1 < b->n ? b->w[i + 1] << 31 : 0);
    if (b->n && b->w[b->n - 1] == 0) b->n--;
}

static int big_cmp(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static void big_sub(Big *a, const Big *b) {            /* a -= b, a >= b */
    uint32_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t sub = (uint64_t)(i < b->n ? b->w[i] : 0) + borrow;
        borrow = (uint64_t)a->w[i] < sub;
        a->w[i] = (uint32_t)((uint64_t)a->w[i] - sub);
    }
    while (a->n && a->w[a->n - 1] == 0) a->n--;
}

static void big_pow5(Big *b, int k) {                  /* b = 5^k */
    static const uint32_t p5[14] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625, 1953125, 9765625, 48828125,
                                     244140625, 1220703125 };
    big_set64(b, 1);
    while (k >= 13) { big_mul32(b, p5[13]); k -= 13; }
    if (k) big_mul32(b, p5[k]);
}

/* floor(num / den) when the quotient is known to be below 2^64; num becomes the remainder. */
static uint64_t big_divq(Big *num, const Big *den) {
    Big t = *den;
    big_shl(&t, 63);
    uint64_t q = 0;
    for (int j = 63; j >= 0; j--) {
        q <<= 1;
        if (big_cmp(num, &t) >= 0) { big_sub(num, &t); q |= 1; }
        big_shr1(&t);
    }
    return q;
}

static uint32_t big_divmod32(Big *b, uint32_t dv) {    /* b /= dv; returns the remainder */
    uint64_t rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | b->w[i];
        b->w[i] = (uint32_t)(cur / dv);
        rem = cur % dv;
    }
    while (b->n && b->w[b->n - 1] == 0) b->n--;
    return (uint32_t)rem;
}

/* The 64 bits of b starting at bit s. */
static uint64_t big_window64(const Big *b, int s) {
    int li = s >> 5, off = s & 31;
    uint64_t lo = li < b->n ? b->w[li] : 0, mid = li + 1 < b->n ? b->w[li + 1] : 0, hi = li + 2 < b->n ? b->w[li + 2] : 0;
    uint64_t r = lo | (mid << 32);
    if (off) r = (r >> off) | (hi << (64 - off));
    return r;
}

/* Is any bit below position s set? */
static int big_low_bits(const Big *b, int s) {
    int li = s >> 5, off = s & 31;
    for (int i = 0; i < li && i < b->n; i++) if (b->w[i]) return 1;
    return li < b->n && off && (b->w[li] & ((1u << off) - 1));
}

/* ---------------------------------------------------------------- rounding */

typedef struct {
    int mbits;                  /* significand bits including the hidden one: 53 / 24 */
    int emin, emax;             /* exponent range of normal numbers: -1022..1023 / -126..127 */
    int bias, fracbits;
    int maxdec, mindec;         /* decimal exponent bounds: a value >= 10^maxdec overflows, a value < 10^mindec rounds to 0 */
    uint64_t signmask, infbits, nanbits;
} FpFmt;

static const FpFmt FMT_D = { 53, -1022, 1023, 1023, 52, 309, -324, 0x8000000000000000ULL, 0x7FF0000000000000ULL, 0x7FF8000000000000ULL };
static const FpFmt FMT_F = { 24, -126, 127, 127, 23, 39, -46, 0x80000000ULL, 0x7F800000ULL, 0x7FC00000ULL };

/* The bit pattern (without sign) of the nearest value to (q + frac) * 2^e2, where frac is in (0,1) when `sticky` is set and
 * 0 otherwise; q != 0. When sticky is set q must have at least mbits + 2 significant bits (callers scale it to 63..64).
 * *range is set for overflow, for a result of zero and for an inexact subnormal. */
static uint64_t pack(uint64_t q, long long e2, int sticky, const FpFmt *f, int *range) {
    int L = 64 - __builtin_clzll(q);
    long long E = (long long)L - 1 + e2;                    /* exponent of the leading bit */
    if (E > f->emax) { *range = 1; return f->infbits; }
    long long keep = f->mbits;                              /* significand bits that survive */
    int sub = 0;
    if (E < f->emin) {
        sub = 1;
        keep = E - f->emin + f->mbits;
        if (keep < 0) { *range = 1; return 0; }             /* below half of the smallest subnormal */
    }
    int drop = L - (int)keep;                               /* low bits of q that are discarded */
    uint64_t m;
    int rb, rest;                                           /* round bit and "anything below it" */
    if (drop > 0) {
        m = drop >= 64 ? 0 : q >> drop;
        rb = (int)((q >> (drop - 1)) & 1);
        rest = (q & ((1ULL << (drop - 1)) - 1)) != 0 || sticky;
    } else {
        m = q << -drop;
        rb = 0;
        rest = 0;
    }
    int inexact = rb || rest;
    if (rb && (rest || (m & 1))) m++;                       /* ties to even */
    if (!sub) {
        uint64_t bits = ((uint64_t)(E + f->bias - 1) << f->fracbits) + m;     /* m has the hidden bit, which adds the 1 */
        if (bits >= f->infbits) { *range = 1; return f->infbits; }
        return bits;
    }
    /* ERANGE for an inexact subnormal (or zero). A value one binade below the smallest normal number that rounds up to it
     * at full precision is not tiny (tininess is judged after rounding to normal precision), as in glibc. */
    int tiny = 1;
    if (E == f->emin - 1 && L > f->mbits) {
        int d = L - f->mbits;
        if ((q >> d) == (1ULL << f->mbits) - 1 && ((q >> (d - 1)) & 1)) tiny = 0;
    }
    if (tiny && (inexact || m == 0)) *range = 1;
    return m;                                               /* subnormal; m == 2^fracbits is the smallest normal number */
}

static uint64_t pack_u128(u128 n, long long e2, const FpFmt *f, int *range) {         /* n * 2^e2, n != 0 */
    uint64_t hi = (uint64_t)(n >> 64);
    if (!hi) return pack((uint64_t)n, e2, 0, f, range);
    int s = 64 - __builtin_clzll(hi);                       /* n has 64 + s bits */
    int sticky = (n & (((u128)1 << s) - 1)) != 0;
    return pack((uint64_t)(n >> s), e2 + s, sticky, f, range);
}

/* hi:lo / d for a quotient that fits in 64 bits (hi < d). */
static inline uint64_t div128by64(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem) {
    uint64_t q, r;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "0"(lo), "1"(hi), "r"(d) : "cc");
    *rem = r;
    return q;
}

static uint64_t pow5_64(int k) {                        /* k <= 27 */
    uint64_t r = 1;
    while (k-- > 0) r *= 5;
    return r;
}

/* ---------------------------------------------------------------- strtod */

static const double P10D[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16,
                                 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
static const float P10F[11] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f };

#define ISDIG(c) ((unsigned)(c) - '0' < 10u)
static int is_space(unsigned c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int match_ci(const unsigned char *p, const char *lit) {      /* lit is lower case */
    for (; *lit; p++, lit++) if ((*p | 32) != (unsigned char)*lit) return 0;
    return 1;
}

static int hexval(unsigned c) {
    if (c - '0' < 10u) return (int)(c - '0');
    c |= 32;
    return c - 'a' < 6u ? (int)(c - 'a' + 10) : -1;
}

#ifdef FP_TESTHOOK
int __fp_skip_paths;            /* test builds: 1 = skip the Clinger path, 2 = skip it and the 128-bit path too */
#define SKIP(n) (__fp_skip_paths >= (n))
#else
#define SKIP(n) 0
#endif

static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }
static uint64_t fbits(float x) { uint32_t u; memcpy(&u, &x, sizeof u); return u; }

/* Parses the longest valid prefix of str. Returns the bit pattern of the result in format f (sign included); *endp is the
 * end of the number (str when there is none). */
static uint64_t fp_parse(const char *str, char **endp, const FpFmt *f, int *range) {
    const unsigned char *p = (const unsigned char *)str;
    *range = 0;
    while (is_space(*p)) p++;
    uint64_t sign = 0;
    if (*p == '-') { sign = f->signmask; p++; } else if (*p == '+') p++;

    unsigned c0 = *p | 32;
    if (c0 == 'i' && match_ci(p, "inf")) {
        p += 3;
        if (match_ci(p, "inity")) p += 5;
        if (endp) *endp = (char *)p;
        return sign | f->infbits;
    }
    if (c0 == 'n' && match_ci(p, "nan")) {
        p += 3;
        if (*p == '(') {
            const unsigned char *q = p + 1;
            while ((ISDIG(*q)) || ((unsigned)(*q | 32) - 'a' < 26u) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        if (endp) *endp = (char *)p;
        return sign | f->nanbits;
    }

    if (p[0] == '0' && (p[1] | 32) == 'x') {                /* hexadecimal floating constant */
        const unsigned char *q = p + 2;
        uint64_t m = 0;
        int any = 0, seen_dot = 0, sticky = 0;
        long long bexp = 0;
        for (;; q++) {
            int d = hexval(*q);
            if (d >= 0) {
                any = 1;
                if (m >> 60 == 0) { m = (m << 4) | (unsigned)d; if (seen_dot) bexp -= 4; }
                else { if (d) sticky = 1; if (!seen_dot) bexp += 4; }
            } else if (*q == '.' && !seen_dot) seen_dot = 1;
            else break;
        }
        if (any) {
            if ((*q | 32) == 'p') {
                const unsigned char *r = q + 1;
                int eneg = 0;
                if (*r == '+' || *r == '-') { eneg = *r == '-'; r++; }
                if (ISDIG(*r)) {
                    long long e = 0;
                    for (; ISDIG(*r); r++) if (e < 1000000000000LL) e = e * 10 + (*r - '0');
                    bexp += eneg ? -e : e;
                    q = r;
                }
            }
            if (endp) *endp = (char *)q;
            if (m == 0) return sign;
            return sign | pack(m, bexp, sticky, f, range);
        }
        /* "0x" without digits is just the number 0, followed by junk */
    }

    /* decimal: first pass over the mantissa, collecting up to 19 significant digits */
    const unsigned char *ms = p, *q = p;
    uint64_t m = 0;
    int nm = 0, many = 0, anyd = 0, seen_dot = 0;
    long long scale = 0;                                    /* decimal exponent of m, before the "eN" part */
    for (;; q++) {
        unsigned c = (unsigned)*q - '0';
        if (c < 10) {
            anyd = 1;
            if (nm == 0 && c == 0) { if (seen_dot) scale--; continue; }
            if (nm < 19) { m = m * 10 + c; nm++; if (seen_dot) scale--; }
            else { many = 1; if (!seen_dot) scale++; }
        } else if (*q == '.' && !seen_dot) seen_dot = 1;
        else break;
    }
    if (!anyd) { if (endp) *endp = (char *)str; return 0; }
    const unsigned char *me = q;
    long long uexp = 0;
    if ((*q | 32) == 'e') {
        const unsigned char *r = q + 1;
        int eneg = 0;
        if (*r == '+' || *r == '-') { eneg = *r == '-'; r++; }
        if (ISDIG(*r)) {
            for (; ISDIG(*r); r++) if (uexp < 1000000000000LL) uexp = uexp * 10 + (*r - '0');
            if (eneg) uexp = -uexp;
            q = r;
        }
    }
    if (endp) *endp = (char *)q;
    if (nm == 0) return sign;                               /* every digit was zero */

    Big D;
    int nd;
    long long e10;
    if (!many) {
        e10 = uexp + scale;
        if (f->mbits == 53 && m < (1ULL << 53) && !SKIP(1)) {                   /* Clinger */
            if (e10 == 0) return sign | dbits((double)m);
            if (e10 > 0 && e10 <= 22) return sign | dbits((double)m * P10D[e10]);
            if (e10 < 0 && e10 >= -22) return sign | dbits((double)m / P10D[-e10]);
            if (e10 > 22 && e10 <= 22 + 15) {
                uint64_t mm = m;
                long long extra = e10 - 22;
                while (extra > 0 && mm <= (1ULL << 53) / 10) { mm *= 10; extra--; }
                if (!extra) return sign | dbits((double)mm * 1e22);
            }
        } else if (f->mbits == 24 && m < (1ULL << 24) && !SKIP(1)) {
            if (e10 == 0) return sign | fbits((float)m);
            if (e10 > 0 && e10 <= 10) return sign | fbits((float)m * P10F[e10]);
            if (e10 < 0 && e10 >= -10) return sign | fbits((float)m / P10F[-e10]);
        }
        if (e10 >= -27 && e10 <= 27 && !SKIP(2)) {                              /* exact in 128 bits */
            if (e10 >= 0) return sign | pack_u128((u128)m * pow5_64((int)e10), e10, f, range);
            uint64_t den = pow5_64((int)-e10);
            int a = 64 - __builtin_clzll(m), b = 64 - __builtin_clzll(den);
            int s = 63 - a + b;                             /* scales the quotient into 2^62 .. 2^64 */
            u128 num = (u128)m << s;
            uint64_t rem;
            uint64_t qq = div128by64((uint64_t)(num >> 64), (uint64_t)num, den, &rem);
            return sign | pack(qq, -(long long)s + e10, rem != 0, f, range);
        }
        big_set64(&D, m);
        nd = nm;
    } else {                                                /* more than 19 digits: read them all (up to MAXDIG) */
        long long frac_all = 0, dropped = 0;
        int sticky = 0, started = 0, kept = 0, chunkn = 0, dot = 0;
        uint32_t chunk = 0;
        D.n = 0;
        for (const unsigned char *r = ms; r < me; r++) {
            if (*r == '.') { dot = 1; continue; }
            unsigned c = (unsigned)*r - '0';
            if (dot) frac_all++;
            if (!started) { if (c == 0) continue; started = 1; }
            if (kept < MAXDIG) {
                chunk = chunk * 10 + c;
                kept++;
                if (++chunkn == 9) { big_mul32(&D, 1000000000u); big_add32(&D, chunk); chunk = 0; chunkn = 0; }
            } else {
                dropped++;
                if (c) sticky = 1;
            }
        }
        if (chunkn) {
            uint32_t pw = 1;
            for (int i = 0; i < chunkn; i++) pw *= 10;
            big_mul32(&D, pw);
            big_add32(&D, chunk);
        }
        nd = kept;
        e10 = uexp - frac_all + dropped;
        if (sticky) { big_mul32(&D, 10); big_add32(&D, 1); nd++; e10--; }      /* a digit that is neither zero nor round */
    }

    /* The value is D * 10^e10 with D having nd digits: it lies in [10^(e10+nd-1), 10^(e10+nd)). */
    if (e10 + nd - 1 >= f->maxdec) { *range = 1; return sign | f->infbits; }
    if (e10 + nd <= f->mindec) { *range = 1; return sign; }
    if (e10 >= 0) {                                         /* D * 5^e10 * 2^e10: an exact integer times 2^e10 */
        int e = (int)e10;
        for (; e >= 13; e -= 13) big_mul32(&D, 1220703125u);
        if (e) { uint32_t pw = 1; for (int i = 0; i < e; i++) pw *= 5; big_mul32(&D, pw); }
        int L = big_bitlen(&D);
        int s = L > 64 ? L - 64 : 0;
        uint64_t qq = big_window64(&D, s);
        return sign | pack(qq, e10 + s, s > 0 && big_low_bits(&D, s), f, range);
    }
    int k = (int)-e10;                                      /* D / 5^k * 2^-k */
    Big den;
    big_pow5(&den, k);
    int a = big_bitlen(&D), b = big_bitlen(&den);
    int s = 63 - a + b;
    if (s >= 0) big_shl(&D, s); else big_shl(&den, -s);
    uint64_t qq = big_divq(&D, &den);
    return sign | pack(qq, -(long long)s - k, D.n != 0, f, range);
}

double strtod(const char *s, char **end) {
    int range;
    uint64_t bits = fp_parse(s, end, &FMT_D, &range);
    if (range) errno = ERANGE;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

float strtof(const char *s, char **end) {
    int range;
    uint32_t bits = (uint32_t)fp_parse(s, end, &FMT_F, &range);
    if (range) errno = ERANGE;
    float x;
    memcpy(&x, &bits, sizeof x);
    return x;
}

long double strtold(const char *s, char **end) { return strtod(s, end); }     /* long double is parsed with double precision */
double atof(const char *s) { return strtod(s, NULL); }
long double atold(const char *s) { return strtod(s, NULL); }

/* ---------------------------------------------------------------- exact decimal digits of a double */

static int put_u64(unsigned char *d, uint64_t v) {          /* decimal digit values of v > 0 */
    unsigned char tmp[20];
    int n = 0;
    while (v) { tmp[n++] = (unsigned char)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) d[i] = tmp[n - 1 - i];
    return n;
}

static void put_chunk9(unsigned char *d, uint32_t c) {
    for (int i = 8; i >= 0; i--) { d[i] = (unsigned char)(c % 10); c /= 10; }
}

void __fp_digits(double v, int sig, int frac, FpDigits *o) {
    uint64_t bits = dbits(v);
    int be = (int)((bits >> 52) & 0x7FF);
    uint64_t m = bits & 0xFFFFFFFFFFFFFULL;
    int e;
    if (be == 0) e = -1074; else { m |= 1ULL << 52; e = be - 1075; }
    int tz = __builtin_ctzll(m);
    m >>= tz;                                               /* now v = m * 2^e with m odd */
    e += tz;

    unsigned char *d = o->buf + 1;
    o->d = d;
    o->more = 0;
    int nd = 0;
    uint64_t fnum = 0;
    int k = 0;
    if (e >= 0) {
        if (64 - __builtin_clzll(m) + e <= 64) {
            nd = put_u64(d, m << e);
        } else {                                            /* an integer of up to 1024 bits: peel off 9 digits at a time */
            Big N;
            uint32_t ch[40];
            int nc = 0;
            big_set64(&N, m);
            big_shl(&N, e);
            while (N.n) ch[nc++] = big_divmod32(&N, 1000000000u);
            nd = put_u64(d, ch[nc - 1]);
            for (int i = nc - 2; i >= 0; i--) { put_chunk9(d + nd, ch[i]); nd += 9; }
        }
    } else {
        k = -e;
        uint64_t ip;
        if (k < 64) { ip = m >> k; fnum = m & ((1ULL << k) - 1); } else { ip = 0; fnum = m; }
        if (ip) nd = put_u64(d, ip);
    }
    int nint = nd;
    o->dp = nint;

    if (fnum) {
        if (sig > 0 && nint >= sig + 1) {                   /* the integer part alone has all the digits that are wanted */
            o->more = 1;
        } else {
            /* fnum / 2^k as L 32-bit limbs F, value F / 2^(32 L); each multiplication by 10^9 pushes the next 9 digits out
             * of the top limb. */
            uint32_t F[36];
            int L = (k + 31) >> 5;
            unsigned sh = (unsigned)(32 * L - k);
            u128 t = (u128)fnum << sh;
            for (int i = 0; i < L; i++) F[i] = i < 4 ? (uint32_t)(t >> (32 * i)) : 0;
            int lo = 0, hi = L < 4 ? L : 4;
            while (lo < hi && F[lo] == 0) lo++;
            while (hi > lo && F[hi - 1] == 0) hi--;
            int first_nz = nint > 0 ? 0 : -1;
            int nfrac = 0;
            for (;;) {
                if (nd + 9 > FP_DIGITS_MAX) break;
                uint64_t carry = 0;
                for (int i = lo; i < hi; i++) {
                    uint64_t tt = (uint64_t)F[i] * 1000000000u + carry;
                    F[i] = (uint32_t)tt;
                    carry = tt >> 32;
                }
                uint32_t chunk;
                if (hi < L) { if (carry) F[hi++] = (uint32_t)carry; chunk = 0; }
                else chunk = (uint32_t)carry;
                while (lo < hi && F[lo] == 0) lo++;
                put_chunk9(d + nd, chunk);
                if (first_nz < 0) {
                    for (int i = 0; i < 9; i++) if (d[nd + i]) { first_nz = nd + i; break; }
                }
                nd += 9;
                nfrac += 9;
                if (lo >= hi) break;                        /* the fraction has ended exactly */
                if (sig > 0 ? (first_nz >= 0 && nd - first_nz >= sig + 1) : nfrac >= frac + 1) break;
            }
            o->more = lo < hi;
        }
    }
    if (!o->more) {
        while (nd > nint && d[nd - 1] == 0) nd--;
    }
    o->nd = nd;
}
