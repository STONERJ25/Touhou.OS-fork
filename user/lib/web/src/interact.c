/*
 * Interaction: hit testing reads the display list of the point from the top (so it agrees with painting about what is
 * in front, clips included), then finds the link, the form control and the cursor (CSS UI 4 "cursor", auto resolved
 * as browsers do: a pointer over links, a text cursor over text). Text selection between two DOM points, the selected
 * text, find in page over the rendered text, scroll positions of the page and of scroll containers.
 */
#include "layout.h"

#include <stdlib.h>

typedef struct SelState { WebNode *an, *fn; int ao, fo; bool on; } SelState;

static void ensure_layout(WebDoc *d) {
    if (d->style_dirty || d->layout_dirty || !d->lay) web_doc_layout(d, d->vw, d->vh);
}

/* ================================================================================================ text offsets */

uint32_t text_src_offset(const LBox *t, uint32_t r) {
    if (!t->tmap) return r;
    if (!t->tlen) return 0;
    if (r >= t->tlen) return t->tmap[t->tlen - 1] + 1;
    return t->tmap[r];
}

uint32_t text_rendered_offset(const LBox *t, uint32_t src) {
    if (!t->tmap) return src < t->tlen ? src : t->tlen;
    uint32_t lo = 0, hi = t->tlen;                           /* the first rendered byte whose source is >= src */
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->tmap[mid] < src) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* ================================================================================================ selection */

static SelState *sel(WebDoc *d, bool create) {
    if (!d->sel && create) d->sel = wa_alloc(&d->arena, sizeof(SelState));
    return d->sel;
}

/* the selection's ends in tree order */
static bool sel_ends(WebDoc *d, WebNode **sn, int *so, WebNode **en, int *eo) {
    SelState *s = d->sel;
    if (!s || !s->on || !s->an || !s->fn) return false;
    int c = web_node_compare_position(s->an, s->fn);
    if (c < 0 || (c == 0 && s->ao <= s->fo)) { *sn = s->an; *so = s->ao; *en = s->fn; *eo = s->fo; }
    else { *sn = s->fn; *so = s->fo; *en = s->an; *eo = s->ao; }
    return true;
}

bool sel_text_range(WebDoc *d, const LBox *t, uint32_t *start, uint32_t *end) {
    WebNode *sn, *en;
    int so, eo;
    if (!d->sel || !t->node || t->node->type != WEB_NODE_TEXT || !sel_ends(d, &sn, &so, &en, &eo)) return false;
    WebNode *n = t->node;
    /* element ends stand for the position before their so-th child: the text node is inside when it follows them */
    if (n != sn && web_node_compare_position(n, sn) < 0 && !(sn->type == WEB_NODE_ELEMENT && web_node_contains(sn, n))) return false;
    if (n != en && web_node_compare_position(n, en) > 0) return false;
    if (n != en && en->type == WEB_NODE_ELEMENT && web_node_contains(en, n)) {
        int k = 0;
        WebNode *c = en->first;
        for (; c && k < eo; c = c->next, k++) if (web_node_contains(c, n)) break;
        if (!c || k >= eo) return false;
    }
    *start = n == sn ? text_rendered_offset(t, (uint32_t)(so < 0 ? 0 : so)) : 0;
    *end = n == en ? text_rendered_offset(t, (uint32_t)(eo < 0 ? 0 : eo)) : t->tlen;
    return *start < *end;
}

void web_doc_select(WebDoc *d, WebNode *a, int ao, WebNode *f, int fo) {
    SelState *s = sel(d, true);
    if (!s) return;
    s->an = a;
    s->ao = ao;
    s->fn = f;
    s->fo = fo;
    s->on = a && f;
}

void web_doc_select_none(WebDoc *d) {
    SelState *s = sel(d, false);
    if (s) s->on = false;
}

void web_doc_select_all(WebDoc *d) {
    WebNode *body = web_doc_body(d);
    if (!body) body = web_doc_element(d);
    if (!body) return;
    WebNode *first = NULL, *last = NULL;
    for (WebNode *n = body->first; n; n = dom_next(n, body))
        if (n->type == WEB_NODE_TEXT) { if (!first) first = n; last = n; }
    if (!first) return;
    web_doc_select(d, first, 0, last, (int)last->u.cd.len);
}

