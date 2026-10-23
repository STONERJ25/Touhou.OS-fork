/* kappa_gfx2d: sprites, fast blits and anti-aliased painting. See kappa_gfx2d.h for the pixel encoding. */
#include <kappa_gfx2d.h>
#include <font8x16.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------- bulk pixel operations */

/* The libc memcpy is a byte-wise rep movsb, which is slow when the CPU is emulated; moving 8 bytes per step is 4x fewer steps. */
void ks_copy32(uint32_t *dst, const uint32_t *src, size_t n) {
    size_t q = n >> 1;
    if (q) __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) : : "memory");
    if (n & 1) *dst = *src;
}

void ks_fill32(uint32_t *dst, uint32_t v, size_t n) {
    size_t q = n >> 1;
    uint64_t vv = ((uint64_t)v << 32) | v;
    if (q) __asm__ volatile ("rep stosq" : "+D"(dst), "+c"(q) : "a"(vv) : "memory");
    if (n & 1) *dst = v;
}

void ks_fill_rect(KCanvas *c, KRect r, uint32_t rgb) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r)) return;
    uint32_t *row = c->px + (size_t)r.y * c->stride + r.x;
    for (int y = 0; y < r.h; y++, row += c->stride) ks_fill32(row, rgb, (size_t)r.w);
}

void ks_copy_rect(KCanvas *dst, int dx, int dy, const KCanvas *src, KRect part) {
    part = kr_intersect(part, kr(0, 0, src->w, src->h));
    KRect d = kr_intersect(kr(dx, dy, part.w, part.h), dst->clip);
    if (kr_empty(d)) return;
    int sx = part.x + (d.x - dx), sy = part.y + (d.y - dy);
    for (int y = 0; y < d.h; y++)
        ks_copy32(dst->px + (size_t)(d.y + y) * dst->stride + d.x, src->px + (size_t)(sy + y) * src->stride + sx, (size_t)d.w);
}

uint32_t ks_add_px(uint32_t a, uint32_t b) {
    const uint32_t m = 0x7F7F7F7Fu;
    uint32_t s = (a & m) + (b & m);
    uint32_t t = s ^ ((a ^ b) & ~m);
    uint32_t carry = ((a & b) | ((a | b) & ~t)) & 0x80808080u;
    uint32_t sat = (carry >> 7) * 0xFFu;
    return (t | sat) & 0x00FFFFFFu;
}

uint32_t ks_mul_px(uint32_t a, int lv) {
    if (lv >= 256) return a & 0xFFFFFF;
    if (lv <= 0) return 0;
    uint32_t rb = (((a & 0xFF00FFu) * (uint32_t)lv) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * (uint32_t)lv) >> 8) & 0x00FF00u;
    return rb | g;
}

uint32_t ks_lerp_px(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a & 0xFFFFFF;
    if (t >= 256) return b & 0xFFFFFF;
    uint32_t it = 256u - (uint32_t)t, ut = (uint32_t)t;
    uint32_t rb = (((a & 0xFF00FFu) * it + (b & 0xFF00FFu) * ut) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * it + (b & 0x00FF00u) * ut) >> 8) & 0x00FF00u;
    return rb | g;
}

void ks_shade_rect(KCanvas *c, KRect r, int lv) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r) || lv >= 256) return;
    uint32_t ulv = lv < 0 ? 0u : (uint32_t)lv;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = c->px + (size_t)(r.y + y) * c->stride + r.x;
        for (int x = 0; x < r.w; x++) {
            uint32_t v = p[x];
            p[x] = ((((v & 0xFF00FFu) * ulv) >> 8) & 0xFF00FFu) | ((((v & 0x00FF00u) * ulv) >> 8) & 0x00FF00u);
        }
    }
}

void ks_tint_rect(KCanvas *c, KRect r, uint32_t rgb, int a256) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r) || a256 <= 0) return;
    if (a256 > 256) a256 = 256;
    uint32_t ua = (uint32_t)a256, ia = 256u - ua;
    uint32_t trb = (rgb & 0xFF00FFu) * ua, tg = (rgb & 0x00FF00u) * ua;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = c->px + (size_t)(r.y + y) * c->stride + r.x;
        for (int x = 0; x < r.w; x++) {
            uint32_t v = p[x];
            p[x] = ((((v & 0xFF00FFu) * ia + trb) >> 8) & 0xFF00FFu) | ((((v & 0x00FF00u) * ia + tg) >> 8) & 0x00FF00u);
        }
    }
}

