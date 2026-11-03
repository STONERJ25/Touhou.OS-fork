/*
 * The painter (see paint.h): a display list in, pixels out. Written from the display list's documentation in web.h and
 * the CSS 2.1 / CSS Backgrounds 3 descriptions of borders (the join of two sides along the diagonal, the dotted and dashed
 * rhythm, double, groove, ridge, inset and outset). Single-precision arithmetic only; the square root is the compiler's builtin.
 */
#include "paint.h"

#include <img.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_CLIPS 64
#define MAX_SCROLLERS 16
#define IMG_CACHE 12                         /* scaled copies kept */
#define IMG_CACHE_BYTES (24u << 20)

typedef struct ImgEntry {
    const uint32_t *src;                     /* the source picture's pixels: the picture's identity */
    int sw, sh, w, h;                        /* source and scaled size */
    Image scaled;
    uint32_t tick;
} ImgEntry;

struct BrPainter {
    BrFonts *fonts;
    ImgEntry cache[IMG_CACHE];
    size_t cache_bytes;
    uint32_t tick;
};

typedef struct PState {
    BrPainter *p;
    BrSurface *dst;
    const BrPaintOpts *o;
    int ox, oy, scale;
    BrRect cur;
    BrRect clips[MAX_CLIPS];
    int nclip, over;
    unsigned alphas[MAX_CLIPS];
    int nalpha, over_a;
    unsigned galpha;
    struct { WebNode *node; BrRect r; } scr[MAX_SCROLLERS];
    int nscr, over_s;
} PState;

BrPainter *brp_new(BrFonts *fonts) {
    BrPainter *p = calloc(1, sizeof *p);
    if (p) p->fonts = fonts;
    return p;
}

void brp_reset_images(BrPainter *p) {
    for (int i = 0; i < IMG_CACHE; i++) {
        if (p->cache[i].scaled.px) img_free(&p->cache[i].scaled);
        memset(&p->cache[i], 0, sizeof p->cache[i]);
    }
    p->cache_bytes = 0;
}

void brp_free(BrPainter *p) {
    if (!p) return;
    brp_reset_images(p);
    free(p);
}

/* ---------------------------------------------------------------- small helpers */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float fsqrt(float v) { return __builtin_sqrtf(v); }

static inline unsigned scale_alpha(unsigned a, unsigned g) { return g >= 255 ? a : br_div255(a * g); }

/* one pixel with coverage `cov` (0..255) of colour argb, clipped */
static inline void put(PState *st, int x, int y, uint32_t argb, unsigned cov) {
    if (x < st->cur.x0 || x >= st->cur.x1 || y < st->cur.y0 || y >= st->cur.y1) return;
    unsigned a = scale_alpha(br_div255((argb >> 24) * cov), st->galpha);
    if (!a) return;
    uint32_t *d = &st->dst->px[(size_t)y * (size_t)st->dst->stride + (size_t)x];
    *d = br_mix(*d, argb, a);
}

static void fill(PState *st, BrRect r, uint32_t argb) {
    r = br_rect_and(r, st->cur);
    if (br_rect_empty(r)) return;
    unsigned a = scale_alpha(argb >> 24, st->galpha);
    if (!a) return;
    uint32_t rgb = argb & 0xFFFFFFu;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *d = st->dst->px + (size_t)y * (size_t)st->dst->stride;
        if (a >= 255) for (int x = r.x0; x < r.x1; x++) d[x] = rgb;
        else for (int x = r.x0; x < r.x1; x++) d[x] = br_mix(d[x], rgb, a);
    }
}

static uint32_t mix_argb(uint32_t a, uint32_t b, unsigned t256) {       /* a + (b - a) * t / 256, the alpha of a kept */
    uint32_t out = a & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int va = (int)((a >> sh) & 255), vb = (int)((b >> sh) & 255);
        out |= (uint32_t)(va + (vb - va) * (int)t256 / 256) << sh;
    }
    return out;
}

static uint32_t with_alpha(uint32_t argb, unsigned a) { return (argb & 0xFFFFFFu) | (uint32_t)a << 24; }

static BrRect inset(BrRect r, int d) { BrRect o = { r.x0 + d, r.y0 + d, r.x1 - d, r.y1 - d }; return o; }

static BrRect item_rect(const PState *st, const WebDisplayItem *it) { return br_rect(st->ox + it->rect.x, st->oy + it->rect.y, it->rect.w, it->rect.h); }

/* ---------------------------------------------------------------- rounded rectangles */

/* coverage (0..255) of the pixel (x, y) of a w x h rectangle with corner radii r (top-left, top-right, bottom-right, bottom-left) */
static unsigned rr_cov(int x, int y, int w, int h, const int r[4]) {
    int rad, cx, cy;
    if (r[0] > 0 && x < r[0] && y < r[0]) { rad = r[0]; cx = r[0]; cy = r[0]; }
    else if (r[1] > 0 && x >= w - r[1] && y < r[1]) { rad = r[1]; cx = w - r[1]; cy = r[1]; }
    else if (r[2] > 0 && x >= w - r[2] && y >= h - r[2]) { rad = r[2]; cx = w - r[2]; cy = h - r[2]; }
    else if (r[3] > 0 && x < r[3] && y >= h - r[3]) { rad = r[3]; cx = r[3]; cy = h - r[3]; }
    else return 255;
    float dx = (float)x + 0.5f - (float)cx, dy = (float)y + 0.5f - (float)cy;
    float c = (float)rad - fsqrt(dx * dx + dy * dy) + 0.5f;
    return c <= 0 ? 0 : c >= 1 ? 255 : (unsigned)(c * 255.0f);
}

/* CSS Backgrounds 3 "corner overlap": radii shrink together when two neighbours would not fit on a side */
static void fit_radii(int w, int h, const uint16_t in[4], int out[4]) {
    float f = 1.0f;
    for (int i = 0; i < 4; i++) out[i] = in[i];
    int sums[4][2] = { { out[0] + out[1], w }, { out[3] + out[2], w }, { out[0] + out[3], h }, { out[1] + out[2], h } };
    for (int i = 0; i < 4; i++)
        if (sums[i][0] > sums[i][1] && sums[i][0] > 0) {
            float k = (float)sums[i][1] / (float)sums[i][0];
            if (k < f) f = k;
        }
    if (f < 1.0f) for (int i = 0; i < 4; i++) out[i] = (int)((float)out[i] * f);
}

static bool has_radii(const uint16_t r[4]) { return r[0] || r[1] || r[2] || r[3]; }

