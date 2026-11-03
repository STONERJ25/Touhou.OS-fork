/*
 * A slow framebuffer, and what the kernel does about it by itself.
 *
 * The normal route to a fast screen is the page table: cpu_init reprograms the PAT so that PTE_PWT alone means write-combining, and
 * the Intel SDM's table of effective memory types gives write-combining for a PAT entry of WC whatever the MTRR says (the
 * uncached type the firmware gives a PCI aperture included). On a PC with a working PAT nothing more is needed, and the boot
 * log's "framebuffer speed" line shows it.
 *
 * It is also the one thing nobody can see before booting on the real machine, and a console that crawls is the likeliest way for
 * that boot to go wrong. So the kernel measures, and when the screen is slow it tries the second route, the one drivers took
 * before the PAT existed: an MTRR of type write-combining over the BAR of the display adapter, with the framebuffer's pages
 * switched to UC- (PTE_PCD alone), the one page type an MTRR of type WC can still turn into write-combining. It measures again,
 * logs both speeds in one line, and takes the change back unless the screen got clearly faster.
 *
 * Kernel command line: fb.mtrr=auto (default) tries only when the screen measured slow; fb.mtrr=on tries anyway and keeps the
 * result unless the screen got clearly slower; fb.mtrr=off never touches the MTRRs. fb.cache=uc (a forced uncached mapping, for
 * comparing speeds) disables all of it.
 *
 * Nothing here is allowed to make things worse: no MTRR is written unless the decision function (mtrr_plan_wc) finds a free
 * register and no overlap with another range, the window is a prefetchable BAR of the display adapter with no RAM in it, and every
 * change is read back and undone if it did not stick or did not help. Other processors are parked (INIT) by now, so their MTRRs
 * cannot disagree with ours; a future SMP start has to copy the boot processor's MTRRs into each core it wakes.
 */
#include "fb.h"
#include "fbcon.h"
#include "kernel.h"
#include "mm.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "cmdline.h"
#include "irq.h"
#include "mtrr.h"
#include "pci.h"

enum { FBT_KEPT, FBT_UNDONE, FBT_REFUSED, FBT_ALREADY };

typedef struct {
    int         verdict;
    const char *why;                    /* FBT_REFUSED, FBT_ALREADY */
    int         slot;
    u64         base, size;             /* the window */
    u64         before, after;          /* MB/s */
} TuneResult;

typedef u64 (*MeasureFn)(void);

static u64 g_tsc_mhz;

static u64 measure_screen(void) { return fbcon_speed_probe(g_tsc_mhz); }

/* Is `after` enough of an improvement over `before` to keep the MTRR? A pure function, so that the self test can run it over numbers. */
static bool worth_keeping(u64 before, u64 after, bool forced) {
    if (!before || !after) return false;
    if (forced) return after * 5 >= before * 4;             /* fb.mtrr=on: unless it got clearly slower (a screen that was fast already shows no gain) */
    return after >= before + before / 2;                    /* auto: at least 1.5 times as fast; uncached against write-combining is 10 to 50 times */
}

static bool window_has_ram(u64 base, u64 size) {
    u32 n;
    const BootMemRegion *r = boot_mmap(&n);
    for (u32 i = 0; i < n; i++) {
        if (r[i].type == BOOTMEM_RESERVED || r[i].type == BOOTMEM_MMIO) continue;
        if (r[i].base < base + size && base < r[i].base + r[i].length) return true;
    }
    return false;
}

/*
 * The attempt itself: plan, write, switch the framebuffer pages to UC-, measure, keep or undo. `measure` returns MB/s with the
 * mapping as it is at that moment (the self test passes a scripted one).
 */
