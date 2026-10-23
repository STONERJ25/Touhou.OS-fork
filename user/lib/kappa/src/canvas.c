/* Software 2D drawing for 0x00RRGGBB pixel buffers, and the bitmap-font text renderer. */
#include <kappa.h>
#include <font8x16.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- geometry */

KRect kr_intersect(KRect a, KRect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    KRect r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

KRect kr_union(KRect a, KRect b) {
    if (kr_empty(a)) return b;
    if (kr_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    KRect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

KRect kr_inset(KRect r, int d) { KRect o = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d }; return o; }
KRect kr_grow(KRect r, int d) { return kr_inset(r, -d); }

/* ---------------------------------------------------------------- colour */

uint32_t kc_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return KRGB(ar + (br - ar) * t / 256, ag + (bg - ag) * t / 256, ab + (bb - ab) * t / 256);
}

uint32_t kc_shade(uint32_t c, int pct) {
    int r = (int)((c >> 16) & 255) * pct / 100, g = (int)((c >> 8) & 255) * pct / 100, b = (int)(c & 255) * pct / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return KRGB(r, g, b);
}

/* ---------------------------------------------------------------- canvas basics */

void kc_init(KCanvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    c->clip = kr(0, 0, w, h);
}

void kc_set_clip(KCanvas *c, KRect r) { c->clip = kr_intersect(r, kr(0, 0, c->w, c->h)); }

void kc_pixel(KCanvas *c, int x, int y, uint32_t rgb) {
    if (!kr_contains(c->clip, x, y)) return;
    c->px[y * c->stride + x] = rgb;
}

void kc_blend_pixel(KCanvas *c, int x, int y, uint32_t rgb, int a) {
    if (!kr_contains(c->clip, x, y) || a <= 0) return;
    uint32_t *p = &c->px[y * c->stride + x];
    if (a >= 255) { *p = rgb; return; }
    uint32_t d = *p;
    int r = (((rgb >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (((rgb >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = ((rgb & 255) * a + (d & 255) * (255 - a)) / 255;
    *p = KRGB(r, g, b);
}

void kc_fill(KCanvas *c, KRect r, uint32_t rgb) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r)) return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride + r.x;
        for (int x = 0; x < r.w; x++) row[x] = rgb;
    }
}

void kc_fill_alpha(KCanvas *c, KRect r, uint32_t rgb, int a) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r) || a <= 0) return;
    if (a >= 255) { kc_fill(c, r, rgb); return; }
    int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255, ia = 255 - a;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->px + (size_t)y * c->stride + r.x;
        for (int x = 0; x < r.w; x++) {
            uint32_t d = row[x];
            int nr = (sr * a + (int)((d >> 16) & 255) * ia) / 255;
            int ng = (sg * a + (int)((d >> 8) & 255) * ia) / 255;
            int nb = (sb * a + (int)(d & 255) * ia) / 255;
            row[x] = KRGB(nr, ng, nb);
        }
    }
}

void kc_hline(KCanvas *c, int x, int y, int len, uint32_t rgb) { kc_fill(c, kr(x, y, len, 1), rgb); }
void kc_vline(KCanvas *c, int x, int y, int len, uint32_t rgb) { kc_fill(c, kr(x, y, 1, len), rgb); }

void kc_outline(KCanvas *c, KRect r, uint32_t rgb) {
    if (kr_empty(r)) return;
    kc_hline(c, r.x, r.y, r.w, rgb);
    kc_hline(c, r.x, r.y + r.h - 1, r.w, rgb);
    kc_vline(c, r.x, r.y, r.h, rgb);
    kc_vline(c, r.x + r.w - 1, r.y, r.h, rgb);
}

void kc_line(KCanvas *c, int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        kc_pixel(c, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void kc_gradient_v(KCanvas *c, KRect r, uint32_t top, uint32_t bottom) {
    if (kr_empty(r)) return;
    for (int i = 0; i < r.h; i++) kc_fill(c, kr(r.x, r.y + i, r.w, 1), kc_mix(top, bottom, r.h > 1 ? i * 256 / (r.h - 1) : 0));
}

void kc_gradient_h(KCanvas *c, KRect r, uint32_t left, uint32_t right) {
    if (kr_empty(r)) return;
    for (int i = 0; i < r.w; i++) kc_fill(c, kr(r.x + i, r.y, 1, r.h), kc_mix(left, right, r.w > 1 ? i * 256 / (r.w - 1) : 0));
}

void kc_circle_fill(KCanvas *c, int cx, int cy, int radius, uint32_t rgb) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius + radius) dx++;
        kc_hline(c, cx - dx, cy + dy, dx * 2 + 1, rgb);
    }
}

