/*
 * The local APIC, only as far as the legacy interrupt controller needs it.
 *
 * This kernel takes its interrupts from the old 8259 PIC (the timer, keyboard, mouse and serial port) and polls
 * everything else, so the only thing the local APIC has to do is let the PIC's wire reach the boot processor: "virtual
 * wire" mode, LINT0 = ExtINT and unmasked. QEMU's firmware leaves it that way; real firmware does not always, and an
 * unmasked-or-not LINT0 is the difference between a machine that boots and one that never sees its first timer tick.
 * If the APIC is globally disabled the PIC is wired straight to the CPU and nothing needs doing.
 *
 * Two more jobs, for machines where the 8254 PIT (the system tick) never delivers an interrupt: the APIC timer can take over
 * as the tick (lapic_timer_start), which needs the end-of-interrupt register and a vector that is not a PIC one.
 * And whatever the firmware left half done in the APIC is cleaned up first: a raised task priority, vectors still marked
 * "in service" and local interrupt sources (thermal, performance counters, error, a timer it armed) that would otherwise fire
 * at vectors this kernel never expects, and, being in service and never acknowledged, block every lower priority interrupt.
 */
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"
#include "acpi.h"

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_X2APIC (1UL << 10)
#define APIC_BASE_ENABLE (1UL << 11)

#define LAPIC_VERSION    0x030
#define LAPIC_TPR        0x080
#define LAPIC_EOI        0x0B0
#define LAPIC_SVR        0x0F0              /* spurious interrupt vector register */
#define LAPIC_ISR        0x100              /* eight 32-bit registers, 0x10 apart */
#define LAPIC_LVT_CMCI   0x2F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_LVT_THERM  0x330
#define LAPIC_LVT_PERF   0x340
#define LAPIC_LINT0      0x350
#define LAPIC_LVT_ERROR  0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define SVR_ENABLE       0x100
#define LINT_EXTINT      0x700              /* delivery mode ExtINT, edge triggered, not masked */
#define LVT_MASKED       0x10000
#define LVT_PERIODIC     0x20000

static volatile u32 *g_lapic;               /* xAPIC register window (NULL in x2APIC mode or without an APIC) */
static bool g_x2apic, g_present;

static u32 lapic_rd(u32 off) { return g_x2apic ? (u32)rdmsr(0x800 + off / 16) : g_lapic[off / 4]; }
static void lapic_wr(u32 off, u32 v) { if (g_x2apic) wrmsr(0x800 + off / 16, v); else g_lapic[off / 4] = v; }

bool lapic_present(void) { return g_present; }

void lapic_eoi(void) { if (g_present) lapic_wr(LAPIC_EOI, 0); }

static void lvt_mask(u32 reg) { lapic_wr(reg, lapic_rd(reg) | LVT_MASKED); }

/*
 * Only the boot processor runs this kernel. The others were left by the firmware spinning or halted in code and stacks that
 * live in firmware memory, memory this kernel hands to its allocator later on: a core still looping in a page that has become
 * somebody's heap executes garbage, and on an Intel chipset a core that triple-faults resets the whole machine, seconds after
 * the screen came up. An INIT to everyone else puts them in the wait-for-SIPI state, where they run nothing and sit still.
 */
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define ICR_INIT_OTHERS 0x000C4500u         /* INIT, level assert, destination shorthand "all excluding self" (SDM 8.4.4.1) */

static void park_other_processors(void) {
    if (acpi_cpu_count() <= 1) return;
    if (g_x2apic) {
        wrmsr(0x800 + LAPIC_ICR_LO / 16, ICR_INIT_OTHERS);
    } else {
        g_lapic[LAPIC_ICR_HI / 4] = 0;
        g_lapic[LAPIC_ICR_LO / 4] = ICR_INIT_OTHERS;
        for (int i = 0; i < 1000 && (g_lapic[LAPIC_ICR_LO / 4] & (1u << 12)); i++) udelay(1);      /* delivery status: pending */
    }
    udelay(200);
    klog("Gensokyo", "local APIC: %d other processor%s sent to wait-for-SIPI (only the boot processor is used)", acpi_cpu_count() - 1,
         acpi_cpu_count() == 2 ? "" : "s");
}

