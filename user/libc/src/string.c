/* String and memory functions. The hot ones (mem*, strlen, strchr, strcmp) work 16 bytes at a time with SSE2, and never read
 * outside the bytes they were given (or, for strlen/strchr, outside the aligned 16-byte block that holds the NUL), so a string
 * that ends at the end of a mapping is safe. They are written so that the compiler cannot turn their loops back into calls
 * to themselves (no_builtin). memcpy copies forward and loads every block before storing it, which also makes it correct for
 * overlapping moves towards lower addresses (what `rep movsb` did); memmove handles the other direction. */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <emmintrin.h>

#ifdef __clang__
#define NB __attribute__((no_builtin))
#else
#define NB
#endif

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;

typedef struct { u64 v; } __attribute__((packed, may_alias)) U64;
typedef struct { u32 v; } __attribute__((packed, may_alias)) U32;
typedef struct { u16 v; } __attribute__((packed, may_alias)) U16;
static inline u64 ld64(const void *p) { return ((const U64 *)p)->v; }
static inline u32 ld32(const void *p) { return ((const U32 *)p)->v; }
static inline u16 ld16(const void *p) { return ((const U16 *)p)->v; }
static inline void st64(void *p, u64 v) { ((U64 *)p)->v = v; }
static inline void st32(void *p, u32 v) { ((U32 *)p)->v = v; }
static inline void st16(void *p, u16 v) { ((U16 *)p)->v = v; }
static inline __m128i ldv(const void *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void stv(void *p, __m128i v) { _mm_storeu_si128((__m128i *)p, v); }
static inline unsigned eqmask(__m128i a, __m128i b) { return (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)); }

NB void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n <= 16) {
        if (n >= 8) { u64 a = ld64(s), b = ld64(s + n - 8); st64(d, a); st64(d + n - 8, b); }
        else if (n >= 4) { u32 a = ld32(s), b = ld32(s + n - 4); st32(d, a); st32(d + n - 4, b); }
        else if (n >= 2) { u16 a = ld16(s), b = ld16(s + n - 2); st16(d, a); st16(d + n - 2, b); }
        else if (n) d[0] = s[0];
        return dst;
    }
    if (n <= 32) {
        __m128i a = ldv(s), b = ldv(s + n - 16);
        stv(d, a);
        stv(d + n - 16, b);
        return dst;
    }
    if (n <= 64) {
        __m128i a = ldv(s), b = ldv(s + 16), c = ldv(s + n - 32), e = ldv(s + n - 16);
        stv(d, a);
        stv(d + 16, b);
        stv(d + n - 32, c);
        stv(d + n - 16, e);
        return dst;
    }
    __m128i t0 = ldv(s + n - 64), t1 = ldv(s + n - 48), t2 = ldv(s + n - 32), t3 = ldv(s + n - 16);
    size_t i = 0;
    for (; n - i > 64; i += 64) {
        __m128i a = ldv(s + i), b = ldv(s + i + 16), c = ldv(s + i + 32), e = ldv(s + i + 48);
        stv(d + i, a);
        stv(d + i + 16, b);
        stv(d + i + 32, c);
        stv(d + i + 48, e);
    }
    stv(d + n - 64, t0);
    stv(d + n - 48, t1);
    stv(d + n - 32, t2);
    stv(d + n - 16, t3);
    return dst;
}

NB void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d <= s || d >= s + n || n <= 64) return memcpy(dst, src, n);      /* memcpy loads before it stores up to 64 bytes */
    __m128i h0 = ldv(s), h1 = ldv(s + 16), h2 = ldv(s + 32), h3 = ldv(s + 48);
    size_t i = n;
    while (i > 64) {
        i -= 64;
        __m128i a = ldv(s + i), b = ldv(s + i + 16), c = ldv(s + i + 32), e = ldv(s + i + 48);
        stv(d + i, a);
        stv(d + i + 16, b);
        stv(d + i + 32, c);
        stv(d + i + 48, e);
    }
    stv(d, h0);
    stv(d + 16, h1);
    stv(d + 32, h2);
    stv(d + 48, h3);
    return dst;
}