static void try_window(u64 base, u64 size, bool forced, MeasureFn measure, TuneResult *r) {
    memset(r, 0, sizeof *r);
    r->base = base;
    r->size = size;
    MtrrState st;
    mtrr_read(&st);
    int slot = mtrr_plan_wc(&st, base, size, &r->why);
    if (slot < 0) {
        r->verdict = slot == MTRR_PLAN_ALREADY ? FBT_ALREADY : FBT_REFUSED;
        return;
    }
    r->slot = slot;
    r->before = measure();
    if (!r->before) {
        r->verdict = FBT_REFUSED;
        r->why = "the screen cannot be timed right now";
        return;
    }
    u64 old_base = st.base[slot], old_mask = st.mask[slot];
    if (!mtrr_write_var((u32)slot, base | MTRR_WC, mtrr_range_mask(&st, size))) {
        mtrr_write_var((u32)slot, old_base, old_mask);
        r->verdict = FBT_REFUSED;
        r->why = "the CPU did not keep the new MTRR value";
        return;
    }
    g_cpu.wc_by_mtrr = true;
    fb_set_map_flags(pte_wc_flags());
    r->after = measure();
    if (worth_keeping(r->before, r->after, forced)) {
        r->verdict = FBT_KEPT;
        return;
    }
    mtrr_write_var((u32)slot, old_base, old_mask);
    g_cpu.wc_by_mtrr = false;
    fb_set_map_flags(pte_wc_flags());
    r->verdict = FBT_UNDONE;
}

static void report(const TuneResult *r, u64 speed_now) {
    char sz[24];
    if (r->size >= (1UL << 30)) snprintf(sz, sizeof sz, "%lu GiB", (unsigned long)(r->size >> 30));
    else snprintf(sz, sizeof sz, "%lu MiB", (unsigned long)(r->size >> 20));
    switch (r->verdict) {
    case FBT_KEPT:
        klog("Kappa", "framebuffer: write-combining MTRR #%d over %#lx + %s (the display BAR): fb speed before %lu MB/s after %lu MB/s, kept",
             r->slot, (unsigned long)r->base, sz, (unsigned long)r->before, (unsigned long)r->after);
        break;
    case FBT_UNDONE:
        klog("Kappa", "framebuffer: write-combining MTRR #%d over %#lx + %s (the display BAR): fb speed before %lu MB/s after %lu MB/s, "
                      "no gain: the MTRR is removed again and the page tables keep the framebuffer (%s)",
             r->slot, (unsigned long)r->base, sz, (unsigned long)r->before, (unsigned long)r->after, fb_cache_name());
        break;
    case FBT_ALREADY:
        klog("Kappa", "framebuffer: no MTRR added (%s); fb speed %lu MB/s stays: if that is slow the display memory itself is", r->why,
             (unsigned long)speed_now);
        break;
    default:
        klog("Kappa", "framebuffer: no MTRR added (%s); fb speed %lu MB/s stays (%s)", r->why, (unsigned long)speed_now, fb_cache_name());
        break;
    }
}

void fb_tune(u64 tsc_mhz) {
    if (!g_fb.ready) return;
    char opt[8];
    bool given = cmdline_value("fb.mtrr", opt, sizeof opt);
    bool off = given && !strcmp(opt, "off"), force = given && !strcmp(opt, "on");
    if (g_fb_force_uc) {
        if (given) klog("Kappa", "fb.cache=uc: the framebuffer is uncached on purpose, fb.mtrr is ignored");
        return;
    }
    if (off) {
        klog("Kappa", "fb.mtrr=off: no write-combining MTRR is tried (the framebuffer is %s)", fb_cache_name());
        return;
    }
    g_tsc_mhz = tsc_mhz;
    u64 now = measure_screen();
    if (!now) {
        if (force) klog("Kappa", "fb.mtrr=on: the screen cannot be timed (the console does not own it, or the TSC rate is unknown)");
        return;
    }
    if (!force && now >= FB_SLOW_MBPS) {
        klog("Kappa", "fb.mtrr=auto: the screen takes %lu MB/s, fast enough: the MTRRs stay as the firmware set them", (unsigned long)now);
        return;
    }

    /* Slow, or asked for: put the evidence on the screen first (a photograph of it is all the owner can send), then try. */
    MtrrState st;
    mtrr_read(&st);
    char layout[320];
    mtrr_describe(&st, layout, sizeof layout);
    klog("Kappa", "mtrr: %s", layout);

    TuneResult r;
    memset(&r, 0, sizeof r);
    paddr_t wb = 0;
    u64 ws = 0;
    bool pf = false;
    u64 fbsize = (u64)g_fb.pitch * g_fb.height * 4;
    if (!pci_display_bar(g_fb.phys, fbsize, &wb, &ws, &pf)) {
        r.verdict = FBT_REFUSED;
        r.why = "no BAR of a display adapter holds the framebuffer";
    } else if (!pf) {
        r.verdict = FBT_REFUSED;
        r.why = "the BAR that holds the framebuffer is not prefetchable";
    } else if (window_has_ram(wb, ws)) {
        r.verdict = FBT_REFUSED;
        r.why = "the BAR overlaps RAM";
    } else {
        klog("Kappa", "framebuffer %#lx lies in a prefetchable BAR of %#lx + %#lx bytes, trying a write-combining MTRR for it", (unsigned long)g_fb.phys,
             (unsigned long)wb, (unsigned long)ws);
        try_window(wb, ws, force, measure_screen, &r);
    }
    report(&r, now);
}

