/* stdio: buffered FILE streams, a full printf engine (integers, strings, floating point), and scanf. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include "pthread_impl.h"

int signbit_d(double x);

/* Every stream has its own recursive lock (a printf is never torn by another thread's output, and a thread blocked reading
 * stdin does not stop the others from printing). __stdio_list_lock guards the list of open streams; the lock order is always
 * list first, then a stream, and fclose never holds a stream lock while it takes the list lock. */
RLock __stdio_list_lock;

#define F_READ  1
#define F_WRITE 2
#define F_EOF   4
#define F_ERR   8
#define F_OWNBUF 16
#define F_APPEND 32

struct FILE {
    int fd;
    int flags;
    int bufmode;                 /* _IOFBF / _IOLBF / _IONBF */
    unsigned char *buf;
    size_t bufsize;
    size_t rpos, rlen;           /* read buffer: consumed / filled */
    size_t wlen;                 /* pending write bytes */
    int ungot;                   /* pushed-back char or -1 */
    struct FILE *next;
    RLock lock;                  /* held by every operation on this stream */
};

static unsigned char g_inbuf[BUFSIZ], g_outbuf[BUFSIZ];
static FILE g_stdin = { 0, F_READ, _IOFBF, g_inbuf, BUFSIZ, 0, 0, 0, -1, NULL };
static FILE g_stdout = { 1, F_WRITE, _IOLBF, g_outbuf, BUFSIZ, 0, 0, 0, -1, NULL };
static FILE g_stderr = { 2, F_WRITE, _IONBF, NULL, 0, 0, 0, 0, -1, NULL };
FILE *stdin = &g_stdin, *stdout = &g_stdout, *stderr = &g_stderr;
static FILE *g_files;

void __stdio_init(void) {
    g_stdout.bufmode = isatty(1) ? _IOLBF : _IOFBF;
    g_stdin.bufmode = _IOFBF;
}

/* The two places stdio blocks in the kernel. A thread cancelled there must let go of the stream's lock on its way out
 * (pthread.c: __cancel_exit looks at stdio_blocked). */
static ssize_t io_write(FILE *f, const void *p, size_t n) {
    struct pthread *self = __pthread_self();
    self->stdio_blocked = &f->lock;
    ssize_t r = write(f->fd, p, n);
    self->stdio_blocked = NULL;
    return r;
}

static ssize_t io_read(FILE *f, void *p, size_t n) {
    struct pthread *self = __pthread_self();
    self->stdio_blocked = &f->lock;
    ssize_t r = read(f->fd, p, n);
    self->stdio_blocked = NULL;
    return r;
}

static int flush_write(FILE *f) {
    size_t off = 0;
    while (off < f->wlen) {
        ssize_t w = io_write(f, f->buf + off, f->wlen - off);
        if (w < 0) { if (errno == EINTR) continue; f->flags |= F_ERR; f->wlen = 0; return EOF; }
        off += (size_t)w;
    }
    f->wlen = 0;
    return 0;
}

static int fflush_nl(FILE *f) {
    if (!f) {
        int r = 0;
        if (flush_write(&g_stdout)) r = EOF;
        for (FILE *p = g_files; p; p = p->next) if ((p->flags & F_WRITE) && flush_write(p)) r = EOF;
        return r;
    }
    if (f->flags & F_WRITE) return flush_write(f);
    return 0;
}

static int fill(FILE *f) {
    if (f->flags & F_EOF) return EOF;
    if (f == &g_stdin) fflush(&g_stdout);
    if (!f->buf) {
        f->buf = malloc(BUFSIZ);
        f->bufsize = f->buf ? BUFSIZ : 0;
        f->flags |= F_OWNBUF;
        if (!f->buf) return EOF;
    }
    ssize_t r;
    do r = io_read(f, f->buf, f->bufsize); while (r < 0 && errno == EINTR);
    if (r < 0) { f->flags |= F_ERR; return EOF; }
    if (r == 0) { f->flags |= F_EOF; return EOF; }
    f->rpos = 0;
    f->rlen = (size_t)r;
    return 0;
}

