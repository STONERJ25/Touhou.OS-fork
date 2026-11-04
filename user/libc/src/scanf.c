/* scanf: the conversion engine (%d %i %u %o %x %X %p %e %f %g %a %c %s %[ %n %%, assignment suppression, widths, length
 * modifiers) over a character source, and sscanf/vsscanf. FILE based scanning is glued on in stdio.c. Floating point input is
 * parsed with strtod/strtof, so it is exactly rounded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include "scan.h"

#define NONE (-2)                               /* "no look-ahead character": the field width is used up */

typedef struct { Src *src; long nread; } Rd;

static int rd_get(Rd *r) {
    int c = r->src->get(r->src->ctx);
    if (c != EOF) r->nread++;
    return c;
}

static void rd_unget(Rd *r, int c) {
    if (c == EOF || c == NONE) return;
    r->src->unget(r->src->ctx, c);
    r->nread--;
}

/* Consumes the look-ahead character c: counts it against the width w and fetches the next one while width is left. */
#define ADV() ((--w > 0) ? rd_get(r) : NONE)
#define HAVE(c) ((c) != EOF && (c) != NONE)

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* Integer field. Returns 1 with the magnitude in *val (saturated at ULLONG_MAX), 0 for a matching failure (no digits), -1 when
 * the input ended before the first character. base 0 means %i: a 0x prefix selects 16, a leading 0 selects 8. */
static int scan_int(Rd *r, int base, int width, unsigned long long *val, int *neg, int *overflow) {
    int w = width > 0 ? width : INT_MAX, c = rd_get(r);
    unsigned long long v = 0;
    int any = 0, over = 0;
    *neg = 0;
    *overflow = 0;
    if (c == EOF) return -1;
    if (c == '+' || c == '-') { *neg = c == '-'; c = ADV(); }
    if ((base == 0 || base == 16) && c == '0') {
        any = 1;
        c = ADV();
        if (c == 'x' || c == 'X') {
            int xch = c;
            c = ADV();
            if (HAVE(c) && digit_value(c) >= 0 && digit_value(c) < 16) { base = 16; any = 0; }
            else { rd_unget(r, c); rd_unget(r, xch); *val = 0; return 1; }      /* "0x" without digits: just the 0 */
        } else if (base == 0 && (c == 'b' || c == 'B')) {                       /* C23: 0b101 */
            int bch = c;
            c = ADV();
            if (HAVE(c) && (c == '0' || c == '1')) { base = 2; any = 0; }
            else { rd_unget(r, c); rd_unget(r, bch); *val = 0; return 1; }
        } else if (base == 0) {
            base = 8;
        }
    } else if (base == 0) {
        base = 10;
    }
    while (HAVE(c)) {
        int d = digit_value(c);
        if (d < 0 || d >= base) break;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) over = 1;
        else v = v * (unsigned)base + (unsigned)d;
        any = 1;
        c = ADV();
    }
    rd_unget(r, c);
    if (!any) return 0;
    *val = over ? ULLONG_MAX : v;
    *overflow = over;
    return 1;
}

#define FBUF 1024

/* Floating point field: the longest prefix of [+-] (digits[.digits][e[+-]digits] | 0xhex[.hex][p[+-]digits] | inf | infinity |
 * nan | nan(chars)) up to the width, collected into buf (NUL terminated, FBUF characters at most). Characters read too far
 * (the "e+" of "1e+x") are given back. Returns 1 on success, 0 for a matching failure and -1 for input failure. */
