/* malloc (boundary-tag free list over brk, mmap for big blocks), conversions, qsort, rand. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <sys/mman.h>
#include <math.h>

/* ---------------------------------------------------------------- malloc */

#define WSIZE 8
#define DSIZE 16
#define CHUNK (256 * 1024)
#define MMAP_THRESHOLD (256 * 1024)
#define MIN_BLOCK 32

#define PACK(sz, a)   ((sz) | (a))
#define GET(p)        (*(size_t *)(p))
#define PUT(p, v)     (*(size_t *)(p) = (v))
#define BSIZE(p)      (GET(p) & ~(size_t)0xF)
#define BALLOC(p)     (GET(p) & 1)
#define HDRP(bp)      ((char *)(bp) - WSIZE)
#define FTRP(bp)      ((char *)(bp) + BSIZE(HDRP(bp)) - DSIZE)
#define NEXT_BLK(bp)  ((char *)(bp) + BSIZE(HDRP(bp)))
#define PREV_BLK(bp)  ((char *)(bp) - BSIZE((char *)(bp) - DSIZE))
#define NEXT_FREE(bp) (*(void **)(bp))
#define PREV_FREE(bp) (*(void **)((char *)(bp) + WSIZE))

static char *g_heap;
static void *g_free;

static void fl_insert(void *bp) {
    NEXT_FREE(bp) = g_free;
    PREV_FREE(bp) = NULL;
    if (g_free) PREV_FREE(g_free) = bp;
    g_free = bp;
}

static void fl_remove(void *bp) {
    if (PREV_FREE(bp)) NEXT_FREE(PREV_FREE(bp)) = NEXT_FREE(bp);
    else g_free = NEXT_FREE(bp);
    if (NEXT_FREE(bp)) PREV_FREE(NEXT_FREE(bp)) = PREV_FREE(bp);
}

static void *coalesce(void *bp) {
    size_t prev_alloc = BALLOC(FTRP(PREV_BLK(bp)));
    size_t next_alloc = BALLOC(HDRP(NEXT_BLK(bp)));
    size_t size = BSIZE(HDRP(bp));
    if (prev_alloc && next_alloc) {
        /* nothing to merge */
    } else if (prev_alloc && !next_alloc) {
        fl_remove(NEXT_BLK(bp));
        size += BSIZE(HDRP(NEXT_BLK(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    } else if (!prev_alloc && next_alloc) {
        fl_remove(PREV_BLK(bp));
        size += BSIZE(HDRP(PREV_BLK(bp)));
        bp = PREV_BLK(bp);
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    } else {
        fl_remove(PREV_BLK(bp));
        fl_remove(NEXT_BLK(bp));
        size += BSIZE(HDRP(PREV_BLK(bp))) + BSIZE(HDRP(NEXT_BLK(bp)));
        bp = PREV_BLK(bp);
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }
    fl_insert(bp);
    return bp;
}

static void *extend_heap(size_t bytes) {
    bytes = (bytes + 4095) & ~(size_t)4095;
    char *bp = sbrk((long)bytes);
    if (bp == (char *)-1) return NULL;
    PUT(HDRP(bp), PACK(bytes, 0));
    PUT(FTRP(bp), PACK(bytes, 0));
    PUT(HDRP(NEXT_BLK(bp)), PACK(0, 1));        /* new epilogue */
    return coalesce(bp);
}

static int heap_init(void) {
    char *p = sbrk(0);
    size_t pad = (8 - ((size_t)p & 15)) & 15;    /* make payloads 16-byte aligned */
    if (sbrk((long)(pad + 4 * WSIZE)) == (void *)-1) return -1;
    g_heap = p + pad;
    PUT(g_heap, 0);                              /* alignment pad word */
    PUT(g_heap + WSIZE, PACK(DSIZE, 1));         /* prologue header */
    PUT(g_heap + 2 * WSIZE, PACK(DSIZE, 1));     /* prologue footer */
    PUT(g_heap + 3 * WSIZE, PACK(0, 1));         /* epilogue header */
    g_heap += 2 * WSIZE;
    return extend_heap(CHUNK) ? 0 : -1;
}

static void *find_fit(size_t size) {
    for (void *bp = g_free; bp; bp = NEXT_FREE(bp))
        if (BSIZE(HDRP(bp)) >= size) return bp;
    return NULL;
}

static void place(void *bp, size_t size) {
    size_t csize = BSIZE(HDRP(bp));
    fl_remove(bp);
    if (csize - size >= MIN_BLOCK) {
        PUT(HDRP(bp), PACK(size, 1));
        PUT(FTRP(bp), PACK(size, 1));
        void *rest = NEXT_BLK(bp);
        PUT(HDRP(rest), PACK(csize - size, 0));
        PUT(FTRP(rest), PACK(csize - size, 0));
        fl_insert(rest);
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n >= MMAP_THRESHOLD) {
        size_t total = (n + DSIZE + 4095) & ~(size_t)4095;
        char *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) return NULL;
        PUT(m, total);                          /* size of the mapping, flagged by magic in the next word */
        PUT(m + WSIZE, 0x4D4D4150);             /* "MMAP" */
        return m + DSIZE;
    }
    if (!g_heap && heap_init()) { errno = ENOMEM; return NULL; }
    size_t size = (n + DSIZE + 15) & ~(size_t)15;
    if (size < MIN_BLOCK) size = MIN_BLOCK;
    void *bp = find_fit(size);
    if (!bp) {
        bp = extend_heap(size > CHUNK ? size : CHUNK);
        if (!bp) { errno = ENOMEM; return NULL; }
    }
    place(bp, size);
    return bp;
}

void free(void *p) {
    if (!p) return;
    char *b = (char *)p;
    if (GET(b - WSIZE) == 0x4D4D4150) {
        size_t total = GET(b - DSIZE);
        munmap(b - DSIZE, total);
        return;
    }
    size_t size = BSIZE(HDRP(p));
    PUT(HDRP(p), PACK(size, 0));
    PUT(FTRP(p), PACK(size, 0));
    coalesce(p);
}

void *calloc(size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) { errno = ENOMEM; return NULL; }
    void *p = malloc(n * sz);
    if (p) memset(p, 0, n * sz);
    return p;
}

static size_t usable(void *p) {
    char *b = p;
    if (GET(b - WSIZE) == 0x4D4D4150) return GET(b - DSIZE) - DSIZE;
    return BSIZE(HDRP(p)) - DSIZE;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    size_t old = usable(p);
    if (n <= old) return p;
    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, old);
    free(p);
    return np;
}