static int fgetc_nl(FILE *f) {
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->wlen) flush_write(f);
    if (f->rpos >= f->rlen && fill(f)) return EOF;
    return f->buf[f->rpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

static int ungetc_nl(int c, FILE *f) {
    if (c == EOF) return EOF;
    f->ungot = c;
    f->flags &= ~F_EOF;
    return c;
}

static int fputc_nl(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    if (f->bufmode == _IONBF || !f->buf) {
        if (!f->buf && f->bufmode != _IONBF) {
            f->buf = malloc(BUFSIZ);
            if (f->buf) { f->bufsize = BUFSIZ; f->flags |= F_OWNBUF; }
        }
        if (!f->buf) {
            return io_write(f, &ch, 1) == 1 ? c : EOF;
        }
    }
    if (f->rlen) { f->rpos = f->rlen = 0; }
    f->buf[f->wlen++] = ch;
    if (f->wlen >= f->bufsize || (f->bufmode == _IOLBF && ch == '\n')) {
        if (flush_write(f)) return EOF;
    }
    return c;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

static size_t fwrite_nl(const void *buf, size_t size, size_t n, FILE *f) {
    size_t total = size * n;
    const unsigned char *p = buf;
    if (!total) return 0;
    if (f->bufmode == _IONBF || !f->buf) {
        if (!f->buf && f->bufmode != _IONBF) {
            f->buf = malloc(BUFSIZ);
            if (f->buf) { f->bufsize = BUFSIZ; f->flags |= F_OWNBUF; }
        }
        if (!f->buf) {
            size_t off = 0;
            while (off < total) {
                ssize_t w = io_write(f, p + off, total - off);
                if (w <= 0) { f->flags |= F_ERR; break; }
                off += (size_t)w;
            }
            return off / size;
        }
    }
    for (size_t i = 0; i < total; i++) {
        if (fputc(p[i], f) == EOF) return i / size;
    }
    return n;
}

static size_t fread_nl(void *buf, size_t size, size_t n, FILE *f) {
    size_t total = size * n, got = 0;
    unsigned char *p = buf;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF) break;
        p[got++] = (unsigned char)c;
        /* fast path: drain the buffer */
        size_t avail = f->rlen - f->rpos;
        if (avail && got < total) {
            size_t take = avail < total - got ? avail : total - got;
            memcpy(p + got, f->buf + f->rpos, take);
            f->rpos += take;
            got += take;
        }
    }
    return size ? got / size : 0;
}

static char *fgets_nl(char *buf, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = 0;
    return buf;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

static ssize_t getdelim_nl(char **line, size_t *cap, int delim, FILE *f) {
    size_t n = 0;
    if (!*line || !*cap) { *cap = 128; *line = malloc(*cap); if (!*line) return -1; }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (n + 2 > *cap) {
            *cap *= 2;
            char *nl = realloc(*line, *cap);
            if (!nl) return -1;
            *line = nl;
        }
        (*line)[n++] = (char)c;
        if (c == delim) break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}

ssize_t getline(char **line, size_t *cap, FILE *f) { return getdelim(line, cap, '\n', f); }

static FILE *fdopen_nl(int fd, const char *mode) {
    FILE *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fd = fd;
    f->ungot = -1;
    f->bufmode = _IOFBF;
    int plus = strchr(mode, '+') != NULL;
    if (mode[0] == 'r') f->flags = F_READ | (plus ? F_WRITE : 0);
    else f->flags = F_WRITE | (plus ? F_READ : 0);
    if (mode[0] == 'a') f->flags |= F_APPEND;
    __rlock(&__stdio_list_lock);
    f->next = g_files;
    g_files = f;
    __runlock(&__stdio_list_lock);
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;
    int plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return NULL;
    }
    int fd = open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    fclose(f);
    return fopen(path, mode);
}

/* The stream lock is released before the stream leaves the list and is freed (list lock, then nothing: see the lock order). */
int fclose(FILE *f) {
    __rlock(&f->lock);
    int r = fflush_nl(f);
    int is_std = f == &g_stdin || f == &g_stdout || f == &g_stderr;
    if (f->fd > 2 || !is_std) {
        if (close(f->fd) < 0) r = EOF;
    }
    if (f->flags & F_OWNBUF) { free(f->buf); f->buf = NULL; f->flags &= ~F_OWNBUF; }
    __runlock(&f->lock);
    if (!is_std) {
        __rlock(&__stdio_list_lock);
        for (FILE **pp = &g_files; *pp; pp = &(*pp)->next)
            if (*pp == f) { *pp = f->next; break; }
        __runlock(&__stdio_list_lock);
        free(f);
    }
    return r;
}

