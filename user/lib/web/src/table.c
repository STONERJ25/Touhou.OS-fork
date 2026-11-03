/*
 * Tables (CSS 2.1 chapter 17): the table grid from row groups (header groups first, footer groups last), rows and cells
 * with colspan/rowspan, the column widths of the automatic layout algorithm (17.5.2.2: minimum and maximum widths of
 * cells and columns, spanning cells spread over their columns, the table width between the sum of the minimums and the
 * available width, the extra spread over the columns in proportion to their maximums), the fixed layout algorithm
 * (17.5.2.1: <col> widths and the first row), percentages of cells and columns against the table width, row heights
 * with rowspans and the baseline of baseline-aligned cells, vertical-align in cells, border-spacing in the separated
 * borders model, captions above or below, and the collapsing borders model approximated: every grid line has one width
 * (the widest border meeting it), cells and the table draw their borders on those lines, overlapping.
 */
#include "layout.h"

#include <stdlib.h>

typedef struct TCell { LBox *b; int row, col, rs, cs; WebUnit mn, mx; float pct; } TCell;
typedef struct TCol { WebUnit mn, mx, w; bool fixed; float pct; } TCol;
typedef struct TRow { LBox *b, *group; WebUnit h, y, asc, spec; } TRow;

typedef struct TGrid {
    L *l;
    LBox *t;
    VEC(TCell) cells;
    VEC(TRow) rows;
    TCol *cols;
    int ncols;
    bool collapse;
    WebUnit hs, vs;                /* border-spacing */
    WebUnit *vl, *hl;              /* collapsing model: vertical line widths [ncols + 1], horizontal [nrows + 1] */
    WebUnit ex;                    /* the table's border and padding, left plus right (separate model) */
} TGrid;

static int attr_int(const LBox *c, WebAtom a, int dflt, int lo, int hi) {
    if (!c->node || !is_el(c->node) || (c->flags & BF_ANON)) return dflt;
    const char *v = dom_attr_val(c->node, a);
    if (!v) return dflt;
    while (*v == ' ' || *v == '\t' || *v == '\n') v++;
    if (*v < '0' || *v > '9') return dflt;
    long n = strtol(v, NULL, 10);
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (int)n;
}

static void add_rows_of(TGrid *G, LBox *grp, int *taken, int *ntaken) {
    L *l = G->l;
    int first_row = (int)G->rows.n;
    for (LBox *r = grp->first; r; r = r->next) {
        if (r->kind != BK_ROW) continue;
        TRow tr;
        memset(&tr, 0, sizeof tr);
        tr.b = r;
        tr.group = grp;
        int ri = (int)G->rows.n;
        if (!vec_push(l->a, G->rows, tr)) return;
        int col = 0;
        for (LBox *c = r->first; c; c = c->next) {
            if (c->kind != BK_CELL) continue;
            while (col < *ntaken && taken[col] > ri) col++;
            TCell tc;
            memset(&tc, 0, sizeof tc);
            tc.b = c;
            tc.row = ri;
            tc.col = col;
            tc.cs = attr_int(c, AT_colspan, 1, 1, 1000);
            tc.rs = attr_int(c, AT_rowspan, 1, 0, 65534);
            if (tc.rs == 0) tc.rs = 1 << 20;                  /* to the end of the row group */
            if (col + tc.cs > 1000) tc.cs = 1000 - col;
            if (tc.cs < 1) break;
            for (int k = 0; k < tc.cs; k++) {
                if (col + k >= *ntaken) { taken[col + k] = 0; *ntaken = col + k + 1; }
                taken[col + k] = ri + (tc.rs > 1 ? tc.rs : 1);
            }
            vec_push(l->a, G->cells, tc);
            col += tc.cs;
        }
    }
    /* rowspans end with their group */
    int last = (int)G->rows.n;
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        if (c->row >= first_row && c->row + c->rs > last) c->rs = last - c->row;
    }
    for (int k = 0; k < *ntaken; k++) if (taken[k] > last) taken[k] = last;
}

