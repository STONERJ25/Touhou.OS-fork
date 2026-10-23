/* Rasterising. Every shape is turned into horizontal spans and handed to a Sink. The picture sink writes pixels (after
 * telling the history about them); the screen sink draws the same spans magnified into the canvas view. Previews are
 * therefore pixel-identical to what the tool finally paints. */
#include "sketchbook.h"
#include <font8x16.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- sinks */

static void isink_span(Sink *s, int x0, int x1, int y) {
    if (y < 0 || y >= g_doc.h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= g_doc.w) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= g_doc.w) x1 = g_doc.w - 1;
    hist_touch(x0, y, x1, y);
    uint32_t *p = g_doc.px + (size_t)y * g_doc.w + x0;
    uint32_t c = s->color;
    for (int n = x1 - x0 + 1; n > 0; n--) *p++ = c;
}

Sink g_isink = { isink_span, 0, 255 };

static void ssink_span(Sink *s, int x0, int x1, int y) {
    ScreenSink *ss = (ScreenSink *)s;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    KRect r = kr(ss->ox + x0 * ss->z, ss->oy + y * ss->z, (x1 - x0 + 1) * ss->z, ss->z);
    if (s->alpha >= 255) kc_fill(ss->cv, r, s->color);
    else kc_fill_alpha(ss->cv, r, s->color, s->alpha);
}

void ssink_init(ScreenSink *s, KCanvas *cv, int ox, int oy, int z) {
    s->base.span = ssink_span;
    s->base.color = 0;
    s->base.alpha = 255;
    s->cv = cv;
    s->ox = ox;
    s->oy = oy;
    s->z = z;
}

/* ---------------------------------------------------------------- brushes and lines */

static int g_stamp_size;
static int8_t g_stamp_lo[SK_TILE], g_stamp_hi[SK_TILE];

static void stamp_prepare(int s) {
    if (s == g_stamp_size) return;
    for (int r = 0; r < s; r++) {
        int lo = -1, hi = -1;
        for (int c = 0; c < s; c++) {
            int dx = 2 * c - (s - 1), dy = 2 * r - (s - 1);
            if (dx * dx + dy * dy <= s * s - 2) { if (lo < 0) lo = c; hi = c; }
        }
        g_stamp_lo[r] = (int8_t)lo;
        g_stamp_hi[r] = (int8_t)hi;
    }
    g_stamp_size = s;
}

void rs_stamp(Sink *sn, int cx, int cy, int size, bool square) {
    if (size <= 1) { sn->span(sn, cx, cx, cy); return; }
    if (size > 32) size = 32;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    if (square) {
        for (int r = 0; r < size; r++) sn->span(sn, x0, x0 + size - 1, y0 + r);
        return;
    }
    stamp_prepare(size);
    for (int r = 0; r < size; r++)
        if (g_stamp_lo[r] >= 0) sn->span(sn, x0 + g_stamp_lo[r], x0 + g_stamp_hi[r], y0 + r);
}

void rs_line(Sink *sn, int x0, int y0, int x1, int y1, int size, bool square) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        rs_stamp(sn, x0, y0, size, square);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void rs_fill_rect(Sink *sn, int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) sn->span(sn, x0, x1, y);
}

/* ---------------------------------------------------------------- rectangles */

void rs_rect(Sink *sn, int x0, int y0, int x1, int y1, int thick, uint32_t prim, uint32_t sec, int style) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (thick < 1) thick = 1;
    if (style == S_FILLED || 2 * thick >= w || 2 * thick >= h) {
        sn->color = prim;
        rs_fill_rect(sn, x0, y0, x1, y1);
        return;
    }
    if (style == S_BOTH) {
        sn->color = sec;
        rs_fill_rect(sn, x0 + thick, y0 + thick, x1 - thick, y1 - thick);
    }
    sn->color = prim;
    for (int y = y0; y < y0 + thick; y++) sn->span(sn, x0, x1, y);
    for (int y = y1 - thick + 1; y <= y1; y++) sn->span(sn, x0, x1, y);
    for (int y = y0 + thick; y <= y1 - thick; y++) {
        sn->span(sn, x0, x0 + thick - 1, y);
        sn->span(sn, x1 - thick + 1, x1, y);
    }
}

/* ---------------------------------------------------------------- ellipses
 * A pixel belongs to the ellipse inscribed in the w x h box when its centre is inside it. With doubled coordinates this is
 * integer arithmetic: ((2x - cx2) / w)^2 + ((2y - cy2) / h)^2 <= 1, which treats odd and even boxes alike. */

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

static bool ell_row(int64_t cx2, int64_t cy2, int64_t w, int64_t h, int y, int *xa, int *xb) {
    if (w <= 0 || h <= 0) return false;
    int64_t dy = 2 * (int64_t)y - cy2;
    int64_t dy2 = dy * dy;
    if (dy2 >= h * h) return false;
    int64_t rhs = w * w * (h * h - dy2);
    int64_t t = isqrt64((uint64_t)(rhs / (h * h)));
    *xa = (int)((cx2 - t + 1) >> 1);
    *xb = (int)((cx2 + t) >> 1);
    return *xa <= *xb;
}

