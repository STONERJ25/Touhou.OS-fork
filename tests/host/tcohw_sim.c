/*
 * shared/tcohw.h against simulated chipsets, for tests/host/test_tcohw.py. The 100/200-series path (the Dell OptiPlex 7050's Q270) cannot run
 * in QEMU, whose q35 is an ICH9; here PCI configuration space and the TCO I/O registers are tables, set up the way the datasheets describe
 * those chipsets, and every configuration write is recorded.
 *
 *   tcohw_sim SCENARIO     q35 | q270 | q270-ones (a hidden P2SB reads as all ones, P2SBC included) | q270-visible (the P2SB is not hidden)
 *                          | q270-nodecode (TCOCTL bit 8 clear) | amd (no Intel LPC bridge)
 *
 * Prints key=value lines: what tcohw_locate() found, the configuration writes, then what NO_REBOOT and the timer did.
 */
#define TCOHW_IO
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t g_cfg[32][8][64];       /* bus 0: device, function, dword; absent functions read as all ones */
static uint32_t g_addr;
static int g_hidden_reads_ones;
static uint16_t g_tco[16], g_tcobase;   /* the TCO registers, by word */
static int g_rld_writes;
static char g_writes[2048];

static int p2sb_hidden(void) { return (g_cfg[31][1][0xE0 / 4] >> 8) & 1; }

static uint32_t cfg_read(void) {
    unsigned dev = (g_addr >> 11) & 31, fn = (g_addr >> 8) & 7, off = g_addr & 0xFC;
    if (!(g_addr & 0x80000000u) || ((g_addr >> 16) & 0xFF)) return 0xFFFFFFFFu;
    if (dev == 31 && fn == 1 && p2sb_hidden() && (g_hidden_reads_ones || off != 0xE0)) return 0xFFFFFFFFu;
    return g_cfg[dev][fn][off / 4];
}

static void cfg_write(uint32_t v) {
    unsigned dev = (g_addr >> 11) & 31, fn = (g_addr >> 8) & 7, off = g_addr & 0xFC;
    size_t n = strlen(g_writes);
    snprintf(g_writes + n, sizeof g_writes - n, "%s%u:%u:%#x=%#x", n ? ";" : "", dev, fn, off, v);
    g_cfg[dev][fn][off / 4] = v;
}

static inline uint16_t tcohw_inw(uint16_t p) {
    if (p >= g_tcobase && p < g_tcobase + 32) return g_tco[(p - g_tcobase) / 2];
    return 0xFFFF;
}

static inline void tcohw_outw(uint16_t p, uint16_t v) {
    if (p < g_tcobase || p >= g_tcobase + 32) return;
    unsigned reg = p - g_tcobase;
    if (reg == 0x00) g_rld_writes++;
    else if (reg == 0x04 || reg == 0x06) g_tco[reg / 2] &= (uint16_t)~v;           /* the status registers: write 1 to clear */
    else if (reg == 0x08) g_tco[reg / 2] = (uint16_t)(v | (g_tco[reg / 2] & 0x1000)); /* TCO_LOCK stays set */
    else g_tco[reg / 2] = v;
}

static inline uint32_t tcohw_inl(uint16_t p) { return p == 0xCFC ? cfg_read() : p == 0xCF8 ? g_addr : 0xFFFFFFFFu; }
static inline void tcohw_outl(uint16_t p, uint32_t v) { if (p == 0xCF8) g_addr = v; else if (p == 0xCFC) cfg_write(v); }
static inline uint64_t tcohw_irq_off(void) { return 0; }
static inline void tcohw_irq_back(uint64_t f) { (void)f; }

#include "tcohw.h"

static void fn(unsigned dev, unsigned f, uint16_t vendor, uint16_t device, uint32_t class_subclass) {
    g_cfg[dev][f][0] = (uint32_t)device << 16 | vendor;
    g_cfg[dev][f][2] = class_subclass << 16 | 0x01;          /* revision 1 */
}

