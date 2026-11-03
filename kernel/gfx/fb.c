/*
 * The linear framebuffer the firmware's GOP left behind.
 *
 * On a PC this is device memory (the integrated GPU's aperture), not RAM, and how it is mapped decides how fast the screen
 * is: write-combining (the PAT, programmed in cpu_init) lets the CPU burst whole cache lines; uncached memory turns every
 * store into its own bus transaction and a full-screen update into a slideshow; and READING write-combined or uncached
 * memory is slower still. So nothing here ever reads the framebuffer back, and the text console (fbcon.c) keeps its own
 * copy of what is on screen instead of scrolling pixels in place.
 *
 * The very first pixels are drawn before the page allocator exists, so the loader's direct map (write-back!) would be the only
 * mapping available. fb_init therefore adds a write-combining window of its own to the loader's page tables, in static memory;
 * fb_remap later replaces it with a normal ioremap once the kernel's own tables are live.
 *
 * Kernel command line: fb.cache=uc maps the framebuffer uncached (for comparing speeds on real hardware: if "uc" is no slower
 * than the default, the write-combining mapping is not taking effect).
 */
#include "fb.h"
#include "kernel.h"
#include "mm.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "cmdline.h"
#include "fbcon.h"

Fb g_fb;
bool g_fb_force_uc;                             /* fb.cache=uc */

extern u64 g_kernel_phys, g_kernel_virt;

/* ---------------------------------------------------------------- early write-combining window */

#define EARLY_FB_VIRT   0xFFFFC00000000000UL    /* the kernel's MMIO window; the loader's tables, which hold this, are dropped by vmm_init */
#define EARLY_PT_PAGES  32                      /* 64 MiB of framebuffer: 4096 x 4096 pixels at most */

static u64 g_early_pdpt[512] ALIGNED(4096);
static u64 g_early_pd[512] ALIGNED(4096);
static u64 g_early_pt[EARLY_PT_PAGES][512] ALIGNED(4096);

static paddr_t kimg_phys(const void *p) { return g_kernel_phys + ((u64)p - g_kernel_virt); }

