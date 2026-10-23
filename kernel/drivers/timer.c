/*
 * The system tick and the TSC clock.
 *
 * The tick is the 8254 PIT (IRQ0 through the 8259 PIC). The TSC rate is measured against the PIT's own counter (channel 2),
 * with the ACPI power-management timer as a second reference and the "@ 3.40GHz" in the CPU's brand string as a third; none
 * of them may be allowed to hang the boot, because on a PC there is no way to see where it stopped: every wait here is bounded.
 *
 * If the PIT's interrupt never reaches the CPU (a chipset without the legacy timer, an interrupt route the firmware left
 * different from what the 8259 expects) the boot would freeze at the first sleep. timer_verify() notices, and the local APIC
 * timer takes over as the tick. QEMU's `-machine q35,pit=off` is that situation.
 */
#include "irq.h"
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "task.h"
#include "input.h"
#include "acpi.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182UL
#define ACPI_TIMER_HZ 3579545ULL
#define SPIN_LIMIT 3000000              /* port reads before a calibration gives up (each is about a microsecond on a PC) */

static volatile u64 g_ticks;
static u64 g_tsc_per_us = 2000;         /* MHz; refined by calibration */
static const char *g_tsc_how = "assumed";
static bool g_tick_lapic;               /* the local APIC timer is the tick (the PIT never answered) */

u64 timer_ticks(void) { return g_ticks; }
u64 uptime_ms(void) { return g_ticks * 1000 / TIMER_HZ; }

static void tick(void) {
    g_ticks++;
    input_tick();
    sched_tick();
}

static void timer_irq(int irq, void *ctx) {
    (void)irq; (void)ctx;
    tick();
}

/* Entry from trap.c for LAPIC_TIMER_VECTOR. The end-of-interrupt comes first: the handler may end up switching tasks. */
void lapic_timer_irq(void) {
    lapic_eoi();
    tick();
}

/* Measure the TSC against the PIT's own counter: channel 2 in mode 0 with the speaker gate, 20 ms. Returns MHz, or 0 if the PIT
 * is missing (OUT2 never rises) or the answer is nonsense (OUT2 already high: no 8254 behind the port). */
static u32 tsc_mhz_pit(void) {
    u16 count = (u16)(PIT_FREQ / 50);
    u8 gate = inb(0x61);
    outb(0x61, (gate & ~0x02) | 0x01);              /* gate on, speaker off */
    outb(PIT_CMD, 0xB0);                            /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    u64 t0 = rdtsc();
    int spins = 0;
    while (!(inb(0x61) & 0x20) && spins < SPIN_LIMIT) spins++;     /* OUT2 goes high at terminal count */
    u64 t1 = rdtsc();
    outb(0x61, gate);
    if (spins >= SPIN_LIMIT) return 0;
    u64 mhz = (t1 - t0) / 20000;
    return mhz >= 100 && mhz <= 20000 ? (u32)mhz : 0;
}

/* The ACPI power-management timer: 3.579545 MHz, 24 or 32 bits, present on every ACPI PC. Read for 20 ms. */
static u32 tsc_mhz_acpi_timer(void) {
    u16 port;
    bool wide;
    if (!acpi_pm_timer(&port, &wide)) return 0;
    u32 mask = wide ? 0xFFFFFFFFu : 0x00FFFFFFu;
    u32 start = inl(port) & mask, now = start;
    u64 want = ACPI_TIMER_HZ / 50;
    u64 t0 = rdtsc();
    int spins = 0;
    while (((now - start) & mask) < want && spins < SPIN_LIMIT) { now = inl(port) & mask; spins++; }
    u64 t1 = rdtsc();
    u64 ticks = (now - start) & mask;
    if (spins >= SPIN_LIMIT || ticks < want) return 0;
    u64 mhz = (t1 - t0) * ACPI_TIMER_HZ / ticks / 1000000ULL;
    return mhz >= 100 && mhz <= 20000 ? (u32)mhz : 0;
}

static bool close_enough(u32 a, u32 b) {            /* within 25 % */
    u32 d = a > b ? a - b : b - a;
    return (u64)d * 4 <= b;
}

static void calibrate_tsc(void) {
    u32 brand = g_cpu.brand_mhz;
    u32 pit = tsc_mhz_pit(), acpi = 0, mhz = 0;
    const char *how = NULL;
    if (pit && (!brand || close_enough(pit, brand))) {
        mhz = pit; how = "PIT";
    } else {
        acpi = tsc_mhz_acpi_timer();
        if (acpi && (!brand || close_enough(acpi, brand))) { mhz = acpi; how = "ACPI timer"; }
        else if (brand) { mhz = brand; how = "CPU brand string"; }
        else if (pit) { mhz = pit; how = "PIT"; }
        else if (acpi) { mhz = acpi; how = "ACPI timer"; }
    }
    if (pit != mhz || acpi || brand)
        klog("Sakuya", "TSC references: PIT %u MHz, ACPI timer %u MHz, brand string %u MHz (0 = unavailable)", pit, acpi, brand);
    if (mhz) { g_tsc_per_us = mhz; g_tsc_how = how; }
}

void timer_init(void) {
    calibrate_tsc();
    u32 divisor = (u32)(PIT_FREQ / TIMER_HZ);
    outb(PIT_CMD, 0x34);                            /* ch0, lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);
    irq_register(0, timer_irq, NULL);
    irq_unmask(0);
    klog("Sakuya", "PIT online at %d Hz, TSC ~%lu MHz (%s)", TIMER_HZ, (unsigned long)g_tsc_per_us, g_tsc_how);
}

/* Called once with interrupts enabled: did a tick arrive? If not the PIT's interrupt is not reaching the CPU and the local APIC
 * timer replaces it. A kernel that cannot sleep cannot boot, and a PC gives no other sign of why it froze. */
void timer_verify(void) {
    u64 t0 = g_ticks;
    for (int i = 0; i < 400 && g_ticks == t0; i++) udelay(500);         /* 200 ms: a tick every millisecond is expected */
    if (g_ticks != t0) return;
    klog("Sakuya", "no timer interrupt arrived in 200 ms: the PIT's IRQ0 is not reaching the CPU (no 8254, or the firmware's interrupt routing)");
    irq_mask(0);
    if (!lapic_timer_start(LAPIC_TIMER_VECTOR, TIMER_HZ)) {
        klog("Sakuya", "and there is no usable local APIC timer either: nothing can ever wake a sleeping task, the boot will stop here");
        return;
    }
    t0 = g_ticks;
    for (int i = 0; i < 400 && g_ticks == t0; i++) udelay(500);
    if (g_ticks == t0) {
        klog("Sakuya", "the local APIC timer does not tick either: nothing can ever wake a sleeping task, the boot will stop here");
        return;
    }
    g_tick_lapic = true;
    klog("Sakuya", "the local APIC timer is the system tick now");
}

bool timer_is_lapic(void) { return g_tick_lapic; }

void udelay(u32 us) {
    u64 end = rdtsc() + (u64)us * g_tsc_per_us;
    while (rdtsc() < end) pause();
}
