#ifndef K_KERNEL_H
#define K_KERNEL_H

#include "types.h"
#include "printk.h"
#include "string.h"
#include "bootinfo.h"

#define KERNEL_NAME      "Gensokyo"
#define KERNEL_VERSION   "0.1.0"
#define KERNEL_CODENAME  "Story of Eastern Wonderland"

NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define ASSERT(cond) \
    do { if (unlikely(!(cond))) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)

void selftest_run(void);

/* Gensokyo commentary for fatal errors (incident.c). */
const char *incident_quip(u64 vector, u64 cr2, u64 error);

extern BootInfo g_boot;

/* Physical <-> higher-half direct map. Only valid for RAM. */
extern u64 g_hhdm;
static inline void  *phys_to_virt(paddr_t p) { return (void *)(p + g_hhdm); }
static inline paddr_t virt_to_phys_hhdm(const void *v) { return (paddr_t)v - g_hhdm; }

#endif