static void fill_rrect(PState *st, BrRect r, const int rad[4], uint32_t argb) {
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    BrRect v = br_rect_and(r, st->cur);
    if (br_rect_empty(v)) return;
    for (int y = v.y0; y < v.y1; y++) {
        int ry = y - r.y0;
        int lr = ry < rad[0] ? rad[0] : ry >= h - rad[3] ? rad[3] : 0;
        int rr = ry < rad[1] ? rad[1] : ry >= h - rad[2] ? rad[2] : 0;
        int xa = r.x0 + lr, xb = r.x1 - rr;                           /* the solid middle of this row */
        if (xb > xa) fill(st, (BrRect){ xa, y, xb, y + 1 }, argb);
        for (int x = v.x0; x < v.x1 && x < xa; x++) put(st, x, y, argb, rr_cov(x - r.x0, ry, w, h, rad));
        for (int x = imax(v.x0, xb); x < v.x1; x++) put(st, x, y, argb, rr_cov(x - r.x0, ry, w, h, rad));
    }
}

/* ---------------------------------------------------------------- lines and ellipses */

/* a line segment of width w with soft edges (check marks, arrows) */
static void stroke(PState *st, float x0, float y0, float x1, float y1, float w, uint32_t argb) {
    float lx = x1 - x0, ly = y1 - y0, len2 = lx * lx + ly * ly;
    int bx0 = (int)(x0 < x1 ? x0 : x1) - (int)w - 1, bx1 = (int)(x0 > x1 ? x0 : x1) + (int)w + 2;
    int by0 = (int)(y0 < y1 ? y0 : y1) - (int)w - 1, by1 = (int)(y0 > y1 ? y0 : y1) + (int)w + 2;
    for (int y = imax(by0, st->cur.y0); y < imin(by1, st->cur.y1); y++)
        for (int x = imax(bx0, st->cur.x0); x < imin(bx1, st->cur.x1); x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = len2 > 0 ? (px * lx + py * ly) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - t * lx, dy = py - t * ly;
            float c = w * 0.5f - fsqrt(dx * dx + dy * dy) + 0.5f;
            if (c > 0) put(st, x, y, argb, c >= 1 ? 255 : (unsigned)(c * 255.0f));
        }
}

/* coverage of a pixel by an ellipse centred (cx, cy) with radii (rx, ry), 4 x 4 samples */
static unsigned ellipse_cov(int x, int y, float cx, float cy, float rx, float ry) {
    if (rx <= 0 || ry <= 0) return 0;
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            float dx = ((float)x + ((float)i + 0.5f) / 4.0f - cx) / rx, dy = ((float)y + ((float)j + 0.5f) / 4.0f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) n++;
        }
    return (unsigned)(n * 255 / 16);
}

static void draw_ellipse(PState *st, BrRect r, uint32_t argb, int ring) {
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    float cx = (float)r.x0 + (float)w / 2.0f, cy = (float)r.y0 + (float)h / 2.0f, rx = (float)w / 2.0f, ry = (float)h / 2.0f;
    BrRect v = br_rect_and(r, st->cur);
    for (int y = v.y0; y < v.y1; y++)
        for (int x = v.x0; x < v.x1; x++) {
            unsigned c = ellipse_cov(x, y, cx, cy, rx, ry);
            if (c && ring > 0) c = c * (255 - ellipse_cov(x, y, cx, cy, rx - (float)ring, ry - (float)ring)) / 255;
            if (c) put(st, x, y, argb, c);
        }
}

/* ---------------------------------------------------------------- borders */

static uint32_t light_of(uint32_t c) { return mix_argb(c, 0xFFFFFFFFu, 150); }
static uint32_t dark_of(uint32_t c) { return mix_argb(c, 0xFF000000u, 115); }

/* the colour of a side for inset, outset, groove and ridge: the top and left sides are light when raised */
static uint32_t shaded(int style, int side, uint32_t c, bool inner_half) {
    bool raised = false;
    switch (style) {
    case WEB_BORDER_OUTSET: raised = true; break;
    case WEB_BORDER_INSET: raised = false; break;
    case WEB_BORDER_GROOVE: raised = inner_half; break;           /* a groove looks cut in on the outside, raised inside */
    case WEB_BORDER_RIDGE: raised = !inner_half; break;
    default: return c;
    }
    bool top_left = side == 0 || side == 3;
    return top_left == raised ? light_of(c) : dark_of(c);
}

/* The side's band as a trapezoid: rows (or columns) d = lo .. hi-1 counted from the outer edge, each shortened by the diagonal
 * to the inner corner. lo and hi select a part of the thickness (the two halves of groove and ridge, the two lines of double). */
static void trap(PState *st, BrRect b, int side, const int bw[4], int lo, int hi, uint32_t color) {
    int w = b.x1 - b.x0, h = b.y1 - b.y0;
    int t = bw[0], r = bw[1], bo = bw[2], l = bw[3];
    if (side == 0 && t > 0) {
        for (int d = lo; d < hi; d++) {
            int xa = l ? (d * l + t / 2) / t : 0, xb = w - (r ? (d * r + t / 2) / t : 0);
            fill(st, (BrRect){ b.x0 + xa, b.y0 + d, b.x0 + xb, b.y0 + d + 1 }, color);
        }
    } else if (side == 2 && bo > 0) {
        for (int d = lo; d < hi; d++) {
            int xa = l ? (d * l + bo / 2) / bo : 0, xb = w - (r ? (d * r + bo / 2) / bo : 0);
            fill(st, (BrRect){ b.x0 + xa, b.y1 - 1 - d, b.x0 + xb, b.y1 - d }, color);
        }
    } else if (side == 3 && l > 0) {
        for (int d = lo; d < hi; d++) {
            int ya = t ? (d * t + l / 2) / l : 0, yb = h - (bo ? (d * bo + l / 2) / l : 0);
            fill(st, (BrRect){ b.x0 + d, b.y0 + ya, b.x0 + d + 1, b.y0 + yb }, color);
        }
    } else if (side == 1 && r > 0) {
        for (int d = lo; d < hi; d++) {
            int ya = t ? (d * t + r / 2) / r : 0, yb = h - (bo ? (d * bo + r / 2) / r : 0);
            fill(st, (BrRect){ b.x1 - 1 - d, b.y0 + ya, b.x1 - d, b.y0 + yb }, color);
        }
    }
}

/* dotted and dashed: a row of marks along the band; the horizontal sides run the whole width, the vertical ones between them */
static void dashes(PState *st, BrRect b, int side, const int bw[4], bool dotted, uint32_t color) {
    int t = bw[side];
    int mark = dotted ? t : t * 3, gap = dotted ? t : t * 2;
    if (mark < 1) mark = 1;
    if (gap < 1) gap = 1;
    bool horiz = side == 0 || side == 2;
    int start = horiz ? b.x0 : b.y0 + bw[0];
    int end = horiz ? b.x1 : b.y1 - bw[2];
    int64_t period = (int64_t)mark + gap, lo = horiz ? st->cur.x0 : st->cur.y0, hi = horiz ? st->cur.x1 : st->cur.y1;
    if (lo > start) start += (int)(((lo - start) / period) * period);       /* only the marks that can be seen: a border a billion pixels long costs no more */
    if ((int64_t)end > hi + period) end = (int)(hi + period);
    for (int p = start; p < end; p += mark + gap) {
        int e = imin(p + mark, end);
        BrRect m;
        if (side == 0) m = (BrRect){ p, b.y0, e, b.y0 + t };
        else if (side == 2) m = (BrRect){ p, b.y1 - t, e, b.y1 };
        else if (side == 3) m = (BrRect){ b.x0, p, b.x0 + t, e };
        else m = (BrRect){ b.x1 - t, p, b.x1, e };
        if (dotted && t >= 3) draw_ellipse(st, m, color, 0);
        else fill(st, m, color);
    }
}

