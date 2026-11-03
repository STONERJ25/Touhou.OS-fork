/*
 * Layout: the visual formatting model of CSS 2.1 (chapters 8 to 10) with the block formatting context, margin collapsing,
 * floats and clearance, positioning, the width and height computations of 10.3 and 10.6 (box-sizing, min/max, auto
 * margins, shrink-to-fit), replaced elements (intrinsic sizes, ratios, aspect-ratio) and form controls as boxes, and the
 * driver that rebuilds the box tree and lays it out for a viewport.
 *
 * Margin collapsing is decided ahead: before a child is placed, the margins that will collapse at its top (its own and,
 * through boxes without top border or padding, its first in-flow descendants') are added to the running "strut" of
 * adjoining margins, and the child is laid out knowing its first child's margin is already accounted for. A child's
 * bottom strut (its margin, and its last children's when nothing separates them) leaves it in bm_pos/bm_neg. Empty
 * blocks let the strut pass through them. Floats live in the BFC's list in BFC content coordinates; line boxes and
 * new formatting contexts are narrowed by them; clearance moves a box below them.
 */
#include "layout.h"

#include <math.h>
#include <stdlib.h>

LayoutState *layout_state(WebDoc *d) { return d->lay; }

/* ================================================================================================ lengths, edges */

WebUnit resolve_len(const CLen *c, WebUnit basis, bool *is_auto) {
    if (is_auto) *is_auto = false;
    switch (c->kind) {
    case CL_PX:
        if (c->pct != 0) {
            if (basis < 0) { if (is_auto) *is_auto = true; return 0; }
            return to_unit(c->px) + (WebUnit)llround((double)c->pct * basis / 100.0);
        }
        return to_unit(c->px);
    case CL_CALC:
        if (basis < 0) { if (is_auto) *is_auto = true; return 0; }
        return to_unit(clen_resolve(c, (float)basis / 64.0f, NULL));
    default:
        if (is_auto) *is_auto = true;
        return 0;
    }
}

WebUnit resolve_len_or(const CLen *c, WebUnit basis, WebUnit dflt) {
    bool a;
    WebUnit v = resolve_len(c, basis, &a);
    return a ? dflt : v;
}

static WebUnit border_px(float w) {
    if (w <= 0) return 0;
    float r = floorf(w);                                     /* whole pixels, but never less than one */
    if (r < 1) r = 1;
    return to_unit(r);
}

void layout_compute_box_edges(L *l, LBox *b, WebUnit cb_w) {
    if (b->flags & BF_KEEP_EDGES) return;
    const CBox *bx = b->s->box;
    const CVis *v = b->s->vis;
    bool no_margin = b->kind == BK_CELL || b->kind == BK_ROW || b->kind == BK_ROW_GROUP || b->kind == BK_COL ||
                     b->kind == BK_COLGROUP || b->kind == BK_TEXT || b->kind == BK_TABLE;
    bool no_padding = b->kind == BK_ROW || b->kind == BK_ROW_GROUP || b->kind == BK_COL || b->kind == BK_COLGROUP ||
                      b->kind == BK_TABLE_WRAPPER || b->kind == BK_TEXT;
    bool no_border = b->kind == BK_TABLE_WRAPPER || b->kind == BK_TEXT || b->kind == BK_ROW || b->kind == BK_ROW_GROUP ||
                     b->kind == BK_COL || b->kind == BK_COLGROUP;
    for (int i = 0; i < 4; i++) {
        b->m[i] = no_margin ? 0 : resolve_len_or(&bx->margin[i], cb_w < 0 ? 0 : cb_w, 0);
        WebUnit p = no_padding ? 0 : resolve_len_or(&bx->padding[i], cb_w < 0 ? 0 : cb_w, 0);
        b->p[i] = p < 0 ? 0 : p;
        b->b[i] = no_border ? 0 : border_px(v->border_w[i]);
    }
    if (b->kind == BK_TABLE && b->s->inh->border_collapse) {
        for (int i = 0; i < 4; i++) b->p[i] = 0;              /* the collapsing model has no table padding */
    }
    (void)l;
}

static WebUnit extra_w(const LBox *b) { return b->b[1] + b->b[3] + b->p[1] + b->p[3]; }
static WebUnit extra_h(const LBox *b) { return b->b[0] + b->b[2] + b->p[0] + b->p[2]; }
WebUnit box_content_w(const LBox *b) { WebUnit w = b->w - extra_w(b); return w < 0 ? 0 : w; }
WebUnit box_content_h(const LBox *b) { WebUnit h = b->h - extra_h(b); return h < 0 ? 0 : h; }

/* a specified size as a border-box size; false when auto (or a percentage of something indefinite) */
static bool spec_size(const LBox *b, const CLen *c, WebUnit basis, bool horiz, WebUnit *out, L *l) {
    if (c->kind == CL_MIN_CONTENT || c->kind == CL_MAX_CONTENT || c->kind == CL_FIT_CONTENT) {
        if (!horiz || !l) return false;
        WebUnit mn, mx;
        intrinsic_widths(l, (LBox *)b, &mn, &mx);
        WebUnit w = c->kind == CL_MIN_CONTENT ? mn : c->kind == CL_MAX_CONTENT ? mx : (basis >= 0 ? (mx < basis ? mx : (mn > basis ? mn : basis)) : mx);
        *out = w + extra_w(b);
        return true;
    }
    bool a;
    WebUnit v = resolve_len(c, basis, &a);
    if (a) return false;
    WebUnit ex = horiz ? extra_w(b) : extra_h(b);
    if (b->s->box->box_sizing) *out = v < ex ? ex : v;
    else *out = (v < 0 ? 0 : v) + ex;
    return true;
}

static WebUnit clamp_size(L *l, const LBox *b, WebUnit v, WebUnit basis, bool horiz) {
    const CBox *bx = b->s->box;
    WebUnit mx, mn;
    if ((horiz ? bx->max_w.kind : bx->max_h.kind) != CL_NONE && spec_size(b, horiz ? &bx->max_w : &bx->max_h, basis, horiz, &mx, l) && v > mx) v = mx;
    if (spec_size(b, horiz ? &bx->min_w : &bx->min_h, basis, horiz, &mn, l) && v < mn) v = mn;
    WebUnit ex = horiz ? extra_w(b) : extra_h(b);
    return v < ex ? ex : v;
}

bool box_spec_size(L *l, const LBox *b, const CLen *c, WebUnit basis, bool horiz, WebUnit *out) {
    return spec_size(b, c, basis, horiz, out, l);
}

WebUnit box_clamp(L *l, const LBox *b, WebUnit v, WebUnit basis, bool horiz) { return clamp_size(l, b, v, basis, horiz); }

