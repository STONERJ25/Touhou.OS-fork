/* memcpy, memmove and memset with constant sizes (which mcc expands in line) and run-time sizes (library calls) must agree,
   unaligned loads written as memcpy, return values, and identical string literals. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static unsigned char src[160], dst[160], buf[160];

static void fill(void) {
    for (int i = 0; i < 160; i++) {
        src[i] = (unsigned char)(i * 37 + 11);
        dst[i] = (unsigned char)(255 - i);
        buf[i] = (unsigned char)(i * 5 + 1);
    }
}

static uint32_t sum(const unsigned char *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

#define ONE(M, b) M((b) + 0); M((b) + 1); M((b) + 2); M((b) + 3); M((b) + 4); M((b) + 5); M((b) + 6); M((b) + 7); M((b) + 8); M((b) + 9);
#define ALL70(M) ONE(M, 0) ONE(M, 10) ONE(M, 20) ONE(M, 30) ONE(M, 40) ONE(M, 50) ONE(M, 60) M(64); M(65); M(66); M(70);
#define ALL40(M) ONE(M, 0) ONE(M, 10) ONE(M, 20) ONE(M, 30) M(32); M(33); M(40);

#define CPY(n)                                                                                                         \
    do {                                                                                                               \
        fill();                                                                                                        \
        void *r = memcpy(dst + 3, src + 5, n);                                                                         \
        printf("cpy %d %d %08x\n", n, r == dst + 3, (unsigned)sum(dst, 160));                                          \
    } while (0)

#define MOVF(n)                                                                                                        \
    do {                                                                                                               \
        fill();                                                                                                        \
        void *r = memmove(buf + 4, buf + 1, n);                                                                        \
        printf("movf %d %d %08x\n", n, r == buf + 4, (unsigned)sum(buf, 160));                                         \
    } while (0)

#define MOVB(n)                                                                                                        \
    do {                                                                                                               \
        fill();                                                                                                        \
        void *r = memmove(buf + 1, buf + 4, n);                                                                        \
        printf("movb %d %d %08x\n", n, r == buf + 1, (unsigned)sum(buf, 160));                                         \
    } while (0)

#define SET(n)                                                                                                         \
    do {                                                                                                               \
        fill();                                                                                                        \
        void *r = memset(dst + 1, 0xA5, n);                                                                            \
        memset(buf + 2, 0, n);                                                                                         \
        memset(src, -1, n);                                                                                            \
        printf("set %d %d %08x %08x %08x\n", n, r == dst + 1, (unsigned)sum(dst, 160), (unsigned)sum(buf, 160),        \
               (unsigned)sum(src, 160));                                                                               \
    } while (0)

/* the same operations with sizes only known at run time: reference results */
static void run_time(void) {
    for (int n = 0; n <= 70; n += (n < 12 ? 1 : 9)) {
        volatile int vn = n;
        fill();
        memcpy(dst + 3, src + 5, (size_t)vn);
        uint32_t a = sum(dst, 160);
        fill();
        memmove(buf + 4, buf + 1, (size_t)vn);
        uint32_t b = sum(buf, 160);
        fill();
        memmove(buf + 1, buf + 4, (size_t)vn);
        uint32_t c = sum(buf, 160);
        fill();
        memset(dst + 1, 0xA5, (size_t)vn);
        uint32_t d = sum(dst, 160);
        printf("runtime %d %08x %08x %08x %08x\n", n, (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
    }
}

struct S {
    int a;
    char b[13];
    double c;
    short d[5];
};

static int same(const char *a, const char *b) { return a == b; }

int main(void) {
    ALL70(CPY)
    ALL40(MOVF)
    ALL40(MOVB)
    ALL70(SET)
    run_time();

    /* unaligned loads and stores spelled as memcpy */
    fill();
    uint64_t v64;
    uint32_t v32;
    uint16_t v16;
    memcpy(&v64, src + 1, 8);
    memcpy(&v32, src + 3, 4);
    memcpy(&v16, src + 7, 2);
    printf("loads %016llx %08x %04x\n", (unsigned long long)v64, (unsigned)v32, (unsigned)v16);
    uint64_t w = 0x1122334455667788ULL;
    memcpy(dst + 5, &w, sizeof w);
    printf("store %08x\n", (unsigned)sum(dst, 160));

    /* structs, string literals, the pointer arguments with side effects */
    struct S s1 = {7, "twelve chars", 2.5, {1, 2, 3, 4, 5}}, s2;
    memset(&s2, 0, sizeof s2);
    memcpy(&s2, &s1, sizeof s1);
    printf("struct %d %s %.1f %d\n", s2.a, s2.b, s2.c, s2.d[4]);
    char text[32];
    memcpy(text, "hello, gensokyo", 16);
    printf("text %s\n", text);
    unsigned char *p = dst, *q = src;
    memcpy(p++, q++, 4);
    memcpy(p++, q++, 4);
    printf("walk %d %d %08x\n", (int)(p - dst), (int)(q - src), (unsigned)sum(dst, 16));

    printf("same literal %d %d\n", same("abc", "abc"), same("abc", "abd"));
    printf("same empty %d\n", same("", ""));
    return 0;
}
