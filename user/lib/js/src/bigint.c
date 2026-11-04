/*
 * bigint.c: BigInt (ECMA-262 6.1.6.2 The BigInt Type, 7.1.13 ToBigInt, 7.1.14 StringToBigInt, 21.2 BigInt Objects, and
 * the BigInt cases of the operators in 6.1.6.2.x and 13.15.3).
 *
 * A BigInt is an immutable heap cell: a sign flag in the header and the magnitude as 32-bit limbs, least significant
 * first, with no leading zero limb (zero has no limbs and is never negative).  32-bit limbs keep every step in 64-bit
 * arithmetic, which is plain C with no compiler extensions; the numbers a script writes are a few limbs long, and the
 * schoolbook algorithms below (Knuth's algorithm D for division) are fast enough for the long ones.  The longest
 * BigInt is 2^30 bits (a RangeError past it, as in other engines); the memory limit usually ends things sooner.
 *
 * Rooting.  Every function that allocates takes its BigInt arguments as values the caller keeps alive (the operators
 * root their operands once, in kz_bigint_binary); the collector does not move cells, so a pointer into an operand's
 * limbs stays valid while the operand is rooted, and the arithmetic itself allocates nothing but its result (and raw
 * scratch buffers, which are not collected).  Long loops poll the host's interrupt handler, so a script that asks for
 * 10n ** 10000000n can be stopped.
 */
#include "jsint.h"

#define BIG_NEG 0x01
#define BIG_MAX_BITS (1u << 30)
#define BIG_MAX_LIMBS (BIG_MAX_BITS / 32)

struct JSBigInt {
    JSGCHeader h;               /* h.aux: the number of limbs in use; h.flags: BIG_NEG */
    uint32_t limb[];
};
#define BIG_HDR offsetof(JSBigInt, limb)
#define BIG_MIN_SIZE 16         /* a cell holds at least the free-list link */

typedef uint32_t limb_t;

static inline JSBigInt *B(JSValue v) { return JV_BIG(v); }
static inline uint32_t nl(const JSBigInt *b) { return b->h.aux; }
static inline bool isneg(const JSBigInt *b) { return (b->h.flags & BIG_NEG) != 0; }

/* ================================================================== cells */

static JSBigInt *big_alloc(JSContext *ctx, uint32_t n) {
    if (n > BIG_MAX_LIMBS) { kz_throw_range(ctx, "Maximum BigInt size exceeded"); return NULL; }
    size_t size = BIG_HDR + (size_t)n * sizeof(limb_t);
    if (size < BIG_MIN_SIZE) size = BIG_MIN_SIZE;
    JSBigInt *b = kz_gc_new(ctx, KZ_GC_BIGINT, size);
    if (!b) return NULL;
    b->h.aux = n;
    return b;
}

/* drops leading zero limbs and sets the sign */
static JSValue big_finish(JSBigInt *b, bool neg) {
    uint32_t n = b->h.aux;
    while (n > 0 && b->limb[n - 1] == 0) n--;
    b->h.aux = n;
    b->h.flags = (neg && n) ? BIG_NEG : 0;
    return JV_MKBIG(b);
}

static JSValue big_zero(JSContext *ctx) {
    JSBigInt *b = big_alloc(ctx, 0);
    return b ? JV_MKBIG(b) : JS_EXCEPTION;
}

static JSValue from_u64(JSContext *ctx, uint64_t mag, bool neg) {
    JSBigInt *b = big_alloc(ctx, mag >> 32 ? 2 : mag ? 1 : 0);
    if (!b) return JS_EXCEPTION;
    if (nl(b) > 0) b->limb[0] = (limb_t)mag;
    if (nl(b) > 1) b->limb[1] = (limb_t)(mag >> 32);
    return big_finish(b, neg);
}

JSValue kz_bigint_from_int64(JSContext *ctx, int64_t v) {
    return from_u64(ctx, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, v < 0);
}

JSValue kz_bigint_from_uint64(JSContext *ctx, uint64_t v) { return from_u64(ctx, v, false); }

/* the low 64 bits of the two's complement value: BigInt64Array and BigUint64Array store these */
int kz_bigint_to_int64(JSContext *ctx, JSValue v, int64_t *out) {
    (void)ctx;
    const JSBigInt *b = B(v);
    uint64_t lo = 0;
    if (nl(b) > 0) lo = b->limb[0];
    if (nl(b) > 1) lo |= (uint64_t)b->limb[1] << 32;
    if (isneg(b)) lo = (uint64_t)0 - lo;
    *out = (int64_t)lo;
    return 0;
}

bool kz_bigint_is_zero(JSValue a) { return nl(B(a)) == 0; }

static JSValue big_copy(JSContext *ctx, const JSBigInt *x, bool neg) {
    JSBigInt *r = big_alloc(ctx, nl(x));
    if (!r) return JS_EXCEPTION;
    memcpy(r->limb, x->limb, (size_t)nl(x) * sizeof(limb_t));
    return big_finish(r, neg);
}

/* ================================================================== magnitudes (limb arrays without leading zeros) */