FontRef *box_font(L *l, const CStyle *s) {
    return font_get(l->d, s->inh);
}

WebUnit line_height_of(L *l, const CStyle *s) {
    const CInh *i = s->inh;
    if (i->lh_kind == LH_PX) return to_unit(i->lh);
    if (i->lh_kind == LH_NUMBER) return to_unit(i->lh * i->font_size);
    FontRef *f = box_font(l, s);
    if (!f) return to_unit(i->font_size * 1.2f);
    return f->m.ascent + f->m.descent + f->m.line_gap;
}

/* ================================================================================================ positioning helpers */

void box_abs(const LBox *b, WebUnit *x, WebUnit *y) {
    WebUnit ax = 0, ay = 0;
    for (const LBox *k = b; k; k = k->ref) {
        ax += k->x + k->rel_x;
        ay += k->y + k->rel_y;
    }
    *x = ax;
    *y = ay;
}

/* the offset of box a's border box relative to box anc's border box (anc must be on a's reference chain) */
static void offset_to(const LBox *a, const LBox *anc, WebUnit *x, WebUnit *y) {
    WebUnit ox = 0, oy = 0;
    for (const LBox *k = a; k && k != anc; k = k->ref) {
        ox += k->x + k->rel_x;
        oy += k->y + k->rel_y;
    }
    *x = ox;
    *y = oy;
}

static bool on_chain(const LBox *a, const LBox *anc) {
    for (const LBox *k = a; k; k = k->ref) if (k == anc) return true;
    return false;
}

LBox *abs_containing_block(const LBox *b) {
    if (b->flags & BF_FIXED) {
        const LBox *k = b;
        while (k->parent) k = k->parent;
        return (LBox *)k;
    }
    for (LBox *p = b->parent; p; p = p->parent) {
        if (p->kind == BK_VIEWPORT) return p;
        if (p->kind != BK_INLINE && p->kind != BK_TEXT && p->s && p->s->box->position != POS_STATIC) return p;
    }
    return NULL;
}

void abs_defer(L *l, LBox *b) {
    PendingAbs pa = { b, abs_containing_block(b) };
    if (!pa.cb) return;
    vec_push(l->a, l->abs, pa);
}

void abs_run(L *l, LBox *cb) {
    /* boxes may defer more (nested positioned boxes): loop by index */
    for (uint32_t i = 0; i < l->abs.n; i++) {
        if (l->abs.v[i].cb != cb) continue;
        LBox *b = l->abs.v[i].box;
        l->abs.v[i].cb = NULL;
        layout_positioned(l, cb, b, b->x, b->y);
    }
}

/* ================================================================================================ floats */

void float_avail(const BFC *bfc, WebUnit y, WebUnit h, WebUnit x0, WebUnit x1, WebUnit *left, WebUnit *right) {
    WebUnit l = x0, r = x1;
    if (h <= 0) h = 1;
    for (uint32_t i = 0; bfc && i < bfc->floats.n; i++) {
        const Float *f = &bfc->floats.v[i];
        if (f->y + f->h <= y || f->y >= y + h || f->h <= 0) continue;
        if (f->side == 1) { if (f->x + f->w > l) l = f->x + f->w; }
        else if (f->x < r) r = f->x;
    }
    *left = l;
    *right = r;
}

WebUnit float_clear_y(const BFC *bfc, int side) {
    WebUnit y = INT32_MIN;
    for (uint32_t i = 0; bfc && i < bfc->floats.n; i++) {
        const Float *f = &bfc->floats.v[i];
        if ((side & f->side) && f->y + f->h > y) y = f->y + f->h;
    }
    return y;
}

WebUnit float_next_bottom(const BFC *bfc, WebUnit y, WebUnit h) {
    WebUnit best = INT32_MAX;
    for (uint32_t i = 0; i < bfc->floats.n; i++) {
        const Float *f = &bfc->floats.v[i];
        if (f->y + f->h <= y || f->y >= y + (h > 0 ? h : 1)) continue;
        if (f->y + f->h < best) best = f->y + f->h;
    }
    return best;
}

/* CSS 2.1 9.5.1: a float is laid out first (shrink-to-fit width in its containing block, its own formatting context),
 * then it goes as high as it can, at or below y (BFC coordinates; its containing block's content box spans x0..x1), not
 * above an earlier float, left or right as far as it fits. Its position is set relative to its parent's border box,
 * whose origin is (sp->bfc_x, sp->bfc_y) in BFC coordinates. */
void float_layout(L *l, LBox *f, const Space *sp, WebUnit cb_w) {
    Space fs = *sp;
    fs.shrink = true;
    fs.force_w = fs.force_h = -1;
    fs.cb_w = cb_w;
    fs.absorb_top = false;
    BFC inner;
    memset(&inner, 0, sizeof inner);
    inner.a = l->a;
    fs.bfc = &inner;
    fs.bfc_x = fs.bfc_y = 0;
    layout_box(l, f, &fs);
}

void float_position(L *l, LBox *f, const Space *sp, WebUnit y, WebUnit x0, WebUnit x1) {
    BFC *bfc = sp->bfc;
    (void)l;
    WebUnit mw = f->m[3] + f->w + f->m[1], mh = f->m[0] + f->h + f->m[2];
    int side = f->s->box->float_ == 2 ? 2 : 1;
    for (uint32_t i = 0; i < bfc->floats.n; i++)
        if (bfc->floats.v[i].y > y) y = bfc->floats.v[i].y;       /* not above an earlier float's top */
    for (int guard = 0; guard < 10000; guard++) {
        WebUnit lft, rgt;
        float_avail(bfc, y, mh, x0, x1, &lft, &rgt);
        bool fits = rgt - lft >= mw || (lft == x0 && rgt == x1);
        if (fits) {
            Float rec;
            rec.side = (uint8_t)side;
            rec.x = side == 1 ? lft : rgt - mw;
            rec.y = y;
            rec.w = mw;
            rec.h = mh;
            vec_push(bfc->a, bfc->floats, rec);
            f->x = rec.x + f->m[3] - sp->bfc_x;
            f->y = rec.y + f->m[0] - sp->bfc_y;
            return;
        }
        WebUnit nb = float_next_bottom(bfc, y, mh);
        if (nb == INT32_MAX || nb <= y) { y += 64; continue; }
        y = nb;
    }
}

void place_float(L *l, LBox *f, const Space *sp, WebUnit y, WebUnit x0, WebUnit x1) {
    float_layout(l, f, sp, x1 - x0);
    float_position(l, f, sp, y, x0, x1);
}

/* ================================================================================================ intrinsic sizes */

