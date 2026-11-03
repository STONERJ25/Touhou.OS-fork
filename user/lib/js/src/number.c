/*
 * number.c: conversions between Numbers and strings, all exact.
 *
 *   - Number::toString (ECMA-262 6.1.6.1.20), with the recommended refinement of its note 2: the shortest digit
 *     string that reads back as the same double, and of those the closest, ties to even.  Digits come from the
 *     free-format algorithm of Steele and White ("How to Print Floating-Point Numbers Accurately", 1990) in the
 *     formulation of Burger and Dybvig (1996), run on exact big integers (the bounds r, s, m+, m-).
 *   - toFixed / toExponential / toPrecision: the exact binary value rounded half up at the requested digit, as the
 *     specification's "pick the larger n" says, again on big integers.
 *   - string to double: correctly rounded (round half to even) for any length and exponent.  Short inputs take
 *     Clinger's exact fast path (at most 15 digits and a power of ten up to 1e22, both exact doubles, one
 *     rounding); everything else is computed as an exact quotient of big integers and rounded once.
 * The big integers are fixed-size arrays of 32-bit limbs on the C stack (BN_LIMBS limbs cover 10^1200 times 2^1100,
 * more than any double needs once inputs are cut at 800 significant digits with a sticky digit after them).
 */
#include "jsint.h"

#define BN_LIMBS 170
typedef struct { int n; uint32_t d[BN_LIMBS]; } Bn;

static void bn_set(Bn *a, uint64_t v) {
    a->n = 0;
    while (v) { a->d[a->n++] = (uint32_t)v; v >>= 32; }
}

static void bn_mul_small(Bn *a, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < BN_LIMBS) a->d[a->n++] = (uint32_t)carry;
}

static void bn_add_small(Bn *a, uint32_t v) {
    uint64_t carry = v;
    for (int i = 0; i < a->n && carry; i++) {
        uint64_t t = (uint64_t)a->d[i] + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < BN_LIMBS) a->d[a->n++] = (uint32_t)carry;
}