void ks_add_rect(KCanvas *c, KRect r, uint32_t rgb) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r)) return;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = c->px + (size_t)(r.y + y) * c->stride + r.x;
        for (int x = 0; x < r.w; x++) p[x] = ks_add_px(p[x], rgb);
    }
}

void ks_half_rect(KCanvas *c, KRect r, uint32_t rgb) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r)) return;
    uint32_t half = (rgb & 0xFEFEFEu) >> 1;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = c->px + (size_t)(r.y + y) * c->stride + r.x;
        for (int x = 0; x < r.w; x++) p[x] = ((p[x] & 0xFEFEFEu) >> 1) + half;
    }
}

void ks_stipple_rect(KCanvas *c, KRect r, uint32_t rgb, int phase) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r)) return;
    for (int y = 0; y < r.h; y++) {
        uint32_t *p = c->px + (size_t)(r.y + y) * c->stride + r.x;
        int start = (r.x + r.y + y + phase) & 1;
        for (int x = start; x < r.w; x += 2) p[x] = rgb;
    }
}

/* ---------------------------------------------------------------- colour helpers */

uint32_t ks_hsv(int h, int s, int v) {
    h %= 360;
    if (h < 0) h += 360;
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    int c = v * s * 255 / 10000;                       /* chroma 0..255 */
    int m = v * 255 / 100 - c;
    int hh = h / 60, f = (h % 60) * 255 / 60;
    int x = hh & 1 ? c * (255 - f) / 255 : c * f / 255;
    int r = 0, g = 0, b = 0;
    switch (hh) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    return KRGB(r + m, g + m, b + m);
}

uint32_t ks_enc(uint32_t rgb, int a) {
    if (a >= 253) return rgb & 0xFFFFFFu;
    if (a <= 1) return KS_KEY;
    return ((uint32_t)a << 24) | (rgb & 0xFFFFFFu);
}

int ks_alpha_of(uint32_t p) {
    if (p < KS_KEY) return 255;
    if (p == KS_KEY) return 0;
    return (int)(p >> 24);
}

/* ---------------------------------------------------------------- sprites */

KSprite *ks_new(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    KSprite *s = malloc(sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->ax = w / 2;
    s->ay = h / 2;
    s->px = malloc((size_t)w * (size_t)h * 4);
    if (!s->px) { free(s); return NULL; }
    ks_fill32(s->px, KS_KEY, (size_t)w * (size_t)h);
    return s;
}

void ks_free(KSprite *s) {
    if (!s) return;
    free(s->px);
    free(s);
}

KSprite *ks_clone(const KSprite *s) {
    KSprite *n = ks_new(s->w, s->h);
    if (!n) return NULL;
    memcpy(n->px, s->px, (size_t)s->w * (size_t)s->h * 4);
    n->ax = s->ax;
    n->ay = s->ay;
    return n;
}

void ks_clear(KSprite *s) { ks_fill32(s->px, KS_KEY, (size_t)s->w * (size_t)s->h); }
void ks_fill(KSprite *s, uint32_t rgb) { ks_fill32(s->px, rgb & 0xFFFFFFu, (size_t)s->w * (size_t)s->h); }
void ks_set_anchor(KSprite *s, int ax, int ay) { s->ax = ax; s->ay = ay; }

KSprite *ks_sub(const KSprite *s, int x, int y, int w, int h) {
    KSprite *n = ks_new(w, h);
    if (!n) return NULL;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            if (sx >= 0 && sy >= 0 && sx < s->w && sy < s->h) n->px[j * w + i] = s->px[sy * s->w + sx];
        }
    return n;
}

void ks_copy_in(KSprite *dst, int x, int y, const KSprite *src) {
    for (int j = 0; j < src->h; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= dst->h) continue;
        for (int i = 0; i < src->w; i++) {
            int dx = x + i;
            if (dx < 0 || dx >= dst->w) continue;
            dst->px[dy * dst->w + dx] = src->px[j * src->w + i];
        }
    }
}

