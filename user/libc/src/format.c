/* The printf engine: integers, strings, pointers and exact floating point (%f %F %e %E %g %G %a %A), plus the functions that
 * print into strings (sprintf family) or file descriptors (dprintf). FILE based printing is in stdio.c; both feed a Sink.
 *
 * Floating point output is exact: the decimal digits come from the exact binary value (fpconv.c) and are rounded half to
 * even on that exact value, so printf("%.2f", 0.125) is 0.12, 2.675 gives 2.67 (the double is just below), and any precision
 * works. A long double argument is converted to double. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include "format.h"
#include "fpconv.h"

#define FL_LEFT 1
#define FL_ZERO 2
#define FL_PLUS 4
#define FL_SPACE 8
#define FL_ALT 16

static const char SPACES[32] = { [0 ... 31] = ' ' };
static const char ZEROS[32] = { [0 ... 31] = '0' };

static void emit(Sink *s, const char *p, size_t n) {
    s->count += n;
    if (s->err) return;
    if (s->direct && n >= s->cap && n >= 256) {         /* a big block: no point in copying it through the staging buffer */
        if (s->pos && s->flush(s)) { s->err = 1; return; }
        if (s->direct(s, p, n)) s->err = 1;
        return;
    }
    while (n) {
        size_t room = s->cap - s->pos;
        if (!room) {
            if (!s->flush) return;                      /* snprintf: what does not fit is only counted */
            if (s->flush(s)) { s->err = 1; return; }
            room = s->cap - s->pos;
            if (!room) { s->err = 1; return; }
        }
        size_t k = n < room ? n : room;
        memcpy(s->buf + s->pos, p, k);
        s->pos += k;
        p += k;
        n -= k;
    }
}

static void fill(Sink *s, char c, long n) {
    const char *src = c == ' ' ? SPACES : ZEROS;
    while (n > 0) {
        size_t k = n < 32 ? (size_t)n : 32;
        emit(s, src, k);
        n -= (long)k;
    }
}

/* ---------------------------------------------------------------- integers */

static void fmt_int(Sink *s, unsigned long long v, int base, int upper, int sgn, int neg, int width, int prec, unsigned flags,
                    int force_prefix) {
    char tmp[72];
    char *e = tmp + sizeof tmp, *p = e;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int nonzero = v != 0;
    if (v == 0) {
        if (prec != 0) *--p = '0';
    } else if (base == 10) {
        while (v) { *--p = (char)('0' + v % 10); v /= 10; }
    } else if (base == 16) {
        while (v) { *--p = dig[v & 15]; v >>= 4; }
    } else {
        while (v) { *--p = (char)('0' + (v & 7)); v >>= 3; }
    }
    int n = (int)(e - p);
    if (base == 8 && (flags & FL_ALT) && (n == 0 || *p != '0')) { *--p = '0'; n++; }
    char prefix[3];
    int plen = 0;
    if (neg) prefix[plen++] = '-';
    else if (sgn && (flags & FL_PLUS)) prefix[plen++] = '+';          /* + and space only apply to signed conversions */
    else if (sgn && (flags & FL_SPACE)) prefix[plen++] = ' ';
    if (base == 16 && (force_prefix || ((flags & FL_ALT) && nonzero))) { prefix[plen++] = '0'; prefix[plen++] = upper ? 'X' : 'x'; }
    long zeros = prec > n ? prec - n : 0;
    long total = plen + zeros + n;
    if ((flags & FL_ZERO) && !(flags & FL_LEFT) && prec < 0 && width > total) zeros += width - total;
    total = plen + zeros + n;
    if (!(flags & FL_LEFT)) fill(s, ' ', width - total);
    emit(s, prefix, (size_t)plen);
    fill(s, '0', zeros);
    emit(s, p, (size_t)n);
    if (flags & FL_LEFT) fill(s, ' ', width - total);
}

/* ---------------------------------------------------------------- floating point */

#define BODY_MAX (FP_DIGITS_MAX + 48)

/* [padding] sign prefix [zero padding] body zeros suffix [padding] */
static void put_layout(Sink *s, char sign, const char *prefix, int nprefix, const char *body, int nbody, long zfill,
                       const char *suffix, int nsuffix, int width, unsigned flags) {
    long total = (sign ? 1 : 0) + nprefix + nbody + zfill + nsuffix;
    long padn = width - total;
    if (!(flags & FL_LEFT) && !(flags & FL_ZERO)) fill(s, ' ', padn);
    if (sign) emit(s, &sign, 1);
    if (nprefix) emit(s, prefix, (size_t)nprefix);
    if (!(flags & FL_LEFT) && (flags & FL_ZERO)) fill(s, '0', padn);
    emit(s, body, (size_t)nbody);
    fill(s, '0', zfill);
    if (nsuffix) emit(s, suffix, (size_t)nsuffix);
    if (flags & FL_LEFT) fill(s, ' ', padn);
}

