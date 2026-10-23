/* Framebuffer layer. Canonical pixel format is 0x00RRGGBB; the layer swaps channels if the hardware wants RGBX. */
#ifndef K_FB_H
#define K_FB_H

#include "types.h"
#include "bootinfo.h"

typedef struct {
    u32 *front;         /* the visible framebuffer (mapped via the direct map) */
    u32 *back;          /* off-screen buffer; equals front until double buffering is enabled */
    u32  width;
    u32  height;
    u32  pitch;         /* in pixels */
    bool swap_rb;
    bool ready;
    paddr_t phys;
} Fb;

extern Fb g_fb;

void fb_init(const BootFramebuffer *bf);
static inline u32 fb_hw(u32 rgb) {
    return g_fb.swap_rb ? ((rgb & 0xFF00) | ((rgb >> 16) & 0xFF) | ((rgb & 0xFF) << 16)) : rgb;
}

void fb_fill_rect(int x, int y, int w, int h, u32 rgb);
void fb_copy_rect(int sx, int sy, int dx, int dy, int w, int h);        /* overlap-safe, within the framebuffer */
void fb_draw_glyph(int x, int y, const unsigned char *rows, u32 fg, u32 bg);
void fb_blit(int x, int y, int w, int h, const u32 *src, u32 src_pitch);   /* src in canonical format */

#endif