/* ---------------------------------------------------------------- blits */

static inline bool clip_blit(const KCanvas *c, int x, int y, int w, int h, int *x0, int *y0, int *x1, int *y1) {
    int cx0 = c->clip.x, cy0 = c->clip.y, cx1 = c->clip.x + c->clip.w, cy1 = c->clip.y + c->clip.h;
    *x0 = x > cx0 ? x : cx0;
    *y0 = y > cy0 ? y : cy0;
    *x1 = x + w < cx1 ? x + w : cx1;
    *y1 = y + h < cy1 ? y + h : cy1;
    return *x1 > *x0 && *y1 > *y0;
}

static inline uint32_t blend_px(uint32_t d, uint32_t p) {
    uint32_t a = p >> 24;
    a += a >> 7;
    uint32_t ia = 256u - a;
    uint32_t rb = (((p & 0xFF00FFu) * a + (d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
    uint32_t g = (((p & 0x00FF00u) * a + (d & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
    return rb | g;
}

void ks_blit(KCanvas *c, int x, int y, const KSprite *s) {
    int x0, y0, x1, y1;
    if (!clip_blit(c, x, y, s->w, s->h, &x0, &y0, &x1, &y1)) return;
    int n = x1 - x0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = s->px + (size_t)(row - y) * s->w + (x0 - x);
        uint32_t *dp = c->px + (size_t)row * c->stride + x0;
        for (int i = 0; i < n; i++) {
            uint32_t p = sp[i];
            if (p < KS_KEY) dp[i] = p;
            else if (p != KS_KEY) dp[i] = blend_px(dp[i], p);
        }
    }
}

void ks_blit_at(KCanvas *c, int x, int y, const KSprite *s) { ks_blit(c, x - s->ax, y - s->ay, s); }

void ks_blit_key(KCanvas *c, int x, int y, const KSprite *s) {
    int x0, y0, x1, y1;
    if (!clip_blit(c, x, y, s->w, s->h, &x0, &y0, &x1, &y1)) return;
    int n = x1 - x0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = s->px + (size_t)(row - y) * s->w + (x0 - x);
        uint32_t *dp = c->px + (size_t)row * c->stride + x0;
        for (int i = 0; i < n; i++) {
            uint32_t p = sp[i], d = dp[i];
            dp[i] = p == KS_KEY ? d : p;
        }
    }
}

void ks_blit_key_at(KCanvas *c, int x, int y, const KSprite *s) { ks_blit_key(c, x - s->ax, y - s->ay, s); }

void ks_blit_part_key(KCanvas *c, int x, int y, const KSprite *s, KRect part) {
    part = kr_intersect(part, kr(0, 0, s->w, s->h));
    int x0, y0, x1, y1;
    if (!clip_blit(c, x, y, part.w, part.h, &x0, &y0, &x1, &y1)) return;
    int n = x1 - x0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = s->px + (size_t)(part.y + row - y) * s->w + part.x + (x0 - x);
        uint32_t *dp = c->px + (size_t)row * c->stride + x0;
        for (int i = 0; i < n; i++) {
            uint32_t p = sp[i], d = dp[i];
            dp[i] = p == KS_KEY ? d : p;
        }
    }
}

void ks_blit_add(KCanvas *c, int x, int y, const KSprite *s) {
    int x0, y0, x1, y1;
    if (!clip_blit(c, x, y, s->w, s->h, &x0, &y0, &x1, &y1)) return;
    int n = x1 - x0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = s->px + (size_t)(row - y) * s->w + (x0 - x);
        uint32_t *dp = c->px + (size_t)row * c->stride + x0;
        for (int i = 0; i < n; i++) {
            uint32_t p = sp[i];
            if (p == KS_KEY) continue;
            dp[i] = ks_add_px(dp[i], p & 0xFFFFFFu);
        }
    }
}

void ks_blit_add_at(KCanvas *c, int x, int y, const KSprite *s) { ks_blit_add(c, x - s->ax, y - s->ay, s); }

void ks_blit_fade(KCanvas *c, int x, int y, const KSprite *s, int a256) {
    if (a256 <= 0) return;
    if (a256 >= 256) { ks_blit(c, x, y, s); return; }
    int x0, y0, x1, y1;
    if (!clip_blit(c, x, y, s->w, s->h, &x0, &y0, &x1, &y1)) return;
    int n = x1 - x0;
    for (int row = y0; row < y1; row++) {
        const uint32_t *sp = s->px + (size_t)(row - y) * s->w + (x0 - x);
        uint32_t *dp = c->px + (size_t)row * c->stride + x0;
        for (int i = 0; i < n; i++) {
            uint32_t p = sp[i];
            if (p == KS_KEY) continue;
            int pa = p < KS_KEY ? 256 : (int)(p >> 24) + ((int)(p >> 24) >> 7);
            int a = pa * a256 >> 8;
            dp[i] = ks_lerp_px(dp[i], p & 0xFFFFFFu, a);
        }
    }
}

void ks_blit_fade_at(KCanvas *c, int x, int y, const KSprite *s, int a256) { ks_blit_fade(c, x - s->ax, y - s->ay, s, a256); }

/* ---------------------------------------------------------------- painting */

void ks_put(KSprite *s, int x, int y, uint32_t rgb, int a) {
    if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h || a <= 0) return;
    if (a > 255) a = 255;
    uint32_t *d = &s->px[(size_t)y * s->w + x];
    int da = ks_alpha_of(*d);
    if (a >= 255 || da == 0) { *d = ks_enc(rgb, a); return; }
    uint32_t dc = *d & 0xFFFFFFu;
    if (da == 255) {
        *d = ks_lerp_px(dc, rgb, a + (a >> 7));
        return;
    }
    int oa = a + da * (255 - a) / 255;                      /* result alpha */
    int wd = da * (255 - a) / 255;                          /* destination weight, out of oa */
    int r = (int)(((rgb >> 16) & 255) * (uint32_t)a + ((dc >> 16) & 255) * (uint32_t)wd) / oa;
    int g = (int)(((rgb >> 8) & 255) * (uint32_t)a + ((dc >> 8) & 255) * (uint32_t)wd) / oa;
    int b = (int)((rgb & 255) * (uint32_t)a + (dc & 255) * (uint32_t)wd) / oa;
    *d = ks_enc(KRGB(r, g, b), oa);
}

static inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

static void bbox(const KSprite *s, float x0, float y0, float x1, float y1, int *ix0, int *iy0, int *ix1, int *iy1) {
    *ix0 = (int)floorf(x0); *iy0 = (int)floorf(y0);
    *ix1 = (int)ceilf(x1); *iy1 = (int)ceilf(y1);
    if (*ix0 < 0) *ix0 = 0;
    if (*iy0 < 0) *iy0 = 0;
    if (*ix1 > s->w) *ix1 = s->w;
    if (*iy1 > s->h) *iy1 = s->h;
}

void ks_disc(KSprite *s, float cx, float cy, float r, uint32_t rgb, int alpha) {
    int x0, y0, x1, y1;
    bbox(s, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = clamp01(r - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov > 0.f) ks_put(s, x, y, rgb, (int)(cov * (float)alpha + 0.5f));
        }
}

void ks_disc_grad(KSprite *s, float cx, float cy, float r, uint32_t inner, uint32_t outer, float hl_dx, float hl_dy) {
    int x0, y0, x1, y1;
    bbox(s, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &x0, &y0, &x1, &y1);
    float hx = cx + hl_dx * r, hy = cy + hl_dy * r;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = clamp01(r - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov <= 0.f) continue;
            float ex = (float)x + 0.5f - hx, ey = (float)y + 0.5f - hy;
            float t = __builtin_sqrtf(ex * ex + ey * ey) / (r * (1.f + __builtin_sqrtf(hl_dx * hl_dx + hl_dy * hl_dy)));
            ks_put(s, x, y, ks_lerp_px(inner, outer, (int)(clamp01(t) * 256.f)), (int)(cov * 255.f + 0.5f));
        }
}