static int scan_float_text(Rd *r, int width, char *buf) {
    int w = width > 0 ? width : INT_MAX, c = rd_get(r), n = 0;
    if (c == EOF) return -1;
#define TAKE() do { buf[n++] = (char)c; c = n < FBUF - 1 ? ADV() : NONE; } while (0)
#define GIVEBACK(keep) do { rd_unget(r, c); c = NONE; while (n > (keep)) rd_unget(r, (unsigned char)buf[--n]); } while (0)
    if (c == '+' || c == '-') TAKE();
    int signlen = n;
    if (HAVE(c) && (c | 32) == 'i') {                                  /* inf, infinity */
        static const char word[] = "infinity";
        int k = 0;
        while (HAVE(c) && k < 8 && (c | 32) == word[k]) { TAKE(); k++; }
        if (k < 3) { GIVEBACK(0); return 0; }
        if (k != 3 && k != 8) GIVEBACK(signlen + 3);                   /* "infin" is "inf" followed by "in" */
        rd_unget(r, c);
        buf[n] = 0;
        return 1;
    }
    if (HAVE(c) && (c | 32) == 'n') {                                  /* nan, nan(chars) */
        TAKE();
        if (!(HAVE(c) && (c | 32) == 'a')) { GIVEBACK(0); return 0; }
        TAKE();
        if (!(HAVE(c) && (c | 32) == 'n')) { GIVEBACK(0); return 0; }
        TAKE();
        if (HAVE(c) && c == '(') {
            int mark = n;
            TAKE();
            while (HAVE(c) && (isalnum(c) || c == '_')) TAKE();
            if (HAVE(c) && c == ')') TAKE();
            else GIVEBACK(mark);
        }
        rd_unget(r, c);
        buf[n] = 0;
        return 1;
    }
    int hex = 0, digits = 0, zero_mark = -1;
    if (HAVE(c) && c == '0') {
        TAKE();
        digits = 1;
        if (HAVE(c) && (c | 32) == 'x') {
            zero_mark = n;                                              /* where to fall back to if no hex digits follow */
            TAKE();
            if (HAVE(c) && ((digit_value(c) >= 0 && digit_value(c) < 16) || c == '.')) { hex = 1; digits = 0; }
            else { GIVEBACK(zero_mark); buf[n] = 0; return 1; }         /* "0x" without hex digits: the number is the 0 */
        }
    }
    int limit = hex ? 16 : 10;
    while (HAVE(c) && digit_value(c) >= 0 && digit_value(c) < limit) { TAKE(); digits = 1; }
    if (HAVE(c) && c == '.') {
        TAKE();
        while (HAVE(c) && digit_value(c) >= 0 && digit_value(c) < limit) { TAKE(); digits = 1; }
    }
    if (!digits) {
        if (zero_mark >= 0) { GIVEBACK(zero_mark); buf[n] = 0; return 1; }       /* "0x." and nothing after it: just the 0 */
        GIVEBACK(0);
        return 0;
    }
    if (HAVE(c) && (hex ? (c | 32) == 'p' : (c | 32) == 'e')) {        /* the exponent counts only if digits follow */
        int mark = n;
        TAKE();
        if (HAVE(c) && (c == '+' || c == '-')) TAKE();
        if (HAVE(c) && c >= '0' && c <= '9') { while (HAVE(c) && c >= '0' && c <= '9') TAKE(); }
        else GIVEBACK(mark);
    }
    rd_unget(r, c);
    buf[n] = 0;
    return 1;
#undef TAKE
#undef GIVEBACK
}

typedef struct { unsigned char bits[32]; } CharSet;

/* Parses a %[ set starting after the '['; returns the position of the closing ']'. */
static const char *parse_set(const char *p, CharSet *cs, int *negate) {
    memset(cs, 0, sizeof *cs);
    *negate = 0;
    if (*p == '^') { *negate = 1; p++; }
    const char *start = p;
    for (; *p && (*p != ']' || p == start); p++) {
        unsigned lo = (unsigned char)*p;
        if (p[1] == '-' && p[2] && p[2] != ']') {                      /* a range */
            unsigned hi = (unsigned char)p[2];
            for (unsigned x = lo; x <= hi; x++) cs->bits[x >> 3] |= (unsigned char)(1u << (x & 7));
            p += 2;
        } else {
            cs->bits[lo >> 3] |= (unsigned char)(1u << (lo & 7));
        }
    }
    return p;
}

static void skip_space(Rd *r) {
    int c;
    while ((c = rd_get(r)) != EOF && isspace(c)) { }
    rd_unget(r, c);
}

