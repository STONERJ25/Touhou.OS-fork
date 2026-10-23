/* vsnprintf: integers, strings, chars, pointers; flags -+ #0, width, precision, h/hh/l/ll/z/j/t modifiers. */
#include "printk.h"
#include "string.h"

#define F_LEFT  1
#define F_ZERO  2
#define F_PLUS  4
#define F_SPACE 8
#define F_ALT   16

typedef struct {
    char  *buf;
    size_t size;
    size_t pos;
} Out;

static inline void put(Out *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void pad(Out *o, char c, int n) {
    while (n-- > 0) put(o, c);
}

static void put_num(Out *o, u64 v, int base, bool upper, bool neg, int width, int prec, unsigned flags, bool force_prefix) {
    char tmp[66];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    bool nonzero = v != 0;

    if (v == 0) {
        if (prec != 0) tmp[n++] = '0';
    } else {
        while (v) { tmp[n++] = dig[v % (u64)base]; v /= (u64)base; }
    }
    if (base == 8 && (flags & F_ALT) && (n == 0 || tmp[n - 1] != '0')) tmp[n++] = '0';

    char prefix[3];
    int plen = 0;
    if (neg) prefix[plen++] = '-';
    else if (flags & F_PLUS) prefix[plen++] = '+';
    else if (flags & F_SPACE) prefix[plen++] = ' ';
    if (base == 16 && (force_prefix || ((flags & F_ALT) && nonzero))) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }

    int zeros = prec > n ? prec - n : 0;
    int total = plen + zeros + n;
    if ((flags & F_ZERO) && !(flags & F_LEFT) && prec < 0 && width > total) zeros += width - total;
    total = plen + zeros + n;

    if (!(flags & F_LEFT)) pad(o, ' ', width - total);
    for (int i = 0; i < plen; i++) put(o, prefix[i]);
    pad(o, '0', zeros);
    while (n) put(o, tmp[--n]);
    if (flags & F_LEFT) pad(o, ' ', width - total);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    Out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;

        unsigned flags = 0;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '0') flags |= F_ZERO;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        int lmod = 0;   /* 0 int, 1 long (also z/j/t), 2 long long, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lmod = lmod >= 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') lmod = 2;
            else if (*fmt == 'h') lmod = lmod <= -1 ? -2 : -1;
            else break;
        }

        char c = *fmt;
        switch (c) {
        case 'd': case 'i': {
            s64 v;
            if (lmod >= 1) v = va_arg(ap, s64);
            else if (lmod == -1) v = (s16)va_arg(ap, int);
            else if (lmod == -2) v = (s8)va_arg(ap, int);
            else v = va_arg(ap, int);
            bool neg = v < 0;
            put_num(&o, neg ? (u64)0 - (u64)v : (u64)v, 10, false, neg, width, prec, flags, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            u64 v;
            if (lmod >= 1) v = va_arg(ap, u64);
            else if (lmod == -1) v = (u16)va_arg(ap, unsigned);
            else if (lmod == -2) v = (u8)va_arg(ap, unsigned);
            else v = va_arg(ap, unsigned);
            int base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            put_num(&o, v, base, c == 'X', false, width, prec, flags, false);
            break;
        }
        case 'p': {
            u64 v = (u64)(uintptr_t)va_arg(ap, void *);
            put_num(&o, v, 16, false, false, width, prec, flags, true);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!(flags & F_LEFT)) pad(&o, ' ', width - 1);
            put(&o, ch);
            if (flags & F_LEFT) pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
            if (!(flags & F_LEFT)) pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (flags & F_LEFT) pad(&o, ' ', width - len);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt--;      /* dangling '%' at the end of the string */
            break;
        default:
            put(&o, '%');
            put(&o, c);
            break;
        }
    }

    if (size) o.buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
