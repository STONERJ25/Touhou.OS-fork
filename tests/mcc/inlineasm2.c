/* Inline assembly, second part: SSE moves and integer operations on %xmm registers (the copy loops of the compositor), and
 * x87 instructions with the "t" / "u" constraints (what user/libc/src/math.c does).  Portable GNU C: gcc builds the
 * same file and prints the same text, which is where the .out file comes from. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- x87: operands in st(0) and st(1) */
static double x87_sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double x87_cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double x87_tan(double x) { double r; __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
static double x87_atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static double x87_f2xm1(double x) { double r; __asm__("f2xm1" : "=t"(r) : "0"(x)); return r; }
static double x87_rint(double x) { double r; __asm__("frndint" : "=t"(r) : "0"(x)); return r; }
static double x87_scale(double x, double n) { double r; __asm__("fscale" : "=t"(r) : "0"(x), "u"(n)); return r; }   /* st(1) stays: gcc pops it itself */
static double x87_log2(double x) { double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double x87_ln(double x) { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double x87_log10(double x) { double r; __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double x87_sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static double x87_fmod(double x, double y) {
    double r;
    __asm__("1: fprem; fnstsw %%ax; sahf; jp 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}
static float x87_sqrtf(float x) { float r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }

/* a result stored through a pointer, and inputs that are expressions with calls in them */
static double twice(double v) { return v * 2; }
static void x87_store(double *out, double a, double b) { __asm__("fscale" : "=t"(*out) : "0"(twice(a)), "u"(b)); }

/* the x87 stack is empty again after every statement: sixteen in a row would overflow it otherwise */
static double x87_many(double x) {
    double s = 0;
    for (int i = 0; i < 16; i++) s += x87_scale(x, (double)i);
    return s;
}

/* ---- SSE */
static void copy64(void *dst, const void *src) {
    __asm__ volatile (
        "movdqu    (%1), %%xmm0\n\t"
        "movdqu  16(%1), %%xmm1\n\t"
        "movdqu  32(%1), %%xmm2\n\t"
        "movdqu  48(%1), %%xmm3\n\t"
        "movntdq %%xmm0,   (%0)\n\t"
        "movntdq %%xmm1, 16(%0)\n\t"
        "movntdq %%xmm2, 32(%0)\n\t"
        "movntdq %%xmm3, 48(%0)\n\t"
        : : "r"(dst), "r"(src) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
}
static void copy16a(void *dst, const void *src) {
    __asm__ volatile ("movdqa (%1), %%xmm7\n\tmovdqa %%xmm7, (%0)" : : "r"(dst), "r"(src) : "xmm7", "memory");
}
static uint32_t padd32(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("movd %1, %%xmm0\n\tmovd %2, %%xmm1\n\tpaddd %%xmm1, %%xmm0\n\tmovd %%xmm0, %0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}
static uint64_t pxor64(uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__("movq %1, %%xmm8\n\tmovq %2, %%xmm9\n\tpxor %%xmm9, %%xmm8\n\tmovq %%xmm8, %0" : "=r"(r) : "r"(a), "r"(b) : "xmm8", "xmm9");
    return r;
}
static void shuffle(uint32_t *out, const uint32_t *in) {
    __asm__ volatile ("movdqu (%1), %%xmm2\n\tpshufd $0x1b, %%xmm2, %%xmm3\n\tmovdqu %%xmm3, (%0)" : : "r"(out), "r"(in) : "xmm2", "xmm3", "memory");
}
static void fill_eq(unsigned char *out, const unsigned char *a, const unsigned char *b) {
    __asm__ volatile ("movdqu (%1), %%xmm0\n\tmovdqu (%2), %%xmm1\n\tpcmpeqb %%xmm1, %%xmm0\n\tmovdqu %%xmm0, (%0)" : : "r"(out), "r"(a), "r"(b) : "xmm0", "xmm1", "memory");
}

int main(void) {
    const double xs[] = {0.5, 1.0, 2.0, 10.0, -0.75};
    for (int i = 0; i < 5; i++) {
        double x = xs[i];
        printf("x=%g sin=%.9g cos=%.9g tan=%.9g atan2=%.9g sqrt=%.9g rint=%.9g\n", x, x87_sin(x), x87_cos(x), x87_tan(x), x87_atan2(x, 2.0),
               x > 0 ? x87_sqrt(x) : 0.0, x87_rint(x * 3.5));
        if (x > 0) printf("  log2=%.9g ln=%.9g log10=%.9g\n", x87_log2(x), x87_ln(x), x87_log10(x));
        printf("  f2xm1=%.9g scale=%.9g fmod=%.9g\n", x87_f2xm1(x / 16), x87_scale(x, 3.0), x87_fmod(x * 7, 1.5));
    }
    double st;
    x87_store(&st, 1.5, 2.0);
    printf("store %g many %.3f sqrtf %g\n", st, x87_many(0.25), (double)x87_sqrtf(2.25f));

    _Alignas(16) uint32_t src[16], dst[16];
    for (int i = 0; i < 16; i++) src[i] = 0x01010101u * (unsigned)(i + 1) + (unsigned)i;
    memset(dst, 0, sizeof dst);
    copy64(dst, src);
    printf("copy64 %s first %08x last %08x\n", memcmp(dst, src, 64) == 0 ? "ok" : "BAD", dst[0], dst[15]);
    memset(dst, 0, sizeof dst);
    copy16a(dst + 4, src + 8);
    printf("copy16a %08x %08x %08x %08x around %08x %08x\n", dst[4], dst[5], dst[6], dst[7], dst[3], dst[8]);
    printf("padd32 %08x %08x\n", padd32(0xFFFFFFF0u, 0x20u), padd32(7, 9));
    printf("pxor64 %016llx\n", (unsigned long long)pxor64(0x123456789ABCDEF0ull, 0xFFFF0000FFFF0000ull));
    uint32_t sh[4], in[4] = {1, 2, 3, 4};
    shuffle(sh, in);
    printf("shuffle %u %u %u %u\n", sh[0], sh[1], sh[2], sh[3]);
    unsigned char a[16], b[16], e[16];
    for (int i = 0; i < 16; i++) {
        a[i] = (unsigned char)i;
        b[i] = (unsigned char)(i % 3 ? i : 99);
    }
    fill_eq(e, a, b);
    for (int i = 0; i < 16; i++) printf("%02x", e[i]);
    printf("\n");
    return 0;
}