NB void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    u64 v = 0x0101010101010101ULL * (unsigned char)c;
    if (n <= 16) {
        if (n >= 8) { st64(d, v); st64(d + n - 8, v); }
        else if (n >= 4) { st32(d, (u32)v); st32(d + n - 4, (u32)v); }
        else if (n >= 2) { st16(d, (u16)v); st16(d + n - 2, (u16)v); }
        else if (n) d[0] = (unsigned char)c;
        return dst;
    }
    __m128i x = _mm_set1_epi8((char)c);
    if (n <= 32) { stv(d, x); stv(d + n - 16, x); return dst; }
    if (n <= 64) { stv(d, x); stv(d + 16, x); stv(d + n - 32, x); stv(d + n - 16, x); return dst; }
    size_t i = 0;
    for (; n - i > 64; i += 64) {
        stv(d + i, x);
        stv(d + i + 16, x);
        stv(d + i + 32, x);
        stv(d + i + 48, x);
    }
    stv(d + n - 64, x);
    stv(d + n - 48, x);
    stv(d + n - 32, x);
    stv(d + n - 16, x);
    return dst;
}

NB int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    if (n >= 16) {
        size_t i = 0;
        for (; i + 16 <= n; i += 16) {
            unsigned m = eqmask(ldv(x + i), ldv(y + i)) ^ 0xFFFFu;
            if (m) { int k = __builtin_ctz(m); return x[i + k] - y[i + k]; }
        }
        if (i < n) {                                        /* the last block overlaps the one before it */
            i = n - 16;
            unsigned m = eqmask(ldv(x + i), ldv(y + i)) ^ 0xFFFFu;
            if (m) { int k = __builtin_ctz(m); return x[i + k] - y[i + k]; }
        }
        return 0;
    }
    if (n >= 8) {
        u64 p = __builtin_bswap64(ld64(x)), q = __builtin_bswap64(ld64(y));
        if (p != q) return p < q ? -1 : 1;
        p = __builtin_bswap64(ld64(x + n - 8));
        q = __builtin_bswap64(ld64(y + n - 8));
        return p < q ? -1 : p > q;
    }
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

NB void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char ch = (unsigned char)c;
    if (n >= 16) {
        __m128i v = _mm_set1_epi8((char)ch);
        size_t i = 0;
        for (; i + 16 <= n; i += 16) {
            unsigned m = eqmask(ldv(p + i), v);
            if (m) return (void *)(p + i + __builtin_ctz(m));
        }
        if (i < n) {
            i = n - 16;
            unsigned m = eqmask(ldv(p + i), v);
            if (m) return (void *)(p + i + __builtin_ctz(m));
        }
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
        if (p[i] == ch) return (void *)(p + i);
    return NULL;
}

NB void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char ch = (unsigned char)c;
    if (n >= 16) {
        __m128i v = _mm_set1_epi8((char)ch);
        size_t i = n;
        while (i >= 16) {
            i -= 16;
            unsigned m = eqmask(ldv(p + i), v);
            if (m) return (void *)(p + i + 31 - __builtin_clz(m));
        }
        if (i) {                                            /* the first block overlaps the one after it */
            unsigned m = eqmask(ldv(p), v);
            if (m) return (void *)(p + 31 - __builtin_clz(m));
        }
        return NULL;
    }
    while (n--)
        if (p[n] == ch) return (void *)(p + n);
    return NULL;
}

NB void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (!nl) return (void *)h;
    if (nl > hl) return NULL;
    const unsigned char *p = h, *nd = n;
    size_t last = hl - nl + 1;                              /* candidate start positions */
    while (last) {
        const unsigned char *q = memchr(p, nd[0], last);
        if (!q) return NULL;
        if (!memcmp(q, nd, nl)) return (void *)q;
        last -= (size_t)(q - p) + 1;
        p = q + 1;
    }
    return NULL;
}

