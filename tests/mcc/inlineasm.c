/* GNU extended inline assembly: constraints, operands, labels, memory, string instructions (portable x86-64) */
#include <stdio.h>
#include <string.h>

static long add_asm(long a, long b) {
    long r = a;
    __asm__("add %1, %0" : "+r"(r) : "r"(b));
    return r;
}

static int swap32(int v) {
    __asm__("bswap %0" : "+r"(v));
    return v;
}

static long mul3(long x) {
    long r;
    __asm__("lea (%1,%1,2), %0" : "=r"(r) : "r"(x));
    return r;
}

static void my_memcpy(void *dst, const void *src, unsigned long n) {
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void my_memset(void *dst, int c, unsigned long n) {
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
}

static void backward_copy(char *dst, const char *src, unsigned long n) {
    __asm__ volatile("std; rep movsb; cld" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static int loop_sum(int n) {
    int total = 0, i = 1;
    __asm__ volatile(
        "1: add %[i], %[t]\n\t"
        "inc %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jle 1b"
        : [t] "+r"(total), [i] "+r"(i)
        : [n] "r"(n)
        : "cc");
    return total;
}

static int max_cmov(int a, int b) {
    int r = a;
    __asm__("cmp %1, %0; cmovl %1, %0" : "+r"(r) : "r"(b) : "cc");
    return r;
}

static int is_zero(long v) {
    unsigned char z;
    __asm__("test %1, %1; setz %0" : "=q"(z) : "r"(v) : "cc");
    return z;
}

static long xadd(long *p, long v) {
    __asm__ volatile("lock xadd %0, %1" : "+r"(v), "+m"(*p) : : "memory", "cc");
    return v;
}

static unsigned long long shl_cl(unsigned long long v, unsigned s) {
    __asm__("shl %%cl, %0" : "+r"(v) : "c"(s));
    return v;
}

static int bsf_bsr(unsigned v, int *hi) {
    int lo;
    __asm__("bsf %1, %0" : "=r"(lo) : "r"(v));
    __asm__("bsr %1, %0" : "=r"(*hi) : "r"(v));
    return lo;
}

static long imm_operand(long x) {
    long r;
    __asm__("mov %1, %0; add %2, %0" : "=r"(r) : "r"(x), "i"(1000));
    return r;
}

static int cpuid_has_vendor(void) {
    unsigned a = 0, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b != 0 || c != 0 || d != 0;
}

static unsigned long long tsc(void) {
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

int main(void) {
    printf("add_asm=%ld swap32=%x mul3=%ld\n", add_asm(40, 2), (unsigned)swap32(0x11223344), mul3(14));
    char buf[16] = "abcdefghijklmno";
    char out[16];
    my_memset(out, '.', 15);
    out[15] = 0;
    my_memcpy(out + 2, buf + 3, 5);
    printf("memcpy/memset: %s\n", out);
    char ov[12] = "0123456789";
    backward_copy(ov + 6, ov + 3, 4);
    printf("backward_copy: %s\n", ov);
    printf("loop_sum(10)=%d max_cmov=%d,%d is_zero=%d,%d\n", loop_sum(10), max_cmov(3, 9), max_cmov(9, 3), is_zero(0), is_zero(5));
    long cell = 40;
    long old = xadd(&cell, 2);
    printf("xadd: old=%ld cell=%ld\n", old, cell);
    printf("shl_cl=%llu\n", shl_cl(3, 40));
    int hi;
    int lo = bsf_bsr(0x00f0u, &hi);
    printf("bsf/bsr: %d %d\n", lo, hi);
    printf("imm_operand=%ld\n", imm_operand(5));
    unsigned long long t1 = tsc(), t2 = tsc();
    printf("cpuid ok=%d tsc monotonic=%d\n", cpuid_has_vendor(), t2 >= t1);
    __asm__ volatile("" ::: "memory");
    __asm__ volatile("nop; pause; mfence");
    return 0;
}
