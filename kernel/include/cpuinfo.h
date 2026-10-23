#ifndef K_CPUINFO_H
#define K_CPUINFO_H

#include "types.h"

typedef struct {
    char vendor[13];
    char brand[49];
    u32  family, model, stepping;
    u32  feat_ecx, feat_edx;        /* CPUID.1 */
    u32  feat7_ebx, feat7_ecx;      /* CPUID.7.0 */
} CpuInfo;

extern CpuInfo g_cpu;

void cpu_init(void);

#define CPU_HAS_SSE2    (g_cpu.feat_edx & (1U << 26))
#define CPU_HAS_APIC    (g_cpu.feat_edx & (1U << 9))
#define CPU_HAS_RDRAND  (g_cpu.feat_ecx & (1U << 30))
#define CPU_HAS_XSAVE   (g_cpu.feat_ecx & (1U << 26))
#define CPU_HAS_SMEP    (g_cpu.feat7_ebx & (1U << 7))
#define CPU_HAS_SMAP    (g_cpu.feat7_ebx & (1U << 20))

#endif
