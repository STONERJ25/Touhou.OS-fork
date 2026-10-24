/* CPU feature setup and identification. */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"

CpuInfo g_cpu;

/* "Intel(R) Core(TM) i5-7500 CPU @ 3.40GHz" -> 3400. On Intel parts that is also the rate the TSC counts at, which gives the
 * timer code a second opinion when the 8254 PIT it calibrates against is missing or misbehaves. */
static u32 brand_mhz(const char *brand) {
    for (const char *p = brand; *p; p++) {
        if (*p != '@') continue;
        p++;
        while (*p == ' ') p++;
        u32 whole = 0, frac = 0, fdigits = 0;
        while (*p >= '0' && *p <= '9') whole = whole * 10 + (u32)(*p++ - '0');
        if (*p == '.') {
            p++;
            for (; *p >= '0' && *p <= '9'; p++) if (fdigits < 3) { frac = frac * 10 + (u32)(*p - '0'); fdigits++; }
        }
        while (fdigits < 3) { frac *= 10; fdigits++; }
        while (*p == ' ') p++;
        if ((p[0] == 'G' || p[0] == 'g') && (p[1] == 'H' || p[1] == 'h')) return whole * 1000 + frac;
        if ((p[0] == 'M' || p[0] == 'm') && (p[1] == 'H' || p[1] == 'h')) return whole;
        return 0;
    }
    return 0;
}

void cpu_init(void) {
    u32 a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    u32 maxleaf = a;
    g_cpu.max_leaf = maxleaf;
    memcpy(g_cpu.vendor, &b, 4);
    memcpy(g_cpu.vendor + 4, &d, 4);
    memcpy(g_cpu.vendor + 8, &c, 4);
    g_cpu.vendor[12] = 0;

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            memcpy(g_cpu.brand + i * 16, &a, 4);
            memcpy(g_cpu.brand + i * 16 + 4, &b, 4);
            memcpy(g_cpu.brand + i * 16 + 8, &c, 4);
            memcpy(g_cpu.brand + i * 16 + 12, &d, 4);
        }
        g_cpu.brand[48] = 0;
        char *s = g_cpu.brand;                              /* Intel pads the string on the left with spaces */
        while (*s == ' ') s++;
        if (s != g_cpu.brand) memmove(g_cpu.brand, s, strlen(s) + 1);
        g_cpu.brand_mhz = brand_mhz(g_cpu.brand);
    } else {
        strlcpy(g_cpu.brand, "unknown x86-64 CPU", sizeof g_cpu.brand);
    }

    cpuid(1, 0, &a, &b, &c, &d);
    u32 fam = (a >> 8) & 0xF, mod = (a >> 4) & 0xF;
    if (fam == 0xF) fam += (a >> 20) & 0xFF;                /* the extended fields only count for these two families */
    if (fam == 0x6 || fam >= 0xF) mod |= ((a >> 16) & 0xF) << 4;
    g_cpu.family = fam;
    g_cpu.model = mod;
    g_cpu.stepping = a & 0xF;
    g_cpu.feat_ecx = c;
    g_cpu.feat_edx = d;
    if (maxleaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_cpu.feat7_ebx = b;
        g_cpu.feat7_ecx = c;
    }

    /* Control registers: write-protect in ring 0, FPU/SSE on, no lazy-FPU trapping. NE makes x87 errors raise #MF instead of
     * going through the legacy FERR# wire, which a modern chipset may not even connect. */
    u64 cr0 = read_cr0();
    cr0 |= (1UL << 16);                         /* WP */
    cr0 |= (1UL << 5);                          /* NE */
    cr0 |= (1UL << 1);                          /* MP */
    cr0 &= ~(1UL << 2);                         /* EM off */
    cr0 &= ~(1UL << 3);                         /* TS off */
    write_cr0(cr0);

    u64 cr4 = read_cr4();
    cr4 |= (1UL << 9) | (1UL << 10);            /* OSFXSR, OSXMMEXCPT */
    if (g_cpu.feat7_ebx & (1U << 7)) cr4 |= (1UL << 20);    /* SMEP */
    write_cr4(cr4);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1UL << 11) | (1UL << 0));   /* NXE, SCE */

    /* PAT: make index 1 (PWT only) write-combining; everything else keeps its power-on meaning. Only on a CPU that says it
     * has a PAT (CPUID.1:EDX[16]): the MSR does not exist otherwise and the write would be a #GP with no screen to show it. */
    if (g_cpu.feat_edx & (1U << 16)) {
        wrmsr(MSR_PAT, 0x0007040600070106UL);
        g_cpu.pat_wc = true;
    }

    /* The firmware's FPU state is whatever it left: x87 to its reset state, SSE to the default control word (all exceptions
     * masked, round to nearest). The default FXSAVE image every new process starts from is taken right after this. */
    __asm__ volatile ("fninit");
    static const u32 mxcsr_default = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr_default));

    write_cr8(0);                               /* the firmware may have left the APIC task priority raised: no interrupt would get through */
}

/* (cpu_mtrr_type_name, the firmware's MTRR type for a physical range, lives in mtrr.c.) */

u64 pte_wc_flags(void) {
    /* A write-combining MTRR range was added for the framebuffer (gfx/fbtune.c): PCD alone selects the UC- entry, which is the one
     * type an MTRR of type WC can still turn into write-combining (UC in the page table would stay UC). */
    if (g_cpu.wc_by_mtrr) return BIT(4);
    /* With the PAT reprogrammed above, PWT alone selects entry 1 = write-combining. Without a PAT the closest thing that does
     * not cache is uncached, which is slow for a framebuffer but never wrong. */
    return g_cpu.pat_wc ? BIT(3) : (BIT(3) | BIT(4));
}

void cpu_describe(char *buf, size_t cap) {
    static const struct { u8 reg; u8 bit; const char *name; } f[] = {
        { 0, 4, "tsc" }, { 0, 6, "pae" }, { 0, 9, "apic" }, { 0, 12, "mtrr" }, { 0, 16, "pat" }, { 0, 24, "fxsr" }, { 0, 25, "sse" },
        { 0, 26, "sse2" }, { 1, 0, "sse3" }, { 1, 9, "ssse3" }, { 1, 19, "sse4.1" }, { 1, 20, "sse4.2" }, { 1, 26, "xsave" },
        { 1, 28, "avx" }, { 1, 30, "rdrand" }, { 2, 7, "smep" }, { 2, 9, "erms" }, { 2, 20, "smap" },
    };
    size_t n = (size_t)snprintf(buf, cap, "family %u model %#x stepping %u:", g_cpu.family, g_cpu.model, g_cpu.stepping);
    for (unsigned i = 0; i < ARRAY_SIZE(f) && n + 10 < cap; i++) {
        u32 v = f[i].reg == 0 ? g_cpu.feat_edx : f[i].reg == 1 ? g_cpu.feat_ecx : g_cpu.feat7_ebx;
        if (v & (1U << f[i].bit)) n += (size_t)snprintf(buf + n, cap - n, " %s", f[i].name);
    }
    if (!g_cpu.pat_wc && n + 20 < cap) snprintf(buf + n, cap - n, " (no PAT: framebuffer uncached)");
}
