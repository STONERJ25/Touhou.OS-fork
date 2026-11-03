/*
 * Memory type range registers (MTRRs).
 *
 * The firmware programs them before it hands over: usually "everything uncached by default" with write-back ranges for the RAM, so
 * a graphics aperture in the PCI hole below 4 GiB (or in a 64-bit window above it) is uncached as far as the MTRRs are concerned.
 * The CPU combines that with the page table's PAT type for every access (Intel SDM vol. 3A, "Effective Page-Level Memory Types
 * for Pentium III and More Recent Processor Families"). In that table a PAT entry of write-combining gives write-combining
 * whatever the MTRR says, which is why the kernel's PAT route (cpu_init) needs no MTRR on a PC. The other direction does not
 * work: an MTRR of type WC does nothing for a page whose PAT type is UC, only for UC- (the PTE_PCD-alone encoding), and UC
 * MTRR ranges always win over WC ones.
 *
 * Everything that decides is a pure function of an MtrrState, so the tests can feed it firmware layouts that no QEMU machine has
 * (a UC range over the framebuffer, no free register, an overlapping WB range). Only mtrr_read and mtrr_write_var touch MSRs.
 */
#include "mtrr.h"
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"

#define MSR_MTRRCAP     0xFE
#define MSR_MTRR_DEF    0x2FF
#define MSR_MTRR_BASE0  0x200               /* PHYSBASE0; PHYSMASK0 follows at +1, PHYSBASE1 at +2, ... */
#define MTRR_VALID      BIT(11)             /* in PHYSMASKn */
#define MTRR_DEF_E      BIT(11)             /* in IA32_MTRR_DEF_TYPE: the MTRRs are on */
#define MTRR_DEF_FE     BIT(10)             /* fixed ranges on */

#define CR0_NW          BIT(29)
#define CR0_CD          BIT(30)
#define CR4_PGE         BIT(7)

/* Bits 11:0 of a base/mask register are not address bits (type, valid bit, reserved). */
#define PAGE_BITS       (~0xFFFUL)

/* ---------------------------------------------------------------- reading */

static u64 phys_address_mask(void) {
    u32 a, b, c, d;
    u32 bits = (g_cpu.feat_edx & (1U << 6)) ? 36 : 32;                  /* PAE says 36 at least when the leaf below is missing */
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        bits = a & 0xFF;
    }
    if (bits < 32) bits = 32;
    if (bits > 52) bits = 52;                                           /* architectural maximum: the MSR fields end there */
    return (1UL << bits) - 1;
}

void mtrr_read(MtrrState *s) {
    memset(s, 0, sizeof *s);
    s->phys_mask = phys_address_mask();
    if (!(g_cpu.feat_edx & (1U << 12))) return;                         /* CPUID.1:EDX.MTRR */
    s->present = true;
    u64 cap = rdmsr(MSR_MTRRCAP);
    s->vcnt = (u32)(cap & 0xFF);
    if (s->vcnt > MTRR_MAX_VAR) s->vcnt = MTRR_MAX_VAR;
    s->wc_ok = (cap >> 10) & 1;
    u64 def = rdmsr(MSR_MTRR_DEF);
    s->enabled = (def & MTRR_DEF_E) != 0;
    s->def_type = (u8)def;
    for (u32 i = 0; i < s->vcnt; i++) {
        s->base[i] = rdmsr(MSR_MTRR_BASE0 + 2 * i);
        s->mask[i] = rdmsr(MSR_MTRR_BASE0 + 2 * i + 1);
    }
}

/* ---------------------------------------------------------------- what a state means */

/* Two ranges (base and mask each) have an address in common iff their bases agree on every bit that both masks care about. This
 * also holds for the odd masks with holes that the hardware tolerates. */
static bool ranges_overlap(u64 base_a, u64 mask_a, u64 base_b, u64 mask_b) {
    return ((base_a ^ base_b) & mask_a & mask_b & PAGE_BITS) == 0;
}

