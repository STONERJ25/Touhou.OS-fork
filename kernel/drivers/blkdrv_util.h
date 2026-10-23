/*
 * Helpers shared by the storage drivers (ahci.c, nvme.c, blk_selftest.c): register access, polling with timeouts,
 * splitting a kernel buffer into physically contiguous DMA runs, and identify-string cleanup.
 * Everything is static inline / macro so the drivers stay independent translation units.
 */
#ifndef K_BLKDRV_UTIL_H
#define K_BLKDRV_UTIL_H

#include "types.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "task.h"
#include "pci.h"
#include "mm.h"
#include "cmdline.h"

/* ---- MMIO (the mapping is uncached, so ordering between volatile accesses is the hardware's problem, not ours) ---- */
static inline u32  mmio_r32(const volatile void *base, u32 off) { return *(const volatile u32 *)((const volatile u8 *)base + off); }
static inline void mmio_w32(volatile void *base, u32 off, u32 v) { *(volatile u32 *)((volatile u8 *)base + off) = v; }

static inline u32 lo32(u64 v) { return (u32)v; }
static inline u32 hi32(u64 v) { return (u32)(v >> 32); }

/*
 * Polls `cond` (an expression that reads hardware) for at most timeout_ms. It spins for ~100 us first, so commands that
 * finish quickly (SSD reads, register handshakes) cost microseconds, then sleeps a millisecond at a time so a slow
 * device (a spinning disk, a link coming up) does not burn the CPU. Evaluates to true if cond became true in time.
 * cond is evaluated several times and must have no side effects other than reading registers.
 */
#define BLK_POLL(cond, timeout_ms) ({                                                   \
    bool _ok = false;                                                                   \
    for (int _s = 0; _s < 32 && !_ok; _s++) {                                           \
        if (cond) _ok = true; else udelay(3);                                           \
    }                                                                                   \
    if (!_ok) {                                                                         \
        u64 _end = uptime_ms() + (u64)(timeout_ms) + 1;                                 \
        for (;;) {                                                                      \
            if (cond) { _ok = true; break; }                                            \
            if (uptime_ms() >= _end) break;                                             \
            task_sleep_ms(1);                                                           \
        }                                                                               \
    }                                                                                   \
    _ok; })

/*
 * Length of the physically contiguous run at the start of the kernel buffer [p, p+len), capped at max bytes; *phys gets
 * the run's physical address. Buffers from the heap or dma_alloc live in the direct map, so they are normally one run.
 */
static inline size_t phys_run(const void *p, size_t len, size_t max, paddr_t *phys) {
    const u8 *b = p;
    paddr_t first = virt_to_phys(b);
    *phys = first;
    size_t run = MIN(len, (size_t)(PAGE_SIZE - ((u64)b & (PAGE_SIZE - 1))));
    while (run < len && run < max) {
        if (virt_to_phys(b + run) != first + run) break;
        run += MIN((size_t)PAGE_SIZE, len - run);
    }
    return MIN(run, max);
}

/*
 * Nothing above 4 GiB is ever handed to a storage controller: the drivers' own structures come from dma_alloc_low (a dma_alloc
 * block that is checked to lie below 4 GiB), and a caller's buffer is used for DMA only if all of it is below the limit, otherwise
 * the data goes through a bounce buffer from dma_alloc_low. That holds whether or not the controller says it can do 64-bit DMA
 * (AHCI CAP.S64A), so a 32-bit-only controller and a 64-bit one are driven identically.
 */
#define DMA_LIMIT 0x100000000ull

static inline void *dma_alloc_low(size_t size, paddr_t *phys) {
    paddr_t p;
    void *v = dma_alloc(size, &p);
    if (!v) return NULL;
    if (p + ALIGN_UP(size, (size_t)PAGE_SIZE) > DMA_LIMIT) { dma_free(v, size); return NULL; }
    *phys = p;
    return v;
}

/* True if every byte of the kernel buffer [buf, buf+len) lies below limit (physical). */
static inline bool buf_dma_low(const void *buf, size_t len, u64 limit) {
    const u8 *b = buf;
    while (len) {
        paddr_t ph;
        size_t run = phys_run(b, len, len, &ph);
        if (ph + run > limit) return false;
        b += run;
        len -= run;
    }
    return true;
}

/* "blk.ro" on the kernel command line: every disk these drivers register is read-only (blk_write returns -EROFS). */
static inline bool blk_ro_requested(void) { return cmdline_flag("blk.ro"); }

/* ---- identify strings ---- */

/* Trims and sanitises a fixed-width, space padded ASCII field (NVMe identify strings). */
static inline void id_string(char *dst, size_t dstsz, const char *src, size_t n) {
    size_t len = 0;
    for (size_t i = 0; i < n && len + 1 < dstsz; i++) {
        char c = src[i];
        if (c == 0) break;
        dst[len++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    while (len && dst[len - 1] == ' ') len--;
    size_t start = 0;
    while (start < len && dst[start] == ' ') start++;
    if (start) memmove(dst, dst + start, len - start);
    dst[len - start] = 0;
}

/* Same for ATA identify strings, which store two characters per 16-bit word, first character in the high byte. */
static inline void ata_string(char *dst, size_t dstsz, const u16 *words, size_t nwords) {
    char raw[64];
    size_t n = 0;
    for (size_t i = 0; i < nwords && n + 2 <= sizeof raw; i++) {
        raw[n++] = (char)(words[i] >> 8);
        raw[n++] = (char)(words[i] & 0xFF);
    }
    id_string(dst, dstsz, raw, n);
}

/* "sda" .. "sdz", "sdaa" .. : the Linux scheme for the n-th SATA disk. */
static inline void sd_name(char *out, size_t cap, int index) {
    char letters[8];
    int n = 0;
    for (int i = index; n < 6; i = i / 26 - 1) {
        letters[n++] = (char)('a' + i % 26);
        if (i < 26) break;
    }
    size_t o = 0;
    if (cap < 4) return;
    out[o++] = 's'; out[o++] = 'd';
    while (n && o + 1 < cap) out[o++] = letters[--n];
    out[o] = 0;
}

#endif
