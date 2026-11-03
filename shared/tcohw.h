/*
 * The Intel TCO watchdog: where it is and how it is driven. Shared by the loader (boot/recovery.c arms it just before the kernel starts)
 * and the kernel (kernel/drivers/tcowdt.c feeds it), so that both find the same registers the same way. Header-only: each side compiles it
 * with its own ABI (the loader is PE/COFF with the Microsoft ABI, the kernel SysV ELF).
 *
 * Written from Intel's public datasheets: the I/O Controller Hub 9 (ICH9) family datasheet (the TCO I/O registers, the LPC bridge's PMBASE
 * and RCBA, the chipset configuration register GCS) and the 100/200-series PCH datasheets (TCOBASE in the SMBus controller, ABASE in the
 * PMC, the PCH private configuration space behind the P2SB bridge).
 *
 * The timer: TCO_TMR holds a count of 0.6 s ticks (10 bits). A write to TCO_RLD reloads the counter from it. When it reaches zero the
 * first time, TCO1_STS.TIMEOUT is set (and an SMI raised if SMI_EN.TCO_EN is set) and it starts again; when it reaches zero the second
 * time TCO2_STS.SECOND_TO_STS is set and the chipset resets the platform, unless the NO_REBOOT bit is set. So the time from the last
 * reload to the reset is two countdowns: tcohw_ticks_for() halves the timeout it is given. SECOND_TO_STS lives in the resume well and
 * survives the reset it caused, which is how the next start knows ("the previous start ended in a watchdog reset"), unless the
 * firmware clears it on the way.
 *
 * Where the registers are, by generation:
 *   ICH6 to the 9-series PCH (and QEMU's q35, an ICH9): the TCO block is 0x60 bytes into the ACPI I/O block, whose base PMBASE is in the
 *     LPC bridge (00:1f.0, config 0x40, bits 15:7; bit 0 always 1). NO_REBOOT is bit 5 of GCS, a memory-mapped register at
 *     RCBA + 0x3410 (RCBA: LPC config 0xf0, bits 31:14, bit 0 enables the window).
 *   100- and 200-series PCH (Sunrise Point, Union Point: the Dell OptiPlex 7050's Q270 is a Union Point): the TCO block has a base of
 *     its own, TCOBASE in the SMBus controller (00:1f.4, config 0x50, bits 15:5), decoded while TCOCTL (config 0x54) bit 8 is set; the
 *     ACPI base ABASE moved to the PMC (00:1f.2, config 0x40). NO_REBOOT is bit 1 of the SMBus controller's General Control register in
 *     the PCH private configuration space: SBREG_BAR (the P2SB bridge 00:1f.1, config 0x10; the firmware hides that device, and bit 8 of
 *     its P2SBC register, config 0xe0, unhides it) + (port 0xc6 << 16) + 0x0c.
 *   300-series and later moved NO_REBOOT again; such a chipset is found by the second rule, and NO_REBOOT is then reported as unknown.
 * The firmware or a pin strap may keep NO_REBOOT set; then the timer runs out and nothing happens. tcohw_noreboot_clear() says whether
 * the bit stays set; the kernel also watches for a second timeout that did not reset the machine.
 */
#ifndef SHARED_TCOHW_H
#define SHARED_TCOHW_H

#include <stdint.h>

/* ---- the TCO I/O registers, relative to the TCO base ---- */
#define TCO_RLD         0x00            /* 16: any write reloads the counter from TCO_TMR */
#define TCO1_STS        0x04            /* 16 */
#define TCO2_STS        0x06            /* 16 */
#define TCO1_CNT        0x08            /* 16 */
#define TCO_TMR         0x12            /* 16: bits 9:0, the count in 0.6 s ticks */