static int fseek_nl(FILE *f, long off, int whence) {
    fflush(f);
    if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->flags &= ~F_EOF;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

static long ftell_nl(FILE *f) {
    long pos = (long)lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return pos - (long)(f->rlen - f->rpos) - (f->ungot >= 0 ? 1 : 0) + (long)f->wlen;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    f->bufmode = mode;
    if (buf) { f->buf = (unsigned char *)buf; f->bufsize = size; f->flags &= ~F_OWNBUF; }
    return 0;
}

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errno));
}

char *tmpnam(char *buf) {
    static char sbuf[32];
    static int n;
    char *b = buf ? buf : sbuf;
    snprintf(b, 32, "/tmp/tmp%d_%d", getpid(), n++);
    return b;
}

/* ---------------------------------------------------------------- printf engine */

typedef struct {
    void (*put)(void *ctx, const char *s, size_t n);
    void *ctx;
    size_t count;
} Sink;

static void emit(Sink *s, const char *p, size_t n) {
    if (n) s->put(s->ctx, p, n);
    s->count += n;
}

static void pad(Sink *s, char c, int n) {
    char buf[32];
    memset(buf, c, sizeof buf);
    while (n > 0) {
        int k = n < (int)sizeof buf ? n : (int)sizeof buf;
        emit(s, buf, (size_t)k);
        n -= k;
    }
}

#define FL_LEFT 1
#define FL_ZERO 2
#define FL_PLUS 4
#define FL_SPACE 8
#define FL_ALT 16

