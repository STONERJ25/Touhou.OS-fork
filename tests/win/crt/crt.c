/* The mini C runtime of the Windows test programs (see crt.h): the entry point, argv, exit and atexit, the heap over HeapAlloc, the string functions,
 * a printf family over vsnprintf, and the two symbols the LLVM windows-msvc target expects from a C runtime: __chkstk (called with the frame size in rax
 * before a frame of 4 KiB or more is allocated; it touches each page from the top down and leaves rsp alone, as the x64 calling convention's page on
 * stack allocation describes) and _fltused. */
#include "crt.h"

typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
#define IMPORT __declspec(dllimport)
IMPORT void ExitProcess(DWORD code);
IMPORT HANDLE GetStdHandle(DWORD which);
IMPORT BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped);
IMPORT char *GetCommandLineA(void);
IMPORT HANDLE GetProcessHeap(void);
IMPORT void *HeapAlloc(HANDLE heap, DWORD flags, size_t n);
IMPORT BOOL HeapFree(HANDLE heap, DWORD flags, void *p);
IMPORT void *HeapReAlloc(HANDLE heap, DWORD flags, void *p, size_t n);

int _fltused = 0;

/* ------------------------------------------------------------------ memory and strings (volatile stores: the compiler must not turn them into calls of themselves) */

void *memcpy(void *d, const void *s, size_t n) { volatile unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    volatile unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
void *memset(void *d, int c, size_t n) { volatile unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; }
    return 0;
}
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
int atoi(const char *s) {
    int neg = 0, v = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ the heap */

void *malloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void *calloc(size_t n, size_t size) {
    if (size && n > (size_t)-1 / size) return NULL;
    return HeapAlloc(GetProcessHeap(), 8 /* HEAP_ZERO_MEMORY */, n * size ? n * size : 1);
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    return HeapReAlloc(GetProcessHeap(), 0, p, n);
}

static void swap(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
/* qsort: a plain quicksort with the middle element as pivot, insertion sort for short ranges */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    while (n > 8) {
        swap(b, b + (n / 2) * size, size);
        size_t last = 0;
        for (size_t i = 1; i < n; i++)
            if (cmp(b + i * size, b) < 0) swap(b + ++last * size, b + i * size, size);
        swap(b, b + last * size, size);
        if (last < n - last - 1) { qsort(b, last, size, cmp); b += (last + 1) * size; n -= last + 1; }
        else { qsort(b + (last + 1) * size, n - last - 1, size, cmp); n = last; }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--) swap(b + (j - 1) * size, b + j * size, size);
}

/* ------------------------------------------------------------------ output: stdout is buffered (flushed at a newline, when full, and at exit), stderr is not */

struct CrtFile { DWORD which; int buffered; size_t len; char buf[4096]; };
static struct CrtFile g_out = { (DWORD)-11, 1, 0, { 0 } }, g_err = { (DWORD)-12, 0, 0, { 0 } };
FILE *stdout = &g_out, *stderr = &g_err;

int fflush(FILE *f) {
    if (!f) { fflush(stdout); return fflush(stderr); }
    if (f->len) {
        DWORD w;
        WriteFile(GetStdHandle(f->which), f->buf, (DWORD)f->len, &w, NULL);
        f->len = 0;
    }
    return 0;
}
static void put(FILE *f, const char *s, size_t n) {
    if (!f->buffered) { DWORD w; WriteFile(GetStdHandle(f->which), s, (DWORD)n, &w, NULL); return; }
    for (size_t i = 0; i < n; i++) {
        f->buf[f->len++] = s[i];
        if (f->len == sizeof f->buf || s[i] == '\n') fflush(f);
    }
}
int fputs(const char *s, FILE *f) { put(f, s, strlen(s)); return 0; }
int puts(const char *s) { fputs(s, stdout); put(stdout, "\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; put(stdout, &ch, 1); return (unsigned char)c; }

/* vsnprintf: flags '-' '0' (and ' ' '+' '#' accepted), a width (or '*'), a precision for %s and the integers, the lengths hh h l ll z,
 * conversions d i u x X o c s p %. Returns the length the whole output would have, like C99. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t o = 0;
#define OUT(ch) do { char c_ = (ch); if (o + 1 < size) buf[o] = c_; o++; } while (0)       /* ch is evaluated once: it may be rev[--r] */
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { OUT(*p); continue; }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1; else if (*p == '0') zero = 1; else if (*p == '+') plus = 1; else if (*p == ' ') space = 1; else if (*p == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') { p++; prec = 0; if (*p == '*') { prec = va_arg(ap, int); p++; } else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        int len = 0;                                    /* 0 int, 1 long, 2 long long, 3 size_t; hh and h read an int */
        while (*p == 'l' || *p == 'h' || *p == 'z') { if (*p == 'l') len++; else if (*p == 'z') len = 3; p++; }
        char tmp[72];
        const char *s = tmp;
        int n = 0, neg = 0;
        char sign = 0;
        switch (*p) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long v;
            int base = *p == 'x' || *p == 'X' || *p == 'p' ? 16 : *p == 'o' ? 8 : 10;
            if (*p == 'p') { v = (unsigned long long)va_arg(ap, void *); alt = 1; }
            else if (*p == 'd' || *p == 'i') {
                long long sv = len == 2 || len == 3 ? va_arg(ap, long long) : len == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
                if (sv < 0) { neg = 1; v = 0ull - (unsigned long long)sv; } else v = (unsigned long long)sv;
                sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            } else v = len == 2 || len == 3 ? va_arg(ap, unsigned long long) : len == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned);
            const char *dig = *p == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[64];
            int r = 0;
            do { rev[r++] = dig[v % base]; v /= base; } while (v);
            if (prec == 0 && r == 1 && rev[0] == '0' && *p != 'p') r = 0;
            int digits = r < prec ? prec : r;
            const char *pre = alt && base == 16 ? (*p == 'X' ? "0X" : "0x") : alt && base == 8 && rev[r - 1] != '0' ? "0" : "";
            int pl = (int)strlen(pre) + (sign != 0);
            int pad = width > digits + pl ? width - digits - pl : 0;
            if (!left && !(zero && prec < 0)) while (pad > 0) { OUT(' '); pad--; }
            if (sign) OUT(sign);
            for (const char *q = pre; *q; q++) OUT(*q);
            if (!left) while (pad > 0) { OUT('0'); pad--; }
            for (int k = r; k < digits; k++) OUT('0');
            while (r) OUT(rev[--r]);
            while (pad > 0) { OUT(' '); pad--; }
            continue;
        }
        case 'c': tmp[0] = (char)va_arg(ap, int); n = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; while (s[n] && (prec < 0 || n < prec)) n++; break;
        case '%': tmp[0] = '%'; n = 1; break;
        default: tmp[0] = '%'; tmp[1] = *p; n = *p ? 2 : 1; if (!*p) p--; break;
        }
        int pad = width > n ? width - n : 0;
        if (!left) while (pad-- > 0) OUT(' ');
        for (int k = 0; k < n; k++) OUT(s[k]);
        if (left) while (pad-- > 0) OUT(' ');
    }
    if (size) buf[o < size ? o : size - 1] = 0;
    return (int)o;