#define TCO1_STS_TIMEOUT   0x0008u      /* the counter reached zero once; write 1 to clear */
#define TCO2_STS_SECOND_TO 0x0002u      /* it reached zero a second time: the platform reset (unless NO_REBOOT); survives that reset; write 1 to clear */
#define TCO2_STS_BOOT      0x0004u      /* the second timeout came before the processor fetched its first instruction */
#define TCO1_CNT_TMR_HLT   0x0800u      /* 1: the counter is halted */
#define TCO1_CNT_LOCK      0x1000u      /* SMI_EN.TCO_EN is locked until the next reset; writing 1 sets it for good, so it is only ever written back as read */

#define TCO_TICK_MS     600
#define TCO_TMR_MASK    0x03FFu
#define TCO_TMR_MIN     2               /* the hardware ignores 0 and 1 */

/* ---- the ACPI I/O block (PMBASE / ABASE) ---- */
#define ACPI_SMI_EN     0x30            /* 32 */
#define SMI_EN_TCO      (1u << 13)      /* TCO events (the first timeout among them) raise an SMI */

#define TCOHW_TIMEOUT_MAX  (2u * TCO_TMR_MASK * TCO_TICK_MS / 1000u)   /* 1227 s: the longest total the hardware can count */

enum { TCO_KIND_NONE = 0, TCO_KIND_ICH = 1, TCO_KIND_SPT = 2 };

typedef struct {
    int kind;                           /* TCO_KIND_* */
    uint16_t lpc_vendor, lpc_device;    /* the LPC bridge at 00:1f.0 */
    uint16_t abase;                     /* the ACPI I/O block (0 = not found) */
    uint16_t tcobase;                   /* the TCO I/O registers */
    uint64_t nr_phys;                   /* the memory-mapped register that holds NO_REBOOT, 0 = not known for this chipset */
    uint32_t nr_bit;
    volatile uint32_t *nr_reg;          /* nr_phys as the caller can reach it: the loader uses the address as it is, the kernel maps it */
    const char *what;                   /* "ICH9 family (q35)": which rule found it */
    const char *why;                    /* kind NONE: why not; otherwise what is missing (no NO_REBOOT register), or 0 */
} TcoHw;

/* ---- port I/O and PCI configuration space (bus 0, mechanism #1), with interrupts off around each address/data pair ----
 * An includer that defines TCOHW_IO brings its own six primitives instead (tests/host/tcohw_sim.c runs this file against simulated chipsets). */
#ifndef TCOHW_IO
static inline void tcohw_outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void tcohw_outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint16_t tcohw_inw(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t tcohw_inl(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static inline uint64_t tcohw_irq_off(void) { uint64_t f; __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory"); return f; }
static inline void tcohw_irq_back(uint64_t f) { if (f & 0x200) __asm__ volatile ("sti" : : : "memory"); }
#endif

static inline uint32_t tcohw_cfg32(unsigned dev, unsigned fn, unsigned off) {
    uint64_t f = tcohw_irq_off();
    tcohw_outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFCu));
    uint32_t v = tcohw_inl(0xCFC);
    tcohw_irq_back(f);
    return v;
}

static inline void tcohw_cfg_write32(unsigned dev, unsigned fn, unsigned off, uint32_t v) {
    uint64_t f = tcohw_irq_off();
    tcohw_outl(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFCu));
    tcohw_outl(0xCFC, v);
    tcohw_irq_back(f);
}

static inline uint16_t tcohw_r16(const TcoHw *t, unsigned reg) { return tcohw_inw((uint16_t)(t->tcobase + reg)); }
static inline void tcohw_w16(const TcoHw *t, unsigned reg, uint16_t v) { tcohw_outw((uint16_t)(t->tcobase + reg), v); }

/* ---- finding it ---- */

static inline int tcohw_is_intel(unsigned dev, unsigned fn, uint32_t class_subclass) {
    uint32_t id = tcohw_cfg32(dev, fn, 0);
    return (id & 0xFFFFu) == 0x8086u && (tcohw_cfg32(dev, fn, 8) >> 16) == class_subclass;
}

/* Fills *t; returns t->kind. Reads PCI configuration space and the TCO registers; the only writes are to the P2SB bridge's P2SBC register,
 * whose hide bit is cleared and set again (the 100/200-series path: the bridge must be visible for a moment to read where the private
 * configuration space is). */