static void draw_border_rect(PState *st, BrRect b, const WebDisplayItem *it) {
    int bw[4];
    for (int i = 0; i < 4; i++) bw[i] = it->styles[i] ? it->widths[i] : 0;
    int w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (bw[0] + bw[2] > h) bw[2] = imax(0, h - bw[0]);               /* a band cannot be thicker than the box */
    if (bw[3] + bw[1] > w) bw[1] = imax(0, w - bw[3]);
    bool plain = true, any = false;
    uint32_t c = 0;
    for (int i = 0; i < 4; i++) {
        if (!bw[i]) continue;
        if (!any) c = it->colors[i];
        any = true;
        if (it->styles[i] != WEB_BORDER_SOLID || it->colors[i] != c) plain = false;
    }
    if (!any) return;
    if (plain) {                                                     /* the common case: plain bands of one colour */
        fill(st, (BrRect){ b.x0, b.y0, b.x1, b.y0 + bw[0] }, c);
        fill(st, (BrRect){ b.x0, b.y1 - bw[2], b.x1, b.y1 }, c);
        fill(st, (BrRect){ b.x0, b.y0 + bw[0], b.x0 + bw[3], b.y1 - bw[2] }, c);
        fill(st, (BrRect){ b.x1 - bw[1], b.y0 + bw[0], b.x1, b.y1 - bw[2] }, c);
        return;
    }
    for (int s = 0; s < 4; s++) {
        int t = bw[s];
        if (t <= 0) continue;
        uint32_t c = it->colors[s];
        switch (it->styles[s]) {
        case WEB_BORDER_DOTTED: dashes(st, b, s, bw, true, c); break;
        case WEB_BORDER_DASHED: dashes(st, b, s, bw, false, c); break;
        case WEB_BORDER_DOUBLE:
            if (t < 3) trap(st, b, s, bw, 0, t, c);
            else { trap(st, b, s, bw, 0, t / 3, c); trap(st, b, s, bw, t - t / 3, t, c); }
            break;
        case WEB_BORDER_GROOVE: case WEB_BORDER_RIDGE: {
            int half = t / 2;
            if (half < 1) { trap(st, b, s, bw, 0, t, shaded(it->styles[s], s, c, false)); break; }
            trap(st, b, s, bw, 0, half, shaded(it->styles[s], s, c, false));
            trap(st, b, s, bw, half, t, shaded(it->styles[s], s, c, true));
            break;
        }
        case WEB_BORDER_INSET: case WEB_BORDER_OUTSET: trap(st, b, s, bw, 0, t, shaded(it->styles[s], s, c, false)); break;
        default: trap(st, b, s, bw, 0, t, c); break;
        }
    }
}

/* A rounded border: the ring between the outer and the inner rounded rectangle. The straight stretches are plain bands, the
 * corner squares are evaluated per pixel; a corner pixel takes the colour of the side the diagonal gives it to. */
static void draw_border_round(PState *st, BrRect b, const WebDisplayItem *it) {
    int w = b.x1 - b.x0, h = b.y1 - b.y0;
    int rad[4], irad[4], bw[4];
    fit_radii(w, h, it->radii, rad);
    for (int i = 0; i < 4; i++) bw[i] = it->styles[i] ? it->widths[i] : 0;
    irad[0] = imax(0, rad[0] - (bw[3] + bw[0]) / 2);
    irad[1] = imax(0, rad[1] - (bw[1] + bw[0]) / 2);
    irad[2] = imax(0, rad[2] - (bw[1] + bw[2]) / 2);
    irad[3] = imax(0, rad[3] - (bw[3] + bw[2]) / 2);
    int iw = w - bw[1] - bw[3], ih = h - bw[0] - bw[2];
    fill(st, (BrRect){ b.x0 + rad[0], b.y0, b.x1 - rad[1], b.y0 + bw[0] }, it->colors[0]);
    fill(st, (BrRect){ b.x0 + rad[3], b.y1 - bw[2], b.x1 - rad[2], b.y1 }, it->colors[2]);
    fill(st, (BrRect){ b.x0, b.y0 + rad[0], b.x0 + bw[3], b.y1 - rad[3] }, it->colors[3]);
    fill(st, (BrRect){ b.x1 - bw[1], b.y0 + rad[1], b.x1, b.y1 - rad[2] }, it->colors[1]);
    for (int c = 0; c < 4; c++) {                                    /* top-left, top-right, bottom-right, bottom-left */
        int r = rad[c];
        if (r <= 0) continue;
        BrRect sq = c == 0 ? (BrRect){ b.x0, b.y0, b.x0 + r, b.y0 + r } : c == 1 ? (BrRect){ b.x1 - r, b.y0, b.x1, b.y0 + r }
                  : c == 2 ? (BrRect){ b.x1 - r, b.y1 - r, b.x1, b.y1 } : (BrRect){ b.x0, b.y1 - r, b.x0 + r, b.y1 };
        int hs = (c == 0 || c == 1) ? 0 : 2, vs = (c == 0 || c == 3) ? 3 : 1;
        BrRect v = br_rect_and(sq, st->cur);
        for (int y = v.y0; y < v.y1; y++)
            for (int x = v.x0; x < v.x1; x++) {
                int rx = x - b.x0, ry = y - b.y0;
                unsigned outer = rr_cov(rx, ry, w, h, rad);
                if (!outer) continue;
                unsigned inner = 0;
                if (rx >= bw[3] && rx < w - bw[1] && ry >= bw[0] && ry < h - bw[2] && iw > 0 && ih > 0) inner = rr_cov(rx - bw[3], ry - bw[0], iw, ih, irad);
                unsigned cover = outer * (255 - inner) / 255;
                if (!cover) continue;
                int dh = (hs == 0) ? ry : h - 1 - ry, dv = (vs == 3) ? rx : w - 1 - rx;
                int side = bw[hs] == 0 ? vs : bw[vs] == 0 ? hs : (dh * bw[vs] <= dv * bw[hs] ? hs : vs);
                put(st, x, y, it->colors[side], cover);
            }
    }
}

/* ---------------------------------------------------------------- images */

static void draw_missing_image(PState *st, BrRect r) {
    fill(st, r, 0xFFEEEEEEu);
    int s = st->scale;
    fill(st, (BrRect){ r.x0, r.y0, r.x1, r.y0 + s }, 0xFF9AA0A6u);
    fill(st, (BrRect){ r.x0, r.y1 - s, r.x1, r.y1 }, 0xFF9AA0A6u);
    fill(st, (BrRect){ r.x0, r.y0, r.x0 + s, r.y1 }, 0xFF9AA0A6u);
    fill(st, (BrRect){ r.x1 - s, r.y0, r.x1, r.y1 }, 0xFF9AA0A6u);
}

