/*
 * Painting: the display list of a rectangle of the page in the painting order of CSS 2.1 Appendix E, simplified:
 *
 *   a stacking context paints its own background and border, then (inside its clip when it scrolls) the stacking
 *   contexts with negative z-index, the backgrounds of its in-flow block descendants in tree order, its floats (each
 *   painted like a stacking context of its own), its inline content line by line (inline box backgrounds, text, atomic
 *   inlines painted atomically, list markers), its positioned descendants with z-index auto or 0 in tree order, and the
 *   ones with positive z-index.
 *
 * Positioned boxes with z-index auto are painted as if they made a stacking context (their positioned descendants do
 * not interleave with the outer ones), opacity groups whole stacking contexts, overflow other than visible clips (and
 * scrolls) the content. Backgrounds: colour, a linear gradient approximated by bands, one image with size, position
 * and repeat. Borders keep their per-side widths, colours, styles and radii for the painter. Text runs carry the font
 * handle the text was measured with. Coordinates are device pixels relative to the rectangle; text origins are in 1/64
 * device pixels.
 */
#include "layout.h"

#include <math.h>
#include <stdlib.h>

typedef struct P {
    WebDoc *d;
    LayoutState *st;
    WebArena *a;
    VEC(WebDisplayItem) items;
    VEC(PaintHit) hits;
    bool hit;
    int scale;
    WebUnit ox, oy;                /* the page point at the list's origin */
    WebUnit cx0, cy0, cx1, cy1;    /* what is not culled */
    WebUnit fx, fy;                /* how far fixed boxes move */
    int depth;
} P;

enum { PH_BLOCKS, PH_FLOATS, PH_INLINE };

static int dev(const P *p, WebUnit u) {
    int64_t v = (int64_t)u * p->scale;
    return (int)(v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64));
}

static int dwidth(const P *p, WebUnit u) {
    if (u <= 0) return 0;
    int v = dev(p, u);
    return v < 1 ? 1 : v;
}

static WebColor resolve_color(WebColor c, const CStyle *s) { return c == COLOR_CURRENT ? s->inh->color : c; }

static WebDisplayItem *item(P *p, int kind, WebUnit x, WebUnit y, WebUnit w, WebUnit h, WebNode *node, LBox *box, const Frag *f,
                            LBox *container) {
    bool group = kind == WEB_DI_CLIP_PUSH || kind == WEB_DI_CLIP_POP || kind == WEB_DI_OPACITY_PUSH || kind == WEB_DI_OPACITY_POP ||
                 kind == WEB_DI_SCROLL_PUSH || kind == WEB_DI_SCROLL_POP;
    if (!group && (w <= 0 || h <= 0 || x >= p->cx1 || y >= p->cy1 || x + w <= p->cx0 || y + h <= p->cy0)) return NULL;
    WebDisplayItem it;
    memset(&it, 0, sizeof it);
    it.kind = (uint8_t)kind;
    int x0 = dev(p, x - p->ox), y0 = dev(p, y - p->oy), x1 = dev(p, x + w - p->ox), y1 = dev(p, y + h - p->oy);
    it.rect.x = x0;
    it.rect.y = y0;
    it.rect.w = x1 - x0;
    it.rect.h = y1 - y0;
    it.node = node;
    if (!vec_push(p->a, p->items, it)) return NULL;
    if (p->hit) {
        PaintHit ph = { box, f, container };
        vec_push(p->a, p->hits, ph);
    }
    return &p->items.v[p->items.n - 1];
}