/* ---------------------------------------------------------------- self test */

static int g_fails;

#define CHECK(cond) do { if (!(cond)) { g_fails++; printk("  FAIL fbtune.c:%d: %s\n", __LINE__, #cond); } } while (0)

static void fake_state(MtrrState *s, u32 vcnt, int def) {
    memset(s, 0, sizeof *s);
    s->present = s->enabled = s->wc_ok = true;
    s->vcnt = vcnt;
    s->def_type = (u8)def;
    s->phys_mask = (1UL << 39) - 1;                             /* what a Kaby Lake reports */
}

static void fake_range(MtrrState *s, u32 i, u64 base, u64 size, int type) {
    s->base[i] = base | (u64)type;
    s->mask[i] = (~(size - 1) & s->phys_mask & ~0xFFFUL) | BIT(11);
}

#define MiB(n) ((u64)(n) << 20)
#define GiB(n) ((u64)(n) << 30)

static u64 g_script[2];
static int g_script_pos;
static u64 scripted(void) { return g_script[g_script_pos++ & 1]; }
static void script(u64 before, u64 after) { g_script[0] = before; g_script[1] = after; g_script_pos = 0; }

static bool same_ranges(const MtrrState *a, const MtrrState *b) {
    if (a->vcnt != b->vcnt) return false;
    for (u32 i = 0; i < a->vcnt; i++)
        if (a->base[i] != b->base[i] || a->mask[i] != b->mask[i]) return false;
    return a->def_type == b->def_type && a->enabled == b->enabled;
}

