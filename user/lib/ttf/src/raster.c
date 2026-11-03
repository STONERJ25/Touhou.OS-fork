/*
 * The rasteriser: outline in, 8-bit coverage out. The signed-area accumulation method (the idea is in the public domain
 * literature on analytic anti-aliasing; this is a from-scratch derivation).
 *
 * The bitmap is a grid of unit cells. Every straight edge of the (flattened) outline is cut at the cell boundaries it crosses, and
 * each piece contributes to its cell exactly the signed area that lies to the right of it inside the cell's row band:
 *
 *      a piece of signed height d (down is positive) whose midpoint is at fraction m of the cell width covers
 *      d * (1 - m) of its own cell, and d of every cell further right.
 *
 * We store that as d*(1-m) in the cell and d*m in the next cell, so that a running sum along the row, which adds up everything to
 * the left, yields the area of the cell covered. No sampling is involved: for a polygon the sum IS the exact covered area of each
 * pixel, to float precision. Curves are first flattened into lines, closer than FLATNESS to the true curve.
 *
 * Non-zero winding: the running sum is the winding number integrated over the cell. Its absolute value clamped to 1 is exact where
 * the winding is 0 or +-1 throughout the pixel (and 2 or more only in the interior of a pixel that is solid anyway), which is every
 * pixel of an ordinary glyph. It overestimates a pixel that holds the edge of one contour and the edge of another one overlapping it
 * (two strokes crossing at a junction): there the true coverage is the area of the union, the sum counts the overlap twice. That is
 * the usual price of this method; fonts with overlapping contours (many modern ones) look right apart from a slightly heavy pixel at
 * the crossing. docs/TTF.md says so.
 *
 * LCD mode renders at three times the horizontal resolution, then filters the stripes with a 5-tap triangle ([1 2 3 2 1]/9) so that
 * colour fringes stay soft; the three stripes of a pixel are its three output bytes. There is no hinting anywhere.
 */
#include "ttfint.h"

#define FLATNESS 0.02f                  /* flattening tolerance, pixels */
#define MAX_SEGMENTS 256                /* per curve */

typedef struct Acc {
    float *cells;
    int w, h;                           /* cells per row (not counting the 2 spare columns) and rows */
    int stride;                         /* w + 2: pieces that touch the right edge write up to two cells beyond it */
} Acc;

static inline float absf(float v) { return v < 0 ? -v : v; }
static inline int ifloor(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
static inline int iceil(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* One piece of an edge inside a single row band, from x xa to xb, signed height d. */
static void row_piece(float *line, float xa, float xb, float d, float wmax) {
    if (xa > xb) { float t = xa; xa = xb; xb = t; }
    if (xa < 0) xa = 0;
    if (xb < 0) xb = 0;
    if (xa > wmax) xa = wmax;
    if (xb > wmax) xb = wmax;
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib) {                                         /* inside one cell: the common case */
        float m = 0.5f * (xa + xb) - (float)ia;
        line[ia] += d * (1.0f - m);
        line[ia + 1] += d * m;
        return;
    }
    float inv = d / (xb - xa);                              /* height per unit of x */
    float xe = (float)(ia + 1);
    float h = (xe - xa) * inv;
    float m = 0.5f * ((xa - (float)ia) + 1.0f);
    line[ia] += h * (1.0f - m);
    line[ia + 1] += h * m;
    for (int c = ia + 1; c < ib; c++) {                     /* whole cells crossed: height inv, midpoint 1/2 */
        line[c] += inv * 0.5f;
        line[c + 1] += inv * 0.5f;
    }
    h = (xb - (float)ib) * inv;
    m = 0.5f * (xb - (float)ib);
    line[ib] += h * (1.0f - m);
    line[ib + 1] += h * m;
}

static void add_edge(const Acc *A, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;                                   /* horizontal edges cover no area to their right */
    float dir = 1.0f;
    if (y0 > y1) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1.0f;
    }
    float hmax = (float)A->h;
    if (y1 <= 0.0f || y0 >= hmax) return;
    float dxdy = (x1 - x0) / (y1 - y0);
    if (y0 < 0.0f) { x0 -= y0 * dxdy; y0 = 0.0f; }
    if (y1 > hmax) { x1 = x0 + (hmax - y0) * dxdy; y1 = hmax; }
    int row0 = (int)y0, row1 = iceil(y1);
    float wmax = (float)A->w;
    for (int row = row0; row < row1; row++) {
        float ya = y0 > (float)row ? y0 : (float)row;
        float yb = y1 < (float)(row + 1) ? y1 : (float)(row + 1);
        float dy = yb - ya;
        if (!(dy > 0.0f)) continue;
        float xa = x0 + (ya - y0) * dxdy, xb = x0 + (yb - y0) * dxdy;
        row_piece(A->cells + (size_t)row * (size_t)A->stride, xa, xb, dy * dir, wmax);
    }
}

static uint32_t segments_for(float dd, float k) {
    float q = dd * k;                                       /* n >= sqrt(q) keeps the chord error under FLATNESS */
    if (!(q < 1000000.0f)) q = 1000000.0f;
    uint32_t n = isqrt32((uint32_t)q) + 1;
    return n > MAX_SEGMENTS ? MAX_SEGMENTS : n;
}