void ks_ring(KSprite *s, float cx, float cy, float r, float width, uint32_t rgb, int alpha) {
    int x0, y0, x1, y1;
    float ro = r + width * 0.5f;
    bbox(s, cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = __builtin_sqrtf(dx * dx + dy * dy);
            float cov = clamp01(ro - d + 0.5f) - clamp01(r - width * 0.5f - d + 0.5f);
            if (cov > 0.f) ks_put(s, x, y, rgb, (int)(cov * (float)alpha + 0.5f));
        }
}

/* Signed distance estimate (pixels, positive outside) to a rotated ellipse; first-order accurate. */
static inline float ellipse_dist(float dx, float dy, float rx, float ry, float ca, float sa) {
    float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
    float q2 = (u * u) / (rx * rx) + (v * v) / (ry * ry);
    if (q2 < 0.04f) return -(rx < ry ? rx : ry);
    float gx = u / (rx * rx), gy = v / (ry * ry);
    float g = 2.f * __builtin_sqrtf(gx * gx + gy * gy);
    return (q2 - 1.f) / g;
}

void ks_ellipse(KSprite *s, float cx, float cy, float rx, float ry, float angle, uint32_t rgb, int alpha) {
    float ca = cosf(angle), sa = sinf(angle);
    float rm = rx > ry ? rx : ry;
    int x0, y0, x1, y1;
    bbox(s, cx - rm - 1, cy - rm - 1, cx + rm + 1, cy + rm + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float d = ellipse_dist((float)x + 0.5f - cx, (float)y + 0.5f - cy, rx, ry, ca, sa);
            float cov = clamp01(0.5f - d);
            if (cov > 0.f) ks_put(s, x, y, rgb, (int)(cov * (float)alpha + 0.5f));
        }
}