static int mag_cmp(const limb_t *a, uint32_t na, const limb_t *b, uint32_t nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (uint32_t i = na; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static uint64_t mag_bitlen(const limb_t *a, uint32_t n) {
    return n ? (uint64_t)(n - 1) * 32 + (uint64_t)(32 - __builtin_clz(a[n - 1])) : 0;
}

/* r[0..na] = a + b (na >= nb) */
static void mag_add(limb_t *r, const limb_t *a, uint32_t na, const limb_t *b, uint32_t nb) {
    uint64_t c = 0;
    uint32_t i = 0;
    for (; i < nb; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (limb_t)c; c >>= 32; }
    for (; i < na; i++) { c += a[i]; r[i] = (limb_t)c; c >>= 32; }
    r[na] = (limb_t)c;
}

/* r[0..na-1] = a - b (a >= b, na >= nb) */
static void mag_sub(limb_t *r, const limb_t *a, uint32_t na, const limb_t *b, uint32_t nb) {
    uint64_t borrow = 0;
    uint32_t i = 0;
    for (; i < nb; i++) { uint64_t t = (uint64_t)a[i] - b[i] - borrow; r[i] = (limb_t)t; borrow = (t >> 63) & 1; }
    for (; i < na; i++) { uint64_t t = (uint64_t)a[i] - borrow; r[i] = (limb_t)t; borrow = (t >> 63) & 1; }
}

/* How much work to do between two looks at the interrupt handler. */
#define POLL_WORK (1u << 22)

static int poll(JSContext *ctx) {
    kz_poll_interrupt(ctx);
    return kz_has_exception(ctx) ? -1 : 0;
}

/* r[0..na+nb-1] (zeroed) = a * b, schoolbook; -1 when interrupted */
static int mag_mul(JSContext *ctx, limb_t *r, const limb_t *a, uint32_t na, const limb_t *b, uint32_t nb) {
    uint64_t work = 0;
    for (uint32_t i = 0; i < na; i++) {
        uint64_t ai = a[i], c = 0;
        if (ai) {
            for (uint32_t j = 0; j < nb; j++) {
                c += ai * b[j] + r[i + j];          /* <= (2^32-1)^2 + 2 (2^32-1) = 2^64 - 1 */
                r[i + j] = (limb_t)c;
                c >>= 32;
            }
            r[i + nb] = (limb_t)c;
        }
        work += nb;
        if (work >= POLL_WORK) { work = 0; if (poll(ctx)) return -1; }
    }
    return 0;
}

/* q[0..n-1] = a / d in place-safe fashion; returns the remainder */
static limb_t mag_divmod_small(limb_t *q, const limb_t *a, uint32_t n, limb_t d) {
    uint64_t rem = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint64_t cur = (rem << 32) | a[i];
        q[i] = (limb_t)(cur / d);
        rem = cur % d;
    }
    return (limb_t)rem;
}

/*
 * Knuth, TAOCP vol. 2, 4.3.1, algorithm D, in the form of Hacker's Delight "divmnu": u (nu limbs) / v (nv >= 2 limbs,
 * top limb nonzero, nu >= nv).  un (nu + 1 limbs) and vn (nv limbs) are scratch; q gets nu - nv + 1 limbs; the
 * remainder is left in un[0..nv-1], shifted left by s bits (the caller shifts it back).  Returns the shift s, or -1
 * when interrupted.
 */
static int knuth_div(JSContext *ctx, limb_t *q, limb_t *un, limb_t *vn, const limb_t *u, uint32_t nu, const limb_t *v, uint32_t nv) {
    int s = __builtin_clz(v[nv - 1]);
    for (uint32_t i = nv - 1; i > 0; i--) vn[i] = (v[i] << s) | (s ? (limb_t)((uint64_t)v[i - 1] >> (32 - s)) : 0);
    vn[0] = v[0] << s;
    un[nu] = s ? (limb_t)((uint64_t)u[nu - 1] >> (32 - s)) : 0;
    for (uint32_t i = nu - 1; i > 0; i--) un[i] = (u[i] << s) | (s ? (limb_t)((uint64_t)u[i - 1] >> (32 - s)) : 0);
    un[0] = u[0] << s;
    const uint64_t base = 1ULL << 32;
    uint64_t work = 0;
    for (int64_t j = (int64_t)nu - (int64_t)nv; j >= 0; j--) {
        uint64_t num = ((uint64_t)un[j + nv] << 32) | un[j + nv - 1];
        uint64_t qhat = num / vn[nv - 1];
        uint64_t rhat = num % vn[nv - 1];
        while (qhat >= base || qhat * vn[nv - 2] > ((rhat << 32) | un[j + nv - 2])) {
            qhat--;
            rhat += vn[nv - 1];
            if (rhat >= base) break;
        }
        int64_t k = 0, t;
        for (uint32_t i = 0; i < nv; i++) {
            uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (limb_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + nv] - k;
        un[j + nv] = (limb_t)t;
        q[j] = (limb_t)qhat;
        if (t < 0) {                                 /* qhat was one too big: add the divisor back */
            q[j]--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < nv; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (limb_t)c;
                c >>= 32;
            }
            un[j + nv] += (limb_t)c;
        }
        work += nv;
        if (work >= POLL_WORK) { work = 0; if (poll(ctx)) return -1; }
    }
    return s;
}

/* ================================================================== the operators */

static JSValue big_addsub(JSContext *ctx, const JSBigInt *x, const JSBigInt *y, bool sub) {
    bool xn = isneg(x), yn = isneg(y) != sub;
    uint32_t nx = nl(x), ny = nl(y);
    if (xn == yn) {
        const JSBigInt *p = x, *q = y;
        uint32_t np = nx, nq = ny;
        if (np < nq) { p = y; q = x; np = ny; nq = nx; }
        JSBigInt *r = big_alloc(ctx, np + 1);
        if (!r) return JS_EXCEPTION;
        mag_add(r->limb, p->limb, np, q->limb, nq);
        return big_finish(r, xn);
    }
    int c = mag_cmp(x->limb, nx, y->limb, ny);
    if (c == 0) return big_zero(ctx);
    const JSBigInt *p = x, *q = y;
    uint32_t np = nx, nq = ny;
    bool pn = xn;
    if (c < 0) { p = y; q = x; np = ny; nq = nx; pn = yn; }
    JSBigInt *r = big_alloc(ctx, np);
    if (!r) return JS_EXCEPTION;
    mag_sub(r->limb, p->limb, np, q->limb, nq);
    return big_finish(r, pn);
}

static JSValue big_mul(JSContext *ctx, const JSBigInt *x, const JSBigInt *y) {
    uint32_t nx = nl(x), ny = nl(y);
    if (nx == 0 || ny == 0) return big_zero(ctx);
    JSBigInt *r = big_alloc(ctx, nx + ny);
    if (!r) return JS_EXCEPTION;
    if (mag_mul(ctx, r->limb, x->limb, nx, y->limb, ny)) return JS_EXCEPTION;
    return big_finish(r, isneg(x) != isneg(y));
}

/* x / y (truncated toward zero) or x % y (the sign of x) */
static JSValue big_divmod(JSContext *ctx, JSValue xv, JSValue yv, bool want_rem) {
    const JSBigInt *x = B(xv), *y = B(yv);
    uint32_t nx = nl(x), ny = nl(y);
    if (ny == 0) return kz_throw_range(ctx, "Division by zero");
    bool xn = isneg(x), yn = isneg(y);
    if (mag_cmp(x->limb, nx, y->limb, ny) < 0) return want_rem ? xv : big_zero(ctx);   /* |x| < |y| (values are immutable) */
    if (ny == 1) {
        limb_t d = y->limb[0];
        if (want_rem) {
            uint64_t rem = 0;
            for (uint32_t i = nx; i-- > 0;) rem = (((rem << 32) | x->limb[i])) % d;
            return from_u64(ctx, rem, xn);
        }
        JSBigInt *q = big_alloc(ctx, nx);
        if (!q) return JS_EXCEPTION;
        mag_divmod_small(q->limb, x->limb, nx, d);
        return big_finish(q, xn != yn);
    }
    /* scratch: un (nx + 1), vn (ny), q (nx - ny + 1) */
    size_t qn = (size_t)nx - ny + 1;
    limb_t *tmp = kz_malloc(ctx, ((size_t)nx + 1 + ny + qn) * sizeof(limb_t));
    if (!tmp) return JS_EXCEPTION;
    limb_t *un = tmp, *vn = tmp + nx + 1, *q = vn + ny;
    int s = knuth_div(ctx, q, un, vn, x->limb, nx, y->limb, ny);
    if (s < 0) { kz_free(ctx->rt, tmp); return JS_EXCEPTION; }
    JSBigInt *r;
    if (want_rem) {
        r = big_alloc(ctx, ny);
        if (r) {
            for (uint32_t i = 0; i < ny; i++) r->limb[i] = (un[i] >> s) | (s ? (limb_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
        }
    } else {
        r = big_alloc(ctx, (uint32_t)qn);
        if (r) memcpy(r->limb, q, qn * sizeof(limb_t));
    }
    kz_free(ctx->rt, tmp);
    if (!r) return JS_EXCEPTION;
    return big_finish(r, want_rem ? xn : xn != yn);
}

/* the magnitude of a shift count, saturating */
static uint64_t count_of(const JSBigInt *y) {
    if (nl(y) > 2) return UINT64_MAX;
    uint64_t c = nl(y) > 0 ? y->limb[0] : 0;
    if (nl(y) > 1) c |= (uint64_t)y->limb[1] << 32;
    return c;
}

static JSValue big_shift(JSContext *ctx, JSValue xv, JSValue yv, bool shift_left) {
    const JSBigInt *x = B(xv), *y = B(yv);
    bool left = shift_left != isneg(y);              /* a negative count shifts the other way */
    uint64_t cnt = count_of(y);
    uint32_t nx = nl(x);
    if (nx == 0 || cnt == 0) return xv;
    if (left) {
        if (cnt >= BIG_MAX_BITS || mag_bitlen(x->limb, nx) + cnt > BIG_MAX_BITS) return kz_throw_range(ctx, "Maximum BigInt size exceeded");
        uint32_t nr = nx + (uint32_t)(cnt / 32) + 1;
        JSBigInt *r = big_alloc(ctx, nr);
        if (!r) return JS_EXCEPTION;
        uint32_t w = (uint32_t)(cnt / 32), s = (uint32_t)(cnt % 32);
        for (uint32_t i = 0; i < nx; i++) {
            uint64_t v = (uint64_t)x->limb[i] << s;
            r->limb[i + w] |= (limb_t)v;
            r->limb[i + w + 1] |= (limb_t)(v >> 32);
        }
        return big_finish(r, isneg(x));
    }
    /* right: floor division by 2^cnt, so a negative number rounds away from zero when bits are lost */
    if (cnt >= (uint64_t)nx * 32) return isneg(x) ? kz_bigint_from_int64(ctx, -1) : big_zero(ctx);
    uint32_t w = (uint32_t)(cnt / 32), s = (uint32_t)(cnt % 32);
    uint32_t nr = nx - w;
    JSBigInt *r = big_alloc(ctx, nr + 1);
    if (!r) return JS_EXCEPTION;
    bool lost = false;
    for (uint32_t i = 0; i < w; i++) if (x->limb[i]) lost = true;
    if (s && (x->limb[w] & (((limb_t)1 << s) - 1))) lost = true;
    for (uint32_t i = w; i < nx; i++) {
        uint64_t v = x->limb[i];
        if (i + 1 < nx) v |= (uint64_t)x->limb[i + 1] << 32;
        r->limb[i - w] = (limb_t)(v >> s);
    }
    if (isneg(x) && lost) {
        for (uint32_t i = 0; i <= nr; i++) if (++r->limb[i] != 0) break;
    }
    return big_finish(r, isneg(x));
}

/* the limbs of a two's complement view of a BigInt, one at a time from the bottom (infinitely sign-extended) */
typedef struct TcIter { const JSBigInt *b; uint32_t borrow; } TcIter;

static limb_t tc_limb(TcIter *it, uint32_t i) {
    limb_t v = i < nl(it->b) ? it->b->limb[i] : 0;
    if (!isneg(it->b)) return v;
    limb_t d = v - it->borrow;                       /* -m = ~(m - 1) */
    it->borrow = (v < it->borrow) ? 1 : 0;
    return ~d;
}

static JSValue big_bitwise(JSContext *ctx, const JSBigInt *x, const JSBigInt *y, int op) {
    uint32_t m = (nl(x) > nl(y) ? nl(x) : nl(y)) + 1;     /* the top limb is only the sign extension */
    JSBigInt *r = big_alloc(ctx, m);
    if (!r) return JS_EXCEPTION;
    TcIter ix = { x, 1 }, iy = { y, 1 };                  /* (the borrow only matters for negative operands) */
    bool xn = isneg(x), yn = isneg(y), neg;
    switch (op) {
    case KZ_BIN_AND: neg = xn && yn; break;
    case KZ_BIN_OR: neg = xn || yn; break;
    default: neg = xn != yn; break;
    }
    for (uint32_t i = 0; i < m; i++) {
        limb_t a = tc_limb(&ix, i), b = tc_limb(&iy, i);
        r->limb[i] = op == KZ_BIN_AND ? (a & b) : op == KZ_BIN_OR ? (a | b) : (a ^ b);
    }
    if (neg) {                                            /* back to a magnitude: negate the two's complement */
        uint64_t c = 1;
        for (uint32_t i = 0; i < m; i++) {
            c += (limb_t)~r->limb[i];
            r->limb[i] = (limb_t)c;
            c >>= 32;
        }
    }
    return big_finish(r, neg);
}

JSValue kz_bigint_neg(JSContext *ctx, JSValue a) {
    const JSBigInt *x = B(a);
    if (nl(x) == 0) return a;
    return big_copy(ctx, x, !isneg(x));
}

/* ~x = -x - 1 */
JSValue kz_bigint_not(JSContext *ctx, JSValue a) {
    const JSBigInt *x = B(a);
    uint32_t n = nl(x);
    if (!isneg(x)) {                                 /* -(x + 1) */
        JSBigInt *r = big_alloc(ctx, n + 1);
        if (!r) return JS_EXCEPTION;
        limb_t one[1] = { 1 };
        mag_add(r->limb, x->limb, n, one, n ? 1 : 0);
        if (n == 0) r->limb[0] = 1;
        return big_finish(r, true);
    }
    JSBigInt *r = big_alloc(ctx, n);                 /* |x| - 1 */
    if (!r) return JS_EXCEPTION;
    limb_t one[1] = { 1 };
    mag_sub(r->limb, x->limb, n, one, 1);
    return big_finish(r, false);
}

static JSValue big_pow(JSContext *ctx, JSValue base, JSValue ev) {
    const JSBigInt *b = B(base), *e = B(ev);
    if (isneg(e)) return kz_throw_range(ctx, "Exponent must be non-negative");
    if (nl(e) == 0) return kz_bigint_from_int64(ctx, 1);
    if (nl(b) == 0) return base;
    if (nl(b) == 1 && b->limb[0] == 1) {             /* 1 and -1 stay as they are, whatever the exponent */
        if (isneg(b) && !(e->limb[0] & 1)) return kz_bigint_from_int64(ctx, 1);
        return base;
    }
    if (nl(e) > 1) return kz_throw_range(ctx, "Maximum BigInt size exceeded");
    uint32_t n = e->limb[0];
    uint64_t bits = mag_bitlen(b->limb, nl(b));
    bool neg = isneg(b) && (n & 1);
    if ((bits - 1) * n >= BIG_MAX_BITS) return kz_throw_range(ctx, "Maximum BigInt size exceeded");
    if (nl(b) >= 1 && (b->limb[nl(b) - 1] & (b->limb[nl(b) - 1] - 1)) == 0) {      /* |base| = 2^k perhaps: a shift */
        bool pow2 = true;
        for (uint32_t i = 0; i + 1 < nl(b); i++) if (b->limb[i]) pow2 = false;
        if (pow2) {
            uint64_t total = (bits - 1) * n;
            JSBigInt *r = big_alloc(ctx, (uint32_t)(total / 32) + 1);
            if (!r) return JS_EXCEPTION;
            r->limb[total / 32] = (limb_t)1 << (total % 32);
            return big_finish(r, neg);
        }
    }
    KZ_ROOTS(ctx, r, 2);                             /* the result so far, base^(2^k) */
    r[0] = kz_bigint_from_int64(ctx, 1);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = isneg(b) ? kz_bigint_neg(ctx, base) : base;      /* work with |base|; the sign is decided at the end */
    if (r[1] == JS_EXCEPTION) return r[1];
    for (;;) {
        if (n & 1) {
            JSValue t = big_mul(ctx, B(r[0]), B(r[1]));
            if (t == JS_EXCEPTION) return t;
            r[0] = t;
        }
        n >>= 1;
        if (!n) break;
        JSValue t = big_mul(ctx, B(r[1]), B(r[1]));
        if (t == JS_EXCEPTION) return t;
        r[1] = t;
    }
    return neg ? kz_bigint_neg(ctx, r[0]) : r[0];
}

JSValue kz_bigint_binary(JSContext *ctx, int op, JSValue a, JSValue b) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = a;
    r[1] = b;
    switch (op) {
    case KZ_BIN_ADD: return big_addsub(ctx, B(a), B(b), false);
    case KZ_BIN_SUB: return big_addsub(ctx, B(a), B(b), true);
    case KZ_BIN_MUL: return big_mul(ctx, B(a), B(b));
    case KZ_BIN_DIV: return big_divmod(ctx, a, b, false);
    case KZ_BIN_MOD: return big_divmod(ctx, a, b, true);
    case KZ_BIN_POW: return big_pow(ctx, a, b);
    case KZ_BIN_AND: case KZ_BIN_OR: case KZ_BIN_XOR: return big_bitwise(ctx, B(a), B(b), op);
    case KZ_BIN_SHL: return big_shift(ctx, a, b, true);
    case KZ_BIN_SAR: return big_shift(ctx, a, b, false);
    case KZ_BIN_SHR: return kz_throw_type(ctx, "BigInts have no unsigned right shift, use >> instead");
    }
    return kz_throw_type(ctx, "unsupported BigInt operation");
}

/* ================================================================== comparison */

int kz_bigint_cmp(JSValue a, JSValue b) {
    const JSBigInt *x = B(a), *y = B(b);
    bool xn = isneg(x), yn = isneg(y);
    if (xn != yn) return xn ? -1 : 1;
    int c = mag_cmp(x->limb, nl(x), y->limb, nl(y));
    return xn ? -c : c;
}

bool kz_bigint_eq(JSValue a, JSValue b) { return a == b || kz_bigint_cmp(a, b) == 0; }

uint32_t kz_bigint_hash(JSValue a) {
    const JSBigInt *x = B(a);
    uint32_t h = isneg(x) ? 0x9E3779B9u : 0x811C9DC5u;
    for (uint32_t i = 0; i < nl(x); i++) h = (h ^ x->limb[i]) * 16777619u;
    return h ^ (h >> 15);
}

/* ================================================================== doubles */

/* the magnitude of a finite integral double (>= 1) as limbs: returns the limb count (at most 32) */
static uint32_t double_limbs(double ad, limb_t out[34], bool *frac) {
    uint64_t bits;
    memcpy(&bits, &ad, 8);
    int e = (int)((bits >> 52) & 0x7FF) - 1023;
    uint64_t m = (bits & ((1ULL << 52) - 1)) | (1ULL << 52);
    *frac = false;
    for (int i = 0; i < 34; i++) out[i] = 0;
    if (e < 52) {
        uint64_t sh = (uint64_t)(52 - e);
        *frac = (m & ((1ULL << sh) - 1)) != 0;
        m >>= sh;
        out[0] = (limb_t)m;
        out[1] = (limb_t)(m >> 32);
        return out[1] ? 2 : out[0] ? 1 : 0;
    }
    uint32_t sh = (uint32_t)(e - 52);
    uint32_t w = sh / 32, s = sh % 32;
    uint64_t lo = m << s, hi = s ? m >> (64 - s) : 0;    /* m << s as 128 bits: m has 53 bits, s < 32, so hi has at most 20 */
    out[w] = (limb_t)lo;
    out[w + 1] = (limb_t)(lo >> 32);
    out[w + 2] = (limb_t)hi;
    uint32_t n = w + 3;
    while (n > 0 && out[n - 1] == 0) n--;
    return n;
}

JSValue kz_bigint_from_double(JSContext *ctx, double d) {
    double ad = d < 0 ? -d : d;
    if (ad < 1) return big_zero(ctx);                /* (the caller checked that d is integral and finite) */
    limb_t limbs[34];
    bool frac;
    uint32_t n = double_limbs(ad, limbs, &frac);
    JSBigInt *r = big_alloc(ctx, n);
    if (!r) return JS_EXCEPTION;
    memcpy(r->limb, limbs, (size_t)n * sizeof(limb_t));
    return big_finish(r, d < 0);
}

/* 2^k scaling of a normal double (k >= 0), saturating to infinity */
static double scale_up(double d, int64_t k) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int64_t e = (int64_t)((bits >> 52) & 0x7FF) + k;
    if (e >= 0x7FF) return KZ_INFINITY;
    bits = (bits & ~(0x7FFULL << 52)) | ((uint64_t)e << 52);
    memcpy(&d, &bits, 8);
    return d;
}

/* round to nearest, ties to even (the specification's Number::... of a BigInt) */
double kz_bigint_to_double(JSValue v) {
    const JSBigInt *b = B(v);
    uint32_t n = nl(b);
    if (n == 0) return 0;
    uint64_t bitlen = mag_bitlen(b->limb, n);
    double r;
    if (bitlen <= 64) {
        uint64_t m = b->limb[0];
        if (n > 1) m |= (uint64_t)b->limb[1] << 32;
        r = (double)m;                               /* the conversion rounds to nearest even */
    } else {
        /* the top 64 bits, with every lower bit folded into the lowest one as a "sticky" bit: the 11 bits below
           the double's 53 then decide the rounding exactly */
        uint64_t sh = bitlen - 64;
        uint32_t w = (uint32_t)(sh / 32), s = (uint32_t)(sh % 32);
        /* bits [sh, sh + 64) of the magnitude: they span the limbs w, w + 1 and w + 2 */
        uint64_t lo = b->limb[w] | ((uint64_t)(w + 1 < n ? b->limb[w + 1] : 0) << 32);
        uint64_t hi = w + 2 < n ? b->limb[w + 2] : 0;
        uint64_t top = s ? (lo >> s) | (hi << (64 - s)) : lo;
        bool sticky = false;
        for (uint32_t i = 0; i < w && !sticky; i++) if (b->limb[i]) sticky = true;
        if (!sticky && s && (b->limb[w] & (((limb_t)1 << s) - 1))) sticky = true;
        if (sticky) top |= 1;
        r = scale_up((double)top, (int64_t)sh);
    }
    return isneg(b) ? -r : r;
}

/* -1, 0, 1: how the BigInt compares to d; 2 when d is NaN */
int kz_bigint_cmp_double(JSValue a, double d) {
    if (d != d) return 2;
    const JSBigInt *x = B(a);
    if (d == KZ_INFINITY) return -1;
    if (d == -KZ_INFINITY) return 1;
    bool xz = nl(x) == 0, xn = isneg(x);
    if (d == 0) return xz ? 0 : (xn ? -1 : 1);
    bool dn = d < 0;
    if (xz) return dn ? 1 : -1;
    if (xn != dn) return xn ? -1 : 1;
    double ad = dn ? -d : d;
    int c;                                           /* of the magnitudes */
    if (ad < 1) c = 1;                               /* |x| >= 1 > |d| */
    else {
        limb_t limbs[34];
        bool frac;
        uint32_t n = double_limbs(ad, limbs, &frac);
        c = mag_cmp(x->limb, nl(x), limbs, n);
        if (c == 0 && frac) c = -1;                  /* |x| = floor(|d|) < |d| */
    }
    return xn ? -c : c;
}

/* ================================================================== strings */

static const char digit_chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";

static int digit_val(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A') + 10;
    return 99;
}

/* The digits s[0..len) of a number in `radix`, as a BigInt; JS_UNDEFINED when one is not a digit of the radix. */
static JSValue parse_digits(JSContext *ctx, const void *s, bool wide, size_t len, int radix, bool neg) {
#define CH(i) (wide ? ((const uint16_t *)s)[i] : ((const uint8_t *)s)[i])
    for (size_t i = 0; i < len; i++) if (digit_val(CH(i)) >= radix) return JS_UNDEFINED;
    while (len > 1 && CH(0) == '0') { s = wide ? (const void *)((const uint16_t *)s + 1) : (const void *)((const uint8_t *)s + 1); len--; }
    if (len == 0) return big_zero(ctx);
    int bpd = 32 - __builtin_clz((unsigned)(radix - 1));           /* ceil(log2 radix) */
    uint64_t bits = (uint64_t)len * (uint64_t)bpd;
    if (bits > (uint64_t)BIG_MAX_BITS + 64) return kz_throw_range(ctx, "Maximum BigInt size exceeded");
    uint32_t cap = (uint32_t)(bits / 32) + 2;
    JSBigInt *r = big_alloc(ctx, cap);
    if (!r) return JS_EXCEPTION;
    if ((radix & (radix - 1)) == 0) {                /* a power of two: pack the bits from the low end */
        uint64_t pos = 0;
        for (size_t i = len; i-- > 0;) {
            uint64_t v = (uint64_t)digit_val(CH(i)) << (pos % 32);
            r->limb[pos / 32] |= (limb_t)v;
            r->limb[pos / 32 + 1] |= (limb_t)(v >> 32);
            pos += (uint64_t)bpd;
        }
        return big_finish(r, neg);
    }
    /* any other radix: multiply-and-add in chunks that fit a limb */
    uint32_t mult = (uint32_t)radix, k = 1;
    while ((uint64_t)mult * (uint64_t)radix <= 0xFFFFFFFFu) { mult *= (uint32_t)radix; k++; }
    uint32_t used = 0;
    uint64_t work = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t take = len - i < k ? (uint32_t)(len - i) : k;
        uint32_t m = 1, val = 0;
        for (uint32_t j = 0; j < take; j++) { val = val * (uint32_t)radix + (uint32_t)digit_val(CH(i + j)); m *= (uint32_t)radix; }
        i += take;
        uint64_t c = val;
        for (uint32_t j = 0; j < used; j++) {
            c += (uint64_t)r->limb[j] * m;
            r->limb[j] = (limb_t)c;
            c >>= 32;
        }
        if (c) r->limb[used++] = (limb_t)c;
        work += used + 1;
        if (work >= POLL_WORK) { work = 0; if (poll(ctx)) return JS_EXCEPTION; }
    }
    return big_finish(r, neg);
#undef CH
}