/* A quadratic's chord error with n equal steps is at most |p0 - 2 p1 + p2| / (4 n^2). */
static void flatten_quad(const Acc *A, float x0, float y0, float x1, float y1, float x2, float y2) {
    float dd = absf(x0 - 2 * x1 + x2) + absf(y0 - 2 * y1 + y2);       /* the sum of the components bounds the length */
    uint32_t n = segments_for(dd, 1.0f / (4.0f * FLATNESS));
    float px = x0, py = y0, inv = 1.0f / (float)n;
    for (uint32_t i = 1; i < n; i++) {
        float t = (float)i * inv, mt = 1.0f - t;
        float a = mt * mt, b = 2.0f * mt * t, c = t * t;
        float x = a * x0 + b * x1 + c * x2, y = a * y0 + b * y1 + c * y2;
        add_edge(A, px, py, x, y);
        px = x; py = y;
    }
    add_edge(A, px, py, x2, y2);
}

/* A cubic's chord error with n equal steps is at most 3/4 * max(|p0 - 2 p1 + p2|, |p1 - 2 p2 + p3|) / n^2. */
static void flatten_cubic(const Acc *A, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    float d1 = absf(x0 - 2 * x1 + x2) + absf(y0 - 2 * y1 + y2);
    float d2 = absf(x1 - 2 * x2 + x3) + absf(y1 - 2 * y2 + y3);
    uint32_t n = segments_for(d1 > d2 ? d1 : d2, 0.75f / FLATNESS);
    float px = x0, py = y0, inv = 1.0f / (float)n;
    for (uint32_t i = 1; i < n; i++) {
        float t = (float)i * inv, mt = 1.0f - t;
        float a = mt * mt * mt, b = 3.0f * mt * mt * t, c = 3.0f * mt * t * t, d = t * t * t;
        float x = a * x0 + b * x1 + c * x2 + d * x3, y = a * y0 + b * y1 + c * y2 + d * y3;
        add_edge(A, px, py, x, y);
        px = x; py = y;
    }
    add_edge(A, px, py, x3, y3);
}

/* The running sum along a row, folded to 0..255. */
static void resolve_row(const float *line, int w, uint8_t *dst) {
    float sum = 0.0f;
    for (int x = 0; x < w; x++) {
        sum += line[x];
        float a = sum < 0 ? -sum : sum;
        if (a > 1.0f) a = 1.0f;
        dst[x] = (uint8_t)(a * 255.0f + 0.5f);
    }
}