#undef OUT
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
static int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char small[512];
    va_list ap2;
    __builtin_va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    if (n < (int)sizeof small) put(f, small, (size_t)n);
    else {
        char *big = malloc((size_t)n + 1);
        vsnprintf(big, (size_t)n + 1, fmt, ap2);
        put(f, big, (size_t)n);
        free(big);
    }
    va_end(ap2);
    return n;
}
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(stdout, fmt, ap); va_end(ap); return n; }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }

/* ------------------------------------------------------------------ exit, atexit, abort */

static void (*g_atexit[32])(void);
static int g_natexit;
int atexit(void (*fn)(void)) { if (g_natexit == 32) return -1; g_atexit[g_natexit++] = fn; return 0; }
void exit(int code) {
    while (g_natexit) g_atexit[--g_natexit]();          /* in the reverse order of registration */
    fflush(NULL);
    ExitProcess((DWORD)code);
    for (;;) {}
}
void abort(void) {
    fflush(NULL);
    fputs("abort() called\n", stderr);
    ExitProcess(3);                                     /* the status the Microsoft C runtime documents for abort */
    for (;;) {}
}

/* ------------------------------------------------------------------ the entry point: argv the way Microsoft documents it */

int main(int argc, char **argv);

static int split(const char *cl, char *store, char **argv) {
    int argc = 0;
    const char *p = cl;
    char *o = store;
    /* argv[0]: up to the first space or tab, or inside quotes; no backslash rules */
    while (*p == ' ' || *p == '\t') p++;
    if (*p) {
        if (argv) argv[argc] = o;
        if (*p == '"') { p++; while (*p && *p != '"') *o++ = *p++; if (*p) p++; }
        else while (*p && *p != ' ' && *p != '\t') *o++ = *p++;
        *o++ = 0;
        argc++;
    }
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argv) argv[argc] = o;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            size_t bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (*p == '"') {
                for (size_t i = 0; i < bs / 2; i++) *o++ = '\\';
                if (bs % 2) { *o++ = '"'; p++; }                    /* an escaped quote */
                else if (quoted && p[1] == '"') { *o++ = '"'; p += 2; }   /* "" inside quotes is a literal quote */
                else { quoted = !quoted; p++; }
            } else {
                for (size_t i = 0; i < bs; i++) *o++ = '\\';
                if (*p && (quoted || (*p != ' ' && *p != '\t'))) *o++ = *p++;
            }
        }
        *o++ = 0;
        argc++;
    }
    return argc;
}

void mainCRTStartup(void) {
    const char *cl = GetCommandLineA();
    char *store = malloc(strlen(cl) + 1);
    int argc = split(cl, store, NULL);
    char **argv = malloc(sizeof(char *) * (size_t)(argc + 1));
    split(cl, store, argv);
    argv[argc] = NULL;
    exit(main(argc, argv));
}

/* __chkstk: rax = the size of the frame about to be allocated. Touch one byte in every page between the caller's rsp and rsp - rax, from the top down,
 * so that a stack that grows through a guard page meets it in order. Only r10 and r11 (and the flags) may change. */
__asm__(".text\n.globl __chkstk\n__chkstk:\n"
        " push %r10\n push %r11\n"
        " lea 24(%rsp), %r10\n"                 /* the caller's rsp before its call */
        " mov %rax, %r11\n"
        "1: cmp $0x1000, %r11\n jb 2f\n"
        " sub $0x1000, %r10\n testq %r10, (%r10)\n sub $0x1000, %r11\n jmp 1b\n"
        "2: sub %r11, %r10\n testq %r10, (%r10)\n"
        " pop %r11\n pop %r10\n ret\n");
