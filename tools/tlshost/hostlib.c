/*
 * The little C library libtls needs when it is compiled into a Windows DLL for tests on the build machine (tools/tlshost.py):
 * mem* and str* functions, malloc, and a clock. Freestanding: no C runtime is linked.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    if (dd < ss) { for (size_t i = 0; i < n; i++) dd[i] = ss[i]; }
    else { for (size_t i = n; i-- > 0;) dd[i] = ss[i]; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

/* malloc: a bump allocator over a big static arena; tlshost_reset() starts over (tests call it between runs) */
static uint8_t g_arena[96u << 20];
static size_t g_used;

void tlshost_reset(void) { g_used = 0; }

void *malloc(size_t n) {
    size_t need = (n + 31) & ~(size_t)31;
    if (g_used + need + 32 > sizeof g_arena) return NULL;
    uint8_t *p = g_arena + g_used + 32;
    *(size_t *)(p - 32) = n;
    g_used += need + 32;
    return p;
}

void free(void *p) { (void)p; }

void *calloc(size_t a, size_t b) {
    void *p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    size_t old = *(size_t *)((uint8_t *)p - 32);
    if (n <= old) return p;
    void *q = malloc(n);
    if (q) memcpy(q, p, old);
    return q;
}

long time(long *t) { if (t) *t = 0; return 0; }

/* the OS services of os_io.c: the tests always pass tls_config.random and tls_config.now themselves */
int tls_os_random(void *buf, size_t n) { (void)buf; (void)n; return -1; }
int tls_os_random_ready(void) { return 1; }
int64_t tls_os_time(void) { return 0; }
