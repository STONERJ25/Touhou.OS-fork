/* stdio: buffered FILE streams. The printf engine is in format.c and the scanf engine in scanf.c; this file holds the streams
 * and the glue (vfprintf, fscanf).
 *
 * Every stream has its own recursive lock (a printf is never torn by another thread's output, and a thread blocked reading
 * stdin does not stop the others from printing); __stdio_list_lock guards the list of open streams. The lock order is always
 * list first, then a stream, and fclose never holds a stream lock while it takes the list lock (see docs/THREADS.md). Every
 * public entry point takes the stream lock once and then works on the buffer with the *_nl functions, which never lock. A
 * process that never created a thread skips the locks (__libc_threaded); fork() takes them regardless (see sys.c).
 * Data moves in blocks: fwrite/fputs/fprintf copy into the buffer with memcpy (and write big blocks straight through when the
 * buffer is empty), fread reads big blocks straight into the caller's memory, fgets/getline find the end of the line with
 * memchr in the buffered data. Buffering: stdout is line buffered on a terminal and fully buffered otherwise, stderr is
 * unbuffered, everything else is fully buffered. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include "pthread_impl.h"
#include "format.h"
#include "scan.h"

RLock __stdio_list_lock;                        /* guards g_files (threaded programs only, and around fork) */

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
    size_t wlen;                 /* pending write bytes (the write buffer is the same memory; only one of the two is in use) */
    int ungot;                   /* pushed-back char (when the buffer has no room for it) or -1 */
    struct FILE *next;
    RLock lock;                  /* held by every operation on this stream (threaded programs only) */
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

#define IS_STD(f) ((f) == &g_stdin || (f) == &g_stdout || (f) == &g_stderr)

/* Takes a stream's lock when the process has threads; returns whether it did (hand that to unlock_stream). */
static inline int lock_stream(FILE *f) {
    int lk = __libc_threaded;
    if (lk) __rlock(&f->lock);
    return lk;
}

static inline void unlock_stream(FILE *f, int lk) { if (lk) __runlock(&f->lock); }

static inline int lock_list(void) {
    int lk = __libc_threaded;
    if (lk) __rlock(&__stdio_list_lock);
    return lk;
}

static inline void unlock_list(int lk) { if (lk) __runlock(&__stdio_list_lock); }

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

/* ---------------------------------------------------------------- the buffer */

/* Writes len bytes to the descriptor; returns how many went out (less than len only after an error). */
static size_t write_all(FILE *f, const char *p, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = io_write(f, p + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { f->flags |= F_ERR; break; }
        off += (size_t)w;
    }
    return off;
}

static int flush_write(FILE *f) {
    if (!f->wlen) return 0;
    size_t n = write_all(f, (const char *)f->buf, f->wlen);
    int bad = n < f->wlen;
    f->wlen = 0;
    return bad ? EOF : 0;
}

static int alloc_buf(FILE *f) {
    if (f->buf && f->bufsize) return 0;
    f->buf = malloc(BUFSIZ);
    if (!f->buf) { f->bufsize = 0; return -1; }
    f->bufsize = BUFSIZ;
    f->flags |= F_OWNBUF;
    return 0;
}

/* Gives the file position back what the read buffer ran ahead (before a write or a seek on a stream with unread data). */
static void drop_read_buffer(FILE *f, int reposition) {
    size_t unread = (f->rlen - f->rpos) + (f->ungot >= 0);
    if (unread && reposition) lseek(f->fd, -(off_t)unread, SEEK_CUR);
    f->rpos = f->rlen = 0;
    f->ungot = -1;
}

static int fflush_nl(FILE *f) {                                         /* one stream; fflush(NULL) is below */
    if (f->flags & F_WRITE) return flush_write(f);
    return 0;
}

static int fill(FILE *f) {
    if (f->flags & F_EOF) return EOF;
    if (f == &g_stdin && g_stdout.wlen) fflush(&g_stdout);             /* a prompt written without a newline must appear first */
    if (alloc_buf(f)) return EOF;
    ssize_t r;
    do r = io_read(f, f->buf, f->bufsize); while (r < 0 && errno == EINTR);
    f->rpos = f->rlen = 0;
    if (r < 0) { f->flags |= F_ERR; return EOF; }
    if (r == 0) { f->flags |= F_EOF; return EOF; }
    f->rlen = (size_t)r;
    return 0;
}

