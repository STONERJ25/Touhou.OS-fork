/* Kernel random numbers: a ChaCha20 generator with fast key erasure, seeded from RDRAND (when present), the TSC and timer jitter, and stirred by
 * input events and the arrival of network frames (random_mix, called from input_emit and net_rx; /dev/random takes more from whoever writes to it). The key is replaced from the generator's own output after every refill, so what has been handed out
 * says nothing about what comes next, and a later reseed from RDRAND (every 256 KiB of output) cannot be undone by anything that happened before it.
 * getrandom(2), /dev/urandom, TCP initial sequence numbers and the ports all come from here; the SSH server's keys depend on it. */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "irq.h"
#include "random.h"

static u32 g_key[8] = { 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu, 0x165667B1u, 0xD3A2646Cu, 0xFD7046C5u, 0xB55A4F09u };
static u64 g_ctr;
static u64 g_mix;                       /* entropy events, folded into the key at the next refill */
static u8  g_pool[32];
static unsigned g_avail;                /* unread bytes at the END of g_pool */
static u64 g_since_reseed;

static inline u32 rol32(u32 x, int k) { return (x << k) | (x >> (32 - k)); }
#define QR(a, b, c, d) do { a += b; d ^= a; d = rol32(d, 16); c += d; b ^= c; b = rol32(b, 12); a += b; d ^= a; d = rol32(d, 8); c += d; b ^= c; b = rol32(b, 7); } while (0)

/* One ChaCha20 block: the key, a 64-bit block counter, a zero nonce. */
static void chacha_block(u8 out[64], const u32 key[8], u64 ctr) {
    u32 in[16] = { 0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                   (u32)ctr, (u32)(ctr >> 32), 0, 0 };
    u32 x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        u32 v = x[i] + in[i];
        out[4 * i] = (u8)v; out[4 * i + 1] = (u8)(v >> 8); out[4 * i + 2] = (u8)(v >> 16); out[4 * i + 3] = (u8)(v >> 24);
    }
}

static u64 hw_random(void) {
    u64 v = rdtsc();
    if (CPU_HAS_RDRAND) {
        for (int tries = 0; tries < 10; tries++) {
            u64 r;
            unsigned char ok;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
            if (ok) { v ^= r; break; }
        }
    }
    return v;
}

static unsigned g_stir;

static void stir_key(u64 v) {                         /* 64 bits into two successive words of the key, round and round */
    g_key[g_stir++ & 7] ^= (u32)v;
    g_key[g_stir++ & 7] ^= (u32)(v >> 32);
}

static void refill(void) {
    if (g_since_reseed >= 256 * 1024) {
        for (int i = 0; i < 4; i++) stir_key(hw_random());
        g_since_reseed = 0;
    }
    if (g_mix) { stir_key(g_mix); g_mix = 0; }
    u8 blk[64];
    chacha_block(blk, g_key, g_ctr++);
    for (int i = 0; i < 8; i++) g_key[i] = (u32)blk[4 * i] | (u32)blk[4 * i + 1] << 8 | (u32)blk[4 * i + 2] << 16 | (u32)blk[4 * i + 3] << 24;     /* the key is erased */
    memcpy(g_pool, blk + 32, 32);
    g_avail = 32;
    g_since_reseed += 32;
    for (int i = 0; i < 64; i++) blk[i] = 0;
}

void random_mix(u64 v) {
    u64 f = irq_save();
    g_mix = ((g_mix << 7) | (g_mix >> 57)) ^ (v * 0x9E3779B97F4A7C15ULL);
    irq_restore(f);
}

static void take(void *dst, size_t n) {               /* n <= 32, interrupts off */
    if (g_avail < n) refill();
    memcpy(dst, g_pool + (32 - g_avail), n);
    for (size_t i = 0; i < n; i++) g_pool[32 - g_avail + i] = 0;
    g_avail -= (unsigned)n;
}

u64 random_u64(void) {
    u64 v;
    u64 f = irq_save();
    take(&v, sizeof v);
    irq_restore(f);
    return v;
}

void random_init(void) {
    u64 f = irq_save();
    for (int i = 0; i < 4; i++) stir_key(hw_random());
    for (int i = 0; i < 32; i++) { stir_key(rdtsc()); udelay(3); }
    for (int i = 0; i < 8; i++) { stir_key(hw_random()); g_key[i] ^= (u32)hw_random(); }
    g_since_reseed = 0;
    g_avail = 0;
    for (int i = 0; i < 4; i++) { u64 sink; take(&sink, sizeof sink); }
    irq_restore(f);
    if (!CPU_HAS_RDRAND) klog("Gensokyo", "random: this CPU has no RDRAND; the generator is seeded from the TSC and timer jitter only, and stirred by input events and network frames as they come");
}

void random_fill(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        size_t c = n < 32 ? n : 32;
        u64 f = irq_save();
        take(p, c);
        irq_restore(f);
        p += c;
        n -= c;
    }
}