/* a child's contribution (margin box) to its parent's min/max-content width */
void box_contribution(L *l, LBox *c, WebUnit *mn, WebUnit *mx) {
    layout_compute_box_edges(l, c, 0);
    WebUnit m = c->m[1] + c->m[3];
    WebUnit sw;
    if (spec_size(c, &c->s->box->width, -1, true, &sw, l)) {
        sw = clamp_size(l, c, sw, -1, true);
        *mn = *mx = sw + m;
        return;
    }
    WebUnit a, b;
    intrinsic_widths(l, c, &a, &b);
    WebUnit ex = extra_w(c);
    *mn = clamp_size(l, c, a + ex, -1, true) + m;
    *mx = clamp_size(l, c, b + ex, -1, true) + m;
}

void intrinsic_widths(L *l, LBox *b, WebUnit *mn, WebUnit *mx) {
    if (b->intr_valid) { *mn = b->min_c; *mx = b->max_c; return; }
    WebUnit a = 0, z = 0;
    if (l->depth > 300) { *mn = *mx = 0; return; }
    l->depth++;
    switch (b->kind) {
    case BK_REPLACED: {
        layout_compute_box_edges(l, b, 0);
        layout_replaced_size(l, b, -1, -1);
        a = z = box_content_w(b);
        break;
    }
    case BK_FLEX: flex_intrinsic(l, b, &a, &z); break;
    case BK_GRID: grid_intrinsic(l, b, &a, &z); break;
    case BK_TABLE_WRAPPER: table_intrinsic(l, b, &a, &z); break;
    case BK_TEXT: case BK_BR: break;
    default:
        if (b->flags & BF_INLINE_CONTENT) inline_intrinsic(l, b, &a, &z);
        else {
            WebUnit fsum_max = 0;
            for (LBox *c = b->first; c; c = c->next) {
                if (c->flags & BF_ABS) continue;
                WebUnit cm, cx;
                box_contribution(l, c, &cm, &cx);
                if (cm > a) a = cm;
                if (c->flags & BF_FLOAT) { fsum_max += cx; if (fsum_max > z) z = fsum_max; continue; }
                fsum_max = 0;
                if (cx > z) z = cx;
            }
        }
        break;
    }
    l->depth--;
    if (z < a) z = a;
    b->min_c = a;
    b->max_c = z;
    b->intr_valid = true;
    *mn = a;
    *mx = z;
}

WebUnit shrink_to_fit(L *l, LBox *b, WebUnit avail) {
    WebUnit mn, mx;
    intrinsic_widths(l, b, &mn, &mx);
    WebUnit ex = extra_w(b);
    WebUnit av = avail - ex;
    WebUnit w = mx < av ? mx : av;
    if (w < mn) w = mn;
    return w + ex;
}

/* ================================================================================================ replaced elements */

static WebUnit avg_char(L *l, const CStyle *s) {
    FontRef *f = box_font(l, s);
    return f ? (f->m.zero_advance ? f->m.zero_advance : to_unit(s->inh->font_size / 2)) : to_unit(s->inh->font_size / 2);
}

static void control_intrinsic(L *l, LBox *b, WebUnit *w, WebUnit *h) {
    const CStyle *s = b->s;
    WebUnit lh = line_height_of(l, s);
    WebUnit ch = avg_char(l, s);
    FontRef *f = box_font(l, s);
    WebNode *el = b->node;
    switch (b->control) {
    case WEB_CONTROL_TEXT: {
        const char *sz = dom_attr_val(el, AT_size);
        int n = sz ? atoi(sz) : 20;
        if (n <= 0) n = 20;
        *w = ch * n;
        *h = lh;
        break;
    }
    case WEB_CONTROL_TEXTAREA: {
        const char *cs = dom_attr_val(el, AT_cols), *rs = dom_attr_val(el, AT_rows);
        int cols = cs ? atoi(cs) : 20, rows = rs ? atoi(rs) : 2;
        if (cols <= 0) cols = 20;
        if (rows <= 0) rows = 2;
        *w = ch * cols;
        *h = lh * rows;
        break;
    }
    case WEB_CONTROL_CHECKBOX: case WEB_CONTROL_RADIO: *w = *h = to_unit(13); break;
    case WEB_CONTROL_BUTTON: {
        uint32_t n;
        const char *lab = control_text(l->d, b, NULL, &n);
        *w = f ? font_measure(l->d, f, lab, n) : 0;
        *h = lh;
        break;
    }
    case WEB_CONTROL_SELECT: case WEB_CONTROL_LISTBOX: {
        const char *labels[256];
        int n = web_select_options(el, labels, 256);
        WebUnit mw = 0;
        for (int i = 0; i < n && i < 256; i++) {
            WebUnit tw = f ? font_measure(l->d, f, labels[i], strlen(labels[i])) : 0;
            if (tw > mw) mw = tw;
        }
        *w = mw + to_unit(b->control == WEB_CONTROL_SELECT ? 24 : 16);
        int rows = 1;
        if (b->control == WEB_CONTROL_LISTBOX) {
            const char *sz = dom_attr_val(el, AT_size);
            rows = sz ? atoi(sz) : 4;
            if (rows <= 1) rows = 4;
        }
        *h = lh * rows;
        break;
    }
    case WEB_CONTROL_RANGE: *w = to_unit(129); *h = to_unit(16); break;
    case WEB_CONTROL_COLOR: *w = to_unit(50); *h = to_unit(27); break;
    case WEB_CONTROL_FILE: *w = to_unit(250); *h = lh; break;
    case WEB_CONTROL_PROGRESS: *w = to_unit(160); *h = to_unit(16); break;
    case WEB_CONTROL_METER: *w = to_unit(80); *h = to_unit(16); break;
    default: *w = to_unit(100); *h = lh; break;
    }
}

static bool attr_num(const WebNode *el, WebAtom a, float *v) {
    const char *s = dom_attr_val(el, a);
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return false;
    *v = (float)atof(s);
    return !strchr(s, '%');
}

/* the intrinsic size and ratio of a replaced box */
static void replaced_intrinsic(L *l, LBox *b) {
    WebNode *el = b->node;
    b->iw = b->ih = 0;
    b->iratio = 0;
    if (b->flags & BF_CONTROL) {
        control_intrinsic(l, b, &b->iw, &b->ih);
        return;
    }
    const char *src = NULL;
    if (b->flags & BF_GENERATED) src = b->label;
    else if (is_html(el, AT_img) || is_html(el, AT_input)) src = dom_attr_val(el, AT_src);
    if (src || is_html(el, AT_img)) {
        int w = 0, h = 0;
        bool failed = false;
        const char *url = src ? doc_resolve_url(l->d, src, strlen(src), l->a) : NULL;
        if (url && image_size(l->d, el, url, &w, &h, &failed) && w > 0 && h > 0) {
            b->iw = to_unit((float)w);
            b->ih = to_unit((float)h);
            b->iratio = (float)w / (float)h;
        } else {
            float aw, ah;
            if (el && attr_num(el, AT_width, &aw) && attr_num(el, AT_height, &ah) && ah > 0) b->iratio = aw / ah;
            if ((failed || !url) && el && dom_attr_val(el, AT_alt) && *dom_attr_val(el, AT_alt)) b->flags |= BF_IMAGE_TEXT;
        }
        return;
    }
    if (el && el->ns == WEB_NS_SVG) {
        float w, h;
        b->iw = attr_num(el, AT_width, &w) ? to_unit(w) : to_unit(300);
        b->ih = attr_num(el, AT_height, &h) ? to_unit(h) : to_unit(150);
        b->iratio = b->ih ? (float)b->iw / (float)b->ih : 0;
        return;
    }
    b->iw = to_unit(300);
    b->ih = to_unit(150);
    b->iratio = 2;
}