/* Rounds the exact digit string to its first `cut` digits, half to even on the exact value (the generator always provides
 * the digit after the cut and says whether more non-zero digits follow). A carry out of the top digit grows the number by one
 * digit (nd = cut + 1, dp + 1). */
static void round_digits(FpDigits *g, int cut) {
    unsigned char *d = g->d;
    if (cut >= g->nd) return;                           /* exact: nothing is dropped */
    int up;
    int c = d[cut];
    if (c != 5) {
        up = c > 5;
    } else {
        int rest = g->more;
        for (int i = cut + 1; !rest && i < g->nd; i++) if (d[i]) rest = 1;
        up = rest ? 1 : (cut > 0 ? d[cut - 1] & 1 : 0);   /* an exact tie goes to the even neighbour */
    }
    g->nd = cut;
    g->more = 0;
    if (up) {
        int i = cut - 1;
        while (i >= 0 && d[i] == 9) { d[i] = 0; i--; }
        if (i >= 0) {
            d[i]++;
        } else {
            g->d = --d;
            d[0] = 1;
            g->nd = cut + 1;
            g->dp++;
        }
    }
}

static void fmt_hexfloat(Sink *s, uint64_t bits, int prec, unsigned flags, int width, int upper, char sign) {
    int be = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & 0xFFFFFFFFFFFFFULL;
    int lead, e;
    if (be == 0) { lead = 0; e = frac ? -1022 : 0; } else { lead = 1; e = be - 1023; }
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t M = ((uint64_t)lead << 52) | frac;
    unsigned leadd = (unsigned)lead;
    int nd = 13;
    if (prec >= 0 && prec < 13) {                       /* round to prec hex digits, half to even */
        int sh = 4 * (13 - prec);
        uint64_t rem = M & ((1ULL << sh) - 1), half = 1ULL << (sh - 1);
        M >>= sh;
        if (rem > half || (rem == half && (M & 1))) M++;
        nd = prec;
        leadd = (unsigned)(M >> (4 * nd));
        frac = nd ? M & ((1ULL << (4 * nd)) - 1) : 0;
    }
    char body[40];
    int nb = 0;
    long zf = 0;
    body[nb++] = dig[leadd];
    uint64_t val = frac;
    int np = nd;
    if (prec < 0) {                                     /* as many digits as needed */
        np = 13;
        while (np > 0 && (val & 15) == 0) { val >>= 4; np--; }
    } else if (prec > 13) {
        zf = prec - 13;
    }
    if (np > 0 || zf > 0 || (flags & FL_ALT)) body[nb++] = '.';
    for (int i = np - 1; i >= 0; i--) body[nb++] = dig[(val >> (4 * i)) & 15];
    char suffix[8];
    int ns = 0;
    suffix[ns++] = upper ? 'P' : 'p';
    suffix[ns++] = e < 0 ? '-' : '+';
    int ex = e < 0 ? -e : e;
    char t[8];
    int nt = 0;
    do { t[nt++] = (char)('0' + ex % 10); ex /= 10; } while (ex);
    while (nt) suffix[ns++] = t[--nt];
    put_layout(s, sign, upper ? "0X" : "0x", 2, body, nb, zf, suffix, ns, width, flags);
}