NB size_t strlen(const char *s) {
    const char *p = (const char *)((uintptr_t)s & ~(uintptr_t)15);
    __m128i z = _mm_setzero_si128();
    unsigned m = eqmask(_mm_load_si128((const __m128i *)p), z) >> ((uintptr_t)s & 15);
    if (m) return (size_t)__builtin_ctz(m);
    for (;;) {
        p += 16;
        m = eqmask(_mm_load_si128((const __m128i *)p), z);
        if (m) return (size_t)(p - s) + (size_t)__builtin_ctz(m);
    }
}

NB size_t strnlen(const char *s, size_t max) {             /* like strlen: never reads past the NUL's aligned block */
    if (!max) return 0;
    const char *p = (const char *)((uintptr_t)s & ~(uintptr_t)15);
    size_t sh = (uintptr_t)s & 15;
    __m128i z = _mm_setzero_si128();
    unsigned m = eqmask(_mm_load_si128((const __m128i *)p), z) >> sh;
    if (m) { size_t k = (size_t)__builtin_ctz(m); return k < max ? k : max; }
    size_t scanned = 16 - sh;
    while (scanned < max) {
        p += 16;
        m = eqmask(_mm_load_si128((const __m128i *)p), z);
        if (m) { size_t k = scanned + (size_t)__builtin_ctz(m); return k < max ? k : max; }
        scanned += 16;
    }
    return max;
}

NB int strcmp(const char *a, const char *b) {
    __m128i z = _mm_setzero_si128();
    for (;;) {
        if (((uintptr_t)a & 4095) <= 4080 && ((uintptr_t)b & 4095) <= 4080) {   /* 16 bytes stay inside both pages */
            __m128i va = ldv(a), vb = ldv(b);
            unsigned stop = (eqmask(va, vb) ^ 0xFFFFu) | eqmask(va, z);
            if (stop) { int k = __builtin_ctz(stop); return (unsigned char)a[k] - (unsigned char)b[k]; }
            a += 16;
            b += 16;
        } else {
            unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
            if (x != y || !x) return x - y;
            a++;
            b++;
        }
    }
}

NB int strncmp(const char *a, const char *b, size_t n) {
    __m128i z = _mm_setzero_si128();
    while (n) {
        if (n >= 16 && ((uintptr_t)a & 4095) <= 4080 && ((uintptr_t)b & 4095) <= 4080) {
            __m128i va = ldv(a), vb = ldv(b);
            unsigned stop = (eqmask(va, vb) ^ 0xFFFFu) | eqmask(va, z);
            if (stop) { int k = __builtin_ctz(stop); return (unsigned char)a[k] - (unsigned char)b[k]; }
            a += 16;
            b += 16;
            n -= 16;
        } else {
            unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
            if (x != y || !x) return x - y;
            a++;
            b++;
            n--;
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; n--; }
    return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0;
}

char *strcpy(char *dst, const char *src) { memcpy(dst, src, strlen(src) + 1); return dst; }

char *strncpy(char *dst, const char *src, size_t n) {
    size_t l = strnlen(src, n);
    memcpy(dst, src, l);
    if (l < n) memset(dst + l, 0, n - l);
    return dst;
}

char *strcat(char *dst, const char *src) { memcpy(dst + strlen(dst), src, strlen(src) + 1); return dst; }

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t l = strnlen(src, n);
    memcpy(d, src, l);
    d[l] = 0;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dl = strnlen(dst, size);
    if (dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

NB char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    if (!ch) return (char *)s + strlen(s);
    const char *p = (const char *)((uintptr_t)s & ~(uintptr_t)15);
    unsigned sh = (unsigned)((uintptr_t)s & 15);
    __m128i v = _mm_set1_epi8((char)ch), z = _mm_setzero_si128();
    __m128i blk = _mm_load_si128((const __m128i *)p);
    unsigned mc = eqmask(blk, v) >> sh, mz = eqmask(blk, z) >> sh;
    for (;;) {
        unsigned any = mc | mz;
        if (any) {
            int k = __builtin_ctz(any);
            return (mc >> k) & 1 ? (char *)(p + sh + k) : NULL;
        }
        p += 16;
        sh = 0;
        blk = _mm_load_si128((const __m128i *)p);
        mc = eqmask(blk, v);
        mz = eqmask(blk, z);
    }
}

char *strrchr(const char *s, int c) {
    if (!(unsigned char)c) return (char *)s + strlen(s);
    const char *last = NULL;
    for (const char *p = strchr(s, c); p; p = strchr(p + 1, c)) last = p;
    return (char *)last;
}

char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (h = strchr(h, n[0]); h; h = strchr(h + 1, n[0]))
        if (!strncmp(h, n, nl)) return (char *)h;
    return NULL;
}

