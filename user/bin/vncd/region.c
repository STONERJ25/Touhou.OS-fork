/*
 * Rectangle sets for damage tracking: every operation sweeps the plane band by band (between consecutive y edges of either
 * operand), works out the covered x intervals of the band, combines them, and coalesces a band with the one above it when
 * their intervals are the same. The result is always in y-x banded form: non-overlapping rectangles sorted by y, then x, the
 * rectangles of one band sharing their y range. CopyRect needs that form to order its rectangles (client.c).
 *
 * The sets stay small (tens of rectangles: window damage from the compositor, a pointer, a clock), so the plain quadratic
 * sweep is the right size; region_simplify() keeps sets that only say "send this" from growing.
 */
#include "vncd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

VRect vrect_intersect(VRect a, VRect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return (VRect){ 0, 0, 0, 0 };
    return (VRect){ x0, y0, x1 - x0, y1 - y0 };
}

VRect vrect_union(VRect a, VRect b) {
    if (vrect_empty(a)) return b;
    if (vrect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (VRect){ x0, y0, x1 - x0, y1 - y0 };
}

static void *grow(void *p, size_t n, size_t size) {
    void *q = realloc(p, n * size);
    if (!q) {                                           /* tiny allocations; if even these fail the daemon cannot go on */
        vlog("out of memory (region)");
        exit(1);
    }
    return q;
}

void region_init(Region *g) { g->r = NULL; g->n = g->cap = 0; }
void region_free(Region *g) { free(g->r); region_init(g); }
void region_clear(Region *g) { g->n = 0; }
bool region_empty(const Region *g) { return g->n == 0; }

static void push(Region *g, VRect a) {
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->r = grow(g->r, (size_t)g->cap, sizeof *g->r);
    }
    g->r[g->n++] = a;
}

void region_set(Region *g, VRect a) {
    g->n = 0;
    if (!vrect_empty(a)) push(g, a);
}

void region_copy(Region *dst, const Region *src) {
    if (dst == src) return;
    dst->n = 0;
    for (int i = 0; i < src->n; i++) push(dst, src->r[i]);
}

/* ------------------------------------------------------------------------------------------------ the sweep */

typedef struct { int x0, x1; } Span;
enum { OP_UNION, OP_SUBTRACT, OP_INTERSECT };

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
static int cmp_span(const void *a, const void *b) { int x = ((const Span *)a)->x0, y = ((const Span *)b)->x0; return x < y ? -1 : x > y; }

/* The x intervals of `r` that cover the band [y0, y1): sorted, merged where they overlap or touch. Every y edge of every
 * rectangle is an edge of the sweep, so a rectangle either covers the whole band or none of it. */
static int band_spans(const VRect *r, int n, int y0, int y1, Span *out) {
    int k = 0;
    for (int i = 0; i < n; i++)
        if (r[i].w > 0 && r[i].h > 0 && r[i].y <= y0 && r[i].y + r[i].h >= y1) out[k++] = (Span){ r[i].x, r[i].x + r[i].w };
    if (k > 1) qsort(out, (size_t)k, sizeof *out, cmp_span);
    int m = 0;
    for (int i = 0; i < k; i++) {
        if (m && out[i].x0 <= out[m - 1].x1) { if (out[i].x1 > out[m - 1].x1) out[m - 1].x1 = out[i].x1; }
        else out[m++] = out[i];
    }
    return m;
}

static int combine(const Span *a, int na, const Span *b, int nb, int op, Span *out) {
    int m = 0;
    if (op == OP_UNION) {
        int i = 0, j = 0;
        while (i < na || j < nb) {
            Span s = (j >= nb || (i < na && a[i].x0 <= b[j].x0)) ? a[i++] : b[j++];
            if (m && s.x0 <= out[m - 1].x1) { if (s.x1 > out[m - 1].x1) out[m - 1].x1 = s.x1; }
            else out[m++] = s;
        }
    } else if (op == OP_INTERSECT) {
        int i = 0, j = 0;
        while (i < na && j < nb) {
            int x0 = a[i].x0 > b[j].x0 ? a[i].x0 : b[j].x0, x1 = a[i].x1 < b[j].x1 ? a[i].x1 : b[j].x1;
            if (x1 > x0) out[m++] = (Span){ x0, x1 };
            if (a[i].x1 < b[j].x1) i++; else j++;
        }
    } else {
        int j = 0;
        for (int i = 0; i < na; i++) {
            int x = a[i].x0;
            while (j < nb && b[j].x1 <= x) j++;
            int k = j;
            while (k < nb && b[k].x0 < a[i].x1) {
                if (b[k].x0 > x) out[m++] = (Span){ x, b[k].x0 };
                if (b[k].x1 > x) x = b[k].x1;
                k++;
            }
            if (x < a[i].x1) out[m++] = (Span){ x, a[i].x1 };
        }
    }
    return m;
}