static __attribute__((noinline)) void fmt_float(Sink *s, double v, int conv, int upper, int prec, unsigned flags, int width) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    char sign = (bits >> 63) ? '-' : (flags & FL_PLUS) ? '+' : (flags & FL_SPACE) ? ' ' : 0;
    if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
        const char *t = (bits & 0xFFFFFFFFFFFFFULL) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        put_layout(s, sign, "", 0, t, 3, 0, "", 0, width, flags & ~(unsigned)FL_ZERO);
        return;
    }
    if (conv == 'a') { fmt_hexfloat(s, bits, prec, flags, width, upper, sign); return; }
    if (prec < 0) prec = 6;
    if (conv == 'g' && prec == 0) prec = 1;
    int alt = (flags & FL_ALT) != 0;
    int zero = (bits << 1) == 0;

    FpDigits dg;
    char body[BODY_MAX];
    int nb = 0;
    long zfill = 0;
    char suffix[8];
    int ns = 0;

    if (conv == 'f') {
        if (zero) { dg.d = dg.buf + 1; dg.nd = 0; dg.dp = 0; dg.more = 0; }
        else { __fp_digits(v, 0, prec, &dg); round_digits(&dg, dg.dp + prec); }
        int ni = dg.dp;
        if (ni == 0) body[nb++] = '0';
        for (int i = 0; i < ni; i++) body[nb++] = (char)('0' + dg.d[i]);
        int avail = dg.nd - ni;
        if (avail < 0) avail = 0;
        int fa = avail < prec ? avail : prec;
        if (prec > 0 || alt) body[nb++] = '.';
        for (int i = 0; i < fa; i++) body[nb++] = (char)('0' + dg.d[ni + i]);
        zfill = prec - fa;
        put_layout(s, sign, "", 0, body, nb, zfill, suffix, 0, width, flags);
        return;
    }

    /* %e and %g work on P significant digits */
    int P = conv == 'e' ? prec + 1 : prec;
    const unsigned char *sd;
    int nsig, X;
    static const unsigned char zero_digit[1] = { 0 };
    if (zero) {
        sd = zero_digit;
        nsig = 0;
        X = 0;
    } else {
        __fp_digits(v, P, 0, &dg);
        int z = 0;
        while (z < dg.nd && dg.d[z] == 0) z++;
        round_digits(&dg, z + P);
        z = 0;
        while (z < dg.nd && dg.d[z] == 0) z++;
        sd = dg.d + z;
        nsig = dg.nd - z;
        if (nsig > P) nsig = P;
        X = dg.dp - 1 - z;
    }
#define SIGDIG(k) ((k) >= 0 && (k) < nsig ? sd[k] : 0)
    int estyle = conv == 'e' || (conv == 'g' && (X < -4 || X >= P));
    if (estyle) {
        int pe = P - 1;                                 /* digits after the point */
        body[nb++] = (char)('0' + SIGDIG(0));
        int have = nsig - 1;
        if (have > pe) have = pe;
        if (have < 0) have = 0;
        int fcount = pe;
        if (conv == 'g' && !alt) {                      /* %g drops trailing zeros */
            fcount = 0;
            for (int i = 1; i <= have; i++) if (sd[i]) fcount = i;
            have = fcount;
        }
        if (fcount > 0 || alt) body[nb++] = '.';
        for (int i = 1; i <= have; i++) body[nb++] = (char)('0' + sd[i]);
        zfill = fcount - have;
        suffix[ns++] = upper ? 'E' : 'e';
        suffix[ns++] = X < 0 ? '-' : '+';
        int ex = X < 0 ? -X : X;
        if (ex >= 100) suffix[ns++] = (char)('0' + ex / 100);
        suffix[ns++] = (char)('0' + ex / 10 % 10);
        suffix[ns++] = (char)('0' + ex % 10);
    } else {                                            /* %g in fixed notation: -4 <= X < P, so P - 1 - X >= 0 decimals */
        int pf = P - 1 - X;
        if (X < 0) body[nb++] = '0';
        for (int i = 0; i <= X; i++) body[nb++] = (char)('0' + SIGDIG(i));
        int have = nsig - X - 1;
        if (have > pf) have = pf;
        if (have < 0) have = 0;
        int fcount = pf;
        if (!alt) {
            fcount = 0;
            for (int j = 0; j < have; j++) if (SIGDIG(X + 1 + j)) fcount = j + 1;
            have = fcount;
        }
        if (fcount > 0 || alt) body[nb++] = '.';
        for (int j = 0; j < have; j++) body[nb++] = (char)('0' + SIGDIG(X + 1 + j));
        zfill = fcount - have;
    }
#undef SIGDIG
    put_layout(s, sign, "", 0, body, nb, zfill, suffix, ns, width, flags);
}

/* ---------------------------------------------------------------- the engine */

