/*
 * The C half of the guarded rdmsr (kernel/include/msrsafe.h, msrsafe.asm): putting the handler in front of the #GP stub.
 *
 * The IDT lives in idt.c as a static table, with no call to change a gate afterwards, and trap.c's #GP path ends in a panic. Rather than add
 * a hook to either file, the table is found with SIDT and vector 13's gate is pointed at msrsafe_gp_stub, which keeps the old target to chain
 * to. The gate is only touched if it looks like the interrupt gate idt.c made (present, type 0xE), and the write happens with interrupts off.
 */
#include "msrsafe.h"
#include "kernel.h"
#include "cpu.h"

typedef struct PACKED {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} Gate;

typedef struct PACKED {
    u16 limit;
    u64 base;
} Idtr;

extern char msrsafe_gp_stub[];
extern u64  msrsafe_chain;
int msrsafe_rdmsr(u32 msr, u64 *out);
int msrsafe_poison(void);

#define VEC_GP 13

static bool g_ready;

void msrsafe_init(void) {
    if (g_ready) return;
    Idtr idtr;
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    if (idtr.limit < (VEC_GP + 1) * sizeof(Gate) - 1) return;
    volatile Gate *g = (volatile Gate *)idtr.base + VEC_GP;
    if (!(g->type & 0x80) || (g->type & 0x0F) != 0x0E) {                        /* not the interrupt gate idt.c builds: leave it alone */
        klog("Utsuho", "msrsafe: vector 13 is not the kernel's interrupt gate (type %#x); registers that may not exist will not be read", g->type);
        return;
    }
    u64 old = (u64)g->off_lo | (u64)g->off_mid << 16 | (u64)g->off_hi << 32;
    u64 stub = (u64)(uintptr_t)msrsafe_gp_stub;
    u64 f = irq_save();
    msrsafe_chain = old;
    g->off_lo = (u16)stub;
    g->off_mid = (u16)(stub >> 16);
    g->off_hi = (u32)(stub >> 32);
    __asm__ volatile ("" ::: "memory");
    irq_restore(f);
    g_ready = true;
}

bool msrsafe_ready(void) { return g_ready; }

int msr_read_safe(u32 msr, u64 *out) {
    if (!g_ready) return -1;
    return msrsafe_rdmsr(msr, out);
}

int msrsafe_selftest(void) {
    if (!g_ready) return -1;
    return msrsafe_poison() == 1 ? 0 : -1;
}