void kc_ellipse_fill(KCanvas *c, int cx, int cy, int rx, int ry, uint32_t rgb) {
    if (rx <= 0 || ry <= 0) return;
    for (int dy = -ry; dy <= ry; dy++) {
        long num = (long)rx * rx * ((long)ry * ry - (long)dy * dy);
        long den = (long)ry * ry;
        int dx = 0;
        while (num >= 0 && (long)(dx + 1) * (dx + 1) * den <= num) dx++;
        kc_hline(c, cx - dx, cy + dy, dx * 2 + 1, rgb);
    }
}

void kc_circle(KCanvas *c, int cx, int cy, int radius, uint32_t rgb) {
    int x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        kc_pixel(c, cx + x, cy + y, rgb); kc_pixel(c, cx - x, cy + y, rgb);
        kc_pixel(c, cx + x, cy - y, rgb); kc_pixel(c, cx - x, cy - y, rgb);
        kc_pixel(c, cx + y, cy + x, rgb); kc_pixel(c, cx - y, cy + x, rgb);
        kc_pixel(c, cx + y, cy - x, rgb); kc_pixel(c, cx - y, cy - x, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void kc_bevel(KCanvas *c, KRect r, KBevel kind) {
    if (r.w < 2 || r.h < 2) return;
    const uint32_t hi = 0xFFFFFF, hi2 = 0xF5F0E2, lo = 0x4A4638, lo2 = 0xA39D8A;
    uint32_t tl1, br1, tl2, br2;
    switch (kind) {
    case KB_RAISED: tl1 = hi; br1 = lo; tl2 = hi2; br2 = lo2; break;
    case KB_SUNKEN: tl1 = lo2; br1 = hi; tl2 = lo; br2 = hi2; break;
    case KB_ETCHED: tl1 = lo2; br1 = hi; tl2 = 0; br2 = 0; break;
    case KB_FRAME:  tl1 = br1 = lo; tl2 = br2 = 0; break;
    default:        tl1 = br1 = lo2; tl2 = br2 = 0; break;
    }
    /* outer ring */
    kc_hline(c, r.x, r.y, r.w, tl1);
    kc_vline(c, r.x, r.y, r.h, tl1);
    kc_hline(c, r.x, r.y + r.h - 1, r.w, br1);
    kc_vline(c, r.x + r.w - 1, r.y, r.h, br1);
    if (kind == KB_RAISED || kind == KB_SUNKEN) {
        kc_hline(c, r.x + 1, r.y + 1, r.w - 2, tl2);
        kc_vline(c, r.x + 1, r.y + 1, r.h - 2, tl2);
        kc_hline(c, r.x + 1, r.y + r.h - 2, r.w - 2, br2);
        kc_vline(c, r.x + r.w - 2, r.y + 1, r.h - 2, br2);
    }
}

void kc_panel(KCanvas *c, KRect r, KBevel kind, uint32_t fill) {
    kc_fill(c, r, fill);
    kc_bevel(c, r, kind);
}

void kc_blit(KCanvas *dst, int x, int y, const uint32_t *src, int sw, int sh, int sstride) {
    KRect r = kr_intersect(kr(x, y, sw, sh), dst->clip);
    if (kr_empty(r)) return;
    for (int row = r.y; row < r.y + r.h; row++)
        memcpy(dst->px + (size_t)row * dst->stride + r.x, src + (size_t)(row - y) * sstride + (r.x - x), (size_t)r.w * 4);
}

void kc_blit_part(KCanvas *dst, int x, int y, const uint32_t *src, int sstride, KRect part) {
    kc_blit(dst, x, y, src + (size_t)part.y * sstride + part.x, part.w, part.h, sstride);
}

void kc_blit_argb(KCanvas *dst, int x, int y, const uint32_t *src, int sw, int sh, int sstride) {
    KRect r = kr_intersect(kr(x, y, sw, sh), dst->clip);
    for (int row = r.y; row < r.y + r.h; row++) {
        const uint32_t *s = src + (size_t)(row - y) * sstride + (r.x - x);
        uint32_t *d = dst->px + (size_t)row * dst->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            uint32_t p = s[i];
            int a = (int)(p >> 24);
            if (a >= 255) d[i] = p & 0xFFFFFF;
            else if (a > 0) {
                uint32_t q = d[i];
                int nr = ((int)((p >> 16) & 255) * a + (int)((q >> 16) & 255) * (255 - a)) / 255;
                int ng = ((int)((p >> 8) & 255) * a + (int)((q >> 8) & 255) * (255 - a)) / 255;
                int nb = ((int)(p & 255) * a + (int)(q & 255) * (255 - a)) / 255;
                d[i] = KRGB(nr, ng, nb);
            }
        }
    }
}

void kc_scroll(KCanvas *c, KRect r, int dy) {
    r = kr_intersect(r, c->clip);
    if (kr_empty(r) || dy == 0 || abs(dy) >= r.h) return;
    if (dy < 0) {
        for (int row = r.y; row < r.y + r.h + dy; row++)
            memcpy(c->px + (size_t)row * c->stride + r.x, c->px + (size_t)(row - dy) * c->stride + r.x, (size_t)r.w * 4);
    } else {
        for (int row = r.y + r.h - 1; row >= r.y + dy; row--)
            memcpy(c->px + (size_t)row * c->stride + r.x, c->px + (size_t)(row - dy) * c->stride + r.x, (size_t)r.w * 4);
    }
}

/* ---------------------------------------------------------------- UTF-8 */

int kc_utf8_next(const char *s, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) { *cp = 0; return 0; }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;                                           /* invalid byte: consume one */
    return 1;
}