int mtrr_type_at(const MtrrState *s, u64 addr) {
    int type = -1;                                                                          /* -1: no variable range covers the address (yet) */
    for (u32 i = 0; i < s->vcnt; i++) {
        if (!(s->mask[i] & MTRR_VALID)) continue;
        if ((addr ^ s->base[i]) & s->mask[i] & PAGE_BITS) continue;
        int t = (int)(s->base[i] & 0xFF);
        if (type == -1 || type == t) type = t;
        else if (type == MTRR_UC || t == MTRR_UC) type = MTRR_UC;                           /* uncached beats everything, even a combination the SDM leaves undefined */
        else if ((type == MTRR_WT && t == MTRR_WB) || (type == MTRR_WB && t == MTRR_WT)) type = MTRR_WT;
        else type = MTRR_UNDEFINED;                                                         /* the SDM does not say what happens */
    }
    return type == -1 ? (int)s->def_type : type;
}

const char *mtrr_type_name(int type) {
    switch (type) {
    case MTRR_UC: return "UC";
    case MTRR_WC: return "WC";
    case MTRR_WT: return "WT";
    case MTRR_WP: return "WP";
    case MTRR_WB: return "WB";
    case MTRR_UNDEFINED: return "conflicting";
    default: return "?";
    }
}

/* What the firmware's MTRRs say about a physical range, in a word for the boot log ("mixed" if its start, middle and end differ). */
const char *cpu_mtrr_type_name(paddr_t start, u64 len) {
    MtrrState s;
    mtrr_read(&s);
    if (!s.present) return "no MTRRs";
    if (!s.enabled) return "MTRRs off (everything uncached)";
    int a = mtrr_type_at(&s, start), b = mtrr_type_at(&s, start + len / 2), c = mtrr_type_at(&s, start + len - 1);
    if (a != b || b != c) return "mixed";
    return mtrr_type_name(a);
}

u64 mtrr_range_mask(const MtrrState *s, u64 size) {
    return (~(size - 1) & s->phys_mask & PAGE_BITS) | MTRR_VALID;
}

static void put_size(char *buf, size_t cap, u64 size) {
    if (size >= (1UL << 30) && !(size & ((1UL << 30) - 1))) snprintf(buf, cap, "%lu GiB", (unsigned long)(size >> 30));
    else if (size >= (1UL << 20) && !(size & ((1UL << 20) - 1))) snprintf(buf, cap, "%lu MiB", (unsigned long)(size >> 20));
    else snprintf(buf, cap, "%lu KiB", (unsigned long)(size >> 10));
}

size_t mtrr_describe(const MtrrState *s, char *buf, size_t cap) {
    if (!s->present) return (size_t)snprintf(buf, cap, "no MTRRs");
    u32 used = 0;
    for (u32 i = 0; i < s->vcnt; i++) if (s->mask[i] & MTRR_VALID) used++;
    size_t n = (size_t)snprintf(buf, cap, "%s, default %s, %u of %u variable ranges in use%s", s->enabled ? "on" : "OFF", mtrr_type_name(s->def_type), used,
                                s->vcnt, s->wc_ok ? "" : ", no write-combining type");
    bool first = true;
    for (u32 i = 0; i < s->vcnt && n + 48 < cap; i++) {
        if (!(s->mask[i] & MTRR_VALID)) continue;
        u64 m = s->mask[i] & PAGE_BITS;
        u64 low = (~m + 1) & s->phys_mask;                                   /* lowest address bit the mask checks = the range's size if the mask is contiguous */
        char sz[24];
        put_size(sz, sizeof sz, low ? low : s->phys_mask + 1);
        n += (size_t)snprintf(buf + n, cap - n, "%s #%u %#lx+%s %s", first ? ":" : ",", i, (unsigned long)(s->base[i] & PAGE_BITS & s->phys_mask), sz,
                              mtrr_type_name((int)(s->base[i] & 0xFF)));
        first = false;
    }
    return n;
}

/* ---------------------------------------------------------------- deciding */

static char g_why[112];

static int refuse(const char **why, const char *msg) {
    if (why) *why = msg;
    return MTRR_PLAN_REFUSED;
}

