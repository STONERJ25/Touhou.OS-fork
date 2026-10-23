/*
 * The local APIC, only as far as the legacy interrupt controller needs it.
 *
 * This kernel takes its interrupts from the old 8259 PIC (the timer, keyboard, mouse and serial port) and polls
 * everything else, so the only thing the local APIC has to do is let the PIC's wire reach the boot processor: "virtual
 * wire" mode, LINT0 = ExtINT and unmasked. QEMU's firmware leaves it that way; real firmware does not always, and an
 * unmasked-or-not LINT0 is the difference between a machine that boots and one that never sees its first timer tick.
 * If the APIC is globally disabled the PIC is wired straight to the CPU and nothing needs doing.
 */
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "irq.h"

#define IA32_APIC_BASE   0x1B
#define APIC_BASE_X2APIC (1UL << 10)
#define APIC_BASE_ENABLE (1UL << 11)

#define LAPIC_SVR        0x0F0              /* spurious interrupt vector register */
#define LAPIC_LINT0      0x350
#define SVR_ENABLE       0x100
#define LINT_EXTINT      0x700              /* delivery mode ExtINT, edge triggered, not masked */

void lapic_virtual_wire(void) {
    u64 base = rdmsr(IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) return;
    u32 lint0;
    if (base & APIC_BASE_X2APIC) {                                   /* x2APIC: the registers are MSRs at 0x800 + offset / 16 */
        u64 svr = rdmsr(0x800 + LAPIC_SVR / 16);
        if (!(svr & SVR_ENABLE)) wrmsr(0x800 + LAPIC_SVR / 16, svr | SVR_ENABLE | 0xFF);
        lint0 = (u32)rdmsr(0x800 + LAPIC_LINT0 / 16);
        if ((lint0 & 0x107FF) != LINT_EXTINT) wrmsr(0x800 + LAPIC_LINT0 / 16, LINT_EXTINT);
    } else {
        volatile u32 *l = ioremap(base & ~0xFFFUL, 0x1000, PTE_PCD);
        u32 svr = l[LAPIC_SVR / 4];
        if (!(svr & SVR_ENABLE)) l[LAPIC_SVR / 4] = svr | SVR_ENABLE | 0xFF;
        lint0 = l[LAPIC_LINT0 / 4];
        if ((lint0 & 0x107FF) != LINT_EXTINT) l[LAPIC_LINT0 / 4] = LINT_EXTINT;
    }
    klog("Gensokyo", "local APIC: %s mode, LINT0 was %#x, now ExtINT (the PIC reaches the boot processor)",
         base & APIC_BASE_X2APIC ? "x2APIC" : "xAPIC", lint0);
}