/* ---------------------------------------------------------------- reading */

static int fgetc_nl(FILE *f) {
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->rpos < f->rlen) return f->buf[f->rpos++];
    if (f->wlen) flush_write(f);
    if (fill(f)) return EOF;
    return f->buf[f->rpos++];
}

static int ungetc_nl(int c, FILE *f) {
    if (c == EOF) return EOF;
    if (f->rpos > 0) f->buf[--f->rpos] = (unsigned char)c;           /* the byte just read is still in the buffer */
    else if (f->ungot < 0) f->ungot = c;
    else return EOF;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

static size_t fread_nl(void *vp, size_t size, size_t n, FILE *f) {
    if (!size || !n) return 0;
    size_t total = n > SIZE_MAX / size ? SIZE_MAX / size * size : size * n;
    unsigned char *p = vp;
    size_t got = 0;
    if (f->wlen) flush_write(f);
    if (f->ungot >= 0) { p[got++] = (unsigned char)f->ungot; f->ungot = -1; }
    while (got < total) {
        size_t avail = f->rlen - f->rpos;
        if (avail) {
            size_t k = avail < total - got ? avail : total - got;
            memcpy(p + got, f->buf + f->rpos, k);
            f->rpos += k;
            got += k;
            continue;
        }
        if (f->flags & F_EOF) break;
        size_t want = total - got;
        if (want >= BUFSIZ) {                                           /* a big block: read it straight into the caller's memory */
            if (f == &g_stdin && g_stdout.wlen) fflush(&g_stdout);
            ssize_t r;
            do r = io_read(f, p + got, want); while (r < 0 && errno == EINTR);
            if (r < 0) { f->flags |= F_ERR; break; }
            if (r == 0) { f->flags |= F_EOF; break; }
            got += (size_t)r;
        } else if (fill(f)) {
            break;
        }
    }
    return got / size;
}

static char *fgets_nl(char *buf, int n, FILE *f) {
    if (n <= 0) return NULL;
    int i = 0;
    if (n == 1) { buf[0] = 0; return buf; }
    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        buf[i++] = (char)c;
        if (c == '\n') { buf[i] = 0; return buf; }
    }
    while (i < n - 1) {
        if (f->rpos >= f->rlen) {
            if (f->wlen) flush_write(f);
            if (fill(f)) break;
        }
        size_t avail = f->rlen - f->rpos, room = (size_t)(n - 1 - i);
        unsigned char *start = f->buf + f->rpos;
        size_t take = avail < room ? avail : room;
        unsigned char *nl = memchr(start, '\n', take);
        if (nl) take = (size_t)(nl - start) + 1;
        memcpy(buf + i, start, take);
        i += (int)take;
        f->rpos += take;
        if (nl) break;
    }
    if (i == 0) return NULL;
    buf[i] = 0;
    return buf;
}

static ssize_t getdelim_nl(char **line, size_t *cap, int delim, FILE *f) {
    size_t n = 0;
    if (!line || !cap) { errno = EINVAL; return -1; }
    if (!*line || !*cap) {
        *cap = 128;
        *line = malloc(*cap);
        if (!*line) { *cap = 0; return -1; }
    }
    int done = 0;
    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        (*line)[n++] = (char)c;
        done = c == delim;
    }
    while (!done) {
        if (f->rpos >= f->rlen) {
            if (f->wlen) flush_write(f);
            if (fill(f)) break;
        }
        size_t avail = f->rlen - f->rpos;
        unsigned char *start = f->buf + f->rpos;
        unsigned char *hit = memchr(start, delim, avail);
        size_t take = hit ? (size_t)(hit - start) + 1 : avail;
        if (n + take + 1 > *cap) {
            size_t ncap = *cap * 2;
            if (ncap < n + take + 1) ncap = n + take + 1;
            char *nl = realloc(*line, ncap);
            if (!nl) return -1;
            *line = nl;
            *cap = ncap;
        }
        memcpy(*line + n, start, take);
        n += take;
        f->rpos += take;
        done = hit != NULL;
    }
    if (!n) return -1;
    if (n + 1 > *cap) {
        char *nl = realloc(*line, n + 1);
        if (!nl) return -1;
        *line = nl;
        *cap = n + 1;
    }
    (*line)[n] = 0;
    return (ssize_t)n;
}

