/*
 * Flexbox (CSS Flexible Box Layout 1, chapter 9 "Flex Layout Algorithm"): flex-direction (row, column and the reverses),
 * flex-wrap (and wrap-reverse), order, flex-grow/-shrink/-basis with the freezing loop of 9.7, the automatic minimum
 * size of flex items (4.5, in the main axis), gaps, justify-content, align-items/-self (stretch, start, end, center,
 * baseline), align-content for multi-line containers, auto margins in both axes. Absolutely positioned children are
 * placed at the content box's start as their static position.
 *
 * Main sizes and cross sizes are border-box sizes here; "outer" adds the item's margins.
 */
#include "layout.h"

#include <stdlib.h>

typedef struct FItem {
    LBox *b;
    WebUnit base, hyp, target;     /* flex base size, hypothetical and target main sizes */
    WebUnit mmin, mmax;            /* main size limits (mmax < 0: none) */
    WebUnit m0, m1, c0, c1;        /* margins: main start, main end, cross start, cross end */
    WebUnit cross;                 /* cross size */
    WebUnit mpos, cpos;            /* margin-box positions in the content box (logical, before reversing) */
    WebUnit asc;                   /* baseline alignment: margin-box top to baseline */
    float grow, shrink;
    bool frozen, am0, am1, ac0, ac1;
    uint8_t align;
} FItem;

typedef struct FLine { int first, n; WebUnit cross, pos, asc; } FLine;

static int item_align(const LBox *c, const LBox *b) {
    int a = c->s->box->align_self;
    if (a == AL_AUTO) a = b->s->box->align_items;
    switch (a) {
    case AL_NORMAL: case AL_STRETCH: return AL_STRETCH;
    case AL_FLEX_END: case AL_END: case AL_SELF_END: return AL_END;
    case AL_CENTER: return AL_CENTER;
    case AL_BASELINE: case AL_LAST_BASELINE: return AL_BASELINE;
    default: return AL_START;
    }
}

static WebUnit gap_of(const CLen *g, WebUnit basis) {
    bool a;
    WebUnit v = resolve_len(g, basis < 0 ? 0 : basis, &a);
    return a || v < 0 ? 0 : v;
}

static WebUnit inner_w(const LBox *b) { return box_content_w(b); }

/* the border-box height a block gets from its content height (CSS 2.1 10.6.3 with min/max) */
static WebUnit final_height(L *l, LBox *b, const Space *sp, WebUnit content) {
    if (sp->force_h >= 0) return sp->force_h;
    WebUnit h;
    if (!box_spec_size(l, b, &b->s->box->height, sp->cb_h, false, &h))
        h = content + b->b[0] + b->b[2] + b->p[0] + b->p[2];
    return box_clamp(l, b, h, sp->cb_h, false);
}

/* the definite content height of the container, or -1 */
static WebUnit definite_inner_h(L *l, LBox *b, const Space *sp) {
    WebUnit ex = b->b[0] + b->b[2] + b->p[0] + b->p[2];
    if (sp->force_h >= 0) return sp->force_h - ex;
    WebUnit h;
    if (!box_spec_size(l, b, &b->s->box->height, sp->cb_h, false, &h)) return -1;
    return box_clamp(l, b, h, sp->cb_h, false) - ex;
}

