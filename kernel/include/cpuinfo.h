#ifndef K_CPUINFO_H
#define K_CPUINFO_H

#include "types.h"

typedef struct {
    char vendor[13];
    char brand[49];
    u32  family, model, stepping;   /* display family / model (the extended fields are folded in, as Linux does) */
    u32  feat_ecx, feat_edx;        /* CPUID.1 */
    u32  feat7_ebx, feat7_ecx;      /* CPUID.7.0 */
    u32  max_leaf;                  /* highest basic CPUID leaf */
    bool pat_wc;                    /* IA32_PAT was reprogrammed so that entry 1 (PTE_PWT alone) is write-combining */
    bool wc_by_mtrr;                /* the framebuffer is write-combining through an MTRR range (PTE_PCD alone, UC-) instead of through the PAT */
    u32  brand_mhz;                 /* "@ 3.40GHz" from the brand string (0 = none); the TSC runs at this rate on Intel parts */
} CpuInfo;

extern CpuInfo g_cpu;

void cpu_init(void);
void cpu_describe(char *buf, size_t cap);   /* "family 6 model 0x9e stepping 9, sse2 pat apic ..." for the boot log */
u64  pte_wc_flags(void);                    /* page-table bits for write-combining memory (uncached when the CPU has no PAT) */
const char *cpu_mtrr_type_name(paddr_t start, u64 len);   /* "UC", "WC", "WB", ... the firmware's MTRRs give this physical range */

#define CPU_HAS_SSE2    (g_cpu.feat_edx & (1U << 26))
#define CPU_HAS_PAT     (g_cpu.feat_edx & (1U << 16))
#define CPU_HAS_TSC     (g_cpu.feat_edx & (1U << 4))
#define CPU_HAS_APIC    (g_cpu.feat_edx & (1U << 9))
#define CPU_HAS_RDRAND  (g_cpu.feat_ecx & (1U << 30))
#define CPU_HAS_XSAVE   (g_cpu.feat_ecx & (1U << 26))
#define CPU_HAS_SMEP    (g_cpu.feat7_ebx & (1U << 7))
#define CPU_HAS_SMAP    (g_cpu.feat7_ebx & (1U << 20))

#endif
