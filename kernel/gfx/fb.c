#include "fb.h"
#include "kernel.h"
#include "mm.h"

Fb g_fb;

void fb_init(const BootFramebuffer *bf) {
    memset(&g_fb, 0, sizeof g_fb);
    if (!bf->base || bf->bpp != 32) return;
    g_fb.phys    = bf->base;
    g_fb.width   = bf->width;
    g_fb.height  = bf->height;
    g_fb.pitch   = bf->pitch / 4;
    g_fb.swap_rb = bf->format == BOOTFB_RGBX;
    g_fb.front   = phys_to_virt(bf->base);      /* the loader's direct map covers the first 4 GiB */
    g_fb.back    = g_fb.front;
    g_fb.ready   = true;
}

void fb_remap(void) {
    if (!g_fb.ready) return;
    bool direct = g_fb.back == g_fb.front;
    g_fb.front = ioremap(g_fb.phys, (size_t)g_fb.pitch * g_fb.height * 4, PTE_PWT);
    if (direct) g_fb.back = g_fb.front;
}

static inline void fill32(u32 *dst, u32 v, size_t n) {
    __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(n) : "a"(v) : "memory");
}

static inline void copy32(u32 *dst, const u32 *src, size_t n) {
    __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

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
}

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
}