/* ---------------------------------------------------------------- writing */

/* Prepares a stream for output: a read buffer that ran ahead of the file position is given back. */
static int wprep(FILE *f) {
    if (!(f->flags & F_WRITE)) { f->flags |= F_ERR; errno = EBADF; return -1; }
    if (f->rlen || f->ungot >= 0) drop_read_buffer(f, !(f->flags & F_APPEND));
    return 0;
}

/* Appends len bytes to the write buffer; a block that is at least as big as the buffer goes straight out when the buffer is
 * empty. Returns the number of bytes accepted. */
static size_t buf_append(FILE *f, const char *p, size_t len) {
    size_t done = 0;
    while (done < len) {
        size_t rem = len - done;
        if (!f->wlen && rem >= f->bufsize) {
            size_t w = write_all(f, p + done, rem);
            return done + w;
        }
        size_t space = f->bufsize - f->wlen, k = rem < space ? rem : space;
        memcpy(f->buf + f->wlen, p + done, k);
        f->wlen += k;
        done += k;
        if (f->wlen == f->bufsize && flush_write(f)) return done;
    }
    return done;
}

/* Writes len bytes honouring the buffering mode; returns the number of bytes accepted. */
static size_t put_bytes(FILE *f, const char *p, size_t len) {
    if (f->bufmode != _IONBF && alloc_buf(f)) return write_all(f, p, len);       /* no memory for a buffer: write directly */
    if (f->bufmode == _IONBF) return write_all(f, p, len);
    if (f->bufmode == _IOLBF) {
        const char *last = memrchr(p, '\n', len);
        if (last) {                                                              /* everything up to the last newline goes out now */
            size_t head = (size_t)(last - p) + 1;
            size_t got = buf_append(f, p, head);
            if (got < head) return got;
            if (flush_write(f)) return got;
            return head + buf_append(f, p + head, len - head);
        }
    }
    return buf_append(f, p, len);
}

static size_t fwrite_nl(const void *vp, size_t size, size_t n, FILE *f) {
    if (!size || !n) return 0;
    size_t total = n > SIZE_MAX / size ? SIZE_MAX / size * size : size * n;
    if (wprep(f)) return 0;
    return put_bytes(f, vp, total) / size;
}

static int fputc_nl(int c, FILE *f) {
    char ch = (char)c;
    if (wprep(f)) return EOF;
    return put_bytes(f, &ch, 1) == 1 ? (unsigned char)c : EOF;
}

static int fputs_nl(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite_nl(s, 1, n, f) == n ? 0 : EOF;
}

static int puts_nl(const char *s) {
    if (fputs_nl(s, stdout) == EOF) return EOF;
    return fputc_nl('\n', stdout) == EOF ? EOF : 0;
}

/* ---------------------------------------------------------------- opening and closing */

static int parse_mode(const char *mode, int *oflags, int *fflags) {
    int plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': *oflags = plus ? O_RDWR : O_RDONLY; *fflags = F_READ | (plus ? F_WRITE : 0); break;
    case 'w': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; *fflags = F_WRITE | (plus ? F_READ : 0); break;
    case 'a': *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; *fflags = F_WRITE | F_APPEND | (plus ? F_READ : 0); break;
    default: errno = EINVAL; return -1;
    }
    return 0;
}

static FILE *fdopen_nl(int fd, const char *mode) {
    int oflags, fflags;
    if (parse_mode(mode, &oflags, &fflags)) return NULL;
    FILE *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fd = fd;
    f->ungot = -1;
    f->bufmode = _IOFBF;
    f->flags = fflags;
    int lk = lock_list();
    f->next = g_files;
    g_files = f;
    unlock_list(lk);
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int oflags, fflags;
    if (parse_mode(mode, &oflags, &fflags)) return NULL;
    int fd = open(path, oflags, 0666);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) { close(fd); return NULL; }
    if (!strncmp(path, "/dev/", 5) && isatty(fd)) f->bufmode = _IOLBF;          /* a terminal opened by name behaves like stdout */
    return f;
}

