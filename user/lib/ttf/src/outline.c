/*
 * Outline and scratch-memory management: the growable buffers behind a TtfRaster, the path builders every glyph decoder writes
 * through, and the one conversion from TrueType's on/off-curve points to path operations.
 *
 * Quadratic splines (TrueType): between two consecutive off-curve points lies an implied on-curve point at their midpoint, and a
 * contour may start at an off-curve point. A contour is started at its first point if that is on the curve, else at its last
 * point if that is, else at the midpoint of the two; the points are then walked once around and the contour is closed.
 */
#include "ttfint.h"

int ttfi_reserve(void **buf, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return 0;
    uint32_t nc = *cap ? *cap : 64;
    while (nc < need) {
        if (nc > 0x40000000u) { nc = need; break; }
        nc *= 2;
    }
    if (elem == 0 || (size_t)nc > (size_t)-1 / elem) return -1;
    void *np = TTF_REALLOC(*buf, (size_t)nc * elem);
    if (!np) return -1;
    *buf = np;
    *cap = nc;
    return 0;
}

int ttfi_reserve_sz(void **buf, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap : 1024;
    while (nc < need) {
        if (nc > ((size_t)-1) / 4) { nc = need; break; }
        nc *= 2;
    }
    if (elem == 0 || nc > (size_t)-1 / elem) return -1;
    void *np = TTF_REALLOC(*buf, nc * elem);
    if (!np) return -1;
    *buf = np;
    *cap = nc;
    return 0;
}

/* ---------------------------------------------------------------- the rasteriser's scratch context */

TtfRaster *ttf_raster_new(void) {
    TtfRaster *r = (TtfRaster *)TTF_MALLOC(sizeof *r);
    if (r) memset(r, 0, sizeof *r);
    return r;
}

void ttf_raster_free(TtfRaster *r) {
    if (!r) return;
    TTF_FREE(r->outline.ops);
    TTF_FREE(r->outline.pts);
    TTF_FREE(r->pts.x);
    TTF_FREE(r->pts.y);
    TTF_FREE(r->pts.on);
    TTF_FREE(r->pts.ends);
    TTF_FREE(r->acc);
    TTF_FREE(r->gray);
    TTF_FREE(r->bmp);
    TTF_FREE(r);
}

/* ---------------------------------------------------------------- path building */

void ttfi_outline_reset(TtfOutline *o) {
    o->nops = o->npts = 0;
    o->xmin = o->ymin = o->xmax = o->ymax = 0;
}

static int path_push(TtfOutline *o, int op, int npoints) {
    if (o->nops >= TTF_MAX_OPS) return TTF_E_TOOBIG;
    if (ttfi_reserve((void **)&o->ops, &o->cap_ops, o->nops + 1, 1)) return TTF_E_NOMEM;
    if (ttfi_reserve((void **)&o->pts, &o->cap_pts, o->npts + 2u * (uint32_t)npoints, sizeof(float))) return TTF_E_NOMEM;
    o->ops[o->nops++] = (uint8_t)op;
    return 0;
}

static void path_point(TtfOutline *o, float x, float y) {
    if (o->npts == 0) { o->xmin = o->xmax = x; o->ymin = o->ymax = y; }
    else {
        if (x < o->xmin) o->xmin = x;
        if (x > o->xmax) o->xmax = x;
        if (y < o->ymin) o->ymin = y;
        if (y > o->ymax) o->ymax = y;
    }
    o->pts[o->npts++] = x;
    o->pts[o->npts++] = y;
}

int ttfi_path_move(TtfOutline *o, float x, float y) {
    int rc = path_push(o, TTF_OP_MOVE, 1);
    if (rc) return rc;
    path_point(o, x, y);
    return 0;
}

int ttfi_path_line(TtfOutline *o, float x, float y) {
    int rc = path_push(o, TTF_OP_LINE, 1);
    if (rc) return rc;
    path_point(o, x, y);
    return 0;
}

int ttfi_path_quad(TtfOutline *o, float cx, float cy, float x, float y) {
    int rc = path_push(o, TTF_OP_QUAD, 2);
    if (rc) return rc;
    path_point(o, cx, cy);
    path_point(o, x, y);
    return 0;
}

int ttfi_path_cubic(TtfOutline *o, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    int rc = path_push(o, TTF_OP_CUBIC, 3);
    if (rc) return rc;
    path_point(o, c1x, c1y);
    path_point(o, c2x, c2y);
    path_point(o, x, y);
    return 0;
}

int ttfi_path_close(TtfOutline *o) { return path_push(o, TTF_OP_CLOSE, 0); }