int main(int argc, char **argv) {
    const char *s = argc > 1 ? argv[1] : "q35";
    memset(g_cfg, 0xFF, sizeof g_cfg);
    int q270 = !strncmp(s, "q270", 4);
    if (!strcmp(s, "q35")) {
        fn(31, 0, 0x8086, 0x2918, 0x0601);                    /* ICH9 LPC */
        g_cfg[31][0][0x40 / 4] = 0x00000601;                  /* PMBASE 0x600 */
        g_cfg[31][0][0xF0 / 4] = 0xFED1C001;                  /* RCBA, enabled */
        fn(31, 3, 0x8086, 0x2930, 0x0C05);                    /* ICH9 SMBus (no TCO base of its own) */
        g_tcobase = 0x660;
    } else if (q270) {
        fn(31, 0, 0x8086, 0xA2C6, 0x0601);                    /* Q270 LPC/eSPI: config 0x40 holds no PMBASE on this generation */
        for (int i = 0x10; i < 0x100; i += 4) g_cfg[31][0][i / 4] = 0;
        fn(31, 2, 0x8086, 0xA2A1, 0x0580);                    /* PMC */
        g_cfg[31][2][0x40 / 4] = 0x00001801;                  /* ABASE 0x1800 */
        fn(31, 4, 0x8086, 0xA2A3, 0x0C05);                    /* SMBus */
        g_cfg[31][4][0x50 / 4] = 0x00000401;                  /* TCOBASE 0x400 */
        g_cfg[31][4][0x54 / 4] = strcmp(s, "q270-nodecode") ? 0x00000100 : 0;
        fn(31, 1, 0x8086, 0xA2A0, 0x0580);                    /* P2SB */
        g_cfg[31][1][0x10 / 4] = 0xFD000004;                  /* SBREG_BAR: a 64-bit memory BAR at 0xfd000000 */
        g_cfg[31][1][0x14 / 4] = 0;
        g_cfg[31][1][0xE0 / 4] = strcmp(s, "q270-visible") ? 0x00000100 : 0;
        g_hidden_reads_ones = !strcmp(s, "q270-ones");
        g_tcobase = 0x400;
    } else if (!strcmp(s, "amd")) {
        fn(20, 3, 0x1022, 0x790E, 0x0601);
    }
    g_tco[TCO_TMR / 2] = 0x0004;
    g_tco[TCO1_CNT / 2] = TCO1_CNT_TMR_HLT;                   /* halted, as the firmware leaves it */
    g_tco[TCO1_STS / 2] = TCO1_STS_TIMEOUT;                   /* a first timeout from before */
    g_tco[TCO2_STS / 2] = TCO2_STS_SECOND_TO;                 /* and the previous start's reset */

    TcoHw t;
    int kind = tcohw_locate(&t);
    printf("kind=%d\nabase=%#x\ntcobase=%#x\nnr_phys=%#llx\nnr_bit=%#x\nwhy=%s\nwhat=%s\n", kind, t.abase, t.tcobase,
           (unsigned long long)t.nr_phys, t.nr_bit, t.why ? t.why : "", t.what ? t.what : "");
    printf("writes=%s\np2sbc=%#x\n", g_writes, g_cfg[31][1][0xE0 / 4]);
    if (kind == TCO_KIND_NONE) return 0;

    static uint32_t nr_mem;                                   /* the memory-mapped register, as the caller would map it */
    nr_mem = 0x00000201u | t.nr_bit;                          /* NO_REBOOT set, other bits that must stay */
    t.nr_reg = &nr_mem;
    int before = tcohw_noreboot(&t);
    int after = tcohw_noreboot_clear(&t);
    printf("nr_before=%d\nnr_after=%d\nnr_mem=%#x\n", before, after, nr_mem);

    tcohw_start(&t, 120);
    printf("tmr=%#x\ncnt1=%#x\nsts1=%#x\nsts2=%#x\nrld_writes=%d\nrunning=%d\n", g_tco[TCO_TMR / 2], g_tco[TCO1_CNT / 2], g_tco[TCO1_STS / 2],
           g_tco[TCO2_STS / 2], g_rld_writes, tcohw_running(&t));
    tcohw_halt(&t);
    printf("halted_cnt1=%#x\n", g_tco[TCO1_CNT / 2]);
    static const unsigned secs[] = { 1, 2, 3, 60, 120, 600, 1227, 5000 };
    printf("ticks=");
    for (unsigned i = 0; i < sizeof secs / sizeof secs[0]; i++) printf("%s%u:%u", i ? "," : "", secs[i], tcohw_ticks_for(secs[i]));
    printf("\nseconds_of_100=%u\n", tcohw_seconds_of(100));
    return 0;
}