static inline int tcohw_locate(TcoHw *t) {
    uint8_t *z = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof *t; i++) z[i] = 0;
    uint32_t id = tcohw_cfg32(31, 0, 0);
    t->lpc_vendor = (uint16_t)id;
    t->lpc_device = (uint16_t)(id >> 16);
    if (t->lpc_vendor != 0x8086u) { t->why = "no Intel LPC bridge at 00:1f.0 (not an Intel chipset)"; return TCO_KIND_NONE; }
    if ((tcohw_cfg32(31, 0, 8) >> 16) != 0x0601u) { t->why = "00:1f.0 is not an ISA/LPC bridge"; return TCO_KIND_NONE; }

    /* 100-series and later: the SMBus controller (class 0c05) at 00:1f.4 owns TCOBASE */
    if (tcohw_is_intel(31, 4, 0x0C05u)) {
        uint32_t tb = tcohw_cfg32(31, 4, 0x50), tc = tcohw_cfg32(31, 4, 0x54);
        if ((tb & 1u) && (tb & 0xFFE0u)) {
            if (!(tc & (1u << 8))) { t->why = "the SMBus controller has a TCO base, but its decoding is switched off (TCOCTL bit 8)"; return TCO_KIND_NONE; }
            t->kind = TCO_KIND_SPT;
            t->tcobase = (uint16_t)(tb & 0xFFE0u);
            t->what = "100/200-series PCH: TCOBASE in the SMBus controller, NO_REBOOT behind the P2SB bridge";
            if ((tcohw_cfg32(31, 2, 0) & 0xFFFFu) == 0x8086u) {                                       /* the PMC */
                uint32_t ab = tcohw_cfg32(31, 2, 0x40);
                if ((ab & 1u) && (ab & 0xFF00u)) t->abase = (uint16_t)(ab & 0xFF00u);
            }
            /* P2SBC (config 0xe0) bit 8 hides the bridge. A hidden bridge may read as all ones, P2SBC included: then it is unhidden with 0 and
             * hidden again with bit 8 alone, never with bits that were not set; otherwise only bit 8 changes and the value read is put back. */
            uint32_t p2sbc = tcohw_cfg32(31, 1, 0xE0);
            int hidden = p2sbc == 0xFFFFFFFFu || (p2sbc & (1u << 8));
            uint32_t back = p2sbc == 0xFFFFFFFFu ? (1u << 8) : p2sbc;
            if (hidden) tcohw_cfg_write32(31, 1, 0xE0, p2sbc == 0xFFFFFFFFu ? 0 : p2sbc & ~(1u << 8));
            uint32_t lo = tcohw_cfg32(31, 1, 0x10), hi = tcohw_cfg32(31, 1, 0x14), pid = tcohw_cfg32(31, 1, 0);
            if (hidden) tcohw_cfg_write32(31, 1, 0xE0, back);
            uint64_t sbreg = (uint64_t)(lo & ~0xFu) | (((lo & 6u) == 4u) ? (uint64_t)hi << 32 : 0);
            if ((pid & 0xFFFFu) == 0x8086u && lo != 0xFFFFFFFFu && sbreg >= 0x80000000ull) {
                t->nr_phys = sbreg + (0xC6ull << 16) + 0x0C;
                t->nr_bit = 1u << 1;
            } else {
                t->why = "the P2SB bridge did not show where the PCH private configuration space is: NO_REBOOT unknown";
            }
            return t->kind;
        }
    }
    /* ICH6 .. 9-series PCH, QEMU's q35: PMBASE in the LPC bridge, the TCO block at PMBASE + 0x60 */
    uint32_t pb = tcohw_cfg32(31, 0, 0x40);
    if ((pb & 1u) && (pb & 0xFF80u)) {
        t->kind = TCO_KIND_ICH;
        t->abase = (uint16_t)(pb & 0xFF80u);
        t->tcobase = (uint16_t)(t->abase + 0x60);
        t->what = "ICH6..9-series: PMBASE + 0x60, NO_REBOOT in GCS (RCBA + 0x3410)";
        uint32_t rcba = tcohw_cfg32(31, 0, 0xF0);
        if ((rcba & 1u) && (rcba & 0xFFFFC000u)) {
            t->nr_phys = (uint64_t)(rcba & 0xFFFFC000u) + 0x3410;
            t->nr_bit = 1u << 5;
        } else {
            t->why = "the root complex register block (RCBA) is not enabled: NO_REBOOT unknown";
        }
        if (tcohw_r16(t, TCO_TMR) == 0xFFFFu) { t->kind = TCO_KIND_NONE; t->why = "nothing answers at PMBASE + 0x60"; }
        return t->kind;
    }
    t->why = "neither an SMBus controller with a TCO base (00:1f.4) nor a PMBASE in the LPC bridge";
    return TCO_KIND_NONE;
}