JSValue kz_bigint_literal(JSContext *ctx, const char *digits, size_t len, int radix) {
    JSValue v = parse_digits(ctx, digits, false, len, radix, false);
    if (v == JS_UNDEFINED) return kz_throw_syntax(ctx, "Invalid BigInt literal");
    return v;
}

static bool is_ws(uint32_t c) {
    return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029 || kz_unicode_is_space(c);
}

/* StringToBigInt (7.1.14): JS_UNDEFINED when the text is not a BigInt */
JSValue kz_bigint_from_string(JSContext *ctx, JSString *s) {
    s = kz_str_flat(ctx, s);
    if (!s) return JS_EXCEPTION;
    size_t a = 0, e = kz_str_len(s);
    while (a < e && is_ws(kz_str_at(s, (uint32_t)a))) a++;
    while (e > a && is_ws(kz_str_at(s, (uint32_t)(e - 1)))) e--;
    if (a == e) return big_zero(ctx);
    bool wide = kz_str_wide(s);
    const void *base = wide ? (const void *)kz_str16(s) : (const void *)kz_str8(s);
#define AT(i) (wide ? (uint32_t)((const uint16_t *)base)[i] : (uint32_t)((const uint8_t *)base)[i])
#define OFF(i) (wide ? (const void *)((const uint16_t *)base + (i)) : (const void *)((const uint8_t *)base + (i)))
    int radix = 10;
    bool neg = false;
    size_t d = a;
    if (e - a >= 2 && AT(a) == '0') {
        uint32_t p = AT(a + 1) | 0x20;
        if (p == 'x') radix = 16; else if (p == 'o') radix = 8; else if (p == 'b') radix = 2;
        if (radix != 10) d = a + 2;
    }
    if (radix == 10 && (AT(a) == '+' || AT(a) == '-')) { neg = AT(a) == '-'; d = a + 1; }
    if (d == e) return JS_UNDEFINED;                 /* "0x", "-", "" after a sign */
    return parse_digits(ctx, OFF(d), wide, e - d, radix, neg);
#undef AT
#undef OFF
}