int __vscan(Src *src, const char *fmt, va_list ap) {
    Rd rd = { src, 0 }, *r = &rd;
    int assigned = 0;
#define INPUT_FAIL() return assigned ? assigned : EOF
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) { skip_space(r); continue; }
        if (*fmt != '%') {
            int c = rd_get(r);
            if (c == EOF) INPUT_FAIL();
            if (c != (unsigned char)*fmt) { rd_unget(r, c); return assigned; }
            continue;
        }
        fmt++;
        int suppress = 0, width = 0, lm = 0;                           /* lm: -2 hh, -1 h, 0, 1 l, 2 ll/j/z/t, 3 L */
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); if (width > 1 << 24) width = 1 << 24; }
        for (;; fmt++) {
            if (*fmt == 'l') lm = lm >= 1 ? 2 : 1;
            else if (*fmt == 'h') lm = lm <= -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') lm = 2;
            else if (*fmt == 'L') lm = 3;
            else break;
        }
        char conv = *fmt;
        if (!conv) break;
        if (conv == '%') {
            skip_space(r);
            int c = rd_get(r);
            if (c == EOF) INPUT_FAIL();
            if (c != '%') { rd_unget(r, c); return assigned; }
            continue;
        }
        if (conv == 'n') {
            if (!suppress) {
                void *where = va_arg(ap, void *);
                if (lm >= 1) *(long long *)where = rd.nread;
                else if (lm == -1) *(short *)where = (short)rd.nread;
                else if (lm == -2) *(signed char *)where = (signed char)rd.nread;
                else *(int *)where = (int)rd.nread;
            }
            continue;
        }
        if (conv != 'c' && conv != '[') skip_space(r);

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            unsigned long long mag;
            int neg, over;
            int rc = scan_int(r, base, width, &mag, &neg, &over);
            if (rc < 0) INPUT_FAIL();
            if (rc == 0) return assigned;
            if (suppress) break;
            if (conv == 'p') {
                *va_arg(ap, void **) = (void *)(uintptr_t)(over ? ULLONG_MAX : neg ? 0ULL - mag : mag);
            } else if (conv == 'd' || conv == 'i') {
                long long sv = neg ? (over || mag > (unsigned long long)LLONG_MAX + 1 ? LLONG_MIN : (long long)(0ULL - mag))
                                   : (over || mag > (unsigned long long)LLONG_MAX ? LLONG_MAX : (long long)mag);
                if (lm >= 1) *va_arg(ap, long long *) = sv;
                else if (lm == -1) *va_arg(ap, short *) = (short)sv;
                else if (lm == -2) *va_arg(ap, signed char *) = (signed char)sv;
                else *va_arg(ap, int *) = (int)sv;
            } else {
                unsigned long long uv = over ? ULLONG_MAX : neg ? 0ULL - mag : mag;
                if (lm >= 1) *va_arg(ap, unsigned long long *) = uv;
                else if (lm == -1) *va_arg(ap, unsigned short *) = (unsigned short)uv;
                else if (lm == -2) *va_arg(ap, unsigned char *) = (unsigned char)uv;
                else *va_arg(ap, unsigned *) = (unsigned)uv;
            }
            assigned++;
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            char buf[FBUF + 8];
            int rc = scan_float_text(r, width, buf);
            if (rc < 0) INPUT_FAIL();
            if (rc == 0) return assigned;
            if (suppress) break;
            if (lm == 1) *va_arg(ap, double *) = strtod(buf, NULL);
            else if (lm == 3) *va_arg(ap, long double *) = strtold(buf, NULL);
            else *va_arg(ap, float *) = strtof(buf, NULL);
            assigned++;
            break;
        }
        case 's': {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int w = width > 0 ? width : INT_MAX, n = 0, c = rd_get(r);
            if (c == EOF) INPUT_FAIL();
            while (HAVE(c) && !isspace(c)) {
                if (out) out[n] = (char)c;
                n++;
                c = ADV();
            }
            rd_unget(r, c);
            if (!n) return assigned;
            if (out) { out[n] = 0; assigned++; }
            break;
        }
        case 'c': {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int w = width > 0 ? width : 1;
            for (int i = 0; i < w; i++) {
                int c = rd_get(r);
                if (c == EOF) { if (i == 0) INPUT_FAIL(); return assigned; }
                if (out) out[i] = (char)c;
            }
            if (out) assigned++;
            break;
        }
        case '[': {
            CharSet cs;
            int negate;
            const char *end = parse_set(fmt + 1, &cs, &negate);
            fmt = end;                                                 /* now at the closing ] (or the end of the format) */
            char *out = suppress ? NULL : va_arg(ap, char *);
            int w = width > 0 ? width : INT_MAX, n = 0, c = rd_get(r);
            if (c == EOF) INPUT_FAIL();
            while (HAVE(c)) {
                int in = (cs.bits[(unsigned char)c >> 3] >> ((unsigned char)c & 7)) & 1;
                if (in == negate) break;
                if (out) out[n] = (char)c;
                n++;
                c = ADV();
            }
            rd_unget(r, c);
            if (!n) return assigned;
            if (out) { out[n] = 0; assigned++; }
            if (!*fmt) fmt--;
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
#undef INPUT_FAIL
}

/* ---------------------------------------------------------------- strings */

typedef struct { const char *s; size_t pos; } StrSrc;

static int str_get(void *ctx) {
    StrSrc *s = ctx;
    if (!s->s[s->pos]) return EOF;
    return (unsigned char)s->s[s->pos++];
}

static void str_unget(void *ctx, int c) { StrSrc *s = ctx; (void)c; s->pos--; }

int vsscanf(const char *str, const char *fmt, va_list ap) {
    StrSrc ss = { str, 0 };
    Src src = { str_get, str_unget, &ss };
    return __vscan(&src, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