/* pixels of an image copied 1:1 (sx, sy: the source offset of the destination's top-left corner) */
static void blit_image(PState *st, const Image *im, int sx, int sy, BrRect dr) {
    BrRect v = br_rect_and(dr, st->cur);
    for (int y = v.y0; y < v.y1; y++) {
        const uint32_t *s = im->px + (size_t)(y - dr.y0 + sy) * (size_t)im->w + (size_t)(v.x0 - dr.x0 + sx);
        uint32_t *d = st->dst->px + (size_t)y * (size_t)st->dst->stride + (size_t)v.x0;
        int n = v.x1 - v.x0;
        if (!im->has_alpha && st->galpha >= 255) { for (int i = 0; i < n; i++) d[i] = s[i] & 0xFFFFFFu; continue; }
        for (int i = 0; i < n; i++) {
            unsigned a = scale_alpha(im->has_alpha ? s[i] >> 24 : 255u, st->galpha);
            if (a) d[i] = br_mix(d[i], s[i], a);
        }
    }
}

/* the picture shrunk to w x h with an area average, from the cache or made now; NULL when it will not fit the cache */
static const Image *scaled_copy(BrPainter *p, const Image *im, int w, int h) {
    p->tick++;
    for (int i = 0; i < IMG_CACHE; i++) {
        ImgEntry *e = &p->cache[i];
        if (e->scaled.px && e->src == im->px && e->sw == im->w && e->sh == im->h && e->w == w && e->h == h) { e->tick = p->tick; return &e->scaled; }
    }
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (bytes > IMG_CACHE_BYTES / 2) return NULL;
    while (p->cache_bytes + bytes > IMG_CACHE_BYTES) {
        int lru = -1;
        for (int i = 0; i < IMG_CACHE; i++)
            if (p->cache[i].scaled.px && (lru < 0 || p->cache[i].tick < p->cache[lru].tick)) lru = i;
        if (lru < 0) break;
        p->cache_bytes -= (size_t)p->cache[lru].w * (size_t)p->cache[lru].h * 4;
        img_free(&p->cache[lru].scaled);
        memset(&p->cache[lru], 0, sizeof p->cache[lru]);
    }
    int victim = 0;
    for (int i = 0; i < IMG_CACHE; i++) {                           /* an empty slot, else the least recently used one */
        if (!p->cache[i].scaled.px) { victim = i; break; }
        if (p->cache[i].tick < p->cache[victim].tick) victim = i;
    }
    ImgEntry *e = &p->cache[victim];
    if (e->scaled.px) { p->cache_bytes -= (size_t)e->w * (size_t)e->h * 4; img_free(&e->scaled); }
    memset(e, 0, sizeof *e);
    if (img_scale(im, w, h, IMG_AREA, &e->scaled) != IMG_OK) { memset(e, 0, sizeof *e); return NULL; }
    e->src = im->px;
    e->sw = im->w;
    e->sh = im->h;
    e->w = w;
    e->h = h;
    e->tick = p->tick;
    p->cache_bytes += bytes;
    return &e->scaled;
}

/* bilinear enlargement of the part (sx0, sy0, sw, sh) of a picture (nearest neighbour for one of a pixel or two, which pages
 * stretch as spacers and colour swatches) */
static void draw_enlarged(PState *st, const Image *im, int sx0, int sy0, int sw, int sh, BrRect dr) {
    BrRect v = br_rect_and(dr, st->cur);
    int dw = dr.x1 - dr.x0, dh = dr.y1 - dr.y0;
    bool nearest = sw * sh <= 4;
    for (int y = v.y0; y < v.y1; y++) {
        float fy = ((float)(y - dr.y0) + 0.5f) * (float)sh / (float)dh - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = (int)fy, y1 = imin(y0 + 1, sh - 1);
        unsigned wy = (unsigned)((fy - (float)y0) * 256.0f);
        if (nearest) { y0 = y1 = iclamp((y - dr.y0) * sh / dh, 0, sh - 1); wy = 0; }
        const uint32_t *r0 = im->px + (size_t)(sy0 + y0) * (size_t)im->w + (size_t)sx0, *r1 = im->px + (size_t)(sy0 + y1) * (size_t)im->w + (size_t)sx0;
        for (int x = v.x0; x < v.x1; x++) {
            float fx = ((float)(x - dr.x0) + 0.5f) * (float)sw / (float)dw - 0.5f;
            if (fx < 0) fx = 0;
            int x0 = (int)fx, x1 = imin(x0 + 1, sw - 1);
            unsigned wx = (unsigned)((fx - (float)x0) * 256.0f);
            if (nearest) { x0 = x1 = iclamp((x - dr.x0) * sw / dw, 0, sw - 1); wx = 0; }
            uint32_t c00 = r0[x0], c10 = r0[x1], c01 = r1[x0], c11 = r1[x1], out = 0;
            for (int sh_ = 0; sh_ < 32; sh_ += 8) {
                unsigned a = (c00 >> sh_) & 255, b = (c10 >> sh_) & 255, c = (c01 >> sh_) & 255, d = (c11 >> sh_) & 255;
                unsigned top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                out |= (((top >> 8) * (256 - wy) + (bot >> 8) * wy) >> 8) << sh_;
            }
            unsigned a = scale_alpha(im->has_alpha ? out >> 24 : 255u, st->galpha);
            uint32_t *d = &st->dst->px[(size_t)y * (size_t)st->dst->stride + (size_t)x];
            if (a) *d = br_mix(*d, out, a);
        }
    }
}

static void draw_image(PState *st, const WebDisplayItem *it) {
    BrRect dr = item_rect(st, it);
    if (br_rect_empty(br_rect_and(dr, st->cur))) return;
    const Image *im = (const Image *)it->image;
    if (!im || !im->px || im->w <= 0 || im->h <= 0) { draw_missing_image(st, dr); return; }
    int sx = 0, sy = 0, sw = im->w, sh = im->h, dw = dr.x1 - dr.x0, dh = dr.y1 - dr.y0;
    bool part = it->src.w > 0 && it->src.h > 0;
    if (part) {
        sx = iclamp(it->src.x, 0, im->w - 1);
        sy = iclamp(it->src.y, 0, im->h - 1);
        sw = iclamp(it->src.w, 1, im->w - sx);
        sh = iclamp(it->src.h, 1, im->h - sy);
    }
    if (dw == sw && dh == sh) { blit_image(st, im, sx, sy, dr); return; }
    if (!part && (dw < sw || dh < sh)) {
        const Image *small = scaled_copy(st->p, im, dw, dh);
        if (small) { blit_image(st, small, 0, 0, dr); return; }
    }
    if (dw >= sw && dh >= sh) { draw_enlarged(st, im, sx, sy, sw, sh, dr); return; }
    BrRect v = br_rect_and(dr, st->cur);                             /* a shrunk piece of a picture, or a copy the cache cannot hold */
    for (int y = v.y0; y < v.y1; y++) {
        const uint32_t *row = im->px + (size_t)(sy + (y - dr.y0) * sh / dh) * (size_t)im->w + (size_t)sx;
        for (int x = v.x0; x < v.x1; x++) put(st, x, y, row[(x - dr.x0) * sw / dw], 255);
    }
}