/* Maps [phys, phys + size) write-combining at EARLY_FB_VIRT in the page tables the loader handed over. NULL if that is impossible. */
static void *early_map(paddr_t phys, size_t size) {
    u64 first = phys & PAGE_MASK;
    size_t pages = (size_t)((phys + size - first + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (pages > EARLY_PT_PAGES * 512) return NULL;
    u64 *pml4 = phys_to_virt(read_cr3() & PTE_ADDR);
    unsigned slot = (EARLY_FB_VIRT >> 39) & 511;
    if (pml4[slot] & PTE_P) return NULL;                                    /* not ours to take */
    u64 flags = PTE_P | PTE_W | PTE_NX | (g_fb_force_uc ? (PTE_PCD | PTE_PWT) : pte_wc_flags());
    for (size_t i = 0; i < pages; i++)
        g_early_pt[i >> 9][i & 511] = (first + ((u64)i << PAGE_SHIFT)) | flags;
    for (size_t t = 0; t < (pages + 511) >> 9; t++) g_early_pd[t] = kimg_phys(g_early_pt[t]) | PTE_P | PTE_W;
    g_early_pdpt[0] = kimg_phys(g_early_pd) | PTE_P | PTE_W;
    pml4[slot] = kimg_phys(g_early_pdpt) | PTE_P | PTE_W;
    write_cr3(read_cr3());
    return (void *)(EARLY_FB_VIRT + (phys & 0xFFF));
}

/* ---------------------------------------------------------------- setup */

void fb_init(const BootFramebuffer *bf) {
    memset(&g_fb, 0, sizeof g_fb);
    if (!bf->base || bf->bpp != 32 || !bf->width || !bf->height || bf->pitch < bf->width * 4) return;
    char opt[8];
    g_fb_force_uc = cmdline_value("fb.cache", opt, sizeof opt) && !strcmp(opt, "uc");
    g_fb.phys    = bf->base;
    g_fb.width   = bf->width;
    g_fb.height  = bf->height;
    g_fb.pitch   = bf->pitch / 4;
    g_fb.swap_rb = bf->format == BOOTFB_RGBX;
    g_fb.front   = early_map(bf->base, (size_t)bf->pitch * bf->height);
    if (!g_fb.front) {
        /* Bigger than the static window, or the slot is taken: the loader's write-back alias still works, it is only slow and
         * not guaranteed to show the pixels at once. fb_remap fixes it as soon as the kernel's own tables exist. */
        g_fb.front = phys_to_virt(bf->base);
    } else {
        __asm__ volatile ("wbinvd" ::: "memory");   /* the firmware may have used the same memory write-back: no stale lines may come back later */
    }
    g_fb.back    = g_fb.front;
    g_fb.ready   = true;
}

static bool g_remapped;                         /* g_fb.front is the kernel's own ioremap now (fb_set_map_flags may edit it) */

void fb_remap(void) {
    if (!g_fb.ready) return;
    bool direct = g_fb.back == g_fb.front;
    u64 flags = g_fb_force_uc ? (PTE_PCD | PTE_PWT) : pte_wc_flags();
    g_fb.front = ioremap(g_fb.phys, (size_t)g_fb.pitch * g_fb.height * 4, flags);
    if (direct) g_fb.back = g_fb.front;
    g_remapped = true;
}

/* Rewrites the cache type bits of every page of the kernel's framebuffer mapping and flushes the TLB entries (vmm_map does). The
 * write-combining buffers are drained first so that no store is in flight while the type changes. Only the kernel's own mapping:
 * what a process mapped through /dev/fb0 keeps its flags (fb_tune runs before any process exists). */
void fb_set_map_flags(u64 flags) {
    if (!g_fb.ready || !g_remapped) return;
    size_t size = (size_t)g_fb.pitch * g_fb.height * 4;
    u64 virt = (u64)g_fb.front & PAGE_MASK;
    paddr_t phys = g_fb.phys & PAGE_MASK;
    u64 pages = ((g_fb.phys & 0xFFF) + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    u64 f = irq_save();
    fb_sync();
    for (u64 i = 0; i < pages; i++) vmm_map(vmm_kernel_pml4(), virt + (i << PAGE_SHIFT), phys + (i << PAGE_SHIFT), PTE_W | PTE_NX | flags);
    irq_restore(f);
}

/* ---------------------------------------------------------------- drawing primitives */

/* Eight bytes per store: half the bus transactions of a dword loop on uncached memory, whole cache lines on write-combining. */
static inline void fill32(u32 *dst, u32 v, size_t n) {
    if (n && ((u64)dst & 4)) { *dst++ = v; n--; }
    u64 q = ((u64)v << 32) | v;
    size_t pairs = n >> 1;
    __asm__ volatile ("rep stosq" : "+D"(dst), "+c"(pairs) : "a"(q) : "memory");
    if (n & 1) *dst = v;
}

static inline void copy32(u32 *dst, const u32 *src, size_t n) {
    if (n && ((u64)dst & 4)) { *dst++ = *src++; n--; }
    size_t pairs = n >> 1;
    __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(pairs) : : "memory");
    if (n & 1) *dst = *src;
}

/* Pushes out whatever the write-combining buffers still hold: without it the last partial line of a drawing pass may sit in the CPU
 * until something else happens to evict it, and a screen that has stopped updating (a panic message) would never show it. */
void fb_sync(void) { __asm__ volatile ("sfence" ::: "memory"); }

void fb_fill_rect(int x, int y, int w, int h, u32 rgb) {
    if (!g_fb.ready) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_fb.width)  w = (int)g_fb.width - x;
    if (y + h > (int)g_fb.height) h = (int)g_fb.height - y;
    if (w <= 0 || h <= 0) return;
    u32 px = fb_hw(rgb);
    for (int row = 0; row < h; row++)
        fill32(g_fb.back + (size_t)(y + row) * g_fb.pitch + x, px, (size_t)w);
    fb_sync();
}

/* Moves pixels inside the framebuffer. READS the framebuffer: fine in RAM (a back buffer), crawls on real display memory; the text
 * console no longer uses it. */
void fb_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    if (!g_fb.ready || w <= 0 || h <= 0) return;
    if (dy <= sy) {
        for (int row = 0; row < h; row++)
            memmove(g_fb.back + (size_t)(dy + row) * g_fb.pitch + dx,
                    g_fb.back + (size_t)(sy + row) * g_fb.pitch + sx, (size_t)w * 4);
    } else {
        for (int row = h - 1; row >= 0; row--)
            memmove(g_fb.back + (size_t)(dy + row) * g_fb.pitch + dx,
                    g_fb.back + (size_t)(sy + row) * g_fb.pitch + sx, (size_t)w * 4);
    }
    fb_sync();
}