/* digits of the magnitude into the end of buf (cap bytes); returns the offset of the first digit, or cap + 1 when interrupted */
static size_t digits_into(JSContext *ctx, uint8_t *buf, size_t cap, const JSBigInt *b, int radix) {
    uint32_t n = nl(b);
    size_t pos = cap;
    if (n <= 2) {                                    /* the common case: one machine word */
        uint64_t v = n > 0 ? b->limb[0] : 0;
        if (n > 1) v |= (uint64_t)b->limb[1] << 32;
        do { buf[--pos] = (uint8_t)digit_chars[v % (uint64_t)radix]; v /= (uint64_t)radix; } while (v);
        return pos;
    }
    if ((radix & (radix - 1)) == 0) {
        int bpd = __builtin_ctz((unsigned)radix);
        uint64_t total = mag_bitlen(b->limb, n);
        for (uint64_t bit = 0; bit < total; bit += (uint64_t)bpd) {
            uint32_t w = (uint32_t)(bit / 32), s = (uint32_t)(bit % 32);
            uint64_t v = b->limb[w];
            if (w + 1 < n) v |= (uint64_t)b->limb[w + 1] << 32;
            buf[--pos] = (uint8_t)digit_chars[(v >> s) & (uint64_t)(radix - 1)];
        }
        return pos;
    }
    limb_t *tmp = kz_malloc(ctx, (size_t)n * sizeof(limb_t));
    if (!tmp) return cap + 1;
    memcpy(tmp, b->limb, (size_t)n * sizeof(limb_t));
    uint32_t chunk = (uint32_t)radix, k = 1;
    while ((uint64_t)chunk * (uint64_t)radix <= 0xFFFFFFFFu) { chunk *= (uint32_t)radix; k++; }
    uint32_t len = n;
    uint64_t work = 0;
    while (len) {
        limb_t rem = mag_divmod_small(tmp, tmp, len, chunk);
        while (len && tmp[len - 1] == 0) len--;
        if (len) { for (uint32_t j = 0; j < k; j++) { buf[--pos] = (uint8_t)digit_chars[rem % (uint32_t)radix]; rem /= (uint32_t)radix; } }
        else { while (rem) { buf[--pos] = (uint8_t)digit_chars[rem % (uint32_t)radix]; rem /= (uint32_t)radix; } }
        work += len + 1;
        if (work >= POLL_WORK / 4) { work = 0; if (poll(ctx)) { kz_free(ctx->rt, tmp); return cap + 1; } }
    }
    kz_free(ctx->rt, tmp);
    return pos;
}

