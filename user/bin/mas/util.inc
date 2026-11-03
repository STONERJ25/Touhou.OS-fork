/* util.inc -- allocation, byte buffers, diagnostics. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

static const char *cur_file = "<command line>"; /* the source line being processed, for messages */
static int cur_line;

static void fatal(const char *fmt, ...) __attribute__((noreturn));
static void fatal(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s:%d: error: ", cur_file, cur_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " -- Marisa: that is not in my grimoire, ze.\n");
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fatal("out of memory");
    return p;
}

static void *xcalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) fatal("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) fatal("out of memory");
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

static char *xfmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *p = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

typedef struct {
    u8 *p;
    size_t len, cap;
} Buf;

static void buf_put(Buf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->len, d, n);
    b->len += n;
}

static void buf_u8(Buf *b, u32 v) {
    u8 c = (u8)v;
    buf_put(b, &c, 1);
}

static void buf_le(Buf *b, u64 v, int bytes) {
    for (int i = 0; i < bytes; i++) buf_u8(b, (u32)(v >> (8 * i)));
}

static void buf_zero(Buf *b, size_t n) {
    for (size_t i = 0; i < n; i++) buf_u8(b, 0);
}

static void put_le(u8 *p, u64 v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (u8)(v >> (8 * i));
}
