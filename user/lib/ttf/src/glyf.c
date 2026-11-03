/*
 * TrueType outlines: the 'loca' index and the 'glyf' glyph descriptions (OpenType specification, "glyf"; Apple's TrueType
 * Reference Manual for the composite flags).
 *
 * A simple glyph is: contour end points, (skipped) hinting instructions, one flag byte per point (with a repeat count), then all
 * x deltas, then all y deltas, each either a byte with a sign flag, a signed 16-bit value, or "same as before". A composite glyph
 * is a list of other glyphs, each with a 2x2 transform and either an offset or a pair of points to bring together.
 *
 * Everything is collected as points first (TtfPts) so that a component can be placed by matching points; outline.c turns the points
 * into quadratic path operations at the end. Limits: TTF_MAX_POINTS points, TTF_MAX_COMPONENTS components and TTF_MAX_DEPTH levels per
 * glyph, so that a composite which includes itself, or a thousand glyphs that each include a thousand, ends with TTF_E_TOOBIG.
 */
#include "ttfint.h"

#define GF_ARG_WORDS        0x0001      /* the two arguments are 16 bits wide, else 8 */
#define GF_ARGS_XY          0x0002      /* the arguments are an offset, else a pair of point numbers */
#define GF_ROUND_XY         0x0004      /* (grid fitting: ignored) */
#define GF_SCALE            0x0008      /* one F2Dot14 scale follows */
#define GF_MORE             0x0020      /* another component follows */
#define GF_XY_SCALE         0x0040      /* two F2Dot14 scales follow */
#define GF_2X2              0x0080      /* four F2Dot14 matrix entries follow */
#define GF_INSTRUCTIONS     0x0100      /* instructions follow the last component (ignored: we stop reading there) */
#define GF_USE_MY_METRICS   0x0200
#define GF_OVERLAP          0x0400
#define GF_SCALED_OFFSET    0x0800      /* the offset is transformed by the component's matrix */
#define GF_UNSCALED_OFFSET  0x1000      /* the offset is not (the default) */

#define COORD_LIMIT (1 << 24)           /* hostile deltas can add up; keep every coordinate where float arithmetic stays exact */

typedef struct Ctx {
    const TtfFont *f;
    Span glyf, loca;
    TtfPts *pts;
    uint32_t components;
} Ctx;

/* The bytes of glyph `gid`; an empty span for a glyph without an outline (a space), which is not an error. */
static void glyph_data(const Ctx *c, uint32_t gid, Span *g) {
    uint32_t a, b;
    if (c->f->loca_long) { a = sp_u32(c->loca, 4u * gid); b = sp_u32(c->loca, 4u * gid + 4); }
    else { a = 2u * sp_u16(c->loca, 2u * gid); b = 2u * sp_u16(c->loca, 2u * gid + 2); }
    *g = sp_make(c->glyf.p, 0);
    if (b <= a || a >= c->glyf.n) return;
    if (b > c->glyf.n) b = (uint32_t)c->glyf.n;
    *g = sp_make(c->glyf.p + a, b - a);
}

static int32_t clampc(int32_t v) { return v > COORD_LIMIT ? COORD_LIMIT : v < -COORD_LIMIT ? -COORD_LIMIT : v; }

static int simple_glyph(Ctx *c, Span g, int ncont) {
    TtfPts *P = c->pts;
    size_t pos = 10;
    if (ncont == 0) return 0;                                       /* a header and nothing else */
    if (!sp_has(g, pos, (size_t)ncont * 2 + 2)) return TTF_E_GLYPH;
    uint32_t last = 0;
    for (int i = 0; i < ncont; i++) {
        uint32_t e = sp_u16(g, pos + 2u * (uint32_t)i);
        if (i > 0 && e <= last) return TTF_E_GLYPH;                 /* contours must be non-empty and in order */
        last = e;
    }
    uint32_t npts = ncont ? last + 1 : 0;
    pos += 2u * (uint32_t)ncont;
    pos += 2 + (size_t)sp_u16(g, pos);                              /* skip the instructions */
    if (pos > g.n) return TTF_E_GLYPH;
    uint32_t base = P->n;
    if (npts > TTF_MAX_POINTS || base + npts > TTF_MAX_POINTS) return TTF_E_TOOBIG;
    int rc = ttfi_pts_reserve(P, base + npts);
    if (rc) return rc;
    if (ttfi_reserve((void **)&P->ends, &P->cap_c, P->ncont + (uint32_t)ncont, sizeof(uint32_t))) return TTF_E_NOMEM;

    uint8_t *fl = P->on + base;                                     /* the raw flags live here until the end */
    for (uint32_t i = 0; i < npts;) {
        if (pos >= g.n) return TTF_E_GLYPH;
        uint8_t f = g.p[pos++];
        fl[i++] = f;
        if (f & 8) {
            if (pos >= g.n) return TTF_E_GLYPH;
            uint32_t rep = g.p[pos++];
            if (rep > npts - i) return TTF_E_GLYPH;
            while (rep--) fl[i++] = f;
        }
    }
    int32_t v = 0;
    for (uint32_t i = 0; i < npts; i++) {
        uint8_t f = fl[i];
        if (f & 2) {
            if (pos >= g.n) return TTF_E_GLYPH;
            int32_t d = g.p[pos++];
            v += (f & 16) ? d : -d;
        } else if (!(f & 16)) {
            if (!sp_has(g, pos, 2)) return TTF_E_GLYPH;
            v += sp_i16(g, pos);
            pos += 2;
        }
        v = clampc(v);
        P->x[base + i] = (float)v;
    }
    v = 0;
    for (uint32_t i = 0; i < npts; i++) {
        uint8_t f = fl[i];
        if (f & 4) {
            if (pos >= g.n) return TTF_E_GLYPH;
            int32_t d = g.p[pos++];
            v += (f & 32) ? d : -d;
        } else if (!(f & 32)) {
            if (!sp_has(g, pos, 2)) return TTF_E_GLYPH;
            v += sp_i16(g, pos);
            pos += 2;
        }
        v = clampc(v);
        P->y[base + i] = (float)v;
    }
    for (uint32_t i = 0; i < npts; i++) fl[i] &= 1;                 /* keep only ON_CURVE */
    for (int i = 0; i < ncont; i++) P->ends[P->ncont++] = base + sp_u16(g, 10 + 2u * (uint32_t)i);
    P->n = base + npts;
    return 0;
}