void rs_ellipse(Sink *sn, int x0, int y0, int x1, int y1, int thick, uint32_t prim, uint32_t sec, int style) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int64_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    int64_t cx2 = (int64_t)x0 + x1, cy2 = (int64_t)y0 + y1;
    if (thick < 1) thick = 1;
    for (int y = y0; y <= y1; y++) {
        int xa, xb;
        if (!ell_row(cx2, cy2, w, h, y, &xa, &xb)) continue;
        if (style == S_FILLED) { sn->color = prim; sn->span(sn, xa, xb, y); continue; }
        int ia = 0, ib = -1;
        bool inner = false;
        if (thick == 1) {                       /* the interior is whatever has all four neighbours inside */
            int ua, ub, da, db;
            if (y > y0 && y < y1 && ell_row(cx2, cy2, w, h, y - 1, &ua, &ub) && ell_row(cx2, cy2, w, h, y + 1, &da, &db)) {
                ia = sk_max(xa + 1, sk_max(ua, da));
                ib = sk_min(xb - 1, sk_min(ub, db));
                inner = ia <= ib;
            }
        } else if (w - 2 * thick > 0 && h - 2 * thick > 0) {
            inner = ell_row(cx2, cy2, w - 2 * thick, h - 2 * thick, y, &ia, &ib);
            if (inner) { ia = sk_max(ia, xa); ib = sk_min(ib, xb); inner = ia <= ib; }
        }
        if (inner && style == S_BOTH) { sn->color = sec; sn->span(sn, ia, ib, y); }
        sn->color = prim;
        if (!inner) sn->span(sn, xa, xb, y);
        else {
            if (ia > xa) sn->span(sn, xa, ia - 1, y);
            if (ib < xb) sn->span(sn, ib + 1, xb, y);
        }
    }
}

/* ---------------------------------------------------------------- text (the 8x16 font, drawn as pixels) */

void text_metrics(const char *text, int *cols, int *lines) {
    int c = 0, best = 0, l = 1;
    uint32_t cp;
    int b;
    while ((b = kc_utf8_next(text, &cp)) > 0) {
        text += b;
        if (cp == '\n') { if (c > best) best = c; c = 0; l++; }
        else c += cp == '\t' ? 4 : 1;
    }
    if (c > best) best = c;
    if (cols) *cols = best;
    if (lines) *lines = l;
}

void rs_text(Sink *sn, int x, int y, const char *text, int scale, uint32_t fg, bool opaque, uint32_t bgc) {
    if (scale < 1) scale = 1;
    int x0 = x;
    uint32_t cp;
    int b;
    while ((b = kc_utf8_next(text, &cp)) > 0) {
        text += b;
        if (cp == '\n') { x = x0; y += FONT_H * scale; continue; }
        int cells = 1;
        if (cp == '\t') { cells = 4; cp = ' '; }
        for (int cell = 0; cell < cells; cell++, x += FONT_W * scale) {
            if (opaque) {
                sn->color = bgc;
                for (int r = 0; r < FONT_H * scale; r++) sn->span(sn, x, x + FONT_W * scale - 1, y + r);
            }
            if (cp == ' ') continue;
            const unsigned char *g = font8x16_glyph(cp);
            sn->color = fg;
            for (int r = 0; r < FONT_H; r++) {
                unsigned bits = g[r];
                for (int c = 0; c < FONT_W;) {
                    if (!(bits & (0x80u >> c))) { c++; continue; }
                    int c1 = c;
                    while (c1 + 1 < FONT_W && (bits & (0x80u >> (c1 + 1)))) c1++;
                    for (int k = 0; k < scale; k++) sn->span(sn, x + c * scale, x + (c1 + 1) * scale - 1, y + r * scale + k);
                    c = c1 + 1;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- spray can */

void rs_spray(Sink *sn, int cx, int cy, int radius, int dots) {
    if (radius < 1) radius = 1;
    for (int i = 0; i < dots; i++) {
        int dx, dy, tries = 0;
        do {
            dx = rand() % (2 * radius + 1) - radius;
            dy = rand() % (2 * radius + 1) - radius;
        } while (dx * dx + dy * dy > radius * radius && ++tries < 8);
        if (dx * dx + dy * dy > radius * radius) continue;
        sn->span(sn, cx + dx, cx + dx, cy + dy);
    }
}

/* ---------------------------------------------------------------- flood fill (iterative, scanline, 4-connected) */

typedef struct { int x, y; } Seed;

bool fill_flood(int sx, int sy, uint32_t color) {
    int w = g_doc.w, h = g_doc.h;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return false;
    uint32_t *px = g_doc.px;
    uint32_t old = px[(size_t)sy * w + sx] & 0xFFFFFF;
    color &= 0xFFFFFF;
    if (old == color) return false;
    size_t cap = 256, n = 0;
    Seed *st = malloc(cap * sizeof *st);
    if (!st) return false;
    st[n++] = (Seed){ sx, sy };
    const size_t limit = (size_t)8 << 20;                      /* 8M seeds = 64 MB: far more than any real picture needs */
    while (n) {
        Seed s = st[--n];
        uint32_t *row = px + (size_t)s.y * w;
        if ((row[s.x] & 0xFFFFFF) != old) continue;
        int xl = s.x, xr = s.x;
        while (xl > 0 && (row[xl - 1] & 0xFFFFFF) == old) xl--;
        while (xr < w - 1 && (row[xr + 1] & 0xFFFFFF) == old) xr++;
        hist_touch(xl, s.y, xr, s.y);
        for (int i = xl; i <= xr; i++) row[i] = color;
        for (int dy = -1; dy <= 1; dy += 2) {
            int yy = s.y + dy;
            if (yy < 0 || yy >= h) continue;
            const uint32_t *r2 = px + (size_t)yy * w;
            bool in_run = false;
            for (int i = xl; i <= xr; i++) {
                if ((r2[i] & 0xFFFFFF) == old) {
                    if (!in_run) {
                        in_run = true;
                        if (n == cap) {
                            if (cap >= limit) continue;
                            Seed *ns = realloc(st, cap * 2 * sizeof *st);
                            if (!ns) continue;
                            st = ns;
                            cap *= 2;
                        }
                        st[n++] = (Seed){ i, yy };
                    }
                } else in_run = false;
            }
        }
    }
    free(st);
    return true;
}