static void fmt_int(Sink *s, unsigned long long v, int base, int upper, int neg, int width, int prec, unsigned flags,
                    int force_prefix) {
    char tmp[72];
    int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int nonzero = v != 0;
    if (v == 0) { if (prec != 0) tmp[n++] = '0'; }
    else while (v) { tmp[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
    if (base == 8 && (flags & FL_ALT) && (n == 0 || tmp[n - 1] != '0')) tmp[n++] = '0';
    char prefix[3];
    int plen = 0;
    if (neg) prefix[plen++] = '-';
    else if (flags & FL_PLUS) prefix[plen++] = '+';
    else if (flags & FL_SPACE) prefix[plen++] = ' ';
    if (base == 16 && (force_prefix || ((flags & FL_ALT) && nonzero))) { prefix[plen++] = '0'; prefix[plen++] = upper ? 'X' : 'x'; }
    int zeros = prec > n ? prec - n : 0;
    int total = plen + zeros + n;
    if ((flags & FL_ZERO) && !(flags & FL_LEFT) && prec < 0 && width > total) zeros += width - total;
    total = plen + zeros + n;
    if (!(flags & FL_LEFT)) pad(s, ' ', width - total);
    emit(s, prefix, (size_t)plen);
    pad(s, '0', zeros);
    for (int i = n - 1; i >= 0; i--) emit(s, &tmp[i], 1);
    if (flags & FL_LEFT) pad(s, ' ', width - total);
}

static double pow10i(int e) {
    double r = 1.0, b = 10.0;
    int neg = e < 0;
    if (neg) e = -e;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return neg ? 1.0 / r : r;
}

/* Formats a non-negative finite double. conv is 'f', 'e' or 'g'. Returns length written into out (size >= 400). */
static int fmt_double(char *out, double v, char conv, int prec, int alt, int upper) {
    int n = 0;
    if (prec < 0) prec = 6;
    if (conv == 'g' && prec == 0) prec = 1;

    int exp10 = 0;
    if (v != 0) {
        exp10 = (int)floor(log10(v));
        if (pow10i(exp10) > v) exp10--;
        if (pow10i(exp10 + 1) <= v) exp10++;
    }
    char c = conv;
    if (conv == 'g') {
        int p = prec;
        if (exp10 < -4 || exp10 >= p) { c = 'e'; prec = p - 1; }
        else { c = 'f'; prec = p - 1 - exp10; if (prec < 0) prec = 0; }
    }

    if (c == 'e') {
        double m = v == 0 ? 0 : v / pow10i(exp10);
        double round = 0.5 / pow10i(prec);
        m += round;
        if (m >= 10.0) { m /= 10.0; exp10++; }
        int d = (int)m;
        out[n++] = (char)('0' + d);
        if (prec > 0 || alt) out[n++] = '.';
        m -= d;
        for (int i = 0; i < prec; i++) { m *= 10; int dd = (int)m; out[n++] = (char)('0' + dd); m -= dd; }
        out[n++] = upper ? 'E' : 'e';
        out[n++] = exp10 < 0 ? '-' : '+';
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e >= 100) out[n++] = (char)('0' + e / 100);
        out[n++] = (char)('0' + e / 10 % 10);
        out[n++] = (char)('0' + e % 10);
    } else {
        v += 0.5 / pow10i(prec);
        if (v >= 1.8e19) {                      /* too large for 64-bit integer part: print scaled digits */
            int e = (int)floor(log10(v));
            double m = v / pow10i(e);
            for (int i = 0; i <= e; i++) { int d = (int)m; out[n++] = (char)('0' + d % 10); m = (m - d) * 10; }
        } else {
            unsigned long long ip = (unsigned long long)v;
            double fp = v - (double)ip;
            char tmp[24];
            int t = 0;
            if (!ip) tmp[t++] = '0';
            while (ip) { tmp[t++] = (char)('0' + ip % 10); ip /= 10; }
            while (t) out[n++] = tmp[--t];
            if (prec > 0 || alt) out[n++] = '.';
            for (int i = 0; i < prec; i++) { fp *= 10; int d = (int)fp; out[n++] = (char)('0' + d); fp -= d; }
        }
    }
    if (conv == 'g' && !alt) {                  /* strip trailing zeros of the fraction */
        char *dot = memchr(out, '.', (size_t)n);
        if (dot) {
            char *e = memchr(out, upper ? 'E' : 'e', (size_t)n);
            int end = e ? (int)(e - out) : n;
            int k = end;
            while (k > 0 && out[k - 1] == '0') k--;
            if (k > 0 && out[k - 1] == '.') k--;
            memmove(out + k, out + end, (size_t)(n - end));
            n -= end - k;
        }
    }
    out[n] = 0;
    return n;
}

static int vformat(Sink *s, const char *fmt, va_list ap) {
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
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { flags |= FL_LEFT; width = -width; } fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lm = 0;                              /* 0 int, 1 long, 2 long long, -1 short, -2 char */
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
            fmt_int(s, neg ? 0ULL - (unsigned long long)v : (unsigned long long)v, 10, 0, neg, width, prec, flags, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v;
            if (lm >= 1) v = va_arg(ap, unsigned long long);
            else if (lm == -1) v = (unsigned short)va_arg(ap, unsigned);
            else if (lm == -2) v = (unsigned char)va_arg(ap, unsigned);
            else v = va_arg(ap, unsigned);
            fmt_int(s, v, c == 'u' ? 10 : c == 'o' ? 8 : 16, c == 'X', 0, width, prec, flags, 0);
            break;
        }
        case 'p':
            fmt_int(s, (unsigned long long)(size_t)va_arg(ap, void *), 16, 0, 0, width, prec, flags, 1);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!(flags & FL_LEFT)) pad(s, ' ', width - 1);
            emit(s, &ch, 1);
            if (flags & FL_LEFT) pad(s, ' ', width - 1);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int len = prec >= 0 ? (int)strnlen(str, (size_t)prec) : (int)strlen(str);
            if (!(flags & FL_LEFT)) pad(s, ' ', width - len);
            emit(s, str, (size_t)len);
            if (flags & FL_LEFT) pad(s, ' ', width - len);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = lm == 3 ? (double)va_arg(ap, long double) : va_arg(ap, double);
            int neg = signbit_d(v);
            if (neg) v = -v;
            char body[420];
            int n;
            if (v != v) { strcpy(body, c < 'a' ? "NAN" : "nan"); n = 3; flags &= ~FL_ZERO; neg = 0; }
            else if (v == INFINITY) { strcpy(body, c < 'a' ? "INF" : "inf"); n = 3; flags &= ~FL_ZERO; }
            else n = fmt_double(body, v, (char)tolower(c), prec, (flags & FL_ALT) != 0, c < 'a');
            char sign = neg ? '-' : (flags & FL_PLUS) ? '+' : (flags & FL_SPACE) ? ' ' : 0;
            int total = n + (sign ? 1 : 0);
            if (!(flags & FL_LEFT) && !(flags & FL_ZERO)) pad(s, ' ', width - total);
            if (sign) emit(s, &sign, 1);
            if (!(flags & FL_LEFT) && (flags & FL_ZERO)) pad(s, '0', width - total);
            emit(s, body, (size_t)n);
            if (flags & FL_LEFT) pad(s, ' ', width - total);
            break;
        }
        case 'n':
            *va_arg(ap, int *) = (int)s->count;
            break;
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
    return (int)s->count;
}