static void bn_mul_pow10(Bn *a, int k) {
    while (k >= 9) { bn_mul_small(a, 1000000000u); k -= 9; }
    static const uint32_t p10[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    if (k > 0) bn_mul_small(a, p10[k]);
}

static void bn_shl(Bn *a, int bits) {
    if (!a->n || bits <= 0) return;
    int limbs = bits / 32, b = bits % 32;
    if (b) {
        uint32_t carry = 0;
        for (int i = 0; i < a->n; i++) {
            uint32_t v = a->d[i];
            a->d[i] = (v << b) | carry;
            carry = v >> (32 - b);
        }
        if (carry && a->n < BN_LIMBS) a->d[a->n++] = carry;
    }
    if (limbs) {
        int n = a->n + limbs;
        if (n > BN_LIMBS) n = BN_LIMBS;
        for (int i = n - 1; i >= limbs; i--) a->d[i] = a->d[i - limbs];
        for (int i = 0; i < limbs; i++) a->d[i] = 0;
        a->n = n;
    }
}

static void bn_shr1(Bn *a) {
    for (int i = 0; i < a->n; i++) a->d[i] = (a->d[i] >> 1) | (i + 1 < a->n ? a->d[i + 1] << 31 : 0);
    while (a->n && !a->d[a->n - 1]) a->n--;
}

static int bn_cmp(const Bn *a, const Bn *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static void bn_sub(Bn *a, const Bn *b) {            /* a -= b, a >= b */
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        borrow = t < 0;
        a->d[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
    while (a->n && !a->d[a->n - 1]) a->n--;
}

static void bn_add(Bn *a, const Bn *b) {            /* a += b */
    uint64_t carry = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)(i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0) + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    a->n = n;
    if (carry && a->n < BN_LIMBS) a->d[a->n++] = (uint32_t)carry;
}

static int bn_bits(const Bn *a) {
    if (!a->n) return 0;
    return (a->n - 1) * 32 + (32 - __builtin_clz(a->d[a->n - 1]));
}

/* a + b compared with c */
static int bn_cmp_sum(const Bn *a, const Bn *b, const Bn *c) {
    Bn t = *a;
    bn_add(&t, b);
    return bn_cmp(&t, c);
}

/* ------------------------------------------------------------------ doubles from bits */

typedef union { double d; uint64_t u; } DU;

/* (mantissa, exponent) with value = f * 2^e, f an integer; f == 0 for zero */
static void decompose(double x, uint64_t *f, int *e) {
    DU v = { x };
    int be = (int)((v.u >> 52) & 0x7FF);
    uint64_t m = v.u & ((1ULL << 52) - 1);
    if (be == 0) { *f = m; *e = -1074; }
    else { *f = m | (1ULL << 52); *e = be - 1075; }
}

/* m (bit 63 set) * 2^e2, plus a sticky fraction below it, rounded half to even into a double */
static double round_to_double(uint64_t m, int e2, bool sticky) {
    int E = e2 + 63;                        /* exponent of the leading bit */
    if (E > 1023) return __builtin_inf();
    int shift;
    if (E >= -1022) shift = 11;
    else {
        shift = 11 + (-1022 - E);
        if (shift > 64) return 0.0;
    }
    uint64_t mant, rem, half;
    if (shift == 64) { mant = 0; rem = m; half = 1ULL << 63; }
    else { mant = m >> shift; rem = m & ((1ULL << shift) - 1); half = 1ULL << (shift - 1); }
    if (rem > half || (rem == half && (sticky || (mant & 1)))) mant++;
    DU v;
    if (E >= -1022) {
        if (mant == (1ULL << 53)) { mant >>= 1; E++; if (E > 1023) return __builtin_inf(); }
        v.u = ((uint64_t)(E + 1023) << 52) | (mant & ((1ULL << 52) - 1));
    } else v.u = mant;                      /* subnormal (rounding up to 2^52 lands on the smallest normal) */
    return v.d;
}

/* An exact big integer as the nearest double. */
static double bn_to_double(const Bn *a) {
    int bits = bn_bits(a);
    if (!bits) return 0.0;
    uint64_t m = 0;
    bool sticky = false;
    if (bits <= 64) {
        for (int i = a->n - 1; i >= 0; i--) m = (m << 32) | a->d[i];
        m <<= (64 - bits);
    } else {
        int low = bits - 64;                /* bits below the top 64 */
        for (int i = 0; i < 64; i++) {
            int b = low + i;
            if ((a->d[b / 32] >> (b % 32)) & 1) m |= 1ULL << i;
        }
        for (int i = 0; i < low / 32 && !sticky; i++) if (a->d[i]) sticky = true;
        if (!sticky && (low % 32) && (a->d[low / 32] & ((1u << (low % 32)) - 1))) sticky = true;
    }
    return round_to_double(m, bits - 64, sticky);
}

/* ------------------------------------------------------------------ decimal to double */

static const double exact_pow10[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20,
    1e21, 1e22
};

#define MAX_DIGITS 800

/* digits: ASCII '0'..'9' without leading zeros, nd of them (nd <= MAX_DIGITS + 1); value = digits * 10^exp10 */
static double decimal_to_double(const char *digits, int nd, long exp10) {
    if (nd == 0) return 0.0;
    long dexp = nd + exp10;                 /* value in [10^(dexp-1), 10^dexp) */
    if (dexp > 310) return __builtin_inf();
    if (dexp < -330) return 0.0;
    if (nd <= 15 && exp10 >= -22 && exp10 <= 22) {
        uint64_t v = 0;
        for (int i = 0; i < nd; i++) v = v * 10 + (uint64_t)(digits[i] - '0');
        double d = (double)v;               /* exact: below 2^53 */
        return exp10 >= 0 ? d * exact_pow10[exp10] : d / exact_pow10[-exp10];
    }
    Bn n;
    n.n = 0;
    for (int i = 0; i < nd; i++) { bn_mul_small(&n, 10); bn_add_small(&n, (uint32_t)(digits[i] - '0')); }
    if (exp10 >= 0) {
        bn_mul_pow10(&n, (int)exp10);
        return bn_to_double(&n);
    }
    /* n / 10^-exp10: scale n by 2^k so that the quotient has exactly 64 bits */
    Bn s;
    bn_set(&s, 1);
    bn_mul_pow10(&s, (int)-exp10);
    int k = 63 + bn_bits(&s) - bn_bits(&n);
    Bn t = n;
    if (k > 0) bn_shl(&t, k);
    {
        Bn s63 = s;
        bn_shl(&s63, 63);
        if (bn_cmp(&t, &s63) < 0) { k++; bn_shl(&t, 1); }
    }
    if (k < 0) {                            /* cannot happen for in-range inputs; be safe */
        return 0.0;
    }
    /* long division for 64 quotient bits */
    Bn d = s;
    bn_shl(&d, 63);
    uint64_t q = 0;
    for (int i = 63; i >= 0; i--) {
        if (bn_cmp(&t, &d) >= 0) { bn_sub(&t, &d); q |= 1ULL << i; }
        bn_shr1(&d);
    }
    return round_to_double(q, -k, t.n != 0);
}

/* A decimal literal: digits [. digits] [e[+-]digits], already validated; underscores are skipped. */
double kz_strtod(const char *s, size_t len) {
    char digits[MAX_DIGITS + 2];
    int nd = 0;
    long exp10 = 0;
    bool seen_point = false, sticky = false;
    size_t i = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (c == '_') continue;
        if (c == '.') { seen_point = true; continue; }
        if (c < '0' || c > '9') break;
        if (nd == 0 && c == '0') { if (seen_point) exp10--; continue; }
        if (nd < MAX_DIGITS) {
            digits[nd++] = c;
            if (seen_point) exp10--;
        } else {
            if (c != '0') sticky = true;
            if (!seen_point) exp10++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool neg = false;
        if (i < len && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
        long e = 0;
        for (; i < len; i++) {
            if (s[i] == '_') continue;
            if (s[i] < '0' || s[i] > '9') break;
            if (e < 100000) e = e * 10 + (s[i] - '0');
        }
        exp10 += neg ? -e : e;
    }
    if (sticky) {                           /* a nonzero tail only breaks ties: one more digit stands for it */
        digits[nd++] = '1';
        exp10--;
    }
    /* trailing zeros of the digit string become exponent */
    while (nd > 0 && digits[nd - 1] == '0') { nd--; exp10++; }
    return decimal_to_double(digits, nd, exp10);
}

/* An integer in any radix 2..36 (digits already validated), rounded once. */
double kz_parse_radix_int(const char *s, size_t len, int radix) {
    Bn n;
    n.n = 0;
    bool overflow = false;
    size_t i = 0;
    while (i < len && s[i] == '0') i++;
    for (; i < len; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else d = c - 'A' + 10;
        if (n.n >= BN_LIMBS - 2) {
            /* far beyond 2^1024: the result is infinite */
            overflow = true;
            break;
        }
        bn_mul_small(&n, (uint32_t)radix);
        bn_add_small(&n, (uint32_t)d);
    }
    if (overflow || bn_bits(&n) > 1100) return __builtin_inf();
    return bn_to_double(&n);
}

/* ------------------------------------------------------------------ string to number (StringToNumber) */

static bool is_str_space(uint32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
           c == 0xFEFF;
}

static bool is_digit_at(JSString *s, uint32_t i) { uint32_t c = kz_str_at(s, i); return c >= '0' && c <= '9'; }

/* Scans a StrUnsignedDecimalLiteral at s[i]; returns the end index (i when there is none). */
static uint32_t scan_decimal(JSString *s, uint32_t i, uint32_t n) {
    uint32_t start = i;
    bool digits = false;
    while (i < n && is_digit_at(s, i)) { i++; digits = true; }
    if (i < n && kz_str_at(s, i) == '.') {
        uint32_t j = i + 1;
        bool frac = false;
        while (j < n && is_digit_at(s, j)) { j++; frac = true; }
        if (digits || frac) { i = j; digits = true; }
    }
    if (!digits) return start;
    if (i < n && (kz_str_at(s, i) | 0x20) == 'e') {
        uint32_t j = i + 1;
        if (j < n && (kz_str_at(s, j) == '+' || kz_str_at(s, j) == '-')) j++;
        bool ed = false;
        while (j < n && is_digit_at(s, j)) { j++; ed = true; }
        if (ed) i = j;
    }
    return i;
}

/* The value of a scanned decimal literal: at most MAX_DIGITS significant digits are kept (a nonzero tail becomes a
   sticky digit), the rest moves into the exponent, and kz_strtod rounds once. */
static double decimal_value(JSString *s, uint32_t from, uint32_t to) {
    char buf[MAX_DIGITS + 32];
    size_t o = 0;
    int sig = 0;
    long shift = 0;
    bool point = false, sticky = false;
    uint32_t i = from;
    for (; i < to; i++) {
        uint32_t c = kz_str_at(s, i);
        if (c == '.') { point = true; continue; }
        if ((c | 0x20) == 'e') break;
        if (sig == 0 && c == '0') { if (point) shift--; continue; }
        if (sig < MAX_DIGITS) { buf[o++] = (char)c; sig++; if (point) shift--; }
        else { if (c != '0') sticky = true; if (!point) shift++; }
    }
    if (sticky) { buf[o++] = '1'; shift--; }
    long e = 0;
    bool neg = false;
    if (i < to) {
        i++;
        if (i < to && (kz_str_at(s, i) == '+' || kz_str_at(s, i) == '-')) neg = kz_str_at(s, i++) == '-';
        for (; i < to; i++) if (e < 1000000) e = e * 10 + (kz_str_at(s, i) - '0');
    }
    e = (neg ? -e : e) + shift;
    if (o == 0) return 0.0;
    buf[o++] = 'e';
    if (e < 0) { buf[o++] = '-'; e = -e; }
    char tmp[16];
    int t = 0;
    do { tmp[t++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (t) buf[o++] = tmp[--t];
    return kz_strtod(buf, o);
}

static double scan_number_prefix(JSString *s, uint32_t i, uint32_t n, uint32_t *end) {
    double sign = 1.0;
    uint32_t start = i;
    if (i < n && (kz_str_at(s, i) == '+' || kz_str_at(s, i) == '-')) { if (kz_str_at(s, i) == '-') sign = -1.0; i++; }
    static const char inf[] = "Infinity";
    if (i + 8 <= n) {
        bool ok = true;
        for (int k = 0; k < 8; k++) if (kz_str_at(s, i + (uint32_t)k) != (uint8_t)inf[k]) { ok = false; break; }
        if (ok) { *end = i + 8; return sign * __builtin_inf(); }
    }
    uint32_t e = scan_decimal(s, i, n);
    if (e == i) { *end = start; return __builtin_nan(""); }
    *end = e;
    return sign * decimal_value(s, i, e);
}

double kz_string_to_number(JSString *s) {
    uint32_t n = kz_str_len(s), i = 0;
    while (i < n && is_str_space(kz_str_at(s, i))) i++;
    while (n > i && is_str_space(kz_str_at(s, n - 1))) n--;
    if (i == n) return 0.0;
    if (n - i > 2 && kz_str_at(s, i) == '0') {
        uint32_t c = kz_str_at(s, i + 1) | 0x20;
        int radix = c == 'x' ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 0;
        if (radix) {
            char buf[128];
            uint32_t j = i + 2;
            bool big = false;
            size_t o = 0;
            for (; j < n; j++) {
                uint32_t ch = kz_str_at(s, j), d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') d = (ch | 0x20) - 'a' + 10;
                else return __builtin_nan("");
                if (d >= (uint32_t)radix) return __builtin_nan("");
                if (o < sizeof buf) buf[o++] = (char)ch;
                else big = true;
            }
            if (!big) return kz_parse_radix_int(buf, o, radix);
            /* very long: accumulate directly */
            Bn v;
            v.n = 0;
            for (j = i + 2; j < n; j++) {
                uint32_t ch = kz_str_at(s, j) | 0x20;
                uint32_t d = ch <= '9' ? ch - '0' : ch - 'a' + 10;
                if (v.n >= BN_LIMBS - 2) return __builtin_inf();
                bn_mul_small(&v, (uint32_t)radix);
                bn_add_small(&v, d);
            }
            return bn_bits(&v) > 1100 ? __builtin_inf() : bn_to_double(&v);
        }
    }
    uint32_t end;
    double d = scan_number_prefix(s, i, n, &end);
    if (end != n) return __builtin_nan("");
    return d;
}

double kz_parse_float(JSString *s, uint32_t i, uint32_t *end) {
    uint32_t n = kz_str_len(s);
    while (i < n && is_str_space(kz_str_at(s, i))) i++;
    double d = scan_number_prefix(s, i, n, end);
    return d;
}

/* ------------------------------------------------------------------ double to digits */

/* Shortest digits (free format).  x > 0, finite.  Writes digits (no NUL) and returns their count; *point gets n with
   x = 0.d1d2... * 10^n. */
static int shortest_digits(double x, char *out, int *point) {
    uint64_t f;
    int e;
    decompose(x, &f, &e);
    bool even = (f & 1) == 0;
    Bn r, s, mp, mm;
    bool unequal = false;
    if (e >= 0) {
        if (f != (1ULL << 52)) {
            bn_set(&r, f); bn_shl(&r, e + 1);
            bn_set(&s, 2);
            bn_set(&mp, 1); bn_shl(&mp, e);
            mm = mp;
        } else {
            bn_set(&r, f); bn_shl(&r, e + 2);
            bn_set(&s, 4);
            bn_set(&mp, 1); bn_shl(&mp, e + 1);
            bn_set(&mm, 1); bn_shl(&mm, e);
            unequal = true;
        }
    } else {
        if (e == -1074 || f != (1ULL << 52)) {
            bn_set(&r, f); bn_shl(&r, 1);
            bn_set(&s, 1); bn_shl(&s, 1 - e);
            bn_set(&mp, 1);
            bn_set(&mm, 1);
        } else {
            bn_set(&r, f); bn_shl(&r, 2);
            bn_set(&s, 1); bn_shl(&s, 2 - e);
            bn_set(&mp, 2);
            bn_set(&mm, 1);
            unequal = true;
        }
    }
    (void)unequal;
    /* estimate k = ceil(log10(x)) from the binary exponent, then correct it */
    int bits = 64 - __builtin_clzll(f);
    int k = (int)__builtin_ceil((e + bits - 1) * 0.30102999566398114 - 1e-10);
    if (k >= 0) bn_mul_pow10(&s, k);
    else { bn_mul_pow10(&r, -k); bn_mul_pow10(&mp, -k); bn_mul_pow10(&mm, -k); }
    /* fixup: we need r + m+ < s (or <= when even) */
    for (;;) {
        int c = bn_cmp_sum(&r, &mp, &s);
        if (c > 0 || (even && c == 0)) { bn_mul_small(&s, 10); k++; continue; }
        break;
    }
    /* the estimate may also be one too high */
    for (;;) {
        Bn r10 = r, mp10 = mp;
        bn_mul_small(&r10, 10);
        bn_mul_small(&mp10, 10);
        int c = bn_cmp_sum(&r10, &mp10, &s);
        if (c < 0 || (!even && c == 0)) {
            r = r10; mp = mp10; bn_mul_small(&mm, 10); k--;
            continue;
        }
        break;
    }
    int nd = 0;
    for (;;) {
        bn_mul_small(&r, 10);
        bn_mul_small(&mp, 10);
        bn_mul_small(&mm, 10);
        int d = 0;
        while (bn_cmp(&r, &s) >= 0) { bn_sub(&r, &s); d++; }
        int c1 = bn_cmp(&r, &mm);
        bool tc1 = even ? c1 <= 0 : c1 < 0;
        int c2 = bn_cmp_sum(&r, &mp, &s);
        bool tc2 = even ? c2 >= 0 : c2 > 0;
        if (!tc1 && !tc2) { out[nd++] = (char)('0' + d); if (nd > 40) break; continue; }
        if (tc1 && !tc2) out[nd++] = (char)('0' + d);
        else if (!tc1 && tc2) out[nd++] = (char)('0' + d + 1);
        else {
            Bn r2 = r;
            bn_shl(&r2, 1);
            int c = bn_cmp(&r2, &s);
            out[nd++] = (char)('0' + (c < 0 ? d : c > 0 ? d + 1 : (d & 1) ? d + 1 : d));
        }
        break;
    }
    *point = k;
    return nd;
}

/* The exact value x > 0 rounded half up to `prec` significant digits (mode 0) or to `prec` digits after the
   decimal point (mode 1).  Returns the digit count (all digits, leading zeros of a fixed result excluded) and sets
   *point (x ~ 0.d1d2... * 10^point).  out needs prec + 400 bytes in mode 1. */
static int exact_digits(double x, int mode, int prec, char *out, int *point) {
    uint64_t f;
    int e;
    decompose(x, &f, &e);
    /* x = f * 2^e = num / den */
    Bn num, den;
    bn_set(&num, f);
    bn_set(&den, 1);
    if (e >= 0) bn_shl(&num, e); else bn_shl(&den, -e);
    /* k = floor(log10 x) + 1, so that 10^(k-1) <= x < 10^k */
    int bits = 64 - __builtin_clzll(f);
    int k = (int)__builtin_floor((e + bits - 1) * 0.30102999566398114) + 1;
    /* correct k against the exact value */
    for (;;) {
        Bn a = num, b = den;            /* is x >= 10^k ? */
        if (k >= 0) bn_mul_pow10(&b, k); else bn_mul_pow10(&a, -k);
        if (bn_cmp(&a, &b) >= 0) { k++; continue; }
        Bn a2 = num, b2 = den;          /* is x < 10^(k-1) ? */
        if (k - 1 >= 0) bn_mul_pow10(&b2, k - 1); else bn_mul_pow10(&a2, -(k - 1));
        if (bn_cmp(&a2, &b2) < 0) { k--; continue; }
        break;
    }
    int ndig = mode == 0 ? prec : k + prec;     /* digits to produce */
    if (ndig <= 0) {
        /* everything rounds to 0 or to one unit in the last place: compare with half of 10^-prec */
        Bn a = num, b = den;
        /* x >= 0.5 * 10^-prec  <=>  2 * x * 10^prec >= 1 */
        bn_shl(&a, 1);
        bn_mul_pow10(&a, prec);
        if (ndig == 0 && bn_cmp(&a, &b) >= 0) { out[0] = '1'; *point = k + 1; return 1; }
        *point = k;
        return 0;
    }
    /* n = round_half_up(x * 10^(ndig - k)) */
    int scale = ndig - k;
    Bn a = num, b = den;
    if (scale >= 0) bn_mul_pow10(&a, scale); else bn_mul_pow10(&b, -scale);
    /* digits of floor(a / b), then the rounding remainder: produce them one at a time from the top */
    /* b10 = b * 10^(ndig-1): the weight of the first digit */
    Bn w = b;
    bn_mul_pow10(&w, ndig - 1);
    for (int i = 0; i < ndig; i++) {
        int d = 0;
        while (bn_cmp(&a, &w) >= 0) { bn_sub(&a, &w); d++; }
        out[i] = (char)('0' + d);
        if (i + 1 < ndig) {
            bn_mul_small(&a, 10);           /* keep w fixed, scale the remainder instead */
        }
    }
    /* remainder a / w is the fraction after the last digit (a < w); round half up: 2a >= w */
    bn_shl(&a, 1);
    if (bn_cmp(&a, &w) >= 0) {
        int i = ndig - 1;
        while (i >= 0 && out[i] == '9') { out[i] = '0'; i--; }
        if (i >= 0) out[i]++;
        else {
            memmove(out + 1, out, (size_t)ndig);
            out[0] = '1';
            k++;
            if (mode == 0) { /* keep prec digits */ } else ndig++;
        }
    }
    *point = k;
    return mode == 0 ? prec : ndig;
}

int kz_dtoa(double d, char *buf) {
    if (d != d) { memcpy(buf, "NaN", 3); return 3; }
    if (d == 0) { buf[0] = '0'; return 1; }
    int o = 0;
    if (d < 0) { buf[o++] = '-'; d = -d; }
    if (d == __builtin_inf()) { memcpy(buf + o, "Infinity", 8); return o + 8; }
    if (d < 9007199254740992.0 && d == (double)(uint64_t)d) {
        uint64_t u = (uint64_t)d;
        char tmp[24];
        int t = 0;
        while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
        while (t) buf[o++] = tmp[--t];
        return o;
    }
    char dig[48];
    int n;
    int k = shortest_digits(d, dig, &n);
    if (k <= n && n <= 21) {
        memcpy(buf + o, dig, (size_t)k); o += k;
        for (int i = 0; i < n - k; i++) buf[o++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(buf + o, dig, (size_t)n); o += n;
        buf[o++] = '.';
        memcpy(buf + o, dig + n, (size_t)(k - n)); o += k - n;
    } else if (-6 < n && n <= 0) {
        buf[o++] = '0'; buf[o++] = '.';
        for (int i = 0; i < -n; i++) buf[o++] = '0';
        memcpy(buf + o, dig, (size_t)k); o += k;
    } else {
        buf[o++] = dig[0];
        if (k > 1) { buf[o++] = '.'; memcpy(buf + o, dig + 1, (size_t)(k - 1)); o += k - 1; }
        buf[o++] = 'e';
        int ex = n - 1;
        buf[o++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        char tmp[8];
        int t = 0;
        do { tmp[t++] = (char)('0' + ex % 10); ex /= 10; } while (ex);
        while (t) buf[o++] = tmp[--t];
    }
    return o;
}

JSValue kz_num_to_str(JSContext *ctx, double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d && !(d == 0 && __builtin_signbit(d)))
        return kz_int_to_string(ctx, (int32_t)d);
    char buf[64];
    int n = kz_dtoa(d, buf);
    return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)n);
}

JSValue kz_num_to_fixed(JSContext *ctx, double x, int f) {
    char buf[512];
    int o = 0;
    if (x != x) return ctx->rt->atom[KZ_ATOM_NaN];
    if (x >= 1e21 || x <= -1e21) return kz_num_to_str(ctx, x);
    if (x < 0) { buf[o++] = '-'; x = -x; }  /* -0 is not < 0: no sign, as specified */
    char dig[400];
    dig[0] = '0';
    int point = 0, nd = x == 0 ? 0 : exact_digits(x, 1, f, dig, &point);
    /* dig[0..nd) are the decimal digits of n = round(x * 10^f): the point goes before the last f of them */
    int ip = nd - f;
    if (ip <= 0) {
        buf[o++] = '0';
        if (f) {
            buf[o++] = '.';
            for (int i = 0; i < -ip; i++) buf[o++] = '0';
            memcpy(buf + o, dig, (size_t)nd); o += nd;
        }
    } else {
        memcpy(buf + o, dig, (size_t)ip); o += ip;
        if (f) { buf[o++] = '.'; memcpy(buf + o, dig + ip, (size_t)f); o += f; }
    }
    return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)o);
}

static int put_exp(char *buf, int o, int ex) {
    buf[o++] = 'e';
    buf[o++] = ex < 0 ? '-' : '+';
    if (ex < 0) ex = -ex;
    char tmp[8];
    int t = 0;
    do { tmp[t++] = (char)('0' + ex % 10); ex /= 10; } while (ex);
    while (t) buf[o++] = tmp[--t];
    return o;
}

JSValue kz_num_to_exponential(JSContext *ctx, double x, int f) {
    char buf[160];
    int o = 0;
    if (x != x) return ctx->rt->atom[KZ_ATOM_NaN];
    if (x < 0) { buf[o++] = '-'; x = -x; }
    if (x == __builtin_inf()) { memcpy(buf + o, "Infinity", 8); return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)(o + 8)); }
    char dig[120];
    int nd, point;
    if (x == 0) {
        nd = f < 0 ? 1 : f + 1;
        for (int i = 0; i < nd; i++) dig[i] = '0';
        point = 1;
    } else if (f < 0) nd = shortest_digits(x, dig, &point);
    else nd = exact_digits(x, 0, f + 1, dig, &point);
    buf[o++] = dig[0];
    if (nd > 1) { buf[o++] = '.'; memcpy(buf + o, dig + 1, (size_t)(nd - 1)); o += nd - 1; }
    o = put_exp(buf, o, point - 1);
    return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)o);
}