JSValue kz_bigint_to_string(JSContext *ctx, JSValue v, int radix) {
    const JSBigInt *b = B(v);
    uint32_t n = nl(b);
    if (n == 0) return kz_str_from_latin1(ctx, (const uint8_t *)"0", 1);
    int bpd = 31 - __builtin_clz((unsigned)radix);                  /* floor(log2 radix) >= 1: an upper bound on the digit count */
    size_t cap = (size_t)((uint64_t)n * 32 / (uint64_t)bpd) + 4;
    uint8_t small[80];
    uint8_t *buf = cap <= sizeof small ? small : kz_malloc(ctx, cap);
    if (!buf) return JS_EXCEPTION;
    size_t pos = digits_into(ctx, buf, cap, b, radix);
    JSValue r;
    if (pos > cap) r = JS_EXCEPTION;
    else {
        if (isneg(b)) buf[--pos] = '-';
        r = kz_str_from_latin1(ctx, buf + pos, (uint32_t)(cap - pos));
    }
    if (buf != small) kz_free(ctx->rt, buf);
    return r;
}

/* ================================================================== conversions from values */

/* NumberToBigInt (21.2.1.1.1) */
static JSValue number_to_bigint(JSContext *ctx, JSValue n) {
    if (JV_IS_INT(n)) return kz_bigint_from_int64(ctx, JV_INT(n));
    double d = jv_dbl(n);
    if (d != d || d == KZ_INFINITY || d == -KZ_INFINITY || kz_math_trunc(d) != d) {
        char buf[64];
        kz_format_value_for_msg(ctx, n, buf, sizeof buf);
        return kz_throw_range(ctx, "The number %s cannot be converted to a BigInt because it is not an integer", buf);
    }
    return kz_bigint_from_double(ctx, d);
}