/* CSS 2.1 10.3.2 and 10.6.2 with aspect-ratio: the content size from the specified sizes and the intrinsic ones */
void layout_replaced_size(L *l, LBox *b, WebUnit cb_w, WebUnit cb_h) {
    replaced_intrinsic(l, b);
    const CBox *bx = b->s->box;
    float ratio = bx->aspect > 0 ? bx->aspect : b->iratio;
    WebUnit w, h;
    bool hw = spec_size(b, &bx->width, cb_w, true, &w, l);
    bool hh = spec_size(b, &bx->height, cb_h, false, &h, l);
    if (hw) w -= extra_w(b);
    if (hh) h -= extra_h(b);
    if (b->flags & BF_IMAGE_TEXT) {
        /* an image that will not show: its alt text in a box (inline layout measures it) */
        const char *alt = dom_attr_val(b->node, AT_alt);
        FontRef *f = box_font(l, b->s);
        WebUnit tw = f && alt ? font_measure(l->d, f, alt, strlen(alt)) : 0;
        if (!hw) w = tw;
        if (!hh) h = line_height_of(l, b->s);
    } else if (!hw && !hh) {
        w = b->iw;
        h = b->ih;
        if (!w && !h && ratio > 0) { w = 0; h = 0; }
        else if (!w && ratio > 0) w = (WebUnit)(h * ratio);
        else if (!h && ratio > 0) h = (WebUnit)(w / ratio);
    } else if (!hw) {
        w = ratio > 0 ? (WebUnit)(h * ratio) : b->iw;
    } else if (!hh) {
        h = ratio > 0 ? (WebUnit)(w / ratio) : b->ih;
    }
    /* min/max, keeping the ratio when only one side was constrained */
    WebUnit bw = clamp_size(l, b, w + extra_w(b), cb_w, true) - extra_w(b);
    if (bw != w && !hh && ratio > 0) h = (WebUnit)(bw / ratio);
    w = bw;
    WebUnit bh = clamp_size(l, b, h + extra_h(b), cb_h, false) - extra_h(b);
    if (bh != h && !hw && ratio > 0) w = (WebUnit)(bh * ratio);
    h = bh;
    b->w = w + extra_w(b);
    b->h = h + extra_h(b);
    b->baseline = b->h + b->m[2];                            /* replaced: the bottom margin edge */
    b->has_baseline = true;
    if (b->flags & BF_CONTROL) {
        FontRef *f = box_font(l, b->s);
        if (f && b->control != WEB_CONTROL_CHECKBOX && b->control != WEB_CONTROL_RADIO && b->control != WEB_CONTROL_TEXTAREA &&
            b->control != WEB_CONTROL_LISTBOX) {
            /* text-like controls: the baseline of their one line of text */
            WebUnit lh = line_height_of(l, b->s);
            WebUnit ch = box_content_h(b);
            WebUnit half = (lh - (f->m.ascent + f->m.descent)) / 2;
            b->baseline = b->b[0] + b->p[0] + (ch - lh) / 2 + half + f->m.ascent;
        }
    }
}

/* ================================================================================================ block layout */

typedef struct Strut { WebUnit pos, neg; } Strut;

static void strut_add(Strut *s, WebUnit m) {
    if (m > s->pos) s->pos = m;
    if (m < s->neg) s->neg = m;
}

static WebUnit strut_val(const Strut *s) { return s->pos + s->neg; }

static bool out_of_flow(const LBox *b) { return (b->flags & (BF_FLOAT | BF_ABS)) != 0; }

/* does the box's top margin adjoin its first in-flow child's? */
static bool top_open(const LBox *b) {
    return b->kind == BK_BLOCK && !(b->flags & (BF_BFC | BF_INLINE_CONTENT)) && b->b[0] == 0 && b->p[0] == 0;
}

static WebUnit width_estimate(L *l, LBox *c, WebUnit cb_w) {
    WebUnit w;
    if (spec_size(c, &c->s->box->width, cb_w, true, &w, l)) return w - extra_w(c);
    return cb_w - c->m[1] - c->m[3] - extra_w(c);
}

/* the margins that collapse at the top of c: its own and those of its first in-flow descendants it lets through */
static void top_strut(L *l, LBox *c, WebUnit cb_w, Strut *st, int depth) {
    layout_compute_box_edges(l, c, cb_w);
    strut_add(st, c->m[0]);
    if (depth > 64 || !top_open(c)) return;
    WebUnit cw = width_estimate(l, c, cb_w);
    for (LBox *k = c->first; k; k = k->next) {
        if (out_of_flow(k)) continue;
        if (!box_is_block_level(k) || k->s->box->clear) return;
        top_strut(l, k, cw, st, depth + 1);
        return;
    }
}

static WebUnit text_align_center_offset(const LBox *parent, const LBox *c, WebUnit content_w) {
    /* -webkit-center (<center>, align=center): block children narrower than the line are centered */
    if (parent->s->inh->text_align != TA_WEBKIT_CENTER) return -1;
    const CBox *bx = c->s->box;
    if (bx->margin[1].kind == CL_AUTO || bx->margin[3].kind == CL_AUTO) return -1;
    WebUnit mw = c->m[3] + c->w + c->m[1];
    if (mw >= content_w) return -1;
    return (content_w - mw) / 2 + c->m[3];
}

/* CSS 2.1 10.3.3: auto horizontal margins of a block-level box in normal flow take what its width leaves */
static void auto_margins(LBox *c, WebUnit cw) {
    const CBox *bx = c->s->box;
    bool ml = bx->margin[3].kind == CL_AUTO, mr = bx->margin[1].kind == CL_AUTO;
    if (!ml && !mr) return;
    WebUnit rem = cw - c->w - (ml ? 0 : c->m[3]) - (mr ? 0 : c->m[1]);
    if (ml && mr) { c->m[3] = rem > 0 ? rem / 2 : 0; c->m[1] = rem - c->m[3]; }
    else if (ml) c->m[3] = rem;
    else c->m[1] = rem;
}

