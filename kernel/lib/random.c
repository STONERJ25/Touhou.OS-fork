/*
 * Kernel random numbers (/dev/random, /dev/urandom, getrandom(), TCP sequence numbers, TLS keys).
 *
 * Generator: ChaCha20 with "fast key erasure". Every request is served from a keystream whose first 32 bytes become the
 * next key, so a later compromise of the kernel's memory cannot reveal earlier outputs. The output of the generator is
 * safe to publish (a TLS ClientHello carries 32 bytes of it); it used to be xoshiro256**, which is not: four outputs give
 * away its whole state.
 *
 * Seeding, in order of trust:
 *   RDSEED  the CPU's conditioned hardware entropy source, when CPUID says it exists (Broadwell and newer, Zen);
 *   RDRAND  the CPU's DRBG, when CPUID says it exists (Ivy Bridge and newer);
 *   jitter  the TSC read after short busy waits and at every stirred-in event (key presses, mouse packets, ...).
 * The first two are the real thing. The jitter is a best effort: a virtual machine or a fast, quiet CPU may give it far less
 * than a bit per sample, so the estimate below credits it at only 1/4 bit. On a CPU with neither instruction the generator
 * is therefore seeded with little more than the boot-time TSC and timing, which an attacker who can guess the boot time to
 * the millisecond could enumerate; /proc/random says so ("ready: no") and libtls refuses to connect in that state.
 *
 * Events stirred in with random_mix() are collected and folded into the key (together with fresh RDRAND/TSC output) every
 * 64 events or five minutes, whichever comes first. Everything runs with interrupts off (one CPU).
 */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "irq.h"
#include "random.h"
#include "initcall.h"
#include "vfs.h"
#include "ktime.h"

static u32  g_key[8];
static u64  g_nonce;                    /* counts the requests served: a different stream every time even if the key stood still */
static u64  g_pend[4];                  /* events not yet folded into the key */
static unsigned g_pend_n, g_pend_pos;
static u64  g_last_fold_ms;
static bool g_have_rdseed, g_have_rdrand;
static unsigned g_entropy_q;            /* estimated entropy in quarter bits */
static unsigned g_hw_seeds, g_folds;
static bool g_inited;

static inline u32 rotl32(u32 x, int k) { return (x << k) | (x >> (32 - k)); }

#define QR(a, b, c, d) do {                                                                              \
        a += b; d ^= a; d = rotl32(d, 16); c += d; b ^= c; b = rotl32(b, 12);                             \
        a += b; d ^= a; d = rotl32(d, 8);  c += d; b ^= c; b = rotl32(b, 7);                              \
    } while (0)

/* One ChaCha20 block (the original 64-bit counter / 64-bit nonce layout). */
static void chacha_block(u8 out[64], const u32 key[8], u64 counter, u64 nonce) {
    u32 s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                  key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                  (u32)counter, (u32)(counter >> 32), (u32)nonce, (u32)(nonce >> 32) };
    u32 x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        u32 v = x[i] + s[i];
        out[i * 4] = (u8)v; out[i * 4 + 1] = (u8)(v >> 8); out[i * 4 + 2] = (u8)(v >> 16); out[i * 4 + 3] = (u8)(v >> 24);
    }
}

static bool hw_rdseed(u64 *v) {
    for (int tries = 0; tries < 20; tries++) {
        unsigned char ok;
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return true;
        pause();
    }
    return false;
}

static bool hw_rdrand(u64 *v) {
    for (int tries = 0; tries < 10; tries++) {
        unsigned char ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return true;
    }
    return false;
}

/* Replaces the key by chacha(key) XOR the new material: the old key is gone, and so is any linear relation to the inputs. */
static void fold(const u64 *material, int words) {
    for (int i = 0; i < words; i++) g_key[(i * 2) & 7] ^= (u32)material[i], g_key[(i * 2 + 1) & 7] ^= (u32)(material[i] >> 32);
    u8 blk[64];
    chacha_block(blk, g_key, 0, ++g_nonce);
    memcpy(g_key, blk, 32);
    memset(blk, 0, sizeof blk);
}

static void reseed(void) {                       /* irqs off */
    u64 m[4];
    for (int i = 0; i < 4; i++) m[i] = g_pend[i];
    m[0] ^= rdtsc();
    if (g_have_rdseed && hw_rdseed(&m[1])) g_hw_seeds++;
    else if (g_have_rdrand && hw_rdrand(&m[1])) g_hw_seeds++;
    if (g_have_rdrand) { u64 v; if (hw_rdrand(&v)) m[2] ^= v; }
    fold(m, 4);
    memset(g_pend, 0, sizeof g_pend);
    g_pend_n = 0;
    g_last_fold_ms = uptime_ms();
    g_folds++;
}