static bool build(TGrid *G, L *l, LBox *t) {
    memset(G, 0, sizeof *G);
    G->l = l;
    G->t = t;
    G->collapse = t->s->inh->border_collapse != 0;
    G->hs = G->collapse ? 0 : to_unit(t->s->inh->bs_h);
    G->vs = G->collapse ? 0 : to_unit(t->s->inh->bs_v);
    int taken[1000];
    int ntaken = 0;
    memset(taken, 0, sizeof taken);
    for (int pass = 0; pass < 3; pass++)                    /* header groups, bodies, footer groups */
        for (LBox *g = t->first; g; g = g->next) {
            if (g->kind != BK_ROW_GROUP) continue;
            int d = g->s->display;
            int want = d == D_TABLE_HEADER_GROUP ? 0 : d == D_TABLE_FOOTER_GROUP ? 2 : 1;
            if (want != pass) continue;
            add_rows_of(G, g, taken, &ntaken);
        }
    int nc = 0;
    for (uint32_t i = 0; i < G->cells.n; i++) if (G->cells.v[i].col + G->cells.v[i].cs > nc) nc = G->cells.v[i].col + G->cells.v[i].cs;
    /* columns declared by <col>/<colgroup> count too */
    WebAtom span = atom_get(&l->d->atoms, "span", 4);
    int ncol_el = 0;
    for (LBox *g = t->first; g; g = g->next) {
        if (g->kind == BK_COL) ncol_el += attr_int(g, span, 1, 1, 1000);
        else if (g->kind == BK_COLGROUP) {
            int inner = 0;
            for (LBox *c = g->first; c; c = c->next) if (c->kind == BK_COL) inner += attr_int(c, span, 1, 1, 1000);
            ncol_el += inner ? inner : attr_int(g, span, 1, 1, 1000);
        }
    }
    if (ncol_el > nc) nc = ncol_el > 1000 ? 1000 : ncol_el;
    G->ncols = nc;
    G->cols = wa_alloc(l->a, sizeof(TCol) * (size_t)(nc ? nc : 1));
    G->vl = wa_alloc(l->a, sizeof(WebUnit) * (size_t)(nc + 1));
    G->hl = wa_alloc(l->a, sizeof(WebUnit) * (size_t)(G->rows.n + 1));
    if (!G->cols || !G->vl || !G->hl) return false;
    /* <col> widths */
    int ci = 0;
    for (LBox *g = t->first; g && ci < nc; g = g->next) {
        if (g->kind != BK_COL && g->kind != BK_COLGROUP) continue;
        LBox *list[2] = { g, NULL };
        bool has_cols = false;
        if (g->kind == BK_COLGROUP) for (LBox *c = g->first; c; c = c->next) if (c->kind == BK_COL) has_cols = true;
        for (LBox *c = has_cols ? g->first : list[0]; c && ci < nc; c = has_cols ? c->next : NULL) {
            if (has_cols && c->kind != BK_COL) continue;
            int n = attr_int(c, span, 1, 1, 1000);
            const CLen *w = &c->s->box->width;
            for (int k = 0; k < n && ci < nc; k++, ci++) {
                if (w->kind == CL_PX && w->pct == 0 && w->px > 0) {
                    G->cols[ci].mn = G->cols[ci].mx = to_unit(w->px);
                    G->cols[ci].fixed = true;
                } else if (w->kind == CL_PX && w->pct > 0 && w->px == 0) G->cols[ci].pct = w->pct;
            }
            if (!has_cols) break;
        }
    }
    /* the collapsing model: one width per grid line */
    if (G->collapse) {
        int nr = (int)G->rows.n;
        for (int k = 0; k <= nc; k++) G->vl[k] = 0;
        for (int k = 0; k <= nr; k++) G->hl[k] = 0;
        layout_compute_box_edges(l, t, 0);
        G->vl[0] = t->b[3];
        G->vl[nc] = u_max(G->vl[nc], t->b[1]);
        G->hl[0] = t->b[0];
        G->hl[nr] = u_max(G->hl[nr], t->b[2]);
        for (uint32_t i = 0; i < G->cells.n; i++) {
            TCell *c = &G->cells.v[i];
            layout_compute_box_edges(l, c->b, 0);
            G->vl[c->col] = u_max(G->vl[c->col], c->b->b[3]);
            G->vl[c->col + c->cs] = u_max(G->vl[c->col + c->cs], c->b->b[1]);
            G->hl[c->row] = u_max(G->hl[c->row], c->b->b[0]);
            int re = c->row + c->rs > nr ? nr : c->row + c->rs;
            G->hl[re] = u_max(G->hl[re], c->b->b[2]);
        }
    }
    return true;
}

