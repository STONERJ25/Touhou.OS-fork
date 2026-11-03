#include "arch.h"
#include "kernel.h"

extern char __kernel_start[], __kernel_end[];

/* Only chase frame pointers that point into memory we know is mapped (kernel image or the direct map). */
static bool frame_ok(u64 p) {
    if (p & 7) return false;
    if (p >= (u64)__kernel_start && p + 16 <= (u64)__kernel_end) return true;
    return p >= g_hhdm && p + 16 <= g_hhdm + (4ULL << 30);
}

static void print_frame(int n, u64 addr) {
    u64 off = 0;
    const char *s = ksym_lookup(addr, &off);
    if (s) printk("  #%d %016lx %s+%#lx\n", n, addr, s, off);
    else   printk("  #%d %016lx\n", n, addr);
}

void dump_backtrace(u64 rip, u64 rbp) {
    printk("backtrace:\n");
    print_frame(0, rip);
    for (int i = 1; i < 24 && frame_ok(rbp); i++) {
        const u64 *fp = (const u64 *)rbp;
        u64 ret = fp[1], next = fp[0];
        if (!ret) break;
        print_frame(i, ret);
        if (next <= rbp) break;
        rbp = next;
    }
}