char *strcasestr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (!strncasecmp(h, n, nl)) return (char *)h;
    return NULL;
}

/* A 256-bit set of the bytes in `chars`, for strspn and friends. */
static void byte_set(unsigned char set[32], const char *chars) {
    memset(set, 0, 32);
    for (const unsigned char *p = (const unsigned char *)chars; *p; p++) set[*p >> 3] |= (unsigned char)(1u << (*p & 7));
}
#define IN_SET(set, c) ((set)[(unsigned char)(c) >> 3] & (1u << ((unsigned char)(c) & 7)))

size_t strspn(const char *s, const char *accept) {
    unsigned char set[32];
    byte_set(set, accept);
    size_t n = 0;
    while (s[n] && IN_SET(set, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    if (!reject[0]) return strlen(s);
    if (!reject[1]) { const char *e = strchr(s, reject[0]); return e ? (size_t)(e - s) : strlen(s); }
    unsigned char set[32];
    byte_set(set, reject);
    size_t n = 0;
    while (s[n] && !IN_SET(set, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}

char *strndup(const char *s, size_t max) {
    size_t n = strnlen(s, max);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) { *end = 0; *save = end + 1; }
    else *save = NULL;
    return s;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strsep(char **sp, const char *delim) {
    char *s = *sp;
    if (!s) return NULL;
    char *end = s + strcspn(s, delim);
    if (*end) { *end = 0; *sp = end + 1; }
    else *sp = NULL;
    return s;
}

char *strerror(int err) {
    switch (err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted (Eiki disapproves)";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Out of memory (Yuyuko is hungry)";
    case EACCES: return "Permission denied (Eiki says no)";
    case EFAULT: return "Bad address (the Boundary held)";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Cross-device link";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENFILE: case EMFILE: return "Too many open files";
    case ENOTTY: return "Inappropriate ioctl for device";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EPIPE: return "Broken pipe";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Result too large";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    case ELOOP: return "Too many levels of symbolic links";
    case ETIMEDOUT: return "Connection timed out";
    case ENOTSOCK: return "Socket operation on non-socket";
    case EDESTADDRREQ: return "Destination address required";
    case EMSGSIZE: return "Message too long";
    case EPROTOTYPE: return "Protocol wrong type for socket";
    case ENOPROTOOPT: return "Protocol not available";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case EOPNOTSUPP: return "Operation not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Cannot assign requested address";
    case ENETDOWN: return "Network is down (the gap is closed)";
    case ENETUNREACH: return "Network is unreachable (Yukari found no gap)";
    case ECONNABORTED: return "Software caused connection abort";
    case ECONNRESET: return "Connection reset by peer";
    case ENOBUFS: return "No buffer space available";
    case EISCONN: return "Transport endpoint is already connected";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTDOWN: return "Host is down";
    case EHOSTUNREACH: return "No route to host";
    case EALREADY: return "Operation already in progress";
    case EINPROGRESS: return "Operation now in progress";
    default: return "Unknown error";
    }
}