static int load_glyph(Ctx *c, uint32_t gid, int depth);

static int composite_glyph(Ctx *c, Span g, int depth) {
    TtfPts *P = c->pts;
    const uint32_t base = P->n;                                     /* point numbers in "match points" count from here */
    size_t pos = 10;
    uint32_t flags;
    do {
        if (!sp_has(g, pos, 4)) return TTF_E_GLYPH;
        flags = sp_u16(g, pos);
        uint32_t gid = sp_u16(g, pos + 2);
        pos += 4;
        int32_t a1, a2;
        if (flags & GF_ARG_WORDS) {
            if (!sp_has(g, pos, 4)) return TTF_E_GLYPH;
            if (flags & GF_ARGS_XY) { a1 = sp_i16(g, pos); a2 = sp_i16(g, pos + 2); }
            else { a1 = (int32_t)sp_u16(g, pos); a2 = (int32_t)sp_u16(g, pos + 2); }
            pos += 4;
        } else {
            if (!sp_has(g, pos, 2)) return TTF_E_GLYPH;
            if (flags & GF_ARGS_XY) { a1 = (int8_t)g.p[pos]; a2 = (int8_t)g.p[pos + 1]; }
            else { a1 = g.p[pos]; a2 = g.p[pos + 1]; }
            pos += 2;
        }
        /* x' = xx*x + xy*y ; y' = yx*x + yy*y. The file order of a full matrix is xscale, scale01, scale10, yscale = xx, yx, xy, yy. */
        float xx = 1, yx = 0, xy = 0, yy = 1;
        if (flags & GF_SCALE) {
            if (!sp_has(g, pos, 2)) return TTF_E_GLYPH;
            xx = yy = (float)sp_i16(g, pos) / 16384.0f;
            pos += 2;
        } else if (flags & GF_XY_SCALE) {
            if (!sp_has(g, pos, 4)) return TTF_E_GLYPH;
            xx = (float)sp_i16(g, pos) / 16384.0f;
            yy = (float)sp_i16(g, pos + 2) / 16384.0f;
            pos += 4;
        } else if (flags & GF_2X2) {
            if (!sp_has(g, pos, 8)) return TTF_E_GLYPH;
            xx = (float)sp_i16(g, pos) / 16384.0f;
            yx = (float)sp_i16(g, pos + 2) / 16384.0f;
            xy = (float)sp_i16(g, pos + 4) / 16384.0f;
            yy = (float)sp_i16(g, pos + 6) / 16384.0f;
            pos += 8;
        }
        if (++c->components > TTF_MAX_COMPONENTS) return TTF_E_TOOBIG;
        const uint32_t start = P->n;
        int rc = load_glyph(c, gid, depth + 1);
        if (rc) return rc;
        if (xx != 1.0f || yy != 1.0f || yx != 0.0f || xy != 0.0f) {
            for (uint32_t i = start; i < P->n; i++) {
                float x = P->x[i], y = P->y[i];
                P->x[i] = xx * x + xy * y;
                P->y[i] = yx * x + yy * y;
            }
        }
        float dx, dy;
        if (flags & GF_ARGS_XY) {
            dx = (float)a1;
            dy = (float)a2;
            if ((flags & GF_SCALED_OFFSET) && !(flags & GF_UNSCALED_OFFSET)) {
                float tx = xx * dx + xy * dy, ty = yx * dx + yy * dy;
                dx = tx;
                dy = ty;
            }
        } else {
            /* Bring point a2 of the component (after its transform) onto point a1 of what the composite has so far. */
            if ((uint32_t)a1 >= start - base || (uint32_t)a2 >= P->n - start) return TTF_E_GLYPH;
            dx = P->x[base + (uint32_t)a1] - P->x[start + (uint32_t)a2];
            dy = P->y[base + (uint32_t)a1] - P->y[start + (uint32_t)a2];
        }
        if (dx != 0.0f || dy != 0.0f) {
            for (uint32_t i = start; i < P->n; i++) { P->x[i] += dx; P->y[i] += dy; }
        }
    } while (flags & GF_MORE);
    return 0;
}

static int load_glyph(Ctx *c, uint32_t gid, int depth) {
    if (depth > TTF_MAX_DEPTH) return TTF_E_TOOBIG;
    if (gid >= (uint32_t)c->f->num_glyphs) return TTF_E_GLYPH;
    Span g;
    glyph_data(c, gid, &g);
    if (g.n == 0) return 0;
    if (g.n < 10) return TTF_E_GLYPH;
    int ncont = sp_i16(g, 0);
    return ncont >= 0 ? simple_glyph(c, g, ncont) : composite_glyph(c, g, depth);
}

int ttfi_glyf_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph) {
    Ctx c;
    c.f = f;
    c.glyf = sp_table(f, f->glyf);
    c.loca = sp_table(f, f->loca);
    c.pts = &r->pts;
    c.components = 0;
    ttfi_pts_reset(&r->pts);
    int rc = load_glyph(&c, glyph, 0);
    if (rc) return rc;
    return ttfi_pts_to_path(&r->pts, &r->outline);
}