void *aligned_alloc(size_t align, size_t n) {
    if (align <= 16) return malloc(n);
    void *raw = malloc(n + align + 16);
    if (!raw) return NULL;
    size_t a = ((size_t)raw + 16 + align - 1) & ~(align - 1);
    /* leak-free aligned blocks would need a header trick; this allocator documents the limitation: do not free() them */
    return (void *)a;
}

int posix_memalign(void **out, size_t align, size_t n) {
    void *p = aligned_alloc(align, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

/* ---------------------------------------------------------------- numbers */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }

/* Parses the magnitude; *negp reports a leading '-'. */
static unsigned long long parse_ull(const char *s, char **end, int base, int *negp) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    *negp = neg;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    int any = 0, overflow = 0;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) overflow = 1;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    if (overflow) { errno = ERANGE; return ULLONG_MAX; }
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg;
    unsigned long long v = parse_ull(s, end, base, &neg);
    return neg ? 0ULL - v : v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg;
    unsigned long long v = parse_ull(s, end, base, &neg);
    if (neg) return v > (unsigned long long)LLONG_MAX + 1 ? LLONG_MIN : (long long)(0ULL - v);
    return v > (unsigned long long)LLONG_MAX ? LLONG_MAX : (long long)v;
}

unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (!strncasecmp(p, "inf", 3)) { if (end) *end = (char *)p + 3; return neg ? -INFINITY : INFINITY; }
    if (!strncasecmp(p, "nan", 3)) { if (end) *end = (char *)p + 3; return NAN; }
    double v = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (isdigit((unsigned char)*p)) { v += (*p - '0') * scale; scale /= 10; p++; digits++; }
    }
    if (!digits) { if (end) *end = (char *)s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '-') { eneg = 1; q++; } else if (*q == '+') q++;
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            while (isdigit((unsigned char)*q)) { e = e * 10 + (*q - '0'); q++; }
            double m = 1;
            for (int i = 0; i < e && i < 400; i++) m *= 10;
            v = eneg ? v / m : v * m;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, NULL); }

/* ---------------------------------------------------------------- rand / qsort */

static unsigned long long g_rand = 0x853C49E6748FEA9BULL;

void srand(unsigned seed) { g_rand = seed * 6364136223846793005ULL + 1442695040888963407ULL; }
void srandom(unsigned seed) { srand(seed); }

int rand(void) {
    g_rand = g_rand * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((g_rand >> 33) & 0x7FFFFFFF);
}
long random(void) { return rand(); }

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void qs(char *base, long lo, long hi, size_t size, int (*cmp)(const void *, const void *)) {
    while (lo < hi) {
        long i = lo, j = hi;
        char *pivot = base + (size_t)((lo + hi) / 2) * size;
        char tmp[256];
        char *pv = size <= sizeof tmp ? tmp : NULL;
        if (!pv) { pv = malloc(size); if (!pv) return; }
        memcpy(pv, pivot, size);
        while (i <= j) {
            while (cmp(base + (size_t)i * size, pv) < 0) i++;
            while (cmp(base + (size_t)j * size, pv) > 0) j--;
            if (i <= j) {
                swap_bytes(base + (size_t)i * size, base + (size_t)j * size, size);
                i++;
                j--;
            }
        }
        if (pv != tmp) free(pv);
        if (j - lo < hi - i) { qs(base, lo, j, size, cmp); lo = i; }
        else { qs(base, i, hi, size, cmp); hi = j; }
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n > 1) qs(base, 0, (long)n - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const void *e = (const char *)base + mid * size;
        int c = cmp(key, e);
        if (!c) return (void *)e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

char *realpath(const char *path, char *out) {
    char cwd[PATH_MAX], tmp[PATH_MAX];
    if (path[0] != '/') {
        if (!getcwd(cwd, sizeof cwd)) return NULL;
        strlcpy(tmp, cwd, sizeof tmp);
        strlcat(tmp, "/", sizeof tmp);
        strlcat(tmp, path, sizeof tmp);
    } else {
        strlcpy(tmp, path, sizeof tmp);
    }
    char *comps[128];
    int nc = 0;
    for (char *s = tmp; *s;) {
        while (*s == '/') s++;
        if (!*s) break;
        char *st = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (!strcmp(st, ".")) continue;
        if (!strcmp(st, "..")) { if (nc) nc--; continue; }
        if (nc < 128) comps[nc++] = st;
    }
    char *o = out ? out : malloc(PATH_MAX);
    if (!o) return NULL;
    o[0] = 0;
    if (!nc) strcpy(o, "/");
    for (int i = 0; i < nc; i++) { strcat(o, "/"); strcat(o, comps[i]); }
    return o;
}