void ks_ellipse_grad(KSprite *s, float cx, float cy, float rx, float ry, float angle, uint32_t inner, uint32_t outer, float hl_dx, float hl_dy) {
    float ca = cosf(angle), sa = sinf(angle);
    float rm = rx > ry ? rx : ry;
    int x0, y0, x1, y1;
    bbox(s, cx - rm - 1, cy - rm - 1, cx + rm + 1, cy + rm + 1, &x0, &y0, &x1, &y1);
    /* highlight position in local (rotated) coordinates */
    float hu = hl_dx * rx, hv = hl_dy * ry;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = ellipse_dist(dx, dy, rx, ry, ca, sa);
            float cov = clamp01(0.5f - d);
            if (cov <= 0.f) continue;
            float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
            float eu = (u - hu) / rx, ev = (v - hv) / ry;
            float t = __builtin_sqrtf(eu * eu + ev * ev) / (1.f + __builtin_sqrtf(hl_dx * hl_dx + hl_dy * hl_dy));
            ks_put(s, x, y, ks_lerp_px(inner, outer, (int)(clamp01(t) * 256.f)), (int)(cov * 255.f + 0.5f));
        }
}

void ks_line(KSprite *s, float x0, float y0, float x1, float y1, float width, uint32_t rgb, int alpha) {
    float hw = width * 0.5f;
    int bx0, by0, bx1, by1;
    float lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
    bbox(s, lx - hw - 1, ly - hw - 1, hx + hw + 1, hy + hw + 1, &bx0, &by0, &bx1, &by1);
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = len2 > 0.f ? (px * vx + py * vy) / len2 : 0.f;
            t = clamp01(t);
            float dx = px - t * vx, dy = py - t * vy;
            float cov = clamp01(hw - __builtin_sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov > 0.f) ks_put(s, x, y, rgb, (int)(cov * (float)alpha + 0.5f));
        }
}

void ks_rect(KSprite *s, float x, float y, float w, float h, uint32_t rgb, int alpha) {
    int x0, y0, x1, y1;
    bbox(s, x, y, x + w, y + h, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float oy = (float)(py + 1) < y + h ? (float)(py + 1) : y + h;
        oy -= (float)py > y ? (float)py : y;
        if (oy <= 0.f) continue;
        for (int px = x0; px < x1; px++) {
            float ox = (float)(px + 1) < x + w ? (float)(px + 1) : x + w;
            ox -= (float)px > x ? (float)px : x;
            if (ox <= 0.f) continue;
            ks_put(s, px, py, rgb, (int)(ox * oy * (float)alpha + 0.5f));
        }
    }
}