static bool is_block_display(const WebNode *el) {
    const CStyle *s = style_of(el);
    if (!s) return false;
    int d = s->display;
    return d == D_BLOCK || d == D_LIST_ITEM || d == D_TABLE || d == D_TABLE_ROW || d == D_FLEX || d == D_GRID || d == D_FLOW_ROOT ||
           d == D_TABLE_CAPTION;
}

char *web_doc_selection_text(WebDoc *d, size_t *len) {
    WebNode *sn, *en;
    int so, eo;
    WebBuf b;
    buf_init(&b, &d->arena);
    if (len) *len = 0;
    ensure_layout(d);
    if (sel_ends(d, &sn, &so, &en, &eo)) {
        WebNode *root = d->root;
        bool started = false, need_nl = false;
        for (WebNode *n = sn; n; n = dom_next(n, root)) {
            if (n->type == WEB_NODE_ELEMENT) {
                if (is_html(n, AT_br)) buf_putc(&b, '\n');
                else if (b.n && is_block_display(n)) need_nl = true;
            } else if (n->type == WEB_NODE_TEXT && n->box) {
                LBox *t = n->box;
                uint32_t s0 = 0, s1 = t->tlen;
                if (!sel_text_range(d, t, &s0, &s1)) { if (n == en) break; continue; }
                if (need_nl && started) buf_putc(&b, '\n');
                need_nl = false;
                buf_put(&b, t->text + s0, s1 - s0);
                started = true;
            }
            if (n == en) break;
        }
    }
    char *out = web_raw_alloc(&d->budget, b.n + 1);
    if (out) {
        if (b.n) memcpy(out, b.p, b.n);
        out[b.n] = 0;
        if (len) *len = b.n;
    }
    buf_free(&b);
    return out;
}

/* ================================================================================================ hit testing */

static int cursor_of(const WebNode *el, bool text, bool link) {
    const CStyle *s = el ? style_of(el) : NULL;
    int c = s ? s->inh->cursor : CUR_AUTO;
    switch (c) {
    case CUR_DEFAULT: return WEB_CURSOR_DEFAULT;
    case CUR_POINTER: return WEB_CURSOR_POINTER;
    case CUR_TEXT: return WEB_CURSOR_TEXT;
    case CUR_MOVE: return WEB_CURSOR_MOVE;
    case CUR_WAIT: return WEB_CURSOR_WAIT;
    case CUR_HELP: return WEB_CURSOR_HELP;
    case CUR_NOT_ALLOWED: return WEB_CURSOR_NOT_ALLOWED;
    case CUR_CROSSHAIR: return WEB_CURSOR_CROSSHAIR;
    case CUR_GRAB: return WEB_CURSOR_GRAB;
    default: return link ? WEB_CURSOR_POINTER : text ? WEB_CURSOR_TEXT : WEB_CURSOR_DEFAULT;
    }
}

static bool labelable(const WebNode *n) {
    return is_html(n, AT_input) || is_html(n, AT_button) || is_html(n, AT_select) || is_html(n, AT_textarea) ||
           is_html(n, AT_meter) || is_html(n, AT_progress) || is_html(n, AT_output);
}

static WebNode *control_for(WebDoc *d, WebNode *el) {
    for (WebNode *k = el; k; k = k->parent) {
        if (labelable(k)) return k;
        if (is_html(k, AT_label)) {
            const char *f = dom_attr_val(k, AT_for);
            if (f) return web_doc_get_element_by_id(d, f);
            for (WebNode *n = k->first; n; n = dom_next(n, k)) if (labelable(n)) return n;
            return NULL;
        }
    }
    return NULL;
}

typedef struct HitRes { int idx; PaintOut out; } HitRes;