static void block_width(L *l, LBox *b, const Space *sp) {
    WebUnit cb_w = sp->cb_w < 0 ? 0 : sp->cb_w;
    if (sp->force_w >= 0) { b->w = sp->force_w < extra_w(b) ? extra_w(b) : sp->force_w; return; }
    const CBox *bx = b->s->box;
    bool ml_auto = bx->margin[3].kind == CL_AUTO, mr_auto = bx->margin[1].kind == CL_AUTO;
    WebUnit w;
    bool specified = spec_size(b, &bx->width, sp->cb_w, true, &w, l);
    if (!specified) {
        if (sp->shrink) w = shrink_to_fit(l, b, cb_w - b->m[1] - b->m[3]);
        else w = cb_w - b->m[3] - b->m[1];
    }
    w = clamp_size(l, b, w, sp->cb_w, true);
    if (!sp->shrink && !(b->flags & (BF_FLOAT | BF_ABS))) {
        WebUnit rem = cb_w - w - b->m[3] - b->m[1];
        if (ml_auto && mr_auto) { WebUnit h = rem > 0 ? rem / 2 : 0; b->m[3] = h; b->m[1] = rem - h; }
        else if (ml_auto) b->m[3] = rem;
        else if (mr_auto) b->m[1] = rem;
    }
    b->w = w;
}

/* the content height a box gives its children for percentages: definite, or -1 */
static WebUnit definite_content_h(L *l, LBox *b, const Space *sp) {
    if (sp->force_h >= 0) return sp->force_h - extra_h(b);
    WebUnit h;
    if (b->kind == BK_VIEWPORT) return box_content_h(b);
    if (!spec_size(b, &b->s->box->height, sp->cb_h, false, &h, l)) {
        /* the root element and the body's height: auto (the viewport only matters for "height: 100%" chains) */
        return -1;
    }
    h = clamp_size(l, b, h, sp->cb_h, false);
    return h - extra_h(b);
}

static void block_height(L *l, LBox *b, WebUnit content_h, const Space *sp) {
    if (sp->force_h >= 0) { b->h = sp->force_h; return; }
    WebUnit h;
    if (!spec_size(b, &b->s->box->height, sp->cb_h, false, &h, l)) h = content_h + extra_h(b);
    b->h = clamp_size(l, b, h, sp->cb_h, false);
}

/* the space a child of b gets, given where it is (its border-box origin relative to b) */
static void child_space(const LBox *b, const Space *sp, BFC *bfc, WebUnit cw, WebUnit ch, Space *out) {
    memset(out, 0, sizeof *out);
    out->cb_w = cw;
    out->cb_h = ch;
    out->bfc = bfc;
    out->force_w = out->force_h = -1;
    if (b->flags & BF_BFC) {
        out->bfc_x = -(b->b[3] + b->p[3]);                   /* the BFC origin is b's content box */
        out->bfc_y = -(b->b[0] + b->p[0]);
    } else {
        out->bfc_x = sp->bfc_x;
        out->bfc_y = sp->bfc_y;
    }
}

static bool is_empty_block(const LBox *b) {
    if (b->h != 0 || b->kind != BK_BLOCK || (b->flags & BF_BFC)) return false;
    if (b->flags & BF_INLINE_CONTENT) return b->nlines == 0;   /* white space alone makes no line boxes */
    for (const LBox *c = b->first; c; c = c->next)
        if (!out_of_flow(c) && !(c->flags & BF_EMPTY_COLLAPSE)) return false;
    return true;
}

/* CSS 2.1 9.4.1 and 8.3.1: the children of b stacked vertically, with their adjoining margins collapsed. y runs in b's
 * content box; st is the strut of margins waiting below the last placed box. */
static void block_flow(L *l, LBox *b, const Space *sp, BFC *bfc, WebUnit cw, WebUnit cbh, WebUnit *out_h) {
    WebUnit left = b->b[3] + b->p[3], top = b->b[0] + b->p[0];
    WebUnit y = 0;
    Strut st = { 0, 0 };
    bool first = true;
    /* the margins of the first child chain were already added above b (top_strut) when nothing separates them */
    bool absorb = top_open(b) && sp->absorb_top;
    Space cs;
    child_space(b, sp, bfc, cw, cbh, &cs);
    WebUnit base_x = cs.bfc_x, base_y = cs.bfc_y;           /* b's border-box origin in BFC coordinates */
    for (LBox *c = b->first; c; c = c->next) {
        if (c->flags & BF_ABS) {
            /* the static position: where its margin box would start if it were in the flow */
            c->ref = b;
            c->x = left;
            c->y = top + y + strut_val(&st);
            abs_defer(l, c);
            continue;
        }
        if (c->flags & BF_FLOAT) {
            c->ref = b;
            WebUnit fy = base_y + top + y + strut_val(&st);
            Space fs = cs;
            fs.bfc_x = base_x;
            fs.bfc_y = base_y;
            place_float(l, c, &fs, fy, base_x + left, base_x + left + cw);
            continue;
        }
        c->ref = b;
        Strut cst = st;
        bool absorbed = first && absorb;
        if (absorbed) { cst.pos = cst.neg = 0; layout_compute_box_edges(l, c, cw); }
        else top_strut(l, c, cw, &cst, 0);
        WebUnit cy = y + (absorbed ? 0 : strut_val(&cst));
        /* clearance: below the floats it must clear */
        if (c->s->box->clear) {
            WebUnit fb = float_clear_y(bfc, c->s->box->clear);
            if (fb != INT32_MIN) {
                WebUnit fb_local = fb - base_y - top;
                if (fb_local > cy) cy = fb_local;
            }
        }
        /* a box that makes a new formatting context does not overlap floats: beside them, or below */
        WebUnit cx = left + c->m[3];
        Space ks = cs;
        if ((c->flags & BF_BFC) && bfc->floats.n) {
            for (int guard = 0; guard < 1000; guard++) {
                WebUnit fl, fr;
                float_avail(bfc, base_y + top + cy, 64, base_x + left, base_x + left + cw, &fl, &fr);
                WebUnit avail = fr - fl;
                WebUnit w;
                bool fixed = spec_size(c, &c->s->box->width, cw, true, &w, l);
                if (fixed ? w + c->m[1] + c->m[3] <= avail : avail > 0) {
                    if (fl != base_x + left || fr != base_x + left + cw) {
                        cx = (fl - base_x) + c->m[3];
                        if (!fixed) ks.cb_w = avail;
                    }
                    break;
                }
                WebUnit nb = float_next_bottom(bfc, base_y + top + cy, 64);
                if (nb == INT32_MAX) break;
                cy = nb - base_y - top;
            }
        }
        bool beside = ks.cb_w != cw || cx != left + c->m[3];
        if (!beside && (c->kind == BK_BLOCK || c->kind == BK_FLEX || c->kind == BK_GRID)) {
            block_width(l, c, &ks);                          /* auto margins are known before the inside is laid out */
            cx = left + c->m[3];
        }
        c->x = cx;
        c->y = top + cy;
        ks.bfc_x = base_x + c->x;
        ks.bfc_y = base_y + c->y;
        ks.absorb_top = true;
        layout_box(l, c, &ks);
        if (!beside) { auto_margins(c, cw); c->x = left + c->m[3]; }
        WebUnit centered = text_align_center_offset(b, c, cw);
        if (centered >= 0) c->x = left + centered;
        if (is_empty_block(c)) {
            /* its top and bottom margins collapse through it, with the ones around it */
            c->flags |= BF_EMPTY_COLLAPSE;
            st = cst;
            strut_add(&st, c->bm_pos);
            strut_add(&st, c->bm_neg);
            continue;
        }
        y = cy + c->h;
        st.pos = c->bm_pos;
        st.neg = c->bm_neg;
        first = false;
    }
    bool bottom_open = b->kind == BK_BLOCK && !(b->flags & BF_BFC) && b->b[2] == 0 && b->p[2] == 0 &&
                       b->s->box->height.kind == CL_AUTO && sp->force_h < 0;
    WebUnit mnh;
    if (bottom_open && spec_size(b, &b->s->box->min_h, sp->cb_h, false, &mnh, l) && mnh > extra_h(b)) bottom_open = false;
    Strut out = { 0, 0 };
    if (bottom_open) {
        out = st;                                            /* the last margins leave through b's bottom */
        *out_h = y;
    } else {
        *out_h = y + strut_val(&st);
    }
    strut_add(&out, b->m[2]);
    b->bm_pos = out.pos;
    b->bm_neg = out.neg;
}