void lapic_virtual_wire(void) {
    u64 base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        klog("Gensokyo", "local APIC: globally disabled by the firmware; the PIC is wired straight to the CPU");
        return;
    }
    g_x2apic = (base & APIC_BASE_X2APIC) != 0;
    if (!g_x2apic) {
        g_lapic = ioremap(base & ~0xFFFUL, 0x1000, PTE_PCD);
        if (!g_lapic) { klog("Gensokyo", "local APIC: cannot map its registers at %#lx", (unsigned long)(base & ~0xFFFUL)); return; }
    }
    g_present = true;

    u32 maxlvt = (lapic_rd(LAPIC_VERSION) >> 16) & 0xFF;
    lapic_wr(LAPIC_TPR, 0);                                          /* a raised task priority would hold back every interrupt below it */
    u32 svr = lapic_rd(LAPIC_SVR);
    lapic_wr(LAPIC_SVR, (svr & ~0xFFu) | SVR_ENABLE | 0xFF);        /* enabled, spurious vector 0xFF (trap.c knows it needs no EOI) */

    /* Interrupts the firmware took and never finished: every set bit in the in-service registers gets its end-of-interrupt. */
    int stuck = 0;
    for (int r = 7; r >= 0; r--) {
        for (int guard = 0; guard < 64 && lapic_rd(LAPIC_ISR + 0x10 * (u32)r); guard++) { lapic_wr(LAPIC_EOI, 0); stuck++; }
    }

    /* Local interrupt sources this kernel does not use stay quiet. LINT0 becomes the PIC's wire; LINT1 (NMI) is left alone. */
    lvt_mask(LAPIC_LVT_TIMER);
    lvt_mask(LAPIC_LVT_ERROR);
    if (maxlvt >= 4) lvt_mask(LAPIC_LVT_PERF);
    if (maxlvt >= 5) lvt_mask(LAPIC_LVT_THERM);
    if (maxlvt >= 6) lvt_mask(LAPIC_LVT_CMCI);
    lapic_wr(LAPIC_TIMER_INIT, 0);

    u32 lint0 = lapic_rd(LAPIC_LINT0);
    if ((lint0 & 0x107FF) != LINT_EXTINT) lapic_wr(LAPIC_LINT0, LINT_EXTINT);
    klog("Gensokyo", "local APIC: %s mode, LINT0 was %#x, now ExtINT (the PIC reaches the boot processor)%s", g_x2apic ? "x2APIC" : "xAPIC", lint0,
         stuck ? " (the firmware had left interrupts in service; they were acknowledged)" : "");
    park_other_processors();
}

/*
 * Takes over the system tick when the PIT's interrupt never arrives. The timer counts the bus clock / 16; its rate is measured
 * against the TSC (udelay) for 10 ms, then it runs periodic at `hz` on `vector`. False if there is no APIC or the count is
 * implausible.
 */
bool lapic_timer_start(u32 vector, u32 hz) {
    if (!g_present || hz == 0 || hz > 10000) return false;
    lapic_wr(LAPIC_TIMER_DIV, 0x3);                                  /* divide by 16 */
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | vector);                  /* one-shot, masked: only counting */
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    udelay(10000);
    u32 left = lapic_rd(LAPIC_TIMER_CUR);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    u64 per_10ms = 0xFFFFFFFFull - left;
    u64 count = per_10ms * 100 / hz;                                 /* counts per tick */
    if (per_10ms < 1000 || count == 0 || count > 0xFFFFFFFFull) return false;
    lapic_wr(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
    lapic_wr(LAPIC_TIMER_INIT, (u32)count);
    klog("Sakuya", "local APIC timer: %lu counts per 10 ms, ticking every %lu counts", (unsigned long)per_10ms, (unsigned long)count);
    return true;
}