/* ToBigInt (7.1.13) */
JSValue kz_to_bigint(JSContext *ctx, JSValue v) {
    KZ_ROOT(ctx, p);
    p = kz_to_primitive(ctx, v, KZ_HINT_NUMBER);
    if (p == JS_EXCEPTION) return p;
    if (JV_IS_BIGINT(p)) return p;
    if (p == JS_TRUE) return kz_bigint_from_int64(ctx, 1);
    if (p == JS_FALSE) return big_zero(ctx);
    if (JV_IS_STRING(p)) {
        JSValue n = kz_bigint_from_string(ctx, JV_STR(p));
        if (n == JS_UNDEFINED) {
            char buf[100];
            kz_format_value_for_msg(ctx, p, buf, sizeof buf);
            return kz_throw_syntax(ctx, "Cannot convert %s to a BigInt", buf);
        }
        return n;
    }
    char buf[100];
    kz_format_value_for_msg(ctx, p, buf, sizeof buf);
    return kz_throw_type(ctx, "Cannot convert %s to a BigInt", buf);
}

/* ================================================================== the BigInt constructor and prototype */

static JSValue bigint_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (ctx->rt->new_target != JS_UNDEFINED) return kz_throw_type(ctx, "BigInt is not a constructor");
    KZ_ROOT(ctx, p);
    p = kz_to_primitive(ctx, argv[0], KZ_HINT_NUMBER);
    if (p == JS_EXCEPTION) return p;
    if (JV_IS_NUMBER(p)) return number_to_bigint(ctx, p);
    return kz_to_bigint(ctx, p);
}

