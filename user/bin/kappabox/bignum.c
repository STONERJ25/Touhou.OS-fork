/* bignum: decimal arbitrary precision for bc and dc.  See bignum.h for the representation and the scale rules. */
#include "bignum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;

#define BASE 1000000000u
static const u32 P10[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static Num *num_alloc(int cap) {
    Num *r = xmalloc(sizeof *r);
    r->neg = r->scale = r->n = 0;
    r->cap = cap < 1 ? 1 : cap;
    r->d = xmalloc((size_t)r->cap * sizeof(u32));
    return r;
}

/* ---- magnitudes: little endian arrays of base 10^9 limbs, no leading zero limbs ---- */

static int mag_cmp(const u32 *a, int na, const u32 *b, int nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (int i = na - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int mag_add(const u32 *a, int na, const u32 *b, int nb, u32 *out) {
    if (na < nb) {
        const u32 *t = a; a = b; b = t;
        int tn = na; na = nb; nb = tn;
    }
    u32 carry = 0;
    int i;
    for (i = 0; i < na; i++) {
        u32 s = a[i] + (i < nb ? b[i] : 0) + carry;
        if (s >= BASE) { s -= BASE; carry = 1; } else carry = 0;
        out[i] = s;
    }
    if (carry) out[i++] = 1;
    return i;
}

/* a >= b */
static int mag_sub(const u32 *a, int na, const u32 *b, int nb, u32 *out) {
    int borrow = 0;
    for (int i = 0; i < na; i++) {
        i64 s = (i64)a[i] - (i < nb ? (i64)b[i] : 0) - borrow;
        if (s < 0) { s += BASE; borrow = 1; } else borrow = 0;
        out[i] = (u32)s;
    }
    while (na > 0 && out[na - 1] == 0) na--;
    return na;
}

/* out has na+nb limbs and must not overlap the inputs */
static int mag_mul(const u32 *a, int na, const u32 *b, int nb, u32 *out) {
    if (!na || !nb) return 0;
    memset(out, 0, (size_t)(na + nb) * sizeof(u32));
    for (int i = 0; i < na; i++) {
        u64 carry = 0, ai = a[i];
        if (!ai) continue;
        for (int j = 0; j < nb; j++) {
            u64 t = ai * b[j] + out[i + j] + carry;
            carry = t / BASE;
            out[i + j] = (u32)(t - carry * BASE);
        }
        out[i + nb] = (u32)carry;
    }
    int n = na + nb;
    while (n > 0 && out[n - 1] == 0) n--;
    return n;
}

/* out has na+1 limbs; may be the same array as a */
static int mag_mulsmall(const u32 *a, int na, u32 m, u32 *out) {
    u64 carry = 0;
    for (int i = 0; i < na; i++) {
        u64 t = (u64)a[i] * m + carry;
        carry = t / BASE;
        out[i] = (u32)(t - carry * BASE);
    }
    int n = na;
    if (carry) out[n++] = (u32)carry;
    while (n > 0 && out[n - 1] == 0) n--;
    return n;
}

/* q (na limbs, may be the same array as a) = a / d; returns the remainder */
static u32 mag_divsmall(const u32 *a, int na, u32 d, u32 *q, int *nq) {
    u64 rem = 0;
    for (int i = na - 1; i >= 0; i--) {
        u64 cur = rem * BASE + a[i];
        u64 qq = cur / d;
        rem = cur - qq * d;
        q[i] = (u32)qq;
    }
    int n = na;
    while (n > 0 && q[n - 1] == 0) n--;
    *nq = n;
    return (u32)rem;
}

/* Long division (Knuth algorithm D).  b != 0.  q has room for na-nb+1 limbs (or 1), r for nb; either may be NULL. */
static void mag_divmod(const u32 *a, int na, const u32 *b, int nb, u32 *q, int *nq, u32 *r, int *nr) {
    if (na < nb) {
        if (q) *nq = 0;
        if (r) { memcpy(r, a, (size_t)na * sizeof(u32)); *nr = na; }
        return;
    }
    if (nb == 1) {
        int n1;
        u32 *qq = q ? q : xmalloc((size_t)na * sizeof(u32));
        u32 rem = mag_divsmall(a, na, b[0], qq, &n1);
        if (q) *nq = n1; else free(qq);
        if (r) { r[0] = rem; *nr = rem ? 1 : 0; }
        return;
    }
    u32 *u = xmalloc((size_t)(na + 1) * sizeof(u32));
    u32 *v = xmalloc((size_t)nb * sizeof(u32));
    u32 d = BASE / (b[nb - 1] + 1);
    u64 carry = 0;
    for (int i = 0; i < nb; i++) {
        u64 t = (u64)b[i] * d + carry;
        carry = t / BASE;
        v[i] = (u32)(t - carry * BASE);
    }
    carry = 0;
    for (int i = 0; i < na; i++) {
        u64 t = (u64)a[i] * d + carry;
        carry = t / BASE;
        u[i] = (u32)(t - carry * BASE);
    }
    u[na] = (u32)carry;
    u64 v1 = v[nb - 1], v2 = v[nb - 2];
    int qn = na - nb + 1;
    u32 *qd = q ? q : xmalloc((size_t)qn * sizeof(u32));
    for (int j = na - nb; j >= 0; j--) {
        u64 num = (u64)u[j + nb] * BASE + u[j + nb - 1];
        u64 qhat = num / v1, rhat = num % v1;
        while (qhat >= BASE || qhat * v2 > rhat * BASE + u[j + nb - 2]) {
            qhat--;
            rhat += v1;
            if (rhat >= BASE) break;
        }
        u64 mc = 0;
        i64 borrow = 0;
        for (int i = 0; i < nb; i++) {
            u64 p = qhat * v[i] + mc;
            mc = p / BASE;
            i64 t = (i64)u[i + j] - (i64)(p - mc * BASE) - borrow;
            if (t < 0) { t += BASE; borrow = 1; } else borrow = 0;
            u[i + j] = (u32)t;
        }
        i64 t = (i64)u[j + nb] - (i64)mc - borrow;
        if (t < 0) {
            t += BASE;
            qhat--;
            u64 c = 0;
            for (int i = 0; i < nb; i++) {
                u64 s = (u64)u[i + j] + v[i] + c;
                c = s / BASE;
                u[i + j] = (u32)(s - c * BASE);
            }
            t = (i64)(((u64)t + c) % BASE);
        }
        u[j + nb] = (u32)t;
        qd[j] = (u32)qhat;
    }
    if (q) {
        int n = qn;
        while (n > 0 && qd[n - 1] == 0) n--;
        *nq = n;
    } else free(qd);
    if (r) {
        int n1;
        mag_divsmall(u, nb, d, r, &n1);
        *nr = n1;
    }
    free(u);
    free(v);
}

/* out = a * 10^k (capacity na + k/9 + 1, not overlapping a) */
static int mag_shl10(const u32 *a, int na, int k, u32 *out) {
    if (na == 0) return 0;
    int q = k / 9, r = k % 9;
    for (int i = 0; i < q; i++) out[i] = 0;
    return mag_mulsmall(a, na, P10[r], out + q) + q;
}

/* out = floor(a / 10^k) (capacity na) */
static int mag_shr10(const u32 *a, int na, int k, u32 *out) {
    int q = k / 9, r = k % 9;
    if (q >= na) return 0;
    int n = na - q, nq;
    if (r == 0) {
        memmove(out, a + q, (size_t)n * sizeof(u32));
        return n;
    }
    mag_divsmall(a + q, n, P10[r], out, &nq);
    return nq;
}

/* a = a mod 10^k, in place */
static int mag_mod10(u32 *a, int na, int k) {
    int q = k / 9, r = k % 9;
    if (q < na) {
        if (r) { a[q] %= P10[r]; na = q + 1; }
        else na = q;
    }
    while (na > 0 && a[na - 1] == 0) na--;
    return na;
}

static int mag_digits(const u32 *a, int na) {
    if (!na) return 0;
    int d = (na - 1) * 9, t = 1;
    u32 top = a[na - 1];
    while (top >= 10) { top /= 10; t++; }
    return d + t;
}

/* ---- numbers ---- */

Num *num_new(void) { return num_alloc(1); }

Num *num_from_long(long v) {
    Num *r = num_alloc(3);
    unsigned long u;
    if (v < 0) { r->neg = 1; u = 0UL - (unsigned long)v; } else u = (unsigned long)v;
    while (u) { r->d[r->n++] = (u32)(u % BASE); u /= BASE; }
    if (!r->n) r->neg = 0;
    return r;
}

Num *num_copy(const Num *a) {
    Num *r = num_alloc(a->n);
    memcpy(r->d, a->d, (size_t)a->n * sizeof(u32));
    r->n = a->n;
    r->neg = a->neg;
    r->scale = a->scale;
    return r;
}

void num_free(Num *a) {
    if (!a) return;
    free(a->d);
    free(a);
}

int num_is_zero(const Num *a) { return a->n == 0; }

/* a with its scale raised to newscale (>= a->scale) */
static Num *scale_up(const Num *a, int newscale) {
    int k = newscale - a->scale;
    Num *r = num_alloc(a->n + k / 9 + 2);
    r->n = mag_shl10(a->d, a->n, k, r->d);
    r->neg = a->neg;
    r->scale = newscale;
    return r;
}

Num *num_setscale(const Num *a, int s) {
    if (s < 0) s = 0;
    if (s >= a->scale) return scale_up(a, s);
    Num *r = num_alloc(a->n + 1);
    r->n = mag_shr10(a->d, a->n, a->scale - s, r->d);
    r->scale = s;
    r->neg = r->n ? a->neg : 0;
    return r;
}

Num *num_intpart(const Num *a) { return num_setscale(a, 0); }

int num_cmp(const Num *a, const Num *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c;
    if (a->scale == b->scale) c = mag_cmp(a->d, a->n, b->d, b->n);
    else if (a->scale < b->scale) {
        Num *t = scale_up(a, b->scale);
        c = mag_cmp(t->d, t->n, b->d, b->n);
        num_free(t);
    } else {
        Num *t = scale_up(b, a->scale);
        c = mag_cmp(a->d, a->n, t->d, t->n);
        num_free(t);
    }
    return a->neg ? -c : c;
}

Num *num_neg(const Num *a) {
    Num *r = num_copy(a);
    if (r->n) r->neg = !r->neg;
    return r;
}

static Num *addsub(const Num *a, const Num *b, int sub) {
    int bneg = sub ? !b->neg : b->neg;
    int sc = a->scale > b->scale ? a->scale : b->scale;
    Num *ta = a->scale < sc ? scale_up(a, sc) : NULL;
    Num *tb = b->scale < sc ? scale_up(b, sc) : NULL;
    const Num *x = ta ? ta : a, *y = tb ? tb : b;
    Num *r = num_alloc((x->n > y->n ? x->n : y->n) + 1);
    r->scale = sc;
    if (x->neg == bneg) {
        r->n = mag_add(x->d, x->n, y->d, y->n, r->d);
        r->neg = x->neg;
    } else {
        int c = mag_cmp(x->d, x->n, y->d, y->n);
        if (c > 0) { r->n = mag_sub(x->d, x->n, y->d, y->n, r->d); r->neg = x->neg; }
        else if (c < 0) { r->n = mag_sub(y->d, y->n, x->d, x->n, r->d); r->neg = bneg; }
    }
    if (!r->n) r->neg = 0;
    num_free(ta);
    num_free(tb);
    return r;
}

Num *num_add(const Num *a, const Num *b) { return addsub(a, b, 0); }
Num *num_sub(const Num *a, const Num *b) { return addsub(a, b, 1); }

Num *num_mul(const Num *a, const Num *b, int scale) {
    Num *r = num_alloc(a->n + b->n + 1);
    r->n = mag_mul(a->d, a->n, b->d, b->n, r->d);
    r->scale = a->scale + b->scale;
    r->neg = r->n ? (a->neg != b->neg) : 0;
    int m = scale;
    if (a->scale > m) m = a->scale;
    if (b->scale > m) m = b->scale;
    int want = r->scale < m ? r->scale : m;
    if (want < r->scale) {
        Num *t = num_setscale(r, want);
        num_free(r);
        r = t;
    }
    return r;
}

Num *num_div(const Num *a, const Num *b, int scale) {
    if (b->n == 0) return NULL;
    if (scale < 0) scale = 0;
    Num *r;
    if (a->n == 0) {
        r = num_alloc(1);
        r->scale = scale;
        return r;
    }
    int e = scale + b->scale - a->scale;
    const u32 *num, *den;
    int nn, nd;
    u32 *tnum = NULL, *tden = NULL;
    if (e >= 0) {
        tnum = xmalloc((size_t)(a->n + e / 9 + 2) * sizeof(u32));
        nn = mag_shl10(a->d, a->n, e, tnum);
        num = tnum; den = b->d; nd = b->n;
    } else {
        tden = xmalloc((size_t)(b->n + (-e) / 9 + 2) * sizeof(u32));
        nd = mag_shl10(b->d, b->n, -e, tden);
        den = tden; num = a->d; nn = a->n;
    }
    r = num_alloc(nn >= nd ? nn - nd + 1 : 1);
    if (nn >= nd) mag_divmod(num, nn, den, nd, r->d, &r->n, NULL, NULL);
    r->scale = scale;
    r->neg = r->n ? (a->neg != b->neg) : 0;
    free(tnum);
    free(tden);
    return r;
}

Num *num_mod(const Num *a, const Num *b, int scale) {
    if (b->n == 0) return NULL;
    int rscale = a->scale > b->scale + scale ? a->scale : b->scale + scale;
    Num *q = num_div(a, b, scale);
    Num *t = num_mul(q, b, rscale);
    Num *r = num_sub(a, t);
    num_free(q);
    num_free(t);
    return r;
}

int num_divmod(const Num *a, const Num *b, Num **quot, Num **rem) {
    if (b->n == 0) return -1;
    Num *q = num_div(a, b, 0);
    Num *t = num_mul(q, b, INT_MAX);
    *rem = num_sub(a, t);
    num_free(t);
    *quot = q;
    return 0;
}

int num_to_long(const Num *a, long *out) {
    Num *ip = num_setscale(a, 0);
    unsigned long v = 0;
    int ok = 1;
    for (int i = ip->n - 1; i >= 0 && ok; i--) {
        if (v > (ULONG_MAX - ip->d[i]) / BASE) ok = 0;
        else v = v * BASE + ip->d[i];
    }
    if (ok) {
        if (ip->neg) { if (v > (unsigned long)LONG_MAX + 1UL) ok = 0; else *out = (long)(0UL - v); }
        else { if (v > (unsigned long)LONG_MAX) ok = 0; else *out = (long)v; }
    }
    num_free(ip);
    return ok;
}

int num_length(const Num *a) {
    int d = mag_digits(a->d, a->n);
    int ip = d > a->scale ? d - a->scale : 0;
    if (ip < 1) ip = 1;
    return ip + a->scale;
}

Num *num_pow(const Num *a, const Num *b, int scale) {
    long e;
    if (!num_to_long(b, &e)) return NULL;
    if (e == 0) return num_from_long(1);
    int neg = e < 0;
    unsigned long ue = neg ? 0UL - (unsigned long)e : (unsigned long)e;
    long long rscale;
    if (neg) rscale = scale;
    else {
        long long full = (long long)a->scale * (long long)(ue > 1000000000UL ? 1000000000UL : ue);
        long long mx = scale > a->scale ? scale : a->scale;
        rscale = full < mx ? full : mx;
    }
    Num *result = num_from_long(1), *base = num_copy(a), *t;
    while (ue) {
        if (ue & 1) { t = num_mul(result, base, INT_MAX); num_free(result); result = t; }
        ue >>= 1;
        if (ue) { t = num_mul(base, base, INT_MAX); num_free(base); base = t; }
    }
    num_free(base);
    if (neg) {
        Num *one = num_from_long(1);
        t = num_div(one, result, (int)rscale);
        num_free(one);
        num_free(result);
        return t;
    }
    if (result->scale > rscale) {
        t = num_setscale(result, (int)rscale);
        num_free(result);
        result = t;
    }
    return result;
}

Num *num_sqrt(const Num *a, int scale) {
    if (a->neg) return NULL;
    int rscale = scale > a->scale ? scale : a->scale;
    Num *r;
    if (a->n == 0) {
        r = num_alloc(1);
        r->scale = rscale;
        return r;
    }
    int sh = 2 * rscale - a->scale;
    int nn = a->n + sh / 9 + 2;
    u32 *N = xmalloc((size_t)nn * sizeof(u32));
    nn = mag_shl10(a->d, a->n, sh, N);
    int dig = mag_digits(N, nn);
    /* Newton: start above the root (10^ceil(dig/2)) and go down until the value stops falling */
    int xk = (dig + 1) / 2;
    u32 *x = xmalloc((size_t)(xk / 9 + 3) * sizeof(u32));
    u32 one = 1;
    int nx = mag_shl10(&one, 1, xk, x);
    u32 *qb = xmalloc((size_t)(nn + 2) * sizeof(u32));
    u32 *sb = xmalloc((size_t)(nn + 3) * sizeof(u32));
    u32 *yb = xmalloc((size_t)(nn + 3) * sizeof(u32));
    for (;;) {
        int nq, ns, ny;
        mag_divmod(N, nn, x, nx, qb, &nq, NULL, NULL);
        ns = mag_add(x, nx, qb, nq, sb);
        mag_divsmall(sb, ns, 2, yb, &ny);
        if (mag_cmp(yb, ny, x, nx) >= 0) break;
        memcpy(x, yb, (size_t)ny * sizeof(u32));
        nx = ny;
    }
    r = num_alloc(nx);
    memcpy(r->d, x, (size_t)nx * sizeof(u32));
    r->n = nx;
    r->scale = rscale;
    free(N); free(x); free(qb); free(sb); free(yb);
    return r;
}

Num *num_modexp(const Num *base, const Num *e, const Num *m) {
    Num *bi = num_setscale(base, 0), *ei = num_setscale(e, 0), *mi = num_setscale(m, 0);
    Num *res = NULL;
    if (mi->n == 0 || ei->neg) goto done;
    {
        Num *r = num_from_long(1), *b = NULL, *q = NULL, *t, *rem;
        num_divmod(r, mi, &q, &rem);        /* r = 1 mod m */
        num_free(r); num_free(q);
        r = rem;
        num_divmod(bi, mi, &q, &rem);
        num_free(q);
        b = rem;
        u32 *w = xmalloc((size_t)(ei->n + 1) * sizeof(u32));
        memcpy(w, ei->d, (size_t)ei->n * sizeof(u32));
        int nw = ei->n;
        while (nw > 0) {
            int nq2;
            u32 bit = mag_divsmall(w, nw, 2, w, &nq2);
            nw = nq2;
            if (bit) {
                t = num_mul(r, b, INT_MAX);
                num_divmod(t, mi, &q, &rem);
                num_free(t); num_free(q); num_free(r);
                r = rem;
            }
            if (nw > 0) {
                t = num_mul(b, b, INT_MAX);
                num_divmod(t, mi, &q, &rem);
                num_free(t); num_free(q); num_free(b);
                b = rem;
            }
        }
        free(w);
        num_free(b);
        res = r;
    }
done:
    num_free(bi); num_free(ei); num_free(mi);
    return res;
}

/* ---- text ---- */

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Num *num_parse(const char *s, int base, const char **end) {
    const char *p = s;
    int idig = 0, fdig = 0;
    while (digit_value(*p) >= 0) { p++; idig++; }
    const char *dot = NULL;
    if (*p == '.') {
        dot = p++;
        while (digit_value(*p) >= 0) { p++; fdig++; }
    }
    *end = p;
    int total = idig + fdig;
    if (total == 0) return num_new();
    /* all digits, integer part first */
    int *dv = xmalloc((size_t)total * sizeof(int));
    int k = 0;
    for (const char *q = s; q < p; q++) {
        if (q == dot) continue;
        int v = digit_value(*q);
        if (total > 1 && v >= base) v = base - 1;
        dv[k++] = v;
    }
    Num *m;
    if (base == 10) {
        int lead = 0;
        while (lead < total && dv[lead] == 0) lead++;
        int len = total - lead;
        m = num_alloc(len / 9 + 2);
        int limbs = (len + 8) / 9;
        for (int i = 0; i < limbs; i++) {
            /* limb i covers digits ending len-1-9*i */
            int hi = lead + len - 9 * i;       /* one past the last digit of this limb */
            int lo = hi - 9 < lead ? lead : hi - 9;
            u32 v = 0;
            for (int j = lo; j < hi; j++) v = v * 10 + (u32)dv[j];
            m->d[i] = v;
        }
        m->n = limbs;
        while (m->n > 0 && m->d[m->n - 1] == 0) m->n--;
        m->scale = fdig;
    } else {
        m = num_alloc(total / 4 + 3);
        for (int i = 0; i < total; i++) {
            int n = mag_mulsmall(m->d, m->n, (u32)base, m->d);
            if (m->n == 0) n = 0;
            m->n = n;
            if (dv[i]) {
                if (m->n == 0) { m->d[0] = (u32)dv[i]; m->n = 1; }
                else {
                    u32 c = (u32)dv[i];
                    int j = 0;
                    while (c) {
                        if (j == m->n) { m->d[m->n++] = 0; }
                        u32 s2 = m->d[j] + c;
                        if (s2 >= BASE) { s2 -= BASE; c = 1; } else c = 0;
                        m->d[j++] = s2;
                    }
                }
            }
        }
        if (fdig) {
            Num *den = num_from_long(1), *t;
            for (int i = 0; i < fdig; i++) {
                t = num_alloc(den->n + 1);
                t->n = mag_mulsmall(den->d, den->n, (u32)base, t->d);
                num_free(den);
                den = t;
            }
            t = num_div(m, den, fdig);
            num_free(m);
            num_free(den);
            m = t;
        }
    }
    free(dv);
    if (m->n == 0) m->neg = 0;
    return m;
}

typedef struct { char *s; size_t n, cap; } Buf;

static void buf_put(Buf *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + n + 1) nc *= 2;
        char *t = realloc(b->s, nc);
        if (!t) { fputs("out of memory\n", stderr); exit(1); }
        b->s = t;
        b->cap = nc;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void buf_digit(Buf *b, int obase, int v, int width) {
    if (obase <= 16) {
        char c = "0123456789ABCDEF"[v];
        buf_put(b, &c, 1);
    } else {
        char tmp[16];
        snprintf(tmp, sizeof tmp, " %0*d", width, v);
        buf_put(b, tmp, strlen(tmp));
    }
}

char *num_to_string(const Num *a, int obase) {
    Buf b = { 0 };
    if (a->n == 0) {
        buf_put(&b, "0", 1);
        return b.s;
    }
    if (a->neg) buf_put(&b, "-", 1);
    if (obase == 10) {
        int L = mag_digits(a->d, a->n);
        char *dg = xmalloc((size_t)L + 1);
        int pos = 0;
        char tmp[16];
        for (int i = a->n - 1; i >= 0; i--) {
            if (i == a->n - 1) snprintf(tmp, sizeof tmp, "%u", (unsigned)a->d[i]);
            else snprintf(tmp, sizeof tmp, "%09u", (unsigned)a->d[i]);
            int l = (int)strlen(tmp);
            memcpy(dg + pos, tmp, (size_t)l);
            pos += l;
        }
        dg[L] = 0;
        if (a->scale == 0) buf_put(&b, dg, (size_t)L);
        else if (L > a->scale) {
            buf_put(&b, dg, (size_t)(L - a->scale));
            buf_put(&b, ".", 1);
            buf_put(&b, dg + L - a->scale, (size_t)a->scale);
        } else {
            buf_put(&b, ".", 1);
            for (int i = L; i < a->scale; i++) buf_put(&b, "0", 1);
            buf_put(&b, dg, (size_t)L);
        }
        free(dg);
        return b.s;
    }
    /* other bases: the integer part by repeated division, the fraction by repeated multiplication */
    int width = 1;
    for (int t = obase - 1; t >= 10; t /= 10) width++;
    u32 chunk = 1;
    int cdig = 0;
    while ((u64)chunk * (u64)obase < BASE) { chunk *= (u32)obase; cdig++; }
    if (cdig == 0) { chunk = (u32)obase; cdig = 1; }     /* obase >= 10^9: one digit per division */
    u32 *w = xmalloc((size_t)(a->n + 1) * sizeof(u32));
    int nw = mag_shr10(a->d, a->n, a->scale, w);
    int cap = 64, nd = 0;
    int *digs = xmalloc((size_t)cap * sizeof(int));
    while (nw > 0) {
        int nn2;
        u32 rem = mag_divsmall(w, nw, chunk, w, &nn2);
        nw = nn2;
        for (int i = 0; i < cdig; i++) {
            if (nw == 0 && rem == 0) break;
            if (nd == cap) { cap *= 2; digs = realloc(digs, (size_t)cap * sizeof(int)); if (!digs) exit(1); }
            digs[nd++] = (int)(rem % (u32)obase);
            rem /= (u32)obase;
        }
    }
    for (int i = nd - 1; i >= 0; i--) buf_digit(&b, obase, digs[i], width);
    free(digs);
    free(w);
    if (a->scale > 0) {
        u32 *f = xmalloc((size_t)(a->n + 3) * sizeof(u32));
        memcpy(f, a->d, (size_t)a->n * sizeof(u32));
        int nf = mag_mod10(f, a->n, a->scale);
        int count = 0;
        {   /* like GNU bc: digits are produced while obase^k has no more than SCALE decimal digits */
            double t = 1.0;
            int e10 = 0;
            while (e10 < a->scale) {
                count++;
                t *= obase;
                while (t >= 10.0) { t /= 10.0; e10++; }
            }
        }
        if (count > 0) {
            buf_put(&b, ".", 1);
            u32 *g = xmalloc((size_t)(a->n + 3) * sizeof(u32));
            for (int i = 0; i < count; i++) {
                nf = mag_mulsmall(f, nf, (u32)obase, f);
                int ng = mag_shr10(f, nf, a->scale, g);
                int dgt = 0;
                if (ng > 0) dgt = (int)g[0] + (ng > 1 ? (int)((u64)g[1] * BASE) : 0);
                nf = mag_mod10(f, nf, a->scale);
                buf_digit(&b, obase, dgt, width);
            }
            free(g);
        }
        free(f);
    }
    return b.s;
}