size_t kc_utf8_len(const char *s) {
    size_t n = 0;
    uint32_t cp;
    int b;
    while ((b = kc_utf8_next(s, &cp)) > 0) { s += b; n++; }
    return n;
}

size_t kc_utf8_offset(const char *s, size_t idx) {
    const char *p = s;
    uint32_t cp;
    int b;
    while (idx && (b = kc_utf8_next(p, &cp)) > 0) { p += b; idx--; }
    return (size_t)(p - s);
}

int kc_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---------------------------------------------------------------- text */

int kc_glyph(KCanvas *c, int x, int y, uint32_t cp, uint32_t fg, int scale) {
    if (scale < 1) scale = 1;
    KRect box = kr(x, y, FONT_W * scale, FONT_H * scale);
    if (kr_empty(kr_intersect(box, c->clip))) return FONT_W * scale;
    const unsigned char *g = font8x16_glyph(cp);
    for (int row = 0; row < FONT_H; row++) {
        unsigned bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x80u >> col))) continue;
            if (scale == 1) kc_pixel(c, x + col, y + row, fg);
            else kc_fill(c, kr(x + col * scale, y + row * scale, scale, scale), fg);
        }
    }
    return FONT_W * scale;
}

int kc_text_scaled(KCanvas *c, int x, int y, const char *s, uint32_t fg, int scale) {
    int x0 = x;
    uint32_t cp;
    int b;
    while ((b = kc_utf8_next(s, &cp)) > 0) {
        s += b;
        if (cp == '\n') continue;
        if (cp == '\t') cp = ' ';
        x += kc_glyph(c, x, y, cp, fg, scale);
    }
    return x - x0;
}

int kc_text(KCanvas *c, int x, int y, const char *s, uint32_t fg) { return kc_text_scaled(c, x, y, s, fg, 1); }

int kc_text_n(KCanvas *c, int x, int y, const char *s, size_t bytes, uint32_t fg) {
    int x0 = x;
    const char *end = s + bytes;
    uint32_t cp;
    int b;
    while (s < end && (b = kc_utf8_next(s, &cp)) > 0) {
        s += b;
        if (cp == '\t') cp = ' ';
        x += kc_glyph(c, x, y, cp, fg, 1);
    }
    return x - x0;
}

int kc_text_bold(KCanvas *c, int x, int y, const char *s, uint32_t fg) {
    kc_text(c, x + 1, y, s, fg);
    return kc_text(c, x, y, s, fg) + 1;
}

int kc_text_shadow(KCanvas *c, int x, int y, const char *s, uint32_t fg, uint32_t shadow) {
    kc_text(c, x + 1, y + 1, s, shadow);
    return kc_text(c, x, y, s, fg);
}

int kc_text_width(const char *s) { return (int)kc_utf8_len(s) * FONT_W; }

void kc_text_in(KCanvas *c, KRect r, const char *s, uint32_t fg, KAlign align) {
    int maxcells = r.w / FONT_W;
    if (maxcells <= 0) return;
    size_t n = kc_utf8_len(s);
    char buf[256];
    const char *show = s;
    if ((int)n > maxcells) {                                 /* too wide: cut and add "..." */
        int keep = maxcells > 3 ? maxcells - 3 : maxcells;
        size_t off = kc_utf8_offset(s, (size_t)keep);
        if (off > sizeof buf - 5) off = sizeof buf - 5;
        memcpy(buf, s, off);
        if (maxcells > 3) { memcpy(buf + off, "...", 3); off += 3; }
        buf[off] = 0;
        show = buf;
        n = (size_t)maxcells;
    }
    int w = (int)n * FONT_W;
    int x = align == KA_LEFT ? r.x : align == KA_RIGHT ? r.x + r.w - w : r.x + (r.w - w) / 2;
    int y = r.y + (r.h - FONT_H) / 2;
    KRect old = c->clip;
    kc_set_clip(c, kr_intersect(old, r));
    kc_text(c, x, y, show, fg);
    c->clip = old;
}
