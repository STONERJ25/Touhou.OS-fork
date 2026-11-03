/*
 * Grid layout (CSS Grid Layout 1), the common subset: explicit tracks from grid-template-columns/-rows (lengths,
 * percentages, fr, auto, min-content, max-content, minmax(), fit-content(), repeat() with a count, auto-fill and
 * auto-fit), grid-template-areas, line numbers (negative ones too), spans and named areas/lines for placement, implicit
 * tracks from grid-auto-columns/-rows, the auto-placement algorithm (row or column flow, sparse or dense), gaps, and the
 * track sizing algorithm of chapter 11 in a simplified form: fixed sizes; intrinsic sizes from the items spanning one
 * track, then the extra space of spanning items spread evenly; growing to the growth limits; fr tracks by the
 * "find the size of an fr" loop; stretching of auto tracks. Items align with justify-self/align-self (stretch, start,
 * end, center). Not done: subgrid, masonry, baseline alignment in grids, the exact intrinsic distribution rules.
 */
#include "layout.h"

#define GINF 0x3FFFFFFF

typedef struct GTrack {
    WebUnit base, limit;
    uint8_t mink, maxk;            /* TS_* of the min and max sizing functions */
    WebUnit minv, maxv;            /* their lengths when fixed */
    float fr;                      /* the flex factor of a TS_FR max */
    WebUnit pos;                   /* the start offset in the content box */
} GTrack;

typedef struct GItem { LBox *b; int c0, c1, r0, r1; } GItem;

typedef struct Grid {
    L *l;
    LBox *g;
    GItem *items;
    int n;
    GTrack *cols, *rows;
    int ncols, nrows;
    int ecols, erows;              /* the explicit grid */
    const Track *ctr, *rtr;        /* the expanded explicit tracks */
    WebUnit cgap, rgap;
} Grid;

static bool ts_len(L *l, const LBox *g, const TrackSize *t, WebUnit avail, WebUnit *out) {
    float fs = g->s->inh->font_size, v = t->v;
    switch (t->unit) {
    case SU_PCT: if (avail < 0) return false; *out = (WebUnit)((double)avail * v / 100); return true;
    case SU_EM: *out = to_unit(v * fs); return true;
    case SU_REM: *out = to_unit(v * 16); return true;
    case SU_EX: case SU_CH: *out = to_unit(v * fs / 2); return true;
    case SU_VW: *out = to_unit(v * (float)l->d->vw / 100); return true;
    case SU_VH: *out = to_unit(v * (float)l->d->vh / 100); return true;
    default: *out = to_unit(v); return true;
    }
}

/* the tracks of a template, with an auto-repeat expanded to fill avail (CSS Grid 7.2.3.2) */
static Track *expand(L *l, const LBox *g, const TrackList *tl, WebUnit avail, WebUnit gap, int *count) {
    if (!tl || !tl->n) { *count = 0; return NULL; }
    int reps = 1;
    if (tl->has_auto_repeat && avail >= 0) {
        WebUnit fixed = 0, rsize = 0;
        for (int i = 0; i < tl->n; i++) {
            const Track *t = &tl->v[i];
            WebUnit v = 0;
            const TrackSize *ts = t->max.kind == TS_LEN ? &t->max : &t->min;
            if (ts->kind == TS_LEN) ts_len(l, g, ts, avail, &v);
            if (t->min.kind == TS_LEN && t->max.kind == TS_LEN) { WebUnit mn; ts_len(l, g, &t->min, avail, &mn); if (mn > v) v = mn; }
            if (i >= tl->repeat_at && i < tl->repeat_at + tl->repeat_n) rsize += v + gap;
            else fixed += v + gap;
        }
        if (rsize > 0) {
            reps = (int)((avail - fixed + gap) / rsize);
            if (reps < 1) reps = 1;
            if (reps > 1000) reps = 1000;
        }
    }
    int n = tl->n + (tl->has_auto_repeat ? (reps - 1) * tl->repeat_n : 0);
    Track *v = wa_alloc(l->a, sizeof(Track) * (size_t)n);
    if (!v) { *count = 0; return NULL; }
    int k = 0;
    for (int i = 0; i < tl->n; i++) {
        if (tl->has_auto_repeat && i == tl->repeat_at) {
            for (int r = 0; r < reps; r++) for (int q = 0; q < tl->repeat_n; q++) v[k++] = tl->v[i + q];
            i += tl->repeat_n - 1;
            continue;
        }
        v[k++] = tl->v[i];
    }
    *count = k;
    return v;
}

