#include "arch.h"
#include "kernel.h"
#include "task.h"
#include "mm.h"

extern char __kernel_start[], __kernel_end[];

/* Only chase frame pointers into memory a frame can live in: the kernel image (boot and interrupt stacks are there), the running task's
 * kernel stack, and RAM through the direct map. Every read goes through kprobe_read64, so even a pointer that passes this test and
 * is not mapped (a hole between the RAM regions) ends the walk instead of causing a second fault. */
static bool frame_ok(u64 p) {
    if (p & 7) return false;
    if (p >= (u64)__kernel_start && p + 16 <= (u64)__kernel_end) return true;
    Task *t = current;
    if (t && t->kstack && p >= (u64)t->kstack && p + 16 <= t->kstack_top) return true;
    return p >= g_hhdm && p + 16 <= g_hhdm + pmm_ram_top();
}

static bool read_frame(u64 rbp, u64 *next, u64 *ret) {
    return frame_ok(rbp) && kprobe_read64((const u64 *)rbp, next) == 0 && kprobe_read64((const u64 *)rbp + 1, ret) == 0;
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
    for (int i = 1; i < 24; i++) {
        u64 next, ret;
        if (!read_frame(rbp, &next, &ret) || !ret) break;
        print_frame(i, ret);
        if (next <= rbp) break;
        rbp = next;
    }
}

/* The return addresses of the callers above the caller of this function: where an object was allocated, for the ledgers. */
int backtrace_capture(u64 *out, int n) {
    u64 rbp = (u64)__builtin_frame_address(0);
    int got = 0;
    while (got < n) {
        u64 next, ret;
        if (!read_frame(rbp, &next, &ret) || !ret) break;
        out[got++] = ret;
        if (next <= rbp) break;
        rbp = next;
    }
    for (int i = got; i < n; i++) out[i] = 0;
    return got;
}