static WebUnit floats_bottom(const BFC *bfc) {
    WebUnit y = 0;
    for (uint32_t i = 0; i < bfc->floats.n; i++)
        if (bfc->floats.v[i].y + bfc->floats.v[i].h > y) y = bfc->floats.v[i].y + bfc->floats.v[i].h;
    return y;
}

/* the extent of the content (relative to b's padding box) for scroll containers */
static void overflow_extent(LBox *b, WebUnit *w, WebUnit *h) {
    WebUnit mw = box_content_w(b) + b->p[1] + b->p[3], mh = box_content_h(b) + b->p[0] + b->p[2];
    WebUnit ox = b->b[3], oy = b->b[0];
    for (LBox *c = b->first; c; c = c->next) {
        if (c->ref != b) continue;
        WebUnit r = c->x + c->w + c->m[1] - ox, d = c->y + c->h + c->m[2] - oy;
        if (r > mw) mw = r;
        if (d > mh) mh = d;
    }
    for (uint32_t i = 0; i < b->nfrags; i++) {
        WebUnit r = b->frags[i].x + b->frags[i].w - ox, d = b->frags[i].y + b->frags[i].h - oy;
        if (r > mw) mw = r;
        if (d > mh) mh = d;
    }
    *w = mw;
    *h = mh;
}

void layout_block_inside(L *l, LBox *b, const Space *sp) {
    WebUnit cw = box_content_w(b);
    WebUnit cbh = definite_content_h(l, b, sp);
    BFC own;
    BFC *bfc = sp->bfc;
    if (b->flags & BF_BFC || !bfc) {
        memset(&own, 0, sizeof own);
        own.a = l->a;
        bfc = &own;
    }
    WebUnit ch = 0;
    b->bm_pos = b->m[2] > 0 ? b->m[2] : 0;
    b->bm_neg = b->m[2] < 0 ? b->m[2] : 0;
    /* a list item with an outside marker: the marker goes beside the first line box inside it (CSS Lists 3) */
    LBox *outer_marker = l->marker;
    bool own_marker = b->s->display == D_LIST_ITEM && b->s->inh->list_pos && (b->marker || b->marker_kind != LT_NONE);
    if (own_marker) { b->flags &= ~(uint32_t)BF_MARKER_DONE; l->marker = b; }
    if (b->flags & BF_INLINE_CONTENT) {
        Space is;
        child_space(b, sp, bfc, cw, cbh, &is);
        layout_inline(l, b, &is);
        ch = b->nlines ? b->lines[b->nlines - 1].y + b->lines[b->nlines - 1].h - (b->b[0] + b->p[0]) : 0;
    } else {
        block_flow(l, b, sp, bfc, cw, cbh, &ch);
    }
    if (own_marker) l->marker = outer_marker;               /* not placed: painted at the item's first line position */
    if (bfc == &own) {
        /* a formatting context root contains its floats */
        WebUnit fb = floats_bottom(bfc);
        if (fb > ch) ch = fb;
    }
    block_height(l, b, ch, sp);
    if (b->flags & BF_SCROLL) {
        overflow_extent(b, &b->cw, &b->ch);
        if (b->node && b->node->type == WEB_NODE_ELEMENT) {
            WebUnit maxx = b->cw - (box_content_w(b) + b->p[1] + b->p[3]), maxy = b->ch - (box_content_h(b) + b->p[0] + b->p[2]);
            if (maxx < 0) maxx = 0;
            if (maxy < 0) maxy = 0;
            b->sx = b->node->u.el.scroll_x < maxx ? b->node->u.el.scroll_x : maxx;
            b->sy = b->node->u.el.scroll_y < maxy ? b->node->u.el.scroll_y : maxy;
            if (b->sx < 0) b->sx = 0;
            if (b->sy < 0) b->sy = 0;
        }
    }
    /* the baseline of a block container: its first line's, else its first in-flow child's (inline-blocks use the last) */
    if (!b->has_baseline) {
        for (LBox *c = b->first; c; c = c->next)
            if (!out_of_flow(c) && c->has_baseline && c->ref == b) {
                b->baseline = c->y + c->baseline;
                b->has_baseline = true;
                if (!(b->flags & BF_INLINE_LEVEL)) break;
            }
    }
}

void layout_relative(LBox *b, WebUnit cb_w, WebUnit cb_h) {
    const CBox *bx = b->s->box;
    b->rel_x = b->rel_y = 0;
    if (bx->position != POS_RELATIVE) return;                /* sticky: laid out as relative with no offset */
    bool la, ra, ta, ba;
    WebUnit lft = resolve_len(&bx->left, cb_w, &la), rgt = resolve_len(&bx->right, cb_w, &ra);
    WebUnit tp = resolve_len(&bx->top, cb_h, &ta), bt = resolve_len(&bx->bottom, cb_h, &ba);
    b->rel_x = !la ? lft : !ra ? -rgt : 0;
    b->rel_y = !ta ? tp : !ba ? -bt : 0;
}

