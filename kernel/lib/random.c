/* Kernel random numbers: xoshiro256** seeded from the TSC, RDRAND (when present) and timer jitter. */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "irq.h"
#include "random.h"

static u64 g_s[4] = { 0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL, 0x2545F4914F6CDD1DULL };

static inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

u64 random_u64(void) {
    u64 f = irq_save();
    u64 result = rotl(g_s[1] * 5, 7) * 9;
    u64 t = g_s[1] << 17;
    g_s[2] ^= g_s[0];
    g_s[3] ^= g_s[1];
    g_s[1] ^= g_s[2];
    g_s[0] ^= g_s[3];
    g_s[2] ^= t;
    g_s[3] = rotl(g_s[3], 45);
    irq_restore(f);
    return result;
}

void random_mix(u64 v) {
    u64 f = irq_save();
    g_s[0] ^= v * 0x9E3779B97F4A7C15ULL;
    g_s[2] ^= rotl(v, 31);
    irq_restore(f);
}

void random_init(void) {
    random_mix(rdtsc());
    if (CPU_HAS_RDRAND) {
        for (int i = 0; i < 4; i++) {
            u64 v;
            unsigned char ok;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) g_s[i] ^= v;
        }
    }
    for (int i = 0; i < 16; i++) { random_mix(rdtsc()); udelay(3); }
    for (int i = 0; i < 16; i++) random_u64();
}

void random_fill(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        u64 v = random_u64();
        size_t c = n < 8 ? n : 8;
        memcpy(p, &v, c);
        p += c;
        n -= c;
    }
}