static void poly_fill(KSprite *s, const float *xy, int n, uint32_t top, uint32_t bottom, int alpha) {
    if (n < 3) return;
    float ymin = xy[1], ymax = xy[1];
    for (int i = 1; i < n; i++) { if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1]; if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1]; }
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;
    float *cov = malloc((size_t)s->w * sizeof(float));
    float *xs = malloc((size_t)(n + 2) * sizeof(float));
    if (!cov || !xs) { free(cov); free(xs); return; }
    float span = ymax - ymin;
    for (int y = y0; y < y1; y++) {
        memset(cov, 0, (size_t)s->w * sizeof(float));
        bool any = false;
        for (int sub = 0; sub < 4; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) * 0.25f;
            int nx = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float ay = xy[2 * i + 1], by = xy[2 * j + 1];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float t = (sy - ay) / (by - ay);
                    xs[nx++] = xy[2 * i] + t * (xy[2 * j] - xy[2 * i]);
                }
            }
            for (int a = 1; a < nx; a++)
                for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { float t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
            for (int k = 0; k + 1 < nx; k += 2) {
                float xa = xs[k], xb = xs[k + 1];
                if (xb <= 0.f || xa >= (float)s->w) continue;
                if (xa < 0.f) xa = 0.f;
                if (xb > (float)s->w) xb = (float)s->w;
                int ia = (int)floorf(xa), ib = (int)floorf(xb);
                if (ib >= s->w) ib = s->w - 1;
                if (ia == ib) { cov[ia] += (xb - xa) * 0.25f; any = true; continue; }
                cov[ia] += ((float)(ia + 1) - xa) * 0.25f;
                for (int px = ia + 1; px < ib; px++) cov[px] += 0.25f;
                cov[ib] += (xb - (float)ib) * 0.25f;
                any = true;
            }
        }
        if (!any) continue;
        uint32_t col = top;
        if (top != bottom) {
            float t = span > 0.f ? ((float)y + 0.5f - ymin) / span : 0.f;
            col = ks_lerp_px(top, bottom, (int)(clamp01(t) * 256.f));
        }
        for (int x = 0; x < s->w; x++)
            if (cov[x] > 0.01f) ks_put(s, x, y, col, (int)(clamp01(cov[x]) * (float)alpha + 0.5f));
    }
    free(cov);
    free(xs);
}

void ks_poly(KSprite *s, const float *xy, int n, uint32_t rgb, int alpha) { poly_fill(s, xy, n, rgb, rgb, alpha); }
void ks_poly_grad_v(KSprite *s, const float *xy, int n, uint32_t top, uint32_t bottom) { poly_fill(s, xy, n, top, bottom, 255); }

void ks_glow(KSprite *s, float cx, float cy, float r, uint32_t rgb, int peak) {
    int x0, y0, x1, y1;
    bbox(s, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float t = 1.f - __builtin_sqrtf(dx * dx + dy * dy) / r;
            if (t <= 0.f) continue;
            ks_put(s, x, y, rgb, (int)(t * t * (float)peak + 0.5f));
        }
}

void ks_star(KSprite *s, float cx, float cy, float ro, float ri, int points, float angle, uint32_t rgb, int alpha) {
    if (points < 3 || points > 16) return;
    float pts[64];
    for (int i = 0; i < points * 2; i++) {
        float a = angle + (float)i * (float)M_PI / (float)points - (float)M_PI_2;
        float r = (i & 1) ? ri : ro;
        pts[2 * i] = cx + r * cosf(a);
        pts[2 * i + 1] = cy + r * sinf(a);
    }
    ks_poly(s, pts, points * 2, rgb, alpha);
}

void ks_outline(KSprite *s, uint32_t rgb) {
    uint8_t *mark = calloc((size_t)s->w * (size_t)s->h, 1);
    if (!mark) return;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            if (ks_alpha_of(s->px[y * s->w + x]) >= 128) continue;
            bool near = false;
            if (x > 0 && ks_alpha_of(s->px[y * s->w + x - 1]) >= 128) near = true;
            if (x + 1 < s->w && ks_alpha_of(s->px[y * s->w + x + 1]) >= 128) near = true;
            if (y > 0 && ks_alpha_of(s->px[(y - 1) * s->w + x]) >= 128) near = true;
            if (y + 1 < s->h && ks_alpha_of(s->px[(y + 1) * s->w + x]) >= 128) near = true;
            if (near) mark[y * s->w + x] = 1;
        }
    for (int i = 0; i < s->w * s->h; i++)
        if (mark[i]) s->px[i] = rgb & 0xFFFFFFu;
    free(mark);
}