void random_mix(u64 v) {
    u64 f = irq_save();
    g_pend[g_pend_pos++ & 3] ^= v * 0x9E3779B97F4A7C15ULL ^ rdtsc();
    g_pend_n++;
    if (g_entropy_q < 1024) g_entropy_q++;                      /* a quarter of a bit per event, never more than 256 bits in all */
    irq_restore(f);
}

void random_init(void) {
    u64 f = irq_save();
    g_have_rdseed = (g_cpu.feat7_ebx & (1U << 18)) != 0;
    g_have_rdrand = CPU_HAS_RDRAND != 0;
    u64 m[4] = { rdtsc(), 0, 0, 0 };
    unsigned bits_q = 0;
    for (int i = 0; i < 4; i++) {                                // 256 bits of the best the CPU has
        u64 v = 0;
        if (g_have_rdseed && hw_rdseed(&v)) { m[i] ^= v; g_hw_seeds++; bits_q += 256; }
        else if (g_have_rdrand && hw_rdrand(&v)) { m[i] ^= v; g_hw_seeds++; bits_q += 128; }
    }
    for (int i = 0; i < 64; i++) {                              // timing jitter: the TSC after short, uneven busy waits
        u64 t0 = rdtsc();
        udelay(1 + (u32)(t0 & 3));
        m[(unsigned)i & 3] = (m[(unsigned)i & 3] << 7 | m[(unsigned)i & 3] >> 57) ^ (rdtsc() - t0);
    }
    bits_q += 64;                                               // 16 bits for the jitter, at a quarter bit per sample
    g_entropy_q = bits_q > 1024 ? 1024 : bits_q;
    fold(m, 4);
    for (int i = 0; i < 4; i++) fold(m, 0);                     // a few rounds of diffusion
    g_last_fold_ms = uptime_ms();
    g_inited = true;
    irq_restore(f);
    klog("Gensokyo", "random: ChaCha20 generator seeded (%s%s%s, about %u bits of entropy)", g_have_rdseed ? "rdseed " : "", g_have_rdrand ? "rdrand " : "",
         (!g_have_rdseed && !g_have_rdrand) ? "TSC jitter only: weak" : "tsc jitter", g_entropy_q / 4);
}

bool random_ready(void) { return g_inited && g_entropy_q >= 128 * 4; }

void random_fill(void *buf, size_t n) {
    u8 *out = buf;
    u64 f = irq_save();
    if (g_pend_n >= 64 || (g_inited && uptime_ms() - g_last_fold_ms > 300000)) reseed();
    u8 blk[64];
    u64 nonce = ++g_nonce;
    chacha_block(blk, g_key, 0, nonce);                         // block 0: next key and the first 32 output bytes
    u32 next[8];
    memcpy(next, blk, 32);
    size_t take = n < 32 ? n : 32;
    memcpy(out, blk + 32, take);
    out += take;
    n -= take;
    for (u64 ctr = 1; n; ctr++) {                               // the rest of the stream under the old key
        chacha_block(blk, g_key, ctr, nonce);
        take = n < 64 ? n : 64;
        memcpy(out, blk, take);
        out += take;
        n -= take;
    }
    memcpy(g_key, next, sizeof g_key);                          // key erasure
    memset(blk, 0, sizeof blk);
    memset(next, 0, sizeof next);
    irq_restore(f);
}

u64 random_u64(void) {
    u64 v;
    random_fill(&v, sizeof v);
    return v;
}

static size_t gen_random(char *b, size_t cap) {
    return (size_t)snprintf(b, cap,
        "generator: ChaCha20, fast key erasure\n"
        "hardware: %s%s%s\n"
        "estimated entropy: %u bits (RDSEED counts in full, RDRAND at half, timing jitter and events at a quarter bit each)\n"
        "hardware seeds taken: %u\nreseeds: %u\nevents waiting: %u\n"
        "ready: %s\n",
        g_have_rdseed ? "rdseed " : "", g_have_rdrand ? "rdrand" : "", (!g_have_rdseed && !g_have_rdrand) ? "none (jitter only)" : "",
        g_entropy_q / 4, g_hw_seeds, g_folds, g_pend_n, random_ready() ? "yes" : "no");
}

static void random_proc_init(void) { procfs_register("random", gen_random); }
INITCALL(random_proc_init);