/* ---------------------------------------------------------------- shadows */

static void draw_shadow(PState *st, BrRect r, const WebDisplayItem *it) {
    int w = r.x1 - r.x0, h = r.y1 - r.y0, rad[4];
    fit_radii(w, h, it->radii, rad);
    int blur = it->width > 0 ? it->width : 0;
    if (blur == 0) { fill_rrect(st, r, rad, it->color); return; }
    int layers = imin(blur, 8);
    if (layers < 2) layers = 2;
    unsigned a = it->color >> 24;
    unsigned per = (a * 2 + (unsigned)layers - 1) / (unsigned)layers;    /* the stacked layers add up to about `a` where all of them overlap */
    if (per < 1) per = 1;
    for (int k = layers; k >= 1; k--) {
        int grow = blur * k / layers - blur / 2;
        BrRect g = { r.x0 - grow, r.y0 - grow, r.x1 + grow, r.y1 + grow };
        if (g.x1 <= g.x0 || g.y1 <= g.y0) continue;
        uint16_t tmp[4];
        for (int i = 0; i < 4; i++) tmp[i] = (uint16_t)(rad[i] ? imax(0, rad[i] + grow) : 0);
        int gfit[4];
        fit_radii(g.x1 - g.x0, g.y1 - g.y0, tmp, gfit);
        fill_rrect(st, g, gfit, with_alpha(it->color, (unsigned)imin((int)per, 255)));
    }
}

/* ---------------------------------------------------------------- form controls */

#define CTL_FRAME     0xFF767676u
#define CTL_FACE      0xFFEFEFEFu
#define CTL_DISABLED  0xFF8A8A8Au
#define CTL_PLACEHOLD 0xFF757575u
#define CTL_ACCENT    0xFFC8102Eu            /* shrine red */
#define CTL_SELECT_BG 0xFF3390FFu

static int css_len(WebNode *el, const char *prop, int scale) {
    char *v = web_element_computed(el, prop);
    if (!v) return 0;
    float f = (float)atof(v);
    web_free(web_node_doc(el), v);
    if (f < 0) f = 0;
    return (int)(f * (float)scale + 0.5f);
}

static void font_metrics_dev(BrFonts *f, WebFontHandle h, int scale, int *asc, int *desc) {
    WebFontMetrics m;
    const WebFontProvider *pr = brf_provider(f);
    memset(&m, 0, sizeof m);
    if (pr->metrics) pr->metrics(pr->user, h, &m);
    *asc = (int)(((int64_t)m.ascent * scale + 32) / 64);
    *desc = (int)(((int64_t)m.descent * scale + 32) / 64);
}

bool brp_control_geom(BrPainter *p, const WebDisplayItem *it, int ox, int oy, int scale, BrControlGeom *g) {
    if (!it || it->kind != WEB_DI_CONTROL || !it->node) return false;
    if (scale < 1) scale = 1;
    WebNode *el = it->node;
    BrRect box = br_rect(ox + it->rect.x, oy + it->rect.y, it->rect.w, it->rect.h);
    BrRect c = { box.x0 + css_len(el, "border-left-width", scale) + css_len(el, "padding-left", scale),
                 box.y0 + css_len(el, "border-top-width", scale) + css_len(el, "padding-top", scale),
                 box.x1 - css_len(el, "border-right-width", scale) - css_len(el, "padding-right", scale),
                 box.y1 - css_len(el, "border-bottom-width", scale) - css_len(el, "padding-bottom", scale) };
    if (c.x1 < c.x0) c.x1 = c.x0;
    if (c.y1 < c.y0) c.y1 = c.y0;
    int asc, desc;
    font_metrics_dev(p->fonts, it->font, scale, &asc, &desc);
    g->content = c;
    g->font = it->font;
    g->line_h = asc + desc;
    g->text_x = c.x0;
    g->baseline = it->control == WEB_CONTROL_TEXTAREA ? c.y0 + asc : c.y0 + ((c.y1 - c.y0) + asc - desc) / 2;
    return true;
}

static int text_w(BrPainter *p, int scale, WebFontHandle h, const char *s, size_t n) {
    return (int)(((int64_t)brf_measure(p->fonts, h, s, n) * scale + 32) / 64);
}

static size_t u8_step(const char *s, size_t i, size_t end) {        /* the length of the character at s[i] */
    size_t k = 1;
    while (i + k < end && ((unsigned char)s[i + k] & 0xC0) == 0x80) k++;
    return k;
}

size_t brp_wrap(BrPainter *p, WebFontHandle font, int scale, int width, const char *text, size_t len, BrLine *out, size_t max) {
    size_t n = 0, i = 0;
    if (width < 1) width = 1;
#define EMIT(a, b) do { if (out && n < max) { out[n].start = (uint32_t)(a); out[n].end = (uint32_t)(b); } n++; } while (0)
    for (;;) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t start = i, pos = i;
        int used = 0;
        while (pos < e) {
            size_t we = pos;
            while (we < e && text[we] != ' ') we++;
            size_t se = we;
            while (se < e && text[se] == ' ') se++;
            int ww = text_w(p, scale, font, text + pos, we - pos), sw = text_w(p, scale, font, text + we, se - we);
            if (used > 0 && used + ww > width) { EMIT(start, pos); start = pos; used = 0; }
            if (ww > width) {                                        /* a word longer than the line: break it between characters */
                size_t q = pos;
                int cw = 0;
                while (q < we) {
                    size_t cl = u8_step(text, q, we);
                    int w1 = text_w(p, scale, font, text + q, cl);
                    if (cw > 0 && cw + w1 > width) { EMIT(start, q); start = q; cw = 0; }
                    cw += w1;
                    q += cl;
                }
                used = cw + sw;
            } else used += ww + sw;
            pos = se;
        }
        EMIT(start, e);
        if (e >= len) break;
        i = e + 1;
    }
#undef EMIT
    return n;
}

static void draw_run(PState *st, WebFontHandle h, const char *s, size_t n, int x, int baseline, uint32_t argb) {
    brf_draw_text(st->p->fonts, st->dst, st->cur, h, st->scale, s, n, (int32_t)x * 64, (int32_t)baseline * 64, argb, st->galpha);
}

/* A password field shows bullets, three bytes each, so offsets into its value are converted by counting characters. */
static size_t shown_offset(const char *value, size_t at, bool password) {
    if (!password) return at;
    size_t chars = 0;
    for (size_t i = 0; i < at && value[i]; i++) if (((unsigned char)value[i] & 0xC0) != 0x80) chars++;
    return chars * 3;
}

static void draw_caret(PState *st, int x, int top, int bottom, uint32_t color) {
    int w = st->scale < 1 ? 1 : st->scale;
    fill(st, (BrRect){ x, top, x + w, bottom }, color);
}

