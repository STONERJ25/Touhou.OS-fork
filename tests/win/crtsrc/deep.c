/* The stack: a recursion 5000 calls deep, and a function with a 64 KiB local array (its prologue calls __chkstk), both inside the bounds the TEB
 * names (gs:[0x08] StackBase, gs:[0x10] StackLimit). */
#include "crt.h"

static unsigned long long gs_read(unsigned off) {
    unsigned long long v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((unsigned long long)off));
    return v;
}

static int depth(int n) {
    volatile char pad[64];
    pad[0] = (char)n;
    return n ? depth(n - 1) + pad[0] - (char)n + 1 : 0;
}

__attribute__((noinline)) static unsigned big(int seed) {
    volatile unsigned char a[65536];
    for (unsigned i = 0; i < sizeof a; i += 512) a[i] = (unsigned char)(i + seed);
    unsigned s = 0;
    for (unsigned i = 0; i < sizeof a; i += 512) s += a[i];
    unsigned long long here = (unsigned long long)&a[0];
    if (here < gs_read(0x10) || here >= gs_read(0x08)) return 0;
    return s;
}

int main(void) {
    printf("recursion: %d\n", depth(5000));
    printf("64 KiB frame: %u\n", big(3));
    return 0;
}