static void radii(const P *p, const LBox *b, uint16_t out[4]) {
    for (int i = 0; i < 4; i++) {
        WebUnit r = resolve_len_or(&b->s->vis->radius[i], b->w, 0);
        int v = dev(p, r);
        out[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
    }
}

static bool hidden(const LBox *b) { return b->s && b->s->inh->visibility != 0; }

static bool positioned(const LBox *b) {
    return b->s && b->kind != BK_VIEWPORT && b->kind != BK_TEXT && b->kind != BK_INLINE && b->s->box->position != POS_STATIC;
}

static bool makes_sc(const LBox *b) {
    if (!b->s || b->kind == BK_TEXT || b->kind == BK_INLINE || b->kind == BK_VIEWPORT) return false;
    const CBox *bx = b->s->box;
    if (bx->position != POS_STATIC && !bx->z_auto) return true;
    if (bx->position == POS_FIXED || bx->position == POS_STICKY) return true;
    if (b->s->vis->opacity < 1) return true;
    if (!bx->z_auto && b->parent && (b->parent->kind == BK_FLEX || b->parent->kind == BK_GRID)) return true;
    return false;
}

static bool layered(const LBox *b) { return positioned(b) || makes_sc(b); }

static bool atomic_inline(const LBox *b) {
    return (b->flags & BF_INLINE_LEVEL) && b->kind != BK_INLINE && b->kind != BK_TEXT && b->kind != BK_BR;
}

static int zof(const LBox *b) { return b->s->box->z_auto ? 0 : b->s->box->z_index; }

/* the relative offsets of the inline boxes between a fragment's box and its container */
static void inline_rel(const LBox *b, const LBox *container, WebUnit *x, WebUnit *y) {
    WebUnit ox = 0, oy = 0;
    int guard = 0;
    for (const LBox *k = b; k && k != container && guard < 64; k = k->parent, guard++)
        if (k->kind == BK_INLINE) { ox += k->rel_x; oy += k->rel_y; }
    *x = ox;
    *y = oy;
}

/* page positions of every box, with the scroll offsets of scroll containers and the fixed boxes' move */
static void place(P *p, LBox *b, int depth) {
    if (depth > 700) return;
    for (LBox *c = b->first; c; c = c->next) {
        LBox *r = c->ref;
        if (r) {
            c->ax = r->ax + c->x + c->rel_x;
            c->ay = r->ay + c->y + c->rel_y;
            if (r->flags & BF_SCROLL) { c->ax -= r->sx; c->ay -= r->sy; }
            if (c->parent && c->parent->kind == BK_INLINE) {
                WebUnit rx, ry;
                inline_rel(c->parent, r, &rx, &ry);
                c->ax += rx;
                c->ay += ry;
            }
        } else {
            c->ax = b->ax;
            c->ay = b->ay;
        }
        if (c->flags & BF_FIXED) { c->ax += p->fx; c->ay += p->fy; }
        place(p, c, depth + 1);
    }
}

/* ================================================================================================ boxes */

static void shadow(P *p, LBox *b) {
    const Shadow *sh = b->s->vis->shadow;
    if (!sh || sh->inset) return;
    WebUnit sp = to_unit(sh->spread);
    WebDisplayItem *it = item(p, WEB_DI_SHADOW, b->ax + to_unit(sh->x) - sp, b->ay + to_unit(sh->y) - sp, b->w + 2 * sp, b->h + 2 * sp,
                              b->node, b, NULL, NULL);
    if (!it) return;
    it->color = resolve_color(sh->color, b->s);
    it->width = dev(p, to_unit(sh->blur));
    radii(p, b, it->radii);
}

static WebColor lerp(WebColor a, WebColor b, float t) {
    WebColor out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float va = (float)((a >> sh) & 255), vb = (float)((b >> sh) & 255);
        uint32_t v = (uint32_t)(va + (vb - va) * t + 0.5f);
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

static void bg_image(P *p, LBox *b, const char *url, WebUnit ax, WebUnit ay, WebUnit aw, WebUnit ah) {
    const CVis *v = b->s->vis;
    int iw, ih;
    if (!image_ready_size(p->d, url, &iw, &ih)) return;
    WebImageHandle h = image_handle(p->d, url);
    /* the positioning area: border, padding or content box */
    WebUnit px = b->ax, py = b->ay, pw = b->w, ph = b->h;
    if (v->bg_origin >= 1) { px += b->b[3]; py += b->b[0]; pw -= b->b[1] + b->b[3]; ph -= b->b[0] + b->b[2]; }
    if (v->bg_origin == 2) { px += b->p[3]; py += b->p[0]; pw -= b->p[1] + b->p[3]; ph -= b->p[0] + b->p[2]; }
    WebUnit tw = to_unit((float)iw), th = to_unit((float)ih);
    if (v->bg_size == BGS_COVER || v->bg_size == BGS_CONTAIN) {
        double sx = (double)pw / tw, sy = (double)ph / th;
        double s = v->bg_size == BGS_COVER ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        tw = (WebUnit)(tw * s);
        th = (WebUnit)(th * s);
    } else if (v->bg_size == BGS_LEN) {
        bool wa, ha;
        WebUnit w = resolve_len(&v->bg_w, pw, &wa), hh = resolve_len(&v->bg_h, ph, &ha);
        if (!wa && !ha) { tw = w; th = hh; }
        else if (!wa) { th = iw ? (WebUnit)((int64_t)w * ih / iw) : th; tw = w; }
        else if (!ha) { tw = ih ? (WebUnit)((int64_t)hh * iw / ih) : tw; th = hh; }
    }
    if (tw <= 0 || th <= 0) return;
    WebUnit x0 = px + resolve_len_or(&v->bg_pos_x, pw - tw, 0), y0 = py + resolve_len_or(&v->bg_pos_y, ph - th, 0);
    bool rx = v->bg_repeat == BG_REPEAT || v->bg_repeat == BG_REPEAT_X || v->bg_repeat == BG_SPACE || v->bg_repeat == BG_ROUND;
    bool ry = v->bg_repeat == BG_REPEAT || v->bg_repeat == BG_REPEAT_Y || v->bg_repeat == BG_SPACE || v->bg_repeat == BG_ROUND;
    item(p, WEB_DI_CLIP_PUSH, ax, ay, aw, ah, NULL, NULL, NULL, NULL);
    WebUnit sx0 = x0, sy0 = y0;
    if (rx) while (sx0 > ax) sx0 -= tw;
    if (ry) while (sy0 > ay) sy0 -= th;
    int count = 0;
    for (WebUnit y = sy0; y < ay + ah && count < 4096; y += th) {
        for (WebUnit x = sx0; x < ax + aw && count < 4096; x += tw) {
            WebDisplayItem *it = item(p, WEB_DI_IMAGE, x, y, tw, th, b->node, b, NULL, NULL);
            if (it) it->image = h;
            count++;
            if (!rx) break;
        }
        if (!ry) break;
    }
    item(p, WEB_DI_CLIP_POP, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
}

static void background(P *p, LBox *b) {
    const CVis *v = b->s->vis;
    WebUnit x = b->ax, y = b->ay, w = b->w, h = b->h;
    if (v->bg_clip >= 1) { x += b->b[3]; y += b->b[0]; w -= b->b[1] + b->b[3]; h -= b->b[0] + b->b[2]; }
    if (v->bg_clip == 2) { x += b->p[3]; y += b->p[0]; w -= b->p[1] + b->p[3]; h -= b->p[0] + b->p[2]; }
    WebColor c = resolve_color(v->bg_color, b->s);
    if (c >> 24) {
        WebDisplayItem *it = item(p, WEB_DI_RECT, x, y, w, h, b->node, b, NULL, NULL);
        if (it) { it->color = c; radii(p, b, it->radii); }
    }
    if (v->bg_gradient[0] || v->bg_gradient[1]) {
        const int bands = 16;
        for (int i = 0; i < bands; i++) {
            WebUnit s0 = (v->bg_gradient_h ? w : h) * i / bands, s1 = (v->bg_gradient_h ? w : h) * (i + 1) / bands;
            WebDisplayItem *it = v->bg_gradient_h ? item(p, WEB_DI_RECT, x + s0, y, s1 - s0, h, b->node, b, NULL, NULL)
                                                  : item(p, WEB_DI_RECT, x, y + s0, w, s1 - s0, b->node, b, NULL, NULL);
            if (it) it->color = lerp(v->bg_gradient[0], v->bg_gradient[1], ((float)i + 0.5f) / (float)bands);
        }
    }
    if (v->bg_image) bg_image(p, b, v->bg_image, x, y, w, h);
}

static void border(P *p, LBox *b, WebUnit x, WebUnit y, WebUnit w, WebUnit h, bool left, bool right) {
    const CVis *v = b->s->vis;
    WebUnit bw[4] = { b->b[0], right ? b->b[1] : 0, b->b[2], left ? b->b[3] : 0 };
    bool any = false;
    for (int i = 0; i < 4; i++) if (bw[i] > 0 && v->border_style[i] != WEB_BORDER_NONE) any = true;
    if (!any) return;
    WebDisplayItem *it = item(p, WEB_DI_BORDER, x, y, w, h, b->node, b, NULL, NULL);
    if (!it) return;
    for (int i = 0; i < 4; i++) {
        bool on = bw[i] > 0 && v->border_style[i] != WEB_BORDER_NONE;
        it->widths[i] = (uint16_t)(on ? dwidth(p, bw[i]) : 0);
        it->styles[i] = on ? v->border_style[i] : WEB_BORDER_NONE;
        it->colors[i] = resolve_color(v->border_color[i], b->s);
    }
    radii(p, b, it->radii);
}

static void outline(P *p, LBox *b) {
    const CVis *v = b->s->vis;
    if (v->outline_style == WEB_BORDER_NONE || v->outline_w <= 0) return;
    WebUnit ow = to_unit(v->outline_w), off = to_unit(v->outline_offset);
    WebDisplayItem *it = item(p, WEB_DI_BORDER, b->ax - off - ow, b->ay - off - ow, b->w + 2 * (off + ow), b->h + 2 * (off + ow),
                              b->node, b, NULL, NULL);
    if (!it) return;
    for (int i = 0; i < 4; i++) {
        it->widths[i] = (uint16_t)dwidth(p, ow);
        it->styles[i] = v->outline_style;
        it->colors[i] = resolve_color(v->outline_color, b->s);
    }
}

static FontRef *font_of(P *p, const CStyle *s) { return font_get(p->d, s->inh); }

static void text_item(P *p, const CStyle *s, const char *t, uint32_t n, WebUnit x, WebUnit baseline, WebUnit w, WebNode *node,
                      LBox *box, const Frag *f, LBox *container) {
    FontRef *font = font_of(p, s);
    if (!font || !n) return;
    WebUnit asc = font->m.ascent, desc = font->m.descent;
    WebDisplayItem *it = item(p, WEB_DI_TEXT, x, baseline - asc, w > 0 ? w : 1, asc + desc, node, box, f, container);
    if (!it) return;
    it->font = font->h;
    it->color = s->inh->color;
    it->text = t;
    it->len = n;
    it->x64 = (int32_t)((int64_t)(x - p->ox) * p->scale);
    it->y64 = (int32_t)((int64_t)(baseline - p->oy) * p->scale);
}

/* text-decoration lines reach text through its inline ancestors and the blocks around it, but not into floats,
 * positioned boxes or atomic inlines (CSS Text Decoration 3, 2.1) */
static void deco_lines(const LBox *t, int *lines, WebColor col[3]) {
    *lines = 0;
    int guard = 0;
    for (const LBox *k = t->parent; k && guard < 200; k = k->parent, guard++) {
        if (!k->s || k->kind == BK_VIEWPORT) break;
        int dl = k->s->vis->deco_line;
        for (int i = 0; i < 3; i++)
            if ((dl & (1 << i)) && !(*lines & (1 << i))) {
                *lines |= 1 << i;
                col[i] = resolve_color(k->s->vis->deco_color, k->s);
            }
        if ((k->flags & (BF_FLOAT | BF_ABS)) || atomic_inline(k) || k->kind == BK_CELL) break;
    }
}

static void text_frag(P *p, LBox *c, const Frag *f) {
    LBox *t = f->box;
    if (hidden(t)) return;
    WebUnit rx, ry;
    inline_rel(t->parent, c, &rx, &ry);
    WebUnit x = c->ax + f->x + rx, base = c->ay + f->baseline + ry;
    WebNode *node = t->node;
    const CStyle *s = t->s;
    /* the selection under the text */
    uint32_t s0, s1;
    if (sel_text_range(p->d, t, &s0, &s1)) {
        uint32_t a = s0 > f->start ? s0 : f->start, e = s1 < f->start + f->len ? s1 : f->start + f->len;
        if (a < e) {
            FontRef *font = font_of(p, s);
            WebUnit xa = font ? font_measure(p->d, font, t->text + f->start, a - f->start) : 0;
            WebUnit xe = font ? font_measure(p->d, font, t->text + f->start, e - f->start) : 0;
            WebDisplayItem *it = item(p, WEB_DI_RECT, x + xa, c->ay + f->y + ry, xe - xa, f->h, NULL, NULL, NULL, NULL);
            if (it) it->color = 0x803390FFu;
        }
    }
    int lines;
    WebColor col[3];
    deco_lines(t, &lines, col);
    FontRef *font = font_of(p, s);
    if (lines && font) {
        WebUnit th = font->m.underline_thickness > 0 ? font->m.underline_thickness : 64;
        if (lines & DECO_UNDERLINE) {
            WebDisplayItem *it = item(p, WEB_DI_RECT, x, base + font->m.underline_offset, f->w, th, NULL, NULL, NULL, NULL);
            if (it) it->color = col[0];
        }
        if (lines & DECO_OVERLINE) {
            WebDisplayItem *it = item(p, WEB_DI_RECT, x, base - font->m.ascent, f->w, th, NULL, NULL, NULL, NULL);
            if (it) it->color = col[1];
        }
    }
    float ls = s->inh->letter_spacing;
    if (ls != 0 && font) {
        /* letter-spacing: one run per character, as measured */
        WebUnit cx = x;
        for (uint32_t i = 0; i < f->len;) {
            size_t len;
            utf8_decode(t->text + f->start + i, f->len - i, &len);
            WebUnit w = font_measure(p->d, font, t->text + f->start + i, len);
            text_item(p, s, t->text + f->start + i, (uint32_t)len, cx, base, w, node, t, f, c);
            cx += w + to_unit(ls) + (t->text[f->start + i] == ' ' ? to_unit(s->inh->word_spacing) : 0);
            i += (uint32_t)len;
        }
    } else text_item(p, s, t->text + f->start, f->len, x, base, f->w, node, t, f, c);
    if (f->flags & FRF_HYPHEN && font) {
        WebUnit hw = font_measure(p->d, font, "-", 1);
        text_item(p, s, "-", 1, x + f->w - hw, base, hw, node, t, f, c);
    }
    if ((lines & DECO_LINE_THROUGH) && font) {
        WebUnit th = font->m.underline_thickness > 0 ? font->m.underline_thickness : 64;
        WebDisplayItem *it = item(p, WEB_DI_RECT, x, base - font->m.x_height / 2 - th / 2, f->w, th, NULL, NULL, NULL, NULL);
        if (it) it->color = col[2];
    }
}

/* a list marker: its text, or the bullet shape (disc, circle, square), or the list-style-image */
static void marker(P *p, LBox *li, WebUnit x, WebUnit base, WebUnit w) {
    const CStyle *s = li->node && is_el(li->node) && li->node->u.el.pstyle[2] ? li->node->u.el.pstyle[2] : li->s;
    if (hidden(li)) return;
    FontRef *f = font_of(p, s);
    if (!f) return;
    const char *img = s->inh->list_image;
    int iw, ih;
    if (img && image_ready_size(p->d, img, &iw, &ih)) {
        WebUnit tw = to_unit((float)iw), th = to_unit((float)ih);
        WebDisplayItem *it = item(p, WEB_DI_IMAGE, x + w - tw - f->m.space_advance, base - th, tw, th, li->node, li, NULL, NULL);
        if (it) it->image = image_handle(p->d, img);
        return;
    }
    if (li->marker) { text_item(p, s, li->marker, (uint32_t)strlen(li->marker), x, base, w, li->node, li, NULL, NULL); return; }
    int lt = li->marker_kind;
    if (lt == LT_NONE) return;
    if (lt == LT_DISCLOSURE_OPEN || lt == LT_DISCLOSURE_CLOSED) {
        const char *t = lt == LT_DISCLOSURE_OPEN ? "\xE2\x96\xBE" : "\xE2\x96\xB8";
        text_item(p, s, t, 3, x, base, w / 2, li->node, li, NULL, NULL);
        return;
    }
    WebUnit fs = to_unit(s->inh->font_size);
    WebUnit d = fs * 5 / 16;
    if (d < 192) d = 192;
    WebUnit adv = font_measure(p->d, f, "\xE2\x80\xA2", 3);
    WebUnit bx = x + (adv - d) / 2, by = base - f->m.x_height / 2 - d / 2;
    WebDisplayItem *it = item(p, lt == LT_SQUARE ? WEB_DI_RECT : WEB_DI_ELLIPSE, bx, by, d, d, li->node, li, NULL, NULL);
    if (!it) return;
    it->color = s->inh->color;
    if (lt == LT_CIRCLE) it->width = p->scale;
}

const char *control_text(WebDoc *d, const LBox *b, uint16_t *state, uint32_t *len) {
    WebNode *el = b->node;
    const char *t = "";
    uint16_t st = 0;
    if (!is_el(el)) { *len = 0; if (state) *state = 0; return ""; }
    if (dom_attr(el, AT_disabled)) st |= WEB_CTL_DISABLED;
    if (el->flags & NF_FOCUS) st |= WEB_CTL_FOCUSED;
    if (el->flags & NF_HOVER) st |= WEB_CTL_HOVER;
    if (el->flags & NF_ACTIVE) st |= WEB_CTL_ACTIVE;
    switch (b->control) {
    case WEB_CONTROL_TEXT: case WEB_CONTROL_TEXTAREA: {
        t = web_control_value(el);
        if (!t || !*t) {
            const char *ph = dom_attr_val(el, AT_placeholder);
            if (ph && *ph) { t = ph; st |= WEB_CTL_PLACEHOLDER; }
            else t = "";
        } else if (is_html(el, AT_input) && dom_attr_is(el, AT_type, "password")) {
            size_t n = 0;
            for (const char *q = t; *q; q++) if (((unsigned char)*q & 0xC0) != 0x80) n++;
            if (n > 256) n = 256;
            char *bullets = wa_alloc(&d->larena, n * 3 + 1);
            if (bullets) {
                for (size_t i = 0; i < n; i++) memcpy(bullets + i * 3, "\xE2\x80\xA2", 3);
                bullets[n * 3] = 0;
                t = bullets;
            }
        }
        break;
    }
    case WEB_CONTROL_BUTTON: {
        const char *v = dom_attr_val(el, AT_value);
        if (v) t = v;
        else if (dom_attr_is(el, AT_type, "reset")) t = "Reset";
        else if (dom_attr_is(el, AT_type, "submit")) t = "Submit";
        else t = "";
        break;
    }
    case WEB_CONTROL_SELECT: case WEB_CONTROL_LISTBOX: {
        const char *labels[512];
        int n = web_select_options(el, labels, 512), i = web_select_index(el);
        t = i >= 0 && i < n && i < 512 ? labels[i] : "";
        break;
    }
    case WEB_CONTROL_CHECKBOX: case WEB_CONTROL_RADIO: {
        bool control_checked(const WebNode *el);
        if (control_checked(el)) st |= WEB_CTL_CHECKED;
        break;
    }
    case WEB_CONTROL_FILE: t = "Choose File"; break;
    default: { const char *v = dom_attr_val(el, AT_value); t = v ? v : ""; break; }
    }
    if (state) *state = st;
    *len = (uint32_t)strlen(t);
    return t;
}

static void replaced_content(P *p, LBox *b) {
    WebUnit cx = b->ax + b->b[3] + b->p[3], cy = b->ay + b->b[0] + b->p[0];
    WebUnit cw = box_content_w(b), ch = box_content_h(b);
    if (b->flags & BF_CONTROL) {
        WebDisplayItem *it = item(p, WEB_DI_CONTROL, b->ax, b->ay, b->w, b->h, b->node, b, NULL, NULL);
        if (!it) return;
        uint16_t st;
        uint32_t len;
        const char *t = control_text(p->d, b, &st, &len);
        FontRef *f = font_of(p, b->s);
        WebDisplayItem *again = &p->items.v[p->items.n - 1];
        again->control = b->control;
        again->state = st;
        again->text = t;
        again->len = len;
        again->font = f ? f->h : 0;
        again->color = b->s->inh->color;
        if (b->control == WEB_CONTROL_PROGRESS || b->control == WEB_CONTROL_METER) {
            const char *v = dom_attr_val(b->node, AT_value), *mx = dom_attr_val(b->node, AT_max);
            double val = v ? atof(v) : 0, m = mx ? atof(mx) : 1;
            again->width = m > 0 ? (int)(val / m * 1000) : 0;      /* per mille */
        }
        return;
    }
    if (b->flags & BF_IMAGE_TEXT) {
        const char *alt = dom_attr_val(b->node, AT_alt);
        FontRef *f = font_of(p, b->s);
        if (alt && f) {
            item(p, WEB_DI_CLIP_PUSH, cx, cy, cw, ch, NULL, NULL, NULL, NULL);
            text_item(p, b->s, alt, (uint32_t)strlen(alt), cx, cy + f->m.ascent, cw, b->node, b, NULL, NULL);
            item(p, WEB_DI_CLIP_POP, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
        }
        return;
    }
    const char *src = (b->flags & BF_GENERATED) ? b->label : (b->node && (is_html(b->node, AT_img) || is_html(b->node, AT_input)))
                      ? dom_attr_val(b->node, AT_src) : NULL;
    if (!src) return;
    const char *url = doc_resolve_url(p->d, src, strlen(src), &p->d->larena);
    int iw, ih;
    if (!url || !image_ready_size(p->d, url, &iw, &ih)) return;
    WebImageHandle h = image_handle(p->d, url);
    /* object-fit */
    WebUnit dw = cw, dh = ch;
    int fit = b->s->box->object_fit;
    WebUnit nw = to_unit((float)iw), nh = to_unit((float)ih);
    if (fit && nw > 0 && nh > 0) {
        double sx = (double)cw / nw, sy = (double)ch / nh, s = 1;
        if (fit == 1) s = sx < sy ? sx : sy;                          /* contain */
        else if (fit == 2) s = sx > sy ? sx : sy;                     /* cover */
        else if (fit == 4) { s = sx < sy ? sx : sy; if (s > 1) s = 1; }   /* scale-down */
        dw = (WebUnit)(nw * s);
        dh = (WebUnit)(nh * s);
    }
    bool clip = dw > cw || dh > ch;
    if (clip) item(p, WEB_DI_CLIP_PUSH, cx, cy, cw, ch, NULL, NULL, NULL, NULL);
    WebDisplayItem *it = item(p, WEB_DI_IMAGE, cx + (cw - dw) / 2, cy + (ch - dh) / 2, dw, dh, b->node, b, NULL, NULL);
    if (it) it->image = h;
    if (clip) item(p, WEB_DI_CLIP_POP, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
}

static WebUnit marker_w(P *p, const LBox *li) {
    const CStyle *s = li->node && is_el(li->node) && li->node->u.el.pstyle[2] ? li->node->u.el.pstyle[2] : li->s;
    FontRef *f = font_of(p, s);
    if (!f) return 0;
    return li->marker ? font_measure(p->d, f, li->marker, strlen(li->marker)) : font_measure(p->d, f, "\xE2\x80\xA2 ", 4);
}

/* a box's own painting: shadow, background, border, outline, replaced content, an unplaced list marker */
static void decor(P *p, LBox *b) {
    if (b->kind == BK_TABLE_WRAPPER || b->kind == BK_VIEWPORT || b->kind == BK_TEXT || b->kind == BK_INLINE) return;
    if (hidden(b)) return;
    if (p->hit && !b->s->inh->pointer_events) item(p, WEB_DI_RECT, b->ax, b->ay, b->w, b->h, b->node, b, NULL, NULL);
    if (b->kind == BK_CELL && b->s->inh->empty_cells && !b->first) return;
    bool canvas = b->node && b->node == p->st->canvas_from;
    if (!canvas) {
        shadow(p, b);
        background(p, b);
    }
    border(p, b, b->ax, b->ay, b->w, b->h, true, true);
    outline(p, b);
    if (b->kind == BK_REPLACED) replaced_content(p, b);
    if (b->s->display == D_LIST_ITEM && b->s->inh->list_pos && !(b->flags & BF_MARKER_DONE) && (b->marker || b->marker_kind != LT_NONE)) {
        FontRef *f = font_of(p, b->s);
        WebUnit mw = marker_w(p, b);
        marker(p, b, b->ax + b->b[3] + b->p[3] - mw, b->ay + b->b[0] + b->p[0] + (f ? f->m.ascent : 0), mw);
    }
}

static void paint_sc(P *p, LBox *root);

static void lines(P *p, LBox *c) {
    for (uint32_t i = 0; i < c->nfrags; i++) {
        const Frag *f = &c->frags[i];
        switch (f->kind) {
        case FR_BOX: {
            LBox *ib = f->box;
            if (hidden(ib)) break;
            WebUnit rx, ry;
            inline_rel(ib, c, &rx, &ry);
            WebUnit x = c->ax + f->x + rx, y = c->ay + f->y + ry;
            if (p->hit && !ib->s->inh->pointer_events) item(p, WEB_DI_RECT, x, y, f->w, f->h, ib->node, ib, f, c);
            const CVis *v = ib->s->vis;
            WebColor bg = resolve_color(v->bg_color, ib->s);
            if (bg >> 24) {
                WebDisplayItem *it = item(p, WEB_DI_RECT, x, y, f->w, f->h, ib->node, ib, f, c);
                if (it) it->color = bg;
            }
            border(p, ib, x, y, f->w, f->h, (f->flags & FRF_FIRST) != 0, (f->flags & FRF_LAST) != 0);
            break;
        }
        case FR_TEXT:
            if (f->len) text_frag(p, c, f);
            break;
        case FR_ATOMIC:
            if (!layered(f->box)) paint_sc(p, f->box);
            break;
        case FR_MARKER:
            marker(p, f->box, c->ax + f->x, c->ay + f->baseline, f->w);
            break;
        default:
            break;
        }
    }
}

static void clip_push(P *p, LBox *b) {
    WebUnit x = b->ax + b->b[3], y = b->ay + b->b[0], w = b->w - b->b[1] - b->b[3], h = b->h - b->b[0] - b->b[2];
    WebDisplayItem *it = item(p, WEB_DI_CLIP_PUSH, x, y, w, h, NULL, b, NULL, NULL);
    if (it) radii(p, b, it->radii);
    const CBox *bx = b->s->box;
    if (bx->overflow_x == OV_SCROLL || bx->overflow_x == OV_AUTO || bx->overflow_y == OV_SCROLL || bx->overflow_y == OV_AUTO) {
        WebDisplayItem *s = item(p, WEB_DI_SCROLL_PUSH, x, y, w, h, b->node, b, NULL, NULL);
        if (s) { s->sx = dev(p, b->sx); s->sy = dev(p, b->sy); }
    }
}

static void clip_pop(P *p, LBox *b) {
    const CBox *bx = b->s->box;
    if (bx->overflow_x == OV_SCROLL || bx->overflow_x == OV_AUTO || bx->overflow_y == OV_SCROLL || bx->overflow_y == OV_AUTO)
        item(p, WEB_DI_SCROLL_POP, 0, 0, 0, 0, NULL, b, NULL, NULL);
    item(p, WEB_DI_CLIP_POP, 0, 0, 0, 0, NULL, b, NULL, NULL);
}

static void walk(P *p, LBox *b, int phase, int depth) {
    if (depth > 600) return;
    for (LBox *c = b->first; c; c = c->next) {
        if (c->kind == BK_TEXT || c->kind == BK_BR || layered(c)) continue;
        if (c->flags & BF_FLOAT) { if (phase == PH_FLOATS) paint_sc(p, c); continue; }
        if (atomic_inline(c)) continue;
        if (c->kind == BK_INLINE) { if (phase == PH_FLOATS) walk(p, c, phase, depth + 1); continue; }
        if (phase == PH_BLOCKS) decor(p, c);
        if (phase == PH_INLINE && (c->flags & BF_INLINE_CONTENT)) lines(p, c);
        bool clip = (c->flags & BF_SCROLL) != 0;
        if (clip) clip_push(p, c);
        walk(p, c, phase, depth + 1);
        if (clip) clip_pop(p, c);
    }
}

typedef VEC(LBox *) BoxVec;

static void collect(P *p, LBox *b, BoxVec *neg, BoxVec *zero, BoxVec *pos, int depth) {
    if (depth > 600) return;
    for (LBox *c = b->first; c; c = c->next) {
        if (c->kind == BK_TEXT || c->kind == BK_BR) continue;
        if (layered(c)) {
            int z = makes_sc(c) ? zof(c) : 0;
            vec_push(p->a, *(z < 0 ? neg : z > 0 ? pos : zero), c);
            continue;
        }
        if ((c->flags & BF_FLOAT) || atomic_inline(c)) continue;
        collect(p, c, neg, zero, pos, depth + 1);
    }
}

static void sort_z(BoxVec *v) {
    for (uint32_t i = 1; i < v->n; i++) {
        LBox *t = v->v[i];
        uint32_t j = i;
        while (j > 0 && zof(v->v[j - 1]) > zof(t)) { v->v[j] = v->v[j - 1]; j--; }
        v->v[j] = t;
    }
}

static void paint_sc(P *p, LBox *root) {
    if (p->depth > 300) return;
    p->depth++;
    bool op = root->kind != BK_VIEWPORT && root->s && root->s->vis->opacity < 1;
    if (op) {
        WebDisplayItem *it = item(p, WEB_DI_OPACITY_PUSH, root->ax, root->ay, root->w, root->h, root->node, root, NULL, NULL);
        if (it) it->alpha = (uint8_t)(root->s->vis->opacity * 255 + 0.5f);
    }
    if (root->kind != BK_VIEWPORT) decor(p, root);
    BoxVec neg = { 0 }, zero = { 0 }, pos = { 0 };
    collect(p, root, &neg, &zero, &pos, 0);
    bool clip = root->kind != BK_VIEWPORT && (root->flags & BF_SCROLL);
    if (clip) clip_push(p, root);
    sort_z(&neg);
    sort_z(&pos);
    for (uint32_t i = 0; i < neg.n; i++) paint_sc(p, neg.v[i]);
    walk(p, root, PH_BLOCKS, 0);
    walk(p, root, PH_FLOATS, 0);
    if (root->flags & BF_INLINE_CONTENT) lines(p, root);
    walk(p, root, PH_INLINE, 0);
    for (uint32_t i = 0; i < zero.n; i++) paint_sc(p, zero.v[i]);
    for (uint32_t i = 0; i < pos.n; i++) paint_sc(p, pos.v[i]);
    if (clip) clip_pop(p, root);
    if (op) item(p, WEB_DI_OPACITY_POP, 0, 0, 0, 0, NULL, root, NULL, NULL);
    p->depth--;
}

void paint_collect(WebDoc *d, WebArena *a, WebRect r, WebUnit fx, WebUnit fy, bool hit, PaintOut *out) {
    memset(out, 0, sizeof *out);
    LayoutState *st = d->lay;
    if (!st || !st->root) return;
    P p;
    memset(&p, 0, sizeof p);
    p.d = d;
    p.st = st;
    p.a = a;
    p.hit = hit;
    p.scale = d->cfg.scale > 0 ? d->cfg.scale : 1;
    p.ox = r.x;
    p.oy = r.y;
    p.cx0 = r.x;
    p.cy0 = r.y;
    p.cx1 = r.x + r.w;
    p.cy1 = r.y + r.h;
    p.fx = fx;
    p.fy = fy;
    st->root->ax = st->root->ay = 0;
    place(&p, st->root, 0);
    paint_sc(&p, st->root);
    out->items = p.items.v;
    out->hits = p.hits.v;
    out->n = (int)p.items.n;
}

const WebDisplayList *web_doc_paint(WebDoc *d, int x, int y, int w, int h) {
    static WebDisplayList empty;
    if (!d) return &empty;
    if (d->style_dirty || d->layout_dirty || !d->lay) web_doc_layout(d, d->vw, d->vh);
    wa_reset(&d->parena);
    WebDisplayList *dl = wa_alloc(&d->parena, sizeof *dl);
    if (!dl) return &empty;
    WebRect r = { WEB_PX(x), WEB_PX(y), WEB_PX(w), WEB_PX(h) };
    PaintOut out;
    paint_collect(d, &d->parena, r, WEB_PX(x), WEB_PX(y), false, &out);
    dl->items = out.items;
    dl->n = out.n;
    dl->background = d->lay ? d->lay->canvas : 0xFFFFFFFFu;
    return dl;
}