void ks_threshold(KSprite *s, int min_alpha) {
    for (int i = 0; i < s->w * s->h; i++) {
        uint32_t p = s->px[i];
        if (p < KS_KEY) continue;
        s->px[i] = ks_alpha_of(p) >= min_alpha ? (p & 0xFFFFFFu) : KS_KEY;
    }
}

void ks_flatten(KSprite *s, uint32_t backdrop) {
    for (int i = 0; i < s->w * s->h; i++) {
        uint32_t p = s->px[i];
        if (p < KS_KEY || p == KS_KEY) continue;
        int a = (int)(p >> 24);
        s->px[i] = ks_lerp_px(backdrop, p & 0xFFFFFFu, a + (a >> 7));
    }
}

void ks_darken_edge(KSprite *s, int percent) {
    uint32_t *copy = malloc((size_t)s->w * (size_t)s->h * 4);
    if (!copy) return;
    memcpy(copy, s->px, (size_t)s->w * (size_t)s->h * 4);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint32_t p = copy[y * s->w + x];
            if (p >= KS_KEY) continue;
            bool edge = x == 0 || y == 0 || x == s->w - 1 || y == s->h - 1 ||
                        copy[y * s->w + x - 1] == KS_KEY || copy[y * s->w + x + 1] == KS_KEY ||
                        copy[(y - 1) * s->w + x] == KS_KEY || copy[(y + 1) * s->w + x] == KS_KEY;
            if (edge) s->px[y * s->w + x] = ks_mul_px(p, percent * 256 / 100);
        }
    free(copy);
}

/* ---------------------------------------------------------------- text sprites */

int ks_text_w(const char *str, int scale) { return (int)kc_utf8_len(str) * FONT_W * scale; }

KSprite *ks_text(const char *str, int scale, uint32_t fg, uint32_t outline) {
    if (scale < 1) scale = 1;
    bool has_outline = outline != 0xFFFFFFFFu;
    int ot = has_outline ? (scale >= 3 ? 2 : 1) : 0;
    size_t n = kc_utf8_len(str);
    int w = (int)n * FONT_W * scale + 2 * ot, h = FONT_H * scale + 2 * ot;
    KSprite *s = ks_new(w < 1 ? 1 : w, h);
    if (!s) return NULL;
    for (int pass = has_outline ? 0 : 1; pass < 2; pass++) {
        const char *p = str;
        int pen = ot;
        uint32_t cp;
        int b;
        while ((b = kc_utf8_next(p, &cp)) > 0) {
            p += b;
            if (cp == '\t') cp = ' ';
            const unsigned char *g = font8x16_glyph(cp);
            for (int row = 0; row < FONT_H; row++) {
                unsigned bits = g[row];
                if (!bits) continue;
                for (int col = 0; col < FONT_W; col++) {
                    if (!(bits & (0x80u >> col))) continue;
                    int x = pen + col * scale, y = ot + row * scale;
                    uint32_t color = pass == 0 ? (outline & 0xFFFFFFu) : (fg & 0xFFFFFFu);
                    int ex = pass == 0 ? ot : 0;
                    for (int yy = y - ex; yy < y + scale + ex; yy++)
                        for (int xx = x - ex; xx < x + scale + ex; xx++)
                            if ((unsigned)xx < (unsigned)s->w && (unsigned)yy < (unsigned)s->h) s->px[yy * s->w + xx] = color;
                }
            }
            pen += FONT_W * scale;
        }
    }
    s->ax = 0;
    s->ay = 0;
    return s;
}

void ks_text_draw(KCanvas *c, int x, int y, const char *str, int scale, uint32_t fg, uint32_t outline) {
    KSprite *s = ks_text(str, scale, fg, outline);
    if (!s) return;
    int ot = outline != 0xFFFFFFFFu ? (scale >= 3 ? 2 : 1) : 0;
    ks_blit_key(c, x - ot, y - ot, s);
    ks_free(s);
}