/* the cell's border and padding, left plus right, as the column widths count them */
static WebUnit cell_ex(const TGrid *G, const TCell *c) {
    if (G->collapse) return c->b->p[1] + c->b->p[3];
    return c->b->p[1] + c->b->p[3] + c->b->b[1] + c->b->b[3];
}

/* the space between columns s and e - 1 that a spanning cell also covers */
static WebUnit inner_spacing(const TGrid *G, int s, int e) {
    WebUnit v = 0;
    for (int k = s + 1; k < e; k++) v += G->collapse ? G->vl[k] : G->hs;
    return v;
}

static WebUnit spacing_total(const TGrid *G) {
    WebUnit v = 0;
    if (G->collapse) for (int k = 0; k <= G->ncols; k++) v += G->vl[k];
    else v = G->hs * (G->ncols + 1);
    return v;
}

static void cell_widths(TGrid *G) {
    L *l = G->l;
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        LBox *b = c->b;
        layout_compute_box_edges(l, b, 0);
        if (G->collapse) {
            b->b[3] = G->vl[c->col];
            b->b[1] = G->vl[c->col + c->cs];
            b->b[0] = G->hl[c->row];
            int nr = (int)G->rows.n;
            b->b[2] = G->hl[c->row + c->rs > nr ? nr : c->row + c->rs];
            b->flags |= BF_KEEP_EDGES;
        }
        WebUnit mn, mx;
        intrinsic_widths(l, b, &mn, &mx);
        WebUnit ex = cell_ex(G, c);
        mn += ex;
        mx += ex;
        const CLen *w = &b->s->box->width;
        if (w->kind == CL_PX && w->pct == 0 && w->px > 0) {
            WebUnit sw = to_unit(w->px);
            if (b->s->box->box_sizing) sw -= G->collapse ? b->b[1] + b->b[3] : 0;
            else sw += ex;
            if (sw > mn) mn = sw;
            mx = mn;
        } else if (w->kind == CL_PX && w->pct > 0) c->pct = w->pct;
        if (mx < mn) mx = mn;
        c->mn = mn;
        c->mx = mx;
    }
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        if (c->cs != 1) continue;
        TCol *k = &G->cols[c->col];
        if (c->mn > k->mn) k->mn = c->mn;
        if (c->mx > k->mx) k->mx = c->mx;
        if (c->pct > k->pct) k->pct = c->pct;
        if (k->fixed && k->mx < k->mn) k->mx = k->mn;
    }
    /* spanning cells: their excess over the spanned columns, spread in proportion to the columns' maximums */
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < G->cells.n; i++) {
            TCell *c = &G->cells.v[i];
            if (c->cs < 2) continue;
            WebUnit want = pass ? c->mx : c->mn;
            WebUnit have = inner_spacing(G, c->col, c->col + c->cs), wsum = 0;
            for (int k = c->col; k < c->col + c->cs; k++) { have += pass ? G->cols[k].mx : G->cols[k].mn; wsum += G->cols[k].mx; }
            if (want <= have) continue;
            WebUnit extra = want - have, given = 0;
            for (int k = c->col; k < c->col + c->cs; k++) {
                WebUnit add = wsum > 0 ? (WebUnit)((int64_t)extra * G->cols[k].mx / wsum) : extra / c->cs;
                if (k == c->col + c->cs - 1) add = extra - given;
                given += add;
                if (pass) G->cols[k].mx += add;
                else { G->cols[k].mn += add; if (G->cols[k].mx < G->cols[k].mn) G->cols[k].mx = G->cols[k].mn; }
            }
        }
}