static FILE *freopen_nl(const char *path, const char *mode, FILE *f) {
    int oflags, fflags;
    if (parse_mode(mode, &oflags, &fflags)) return NULL;
    fflush_nl(f);
    int oldfd = f->fd, fd;
    if (path) fd = open(path, oflags, 0666);
    else fd = dup(oldfd);
    if (fd < 0) return NULL;
    if (fd != oldfd) {                                                          /* the stream keeps its descriptor number */
        dup2(fd, oldfd);
        close(fd);
        fd = oldfd;
    }
    f->fd = fd;
    f->flags = (f->flags & F_OWNBUF) | fflags;
    f->rpos = f->rlen = f->wlen = 0;
    f->ungot = -1;
    if (f == &g_stdout) f->bufmode = isatty(fd) ? _IOLBF : _IOFBF;
    return f;
}

/* The stream lock is released before the stream leaves the list and is freed (list lock, then nothing: see the lock order). */
int fclose(FILE *f) {
    int lk = lock_stream(f);
    int r = fflush_nl(f);
    if (!IS_STD(f) || f->fd > 2) {
        if (close(f->fd) < 0) r = EOF;
    }
    if (f->flags & F_OWNBUF) { free(f->buf); f->buf = NULL; f->bufsize = 0; f->flags &= ~F_OWNBUF; }
    unlock_stream(f, lk);
    if (!IS_STD(f)) {
        int ll = lock_list();
        for (FILE **pp = &g_files; *pp; pp = &(*pp)->next)
            if (*pp == f) { *pp = f->next; break; }
        unlock_list(ll);
        free(f);
    }
    return r;
}

static int fseek_nl(FILE *f, long off, int whence) {
    if ((f->flags & F_WRITE) && flush_write(f)) return -1;
    if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
    if (lseek(f->fd, off, whence) < 0) return -1;              /* a failed seek leaves the buffered data and the flags alone */
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->flags &= ~F_EOF;
    return 0;
}

static long ftell_nl(FILE *f) {
    long pos = (long)lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return pos - (long)(f->rlen - f->rpos) - (f->ungot >= 0 ? 1 : 0) + (long)f->wlen;
}

