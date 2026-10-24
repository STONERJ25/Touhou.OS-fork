/*
 * kappa_gfx2d: pre-rendered sprites, fast blits and anti-aliased procedural painting.
 *
 * Meant for games and anything else that draws the same art many times per second: paint a sprite once at start-up with
 * the ks_* primitives (circles, ellipses, polygons, capsules, glows), then put it on a KCanvas with ks_blit*.
 * The KCanvas functions here write pixels directly and expect a 1x canvas: a program that uses them on its window calls
 * kapp_set_hidpi(false) first (see kappa.h), and the window server enlarges its windows on a hi-dpi screen.
 *
 * Pixel encoding of a KSprite (stride = width):
 *     0x00RRGGBB            opaque colour
 *     KS_KEY (0x01000000)   fully transparent
 *     0xAARRGGBB, AA 2..254 translucent (alpha = AA); only the generic ks_blit() honours it
 * Sprites that never contain translucent pixels ("keyed") can use ks_blit_key(), which is a plain colour-key blit
 * without any arithmetic: that is what bullets and other swarm objects should use.
 */
#ifndef KAPPA_GFX2D_H
#define KAPPA_GFX2D_H

#include <kappa.h>

#define KS_KEY 0x01000000u

typedef struct KSprite {
    int w, h;
    uint32_t *px;                   /* w * h pixels */
    int ax, ay;                     /* anchor (hot spot), used by the *_at blits */
} KSprite;

/* ---------------------------------------------------------------- lifetime */
KSprite *ks_new(int w, int h);                                   /* all transparent, anchor in the middle */
void     ks_free(KSprite *s);
KSprite *ks_clone(const KSprite *s);
void     ks_clear(KSprite *s);
void     ks_fill(KSprite *s, uint32_t rgb);                      /* opaque fill */
void     ks_set_anchor(KSprite *s, int ax, int ay);
KSprite *ks_sub(const KSprite *s, int x, int y, int w, int h);   /* copy of a part */
void     ks_copy_in(KSprite *dst, int x, int y, const KSprite *src);   /* plain copy, no blending (clipped) */

/* ---------------------------------------------------------------- blitting (clipped to c->clip) */
void ks_blit(KCanvas *c, int x, int y, const KSprite *s);        /* opaque + transparent + translucent pixels */
void ks_blit_at(KCanvas *c, int x, int y, const KSprite *s);     /* same, with the anchor at (x, y) */
void ks_blit_key(KCanvas *c, int x, int y, const KSprite *s);    /* colour key only: fastest, ignores translucency */
void ks_blit_key_at(KCanvas *c, int x, int y, const KSprite *s);
void ks_blit_add(KCanvas *c, int x, int y, const KSprite *s);    /* adds the sprite colour (saturating); alpha ignored, KEY = nothing */
void ks_blit_add_at(KCanvas *c, int x, int y, const KSprite *s);
void ks_blit_fade(KCanvas *c, int x, int y, const KSprite *s, int a256);   /* generic blit scaled by a global alpha 0..256 */
void ks_blit_fade_at(KCanvas *c, int x, int y, const KSprite *s, int a256);
void ks_blit_part_key(KCanvas *c, int x, int y, const KSprite *s, KRect part);   /* colour-key blit of part of a sprite */

