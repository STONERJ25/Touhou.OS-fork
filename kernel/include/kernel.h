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

/* Something that must never happen, but that the kernel survives (a double free that was noticed in time, a pointer from user space
 * that turned out to be bad): logged under the tag WARNING with a backtrace and counted in /proc/kstats. The hardening tests fail
 * on any warning. Use panic() only when going on is not safe. */
void kwarn_at(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
u64  kwarn_count(void);
#define KWARN(...) kwarn_at(__FILE__, __LINE__, __VA_ARGS__)
#define WARN_ON(cond) ({ int _w = !!(cond); if (unlikely(_w)) KWARN("%s", #cond); _w; })

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