static void draw_field(PState *st, const WebDisplayItem *it, bool multiline) {
    BrControlGeom g;
    if (!brp_control_geom(st->p, it, st->ox, st->oy, st->scale, &g)) return;
    BrRect saved = st->cur;
    st->cur = br_rect_and(saved, g.content);
    bool focused = st->o->focus && st->o->focus == it->node;
    bool placeholder = (it->state & WEB_CTL_PLACEHOLDER) != 0;
    uint32_t col = placeholder ? CTL_PLACEHOLD : (it->state & WEB_CTL_DISABLED) ? CTL_DISABLED : it->color;
    const char *txt = it->text ? it->text : "";
    size_t len = it->len;
    const char *value = focused && !placeholder ? web_control_value(it->node) : "";
    if (!value) value = "";
    const char *type = focused ? web_element_get_attr(it->node, "type") : NULL;
    bool password = type && strcasecmp(type, "password") == 0;
    size_t lo = st->o->caret < st->o->anchor ? st->o->caret : st->o->anchor, hi = st->o->caret < st->o->anchor ? st->o->anchor : st->o->caret;
    if (!multiline) {
        int x = g.text_x - (focused ? st->o->focus_scroll : 0);
        if (focused && !placeholder && hi > lo) {
            int xa = x + text_w(st->p, st->scale, g.font, txt, shown_offset(value, lo, password));
            int xb = x + text_w(st->p, st->scale, g.font, txt, shown_offset(value, hi, password));
            fill(st, (BrRect){ xa, g.baseline - g.line_h * 4 / 5, xb, g.baseline + g.line_h / 5 + 1 }, BR_SELECTION_BG);
        }
        draw_run(st, g.font, txt, len, x, g.baseline, col);
        if (focused && st->o->caret_on) {
            int cx = x + (placeholder ? 0 : text_w(st->p, st->scale, g.font, txt, shown_offset(value, st->o->caret, password)));
            draw_caret(st, cx, g.baseline - g.line_h * 4 / 5, g.baseline + g.line_h / 5 + 1, it->color);
        }
    } else {
        int width = g.content.x1 - g.content.x0;
        size_t nl = brp_wrap(st->p, g.font, st->scale, width, txt, len, NULL, 0);
        BrLine *lines = malloc((nl ? nl : 1) * sizeof *lines);
        if (lines) {
            brp_wrap(st->p, g.font, st->scale, width, txt, len, lines, nl);
            int top = g.baseline - (focused ? st->o->focus_scroll : 0);
            size_t carets = 0;
            for (size_t i = 0; i < nl; i++) if (lines[i].start <= st->o->caret) carets = i;
            for (size_t i = 0; i < nl; i++) {
                int base = top + (int)i * g.line_h;
                if (base + g.line_h < g.content.y0 || base - g.line_h > g.content.y1) continue;
                const char *ls = txt + lines[i].start;
                size_t ln = lines[i].end - lines[i].start;
                if (focused && !placeholder && hi > lo && hi > lines[i].start && lo < lines[i].end + 1) {
                    size_t a = lo > lines[i].start ? lo - lines[i].start : 0, b = hi < lines[i].end ? hi - lines[i].start : ln;
                    int xa = g.text_x + text_w(st->p, st->scale, g.font, ls, a), xb = g.text_x + text_w(st->p, st->scale, g.font, ls, b);
                    if (hi > lines[i].end) xb += text_w(st->p, st->scale, g.font, " ", 1);
                    fill(st, (BrRect){ xa, base - g.line_h * 4 / 5, xb, base + g.line_h / 5 + 1 }, BR_SELECTION_BG);
                }
                draw_run(st, g.font, ls, ln, g.text_x, base, col);
                if (focused && st->o->caret_on && i == carets && !placeholder) {
                    size_t a = st->o->caret > lines[i].start ? st->o->caret - lines[i].start : 0;
                    if (a > ln) a = ln;
                    draw_caret(st, g.text_x + text_w(st->p, st->scale, g.font, ls, a), base - g.line_h * 4 / 5, base + g.line_h / 5 + 1, it->color);
                }
            }
            free(lines);
        }
    }
    st->cur = saved;
}

static void draw_checkable(PState *st, const WebDisplayItem *it, BrRect box) {
    int s = st->scale;
    int w = box.x1 - box.x0, h = box.y1 - box.y0, side = imin(w, h);
    BrRect sq = { box.x0 + (w - side) / 2, box.y0 + (h - side) / 2, box.x0 + (w - side) / 2 + side, box.y0 + (h - side) / 2 + side };
    bool radio = it->control == WEB_CONTROL_RADIO, checked = (it->state & WEB_CTL_CHECKED) != 0, off = (it->state & WEB_CTL_DISABLED) != 0;
    uint32_t frame = off ? CTL_DISABLED : (it->state & WEB_CTL_HOVER) ? 0xFF3C3C3Cu : CTL_FRAME, face = off ? 0xFFE8E8E8u : 0xFFFFFFFFu;
    if (radio) {
        draw_ellipse(st, sq, frame, 0);
        draw_ellipse(st, inset(sq, s), face, 0);
        if (checked) draw_ellipse(st, inset(sq, side / 4 + s / 2), off ? CTL_DISABLED : CTL_ACCENT, 0);
    } else {
        int rad[4] = { s, s, s, s };
        fill_rrect(st, sq, rad, frame);
        int r2[4] = { s > 1 ? s - 1 : 0, s > 1 ? s - 1 : 0, s > 1 ? s - 1 : 0, s > 1 ? s - 1 : 0 };
        fill_rrect(st, inset(sq, s), r2, face);
        if (checked) {
            float fs = (float)side;
            uint32_t c = off ? CTL_DISABLED : CTL_ACCENT;
            float lw = fs * 0.17f < 1.5f ? 1.5f : fs * 0.17f;
            stroke(st, (float)sq.x0 + fs * 0.22f, (float)sq.y0 + fs * 0.52f, (float)sq.x0 + fs * 0.42f, (float)sq.y0 + fs * 0.72f, lw, c);
            stroke(st, (float)sq.x0 + fs * 0.42f, (float)sq.y0 + fs * 0.72f, (float)sq.x0 + fs * 0.80f, (float)sq.y0 + fs * 0.28f, lw, c);
        }
    }
    if (it->state & WEB_CTL_FOCUSED) {
        BrRect f = { sq.x0 - 2 * s, sq.y0 - 2 * s, sq.x1 + 2 * s, sq.y1 + 2 * s };
        fill(st, (BrRect){ f.x0, f.y0, f.x1, f.y0 + s }, CTL_ACCENT);
        fill(st, (BrRect){ f.x0, f.y1 - s, f.x1, f.y1 }, CTL_ACCENT);
        fill(st, (BrRect){ f.x0, f.y0, f.x0 + s, f.y1 }, CTL_ACCENT);
        fill(st, (BrRect){ f.x1 - s, f.y0, f.x1, f.y1 }, CTL_ACCENT);
    }
}