/* the table's border-box width for the available width, and the column widths */
static WebUnit column_widths(TGrid *G, WebUnit avail, WebUnit spec, bool has_spec) {
    WebUnit smin = 0, smax = 0;
    int nc = G->ncols;
    for (int k = 0; k < nc; k++) { smin += G->cols[k].mn; smax += G->cols[k].mx; }
    WebUnit fixed_part = spacing_total(G) + G->ex;
    WebUnit W;
    if (has_spec) W = u_max(spec, smin + fixed_part);
    else W = u_max(smin + fixed_part, u_min(avail, smax + fixed_part));
    WebUnit room = W - fixed_part;
    /* percentages of the columns' share of the table */
    WebUnit pct_used = 0;
    for (int k = 0; k < nc; k++)
        if (G->cols[k].pct > 0 && !G->cols[k].fixed) {
            WebUnit v = (WebUnit)((double)room * G->cols[k].pct / 100);
            if (v < G->cols[k].mn) v = G->cols[k].mn;
            G->cols[k].w = v;
            pct_used += v;
        }
    WebUnit rmin = 0, rmax = 0, amax = 0;
    for (int k = 0; k < nc; k++) {
        if (G->cols[k].pct > 0 && !G->cols[k].fixed) continue;
        rmin += G->cols[k].mn;
        rmax += G->cols[k].mx;
        if (!G->cols[k].fixed) amax += G->cols[k].mx;
    }
    WebUnit r = room - pct_used;
    for (int k = 0; k < nc; k++) {
        TCol *c = &G->cols[k];
        if (c->pct > 0 && !c->fixed) continue;
        if (r >= rmax) c->w = c->mx;
        else if (r > rmin && rmax > rmin) c->w = c->mn + (WebUnit)((int64_t)(c->mx - c->mn) * (r - rmin) / (rmax - rmin));
        else c->w = c->mn;
    }
    if (r > rmax) {
        /* the extra width: to the auto columns in proportion to their maximums, else to all of them */
        WebUnit extra = r - rmax, given = 0;
        int nauto = 0;
        for (int k = 0; k < nc; k++) if (!G->cols[k].fixed && !(G->cols[k].pct > 0)) nauto++;
        int last = -1;
        for (int k = 0; k < nc; k++) {
            TCol *c = &G->cols[k];
            if (c->pct > 0 && !c->fixed) continue;
            if (nauto && c->fixed) continue;
            WebUnit add;
            if (nauto) add = amax > 0 ? (WebUnit)((int64_t)extra * c->mx / amax) : extra / nauto;
            else add = rmax > 0 ? (WebUnit)((int64_t)extra * c->mx / rmax) : extra / (nc ? nc : 1);
            c->w += add;
            given += add;
            last = k;
        }
        if (last >= 0) G->cols[last].w += extra - given;
    }
    WebUnit sum = fixed_part;
    for (int k = 0; k < nc; k++) sum += G->cols[k].w;
    return u_max(W, sum);
}

/* 17.5.2.1: the fixed table layout */
static WebUnit fixed_widths(TGrid *G, WebUnit W) {
    int nc = G->ncols;
    WebUnit fixed_part = spacing_total(G) + G->ex;
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        if (c->row != 0) continue;
        const CLen *w = &c->b->s->box->width;
        if (w->kind == CL_PX && w->pct == 0 && w->px > 0) {
            layout_compute_box_edges(G->l, c->b, 0);
            WebUnit v = to_unit(w->px) + (c->b->s->box->box_sizing ? 0 : cell_ex(G, c));
            for (int k = c->col; k < c->col + c->cs; k++)
                if (!G->cols[k].fixed) { G->cols[k].mn = G->cols[k].mx = v / c->cs; G->cols[k].fixed = true; }
        }
    }
    WebUnit used = 0;
    int nfree = 0;
    for (int k = 0; k < nc; k++) {
        if (G->cols[k].fixed) { G->cols[k].w = G->cols[k].mn; used += G->cols[k].w; }
        else if (G->cols[k].pct > 0) { G->cols[k].w = (WebUnit)((double)(W - fixed_part) * G->cols[k].pct / 100); used += G->cols[k].w; }
        else nfree++;
    }
    WebUnit rest = W - fixed_part - used;
    for (int k = 0; k < nc; k++)
        if (!G->cols[k].fixed && !(G->cols[k].pct > 0)) G->cols[k].w = nfree && rest > 0 ? rest / nfree : 0;
    WebUnit sum = fixed_part;
    for (int k = 0; k < nc; k++) sum += G->cols[k].w;
    return u_max(W, sum);
}