static void put_file(void *ctx, const char *p, size_t n) { fwrite(p, 1, n, ctx); }

typedef struct { char *buf; size_t size, pos; } StrCtx;

static void put_str(void *ctx, const char *p, size_t n) {
    StrCtx *c = ctx;
    for (size_t i = 0; i < n; i++) {
        if (c->pos + 1 < c->size) c->buf[c->pos] = p[i];
        c->pos++;
    }
}

static int vfprintf_nl(FILE *f, const char *fmt, va_list ap) {
    Sink s = { put_file, f, 0 };
    return vformat(&s, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    StrCtx c = { buf, n, 0 };
    Sink s = { put_str, &c, 0 };
    int r = vformat(&s, fmt, ap);
    if (n) buf[c.pos < n ? c.pos : n - 1] = 0;
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)INT_MAX, fmt, ap); }

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

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

int dprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (r > (int)sizeof buf - 1) r = (int)sizeof buf - 1;
    return (int)write(fd, buf, (size_t)r);
}

int asprintf(char **out, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    *out = malloc((size_t)n + 1);
    if (!*out) { va_end(ap2); return -1; }
    vsnprintf(*out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return n;
}

/* ---------------------------------------------------------------- scanf */

typedef struct {
    int (*get)(void *ctx);
    void (*unget)(void *ctx, int c);
    void *ctx;
} Src;

static int scan_core(Src *src, const char *fmt, va_list ap) {
    int assigned = 0, nread = 0;
#define GET() (nread++, src->get(src->ctx))
#define UNGET(c) (nread--, src->unget(src->ctx, (c)))
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            int c;
            while ((c = GET()) != EOF && isspace(c)) { }
            if (c != EOF) UNGET(c); else nread--;
            continue;
        }
        if (*fmt != '%') {
            int c = GET();
            if (c != *fmt) { if (c != EOF) UNGET(c); else nread--; return assigned ? assigned : (c == EOF ? EOF : 0); }
            continue;
        }
        fmt++;
        int suppress = 0, width = 0, lm = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        for (;; fmt++) {
            if (*fmt == 'l') lm++;
            else if (*fmt == 'h') lm--;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lm = 2;
            else break;
        }
        char conv = *fmt;
        if (conv == '%') { int c = GET(); if (c != '%') return assigned; continue; }
        if (conv == 'n') { if (!suppress) *va_arg(ap, int *) = nread; continue; }
        if (conv != 'c' && conv != '[') {
            int c;
            while ((c = GET()) != EOF && isspace(c)) { }
            if (c == EOF) { nread--; return assigned ? assigned : EOF; }
            UNGET(c);
        }
        if (!width) width = conv == 'c' ? 1 : 1 << 20;
        char buf[512];
        int n = 0;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            int c = GET();
            if ((c == '-' || c == '+') && n < width && n < 500) { buf[n++] = (char)c; c = GET(); }
            while (c != EOF && n < width && n < 500 && (base == 16 ? isxdigit(c) || ((c == 'x' || c == 'X') && n <= 2) :
                   base == 0 ? isxdigit(c) || c == 'x' || c == 'X' : isdigit(c))) { buf[n++] = (char)c; c = GET(); }
            if (c != EOF) UNGET(c); else nread--;
            buf[n] = 0;
            if (!n || (n == 1 && (buf[0] == '-' || buf[0] == '+'))) return assigned;
            if (!suppress) {
                long long v = strtoll(buf, NULL, base);
                if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    unsigned long long u = strtoull(buf, NULL, base);
                    if (lm >= 1) *va_arg(ap, unsigned long *) = (unsigned long)u;
                    else if (lm == -1) *va_arg(ap, unsigned short *) = (unsigned short)u;
                    else if (lm == -2) *va_arg(ap, unsigned char *) = (unsigned char)u;
                    else *va_arg(ap, unsigned *) = (unsigned)u;
                } else {
                    if (lm >= 1) *va_arg(ap, long *) = (long)v;
                    else if (lm == -1) *va_arg(ap, short *) = (short)v;
                    else if (lm == -2) *va_arg(ap, signed char *) = (signed char)v;
                    else *va_arg(ap, int *) = (int)v;
                }
                assigned++;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            int c = GET();
            while (c != EOF && n < width && n < 500 && (isdigit(c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) { buf[n++] = (char)c; c = GET(); }
            if (c != EOF) UNGET(c); else nread--;
            buf[n] = 0;
            if (!n) return assigned;
            if (!suppress) {
                double d = strtod(buf, NULL);
                if (lm >= 1) *va_arg(ap, double *) = d; else *va_arg(ap, float *) = (float)d;
                assigned++;
            }
        } else if (conv == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int c = GET();
            while (c != EOF && !isspace(c) && n < width) { if (out) out[n] = (char)c; n++; c = GET(); }
            if (c != EOF) UNGET(c); else nread--;
            if (!n) return assigned;
            if (out) { out[n] = 0; assigned++; }
        } else if (conv == 'c') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < width; i++) {
                int c = GET();
                if (c == EOF) { nread--; return assigned ? assigned : EOF; }
                if (out) out[i] = (char)c;
            }
            if (out) assigned++;
        } else if (conv == '[') {
            fmt++;
            int negate = 0;
            if (*fmt == '^') { negate = 1; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            size_t setlen = (size_t)(fmt - set);
            char *out = suppress ? NULL : va_arg(ap, char *);
            int c = GET();
            while (c != EOF && n < width) {
                int in = 0;
                for (size_t i = 0; i < setlen; i++) {
                    if (i + 2 < setlen && set[i + 1] == '-') { if (c >= set[i] && c <= set[i + 2]) in = 1; i += 2; }
                    else if (set[i] == c) in = 1;
                }
                if (in == negate) break;
                if (out) out[n] = (char)c;
                n++;
                c = GET();
            }
            if (c != EOF) UNGET(c); else nread--;
            if (!n) return assigned;
            if (out) { out[n] = 0; assigned++; }
        } else {
            return assigned;
        }
    }
    return assigned;