int __vformat(Sink *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            const char *p = fmt;
            while (*fmt && *fmt != '%') fmt++;
            emit(s, p, (size_t)(fmt - p));
            fmt--;
            continue;
        }
        fmt++;
        unsigned flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= FL_LEFT;
            else if (*fmt == '0') flags |= FL_ZERO;
            else if (*fmt == '+') flags |= FL_PLUS;
            else if (*fmt == ' ') flags |= FL_SPACE;
            else if (*fmt == '#') flags |= FL_ALT;
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= FL_LEFT; width = width == INT_MIN ? INT_MAX : -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); if (width > 100000000) width = 100000000; }
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt++ - '0'); if (prec > 100000000) prec = 100000000; }
        }
        int lm = 0;                                     /* 0 int, 1 long, 2 long long, -1 short, -2 char, 3 long double */
        for (;; fmt++) {
            if (*fmt == 'l') lm = lm >= 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') lm = 2;
            else if (*fmt == 'h') lm = lm <= -1 ? -2 : -1;
            else if (*fmt == 'L') lm = 3;
            else break;
        }
        char c = *fmt;
        switch (c) {
        case 'd': case 'i': {
            long long v;
            if (lm >= 1) v = va_arg(ap, long long);
            else if (lm == -1) v = (short)va_arg(ap, int);
            else if (lm == -2) v = (signed char)va_arg(ap, int);
            else v = va_arg(ap, int);
            int neg = v < 0;
            fmt_int(s, neg ? 0ULL - (unsigned long long)v : (unsigned long long)v, 10, 0, 1, neg, width, prec, flags, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            if (lm >= 1) v = va_arg(ap, unsigned long long);
            else if (lm == -1) v = (unsigned short)va_arg(ap, unsigned);
            else if (lm == -2) v = (unsigned char)va_arg(ap, unsigned);
            else v = va_arg(ap, unsigned);
            fmt_int(s, v, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X', 0, 0, width, prec, flags, 0);
            break;
        }
        case 'p':
            fmt_int(s, (unsigned long long)(size_t)va_arg(ap, void *), 16, 0, 0, 0, width, prec, flags, 1);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!(flags & FL_LEFT)) fill(s, ' ', width - 1);
            emit(s, &ch, 1);
            if (flags & FL_LEFT) fill(s, ' ', width - 1);
            break;
        }
        case 's': case 'm': {
            const char *str = c == 'm' ? strerror(errno) : va_arg(ap, const char *);
            if (!str) str = "(null)";
            long len = prec >= 0 ? (long)strnlen(str, (size_t)prec) : (long)strlen(str);
            if (!(flags & FL_LEFT)) fill(s, ' ', width - len);
            emit(s, str, (size_t)len);
            if (flags & FL_LEFT) fill(s, ' ', width - len);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = lm == 3 ? (double)va_arg(ap, long double) : va_arg(ap, double);
            fmt_float(s, v, c | 32, c < 'a', prec, flags, width);
            break;
        }
        case 'n': {
            void *where = va_arg(ap, void *);
            if (lm >= 1) *(long long *)where = (long long)s->count;
            else if (lm == -1) *(short *)where = (short)s->count;
            else if (lm == -2) *(signed char *)where = (signed char)s->count;
            else *(int *)where = (int)s->count;
            break;
        }
        case '%':
            emit(s, "%", 1);
            break;
        case 0:
            fmt--;
            break;
        default:
            emit(s, "%", 1);
            emit(s, &c, 1);
            break;
        }
    }
    return s->count > INT_MAX ? INT_MAX : (int)s->count;
}

/* ---------------------------------------------------------------- printing into strings and descriptors */

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    Sink s = { 0 };
    s.buf = buf;
    s.cap = n ? n - 1 : 0;                              /* room for the terminating NUL */
    __vformat(&s, fmt, ap);
    if (n) buf[s.pos] = 0;
    if (s.count > INT_MAX) { errno = EOVERFLOW; return -1; }
    return (int)s.count;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)INT_MAX, fmt, ap); }

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **out, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n < 0) { va_end(ap2); *out = NULL; return -1; }
    *out = malloc((size_t)n + 1);
    if (!*out) { va_end(ap2); return -1; }
    vsnprintf(*out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return n;
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

static int fd_write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fd_flush(Sink *s) {
    int r = fd_write_all((int)(intptr_t)s->ctx, s->buf, s->pos);
    s->pos = 0;
    return r;
}

static int fd_direct(Sink *s, const char *p, size_t n) { return fd_write_all((int)(intptr_t)s->ctx, p, n); }

int vdprintf(int fd, const char *fmt, va_list ap) {
    char stage[1024];
    Sink s = { 0 };
    s.buf = stage;
    s.cap = sizeof stage;
    s.flush = fd_flush;
    s.direct = fd_direct;
    s.ctx = (void *)(intptr_t)fd;
    __vformat(&s, fmt, ap);
    if (!s.err && s.pos && fd_flush(&s)) s.err = 1;
    if (s.err) return -1;
    return s.count > INT_MAX ? INT_MAX : (int)s.count;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}