static void lay(L *l, LBox *c, WebUnit cbw, WebUnit fw, WebUnit fh) {
    Space s;
    memset(&s, 0, sizeof s);
    s.cb_w = cbw;
    s.cb_h = -1;
    s.force_w = fw;
    s.force_h = fh;
    BFC own;
    memset(&own, 0, sizeof own);
    own.a = l->a;
    s.bfc = &own;
    layout_box(l, c, &s);
}

static void shift_refs(LBox *cell, LBox *p, WebUnit dy, int depth) {
    for (LBox *c = p->first; c; c = c->next) {
        if (c->ref == cell) c->y += dy;
        else if ((c->kind == BK_INLINE || c->kind == BK_TEXT) && depth < 100) shift_refs(cell, c, dy, depth + 1);
    }
}

/* vertical-align in a cell moves its content down */
static void shift_content(LBox *cell, WebUnit dy) {
    if (!dy) return;
    for (uint32_t i = 0; i < cell->nlines; i++) { cell->lines[i].y += dy; cell->lines[i].baseline += dy; }
    for (uint32_t i = 0; i < cell->nfrags; i++) { cell->frags[i].y += dy; cell->frags[i].baseline += dy; }
    shift_refs(cell, cell, dy, 0);
    cell->baseline += dy;
}

/* the height a cell asks of its rows: in the collapsing model the grid lines are not part of the rows */
static WebUnit cell_h(const TGrid *G, const TCell *c) {
    return G->collapse ? c->b->h - c->b->b[0] - c->b->b[2] : c->b->h;
}

static WebUnit cell_asc(const TGrid *G, const TCell *c) {
    LBox *b = c->b;
    WebUnit a = b->has_baseline ? b->baseline : b->h - b->b[2] - b->p[2];
    return G->collapse ? a - b->b[0] : a;
}

static WebUnit col_x(const TGrid *G, int k) {
    WebUnit x = G->collapse ? 0 : G->t->b[3] + G->t->p[3] + G->hs;
    for (int i = 0; i < k; i++) x += G->collapse ? G->vl[i] + G->cols[i].w : G->cols[i].w + G->hs;
    return x;
}