static void region_op(Region *g, const VRect *b, int nb, int op) {
    const VRect *a = g->r;
    int na = g->n;
    if (op == OP_INTERSECT && (na == 0 || nb == 0)) { g->n = 0; return; }
    if (op == OP_SUBTRACT && (na == 0 || nb == 0)) return;
    int ne = 0;
    int *ys = grow(NULL, (size_t)(2 * (na + nb) + 1), sizeof *ys);
    for (int i = 0; i < na; i++) if (a[i].w > 0 && a[i].h > 0) { ys[ne++] = a[i].y; ys[ne++] = a[i].y + a[i].h; }
    for (int i = 0; i < nb; i++) if (b[i].w > 0 && b[i].h > 0) { ys[ne++] = b[i].y; ys[ne++] = b[i].y + b[i].h; }
    qsort(ys, (size_t)ne, sizeof *ys, cmp_int);
    int nu = 0;
    for (int i = 0; i < ne; i++) if (!nu || ys[nu - 1] != ys[i]) ys[nu++] = ys[i];
    Span *sa = grow(NULL, (size_t)na + 1, sizeof *sa);
    Span *sb = grow(NULL, (size_t)nb + 1, sizeof *sb);
    Span *so = grow(NULL, (size_t)(na + nb) * 2 + 2, sizeof *so);
    Span *prev = grow(NULL, (size_t)(na + nb) * 2 + 2, sizeof *prev);
    int nprev = 0, prev_start = 0, prev_y1 = 0;
    Region out;
    region_init(&out);
    for (int i = 0; i + 1 < nu; i++) {
        int y0 = ys[i], y1 = ys[i + 1];
        int ka = band_spans(a, na, y0, y1, sa);
        int kb = band_spans(b, nb, y0, y1, sb);
        int ko = combine(sa, ka, sb, kb, op, so);
        if (ko == 0) { nprev = 0; continue; }
        if (nprev == ko && prev_y1 == y0 && !memcmp(prev, so, (size_t)ko * sizeof *so)) {
            for (int k = 0; k < ko; k++) out.r[prev_start + k].h += y1 - y0;          /* the same intervals as the band above: grow it */
            prev_y1 = y1;
            continue;
        }
        prev_start = out.n;
        for (int k = 0; k < ko; k++) push(&out, (VRect){ so[k].x0, y0, so[k].x1 - so[k].x0, y1 - y0 });
        memcpy(prev, so, (size_t)ko * sizeof *so);
        nprev = ko;
        prev_y1 = y1;
    }
    free(ys); free(sa); free(sb); free(so); free(prev);
    free(g->r);
    *g = out;
}

void region_add_rect(Region *g, VRect a) {
    if (vrect_empty(a)) return;
    if (g->n == 0) { push(g, a); return; }
    region_op(g, &a, 1, OP_UNION);
}

void region_union(Region *g, const Region *a) {
    if (a->n == 0) return;
    if (g == a) return;
    region_op(g, a->r, a->n, OP_UNION);
}

void region_subtract(Region *g, const Region *a) {
    if (g->n == 0 || a->n == 0) return;
    if (g == a) { g->n = 0; return; }
    region_op(g, a->r, a->n, OP_SUBTRACT);
}

void region_subtract_rect(Region *g, VRect a) {
    if (g->n == 0 || vrect_empty(a)) return;
    region_op(g, &a, 1, OP_SUBTRACT);
}

void region_intersect(Region *g, const Region *a) {
    if (g == a) return;
    region_op(g, a->r, a->n, OP_INTERSECT);
}

void region_intersect_rect(Region *g, VRect a) {
    if (vrect_empty(a)) { g->n = 0; return; }
    region_op(g, &a, 1, OP_INTERSECT);
}

void region_translate(Region *g, int dx, int dy) {
    for (int i = 0; i < g->n; i++) { g->r[i].x += dx; g->r[i].y += dy; }
}

long region_area(const Region *g) {
    long s = 0;
    for (int i = 0; i < g->n; i++) s += (long)g->r[i].w * g->r[i].h;
    return s;
}

VRect region_bbox(const Region *g) {
    VRect b = { 0, 0, 0, 0 };
    for (int i = 0; i < g->n; i++) b = vrect_union(b, g->r[i]);
    return b;
}

bool region_contains_point(const Region *g, int x, int y) {
    for (int i = 0; i < g->n; i++)
        if (x >= g->r[i].x && y >= g->r[i].y && x < g->r[i].x + g->r[i].w && y < g->r[i].y + g->r[i].h) return true;
    return false;
}

/* A set with fewer rectangles that contains `g`: the two rectangles whose bounding box wastes least are merged until at most
 * max_rects are left (then the bands are rebuilt, which can split again; after a few rounds the bounding box of everything). */
void region_simplify(Region *g, int max_rects) {
    for (int round = 0; round < 4 && g->n > max_rects; round++) {
        while (g->n > max_rects) {
            int bi = 0, bj = 1;
            long best = -1;
            for (int i = 0; i < g->n; i++)
                for (int j = i + 1; j < g->n; j++) {
                    VRect u = vrect_union(g->r[i], g->r[j]);
                    long waste = (long)u.w * u.h - (long)g->r[i].w * g->r[i].h - (long)g->r[j].w * g->r[j].h;
                    if (best < 0 || waste < best) { best = waste; bi = i; bj = j; }
                }
            g->r[bi] = vrect_union(g->r[bi], g->r[bj]);
            g->r[bj] = g->r[--g->n];
        }
        Region t;
        region_init(&t);
        for (int i = 0; i < g->n; i++) region_add_rect(&t, g->r[i]);
        free(g->r);
        *g = t;
    }
    if (g->n > max_rects) region_set(g, region_bbox(g));
}