/* the topmost item at a page point (layout units), within every clip around it */
static int top_item(const PaintOut *out) {
    bool inside[512];
    int depth = 0, best = -1;
    bool all = true;
    for (int i = 0; i < out->n; i++) {
        const WebDisplayItem *it = &out->items[i];
        bool contains = it->rect.x <= 0 && 0 < it->rect.x + it->rect.w && it->rect.y <= 0 && 0 < it->rect.y + it->rect.h;
        if (it->kind == WEB_DI_CLIP_PUSH) {
            if (depth < 512) inside[depth] = all;
            depth++;
            all = all && contains;
            continue;
        }
        if (it->kind == WEB_DI_CLIP_POP) {
            if (depth > 0) depth--;
            all = depth < 512 ? inside[depth] : all;
            continue;
        }
        if (it->kind == WEB_DI_OPACITY_PUSH || it->kind == WEB_DI_OPACITY_POP || it->kind == WEB_DI_SCROLL_PUSH ||
            it->kind == WEB_DI_SCROLL_POP) continue;
        if (all && contains && it->node) best = i;
    }
    return best;
}

static int caret_offset(WebDoc *d, const PaintHit *h, WebUnit px) {
    const Frag *f = h->frag;
    LBox *t = h->box;
    FontRef *font = font_get(d, t->s->inh);
    WebUnit x = px - (h->container->ax + f->x);
    uint32_t best = f->start;
    WebUnit prev = 0;
    for (uint32_t i = 0; i < f->len;) {
        size_t len;
        utf8_decode(t->text + f->start + i, f->len - i, &len);
        WebUnit w = font ? font_measure(d, font, t->text + f->start, i + (uint32_t)len) : 0;
        if (x < (prev + w) / 2) break;
        i += (uint32_t)len;
        best = f->start + i;
        prev = w;
    }
    return (int)text_src_offset(t, best);
}

bool web_doc_hit_test(WebDoc *d, int x, int y, WebHit *hit) {
    memset(hit, 0, sizeof *hit);
    ensure_layout(d);
    LayoutState *st = d->lay;
    if (!st || x < 0 || y < 0) return false;
    WebArena a;
    wa_init(&a, &d->budget);
    WebRect r = { WEB_PX(x), WEB_PX(y), 64, 64 };
    PaintOut out;
    paint_collect(d, &a, r, WEB_PX(d->scroll_x), WEB_PX(d->scroll_y), true, &out);
    int i = top_item(&out);
    bool ok = false;
    if (i >= 0) {
        const WebDisplayItem *it = &out.items[i];
        const PaintHit *h = &out.hits[i];
        hit->node = it->node;
        if (it->kind == WEB_DI_TEXT && h->frag && h->box && h->box->node && h->box->node->type == WEB_NODE_TEXT)
            hit->offset = caret_offset(d, h, WEB_PX(x));
        ok = true;
    } else if (x < (st->doc_w + 63) / 64 && y < (st->doc_h + 63) / 64) {
        hit->node = web_doc_body(d) ? web_doc_body(d) : web_doc_element(d);
        ok = hit->node != NULL;
    }
    if (ok && hit->node) {
        WebNode *el = hit->node->type == WEB_NODE_ELEMENT ? hit->node : hit->node->parent;
        while (el && el->type != WEB_NODE_ELEMENT) el = el->parent;
        hit->element = el;
        for (WebNode *k = el; k; k = k->parent)
            if ((is_html(k, AT_a) || is_html(k, AT_area)) && dom_attr(k, AT_href)) {
                hit->link = k;
                const char *href = dom_attr_val(k, AT_href);
                hit->href = doc_resolve_url(d, href, strlen(href), &d->larena);
                break;
            }
        hit->control = el ? control_for(d, el) : NULL;
        bool text = hit->node->type == WEB_NODE_TEXT || (hit->control && (is_html(hit->control, AT_textarea) ||
                    (is_html(hit->control, AT_input) && el && el->box && el->box->control == WEB_CONTROL_TEXT)));
        hit->cursor = (WebCursor)cursor_of(el, text, hit->link != NULL);
    }
    wa_destroy(&a);
    return ok;
}

/* ================================================================================================ find in page */