static void layout_table(TGrid *G, LBox *t, WebUnit W) {
    L *l = G->l;
    int nr = (int)G->rows.n, nc = G->ncols;
    t->w = W;
    WebUnit inner = W - spacing_total(G) - G->ex, sumw = 0;
    for (int k = 0; k < nc; k++) sumw += G->cols[k].w;
    if (nc && inner > sumw) G->cols[nc - 1].w += inner - sumw;
    /* cells at their widths */
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        WebUnit w = inner_spacing(G, c->col, c->col + c->cs);
        for (int k = c->col; k < c->col + c->cs && k < nc; k++) w += G->cols[k].w;
        if (G->collapse) w += c->b->b[1] + c->b->b[3];
        c->b->ref = t;
        lay(l, c->b, W, w, -1);
        WebUnit sh;
        if (box_spec_size(l, c->b, &c->b->s->box->height, -1, false, &sh) && sh > c->b->h) c->b->h = sh;
    }
    /* row heights: specified heights, single-row cells, baselines, then the rowspans */
    for (int r = 0; r < nr; r++) {
        TRow *row = &G->rows.v[r];
        WebUnit sh;
        row->h = 0;
        if (box_spec_size(l, row->b, &row->b->s->box->height, -1, false, &sh)) row->h = sh;
        row->asc = 0;
    }
    WebUnit *desc = wa_alloc(l->a, sizeof(WebUnit) * (size_t)(nr ? nr : 1));
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        if (c->rs != 1) continue;
        TRow *row = &G->rows.v[c->row];
        WebUnit ch = cell_h(G, c);
        if (ch > row->h) row->h = ch;
        if (c->b->s->box->va_kind == VA_BASELINE) {
            WebUnit a = cell_asc(G, c);
            if (a > row->asc) row->asc = a;
            if (desc && ch - a > desc[c->row]) desc[c->row] = ch - a;
        }
    }
    for (int r = 0; r < nr; r++) if (desc && G->rows.v[r].asc + desc[r] > G->rows.v[r].h) G->rows.v[r].h = G->rows.v[r].asc + desc[r];
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        if (c->rs < 2) continue;
        int re = c->row + c->rs > nr ? nr : c->row + c->rs;
        WebUnit have = 0;
        for (int r = c->row; r < re; r++) have += G->rows.v[r].h + (r > c->row ? (G->collapse ? G->hl[r] : G->vs) : 0);
        if (cell_h(G, c) > have && re > c->row) G->rows.v[re - 1].h += cell_h(G, c) - have;
    }
    /* the table's height: rows, spacing, edges; a taller specified height goes to the rows */
    WebUnit eh = G->collapse ? 0 : t->b[0] + t->b[2] + t->p[0] + t->p[2];
    WebUnit sum = eh;
    if (G->collapse) for (int r = 0; r <= nr; r++) sum += G->hl[r];
    else sum += G->vs * (nr + 1);
    for (int r = 0; r < nr; r++) sum += G->rows.v[r].h;
    WebUnit spec;
    if (box_spec_size(l, t, &t->s->box->height, -1, false, &spec) && spec > sum && nr) {
        WebUnit extra = spec - sum;
        for (int r = 0; r < nr; r++) G->rows.v[r].h += extra / nr + (r == nr - 1 ? extra % nr : 0);
        sum = spec;
    }
    t->h = sum;
    WebUnit y = G->collapse ? 0 : t->b[0] + t->p[0] + G->vs;
    for (int r = 0; r < nr; r++) {
        if (G->collapse) y += G->hl[r];
        G->rows.v[r].y = y;
        y += G->rows.v[r].h + (G->collapse ? 0 : G->vs);
    }
    /* cells: their final heights and positions, vertical-align */
    for (uint32_t i = 0; i < G->cells.n; i++) {
        TCell *c = &G->cells.v[i];
        LBox *b = c->b;
        int re = c->row + c->rs > nr ? nr : c->row + c->rs;
        WebUnit top = G->rows.v[c->row].y, bot = G->rows.v[re - 1].y + G->rows.v[re - 1].h;
        b->x = col_x(G, c->col) + (G->collapse ? 0 : 0);
        b->y = top - (G->collapse ? b->b[0] : 0);
        WebUnit h = bot - top + (G->collapse ? b->b[0] + b->b[2] : 0);
        WebUnit content_bottom = b->h;
        WebUnit a = cell_asc(G, c);
        b->h = h;
        int va = b->s->box->va_kind;
        WebUnit dy = 0;
        if (va == VA_MIDDLE) dy = (h - content_bottom) / 2;
        else if (va == VA_BOTTOM) dy = h - content_bottom;
        else if (va == VA_BASELINE && c->rs == 1) dy = G->rows.v[c->row].asc - a;
        if (dy > 0) shift_content(b, dy);
    }
    /* rows and row groups cover their cells (their backgrounds) */
    WebUnit gx = col_x(G, 0), gw = nc ? col_x(G, nc - 1) + G->cols[nc - 1].w - gx : 0;
    for (int r = 0; r < nr; r++) {
        LBox *rb = G->rows.v[r].b;
        rb->ref = t;
        rb->x = gx;
        rb->y = G->rows.v[r].y;
        rb->w = gw;
        rb->h = G->rows.v[r].h;
    }
    for (LBox *g = t->first; g; g = g->next) {
        if (g->kind != BK_ROW_GROUP) continue;
        g->ref = t;
        WebUnit y0 = INT32_MAX, y1 = INT32_MIN;
        for (LBox *rb = g->first; rb; rb = rb->next) {
            if (rb->kind != BK_ROW) continue;
            if (rb->y < y0) y0 = rb->y;
            if (rb->y + rb->h > y1) y1 = rb->y + rb->h;
        }
        g->x = gx;
        g->w = gw;
        g->y = y0 == INT32_MAX ? 0 : y0;
        g->h = y0 == INT32_MAX ? 0 : y1 - y0;
    }
    t->has_baseline = false;
    if (nr) {
        WebUnit a = G->rows.v[0].asc;
        if (!a) for (uint32_t i = 0; i < G->cells.n; i++) if (G->cells.v[i].row == 0 && G->cells.v[i].b->has_baseline) { a = G->cells.v[i].b->baseline; break; }
        t->baseline = G->rows.v[0].y + a;
        t->has_baseline = true;
    }
    if (G->collapse) {
        t->b[3] = G->vl[0];
        t->b[1] = G->vl[nc];
        t->b[0] = G->hl[0];
        t->b[2] = G->hl[nr];
        t->flags |= BF_KEEP_EDGES;
    }
}

