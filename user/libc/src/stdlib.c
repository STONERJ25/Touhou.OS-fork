/* Conversions (strtol and friends; strtod is in fpconv.c), rand, qsort, bsearch, realpath. malloc is in malloc.c. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>

/* ---------------------------------------------------------------- numbers */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r = { a / b, a % b }; return r; }
intmax_t imaxabs(intmax_t x) { return x < 0 ? -x : x; }

/* Parses the magnitude of an integer in the given base (0 = look at the prefix, 2..36). *negp reports a leading '-', *over an
 * overflow of unsigned long long (the value is then ULLONG_MAX). A "0x" prefix counts only when a hex digit follows it, so
 * "0xg" is the number 0 with the end pointer at the 'x'. A bad base fails with EINVAL; without digits *end is s. */
static unsigned long long parse_ull(const char *s, char **end, int base, int *negp, int *over) {
    const char *p = s;
    *negp = 0;
    *over = 0;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end) *end = (char *)s;
        return 0;
    }
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    *negp = neg;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && isxdigit((unsigned char)p[2])) { base = 16; p += 2; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    int any = 0, overflow = 0;
    for (;; p++) {
        int d;
        unsigned char c = (unsigned char)*p;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((unsigned)((c | 32) - 'a') < 26u) d = (c | 32) - 'a' + 10;
        else break;
        if (d >= base) break;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) overflow = 1;
        else v = v * (unsigned)base + (unsigned)d;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    if (overflow) { *over = 1; return ULLONG_MAX; }
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, over;
    unsigned long long v = parse_ull(s, end, base, &neg, &over);
    if (over) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? 0ULL - v : v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg, over;
    unsigned long long v = parse_ull(s, end, base, &neg, &over);
    if (over) { errno = ERANGE; return neg ? LLONG_MIN : LLONG_MAX; }
    if (neg) {
        if (v > (unsigned long long)LLONG_MAX + 1) { errno = ERANGE; return LLONG_MIN; }
        return (long long)(0ULL - v);
    }
    if (v > (unsigned long long)LLONG_MAX) { errno = ERANGE; return LLONG_MAX; }
    return (long long)v;
}

unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* strtod, strtof, strtold and atof are in fpconv.c (exactly rounded). */

/* ---------------------------------------------------------------- rand */

static unsigned long long g_rand = 0x853C49E6748FEA9BULL;

void srand(unsigned seed) { g_rand = seed * 6364136223846793005ULL + 1442695040888963407ULL; }
void srandom(unsigned seed) { srand(seed); }

int rand(void) {
    g_rand = g_rand * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((g_rand >> 33) & 0x7FFFFFFF);
}
long random(void) { return rand(); }

/* ---------------------------------------------------------------- qsort */

typedef int (*Cmp)(const void *, const void *);

static void swap_elems(char *a, char *b, size_t n) {
    if (a == b) return;
    if (!(n & 7) && !(((uintptr_t)a | (uintptr_t)b) & 7)) {
        uint64_t *x = (uint64_t *)a, *y = (uint64_t *)b;
        for (size_t i = 0; i < n / 8; i++) { uint64_t t = x[i]; x[i] = y[i]; y[i] = t; }
        return;
    }
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void sift_down(char *base, size_t root, size_t n, size_t size, Cmp cmp) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        swap_elems(base + root * size, base + child * size, size);
        root = child;
    }
}

static void heap_sort(char *base, size_t n, size_t size, Cmp cmp) {
    for (size_t i = n / 2; i-- > 0;) sift_down(base, i, n, size, cmp);
    for (size_t end = n; end-- > 1;) {
        swap_elems(base, base + end * size, size);
        sift_down(base, 0, end, size, cmp);
    }
}

/* Quicksort on [lo, hi] with a median-of-three pivot; recursion depth is limited, and a part that stays too deep falls back
 * to heapsort, so no input makes it quadratic. */
static void qs(char *base, long lo, long hi, size_t size, Cmp cmp, int depth) {
    while (hi - lo > 12) {
        if (depth-- == 0) { heap_sort(base + (size_t)lo * size, (size_t)(hi - lo + 1), size, cmp); return; }
        long mid = lo + (hi - lo) / 2;
        char *a = base + (size_t)lo * size, *b = base + (size_t)mid * size, *c = base + (size_t)hi * size;
        if (cmp(a, b) > 0) swap_elems(a, b, size);
        if (cmp(b, c) > 0) { swap_elems(b, c, size); if (cmp(a, b) > 0) swap_elems(a, b, size); }
        char tmp[256];
        char *pv = size <= sizeof tmp ? tmp : malloc(size);
        if (!pv) { heap_sort(base + (size_t)lo * size, (size_t)(hi - lo + 1), size, cmp); return; }
        memcpy(pv, b, size);
        long i = lo, j = hi;
        while (i <= j) {
            while (cmp(base + (size_t)i * size, pv) < 0) i++;
            while (cmp(base + (size_t)j * size, pv) > 0) j--;
            if (i <= j) {
                swap_elems(base + (size_t)i * size, base + (size_t)j * size, size);
                i++;
                j--;
            }
        }
        if (pv != tmp) free(pv);
        if (j - lo < hi - i) { qs(base, lo, j, size, cmp, depth); lo = i; }
        else { qs(base, i, hi, size, cmp, depth); hi = j; }
    }
    for (long i = lo + 1; i <= hi; i++) {                       /* insertion sort for the short parts */
        for (long j = i; j > lo && cmp(base + (size_t)(j - 1) * size, base + (size_t)j * size) > 0; j--)
            swap_elems(base + (size_t)(j - 1) * size, base + (size_t)j * size, size);
    }
}

void qsort(void *base, size_t n, size_t size, Cmp cmp) {
    if (n < 2 || !size) return;
    int depth = 2 * (64 - __builtin_clzl(n));
    qs(base, 0, (long)n - 1, size, cmp, depth);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, Cmp cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
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
