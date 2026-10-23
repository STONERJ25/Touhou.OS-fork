/* Framebuffer layer. Canonical pixel format is 0x00RRGGBB; the layer swaps channels if the hardware wants RGBX. */
#ifndef K_FB_H
#define K_FB_H

#include "types.h"
#include "bootinfo.h"

typedef struct {
    u32 *front;         /* the visible framebuffer (a write-combining mapping of the display memory) */
    u32 *back;          /* where drawing goes; equals front (nothing double-buffers in the kernel: see fbcon.c) */
    u32  width;
    u32  height;
    u32  pitch;         /* in pixels */
    bool swap_rb;
    bool ready;
    paddr_t phys;
    u64  bench_cycles;  /* TSC cycles the first full-screen fill took (fb_report turns it into MB/s once the TSC rate is known) */
    u64  bench_bytes;
} Fb;

extern Fb g_fb;
extern bool g_fb_force_uc;              /* fb.cache=uc on the kernel command line */

void fb_init(const BootFramebuffer *bf);   /* maps the framebuffer write-combining in the loader's page tables (static memory) */
void fb_remap(void);                    /* switch to an ioremap once the kernel's own page tables are live */
static inline u32 fb_hw(u32 rgb) {
    return g_fb.swap_rb ? ((rgb & 0xFF00) | ((rgb >> 16) & 0xFF) | ((rgb & 0xFF) << 16)) : rgb;
}

void fb_fill_rect(int x, int y, int w, int h, u32 rgb);
void fb_copy_rect(int sx, int sy, int dx, int dy, int w, int h);        /* overlap-safe, within the framebuffer; READS it (slow on real hardware) */
void fb_draw_glyph(int x, int y, const unsigned char *rows, u32 fg, u32 bg);
void fb_blit(int x, int y, int w, int h, const u32 *src, u32 src_pitch);   /* src in canonical format */
void fb_put_span(int x, int y, const u32 *hw_pixels, int n);            /* n pixels already in hardware format, one scanline */
void fb_sync(void);                     /* flush the write-combining buffers (sfence) */
void fb_bench_fill(u32 rgb);            /* a timed full-screen fill; the result is printed by fb_report */
void fb_report(u64 tsc_mhz);            /* one log line: fill speed (call once the TSC is calibrated) */
const char *fb_cache_name(void);        /* "write-combining (PAT)", "uncached ..." */

#endif
