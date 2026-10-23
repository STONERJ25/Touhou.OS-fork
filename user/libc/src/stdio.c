/* stdio: buffered FILE streams, a full printf engine (integers, strings, floating point), and scanf. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include "pthread_impl.h"
#include "format.h"

/* One recursive lock serialises all of stdio: threads share the streams, and a printf is never torn by another thread's output. */
RLock __stdio_lock;

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

static int flush_write(FILE *f) {
    size_t off = 0;
    while (off < f->wlen) {
        ssize_t w = write(f->fd, f->buf + off, f->wlen - off);
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
    do r = read(f->fd, f->buf, f->bufsize); while (r < 0 && errno == EINTR);
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
            return write(f->fd, &ch, 1) == 1 ? c : EOF;
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
                ssize_t w = write(f->fd, p + off, total - off);
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
    f->next = g_files;
    g_files = f;
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

static int fclose_nl(FILE *f) {
    int r = fflush(f);
    if (f->fd > 2 || (f != &g_stdin && f != &g_stdout && f != &g_stderr)) {
        if (close(f->fd) < 0) r = EOF;
    }
    for (FILE **pp = &g_files; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->flags & F_OWNBUF) free(f->buf);
    if (f != &g_stdin && f != &g_stdout && f != &g_stderr) free(f);
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

/* ---------------------------------------------------------------- printf */
/* The engine itself is in format.c; this is only the FILE sink. */

static int file_flush(Sink *s) {
    size_t n = fwrite(s->buf, 1, s->pos, s->ctx);
    int bad = n != s->pos;
    s->pos = 0;
    return bad ? -1 : 0;
}

static int file_direct(Sink *s, const char *p, size_t n) { return fwrite(p, 1, n, s->ctx) != n ? -1 : 0; }

static int vfprintf_nl(FILE *f, const char *fmt, va_list ap) {
    char stage[1024];
    Sink s = { 0 };
    s.buf = stage;
    s.cap = sizeof stage;
    s.flush = file_flush;
    s.direct = file_direct;
    s.ctx = f;
    __vformat(&s, fmt, ap);
    if (!s.err && s.pos && file_flush(&s)) s.err = 1;
    if (s.err) return -1;
    return s.count > INT_MAX ? INT_MAX : (int)s.count;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

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