/* BigInt.asIntN (0) and BigInt.asUintN (1): the low `bits` bits of the two's complement value */
static JSValue bigint_as_n(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    uint64_t bits;
    if (kz_to_index(ctx, argv[0], &bits)) return JS_EXCEPTION;
    KZ_ROOT(ctx, xv);
    xv = kz_to_bigint(ctx, argv[1]);
    if (xv == JS_EXCEPTION) return xv;
    const JSBigInt *x = B(xv);
    uint32_t n = nl(x);
    bool is_signed = magic == 0;
    if (bits == 0) return big_zero(ctx);
    uint64_t xbits = mag_bitlen(x->limb, n);
    if (!isneg(x)) {
        if (is_signed ? xbits < bits : xbits <= bits) return xv;                         /* already in range */
    } else if (is_signed) {
        /* -2^(bits-1) <= x: |x| < 2^(bits-1), or |x| = 2^(bits-1) */
        if (xbits < bits) return xv;
        if (xbits == bits) {
            bool pow2 = (x->limb[n - 1] & (x->limb[n - 1] - 1)) == 0;
            for (uint32_t i = 0; i + 1 < n && pow2; i++) if (x->limb[i]) pow2 = false;
            if (pow2) return xv;
        }
    }
    if (bits > BIG_MAX_BITS) {
        /* only a negative unsigned result (2^bits - |x| mod ...) would need that many bits */
        return kz_throw_range(ctx, "Maximum BigInt size exceeded");
    }
    uint32_t k = (uint32_t)((bits + 31) / 32);
    JSBigInt *r = big_alloc(ctx, k);
    if (!r) return JS_EXCEPTION;
    x = B(xv);
    TcIter it = { x, 1 };
    for (uint32_t i = 0; i < k; i++) r->limb[i] = tc_limb(&it, i);
    uint32_t top_bits = (uint32_t)(bits % 32);
    if (top_bits) r->limb[k - 1] &= ((limb_t)1 << top_bits) - 1;
    bool neg = false;
    if (is_signed && ((r->limb[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1)) {            /* the sign bit is set: u - 2^bits */
        uint64_t c = 1;
        for (uint32_t i = 0; i < k; i++) { c += (limb_t)~r->limb[i]; r->limb[i] = (limb_t)c; c >>= 32; }
        if (top_bits) r->limb[k - 1] &= ((limb_t)1 << top_bits) - 1;
        neg = true;
    }
    return big_finish(r, neg);
}

static int this_bigint(JSContext *ctx, JSValue v, JSValue *out, const char *method) {
    if (JV_IS_BIGINT(v)) { *out = v; return 0; }
    if (kz_is_class(v, KZ_CLASS_BIGINT)) { *out = JV_OBJ(v)->u.prim; return 0; }
    kz_throw_type(ctx, "BigInt.prototype.%s requires that 'this' be a BigInt", method);
    return -1;
}

static JSValue proto_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOT(ctx, x);
    if (this_bigint(ctx, this_val, &x, "toString")) return JS_EXCEPTION;
    double radix = 10;
    if (argv[0] != JS_UNDEFINED && kz_to_integer_or_inf(ctx, argv[0], &radix)) return JS_EXCEPTION;
    if (radix < 2 || radix > 36) return kz_throw_range(ctx, "toString() radix must be between 2 and 36");
    return kz_bigint_to_string(ctx, x, (int)radix);
}

/* the en-US style of Number.prototype.toLocaleString: digit groups of three */
static JSValue proto_to_locale_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    if (this_bigint(ctx, this_val, &r[0], "toLocaleString")) return JS_EXCEPTION;
    r[1] = kz_bigint_to_string(ctx, r[0], 10);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[1] = kz_str_flatten(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSString *s = JV_STR(r[1]);
    uint32_t len = kz_str_len(s), start = (len && kz_str_at(s, 0) == '-') ? 1 : 0, digits = len - start;
    if (digits <= 3) return r[1];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    if (start) kz_sb_putc(&sb, '-');
    for (uint32_t i = 0; i < digits; i++) {
        if (i && (digits - i) % 3 == 0) kz_sb_putc(&sb, ',');
        kz_sb_putc(&sb, kz_str_at(s, start + i));
    }
    return kz_sb_finish(&sb);
}

static JSValue proto_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue x;
    if (this_bigint(ctx, this_val, &x, "valueOf")) return JS_EXCEPTION;
    return x;
}

static const KzFuncDef bigint_funcs[] = {
    KZ_FN_MAGIC("asIntN", bigint_as_n, 2, 0),
    KZ_FN_MAGIC("asUintN", bigint_as_n, 2, 1),
};

static const KzFuncDef bigint_proto_funcs[] = {
    KZ_FN("toLocaleString", proto_to_locale_string, 0),
    KZ_FN_ARGS("toString", proto_to_string, 0, 1),
    KZ_FN("valueOf", proto_value_of, 0),
};

int kz_init_bigint(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_BIGINT_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, bigint_ctor, "BigInt", 1, 0, KZ_I_BIGINT_PROTO, KZ_I_BIGINT);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], bigint_funcs, KZ_COUNTOF(bigint_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], bigint_proto_funcs, KZ_COUNTOF(bigint_proto_funcs))) return -1;
    r[2] = kz_atom(ctx, "BigInt");
    if (r[2] == JS_EXCEPTION || kz_define_value(ctx, r[0], ctx->rt->sym[KZ_SYM_toStringTag], r[2], KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_BigInt, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