static void lay(L *l, LBox *c, WebUnit cbw, WebUnit cbh, WebUnit fw, WebUnit fh) {
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

/* 9.7: resolving flexible lengths on one line */
static void resolve_flex(FItem *it, int n, WebUnit avail, WebUnit gaps) {
    WebUnit sum = gaps;
    for (int i = 0; i < n; i++) sum += it[i].hyp + it[i].m0 + it[i].m1;
    bool grow = sum < avail;
    for (int i = 0; i < n; i++) {
        it[i].target = it[i].hyp;
        float f = grow ? it[i].grow : it[i].shrink;
        it[i].frozen = avail < 0 || f <= 0 || (grow ? it[i].base > it[i].hyp : it[i].base < it[i].hyp);
    }
    if (avail < 0) return;
    WebUnit initial_free = avail - sum;
    for (int round = 0; round <= n; round++) {
        bool any = false;
        WebUnit used = gaps;
        float sumf = 0, sums = 0;
        for (int i = 0; i < n; i++) {
            used += (it[i].frozen ? it[i].target : it[i].base) + it[i].m0 + it[i].m1;
            if (!it[i].frozen) {
                any = true;
                sumf += grow ? it[i].grow : it[i].shrink;
                sums += it[i].shrink * (float)(it[i].base > 0 ? it[i].base : 0);
            }
        }
        if (!any) break;
        WebUnit free = avail - used;
        if (sumf < 1) {
            WebUnit scaled = (WebUnit)((float)initial_free * sumf);
            if ((scaled < 0 ? -scaled : scaled) < (free < 0 ? -free : free)) free = scaled;
        }
        WebUnit total = 0;
        for (int i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            WebUnit t;
            if (grow) t = it[i].base + (sumf > 0 ? (WebUnit)((float)free * (it[i].grow / sumf)) : 0);
            else {
                float r = sums > 0 ? it[i].shrink * (float)(it[i].base > 0 ? it[i].base : 0) / sums : 0;
                t = it[i].base + (WebUnit)((float)free * r);
            }
            WebUnit cl = t;
            if (it[i].mmax >= 0 && cl > it[i].mmax) cl = it[i].mmax;
            if (cl < it[i].mmin) cl = it[i].mmin;
            if (cl < 0) cl = 0;
            total += cl - t;
            it[i].target = cl;
            it[i].hyp = t;                                   /* (the unclamped size, to find the violators) */
        }
        for (int i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            WebUnit v = it[i].target - it[i].hyp;
            if (total == 0 || (total > 0 && v > 0) || (total < 0 && v < 0)) it[i].frozen = true;
        }
    }
}

void layout_flex(L *l, LBox *b, const Space *sp) {
    const CBox *bx = b->s->box;
    bool row = bx->flex_dir < 2, rev = bx->flex_dir == 1 || bx->flex_dir == 3;
    bool wrap = bx->flex_wrap != 0, wrap_rev = bx->flex_wrap == 2;
    WebUnit cw = inner_w(b);
    WebUnit chd = definite_inner_h(l, b, sp);
    WebUnit left = b->b[3] + b->p[3], top = b->b[0] + b->p[0];
    WebUnit main_avail = row ? cw : chd;
    WebUnit gap_main = gap_of(row ? &bx->col_gap : &bx->row_gap, row ? cw : chd);
    WebUnit gap_cross = gap_of(row ? &bx->row_gap : &bx->col_gap, row ? chd : cw);
    int n = 0;
    for (LBox *c = b->first; c; c = c->next) {
        if (c->flags & BF_ABS) { c->ref = b; c->x = left; c->y = top; abs_defer(l, c); continue; }
        n++;
    }
    FItem *it = n ? wa_alloc(l->a, sizeof(FItem) * (size_t)n) : NULL;
    if (n && !it) { b->h = final_height(l, b, sp, 0); return; }
    int k = 0;
    for (LBox *c = b->first; c; c = c->next) if (!(c->flags & BF_ABS)) it[k++].b = c;
    /* order: a stable sort */
    for (int i = 1; i < n; i++) {
        FItem t = it[i];
        int j = i - 1;
        while (j >= 0 && it[j].b->s->box->order > t.b->s->box->order) { it[j + 1] = it[j]; j--; }
        it[j + 1] = t;
    }
    /* flex base sizes and hypothetical main sizes (9.2) */
    for (int i = 0; i < n; i++) {
        FItem *f = &it[i];
        LBox *c = f->b;
        c->ref = b;
        const CBox *cb = c->s->box;
        layout_compute_box_edges(l, c, cw);
        f->m0 = row ? c->m[3] : c->m[0];
        f->m1 = row ? c->m[1] : c->m[2];
        f->c0 = row ? c->m[0] : c->m[3];
        f->c1 = row ? c->m[2] : c->m[1];
        f->am0 = (row ? cb->margin[3] : cb->margin[0]).kind == CL_AUTO;
        f->am1 = (row ? cb->margin[1] : cb->margin[2]).kind == CL_AUTO;
        f->ac0 = (row ? cb->margin[0] : cb->margin[3]).kind == CL_AUTO;
        f->ac1 = (row ? cb->margin[2] : cb->margin[1]).kind == CL_AUTO;
        f->grow = cb->flex_grow;
        f->shrink = cb->flex_shrink;
        f->align = (uint8_t)item_align(c, b);
        WebUnit base = 0, content = 0;
        bool have = false, from_content = false;
        const CLen *basis = &cb->flex_basis;
        if (basis->kind != CL_AUTO && basis->kind != CL_CONTENT)
            have = box_spec_size(l, c, basis, main_avail, row, &base);
        if (!have && basis->kind == CL_AUTO) have = box_spec_size(l, c, row ? &cb->width : &cb->height, row ? cw : chd, row, &base);
        if (row) {
            WebUnit mn, mx;
            intrinsic_widths(l, c, &mn, &mx);
            WebUnit ex = c->b[1] + c->b[3] + c->p[1] + c->p[3];
            if (c->kind == BK_REPLACED) { layout_replaced_size(l, c, cw, chd); mn = mx = c->w - ex; }
            content = mn + ex;
            if (!have) { base = mx + ex; from_content = true; }
        } else {
            /* a column item: its height at its cross size */
            WebUnit w;
            bool fixed = box_spec_size(l, c, &cb->width, cw, true, &w);
            if (!fixed) {
                if (f->align == AL_STRETCH && !f->ac0 && !f->ac1) w = cw - f->c0 - f->c1;
                else w = shrink_to_fit(l, c, cw - f->c0 - f->c1);
            }
            w = box_clamp(l, c, w, cw, true);
            lay(l, c, cw, chd, w, -1);
            f->cross = c->w;
            content = c->h;
            if (!have) { base = c->h; from_content = true; }
        }
        f->base = base;
        WebUnit mmin = 0, mmax = -1, v;
        const CLen *minp = row ? &cb->min_w : &cb->min_h, *maxp = row ? &cb->max_w : &cb->max_h;
        if (maxp->kind != CL_NONE && box_spec_size(l, c, maxp, row ? cw : chd, row, &v)) mmax = v;
        if (minp->kind == CL_AUTO) {
            /* the automatic minimum size: the content size, or the specified size when that is smaller */
            if (!(c->flags & BF_SCROLL)) {
                mmin = content;
                if (have && !from_content && base < mmin && basis->kind == CL_AUTO) mmin = base;
                if (mmax >= 0 && mmin > mmax) mmin = mmax;
            }
        } else if (box_spec_size(l, c, minp, row ? cw : chd, row, &v)) mmin = v;
        f->mmin = mmin;
        f->mmax = mmax;
        WebUnit h = base;
        if (mmax >= 0 && h > mmax) h = mmax;
        if (h < mmin) h = mmin;
        f->hyp = h;
    }
    /* lines (9.3) */
    FLine *lines = wa_alloc(l->a, sizeof(FLine) * (size_t)(n ? n : 1));
    int nl = 0;
    if (!lines) { b->h = final_height(l, b, sp, 0); return; }
    for (int i = 0; i < n;) {
        int j = i;
        WebUnit sum = 0;
        while (j < n) {
            WebUnit o = it[j].hyp + it[j].m0 + it[j].m1;
            if (wrap && main_avail >= 0 && j > i && sum + gap_main + o > main_avail) break;
            sum += (j > i ? gap_main : 0) + o;
            j++;
        }
        lines[nl].first = i;
        lines[nl].n = j - i;
        nl++;
        i = j;
    }
    /* the main size of a column container with an indefinite height: its content */
    WebUnit inner_main = main_avail;
    if (inner_main < 0) {
        WebUnit mx = 0;
        for (int li = 0; li < nl; li++) {
            WebUnit s = 0;
            for (int q = 0; q < lines[li].n; q++) s += it[lines[li].first + q].hyp + it[lines[li].first + q].m0 + it[lines[li].first + q].m1;
            s += gap_main * (lines[li].n > 1 ? lines[li].n - 1 : 0);
            if (s > mx) mx = s;
        }
        WebUnit ex = b->b[0] + b->b[2] + b->p[0] + b->p[2];
        inner_main = final_height(l, b, sp, mx) - ex;
    }
    /* flexible lengths, then the cross sizes from laying the items out at their main sizes */
    for (int li = 0; li < nl; li++) {
        FItem *f = &it[lines[li].first];
        int m = lines[li].n;
        resolve_flex(f, m, inner_main, gap_main * (m > 1 ? m - 1 : 0));
        WebUnit cross = 0, asc_max = 0, desc_max = 0;
        for (int q = 0; q < m; q++) {
            LBox *c = f[q].b;
            if (row) {
                if (c->kind == BK_REPLACED) {
                    lay(l, c, cw, chd, f[q].target, -1);
                } else lay(l, c, cw, chd, f[q].target, -1);
                f[q].cross = c->h;
            } else if (c->h != f[q].target || c->w != f[q].cross) {
                lay(l, c, cw, chd, f[q].cross, f[q].target);
            }
            WebUnit oc = f[q].cross + f[q].c0 + f[q].c1;
            if (f[q].align == AL_BASELINE && row) {
                WebUnit a = c->has_baseline ? f[q].c0 + c->baseline : oc;
                f[q].asc = a;
                if (a > asc_max) asc_max = a;
                if (oc - a > desc_max) desc_max = oc - a;
            }
            if (oc > cross) cross = oc;
        }
        if (asc_max + desc_max > cross) cross = asc_max + desc_max;
        lines[li].cross = cross;
        lines[li].asc = asc_max;
    }
    /* the container's cross size */
    WebUnit inner_cross;
    WebUnit sum_cross = 0;
    for (int li = 0; li < nl; li++) sum_cross += lines[li].cross + (li ? gap_cross : 0);
    if (row) {
        b->h = final_height(l, b, sp, sum_cross);
        inner_cross = box_content_h(b);
    } else {
        inner_cross = cw;
        WebUnit ex = b->b[0] + b->b[2] + b->p[0] + b->p[2];
        b->h = inner_main + ex;
        b->h = box_clamp(l, b, b->h, sp->cb_h, false);
    }
    bool definite_cross = row ? (chd >= 0) : true;
    if (nl == 1 && (!wrap || definite_cross) && definite_cross) lines[0].cross = inner_cross;
    /* align-content: the free cross space among the lines (normal behaves as stretch) */
    WebUnit cfree = inner_cross - sum_cross;
    WebUnit cpos = 0, cgap = gap_cross;
    if (nl > 1 || wrap) {
        int ac = bx->align_content;
        if (cfree > 0) {
            switch (ac) {
            case AL_FLEX_END: case AL_END: cpos = cfree; break;
            case AL_CENTER: cpos = cfree / 2; break;
            case AL_SPACE_BETWEEN: if (nl > 1) cgap += cfree / (nl - 1); break;
            case AL_SPACE_AROUND: cgap += cfree / nl; cpos = cfree / nl / 2; break;
            case AL_SPACE_EVENLY: cgap += cfree / (nl + 1); cpos = cfree / (nl + 1); break;
            case AL_FLEX_START: case AL_START: break;
            default: for (int li = 0; li < nl; li++) lines[li].cross += cfree / nl; break;
            }
        }
    }
    for (int li = 0; li < nl; li++) {
        lines[li].pos = cpos;
        cpos += lines[li].cross + cgap;
    }
    /* stretched items, cross positions, main positions */
    for (int li = 0; li < nl; li++) {
        FItem *f = &it[lines[li].first];
        int m = lines[li].n;
        FLine *ln = &lines[li];
        for (int q = 0; q < m; q++) {
            LBox *c = f[q].b;
            const CBox *cb = c->s->box;
            bool cross_auto = (row ? cb->height : cb->width).kind == CL_AUTO;
            if (f[q].align == AL_STRETCH && cross_auto && !f[q].ac0 && !f[q].ac1) {
                WebUnit want = ln->cross - f[q].c0 - f[q].c1;
                want = box_clamp(l, c, want, row ? chd : cw, !row);
                if (want < 0) want = 0;
                if (want != f[q].cross) {
                    if (row) lay(l, c, cw, chd, f[q].target, want);
                    else lay(l, c, cw, chd, want, f[q].target);
                    f[q].cross = want;
                }
            }
            WebUnit oc = f[q].cross + f[q].c0 + f[q].c1;
            WebUnit room = ln->cross - oc, p = 0;
            if (f[q].ac0 || f[q].ac1) {
                if (room > 0) p = f[q].ac0 && f[q].ac1 ? room / 2 : f[q].ac0 ? room : 0;
            } else switch (f[q].align) {
            case AL_END: p = room; break;
            case AL_CENTER: p = room / 2; break;
            case AL_BASELINE: if (row) p = ln->asc - f[q].asc; break;
            default: break;
            }
            if (wrap_rev && f[q].align != AL_BASELINE) p = room - p;
            f[q].cpos = ln->pos + p;
        }
        /* main axis: auto margins take the free space first, else justify-content */
        WebUnit used = gap_main * (m > 1 ? m - 1 : 0);
        int nauto = 0;
        for (int q = 0; q < m; q++) { used += f[q].target + f[q].m0 + f[q].m1; nauto += f[q].am0 + f[q].am1; }
        WebUnit free = inner_main - used, mpos = 0, extra = 0;
        if (nauto && free > 0) {
            WebUnit per = free / nauto;
            for (int q = 0; q < m; q++) { if (f[q].am0) f[q].m0 += per; if (f[q].am1) f[q].m1 += per; }
            free = 0;
        }
        int jc = bx->justify_content;
        if (free > 0) switch (jc) {
        case AL_FLEX_END: case AL_END: case AL_RIGHT: mpos = free; break;
        case AL_CENTER: mpos = free / 2; break;
        case AL_SPACE_BETWEEN: if (m > 1) extra = free / (m - 1); break;
        case AL_SPACE_AROUND: extra = free / m; mpos = extra / 2; break;
        case AL_SPACE_EVENLY: extra = free / (m + 1); mpos = extra; break;
        default: break;
        }
        else if (free < 0 && (jc == AL_CENTER)) mpos = free / 2;
        for (int q = 0; q < m; q++) {
            f[q].mpos = mpos;
            mpos += f[q].m0 + f[q].target + f[q].m1 + gap_main + extra;
        }
        /* positions relative to the container's border box */
        for (int q = 0; q < m; q++) {
            LBox *c = f[q].b;
            WebUnit ms = f[q].mpos + f[q].m0;              /* border-box start on the main axis */
            if (rev) ms = inner_main - (f[q].mpos + f[q].m0 + f[q].target);
            WebUnit cs = f[q].cpos + f[q].c0;
            if (wrap_rev) cs = inner_cross - (ln->pos + ln->cross) + (f[q].cpos - ln->pos) + f[q].c0;
            if (row) { c->x = left + ms; c->y = top + cs; }
            else { c->x = left + cs; c->y = top + ms; }
        }
    }
    /* the container's baseline: its first item's */
    b->has_baseline = false;
    if (n) {
        LBox *c = it[0].b;
        for (int i = 0; i < n; i++) if (it[i].align == AL_BASELINE && it[i].b->has_baseline) { c = it[i].b; break; }
        if (c->has_baseline) { b->baseline = c->y + c->baseline; b->has_baseline = true; }
        else { b->baseline = c->y + c->h; b->has_baseline = true; }
    }
    if (b->flags & BF_SCROLL) {
        WebUnit mw = 0, mh = 0;
        for (int i = 0; i < n; i++) {
            LBox *c = it[i].b;
            if (c->x + c->w + c->m[1] - b->b[3] > mw) mw = c->x + c->w + c->m[1] - b->b[3];
            if (c->y + c->h + c->m[2] - b->b[0] > mh) mh = c->y + c->h + c->m[2] - b->b[0];
        }
        WebUnit pw = b->w - b->b[1] - b->b[3], ph = b->h - b->b[0] - b->b[2];
        b->cw = mw > pw ? mw : pw;
        b->ch = mh > ph ? mh : ph;
        if (b->node && is_el(b->node)) {
            WebUnit maxx = b->cw - pw, maxy = b->ch - ph;
            b->sx = b->node->u.el.scroll_x < maxx ? b->node->u.el.scroll_x : maxx;
            b->sy = b->node->u.el.scroll_y < maxy ? b->node->u.el.scroll_y : maxy;
            if (b->sx < 0) b->sx = 0;
            if (b->sy < 0) b->sy = 0;
        }
    }
}

void flex_intrinsic(L *l, LBox *b, WebUnit *mn, WebUnit *mx) {
    const CBox *bx = b->s->box;
    bool row = bx->flex_dir < 2, wrap = bx->flex_wrap != 0;
    WebUnit gap = gap_of(row ? &bx->col_gap : &bx->row_gap, 0);
    WebUnit smin = 0, smax = 0, bmin = 0, bmax = 0;
    int n = 0;
    for (LBox *c = b->first; c; c = c->next) {
        if (c->flags & BF_ABS) continue;
        WebUnit a, z;
        box_contribution(l, c, &a, &z);
        smin += a + (n ? gap : 0);
        smax += z + (n ? gap : 0);
        if (a > bmin) bmin = a;
        if (z > bmax) bmax = z;
        n++;
    }
    if (row) { *mn = wrap ? bmin : smin; *mx = smax; }
    else { *mn = bmin; *mx = bmax; }
}