typedef struct TextRef { LBox *t; uint32_t at; } TextRef;
typedef VEC(TextRef) TextRefs;

static void flatten(LBox *b, WebBuf *buf, TextRefs *refs, WebArena *a, LBox **last_container, LBox *container, int depth) {
    if (depth > 600) return;
    for (LBox *c = b->first; c; c = c->next) {
        if (c->kind == BK_TEXT && (c->flags & BF_TEXT_READY)) {
            if (*last_container && *last_container != container) buf_putc(buf, '\n');
            *last_container = container;
            TextRef r = { c, buf->n };
            vec_push(a, *refs, r);
            buf_put(buf, c->text, c->tlen);
            continue;
        }
        flatten(c, buf, refs, a, last_container, (c->flags & BF_INLINE_CONTENT) ? c : container, depth + 1);
    }
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static const TextRef *ref_at(const TextRef *v, uint32_t n, uint32_t pos) {
    const TextRef *best = NULL;
    for (uint32_t i = 0; i < n; i++) { if (v[i].at <= pos) best = &v[i]; else break; }
    return best;
}

static LBox *container_of(LBox *t) {
    for (LBox *k = t->parent; k; k = k->parent) if (k->flags & BF_INLINE_CONTENT) return k;
    return NULL;
}

int web_doc_find(WebDoc *d, const char *needle, int which, WebRect *rect) {
    ensure_layout(d);
    if (rect) memset(rect, 0, sizeof *rect);
    LayoutState *st = d->lay;
    size_t nl = needle ? strlen(needle) : 0;
    if (!st || !st->root || !nl) return 0;
    WebArena a;
    wa_init(&a, &d->budget);
    WebBuf buf;
    buf_init(&buf, &a);
    TextRefs refs = { 0 };
    LBox *last = NULL;
    flatten(st->root, &buf, &refs, &a, &last, NULL, 0);
    int count = 0;
    for (uint32_t i = 0; i + nl <= buf.n;) {
        size_t k = 0;
        while (k < nl && lower(buf.p[i + k]) == lower(needle[k])) k++;
        if (k < nl) { i++; continue; }
        if (count == which) {
            const TextRef *s = ref_at(refs.v, refs.n, i), *e = ref_at(refs.v, refs.n, (uint32_t)(i + nl - 1));
            if (s && e) {
                uint32_t so = i - s->at, eo = (uint32_t)(i + nl) - e->at;
                if (eo > e->t->tlen) eo = e->t->tlen;
                WebNode *sn = s->t->node, *en = e->t->node;
                if (sn && en && sn->type == WEB_NODE_TEXT && en->type == WEB_NODE_TEXT)
                    web_doc_select(d, sn, (int)text_src_offset(s->t, so), en, (int)text_src_offset(e->t, eo));
                LBox *c = container_of(s->t);
                if (rect && c) {
                    WebUnit cx, cy;
                    box_abs(c, &cx, &cy);
                    for (uint32_t q = 0; q < c->nfrags; q++) {
                        const Frag *f = &c->frags[q];
                        if (f->kind != FR_TEXT || f->box != s->t || so < f->start || so > f->start + f->len) continue;
                        FontRef *font = font_get(d, s->t->s->inh);
                        WebUnit x0 = font ? font_measure(d, font, s->t->text + f->start, so - f->start) : 0;
                        uint32_t endo = e->t == s->t && eo <= f->start + f->len ? eo : f->start + f->len;
                        WebUnit x1 = font ? font_measure(d, font, s->t->text + f->start, endo - f->start) : f->w;
                        rect->x = cx + f->x + x0;
                        rect->y = cy + f->y;
                        rect->w = x1 - x0;
                        rect->h = f->h;
                        break;
                    }
                }
            }
        }
        count++;
        i += (uint32_t)nl;
    }
    wa_destroy(&a);
    return count;
}

/* ================================================================================================ scrolling */

void web_doc_set_scroll(WebDoc *d, int x, int y) {
    ensure_layout(d);
    int w = 0, h = 0;
    web_doc_content_size(d, &w, &h);
    int mx = w - d->vw, my = h - d->vh;
    if (x > mx) x = mx;
    if (y > my) y = my;
    d->scroll_x = x < 0 ? 0 : x;
    d->scroll_y = y < 0 ? 0 : y;
}

void web_doc_scroll(WebDoc *d, int *x, int *y) {
    if (x) *x = d->scroll_x;
    if (y) *y = d->scroll_y;
}

static bool scrolls(const LBox *b) {
    if (!(b->flags & BF_SCROLL) || !b->s) return false;
    const CBox *bx = b->s->box;
    return bx->overflow_x == OV_SCROLL || bx->overflow_x == OV_AUTO || bx->overflow_y == OV_SCROLL || bx->overflow_y == OV_AUTO;
}

WebNode *web_doc_scroller_at(WebDoc *d, int x, int y) {
    ensure_layout(d);
    if (!d->lay) return NULL;
    WebArena a;
    wa_init(&a, &d->budget);
    WebRect r = { WEB_PX(x), WEB_PX(y), 64, 64 };
    PaintOut out;
    paint_collect(d, &a, r, WEB_PX(d->scroll_x), WEB_PX(d->scroll_y), true, &out);
    int i = top_item(&out);
    WebNode *res = NULL;
    if (i >= 0) {
        LBox *b = out.hits[i].box;
        if (out.hits[i].container) b = out.hits[i].container;
        for (; b; b = b->parent)
            if (scrolls(b) && (b->cw > b->w - b->b[1] - b->b[3] || b->ch > b->h - b->b[0] - b->b[2])) { res = b->node; break; }
    }
    wa_destroy(&a);
    return res;
}

bool web_element_scroll(WebNode *el, int *x, int *y, int *max_x, int *max_y) {
    if (!is_el(el)) return false;
    ensure_layout(el->doc);
    LBox *b = el->box;
    if (!b || !(b->flags & BF_SCROLL)) return false;
    WebUnit pw = b->w - b->b[1] - b->b[3], ph = b->h - b->b[0] - b->b[2];
    if (x) *x = b->sx / 64;
    if (y) *y = b->sy / 64;
    if (max_x) *max_x = b->cw > pw ? (b->cw - pw) / 64 : 0;
    if (max_y) *max_y = b->ch > ph ? (b->ch - ph) / 64 : 0;
    return true;
}

void web_element_set_scroll(WebNode *el, int x, int y) {
    if (!is_el(el)) return;
    el->u.el.scroll_x = WEB_PX(x < 0 ? 0 : x);
    el->u.el.scroll_y = WEB_PX(y < 0 ? 0 : y);
    LBox *b = el->box;
    if (b && (b->flags & BF_SCROLL) && el->doc->lay && !el->doc->layout_dirty) {
        WebUnit pw = b->w - b->b[1] - b->b[3], ph = b->h - b->b[0] - b->b[2];
        WebUnit mx = b->cw > pw ? b->cw - pw : 0, my = b->ch > ph ? b->ch - ph : 0;
        b->sx = el->u.el.scroll_x < mx ? el->u.el.scroll_x : mx;
        b->sy = el->u.el.scroll_y < my ? el->u.el.scroll_y : my;
    }
}

bool web_doc_fragment_position(WebDoc *d, const char *frag, int *x, int *y) {
    if (!frag) return false;
    if (*frag == '#') frag++;
    ensure_layout(d);
    WebNode *el = *frag ? web_doc_get_element_by_id(d, frag) : NULL;
    if (!el && *frag) {
        WebNode *root = d->root;
        for (WebNode *n = root->first; n; n = dom_next(n, root))
            if (is_html(n, AT_a) && dom_attr_val(n, AT_name) && !strcmp(dom_attr_val(n, AT_name), frag)) { el = n; break; }
    }
    if (!*frag || !strcasecmp(frag, "top")) { *x = *y = 0; return true; }
    WebRect r;
    if (!el || !web_element_rect(el, &r)) return false;
    *x = r.x / 64;
    *y = r.y / 64;
    return true;
}