static int setvbuf_nl(FILE *f, char *buf, int mode, size_t size) {
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    if (f->wlen) flush_write(f);
    if (buf && size) {
        if (f->flags & F_OWNBUF) { free(f->buf); f->flags &= ~F_OWNBUF; }
        f->buf = (unsigned char *)buf;
        f->bufsize = size;
        f->rpos = f->rlen = 0;
    } else if (mode != _IONBF && !buf && size && (f->flags & F_OWNBUF) && size != f->bufsize) {
        unsigned char *nb = malloc(size);                                       /* a new size without a caller's buffer */
        if (nb) { free(f->buf); f->buf = nb; f->bufsize = size; f->rpos = f->rlen = 0; }
    }
    f->bufmode = mode;
    return 0;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

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

/* ---------------------------------------------------------------- formatted input and output */

static int file_flush(Sink *s) {
    size_t n = fwrite_nl(s->buf, 1, s->pos, s->ctx);
    int bad = n != s->pos;
    s->pos = 0;
    return bad ? -1 : 0;
}

static int file_direct(Sink *s, const char *p, size_t n) { return fwrite_nl(p, 1, n, s->ctx) != n ? -1 : 0; }

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

static int file_get(void *ctx) { return fgetc_nl(ctx); }
static void file_unget(void *ctx, int c) { ungetc_nl(c, ctx); }

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

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    Src src = { file_get, file_unget, f };
    int lk = lock_stream(f);
    int r = __vscan(&src, fmt, ap);
    unlock_stream(f, lk);
    return r;
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

/* ---------------------------------------------------------------- thread-safe entry points */

#define STDIO_LOCKED(rettype, name, params, args)      \
    rettype name params {                              \
        int lk = lock_stream(f);                       \
        rettype r = name##_nl args;                    \
        unlock_stream(f, lk);                          \
        return r;                                      \
    }

STDIO_LOCKED(int, ungetc, (int c, FILE *f), (c, f))
STDIO_LOCKED(size_t, fwrite, (const void *buf, size_t size, size_t n, FILE *f), (buf, size, n, f))
STDIO_LOCKED(size_t, fread, (void *buf, size_t size, size_t n, FILE *f), (buf, size, n, f))
STDIO_LOCKED(char *, fgets, (char *buf, int n, FILE *f), (buf, n, f))
STDIO_LOCKED(ssize_t, getdelim, (char **line, size_t *cap, int delim, FILE *f), (line, cap, delim, f))
STDIO_LOCKED(FILE *, freopen, (const char *path, const char *mode, FILE *f), (path, mode, f))
STDIO_LOCKED(int, fseek, (FILE *f, long off, int whence), (f, off, whence))
STDIO_LOCKED(long, ftell, (FILE *f), (f))
STDIO_LOCKED(int, vfprintf, (FILE *f, const char *fmt, va_list ap), (f, fmt, ap))
STDIO_LOCKED(int, setvbuf, (FILE *f, char *buf, int mode, size_t size), (f, buf, mode, size))
STDIO_LOCKED(int, fputs, (const char *s, FILE *f), (s, f))

int fgetc(FILE *f) {
    if (!__libc_threaded && f->ungot < 0 && f->rpos < f->rlen) return f->buf[f->rpos++];       /* the common case, no call chain */
    int lk = lock_stream(f);
    int r = fgetc_nl(f);
    unlock_stream(f, lk);
    return r;
}

int fputc(int c, FILE *f) {
    if (!__libc_threaded && f->bufsize && (f->flags & F_WRITE) && f->bufmode != _IONBF && !f->rlen && f->ungot < 0 &&
        f->wlen + 1 < f->bufsize && !(f->bufmode == _IOLBF && c == '\n')) {
        f->buf[f->wlen++] = (unsigned char)c;
        return (unsigned char)c;
    }
    int lk = lock_stream(f);
    int r = fputc_nl(c, f);
    unlock_stream(f, lk);
    return r;
}

int puts(const char *s) {
    int lk = lock_stream(stdout);
    int r = puts_nl(s);
    unlock_stream(stdout, lk);
    return r;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
ssize_t getline(char **line, size_t *cap, FILE *f) { return getdelim(line, cap, '\n', f); }

/* fflush(NULL) flushes every output stream. The list lock is held while the streams are visited (a stream cannot be closed
 * meanwhile); the order is always list lock first, then a stream's lock. */
int fflush(FILE *f) {
    int r = 0;
    if (f) {
        int lk = lock_stream(f);
        r = fflush_nl(f);
        unlock_stream(f, lk);
        return r;
    }
    int lk = lock_stream(&g_stdout);
    if (flush_write(&g_stdout)) r = EOF;
    unlock_stream(&g_stdout, lk);
    int ll = lock_list();
    for (FILE *p = g_files; p; p = p->next) {
        if (!(p->flags & F_WRITE)) continue;
        int pl = lock_stream(p);
        if (flush_write(p)) r = EOF;
        unlock_stream(p, pl);
    }
    unlock_list(ll);
    return r;
}

FILE *fdopen(int fd, const char *mode) { return fdopen_nl(fd, mode); }         /* the list lock is taken inside */

/* ---------------------------------------------------------------- around fork() (see sys.c) */

/* fork() takes these whether or not the process has threads (a single-threaded process holds none of them at that point). */
void __stdio_fork_prepare(void) {
    __rlock(&__stdio_list_lock);
    __rlock(&g_stdin.lock);
    __rlock(&g_stdout.lock);
    __rlock(&g_stderr.lock);
}

void __stdio_fork_parent(void) {
    __runlock(&g_stderr.lock);
    __runlock(&g_stdout.lock);
    __runlock(&g_stdin.lock);
    __runlock(&__stdio_list_lock);
}

/* The child has one thread: every lock another thread held is dead for good, so all of them start again unlocked. */
void __stdio_fork_child(void) {
    memset(&__stdio_list_lock, 0, sizeof __stdio_list_lock);
    for (FILE *p = g_files; p; p = p->next) memset(&p->lock, 0, sizeof p->lock);
    memset(&g_stdin.lock, 0, sizeof g_stdin.lock);
    memset(&g_stdout.lock, 0, sizeof g_stdout.lock);
    memset(&g_stderr.lock, 0, sizeof g_stderr.lock);
}