/* ---------------------------------------------------------------- TrueType points */

void ttfi_pts_reset(TtfPts *p) { p->n = p->ncont = 0; }

/* x, y and on share one capacity. If a later array fails to grow, cap_n keeps the old value and the arrays that did grow are just
 * larger than they need to be. */
int ttfi_pts_reserve(TtfPts *p, uint32_t total) {
    if (total > TTF_MAX_POINTS) return TTF_E_TOOBIG;
    if (total <= p->cap_n) return 0;
    uint32_t nc = p->cap_n ? p->cap_n : 256;
    while (nc < total) nc *= 2;
    float *nx = (float *)TTF_REALLOC(p->x, (size_t)nc * sizeof(float));
    if (!nx) return TTF_E_NOMEM;
    p->x = nx;
    float *ny = (float *)TTF_REALLOC(p->y, (size_t)nc * sizeof(float));
    if (!ny) return TTF_E_NOMEM;
    p->y = ny;
    uint8_t *non = (uint8_t *)TTF_REALLOC(p->on, nc);
    if (!non) return TTF_E_NOMEM;
    p->on = non;
    p->cap_n = nc;
    return 0;
}

int ttfi_pts_add(TtfPts *p, float x, float y, int on) {
    int rc = ttfi_pts_reserve(p, p->n + 1);
    if (rc) return rc;
    p->x[p->n] = x;
    p->y[p->n] = y;
    p->on[p->n] = (uint8_t)(on != 0);
    p->n++;
    return 0;
}

int ttfi_pts_end_contour(TtfPts *p) {
    if (p->n == 0) return 0;
    if (p->ncont > 0 && p->ends[p->ncont - 1] + 1 >= p->n) return 0;       /* nothing added since the last end */
    if (ttfi_reserve((void **)&p->ends, &p->cap_c, p->ncont + 1, sizeof(uint32_t))) return TTF_E_NOMEM;
    p->ends[p->ncont++] = p->n - 1;
    return 0;
}

int ttfi_pts_to_path(const TtfPts *p, TtfOutline *o) {
    uint32_t first = 0;
    for (uint32_t c = 0; c < p->ncont; c++) {
        uint32_t last = p->ends[c];
        if (last >= p->n || last < first) return TTF_E_GLYPH;
        uint32_t n = last - first + 1;
        const float *x = p->x + first, *y = p->y + first;
        const uint8_t *on = p->on + first;
        first = last + 1;
        if (n < 2) continue;                                    /* a lone point encloses nothing */
        uint32_t from, to;                                      /* the points to walk, [from, to) */
        float sx, sy;
        if (on[0]) { sx = x[0]; sy = y[0]; from = 1; to = n; }
        else if (on[n - 1]) { sx = x[n - 1]; sy = y[n - 1]; from = 0; to = n - 1; }
        else { sx = (x[0] + x[n - 1]) * 0.5f; sy = (y[0] + y[n - 1]) * 0.5f; from = 0; to = n; }
        int rc = ttfi_path_move(o, sx, sy);
        if (rc) return rc;
        int have = 0;                                           /* a pending off-curve control point */
        float cx = 0, cy = 0;
        for (uint32_t i = from; i < to; i++) {
            if (on[i]) {
                rc = have ? ttfi_path_quad(o, cx, cy, x[i], y[i]) : ttfi_path_line(o, x[i], y[i]);
                have = 0;
            } else if (have) {
                rc = ttfi_path_quad(o, cx, cy, (cx + x[i]) * 0.5f, (cy + y[i]) * 0.5f);
                cx = x[i]; cy = y[i];
            } else {
                cx = x[i]; cy = y[i]; have = 1;
                rc = 0;
            }
            if (rc) return rc;
        }
        if (have) { rc = ttfi_path_quad(o, cx, cy, sx, sy); if (rc) return rc; }
        rc = ttfi_path_close(o);
        if (rc) return rc;
    }
    return 0;
}

/* ---------------------------------------------------------------- the public entry */

int ttf_glyph_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph, const TtfOutline **out) {
    if (!r || !f || !out) return TTF_E_ARG;
    *out = NULL;
    ttfi_outline_reset(&r->outline);
    if (glyph >= (uint32_t)f->num_glyphs) return TTF_E_GLYPH;
    int rc = f->is_cff ? ttfi_cff_outline(r, f, glyph) : ttfi_glyf_outline(r, f, glyph);
    if (rc) {
        ttfi_outline_reset(&r->outline);                       /* never hand out half a glyph */
        return rc;
    }
    *out = &r->outline;
    return TTF_OK;
}