/* ---- NO_REBOOT ---- */

/* 1 set, 0 clear, -1 unknown (no register for this chipset, or it reads as all ones). */
static inline int tcohw_noreboot(const TcoHw *t) {
    if (!t->nr_reg) return -1;
    uint32_t v = *t->nr_reg;
    if (v == 0xFFFFFFFFu) return -1;
    return (v & t->nr_bit) ? 1 : 0;
}

/* Clears it where the hardware lets us; returns what it reads afterwards (1: it stays set: a pin strap or a lock in the firmware). */
static inline int tcohw_noreboot_clear(const TcoHw *t) {
    int now = tcohw_noreboot(t);
    if (now != 1) return now;
    *t->nr_reg = *t->nr_reg & ~t->nr_bit;
    return tcohw_noreboot(t);
}

/* ---- the timer ---- */

/* The TCO_TMR value for `seconds` from the last reload to the reset (two countdowns of 0.6 s ticks). */
static inline uint16_t tcohw_ticks_for(uint32_t seconds) {
    uint32_t ticks = (seconds * 1000u + TCO_TICK_MS) / (2u * TCO_TICK_MS);           /* rounded to the nearest tick */
    if (ticks < TCO_TMR_MIN) ticks = TCO_TMR_MIN;
    if (ticks > TCO_TMR_MASK) ticks = TCO_TMR_MASK;
    return (uint16_t)ticks;
}

static inline uint32_t tcohw_seconds_of(uint16_t ticks) { return (2u * (ticks & TCO_TMR_MASK) * TCO_TICK_MS + 500u) / 1000u; }

static inline int tcohw_running(const TcoHw *t) { return !(tcohw_r16(t, TCO1_CNT) & TCO1_CNT_TMR_HLT); }

static inline void tcohw_halt(const TcoHw *t) { tcohw_w16(t, TCO1_CNT, (uint16_t)(tcohw_r16(t, TCO1_CNT) | TCO1_CNT_TMR_HLT)); }

/* A feed: a first timeout that already passed is forgotten (otherwise the next zero would count as the second one), then the reload. */
static inline void tcohw_reload(const TcoHw *t) {
    if (tcohw_r16(t, TCO1_STS) & TCO1_STS_TIMEOUT) tcohw_w16(t, TCO1_STS, TCO1_STS_TIMEOUT);
    tcohw_w16(t, TCO_RLD, 1);
}

/* Halts the counter, sets the timeout, reloads and lets it run. The status bits of an earlier start are not touched: the kernel reads them. */
static inline void tcohw_start(const TcoHw *t, uint32_t seconds) {
    tcohw_halt(t);
    tcohw_w16(t, TCO_TMR, (uint16_t)((tcohw_r16(t, TCO_TMR) & ~TCO_TMR_MASK) | tcohw_ticks_for(seconds)));
    tcohw_reload(t);
    tcohw_w16(t, TCO1_CNT, (uint16_t)(tcohw_r16(t, TCO1_CNT) & ~TCO1_CNT_TMR_HLT));
    tcohw_reload(t);
}

#endif /* SHARED_TCOHW_H */