static LBox *table_of(LBox *w) {
    for (LBox *c = w->first; c; c = c->next) if (c->kind == BK_TABLE) return c;
    return NULL;
}

static bool prepare(TGrid *G, L *l, LBox *t) {
    t->flags &= ~(uint32_t)BF_KEEP_EDGES;
    layout_compute_box_edges(l, t, 0);
    if (!build(G, l, t)) return false;
    G->ex = G->collapse ? 0 : t->b[1] + t->b[3] + t->p[1] + t->p[3];
    cell_widths(G);
    return true;
}

void layout_table_wrapper(L *l, LBox *w, const Space *sp) {
    LBox *t = table_of(w);
    WebUnit avail = sp->force_w >= 0 ? sp->force_w : (sp->cb_w < 0 ? 0 : sp->cb_w) - w->m[3] - w->m[1];
    if (!t) { w->w = 0; w->h = 0; return; }
    t->ref = w;
    TGrid G;
    if (!prepare(&G, l, t)) { w->w = w->h = 0; return; }
    WebUnit spec = 0;
    bool has_spec = box_spec_size(l, t, &t->s->box->width, sp->cb_w, true, &spec);
    if (has_spec && !t->s->box->box_sizing) {
        /* a table's width is its border-box width in browsers; spec_size added the edges for content-box sizing */
        spec -= t->b[1] + t->b[3] + t->p[1] + t->p[3];
    }
    if (sp->force_w >= 0) { spec = sp->force_w; has_spec = true; }
    WebUnit W;
    if (t->s->box->table_layout && has_spec) W = fixed_widths(&G, spec);
    else W = column_widths(&G, avail, spec, has_spec);
    layout_table(&G, t, W);
    /* captions at the table's width, above or below */
    WebUnit y = 0;
    for (int side = 0; side < 2; side++) {
        if (side == 1) { t->x = 0; t->y = y; y += t->h; }
        for (LBox *c = w->first; c; c = c->next) {
            if (c == t || c->kind != BK_BLOCK) continue;
            if ((c->s->inh->caption_side != 0) != (side == 1)) continue;
            c->ref = w;
            Space cs;
            memset(&cs, 0, sizeof cs);
            cs.cb_w = W;
            cs.cb_h = -1;
            cs.force_w = cs.force_h = -1;
            BFC own;
            memset(&own, 0, sizeof own);
            own.a = l->a;
            cs.bfc = &own;
            layout_compute_box_edges(l, c, W);
            c->x = c->m[3];
            c->y = y + c->m[0];
            layout_box(l, c, &cs);
            y += c->m[0] + c->h + c->m[2];
        }
    }
    w->w = W;
    w->h = y;
    w->has_baseline = t->has_baseline;
    w->baseline = t->y + t->baseline;
}

void table_intrinsic(L *l, LBox *w, WebUnit *mn, WebUnit *mx) {
    LBox *t = table_of(w);
    *mn = *mx = 0;
    if (!t) return;
    TGrid G;
    if (!prepare(&G, l, t)) return;
    WebUnit smin = spacing_total(&G) + G.ex, smax = smin;
    for (int k = 0; k < G.ncols; k++) { smin += G.cols[k].mn; smax += G.cols[k].mx; }
    const CLen *wl = &t->s->box->width;
    if (wl->kind == CL_PX && wl->pct == 0 && wl->px > 0) {
        WebUnit v = to_unit(wl->px);
        smin = smax = u_max(v, smin);
    }
    for (LBox *c = w->first; c; c = c->next) {
        if (c == t || c->kind != BK_BLOCK) continue;
        WebUnit a, z;
        box_contribution(l, c, &a, &z);
        if (a > smin) smin = a;
    }
    *mn = smin;
    *mx = u_max(smax, smin);
}
