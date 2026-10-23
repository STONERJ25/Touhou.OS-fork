/*
 * Optional in-kernel I/O check for the storage drivers, switched on from the kernel command line:
 *
 *     blkselftest=sdb,nvme0n1:rw
 *
 * The listed devices must hold the test image written by tests/case_blockdev.py: every 512-byte block b is filled with
 * the 64-bit word pat_a(b). The check reads it with many request sizes and buffer alignments (including requests far
 * larger than the 64 KiB the /dev nodes ever issue, which is the path a filesystem will use), compares the data, and times
 * a large read. With ":rw" it also overwrites the device with a second pattern using odd-sized, misaligned writes, reads
 * that back with different request boundaries and times a large write. ":rw" destroys the device contents; never name a
 * real disk.
 *
 * Each driver calls blk_selftest("<driver>") at the end of its init; devices that belong to another driver are skipped.
 */
#include "kernel.h"
#include "blk.h"
#include "cmdline.h"
#include "irq.h"
#include "mm.h"

typedef u64 (*PatFn)(u64 block);

static u64 pat_a(u64 b) { return ((b + 1) * 0x9E3779B97F4A7C15ull) ^ 0x5A5A5A5A5A5A5A5Aull; }
static u64 pat_b(u64 b) { return ((b + 7) * 0xD1B54A32D192ED03ull) ^ 0x1234567890ABCDEFull; }

static void pat_fill(u8 *buf, u64 off, size_t len, PatFn fn) {
    for (size_t i = 0; i < len;) {
        u64 pos = off + i;
        u64 w = fn(pos / 512);
        size_t take = MIN((size_t)(512 - pos % 512), len - i);
        for (size_t k = 0; k < take; k++) buf[i + k] = (u8)(w >> (8 * ((pos + k) & 7)));
        i += take;
    }
}

/* Returns the offset of the first byte that differs from the pattern, or -1. */
static long pat_check(const u8 *buf, u64 off, size_t len, PatFn fn) {
    for (size_t i = 0; i < len;) {
        u64 pos = off + i;
        u64 w = fn(pos / 512);
        size_t take = MIN((size_t)(512 - pos % 512), len - i);
        for (size_t k = 0; k < take; k++)
            if (buf[i + k] != (u8)(w >> (8 * ((pos + k) & 7)))) return (long)(i + k);
        i += take;
    }
    return -1;
}

typedef struct {
    BlockDev *d;
    u8     *buf;            /* page aligned scratch buffer */
    size_t  cap;
    int     problems;
} Ctx;

static const unsigned g_mis[] = { 0, 1, 2, 3, 4, 8, 12, 511 };          /* buffer misalignments (bytes) */