#undef GET
#undef UNGET
}

typedef struct { const char *s; size_t pos; } StrSrc;
static int str_get(void *ctx) { StrSrc *s = ctx; return s->s[s->pos] ? (unsigned char)s->s[s->pos++] : (s->pos++, EOF); }
static void str_unget(void *ctx, int c) { StrSrc *s = ctx; (void)c; s->pos--; }
static int file_get(void *ctx) { return fgetc(ctx); }
static void file_unget(void *ctx, int c) { ungetc(c, ctx); }

int vsscanf(const char *str, const char *fmt, va_list ap) {
    StrSrc ss = { str, 0 };
    Src src = { str_get, str_unget, &ss };
    return scan_core(&src, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    Src src = { file_get, file_unget, f };
    va_list ap;
    va_start(ap, fmt);
    __rlock(&__stdio_lock);
    int r = scan_core(&src, fmt, ap);
    __runlock(&__stdio_lock);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    Src src = { file_get, file_unget, stdin };
    va_list ap;
    va_start(ap, fmt);
    __rlock(&__stdio_lock);
    int r = scan_core(&src, fmt, ap);
    __runlock(&__stdio_lock);
    va_end(ap);
    return r;
}

/* ---------------------------------------------------------------- thread-safe entry points */

#define STDIO_LOCKED(rettype, name, params, args)      \
    rettype name params {                              \
        __rlock(&__stdio_lock);                        \
        rettype r = name##_nl args;                    \
        __runlock(&__stdio_lock);                      \
        return r;                                      \
    }

STDIO_LOCKED(int, fflush, (FILE *f), (f))
STDIO_LOCKED(int, fgetc, (FILE *f), (f))
STDIO_LOCKED(int, ungetc, (int c, FILE *f), (c, f))
STDIO_LOCKED(int, fputc, (int c, FILE *f), (c, f))
STDIO_LOCKED(size_t, fwrite, (const void *buf, size_t size, size_t n, FILE *f), (buf, size, n, f))
STDIO_LOCKED(size_t, fread, (void *buf, size_t size, size_t n, FILE *f), (buf, size, n, f))
STDIO_LOCKED(char *, fgets, (char *buf, int n, FILE *f), (buf, n, f))
STDIO_LOCKED(ssize_t, getdelim, (char **line, size_t *cap, int delim, FILE *f), (line, cap, delim, f))
STDIO_LOCKED(FILE *, fdopen, (int fd, const char *mode), (fd, mode))
STDIO_LOCKED(int, fclose, (FILE *f), (f))
STDIO_LOCKED(int, fseek, (FILE *f, long off, int whence), (f, off, whence))
STDIO_LOCKED(long, ftell, (FILE *f), (f))
STDIO_LOCKED(int, vfprintf, (FILE *f, const char *fmt, va_list ap), (f, fmt, ap))