/* The decisions, over firmware layouts a PC can have and QEMU does not. */
static void test_planner(void) {
    MtrrState s;
    const char *why = NULL;
    const u64 fb = GiB(3);                                      /* 0xC0000000: the usual place of a graphics aperture, in the PCI hole */

    /* 3 GiB of RAM below the hole, 4 GiB above it, everything else uncached by default: free slot 3, the window is simply uncovered. */
    fake_state(&s, 8, MTRR_UC);
    fake_range(&s, 0, 0, GiB(2), MTRR_WB);
    fake_range(&s, 1, GiB(2), GiB(1), MTRR_WB);
    fake_range(&s, 2, GiB(4), GiB(4), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == 3);
    CHECK(mtrr_type_at(&s, 0x1000) == MTRR_WB);
    CHECK(mtrr_type_at(&s, fb) == MTRR_UC);                     /* the hole falls back to the default */
    CHECK(mtrr_type_at(&s, GiB(5)) == MTRR_WB);
    CHECK(mtrr_range_mask(&s, MiB(256)) == (0x7FF0000000UL | BIT(11)));

    /* The firmware carved the hole out of one big WB range with a UC range on top: UC wins over everything, so WC would do nothing. */
    fake_state(&s, 10, MTRR_UC);
    fake_range(&s, 0, 0, GiB(8), MTRR_WB);
    fake_range(&s, 1, fb, GiB(1), MTRR_UC);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    CHECK(why && strstr(why, "(UC)"));
    CHECK(mtrr_type_at(&s, fb + 0x1000) == MTRR_UC);            /* UC over WB */
    CHECK(mtrr_type_at(&s, GiB(1)) == MTRR_WB);

    /* What OVMF programs in QEMU (read from a real boot): write-back by default, uncached holes for the PCI window below 4 GiB and for
     * the 64-bit one. A graphics aperture in either is under a UC range. */
    fake_state(&s, 8, MTRR_WB);
    s.phys_mask = (1UL << 40) - 1;
    fake_range(&s, 0, GiB(2), GiB(2), MTRR_UC);
    fake_range(&s, 1, GiB(768), GiB(128), MTRR_UC);
    CHECK(mtrr_plan_wc(&s, GiB(2), MiB(16), &why) == MTRR_PLAN_REFUSED && why && strstr(why, "MTRR #0 (UC)"));
    CHECK(mtrr_type_at(&s, GiB(2)) == MTRR_UC);
    CHECK(mtrr_type_at(&s, 0x1000) == MTRR_WB);
    char text[160];
    mtrr_describe(&s, text, sizeof text);
    CHECK(!strcmp(text, "on, default WB, 2 of 8 variable ranges in use: #0 0x80000000+2 GiB UC, #1 0xc000000000+128 GiB UC"));
    s.mask[0] = 0;                                              /* without the UC hole the same window is simply written back by default: WC may go there */
    CHECK(mtrr_plan_wc(&s, GiB(2), MiB(16), &why) == 0);

    /* A WB range reaches over the window and nothing carves it out: WB and WC overlapping is undefined, so no. */
    fake_state(&s, 10, MTRR_UC);
    fake_range(&s, 0, 0, GiB(4), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    CHECK(why && strstr(why, "(WB)") && strstr(why, "undefined"));

    /* The same, but the WB range is smaller than the window and lies inside it. */
    fake_state(&s, 10, MTRR_UC);
    fake_range(&s, 4, fb + MiB(64), MiB(64), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    CHECK(mtrr_type_at(&s, fb + MiB(64) + 4096) == MTRR_WB);
    CHECK(mtrr_type_at(&s, fb) == MTRR_UC);

    /* Every variable register is taken. */
    fake_state(&s, 4, MTRR_UC);
    for (u32 i = 0; i < 4; i++) fake_range(&s, i, GiB(8) + GiB(i), GiB(1), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    CHECK(why && strstr(why, "in use"));

    /* The first free register is used, not the next one after the last. */
    fake_state(&s, 8, MTRR_UC);
    fake_range(&s, 1, 0, GiB(2), MTRR_WB);
    fake_range(&s, 2, GiB(2), GiB(1), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == 0);

    /* Somebody (an earlier boot stage) already made it write-combining. */
    fake_state(&s, 8, MTRR_UC);
    fake_range(&s, 0, fb, GiB(1), MTRR_WC);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_ALREADY);

    /* A 64-bit BAR above 4 GiB. */
    fake_state(&s, 8, MTRR_UC);
    fake_range(&s, 0, 0, GiB(8), MTRR_WB);
    CHECK(mtrr_plan_wc(&s, GiB(64), MiB(256), &why) == 1);

    /* A default type of WB: the window is covered by nothing, and nothing conflicts. */
    fake_state(&s, 8, MTRR_WB);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == 0);

    /* A UC range whose mask has a hole (hardware accepts it): 0xC0000000-0xC0FFFFFF and 0xD0000000-0xD0FFFFFF. */
    fake_state(&s, 8, MTRR_UC);
    s.base[0] = GiB(3) | MTRR_UC;
    s.mask[0] = (s.phys_mask & ~0xFFFUL & ~(BIT(28) | (MiB(16) - 1))) | BIT(11);
    CHECK(mtrr_type_at(&s, 0xD0000000UL) == MTRR_UC);
    CHECK(mtrr_type_at(&s, 0xC0000000UL) == MTRR_UC);
    CHECK(mtrr_plan_wc(&s, 0xD0000000UL, MiB(16), &why) == MTRR_PLAN_REFUSED);
    CHECK(mtrr_plan_wc(&s, 0xC8000000UL, MiB(16), &why) == 1);          /* bit 27 differs: no address in common */

    /* Things that rule an MTRR out before any register is looked at. */
    fake_state(&s, 8, MTRR_UC);
    s.enabled = false;
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED && why && strstr(why, "switched off"));
    fake_state(&s, 8, MTRR_UC);
    s.wc_ok = false;
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED && why && strstr(why, "write-combining"));
    fake_state(&s, 0, MTRR_UC);
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    fake_state(&s, 8, MTRR_UC);
    s.present = false;
    CHECK(mtrr_plan_wc(&s, fb, MiB(256), &why) == MTRR_PLAN_REFUSED);
    fake_state(&s, 8, MTRR_UC);
    CHECK(mtrr_plan_wc(&s, fb + MiB(8), MiB(256), &why) == MTRR_PLAN_REFUSED && why && strstr(why, "aligned"));      /* not naturally aligned */
    CHECK(mtrr_plan_wc(&s, fb, MiB(100), &why) == MTRR_PLAN_REFUSED);                                               /* not a power of two */
    CHECK(mtrr_plan_wc(&s, fb, 2048, &why) == MTRR_PLAN_REFUSED);                                                   /* below the 4 KiB granularity */
    CHECK(mtrr_plan_wc(&s, GiB(1024), MiB(256), &why) == MTRR_PLAN_REFUSED && why && strstr(why, "address bits")); /* beyond 39 bits */

    /* Type arithmetic for overlapping ranges: UC beats everything, WT beats WB, anything else is left undefined by the SDM. */
    fake_state(&s, 8, MTRR_UC);
    fake_range(&s, 0, 0, GiB(1), MTRR_WB);
    fake_range(&s, 1, 0, MiB(1), MTRR_WT);
    fake_range(&s, 2, MiB(2), MiB(1), MTRR_WC);
    fake_range(&s, 3, MiB(2), MiB(1), MTRR_WB);
    CHECK(mtrr_type_at(&s, 0x1000) == MTRR_WT);
    CHECK(mtrr_type_at(&s, MiB(2) + 0x1000) == MTRR_UNDEFINED);
    CHECK(mtrr_type_at(&s, MiB(8)) == MTRR_WB);

    /* The verdict on the two speeds. */
    CHECK(worth_keeping(100, 3000, false));
    CHECK(!worth_keeping(100, 120, false));
    CHECK(!worth_keeping(2000, 1500, false));
    CHECK(worth_keeping(100, 150, false));
    CHECK(!worth_keeping(100, 149, false));
    CHECK(worth_keeping(2000, 1700, true));                     /* forced: only a clear loss undoes it */
    CHECK(!worth_keeping(2000, 1500, true));
    CHECK(!worth_keeping(0, 500, false));
    CHECK(!worth_keeping(500, 0, true));
}

