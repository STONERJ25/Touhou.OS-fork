/* 8254 PIT as the system tick, plus TSC calibration for microsecond delays. */
#include "irq.h"
#include "kernel.h"
#include "cpu.h"
#include "task.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182UL

static volatile u64 g_ticks;
static u64 g_tsc_per_us = 2000;         /* refined by calibration */

u64 timer_ticks(void) { return g_ticks; }
u64 uptime_ms(void) { return g_ticks * 1000 / TIMER_HZ; }

static void timer_irq(int irq, void *ctx) {
    (void)irq; (void)ctx;
    g_ticks++;
    sched_tick();
}

/* Measure the TSC against the PIT's own counter (channel 2 one-shot gate would also work; this is simpler). */
static void calibrate_tsc(void) {
    /* Use channel 2 in mode 0 with the speaker gate: count down 20 ms worth of PIT ticks. */
    u16 count = (u16)(PIT_FREQ / 50);
    u8 gate = inb(0x61);
    outb(0x61, (gate & ~0x02) | 0x01);              /* gate on, speaker off */
    outb(PIT_CMD, 0xB0);                            /* ch2, lobyte/hibyte, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    u64 t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) { }                 /* OUT2 goes high at terminal count */
    u64 t1 = rdtsc();
    outb(0x61, gate);
    u64 per_us = (t1 - t0) / 20000;
    if (per_us >= 100) g_tsc_per_us = per_us;
}

void timer_init(void) {
    calibrate_tsc();
    u32 divisor = (u32)(PIT_FREQ / TIMER_HZ);
    outb(PIT_CMD, 0x34);                            /* ch0, lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);
    irq_register(0, timer_irq, NULL);
    irq_unmask(0);
    klog("Sakuya", "PIT online at %d Hz, TSC ~%lu MHz", TIMER_HZ, (unsigned long)g_tsc_per_us);
}

void udelay(u32 us) {
    u64 end = rdtsc() + (u64)us * g_tsc_per_us;
    while (rdtsc() < end) pause();
}