int mtrr_plan_wc(const MtrrState *s, u64 base, u64 size, const char **why) {
    if (!s->present) return refuse(why, "this CPU has no MTRRs");
    if (!s->wc_ok) return refuse(why, "the CPU cannot make a variable MTRR write-combining");
    if (!s->vcnt) return refuse(why, "the CPU has no variable MTRRs");
    if (!s->enabled) return refuse(why, "the firmware left the MTRRs switched off");
    if (size < 4096 || (size & (size - 1)) || (base & (size - 1))) return refuse(why, "the window is not a naturally aligned power of two");
    if (base > s->phys_mask || size - 1 > s->phys_mask - base) return refuse(why, "the window lies above the address bits the CPU implements");

    u64 wmask = ~(size - 1) & s->phys_mask & PAGE_BITS;
    int free_slot = -1, uc = -1, other = -1, wc = -1;           /* the first free register, and the first overlapping range of each kind */
    for (u32 i = 0; i < s->vcnt; i++) {
        if (!(s->mask[i] & MTRR_VALID)) {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (!ranges_overlap(base, wmask, s->base[i], s->mask[i] & s->phys_mask)) continue;
        int t = (int)(s->base[i] & 0xFF);
        if (t == MTRR_UC) { if (uc < 0) uc = (int)i; }
        else if (t == MTRR_WC) { if (wc < 0) wc = (int)i; }
        else if (other < 0) other = (int)i;
    }
    if (uc >= 0) {                                              /* UC beats everything, so a WC range next to it would do nothing */
        snprintf(g_why, sizeof g_why, "MTRR #%d (UC) covers part of it and UC wins over write-combining", uc);
        if (why) *why = g_why;
        return MTRR_PLAN_REFUSED;
    }
    if (other >= 0) {                                           /* WB/WT/WP over WC: the SDM does not say what the CPU does */
        snprintf(g_why, sizeof g_why, "MTRR #%d (%s) overlaps it and the SDM leaves that combination undefined", other,
                 mtrr_type_name((int)(s->base[other] & 0xFF)));
        if (why) *why = g_why;
        return MTRR_PLAN_REFUSED;
    }
    if (wc >= 0) {
        snprintf(g_why, sizeof g_why, "MTRR #%d already makes it write-combining", wc);
        if (why) *why = g_why;
        return MTRR_PLAN_ALREADY;
    }
    if (free_slot < 0) {
        snprintf(g_why, sizeof g_why, "all %u variable MTRRs are in use", s->vcnt);
        if (why) *why = g_why;
        return MTRR_PLAN_REFUSED;
    }
    return free_slot;
}

/* ---------------------------------------------------------------- writing */

/*
 * Intel SDM vol. 3A 11.11.8 ("MTRR considerations in MP systems"), for one processor (the others are parked): interrupts off, enter
 * the no-fill cache mode (CD=1, NW=0), write back and invalidate the caches, flush the TLB (clearing CR4.PGE also drops global
 * entries), switch all ranges off, write the pair, switch them on, flush caches and TLB a second time, leave no-fill mode, restore
 * CR4.PGE. This is the sequence Linux uses (prepare_set/post_set). While CD is set every access goes to memory, so this stays
 * short and in one function.
 */
static void __attribute__((noinline)) mtrr_commit(u32 msr, u64 base, u64 mask) {
    u64 flags = irq_save();
    u64 cr0 = read_cr0(), cr4 = read_cr4();
    u64 def = rdmsr(MSR_MTRR_DEF);

    write_cr0((cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile ("wbinvd" ::: "memory");
    if (cr4 & CR4_PGE) write_cr4(cr4 & ~CR4_PGE);
    else write_cr3(read_cr3());
    wrmsr(MSR_MTRR_DEF, def & ~(MTRR_DEF_E | MTRR_DEF_FE | 0xFFUL));
    __asm__ volatile ("wbinvd" ::: "memory");

    wrmsr(msr, base);
    wrmsr(msr + 1, mask);

    wrmsr(MSR_MTRR_DEF, def);
    __asm__ volatile ("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    write_cr0(cr0);
    if (cr4 & CR4_PGE) write_cr4(cr4);
    irq_restore(flags);
}

bool mtrr_write_var(u32 slot, u64 base, u64 mask) {
    if (!(g_cpu.feat_edx & (1U << 12)) || slot >= MTRR_MAX_VAR || slot >= (rdmsr(MSR_MTRRCAP) & 0xFF)) return false;
    u32 msr = MSR_MTRR_BASE0 + 2 * slot;
    mtrr_commit(msr, base, mask);
    return rdmsr(msr) == base && rdmsr(msr + 1) == mask;
}