JSValue kz_num_to_precision(JSContext *ctx, double x, int p) {
    char buf[160];
    int o = 0;
    if (x != x) return ctx->rt->atom[KZ_ATOM_NaN];
    if (x < 0) { buf[o++] = '-'; x = -x; }
    if (x == __builtin_inf()) { memcpy(buf + o, "Infinity", 8); return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)(o + 8)); }
    char dig[120];
    dig[0] = '0';
    int e;
    if (x == 0) {
        for (int i = 0; i < p; i++) dig[i] = '0';
        e = 0;
    } else {
        int point;
        exact_digits(x, 0, p, dig, &point);
        e = point - 1;
    }
    if (e < -6 || e >= p) {
        buf[o++] = dig[0];
        if (p > 1) { buf[o++] = '.'; memcpy(buf + o, dig + 1, (size_t)(p - 1)); o += p - 1; }
        o = put_exp(buf, o, e);
    } else if (e == p - 1) {
        memcpy(buf + o, dig, (size_t)p); o += p;
    } else if (e >= 0) {
        memcpy(buf + o, dig, (size_t)(e + 1)); o += e + 1;
        buf[o++] = '.';
        memcpy(buf + o, dig + e + 1, (size_t)(p - (e + 1))); o += p - (e + 1);
    } else {
        buf[o++] = '0'; buf[o++] = '.';
        for (int i = 0; i < -(e + 1); i++) buf[o++] = '0';
        memcpy(buf + o, dig, (size_t)p); o += p;
    }
    return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)o);
}