static float attr_float(WebNode *n, const char *name, float dflt) {
    const char *v = web_element_get_attr(n, name);
    return v && *v ? (float)atof(v) : dflt;
}

static uint32_t parse_hex_color(const char *v) {
    if (!v || v[0] != '#' || strlen(v) != 7) return 0xFF000000u;
    unsigned x = 0;
    for (int i = 1; i < 7; i++) {
        char c = v[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 0;
        x = x * 16 + d;
    }
    return 0xFF000000u | x;
}

static void draw_button_label(PState *st, const WebDisplayItem *it) {
    BrControlGeom g;
    if (!it->len || !brp_control_geom(st->p, it, st->ox, st->oy, st->scale, &g)) return;
    BrRect saved = st->cur;
    st->cur = br_rect_and(saved, g.content);
    int tw = text_w(st->p, st->scale, g.font, it->text, it->len), cw = g.content.x1 - g.content.x0;
    int x = tw < cw ? g.content.x0 + (cw - tw) / 2 : g.content.x0, base = g.baseline;
    if (it->state & WEB_CTL_ACTIVE) { x += st->scale; base += st->scale; }
    draw_run(st, g.font, it->text, it->len, x, base, (it->state & WEB_CTL_DISABLED) ? CTL_DISABLED : it->color);
    st->cur = saved;
}

static void draw_select(PState *st, const WebDisplayItem *it, BrRect box) {
    BrControlGeom g;
    if (!brp_control_geom(st->p, it, st->ox, st->oy, st->scale, &g)) return;
    int s = st->scale;
    BrRect arrow = { g.content.x1 - 17 * s, g.content.y0, g.content.x1, g.content.y1 };
    if (arrow.x0 < g.content.x0) arrow.x0 = g.content.x0;
    fill(st, arrow, (it->state & WEB_CTL_HOVER) ? 0xFFE3E3E3u : CTL_FACE);
    fill(st, (BrRect){ arrow.x0, arrow.y0, arrow.x0 + s, arrow.y1 }, 0xFFBDBDBDu);
    int cx = (arrow.x0 + arrow.x1) / 2, cy = (arrow.y0 + arrow.y1) / 2, half = 4 * s;
    for (int row = 0; row < half; row++) {                         /* a small downward triangle */
        int hw = half - row;
        fill(st, (BrRect){ cx - hw, cy - half / 2 + row, cx + hw, cy - half / 2 + row + 1 }, (it->state & WEB_CTL_DISABLED) ? CTL_DISABLED : 0xFF404040u);
    }
    BrRect saved = st->cur;
    st->cur = br_rect_and(saved, (BrRect){ g.content.x0, g.content.y0, arrow.x0, g.content.y1 });
    draw_run(st, g.font, it->text, it->len, g.content.x0 + 4 * s, g.baseline, (it->state & WEB_CTL_DISABLED) ? CTL_DISABLED : it->color);
    st->cur = saved;
    (void)box;
}

static void draw_listbox(PState *st, const WebDisplayItem *it) {
    BrControlGeom g;
    if (!brp_control_geom(st->p, it, st->ox, st->oy, st->scale, &g)) return;
    BrRect saved = st->cur;
    st->cur = br_rect_and(saved, g.content);
    const char *labels[256];
    int n = web_select_options(it->node, labels, 256), sel = web_select_index(it->node);
    if (n > 256) n = 256;
    int rowh = g.line_h + 2 * st->scale;
    int rows = rowh > 0 ? (g.content.y1 - g.content.y0) / rowh : 1;
    if (rows < 1) rows = 1;
    int first = sel >= rows ? sel - rows + 1 : 0;
    for (int i = first; i < n && i < first + rows + 1; i++) {
        BrRect row = { g.content.x0, g.content.y0 + (i - first) * rowh, g.content.x1, g.content.y0 + (i - first + 1) * rowh };
        bool on = i == sel;
        if (on) fill(st, row, CTL_SELECT_BG);
        draw_run(st, g.font, labels[i], strlen(labels[i]), g.content.x0 + 3 * st->scale, row.y0 + st->scale + g.line_h * 4 / 5, on ? 0xFFFFFFFFu : it->color);
    }
    st->cur = saved;
}

static void draw_range(PState *st, const WebDisplayItem *it, BrRect box) {
    int s = st->scale;
    float mn = attr_float(it->node, "min", 0), mx = attr_float(it->node, "max", 100);
    float val = attr_float(it->node, "value", mn + (mx - mn) / 2);
    float t = mx > mn ? (val - mn) / (mx - mn) : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    int thumb = 14 * s, cy = (box.y0 + box.y1) / 2, x0 = box.x0 + thumb / 2, x1 = box.x1 - thumb / 2;
    bool off = (it->state & WEB_CTL_DISABLED) != 0;
    int tx = x0 + (int)((float)(x1 - x0) * t);
    int rad[4] = { 2 * s, 2 * s, 2 * s, 2 * s };
    fill_rrect(st, (BrRect){ x0, cy - 2 * s, x1, cy + 2 * s }, rad, 0xFFBDBDBDu);
    if (tx > x0) fill_rrect(st, (BrRect){ x0, cy - 2 * s, tx, cy + 2 * s }, rad, off ? CTL_DISABLED : CTL_ACCENT);
    draw_ellipse(st, (BrRect){ tx - thumb / 2, cy - thumb / 2, tx + thumb / 2, cy + thumb / 2 }, off ? CTL_DISABLED : CTL_FRAME, 0);
    draw_ellipse(st, (BrRect){ tx - thumb / 2 + s, cy - thumb / 2 + s, tx + thumb / 2 - s, cy + thumb / 2 - s }, 0xFFFFFFFFu, 0);
}

static void draw_bar(PState *st, const WebDisplayItem *it, BrRect box, uint32_t fillc) {
    int s = st->scale;
    int rad[4] = { 3 * s, 3 * s, 3 * s, 3 * s };
    fill_rrect(st, box, rad, CTL_FRAME);
    fill_rrect(st, inset(box, s), rad, 0xFFFFFFFFu);
    int inner = box.x1 - box.x0 - 4 * s;
    int fw = inner * iclamp(it->width, 0, 1000) / 1000;
    if (fw > 0) fill_rrect(st, (BrRect){ box.x0 + 2 * s, box.y0 + 2 * s, box.x0 + 2 * s + fw, box.y1 - 2 * s }, rad, fillc);
}

static void draw_control(PState *st, const WebDisplayItem *it) {
    BrRect box = item_rect(st, it);
    if (br_rect_empty(br_rect_and(box, st->cur))) return;
    int s = st->scale;
    switch (it->control) {
    case WEB_CONTROL_TEXT: draw_field(st, it, false); break;
    case WEB_CONTROL_TEXTAREA: draw_field(st, it, true); break;
    case WEB_CONTROL_CHECKBOX: case WEB_CONTROL_RADIO: draw_checkable(st, it, box); break;
    case WEB_CONTROL_BUTTON:
        draw_button_label(st, it);
        if (it->state & WEB_CTL_ACTIVE) fill(st, box, 0x18000000u);
        else if (it->state & WEB_CTL_HOVER) fill(st, box, 0x22FFFFFFu);
        break;
    case WEB_CONTROL_SELECT: draw_select(st, it, box); break;
    case WEB_CONTROL_LISTBOX: draw_listbox(st, it); break;
    case WEB_CONTROL_RANGE: draw_range(st, it, box); break;
    case WEB_CONTROL_COLOR: {
        BrRect in = inset(box, 2 * s);
        fill(st, box, CTL_FRAME);
        fill(st, inset(box, s), 0xFFFFFFFFu);
        fill(st, in, parse_hex_color(web_element_get_attr(it->node, "value")));
        break;
    }
    case WEB_CONTROL_FILE: {
        BrControlGeom g;
        if (!brp_control_geom(st->p, it, st->ox, st->oy, s, &g)) break;
        int bw = text_w(st->p, s, g.font, it->text, it->len) + 16 * s;
        BrRect btn = { box.x0, box.y0, box.x0 + bw, box.y1 };
        int rad[4] = { s, s, s, s };
        fill_rrect(st, btn, rad, CTL_FRAME);
        fill_rrect(st, inset(btn, s), rad, CTL_FACE);
        draw_run(st, g.font, it->text, it->len, btn.x0 + 8 * s, g.baseline, it->color);
        draw_run(st, g.font, "No file chosen", 14, btn.x1 + 8 * s, g.baseline, CTL_PLACEHOLD);
        break;
    }
    case WEB_CONTROL_PROGRESS: draw_bar(st, it, box, 0xFF3390FFu); break;
    case WEB_CONTROL_METER: draw_bar(st, it, box, 0xFF3FA34Du); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- scroll bars */

static void draw_scrollbars(PState *st, WebNode *node, BrRect r) {
    int x, y, mx, my;
    if (!node || !web_element_scroll(node, &x, &y, &mx, &my)) return;
    int s = st->scale, bar = 6 * s;
    int rad[4] = { 3 * s, 3 * s, 3 * s, 3 * s };
    int view_h = r.y1 - r.y0, view_w = r.x1 - r.x0;
    if (my > 0 && view_h > 0) {
        int total = view_h + my * s, th = imax(16 * s, view_h * view_h / total);
        int ty = r.y0 + (view_h - th) * imin(y * s, my * s) / (my * s);
        fill(st, (BrRect){ r.x1 - bar, r.y0, r.x1, r.y1 }, 0x14000000u);
        fill_rrect(st, (BrRect){ r.x1 - bar, ty, r.x1, ty + th }, rad, 0x66000000u);
    }
    if (mx > 0 && view_w > 0) {
        int total = view_w + mx * s, tw = imax(16 * s, view_w * view_w / total);
        int tx = r.x0 + (view_w - tw) * imin(x * s, mx * s) / (mx * s);
        fill(st, (BrRect){ r.x0, r.y1 - bar, r.x1, r.y1 }, 0x14000000u);
        fill_rrect(st, (BrRect){ tx, r.y1 - bar, tx + tw, r.y1 }, rad, 0x66000000u);
    }
}

/* ---------------------------------------------------------------- the display list */

void brp_render(BrPainter *p, BrSurface *dst, const WebDisplayList *dl, const BrPaintOpts *o) {
    if (!p || !dst || !dst->px || !dl || !o) return;
    PState st;
    memset(&st, 0, sizeof st);
    st.p = p;
    st.dst = dst;
    st.o = o;
    st.ox = o->ox;
    st.oy = o->oy;
    st.scale = o->scale > 0 ? o->scale : 1;
    st.galpha = 255;
    BrRect all = br_rect(0, 0, dst->w, dst->h);
    BrRect clip = (o->clip.x1 > o->clip.x0 && o->clip.y1 > o->clip.y0) ? br_rect_and(o->clip, all) : all;
    st.cur = clip;
    if (br_rect_empty(clip)) return;
    if (o->fill_background) {
        uint32_t bg = dl->background;
        if ((bg >> 24) == 0) bg = 0xFFFFFFFFu;
        fill(&st, clip, bg | 0xFF000000u);
    }
    for (int i = 0; i < dl->n; i++) {
        const WebDisplayItem *it = &dl->items[i];
        switch (it->kind) {
        case WEB_DI_RECT: {
            BrRect r = item_rect(&st, it);
            if (has_radii(it->radii)) {
                int rad[4];
                fit_radii(r.x1 - r.x0, r.y1 - r.y0, it->radii, rad);
                fill_rrect(&st, r, rad, it->color);
            } else fill(&st, r, it->color);
            break;
        }
        case WEB_DI_BORDER: {
            BrRect r = item_rect(&st, it);
            if (has_radii(it->radii)) draw_border_round(&st, r, it);
            else draw_border_rect(&st, r, it);
            break;
        }
        case WEB_DI_TEXT:
            brf_draw_text(p->fonts, dst, st.cur, it->font, st.scale, it->text, it->len, (int32_t)st.ox * 64 + it->x64, (int32_t)st.oy * 64 + it->y64,
                          it->color, st.galpha);
            break;
        case WEB_DI_IMAGE: draw_image(&st, it); break;
        case WEB_DI_CLIP_PUSH:
            if (st.nclip < MAX_CLIPS) { st.clips[st.nclip++] = st.cur; st.cur = br_rect_and(st.cur, item_rect(&st, it)); }
            else st.over++;
            break;
        case WEB_DI_CLIP_POP:
            if (st.over) st.over--;
            else if (st.nclip) st.cur = st.clips[--st.nclip];
            break;
        case WEB_DI_OPACITY_PUSH:
            if (st.nalpha < MAX_CLIPS) { st.alphas[st.nalpha++] = st.galpha; st.galpha = br_div255(st.galpha * it->alpha); }
            else st.over_a++;
            break;
        case WEB_DI_OPACITY_POP:
            if (st.over_a) st.over_a--;
            else if (st.nalpha) st.galpha = st.alphas[--st.nalpha];
            break;
        case WEB_DI_SCROLL_PUSH:
            if (st.nscr < MAX_SCROLLERS) { st.scr[st.nscr].node = it->node; st.scr[st.nscr].r = item_rect(&st, it); st.nscr++; }
            else st.over_s++;
            break;
        case WEB_DI_SCROLL_POP:
            if (st.over_s) st.over_s--;
            else if (st.nscr) { st.nscr--; draw_scrollbars(&st, st.scr[st.nscr].node, st.scr[st.nscr].r); }
            break;
        case WEB_DI_CONTROL: draw_control(&st, it); break;
        case WEB_DI_ELLIPSE: draw_ellipse(&st, item_rect(&st, it), it->color, it->width > 0 ? it->width : 0); break;
        case WEB_DI_SHADOW: draw_shadow(&st, item_rect(&st, it), it); break;
        default: break;
        }
    }
}