static u64 fb_page_flags(void) {
    u64 fl = 0;
    if (!g_fb.ready || !vmm_translate(vmm_kernel_pml4(), (u64)g_fb.front, &fl)) return ~0UL;
    return fl & (PTE_PWT | PTE_PCD);
}

/* The real thing on the MTRRs this machine has, in a window nothing can be at (the last MiB of the physical address space). */
static void test_hardware(void) {
    MtrrState s0, s1;
    mtrr_read(&s0);
    if (!s0.present) { klog("selftest", "mtrr: this CPU has none, the hardware part is skipped"); return; }
    u64 win = s0.phys_mask + 1 - MiB(1);
    const char *why = NULL;
    int slot = mtrr_plan_wc(&s0, win, MiB(1), &why);
    if (slot < 0) {
        klog("selftest", "mtrr: the hardware part is skipped (%s)", why ? why : "?");
        return;
    }
    u64 cr0 = read_cr0(), cr4 = read_cr4(), def = rdmsr(0x2FF);

    /* One range written and read back; every other register, the default type and the control registers stay as they were. */
    CHECK(mtrr_write_var((u32)slot, win | MTRR_WC, mtrr_range_mask(&s0, MiB(1))));
    mtrr_read(&s1);
    CHECK(mtrr_type_at(&s1, win) == MTRR_WC);
    CHECK(mtrr_type_at(&s1, win - 4096) == mtrr_type_at(&s0, win - 4096));
    for (u32 i = 0; i < s0.vcnt; i++)
        if ((int)i != slot) CHECK(s0.base[i] == s1.base[i] && s0.mask[i] == s1.mask[i]);
    CHECK(read_cr0() == cr0 && read_cr4() == cr4 && rdmsr(0x2FF) == def);
    CHECK(mtrr_plan_wc(&s1, win, MiB(1), &why) == MTRR_PLAN_ALREADY);
    CHECK(mtrr_write_var((u32)slot, s0.base[slot], s0.mask[slot]));
    mtrr_read(&s1);
    CHECK(same_ranges(&s0, &s1));
    CHECK(read_cr0() == cr0 && read_cr4() == cr4 && rdmsr(0x2FF) == def);

    /* The whole attempt with made-up speeds: kept, undone for lack of gain, kept by force, undone for a loss. The framebuffer's page
     * flags and the registers must come out right every time. */
    u64 pat_flags = fb_page_flags();
    TuneResult r;
    script(100, 3000);
    try_window(win, MiB(1), false, scripted, &r);
    CHECK(r.verdict == FBT_KEPT && r.slot == slot && r.before == 100 && r.after == 3000);
    CHECK(g_cpu.wc_by_mtrr);
    mtrr_read(&s1);
    CHECK(mtrr_type_at(&s1, win) == MTRR_WC);
    if (g_fb.ready) CHECK(fb_page_flags() == PTE_PCD);          /* UC-, so the MTRR decides */
    CHECK(pte_wc_flags() == PTE_PCD);
    mtrr_write_var((u32)slot, s0.base[slot], s0.mask[slot]);    /* back to the start for the next case */
    g_cpu.wc_by_mtrr = false;
    fb_set_map_flags(pte_wc_flags());

    script(100, 110);
    try_window(win, MiB(1), false, scripted, &r);
    CHECK(r.verdict == FBT_UNDONE);
    CHECK(!g_cpu.wc_by_mtrr);
    mtrr_read(&s1);
    CHECK(same_ranges(&s0, &s1));
    CHECK(read_cr0() == cr0 && read_cr4() == cr4 && rdmsr(0x2FF) == def);
    if (g_fb.ready) CHECK(fb_page_flags() == pat_flags);

    script(2000, 1700);
    try_window(win, MiB(1), true, scripted, &r);
    CHECK(r.verdict == FBT_KEPT);
    mtrr_write_var((u32)slot, s0.base[slot], s0.mask[slot]);
    g_cpu.wc_by_mtrr = false;
    fb_set_map_flags(pte_wc_flags());

    script(2000, 1000);
    try_window(win, MiB(1), true, scripted, &r);
    CHECK(r.verdict == FBT_UNDONE);
    mtrr_read(&s1);
    CHECK(same_ranges(&s0, &s1));

    script(0, 500);                                             /* an unmeasurable screen changes nothing */
    try_window(win, MiB(1), false, scripted, &r);
    CHECK(r.verdict == FBT_REFUSED);
    mtrr_read(&s1);
    CHECK(same_ranges(&s0, &s1));
    if (g_fb.ready) CHECK(fb_page_flags() == pat_flags);
}

/* (The lookup of the display adapter's BAR needs the PCI scan, which runs after the self tests; a boot with fb.mtrr=on exercises it
 * and tests/case_hwaudit.py checks the window it reports.) */
int fb_tune_selftest(void) {
    g_fails = 0;
    klog("selftest", "mtrr: decisions over made-up firmware layouts, the update sequence on the real registers");
    test_planner();
    test_hardware();
    klog("selftest", "mtrr: %s", g_fails ? "FAIL" : "ok");
    return g_fails;
}