int ttf_render(TtfRaster *r, const TtfFont *f, uint32_t glyph, const TtfRenderParams *p, TtfBitmap *out) {
    if (!out) return TTF_E_ARG;
    memset(out, 0, sizeof *out);
    out->bpp = 1;
    if (!r || !f || !p) return TTF_E_ARG;
    float size = p->size_px;
    if (!(size >= 1.0f && size <= TTF_MAX_SIZE_PX)) return TTF_E_ARG;
    if (p->mode != TTF_MODE_GRAY && p->mode != TTF_MODE_LCD) return TTF_E_ARG;
    float sub_x = p->sub_x, sub_y = p->sub_y, skew = p->skew;
    if (!(sub_x >= 0.0f)) sub_x = 0.0f;
    if (sub_x > 0.99999f) sub_x = 0.99999f;
    if (!(sub_y >= 0.0f)) sub_y = 0.0f;
    if (sub_y > 0.99999f) sub_y = 0.99999f;
    if (!(skew > -4.0f && skew < 4.0f)) skew = 0.0f;
    const int lcd = p->mode == TTF_MODE_LCD;
    const int hx = lcd ? 3 : 1;
    out->bpp = lcd ? 3 : 1;
    const float s = ttf_scale(f, size);

    const TtfOutline *ol;
    int rc = ttf_glyph_outline(r, f, glyph, &ol);
    if (rc) return rc;
    out->advance = (float)ttf_advance_units(f, glyph) * s;          /* known even when there is nothing to draw (a space) */
    if (ol->nops == 0) return TTF_OK;

    /* Bounds in pixels. Skew is linear, so the extremes of x are at corners of the font-unit box. */
    float xs[4], lo, hi;
    xs[0] = ol->xmin + skew * ol->ymin; xs[1] = ol->xmax + skew * ol->ymin;
    xs[2] = ol->xmin + skew * ol->ymax; xs[3] = ol->xmax + skew * ol->ymax;
    lo = hi = xs[0];
    for (int i = 1; i < 4; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
    float fx0 = lo * s + sub_x, fx1 = hi * s + sub_x;
    float fy0 = ol->ymin * s + sub_y, fy1 = ol->ymax * s + sub_y;
    if (!(absf(fx0) < 1e7f && absf(fx1) < 1e7f && absf(fy0) < 1e7f && absf(fy1) < 1e7f)) return TTF_E_TOOBIG;     /* also catches NaN */
    const float eps = 1e-4f;                                /* coordinates that are whole numbers up to rounding stay on their pixel edge */
    int px0 = ifloor(fx0 + eps), px1 = iceil(fx1 - eps), py0 = ifloor(fy0 + eps), py1 = iceil(fy1 - eps);
    if (lcd) { px0--; px1++; }                              /* room for the filter's tails */
    if (px1 <= px0 || py1 <= py0) return TTF_OK;
    int W = px1 - px0, H = py1 - py0;
    if (W > TTF_MAX_DIM || H > TTF_MAX_DIM) return TTF_E_TOOBIG;
    size_t cw = (size_t)W * (size_t)hx;
    size_t stride = cw + 2;
    if (stride * (size_t)H > TTF_MAX_CELLS) return TTF_E_TOOBIG;

    size_t ncells = stride * (size_t)H;
    if (ttfi_reserve_sz((void **)&r->acc, &r->acc_cap, ncells, sizeof(float))) return TTF_E_NOMEM;
    memset(r->acc, 0, ncells * sizeof(float));
    Acc A;
    A.cells = r->acc;
    A.w = (int)cw;
    A.h = H;
    A.stride = (int)stride;

    /* Walk the path in cell coordinates: u = (X - px0) * hx to the right, v = py1 - Y downward. */
    const float *pt = ol->pts;
    const float ox = (float)px0, oy = (float)py1, fhx = (float)hx;
#define TX(x, y) ((((x) + skew * (y)) * s + sub_x - ox) * fhx)
#define TY(y)    (oy - ((y) * s + sub_y))
    float sx = 0, sy = 0, cx = TX(0.0f, 0.0f), cy = TY(0.0f);
    int open = 0;
    for (uint32_t i = 0; i < ol->nops; i++) {
        int op = ol->ops[i];
        if (op != TTF_OP_MOVE && op != TTF_OP_CLOSE && !open) { sx = cx; sy = cy; open = 1; }      /* drawing before any moveto starts at the current point */
        switch (op) {
        case TTF_OP_MOVE: {
            if (open) add_edge(&A, cx, cy, sx, sy);
            cx = sx = TX(pt[0], pt[1]);
            cy = sy = TY(pt[1]);
            open = 1;
            pt += 2;
            break;
        }
        case TTF_OP_LINE: {
            float x = TX(pt[0], pt[1]), y = TY(pt[1]);
            add_edge(&A, cx, cy, x, y);
            cx = x; cy = y;
            pt += 2;
            break;
        }
        case TTF_OP_QUAD: {
            float x1 = TX(pt[0], pt[1]), y1 = TY(pt[1]), x2 = TX(pt[2], pt[3]), y2 = TY(pt[3]);
            flatten_quad(&A, cx, cy, x1, y1, x2, y2);
            cx = x2; cy = y2;
            pt += 4;
            break;
        }
        case TTF_OP_CUBIC: {
            float x1 = TX(pt[0], pt[1]), y1 = TY(pt[1]), x2 = TX(pt[2], pt[3]), y2 = TY(pt[3]), x3 = TX(pt[4], pt[5]), y3 = TY(pt[5]);
            flatten_cubic(&A, cx, cy, x1, y1, x2, y2, x3, y3);
            cx = x3; cy = y3;
            pt += 6;
            break;
        }
        default:                                            /* CLOSE */
            if (open) add_edge(&A, cx, cy, sx, sy);
            cx = sx; cy = sy;
            open = 0;
            break;
        }
    }
    if (open) add_edge(&A, cx, cy, sx, sy);                 /* a contour left open is closed by the fill */
#undef TX
#undef TY

    size_t nbytes = cw * (size_t)H;
    if (ttfi_reserve_sz((void **)&r->bmp, &r->bmp_cap, nbytes, 1)) return TTF_E_NOMEM;
    if (!lcd) {
        for (int y = 0; y < H; y++) resolve_row(A.cells + (size_t)y * stride, W, r->bmp + (size_t)y * (size_t)W);
    } else {
        if (ttfi_reserve_sz((void **)&r->gray, &r->gray_cap, nbytes, 1)) return TTF_E_NOMEM;
        for (int y = 0; y < H; y++) {
            uint8_t *g = r->gray + (size_t)y * cw, *d = r->bmp + (size_t)y * cw;
            resolve_row(A.cells + (size_t)y * stride, (int)cw, g);
            for (int i = 0; i < (int)cw; i++) {             /* triangle filter over five stripes: weights 28 57 86 57 28 (sum 256) */
                int v = 86 * g[i];
                if (i >= 1) v += 57 * g[i - 1];
                if (i >= 2) v += 28 * g[i - 2];
                if (i + 1 < (int)cw) v += 57 * g[i + 1];
                if (i + 2 < (int)cw) v += 28 * g[i + 2];
                v = (v + 128) >> 8;
                d[i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
    out->width = W;
    out->height = H;
    out->left = px0;
    out->top = py1;
    out->stride = (int)cw;
    out->bpp = lcd ? 3 : 1;
    out->data = r->bmp;
    return TTF_OK;
}