/* a named line (the n-th with that name, 1-based) in the expanded tracks: its index, or -1 */
static int named_line(const Track *t, int nt, const TrackList *tl, WebAtom name, int nth) {
    int seen = 0;
    for (int i = 0; i <= nt; i++) {
        const WebAtom *names = i < nt ? t[i].names : (tl ? tl->end_names : NULL);
        int nn = i < nt ? t[i].nnames : (tl ? tl->nend : 0);
        for (int k = 0; k < nn; k++) if (names[k] == name && ++seen == nth) return i;
    }
    return -1;
}

/* an area's lines in one axis: false when there is no such area */
static bool area_lines(const GridAreas *a, WebAtom name, bool cols, int *s, int *e) {
    if (!a || !name) return false;
    int lo = 1 << 30, hi = -1;
    for (int r = 0; r < a->rows; r++)
        for (int c = 0; c < a->cols; c++)
            if (a->cells[r * a->cols + c] == name) {
                int v = cols ? c : r;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
    if (hi < 0) return false;
    *s = lo;
    *e = hi + 1;
    return true;
}

/* resolves one side of a placement: a line index, or -1 for auto; *span: a span (>= 1) when the side is a span */
static int resolve_line(Grid *G, const GridLine *gl, bool cols, bool is_end, int *span) {
    *span = 0;
    if (gl->autoval || (!gl->n && !gl->name && !gl->span)) return -1;
    if (gl->span) { *span = gl->n > 0 ? gl->n : 1; return -1; }
    const GridAreas *ar = G->g->s->box->areas;
    int nt = cols ? G->ecols : G->erows;
    const Track *tr = cols ? G->ctr : G->rtr;
    const TrackList *tl = cols ? G->g->s->box->tpl_cols : G->g->s->box->tpl_rows;
    if (gl->name) {
        int s, e;
        if (!gl->n && area_lines(ar, gl->name, cols, &s, &e)) return is_end ? e : s;
        int k = named_line(tr, nt, tl, gl->name, gl->n > 0 ? gl->n : 1);
        if (k >= 0) return k;
        return -1;
    }
    if (gl->n > 0) return gl->n - 1;
    int k = nt + 1 + gl->n;                                  /* -1 is the last explicit line */
    return k < 0 ? 0 : k;
}

static void place_axis(Grid *G, GItem *it, bool cols, int *s, int *e) {
    const CBox *cb = it->b->s->box;
    const GridLine *gs = cols ? &cb->gc_start : &cb->gr_start, *ge = cols ? &cb->gc_end : &cb->gr_end;
    int ss, es;
    int a = resolve_line(G, gs, cols, false, &ss), b = resolve_line(G, ge, cols, true, &es);
    if (a >= 0 && b >= 0) { if (b < a) { int t = a; a = b; b = t; } if (b == a) b = a + 1; }
    else if (a >= 0) b = a + (es ? es : 1);
    else if (b >= 0) { a = b - (ss ? ss : 1); if (a < 0) { a = 0; if (b <= a) b = 1; } }
    else { *s = -1; *e = ss ? ss : es ? es : 1; return; }        /* auto: e carries the span */
    *s = a;
    *e = b;
}

/* ------------------------------------------------------------------------------------------------ auto-placement */

typedef struct Occ { uint8_t *v; int rows, cols; WebArena *a; } Occ;

static bool occ_free(Occ *o, int r0, int r1, int c0, int c1) {
    for (int r = r0; r < r1; r++) {
        if (r >= o->rows) return true;
        for (int c = c0; c < c1 && c < o->cols; c++) if (o->v[r * o->cols + c]) return false;
    }
    return true;
}

static void occ_mark(Occ *o, int r0, int r1, int c0, int c1) {
    if (r1 > o->rows) {
        int nr = r1 + 8;
        uint8_t *nv = wa_alloc(o->a, (size_t)nr * (size_t)o->cols);
        if (!nv) return;
        if (o->v) memcpy(nv, o->v, (size_t)o->rows * (size_t)o->cols);
        o->v = nv;
        o->rows = nr;
    }
    for (int r = r0; r < r1; r++) for (int c = c0; c < c1 && c < o->cols; c++) o->v[r * o->cols + c] = 1;
}

static void auto_place(Grid *G, bool colflow, bool dense) {
    /* in column flow the axes swap: "rows" below are the columns */
    int minor_n = colflow ? G->erows : G->ecols;
    for (int i = 0; i < G->n; i++) {
        GItem *it = &G->items[i];
        int ms = colflow ? it->r0 : it->c0, me = colflow ? it->r1 : it->c1;
        if (ms >= 0 && me > minor_n) minor_n = me;
        if (ms < 0 && me > minor_n) minor_n = me;
    }
    if (minor_n < 1) minor_n = 1;
    Occ o = { NULL, 0, minor_n, G->l->a };
    /* items with a definite position in both axes, then the ones locked to a major line, then the rest */
    for (int pass = 0; pass < 3; pass++) {
        int cur_major = 0, cur_minor = 0;
        for (int i = 0; i < G->n; i++) {
            GItem *it = &G->items[i];
            int *M0 = colflow ? &it->c0 : &it->r0, *M1 = colflow ? &it->c1 : &it->r1;
            int *m0 = colflow ? &it->r0 : &it->c0, *m1 = colflow ? &it->r1 : &it->c1;
            bool maj = *M0 >= 0, min = *m0 >= 0;
            if (pass == 0) { if (maj && min) occ_mark(&o, *M0, *M1, *m0, *m1); continue; }
            if (pass == 1) {
                if (!maj || min) continue;
                int span = *m1;
                int c = 0;
                while (c + span <= minor_n && !occ_free(&o, *M0, *M1, c, c + span)) c++;
                if (c + span > minor_n) c = 0;
                *m0 = c;
                *m1 = c + span;
                occ_mark(&o, *M0, *M1, *m0, *m1);
                continue;
            }
            if (maj) continue;
            int mspan = *M1;
            if (dense) { cur_major = 0; cur_minor = 0; }
            if (min) {
                int c0 = *m0, c1 = *m1;
                if (c0 < cur_minor && !dense) cur_major++;
                int r = cur_major;
                while (!occ_free(&o, r, r + mspan, c0, c1) && r < 100000) r++;
                *M0 = r;
                *M1 = r + mspan;
                occ_mark(&o, r, r + mspan, c0, c1);
                cur_major = r;
                cur_minor = c1;
                continue;
            }
            int span = *m1 > minor_n ? minor_n : *m1;
            int r = cur_major, c = cur_minor;
            for (int guard = 0; guard < 1000000; guard++) {
                if (c + span > minor_n) { r++; c = 0; continue; }
                if (occ_free(&o, r, r + mspan, c, c + span)) break;
                c++;
            }
            *M0 = r;
            *M1 = r + mspan;
            *m0 = c;
            *m1 = c + span;
            occ_mark(&o, r, r + mspan, c, c + span);
            cur_major = r;
            cur_minor = c + span;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ track sizing */

static void init_tracks(Grid *G, GTrack *t, int n, const Track *ex, int nex, const TrackList *implicit, WebUnit avail) {
    for (int i = 0; i < n; i++) {
        Track def;
        memset(&def, 0, sizeof def);
        def.min.kind = def.max.kind = TS_AUTO;
        const Track *tr = i < nex ? &ex[i] : (implicit && implicit->n ? &implicit->v[(i - nex) % implicit->n] : &def);
        GTrack *g = &t[i];
        memset(g, 0, sizeof *g);
        g->mink = tr->min.kind;
        g->maxk = tr->max.kind;
        if (g->mink == TS_LEN && !ts_len(G->l, G->g, &tr->min, avail, &g->minv)) g->mink = TS_AUTO;
        if (g->maxk == TS_LEN && !ts_len(G->l, G->g, &tr->max, avail, &g->maxv)) g->maxk = TS_AUTO;
        if (g->maxk == TS_FIT_CONTENT && !ts_len(G->l, G->g, &tr->max, avail, &g->maxv)) g->maxk = TS_AUTO;
        if (g->maxk == TS_FR) g->fr = tr->max.v;
        if (g->mink == TS_FR) g->mink = TS_AUTO;
        g->base = g->mink == TS_LEN ? g->minv : 0;
        g->limit = g->maxk == TS_LEN ? g->maxv : GINF;
        if (g->limit < g->base) g->limit = g->base;
    }
}

static bool intrinsic_min(int k) { return k == TS_AUTO || k == TS_MIN_CONTENT || k == TS_MAX_CONTENT; }

/* the item's contributions in one axis: margin-box min/max-content widths, or its height at its column width */
static void contrib(Grid *G, GItem *it, bool cols, WebUnit *mn, WebUnit *mx) {
    if (cols) { box_contribution(G->l, it->b, mn, mx); return; }
    LBox *c = it->b;
    *mn = *mx = c->m[0] + c->h + c->m[2];
}

static void size_tracks(Grid *G, GTrack *t, int n, bool cols, WebUnit avail, WebUnit gap, bool min_mode) {
    /* items spanning one track */
    for (int i = 0; i < G->n; i++) {
        GItem *it = &G->items[i];
        int s = cols ? it->c0 : it->r0, e = cols ? it->c1 : it->r1;
        if (e - s != 1 || s < 0 || s >= n) continue;
        GTrack *g = &t[s];
        if (!intrinsic_min(g->mink) && g->maxk != TS_AUTO && g->maxk != TS_MIN_CONTENT && g->maxk != TS_MAX_CONTENT &&
            g->maxk != TS_FIT_CONTENT && g->maxk != TS_FR) continue;
        WebUnit mn, mx;
        contrib(G, it, cols, &mn, &mx);
        if (min_mode) mx = mn;
        if (intrinsic_min(g->mink)) {
            WebUnit v = g->mink == TS_MAX_CONTENT ? mx : mn;
            if (v > g->base) g->base = v;
        }
        if (g->maxk == TS_AUTO || g->maxk == TS_MAX_CONTENT || g->maxk == TS_MIN_CONTENT || g->maxk == TS_FIT_CONTENT) {
            WebUnit v = g->maxk == TS_MIN_CONTENT ? mn : mx;
            if (g->maxk == TS_FIT_CONTENT && v > g->maxv) v = g->maxv;
            if (g->limit == GINF || v > g->limit) g->limit = g->limit == GINF ? v : (v > g->limit ? v : g->limit);
        }
    }
    for (int i = 0; i < n; i++) {
        if (t[i].limit == GINF && t[i].maxk != TS_FR) t[i].limit = t[i].base;
        if (t[i].maxk != TS_FR && t[i].limit < t[i].base) t[i].limit = t[i].base;
    }
    /* spanning items not crossing flexible tracks: their extra space spread over the intrinsic tracks */
    for (int i = 0; i < G->n; i++) {
        GItem *it = &G->items[i];
        int s = cols ? it->c0 : it->r0, e = cols ? it->c1 : it->r1;
        if (e - s < 2 || s < 0 || e > n) continue;
        bool flex = false;
        int nint = 0;
        WebUnit sum = gap * (e - s - 1);
        for (int k = s; k < e; k++) { if (t[k].maxk == TS_FR) flex = true; if (intrinsic_min(t[k].mink)) nint++; sum += t[k].base; }
        if (flex || !nint) continue;
        WebUnit mn, mx;
        contrib(G, it, cols, &mn, &mx);
        WebUnit extra = (min_mode ? mn : mx) - sum;
        if (extra <= 0) continue;
        for (int k = s; k < e; k++)
            if (intrinsic_min(t[k].mink)) {
                t[k].base += extra / nint;
                if (t[k].limit < t[k].base) t[k].limit = t[k].base;
            }
    }
    WebUnit gaps = gap * (n > 1 ? n - 1 : 0);
    /* grow the tracks to their limits with the free space */
    if (avail >= 0) {
        for (int round = 0; round < 8; round++) {
            WebUnit used = gaps;
            int ngrow = 0;
            for (int i = 0; i < n; i++) { used += t[i].base; if (t[i].maxk != TS_FR && t[i].base < t[i].limit) ngrow++; }
            WebUnit free = avail - used;
            if (free <= 0 || !ngrow) break;
            WebUnit per = free / ngrow;
            if (per <= 0) break;
            for (int i = 0; i < n; i++)
                if (t[i].maxk != TS_FR && t[i].base < t[i].limit) {
                    t[i].base += per;
                    if (t[i].base > t[i].limit) t[i].base = t[i].limit;
                }
        }
    } else {
        for (int i = 0; i < n; i++) if (t[i].maxk != TS_FR) t[i].base = t[i].limit;
    }
    /* flexible tracks (11.7) */
    float sumfr = 0;
    for (int i = 0; i < n; i++) if (t[i].maxk == TS_FR) sumfr += t[i].fr;
    if (sumfr > 0) {
        float frsize = 0;
        if (avail >= 0) {
            bool inflex[1024] = { false };
            for (int round = 0; round < 32; round++) {
                WebUnit left = avail - gaps;
                float fs = 0;
                for (int i = 0; i < n; i++) {
                    if (t[i].maxk == TS_FR && !(i < 1024 && inflex[i])) fs += t[i].fr;
                    else left -= t[i].base;
                }
                if (fs <= 0) break;
                if (fs < 1) fs = 1;
                frsize = (float)left / fs;
                bool again = false;
                for (int i = 0; i < n && i < 1024; i++)
                    if (t[i].maxk == TS_FR && !inflex[i] && (float)t[i].base > frsize * t[i].fr) { inflex[i] = true; again = true; }
                if (!again) break;
            }
        } else {
            for (int i = 0; i < n; i++)
                if (t[i].maxk == TS_FR && t[i].fr > 0) {
                    float v = (float)t[i].base / (t[i].fr < 1 ? 1 : t[i].fr);
                    if (v > frsize) frsize = v;
                }
            /* items crossing flexible tracks: their max-content contribution over the fr sum */
            for (int i = 0; i < G->n; i++) {
                GItem *it = &G->items[i];
                int s = cols ? it->c0 : it->r0, e = cols ? it->c1 : it->r1;
                float f = 0;
                WebUnit fixed = gap * (e - s - 1);
                for (int k = s; k >= 0 && k < e && k < n; k++) { if (t[k].maxk == TS_FR) f += t[k].fr; else fixed += t[k].base; }
                if (f <= 0) continue;
                WebUnit mn, mx;
                contrib(G, it, cols, &mn, &mx);
                float v = (float)((min_mode ? mn : mx) - fixed) / (f < 1 ? 1 : f);
                if (v > frsize) frsize = v;
            }
        }
        if (frsize < 0) frsize = 0;
        for (int i = 0; i < n; i++)
            if (t[i].maxk == TS_FR) {
                WebUnit v = (WebUnit)(frsize * t[i].fr);
                if (v > t[i].base) t[i].base = v;
            }
    } else if (avail >= 0) {
        /* stretch the auto tracks with what is left (justify-content/align-content: normal) */
        const CBox *bx = G->g->s->box;
        int jc = cols ? bx->justify_content : bx->align_content;
        if (jc == AL_NORMAL || jc == AL_STRETCH) {
            WebUnit used = gaps;
            int na = 0;
            for (int i = 0; i < n; i++) { used += t[i].base; if (t[i].maxk == TS_AUTO) na++; }
            if (avail > used && na) {
                WebUnit per = (avail - used) / na;
                for (int i = 0; i < n; i++) if (t[i].maxk == TS_AUTO) t[i].base += per;
            }
        }
    }
}

static WebUnit place_tracks(GTrack *t, int n, WebUnit gap, WebUnit avail, int content_align) {
    WebUnit total = gap * (n > 1 ? n - 1 : 0);
    for (int i = 0; i < n; i++) total += t[i].base;
    WebUnit pos = 0, extra = 0, free = avail >= 0 ? avail - total : 0;
    if (free > 0) switch (content_align) {
    case AL_END: case AL_FLEX_END: pos = free; break;
    case AL_CENTER: pos = free / 2; break;
    case AL_SPACE_BETWEEN: if (n > 1) extra = free / (n - 1); break;
    case AL_SPACE_AROUND: extra = free / n; pos = extra / 2; break;
    case AL_SPACE_EVENLY: extra = free / (n + 1); pos = extra; break;
    default: break;
    }
    for (int i = 0; i < n; i++) { t[i].pos = pos; pos += t[i].base + gap + extra; }
    return total;
}

static WebUnit span_size(const GTrack *t, int s, int e, WebUnit gap) {
    if (s < 0 || e <= s) return 0;
    return t[e - 1].pos + t[e - 1].base - t[s].pos + 0 * gap;
}

static int self_align(int a, int deflt) {
    switch (a) {
    case AL_AUTO: case AL_NORMAL: return deflt;
    case AL_STRETCH: return AL_STRETCH;
    case AL_END: case AL_FLEX_END: case AL_SELF_END: case AL_RIGHT: return AL_END;
    case AL_CENTER: return AL_CENTER;
    default: return AL_START;
    }
}

static void lay_item(L *l, LBox *c, WebUnit cbw, WebUnit cbh, WebUnit fw, WebUnit fh) {
    Space s;
    memset(&s, 0, sizeof s);
    s.cb_w = cbw;
    s.cb_h = cbh;
    s.force_w = fw;
    s.force_h = fh;
    BFC own;
    memset(&own, 0, sizeof own);
    own.a = l->a;
    s.bfc = &own;
    layout_box(l, c, &s);
}

static bool build(Grid *G, L *l, LBox *g, WebUnit cw, WebUnit ch) {
    const CBox *bx = g->s->box;
    memset(G, 0, sizeof *G);
    G->l = l;
    G->g = g;
    G->cgap = resolve_len_or(&bx->col_gap, cw < 0 ? 0 : cw, 0);
    G->rgap = resolve_len_or(&bx->row_gap, ch < 0 ? 0 : ch, 0);
    if (G->cgap < 0) G->cgap = 0;
    if (G->rgap < 0) G->rgap = 0;
    G->ctr = expand(l, g, bx->tpl_cols, cw, G->cgap, &G->ecols);
    G->rtr = expand(l, g, bx->tpl_rows, ch, G->rgap, &G->erows);
    if (bx->areas) {
        if (bx->areas->cols > G->ecols) G->ecols = bx->areas->cols;
        if (bx->areas->rows > G->erows) G->erows = bx->areas->rows;
    }
    int n = 0;
    for (LBox *c = g->first; c; c = c->next) if (!(c->flags & BF_ABS)) n++;
    G->items = n ? wa_alloc(l->a, sizeof(GItem) * (size_t)n) : NULL;
    if (n && !G->items) return false;
    for (LBox *c = g->first; c; c = c->next) {
        if (c->flags & BF_ABS) continue;
        GItem *it = &G->items[G->n++];
        it->b = c;
        place_axis(G, it, true, &it->c0, &it->c1);
        place_axis(G, it, false, &it->r0, &it->r1);
    }
    /* order-modified document order */
    for (int i = 1; i < G->n; i++) {
        GItem t = G->items[i];
        int j = i - 1;
        while (j >= 0 && G->items[j].b->s->box->order > t.b->s->box->order) { G->items[j + 1] = G->items[j]; j--; }
        G->items[j + 1] = t;
    }
    auto_place(G, (bx->auto_flow & 2) != 0, (bx->auto_flow & 4) != 0);
    G->ncols = G->ecols;
    G->nrows = G->erows;
    for (int i = 0; i < G->n; i++) {
        if (G->items[i].c1 > G->ncols) G->ncols = G->items[i].c1;
        if (G->items[i].r1 > G->nrows) G->nrows = G->items[i].r1;
    }
    if (G->ncols > 1000) G->ncols = 1000;
    if (G->nrows > 100000) G->nrows = 100000;
    G->cols = wa_alloc(l->a, sizeof(GTrack) * (size_t)(G->ncols ? G->ncols : 1));
    G->rows = wa_alloc(l->a, sizeof(GTrack) * (size_t)(G->nrows ? G->nrows : 1));
    if (!G->cols || !G->rows) return false;
    init_tracks(G, G->cols, G->ncols, G->ctr, G->ecols < G->ncols ? (G->ctr ? G->ecols : 0) : (G->ctr ? G->ecols : 0), bx->auto_cols, cw);
    init_tracks(G, G->rows, G->nrows, G->rtr, G->rtr ? G->erows : 0, bx->auto_rows, ch);
    for (int i = 0; i < G->n; i++) {
        GItem *it = &G->items[i];
        if (it->c1 > G->ncols) it->c1 = G->ncols;
        if (it->r1 > G->nrows) it->r1 = G->nrows;
    }
    return true;
}

void layout_grid(L *l, LBox *g, const Space *sp) {
    WebUnit cw = box_content_w(g);
    WebUnit ex = g->b[0] + g->b[2] + g->p[0] + g->p[2];
    WebUnit chd = -1, hv;
    if (sp->force_h >= 0) chd = sp->force_h - ex;
    else if (box_spec_size(l, g, &g->s->box->height, sp->cb_h, false, &hv)) chd = box_clamp(l, g, hv, sp->cb_h, false) - ex;
    WebUnit left = g->b[3] + g->p[3], top = g->b[0] + g->p[0];
    for (LBox *c = g->first; c; c = c->next)
        if (c->flags & BF_ABS) { c->ref = g; c->x = left; c->y = top; abs_defer(l, c); }
    Grid G;
    if (!build(&G, l, g, cw, chd)) { g->h = ex; return; }
    const CBox *bx = g->s->box;
    size_tracks(&G, G.cols, G.ncols, true, cw, G.cgap, false);
    place_tracks(G.cols, G.ncols, G.cgap, cw, bx->justify_content);
    /* the items at their column widths give the row contributions */
    for (int i = 0; i < G.n; i++) {
        GItem *it = &G.items[i];
        LBox *c = it->b;
        c->ref = g;
        layout_compute_box_edges(l, c, cw);
        WebUnit aw = span_size(G.cols, it->c0, it->c1, G.cgap);
        int js = self_align(c->s->box->justify_self, self_align(bx->justify_items, c->kind == BK_REPLACED ? AL_START : AL_STRETCH));
        WebUnit w;
        if (!box_spec_size(l, c, &c->s->box->width, aw, true, &w)) {
            if (js == AL_STRETCH && c->s->box->margin[1].kind != CL_AUTO && c->s->box->margin[3].kind != CL_AUTO && c->kind != BK_REPLACED)
                w = aw - c->m[1] - c->m[3];
            else if (c->kind == BK_REPLACED) { layout_replaced_size(l, c, aw, -1); w = c->w; }
            else w = shrink_to_fit(l, c, aw - c->m[1] - c->m[3]);
        }
        w = box_clamp(l, c, w, aw, true);
        if (w < 0) w = 0;
        lay_item(l, c, aw, -1, w, -1);
    }
    size_tracks(&G, G.rows, G.nrows, false, chd, G.rgap, false);
    WebUnit total_h = place_tracks(G.rows, G.nrows, G.rgap, chd, bx->align_content);
    /* the container's height, then the rows again if it is definite only now (min-height) */
    WebUnit h;
    if (sp->force_h >= 0) h = sp->force_h;
    else if (chd >= 0) h = chd + ex;
    else h = box_clamp(l, g, total_h + ex, sp->cb_h, false);
    g->h = h;
    if (chd < 0 && h - ex > total_h) place_tracks(G.rows, G.nrows, G.rgap, h - ex, bx->align_content);
    /* final item positions and stretched heights */
    for (int i = 0; i < G.n; i++) {
        GItem *it = &G.items[i];
        LBox *c = it->b;
        const CBox *cb = c->s->box;
        WebUnit ax = it->c0 >= 0 && it->c0 < G.ncols ? G.cols[it->c0].pos : 0;
        WebUnit ay = it->r0 >= 0 && it->r0 < G.nrows ? G.rows[it->r0].pos : 0;
        WebUnit aw = span_size(G.cols, it->c0, it->c1, G.cgap), ah = span_size(G.rows, it->r0, it->r1, G.rgap);
        int as = self_align(cb->align_self, self_align(bx->align_items, c->kind == BK_REPLACED ? AL_START : AL_STRETCH));
        int js = self_align(cb->justify_self, self_align(bx->justify_items, c->kind == BK_REPLACED ? AL_START : AL_STRETCH));
        if (as == AL_STRETCH && cb->height.kind == CL_AUTO && cb->margin[0].kind != CL_AUTO && cb->margin[2].kind != CL_AUTO) {
            WebUnit want = box_clamp(l, c, ah - c->m[0] - c->m[2], ah, false);
            if (want < 0) want = 0;
            if (want != c->h) lay_item(l, c, aw, ah, c->w, want);
        }
        WebUnit rx = aw - (c->m[3] + c->w + c->m[1]), ry = ah - (c->m[0] + c->h + c->m[2]);
        WebUnit ox = 0, oy = 0;
        if (cb->margin[3].kind == CL_AUTO && cb->margin[1].kind == CL_AUTO) ox = rx > 0 ? rx / 2 : 0;
        else if (cb->margin[3].kind == CL_AUTO) ox = rx > 0 ? rx : 0;
        else if (js == AL_END) ox = rx;
        else if (js == AL_CENTER) ox = rx / 2;
        if (cb->margin[0].kind == CL_AUTO && cb->margin[2].kind == CL_AUTO) oy = ry > 0 ? ry / 2 : 0;
        else if (cb->margin[0].kind == CL_AUTO) oy = ry > 0 ? ry : 0;
        else if (as == AL_END) oy = ry;
        else if (as == AL_CENTER) oy = ry / 2;
        c->x = left + ax + ox + c->m[3];
        c->y = top + ay + oy + c->m[0];
    }
    g->has_baseline = false;
    if (G.n) {
        LBox *c = G.items[0].b;
        g->baseline = c->y + (c->has_baseline ? c->baseline : c->h);
        g->has_baseline = true;
    }
}

void grid_intrinsic(L *l, LBox *g, WebUnit *mn, WebUnit *mx) {
    Grid G;
    if (!build(&G, l, g, -1, -1)) { *mn = *mx = 0; return; }
    WebUnit gaps = G.cgap * (G.ncols > 1 ? G.ncols - 1 : 0);
    size_tracks(&G, G.cols, G.ncols, true, -1, G.cgap, true);
    WebUnit a = gaps;
    for (int i = 0; i < G.ncols; i++) a += G.cols[i].base;
    Grid H;
    build(&H, l, g, -1, -1);
    size_tracks(&H, H.cols, H.ncols, true, -1, H.cgap, false);
    WebUnit z = gaps;
    for (int i = 0; i < H.ncols; i++) z += H.cols[i].base;
    *mn = a;
    *mx = z < a ? a : z;
}