void layout_box(L *l, LBox *b, const Space *sp) {
    if (l->depth > 350 || l->d->budget.exceeded) { b->w = b->h = 0; return; }
    l->depth++;
    b->has_baseline = false;
    b->nlines = b->nfrags = 0;
    layout_compute_box_edges(l, b, sp->cb_w);
    switch (b->kind) {
    case BK_REPLACED:
        if (sp->force_w >= 0 || sp->force_h >= 0) {
            layout_replaced_size(l, b, sp->cb_w, sp->cb_h);
            if (sp->force_w >= 0) b->w = sp->force_w;
            if (sp->force_h >= 0) b->h = sp->force_h;
        } else layout_replaced_size(l, b, sp->cb_w, sp->cb_h);
        b->bm_pos = b->m[2] > 0 ? b->m[2] : 0;
        b->bm_neg = b->m[2] < 0 ? b->m[2] : 0;
        break;
    case BK_FLEX:
        block_width(l, b, sp);
        layout_flex(l, b, sp);
        b->bm_pos = b->m[2] > 0 ? b->m[2] : 0;
        b->bm_neg = b->m[2] < 0 ? b->m[2] : 0;
        break;
    case BK_GRID:
        block_width(l, b, sp);
        layout_grid(l, b, sp);
        b->bm_pos = b->m[2] > 0 ? b->m[2] : 0;
        b->bm_neg = b->m[2] < 0 ? b->m[2] : 0;
        break;
    case BK_TABLE_WRAPPER:
        layout_table_wrapper(l, b, sp);
        b->bm_pos = b->m[2] > 0 ? b->m[2] : 0;
        b->bm_neg = b->m[2] < 0 ? b->m[2] : 0;
        break;
    default:
        block_width(l, b, sp);
        layout_block_inside(l, b, sp);
        break;
    }
    layout_relative(b, sp->cb_w, sp->cb_h);
    b->flags |= BF_LAID_OUT;
    if (b->s->box->position != POS_STATIC) abs_run(l, b);
    l->depth--;
}

/* ================================================================================================ positioned boxes */

/* CSS 2.1 10.3.7, 10.3.8, 10.6.4, 10.6.5: an absolutely positioned box in the padding box of its containing block cb.
 * (sx, sy) is its static position (the margin edge it would have in the flow) relative to its current reference box.
 * Afterwards its reference box is cb. */
void layout_positioned(L *l, LBox *cb, LBox *b, WebUnit sx, WebUnit sy) {
    WebUnit cbx0 = 0, cby0 = 0, cbw = cb->w, cbh = cb->h;
    if (cb->kind != BK_VIEWPORT) {
        cbx0 = cb->b[3];
        cby0 = cb->b[0];
        cbw = cb->w - cb->b[1] - cb->b[3];
        cbh = cb->h - cb->b[0] - cb->b[2];
    }
    WebUnit ox = 0, oy = 0;
    if (b->ref && b->ref != cb && on_chain(b->ref, cb)) offset_to(b->ref, cb, &ox, &oy);
    WebUnit stx = ox + sx - cbx0, sty = oy + sy - cby0;     /* the static position in the padding box */
    layout_compute_box_edges(l, b, cbw);
    const CBox *bx = b->s->box;
    bool la, ra, ta, ba;
    WebUnit lft = resolve_len(&bx->left, cbw, &la), rgt = resolve_len(&bx->right, cbw, &ra);
    WebUnit tp = resolve_len(&bx->top, cbh, &ta), bot = resolve_len(&bx->bottom, cbh, &ba);
    bool mla = bx->margin[3].kind == CL_AUTO, mra = bx->margin[1].kind == CL_AUTO;
    bool mta = bx->margin[0].kind == CL_AUTO, mba = bx->margin[2].kind == CL_AUTO;
    Space sp;
    memset(&sp, 0, sizeof sp);
    sp.cb_w = cbw;
    sp.cb_h = cbh;
    sp.force_w = sp.force_h = -1;
    BFC own;
    memset(&own, 0, sizeof own);
    own.a = l->a;
    sp.bfc = &own;
    bool replaced = b->kind == BK_REPLACED;
    /* the width */
    WebUnit w = 0;
    bool wauto;
    if (replaced) {
        layout_replaced_size(l, b, cbw, cbh);
        w = b->w;
        wauto = false;
    } else wauto = !spec_size(b, &bx->width, cbw, true, &w, l);
    if (la && ra) { lft = stx; la = false; ra = true; }      /* both auto: the static position decides */
    if (wauto) {
        if (!la && !ra) w = cbw - lft - rgt - b->m[3] - b->m[1];          /* solve for the width */
        else w = shrink_to_fit(l, b, la ? cbw - rgt - b->m[3] - b->m[1] : cbw - lft - b->m[3] - b->m[1]);
    }
    w = clamp_size(l, b, w, cbw, true);
    if (!la && !ra && !wauto && (mla || mra)) {
        WebUnit rem = cbw - lft - rgt - w;
        if (mla && mra) {
            if (rem >= 0) { b->m[3] = rem / 2; b->m[1] = rem - rem / 2; }
            else { b->m[3] = 0; b->m[1] = rem; }
        } else if (mla) b->m[3] = rem - b->m[1];
        else b->m[1] = rem - b->m[3];
    }
    if (la) lft = cbw - rgt - b->m[1] - w - b->m[3];
    /* the height: laid out at that width; top and bottom both set stretch an auto height */
    WebUnit hspec = 0;
    bool hauto = replaced || !spec_size(b, &bx->height, cbh, false, &hspec, l);
    if (!replaced) {
        sp.force_w = w;
        if (hauto && !ta && !ba) sp.force_h = clamp_size(l, b, cbh - tp - bot - b->m[0] - b->m[2], cbh, false);
        WebUnit keep[4];
        memcpy(keep, b->m, sizeof keep);                     /* layout_box recomputes the edges: keep the solved margins */
        layout_box(l, b, &sp);
        memcpy(b->m, keep, sizeof keep);
    }
    WebUnit h = b->h;
    if (ta && ba) { tp = sty; ta = false; }
    if (!ta && !ba && !hauto && (mta || mba)) {
        WebUnit rem = cbh - tp - bot - h;
        if (mta && mba) { b->m[0] = rem > 0 ? rem / 2 : 0; b->m[2] = rem - b->m[0]; }
        else if (mta) b->m[0] = rem - b->m[2];
        else b->m[2] = rem - b->m[0];
    }
    if (ta) tp = cbh - bot - b->m[2] - h - b->m[0];
    b->ref = cb;
    b->x = cbx0 + lft + b->m[3];
    b->y = cby0 + tp + b->m[0];
    b->rel_x = b->rel_y = 0;
}

