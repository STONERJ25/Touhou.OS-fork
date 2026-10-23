/* CPU feature setup and identification. */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"

CpuInfo g_cpu;

void cpu_init(void) {
    u32 a, b, c, d;

    cpuid(0, 0, &a, &b, &c, &d);
    u32 maxleaf = a;
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
    } else {
        strlcpy(g_cpu.brand, "unknown x86-64 CPU", sizeof g_cpu.brand);
    }

    cpuid(1, 0, &a, &b, &c, &d);
    g_cpu.family = (a >> 8) & 0xF;
    g_cpu.model = (a >> 4) & 0xF;
    g_cpu.stepping = a & 0xF;
    g_cpu.feat_ecx = c;
    g_cpu.feat_edx = d;
    if (maxleaf >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        g_cpu.feat7_ebx = b;
        g_cpu.feat7_ecx = c;
    }

    /* Control registers: write-protect in ring 0, FPU/SSE on, no lazy-FPU trapping. */
    u64 cr0 = read_cr0();
    cr0 |= (1UL << 16);                         /* WP */
    cr0 |= (1UL << 1);                          /* MP */
    cr0 &= ~(1UL << 2);                         /* EM off */
    cr0 &= ~(1UL << 3);                         /* TS off */
    write_cr0(cr0);

    u64 cr4 = read_cr4();
    cr4 |= (1UL << 9) | (1UL << 10);            /* OSFXSR, OSXMMEXCPT */
    if (g_cpu.feat7_ebx & (1U << 7)) cr4 |= (1UL << 20);    /* SMEP */
    write_cr4(cr4);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1UL << 11) | (1UL << 0));   /* NXE, SCE */

    /* PAT: make index 1 (PWT only) write-combining; everything else keeps its power-on meaning. */
    wrmsr(MSR_PAT, 0x0007040600070106UL);

    __asm__ volatile ("fninit");
}