static void problem(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void problem(Ctx *c, const char *fmt, ...) {
    c->problems++;
    if (c->problems > 8) return;
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    klog("Kappa", "selftest %s: %s", c->d->name, msg);
}

static bool read_check(Ctx *c, u64 lba, u32 count, unsigned mis, PatFn fn) {
    BlockDev *d = c->d;
    size_t bytes = (size_t)count * d->sector_size;
    if (bytes + mis > c->cap || lba + count > d->sectors) return true;      /* does not fit; not a failure */
    u8 *b = c->buf + mis;
    int rc = blk_read(d, lba, count, b);
    if (rc) { problem(c, "read of %u sectors at lba %lu (buffer +%u) failed: error %d", count, (unsigned long)lba, mis, rc); return false; }
    long bad = pat_check(b, lba * d->sector_size, bytes, fn);
    if (bad >= 0) {
        problem(c, "data mismatch in a read of %u sectors at lba %lu (buffer +%u), first bad byte at +%ld", count, (unsigned long)lba, mis, bad);
        return false;
    }
    return true;
}

static bool write_pat(Ctx *c, u64 lba, u32 count, unsigned mis, PatFn fn) {
    BlockDev *d = c->d;
    size_t bytes = (size_t)count * d->sector_size;
    if (bytes + mis > c->cap || lba + count > d->sectors) return true;
    u8 *b = c->buf + mis;
    pat_fill(b, lba * d->sector_size, bytes, fn);
    int rc = blk_write(d, lba, count, b);
    if (rc) { problem(c, "write of %u sectors at lba %lu (buffer +%u) failed: error %d", count, (unsigned long)lba, mis, rc); return false; }
    return true;
}

static void rate_string(char *out, size_t cap, u64 bytes, u64 ms) {
    if (!ms) ms = 1;
    u64 tenths = bytes * 10000 / ms / (1024 * 1024);
    snprintf(out, cap, "%lu.%lu MB/s", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

static void run_one(BlockDev *d, bool rw) {
    Ctx c = { .d = d };
    u32 ss = d->sector_size;
    u64 total = d->sectors;
    size_t big = 16u << 20;                                         /* the largest single request: 16 MiB, fewer on a small device */
    while (big > ss && big > total * ss) big >>= 1;
    c.buf = NULL;
    for (; big >= (64u << 10); big >>= 1) {
        c.buf = kmalloc(big);                                       /* a power of two: an exact buddy block, page aligned */
        if (c.buf) break;
    }
    if (!c.buf) { klog("Kappa", "selftest %s: no memory for the scratch buffer", d->name); return; }
    c.cap = big;
    u32 bigsec = (u32)(big / ss);
    char rrate[32], wrate[32] = "-";

    /* 1. many request sizes at many starting points and buffer alignments */
    static const u32 sizes[] = { 1, 2, 3, 7, 8, 9, 15, 16, 31, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 1000, 1024, 2047, 2049, 4096 };
    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        u32 n = sizes[i];
        if (n > total) continue;
        read_check(&c, (u64)i * 977 % (total - n + 1), n, g_mis[i % ARRAY_SIZE(g_mis)], pat_a);
        read_check(&c, total - n, n, g_mis[(i + 3) % ARRAY_SIZE(g_mis)], pat_a);       /* flush against the end of the device */
    }
    for (unsigned m = 0; m < ARRAY_SIZE(g_mis); m++)
        read_check(&c, 5 + m, 100, g_mis[m], pat_a);

    /* 2. one big request, timed, then an unaligned one */
    u64 t0 = uptime_ms();
    bool ok = read_check(&c, 0, bigsec, 0, pat_a);
    rate_string(rrate, sizeof rrate, big, uptime_ms() - t0);
    if (ok && bigsec > 1) read_check(&c, 1, bigsec - 1, 2, pat_a);
    if (ok && bigsec > 1) read_check(&c, 1, bigsec - 1, 1, pat_a);

    if (rw) {
        /* 3. overwrite what we can reach with pattern B, using ragged request sizes and alignments */
        u64 limit = MIN(total, (u64)(24u << 20) / ss);
        unsigned k = 0;
        for (u64 lba = 0; lba < limit; k++) {
            u32 n = (u32)MIN((u64)sizes[k % ARRAY_SIZE(sizes)], limit - lba);
            if (!write_pat(&c, lba, n, g_mis[k % ARRAY_SIZE(g_mis)], pat_b)) break;
            lba += n;
        }
        /* 4. read it back with different request boundaries */
        for (u64 l = 0; l < limit;) {
            u32 n = (u32)MIN((u64)bigsec / 2 + 3, limit - l);
            if (!read_check(&c, l, n, g_mis[(l / 7) % ARRAY_SIZE(g_mis)], pat_b)) break;
            l += n;
        }
        /* 5. one big write, timed, flushed and verified */
        pat_fill(c.buf, 0, big, pat_b);
        t0 = uptime_ms();
        int rc = blk_write(d, 0, bigsec, c.buf);
        rate_string(wrate, sizeof wrate, big, uptime_ms() - t0);
        if (rc) problem(&c, "big write of %u sectors failed: error %d", bigsec, rc);
        rc = blk_flush(d);
        if (rc) problem(&c, "flush failed: error %d", rc);
        read_check(&c, 0, bigsec, 0, pat_b);
    }

    klog("Kappa", "selftest %s: %s (%d problems), %lu KiB requests, read %s, write %s", d->name, c.problems ? "FAIL" : "PASS", c.problems,
         (unsigned long)(big >> 10), rrate, wrate);
    kfree(c.buf);
}

void blk_selftest(const char *driver) {
    char spec[96];
    if (!cmdline_value("blkselftest", spec, sizeof spec)) return;
    char *p = spec;
    while (*p) {
        char *end = p;
        while (*end && *end != ',') end++;
        char saved = *end;
        *end = 0;
        bool rw = false;
        char *colon = strchr(p, ':');
        if (colon) { rw = !strcmp(colon + 1, "rw"); *colon = 0; }
        BlockDev *d = blk_find(p);
        if (d && d->driver && !strcmp(d->driver, driver) && !d->parent) run_one(d, rw);
        if (!saved) break;
        p = end + 1;
    }
}