void fb_draw_glyph(int x, int y, const unsigned char *rows, u32 fg, u32 bg) {
    if (!g_fb.ready) return;
    if (x < 0 || y < 0 || x + 8 > (int)g_fb.width || y + 16 > (int)g_fb.height) return;
    u32 f = fb_hw(fg), b = fb_hw(bg);
    for (int row = 0; row < 16; row++) {
        u32 *dst = g_fb.back + (size_t)(y + row) * g_fb.pitch + x;
        unsigned bits = rows[row];
        for (int col = 0; col < 8; col++)
            dst[col] = (bits & (0x80u >> col)) ? f : b;
    }
    fb_sync();
}

void fb_blit(int x, int y, int w, int h, const u32 *src, u32 src_pitch) {
    if (!g_fb.ready) return;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= (int)g_fb.height) continue;
        int x0 = x, n = w;
        const u32 *s = src + (size_t)row * src_pitch;
        if (x0 < 0) { s -= x0; n += x0; x0 = 0; }
        if (x0 + n > (int)g_fb.width) n = (int)g_fb.width - x0;
        if (n <= 0) continue;
        u32 *d = g_fb.back + (size_t)dy * g_fb.pitch + x0;
        if (!g_fb.swap_rb) copy32(d, s, (size_t)n);
        else for (int i = 0; i < n; i++) d[i] = fb_hw(s[i]);
    }
    fb_sync();
    fbcon_taint_rect(x, y, w, h);               /* the text console does not know about these pixels: repaint the cells when they scroll */
}

/* One scanline segment already in hardware pixel format (fbcon renders a whole text row into RAM first, then calls this 16 times). */
void fb_put_span(int x, int y, const u32 *hw_pixels, int n) {
    if (!g_fb.ready || y < 0 || y >= (int)g_fb.height || x < 0 || n <= 0) return;
    if (x + n > (int)g_fb.width) n = (int)g_fb.width - x;
    if (n > 0) copy32(g_fb.back + (size_t)y * g_fb.pitch + x, hw_pixels, (size_t)n);
}

/* ---------------------------------------------------------------- speed measurement */

/* The first full-screen fill is timed with the TSC (its rate is not known yet, so fb_report prints it later). */
void fb_bench_fill(u32 rgb) {
    if (!g_fb.ready) return;
    u64 t0 = rdtsc();
    fb_fill_rect(0, 0, (int)g_fb.width, (int)g_fb.height, rgb);
    g_fb.bench_cycles = rdtsc() - t0;
    g_fb.bench_bytes = (u64)g_fb.width * g_fb.height * 4;
}

const char *fb_cache_name(void) {
    return g_fb_force_uc ? "uncached (fb.cache=uc)" : g_cpu.wc_by_mtrr ? "write-combining (MTRR)" :
           g_cpu.pat_wc ? "write-combining (PAT)" : "uncached (this CPU has no PAT)";
}

void fb_report(u64 tsc_mhz) {
    if (!g_fb.ready || !g_fb.bench_cycles || !tsc_mhz) return;
    u64 us = g_fb.bench_cycles / tsc_mhz;                       /* cycles / (cycles per microsecond) */
    u64 mbps = us ? g_fb.bench_bytes / us : 0;                  /* bytes per microsecond = MB/s */
    klog("Kappa", "framebuffer speed: a full-screen fill (%lu KiB) took %lu.%lu ms, ~%lu MB/s", (unsigned long)(g_fb.bench_bytes >> 10),
         (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 100), (unsigned long)mbps);
    if (mbps && mbps < FB_SLOW_MBPS && !g_fb_force_uc)
        klog("Kappa", "the framebuffer is slow (under %d MB/s): the mapping is probably not write-combining and the console will crawl "
                      "(compare with fb.cache=uc on the kernel command line); after the PCI scan the kernel tries a write-combining MTRR "
                      "(fb.mtrr=off stops that)", FB_SLOW_MBPS);
}