/* Number.prototype.toString(radix) for radix != 10.  Integers are exact; fractions get digits until the value is
   pinned down within half a unit in the last place of the double, then rounded. */
JSValue kz_num_to_str_radix(JSContext *ctx, double x, int radix) {
    static const char digs[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (x != x) return ctx->rt->atom[KZ_ATOM_NaN];
    if (x == __builtin_inf()) return ctx->rt->atom[KZ_ATOM_Infinity];
    if (x == -__builtin_inf()) return ctx->rt->atom[KZ_ATOM_minus_Infinity];
    bool neg = x < 0;
    if (neg) x = -x;
    double ip = __builtin_floor(x), fp = x - ip;
    char frac[1100];
    int nf = 0;
    if (fp > 0) {
        DU v = { x }, nx;
        nx.u = v.u + 1;
        double delta = 0.5 * (nx.d - x);
        if (delta <= 0) delta = 4.9406564584124654e-324;
        if (fp >= delta) {
            for (;;) {
                fp *= radix;
                delta *= radix;
                int digit = (int)fp;
                frac[nf++] = (char)digit;
                fp -= digit;
                if (fp > 0.5 || (fp == 0.5 && (digit & 1))) {
                    if (fp + delta > 1) {
                        /* round up and propagate */
                        for (;;) {
                            int i = nf - 1;
                            if (i < 0) { ip += 1; break; }
                            if (frac[i] + 1 < radix) { frac[i]++; break; }
                            nf--;
                            if (nf == 0) { ip += 1; break; }
                        }
                        break;
                    }
                }
                if (!(fp >= delta) || nf >= 1099) break;
            }
        }
    }
    /* the integer part, exactly */
    char intd[1100];
    int ni = 0;
    if (ip < 9007199254740992.0) {
        uint64_t u = (uint64_t)ip;
        do { intd[ni++] = digs[u % (uint64_t)radix]; u /= (uint64_t)radix; } while (u);
    } else {
        uint64_t f;
        int e;
        decompose(ip, &f, &e);
        Bn n;
        bn_set(&n, f);
        bn_shl(&n, e);
        while (n.n) {
            uint64_t rem = 0;
            for (int i = n.n - 1; i >= 0; i--) {
                uint64_t cur = (rem << 32) | n.d[i];
                n.d[i] = (uint32_t)(cur / (uint64_t)radix);
                rem = cur % (uint64_t)radix;
            }
            while (n.n && !n.d[n.n - 1]) n.n--;
            intd[ni++] = digs[rem];
        }
    }
    char out[2300];
    int o = 0;
    if (neg) out[o++] = '-';
    while (ni) out[o++] = intd[--ni];
    if (nf) {
        out[o++] = '.';
        for (int i = 0; i < nf; i++) out[o++] = digs[(int)frac[i]];
    }
    return kz_str_from_latin1(ctx, (const uint8_t *)out, (uint32_t)o);
}

int32_t kz_double_to_int32(double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0) return (int32_t)d;
    if (d != d || d == __builtin_inf() || d == -__builtin_inf()) return 0;
    /* modulo 2^32 of the integer part, exactly */
    uint64_t f;
    int e;
    decompose(d < 0 ? -d : d, &f, &e);
    uint32_t r;
    if (e >= 32) r = 0;
    else if (e >= 0) r = (uint32_t)(f << e);
    else r = (uint32_t)(f >> -e);
    if (d < 0) r = (uint32_t)-r;
    return (int32_t)r;
}