/* ================================================================================================ the document */

static void doc_extent(const LBox *b, WebUnit ox, WebUnit oy, WebUnit *mw, WebUnit *mh, int depth) {
    if (depth > 400) return;
    WebUnit x = ox + b->x + b->rel_x, y = oy + b->y + b->rel_y;
    if (b->kind != BK_INLINE && b->kind != BK_TEXT) {
        if (x + b->w + (b->m[1] > 0 ? b->m[1] : 0) > *mw) *mw = x + b->w + (b->m[1] > 0 ? b->m[1] : 0);
        if (y + b->h + (b->m[2] > 0 ? b->m[2] : 0) > *mh) *mh = y + b->h + (b->m[2] > 0 ? b->m[2] : 0);
    }
    for (uint32_t i = 0; i < b->nfrags; i++) {
        if (x + b->frags[i].x + b->frags[i].w > *mw) *mw = x + b->frags[i].x + b->frags[i].w;
        if (y + b->frags[i].y + b->frags[i].h > *mh) *mh = y + b->frags[i].y + b->frags[i].h;
    }
    if (b->flags & BF_SCROLL) return;                         /* its content scrolls inside it */
    for (const LBox *c = b->first; c; c = c->next) {
        WebUnit cx = 0, cy = 0;
        /* children positioned relative to another box: find their absolute origin */
        if (c->ref == b) { cx = x; cy = y; }
        else if (c->ref) { box_abs(c->ref, &cx, &cy); }
        doc_extent(c, cx, cy, mw, mh, depth + 1);
    }
}

static WebNode *find_body(WebDoc *d) {
    WebNode *b = web_doc_body(d);
    return is_html(b, AT_body) ? b : NULL;
}

int web_doc_layout(WebDoc *d, int vw, int vh) {
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    bool size_changed = d->vw != vw || d->vh != vh || !d->lay;
    d->vw = vw;
    d->vh = vh;
    style_update(d);
    if (!d->layout_dirty && !size_changed) return web_doc_status(d);
    wa_reset(&d->larena);
    LayoutState *st = wa_alloc(&d->larena, sizeof *st);
    if (!st) return WEB_E_NOMEM;
    d->lay = st;
    L l;
    memset(&l, 0, sizeof l);
    l.d = d;
    l.st = st;
    l.a = &d->larena;
    st->vw = to_unit((float)vw);
    st->vh = to_unit((float)vh);
    LBox *vp = boxes_build(&l);
    st->root = vp;
    if (!vp) return WEB_E_NOMEM;
    vp->w = st->vw;
    vp->h = st->vh;
    BFC bfc;
    memset(&bfc, 0, sizeof bfc);
    bfc.a = l.a;
    Space sp;
    memset(&sp, 0, sizeof sp);
    sp.cb_w = st->vw;
    sp.cb_h = st->vh;
    sp.bfc = &bfc;
    sp.force_w = sp.force_h = -1;
    for (LBox *c = vp->first; c; c = c->next) {
        c->ref = vp;
        c->flags |= BF_BFC;                                  /* the root element: margins do not collapse through it */
        layout_compute_box_edges(&l, c, st->vw);
        c->x = c->m[3];
        c->y = c->m[0];
        sp.bfc_x = c->x;
        sp.bfc_y = c->y;
        layout_box(&l, c, &sp);
    }
    abs_run(&l, vp);
    /* positioned boxes whose containing block was never laid out (inline containers): use the viewport */
    for (uint32_t i = 0; i < l.abs.n; i++)
        if (l.abs.v[i].cb) { LBox *b = l.abs.v[i].box; l.abs.v[i].cb = NULL; layout_positioned(&l, vp, b, b->x, b->y); }
    WebUnit mw = st->vw, mh = st->vh;
    for (LBox *c = vp->first; c; c = c->next) doc_extent(c, 0, 0, &mw, &mh, 0);
    st->doc_w = mw;
    st->doc_h = mh;
    /* the canvas background: the root's, or the body's when the root has none (CSS 2.1 14.2) */
    WebNode *root = web_doc_element(d);
    st->canvas = 0xFFFFFFFFu;
    if (root && root->u.el.style) {
        WebColor c = root->u.el.style->vis->bg_color;
        if (c >> 24) { st->canvas = c; st->canvas_from = root; }
        else {
            WebNode *body = find_body(d);
            if (body && body->u.el.style && (body->u.el.style->vis->bg_color >> 24)) {
                st->canvas = body->u.el.style->vis->bg_color;
                st->canvas_from = body;
            }
        }
        if ((st->canvas >> 24) != 0xFF) {                     /* translucent: over white */
            uint32_t a = st->canvas >> 24;
            uint32_t r = ((st->canvas >> 16) & 255) * a / 255 + 255 * (255 - a) / 255;
            uint32_t g = ((st->canvas >> 8) & 255) * a / 255 + 255 * (255 - a) / 255;
            uint32_t bl = (st->canvas & 255) * a / 255 + 255 * (255 - a) / 255;
            st->canvas = 0xFF000000u | r << 16 | g << 8 | bl;
        }
    }
    d->layout_dirty = false;
    return web_doc_status(d);
}

void web_doc_content_size(WebDoc *d, int *w, int *h) {
    LayoutState *st = d->lay;
    *w = st ? (st->doc_w + 63) / 64 : 0;
    *h = st ? (st->doc_h + 63) / 64 : 0;
}

bool web_element_rect(WebNode *el, WebRect *r) {
    if (!is_el(el) || !el->box || !el->doc->lay) return false;
    LBox *b = el->box;
    if (b->kind == BK_INLINE) {
        /* an inline box: the union of its fragments */
        LBox *c = b->ref ? b->ref : b->parent;
        while (c && !(c->flags & BF_INLINE_CONTENT)) c = c->parent;
        if (!c) return false;
        WebUnit ax, ay, x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        box_abs(c, &ax, &ay);
        for (uint32_t i = 0; i < c->nfrags; i++) {
            Frag *f = &c->frags[i];
            if (f->box != b) continue;
            if (f->x < x0) x0 = f->x;
            if (f->y < y0) y0 = f->y;
            if (f->x + f->w > x1) x1 = f->x + f->w;
            if (f->y + f->h > y1) y1 = f->y + f->h;
        }
        if (x0 == INT32_MAX) return false;
        r->x = ax + x0;
        r->y = ay + y0;
        r->w = x1 - x0;
        r->h = y1 - y0;
        return true;
    }
    WebUnit ax, ay;
    box_abs(b, &ax, &ay);
    r->x = ax;
    r->y = ay;
    r->w = b->w;
    r->h = b->h;
    return true;
}