/* ---------------------------------------------------------------- bulk pixel operations (fast: rep movsq / stosq) */
void ks_copy32(uint32_t *dst, const uint32_t *src, size_t n);    /* n pixels, regions must not overlap */
void ks_fill32(uint32_t *dst, uint32_t v, size_t n);
void ks_fill_rect(KCanvas *c, KRect r, uint32_t rgb);            /* opaque rectangle fast path */
void ks_copy_rect(KCanvas *dst, int dx, int dy, const KCanvas *src, KRect part);   /* canvas to canvas, clipped, no blending */
void ks_shade_rect(KCanvas *c, KRect r, int level256);           /* multiplies all channels: 256 = unchanged, 128 = half as bright */
void ks_tint_rect(KCanvas *c, KRect r, uint32_t rgb, int a256);  /* blends the region towards rgb (a256 0..256) */
void ks_add_rect(KCanvas *c, KRect r, uint32_t rgb);             /* adds a colour to every pixel (saturating) */
void ks_half_rect(KCanvas *c, KRect r, uint32_t rgb);            /* 50% blend with rgb (cheap) */
void ks_stipple_rect(KCanvas *c, KRect r, uint32_t rgb, int phase);   /* paints every other pixel (checkerboard): a fake 50% blend */
uint32_t ks_add_px(uint32_t a, uint32_t b);                      /* per-channel saturating add */
uint32_t ks_mul_px(uint32_t a, int level256);                    /* per-channel multiply */
uint32_t ks_lerp_px(uint32_t a, uint32_t b, int t256);

/* ---------------------------------------------------------------- colour helpers */
uint32_t ks_hsv(int h360, int s100, int v100);                   /* 0x00RRGGBB */
uint32_t ks_enc(uint32_t rgb, int alpha);                        /* encodes a straight-alpha colour as a sprite pixel */
int      ks_alpha_of(uint32_t pixel);                            /* 0..255 */

/* ---------------------------------------------------------------- painting (for generating art; floats, anti-aliased) */
void ks_put(KSprite *s, int x, int y, uint32_t rgb, int alpha);  /* source-over one pixel */
void ks_disc(KSprite *s, float cx, float cy, float r, uint32_t rgb, int alpha);
void ks_disc_grad(KSprite *s, float cx, float cy, float r, uint32_t inner, uint32_t outer, float hl_dx, float hl_dy);   /* radial gradient, highlight offset in units of r */
void ks_ring(KSprite *s, float cx, float cy, float r, float width, uint32_t rgb, int alpha);
void ks_ellipse(KSprite *s, float cx, float cy, float rx, float ry, float angle, uint32_t rgb, int alpha);
void ks_ellipse_grad(KSprite *s, float cx, float cy, float rx, float ry, float angle, uint32_t inner, uint32_t outer, float hl_dx, float hl_dy);
void ks_line(KSprite *s, float x0, float y0, float x1, float y1, float width, uint32_t rgb, int alpha);   /* capsule */
void ks_rect(KSprite *s, float x, float y, float w, float h, uint32_t rgb, int alpha);
void ks_poly(KSprite *s, const float *xy, int n, uint32_t rgb, int alpha);                               /* even-odd fill, 4x vertical supersampling */
void ks_poly_grad_v(KSprite *s, const float *xy, int n, uint32_t top, uint32_t bottom);                  /* polygon filled with a vertical gradient */
void ks_glow(KSprite *s, float cx, float cy, float r, uint32_t rgb, int peak_alpha);                     /* soft round glow (quadratic falloff) */
void ks_star(KSprite *s, float cx, float cy, float ro, float ri, int points, float angle, uint32_t rgb, int alpha);
void ks_outline(KSprite *s, uint32_t rgb);                       /* 1 px outline around the non-transparent area (4-neighbourhood) */
void ks_threshold(KSprite *s, int min_alpha);                    /* translucent pixels become opaque (alpha >= min_alpha) or transparent */
void ks_flatten(KSprite *s, uint32_t backdrop);                  /* composite translucent pixels onto backdrop so the sprite is "keyed" */
void ks_darken_edge(KSprite *s, int percent);                    /* shades pixels that touch transparency (cheap faux outline) */

/* Text with the built-in 8x16 font, as a sprite: scale 1..8, optional outline colour (0xFFFFFFFF = none). */
KSprite *ks_text(const char *s, int scale, uint32_t fg, uint32_t outline);
void     ks_text_draw(KCanvas *c, int x, int y, const char *s, int scale, uint32_t fg, uint32_t outline);   /* one-shot, slower: caches nothing */
int      ks_text_w(const char *s, int scale);

#endif
