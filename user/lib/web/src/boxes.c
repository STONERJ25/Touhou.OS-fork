/*
 * The box tree (CSS 2.1 chapter 9.2, Display 3, Tables 17.2.1): every rendered element gets a box according to its display
 * type; text nodes become text boxes; ::before and ::after become boxes with their generated text; list items get a marker.
 * Then the anonymous boxes the formatting rules need are added:
 *   - a block container with both inline-level and block-level children wraps each run of inline-level children in an
 *     anonymous block (runs of collapsible white space alone are dropped);
 *   - an inline box that contains block-level boxes is laid out as a block (the CSS 2.1 "block-in-inline" split is not
 *     done: the inline then behaves as a block box, which is what such markup, a link around cards, usually means);
 *   - flex and grid containers wrap runs of text in anonymous items;
 *   - tables get their missing rows, row groups, cells and wrapper (the 17.2.1 algorithm, simplified for white space).
 * Counters: the list-item counter of HTML lists (ol start/reversed, li value) and counter-reset/-increment/-set, with
 * the CSS 2.1 scope (until the end of the parent of the element that created the counter).
 */
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>

const char *forms_input_type(const WebNode *el);      /* forms.c */
bool control_checked(const WebNode *el);

LBox *box_new(L *l, int kind, WebNode *node, const CStyle *s) {
    LBox *b = wa_alloc(l->a, sizeof *b);
    if (!b) return NULL;
    b->kind = (uint8_t)kind;
    b->node = node;
    b->s = s;
    b->order = ++l->order;
    l->st->nboxes++;
    return b;
}

void box_append(LBox *parent, LBox *child) {
    child->parent = parent;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void box_unlink(LBox *c) {
    LBox *p = c->parent;
    if (!p) return;
    if (c->prev) c->prev->next = c->next; else p->first = c->next;
    if (c->next) c->next->prev = c->prev; else p->last = c->prev;
    c->parent = c->prev = c->next = NULL;
}

static void box_insert_before(LBox *parent, LBox *child, LBox *ref) {
    if (!ref) { box_append(parent, child); return; }
    child->parent = parent;
    child->next = ref;
    child->prev = ref->prev;
    if (ref->prev) ref->prev->next = child; else parent->first = child;
    ref->prev = child;
}

bool box_is_block_level(const LBox *b) {
    if (b->flags & (BF_FLOAT | BF_ABS)) return false;          /* out of flow: neither, handled apart */
    return !(b->flags & BF_INLINE_LEVEL);
}

static bool out_of_flow(const LBox *b) { return (b->flags & (BF_FLOAT | BF_ABS)) != 0; }

/* ================================================================================================ counters */

typedef struct Counter { WebAtom name; int value; } Counter;
typedef struct Counters { Counter v[256]; int n; } Counters;

static Counter *counter_find(Counters *c, WebAtom name) {
    for (int i = c->n - 1; i >= 0; i--) if (c->v[i].name == name) return &c->v[i];
    return NULL;
}

static void counter_reset(Counters *c, WebAtom name, int value) {
    if (c->n < 256) { c->v[c->n].name = name; c->v[c->n].value = value; c->n++; }
}

static void counter_add(Counters *c, WebAtom name, int by) {
    Counter *k = counter_find(c, name);
    if (!k) { counter_reset(c, name, 0); k = &c->v[c->n - 1]; }
    k->value += by;
}

static void counter_set(Counters *c, WebAtom name, int value) {
    Counter *k = counter_find(c, name);
    if (!k) { counter_reset(c, name, value); return; }
    k->value = value;
}

/* ================================================================================================ builder */

typedef struct Builder {
    L *l;
    Counters counters;
    WebAtom list_item;
    int quote_depth;
    const CStyle *text_style;      /* display: contents: the style its text children inherit */
} Builder;

static void build_children(Builder *bd, WebNode *el, LBox *box);
static void build_node(Builder *bd, WebNode *n, LBox *parent);

static LBox *text_box(L *l, const char *s, size_t n, const CStyle *st, WebNode *node) {
    LBox *t = box_new(l, BK_TEXT, node, st);
    if (!t) return NULL;
    t->text = s;
    t->tlen = (uint32_t)n;
    t->flags |= BF_INLINE_LEVEL;
    return t;
}

static bool is_replaced_el(const WebNode *el) {
    if (el->ns == WEB_NS_SVG) return el->name == AT_svg;
    if (el->ns != WEB_NS_HTML) return false;
    switch (el->name) {
    case AT_img: case AT_video: case AT_canvas: case AT_iframe: case AT_embed: case AT_input: case AT_select:
    case AT_textarea: case AT_meter: case AT_progress:
        return true;
    case AT_object: return dom_attr(el, AT_data) != NULL;
    default: return false;
    }
}

/* the number formats of list markers and counter() */
static void roman(char *out, size_t cap, int v, bool upper) {
    static const struct { int v; const char *s; } r[] = { {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
        {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"} };
    size_t k = 0;
    if (v <= 0 || v >= 4000) { snprintf(out, cap, "%d", v); return; }
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        while (v >= r[i].v && k + 3 < cap) {
            for (const char *s = r[i].s; *s; s++) out[k++] = upper ? (char)(*s - 32) : *s;
            v -= r[i].v;
        }
    out[k] = 0;
}

static void alpha(char *out, size_t cap, int v, bool upper) {
    if (v <= 0) { snprintf(out, cap, "%d", v); return; }
    char tmp[16];
    int k = 0;
    while (v > 0 && k < 15) { v--; tmp[k++] = (char)((upper ? 'A' : 'a') + v % 26); v /= 26; }
    int n = 0;
    while (k > 0 && (size_t)n + 1 < cap) out[n++] = tmp[--k];
    out[n] = 0;
}

void format_counter(char *out, size_t cap, int v, int style) {
    switch (style) {
    case LT_DECIMAL_LZ: snprintf(out, cap, v >= 0 && v < 10 ? "0%d" : "%d", v); break;
    case LT_LOWER_ALPHA: alpha(out, cap, v, false); break;
    case LT_UPPER_ALPHA: alpha(out, cap, v, true); break;
    case LT_LOWER_ROMAN: roman(out, cap, v, false); break;
    case LT_UPPER_ROMAN: roman(out, cap, v, true); break;
    case LT_LOWER_GREEK: {
        if (v <= 0 || v > 24) { snprintf(out, cap, "%d", v); break; }
        uint32_t cp = 0x3B1 + (uint32_t)(v - 1) + (v > 17 ? 1 : 0);   /* skips final sigma */
        int n = utf8_encode(cp, out);
        out[n] = 0;
        break;
    }
    case LT_DISC: snprintf(out, cap, "\xE2\x80\xA2"); break;
    case LT_CIRCLE: snprintf(out, cap, "\xE2\x97\xA6"); break;
    case LT_SQUARE: snprintf(out, cap, "\xE2\x96\xAA"); break;
    case LT_NONE: out[0] = 0; break;
    default: snprintf(out, cap, "%d", v); break;
    }
}

static const CStyle *anon_style(L *l, const CStyle *parent, int display) {
    /* an anonymous box inherits from its parent and has the initial non-inherited values, with the given display */
    CStyle *s = wa_alloc(l->a, sizeof *s);
    const CStyle *init = style_initial(l->d);
    if (!s || !init) return parent ? parent : init;
    *s = *init;
    s->inh = parent ? parent->inh : init->inh;
    s->vars = parent ? parent->vars : NULL;
    if (display != D_INLINE) {
        CBox *b = wa_alloc(l->a, sizeof *b);
        if (b) { *b = *init->box; b->display = (uint8_t)display; s->box = b; }
    }
    s->display = (uint8_t)display;
    return s;
}

/* generated content: the text of a content list */
static void content_text(Builder *bd, const CssContent *ct, WebNode *el, WebBuf *out, const CStyle *st) {
    L *l = bd->l;
    for (int i = 0; i < ct->n; i++) {
        const ContentItem *it = &ct->v[i];
        switch (it->kind) {
        case CC_STRING: buf_puts(out, it->s); break;
        case CC_ATTR: { const char *v = dom_attr_val(el, it->name); if (v) buf_puts(out, v); break; }
        case CC_COUNTER: case CC_COUNTERS: {
            char tmp[48];
            if (it->kind == CC_COUNTERS) {
                bool first = true;
                for (int k = 0; k < bd->counters.n; k++) {
                    if (bd->counters.v[k].name != it->name) continue;
                    if (!first && it->sep) buf_puts(out, it->sep);
                    format_counter(tmp, sizeof tmp, bd->counters.v[k].value, it->style);
                    buf_puts(out, tmp);
                    first = false;
                }
                if (first) { format_counter(tmp, sizeof tmp, 0, it->style); buf_puts(out, tmp); }
            } else {
                Counter *c = counter_find(&bd->counters, it->name);
                format_counter(tmp, sizeof tmp, c ? c->value : 0, it->style);
                buf_puts(out, tmp);
            }
            break;
        }
        case CC_OPEN_QUOTE: case CC_CLOSE_QUOTE: {
            const Quotes *q = st->inh->quotes;
            if (it->kind == CC_CLOSE_QUOTE && bd->quote_depth > 0) bd->quote_depth--;
            int depth = bd->quote_depth;
            if (q && q->n >= 2) {
                int pair = depth < q->n / 2 ? depth : q->n / 2 - 1;
                buf_puts(out, q->v[pair * 2 + (it->kind == CC_CLOSE_QUOTE)]);
            } else {
                static const char *const dq[4] = { "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99" };
                buf_puts(out, dq[(depth ? 2 : 0) + (it->kind == CC_CLOSE_QUOTE)]);
            }
            if (it->kind == CC_OPEN_QUOTE) bd->quote_depth++;
            break;
        }
        case CC_NO_OPEN_QUOTE: bd->quote_depth++; break;
        case CC_NO_CLOSE_QUOTE: if (bd->quote_depth > 0) bd->quote_depth--; break;
        default: break;
        }
    }
    (void)l;
}

static void build_pseudo(Builder *bd, WebNode *el, LBox *box, int which) {
    const CStyle *ps = el->u.el.pstyle[which];
    if (!ps) return;
    L *l = bd->l;
    const CssContent *ct = ps->box->content;
    if (!ct || ct->none || ct->normal) return;
    int kind = ps->display == D_INLINE ? BK_INLINE : BK_BLOCK;
    LBox *g = box_new(l, kind, el, ps);
    if (!g) return;
    g->flags |= BF_GENERATED | BF_ANON;
    if (ps->display == D_INLINE || ps->display == D_INLINE_BLOCK) g->flags |= BF_INLINE_LEVEL;
    if (ps->display == D_INLINE_BLOCK) g->flags |= BF_BFC;
    WebBuf b;
    buf_init(&b, l->a);
    for (int i = 0; i < ct->n; i++) {
        if (ct->v[i].kind == CC_URL) {
            if (b.n) { LBox *t = text_box(l, b.p, b.n, ps, el); if (t) box_append(g, t); buf_init(&b, l->a); }
            LBox *img = box_new(l, BK_REPLACED, el, ps);
            if (img) {
                img->flags |= BF_INLINE_LEVEL | BF_ANON;
                img->label = ct->v[i].s;
                box_append(g, img);
            }
            continue;
        }
        CssContent one = { (ContentItem *)&ct->v[i], 1, false, false };
        content_text(bd, &one, el, &b, ps);
    }
    if (b.n) {
        LBox *t = text_box(l, b.p, b.n, ps, el);
        if (t) box_append(g, t);
    }
    box_append(box, g);
}

/* the list-item counter and the marker */
static void list_item(Builder *bd, WebNode *el, LBox *box) {
    const CStyle *s = box->s;
    Counter *c = counter_find(&bd->counters, bd->list_item);
    int v = c ? c->value : 1;
    box->marker_kind = s->inh->list_type;
    char tmp[48];
    if (s->inh->list_type == LT_STRING && s->inh->list_string) box->marker = s->inh->list_string;
    else if (s->inh->list_type >= LT_DECIMAL && s->inh->list_type != LT_DISCLOSURE_OPEN && s->inh->list_type != LT_DISCLOSURE_CLOSED) {
        format_counter(tmp, sizeof tmp, v, s->inh->list_type);
        size_t n = strlen(tmp);
        char *m = wa_alloc(bd->l->a, n + 3);
        if (m) { memcpy(m, tmp, n); m[n] = '.'; m[n + 1] = ' '; m[n + 2] = 0; }
        box->marker = m;
    }
    if (s->inh->list_type == LT_NONE && !s->inh->list_image) return;
    if (!s->inh->list_pos) {
        /* inside: an inline marker box at the start of the content */
        const CStyle *ms = el->u.el.pstyle[2] ? el->u.el.pstyle[2] : s;
        LBox *mk = box_new(bd->l, BK_INLINE, el, ms);
        if (!mk) return;
        mk->flags |= BF_MARKER | BF_INLINE_LEVEL | BF_ANON;
        const char *txt = box->marker;
        char bullet[8];
        if (!txt) {
            int lt = s->inh->list_type;
            const char *bs = lt == LT_CIRCLE ? "\xE2\x97\xA6 " : lt == LT_SQUARE ? "\xE2\x96\xAA " :
                             lt == LT_DISCLOSURE_OPEN ? "\xE2\x96\xBE " : lt == LT_DISCLOSURE_CLOSED ? "\xE2\x96\xB8 " : "\xE2\x80\xA2 ";
            strcpy(bullet, bs);
            txt = wa_strdup(bd->l->a, bullet);
        }
        LBox *t = text_box(bd->l, txt, strlen(txt), ms, el);
        if (t) box_append(mk, t);
        box_append(box, mk);
        box->marker = NULL;
        box->marker_kind = LT_NONE;
    }
}

static void enter_counters(Builder *bd, WebNode *el, const CStyle *s) {
    const CBox *b = s->box;
    bool list_reset = false;
    if (b->c_reset) for (int i = 0; i < b->c_reset->n; i++) {
        counter_reset(&bd->counters, b->c_reset->v[i].name, b->c_reset->v[i].value);
        if (b->c_reset->v[i].name == bd->list_item) list_reset = true;
    }
    /* HTML lists reset the list-item counter (ol start and reversed decide the first number) */
    if (!list_reset && (is_html(el, AT_ol) || is_html(el, AT_ul) || is_html(el, AT_menu) || is_html(el, AT_dir))) {
        int start = 1;
        bool rev = is_html(el, AT_ol) && dom_attr(el, AT_reversed);
        if (rev) {
            int count = 0;
            for (WebNode *k = el->first; k; k = k->next) if (is_html(k, AT_li)) count++;
            start = count;
        }
        const char *st = is_html(el, AT_ol) ? dom_attr_val(el, AT_start) : NULL;
        if (st) start = atoi(st);
        counter_reset(&bd->counters, bd->list_item, rev ? start + 1 : start - 1);   /* the items count down in reversed lists */
    }
    if (b->c_set) for (int i = 0; i < b->c_set->n; i++) counter_set(&bd->counters, b->c_set->v[i].name, b->c_set->v[i].value);
    bool inc_li = false;
    if (b->c_inc) for (int i = 0; i < b->c_inc->n; i++) {
        counter_add(&bd->counters, b->c_inc->v[i].name, b->c_inc->v[i].value);
        if (b->c_inc->v[i].name == bd->list_item) inc_li = true;
    }
    if (s->display == D_LIST_ITEM && !inc_li) {
        const char *val = is_html(el, AT_li) ? dom_attr_val(el, AT_value) : NULL;
        if (val) counter_set(&bd->counters, bd->list_item, atoi(val));
        else {
            WebNode *p = el->parent;
            bool rev = p && is_html(p, AT_ol) && dom_attr(p, AT_reversed);
            counter_add(&bd->counters, bd->list_item, rev ? -1 : 1);
        }
    }
}

static int control_kind(const WebNode *el) {
    if (is_html(el, AT_textarea)) return WEB_CONTROL_TEXTAREA;
    if (is_html(el, AT_select)) return (dom_attr(el, AT_multiple) || (dom_attr_val(el, AT_size) && atoi(dom_attr_val(el, AT_size)) > 1)) ? WEB_CONTROL_LISTBOX : WEB_CONTROL_SELECT;
    if (is_html(el, AT_progress)) return WEB_CONTROL_PROGRESS;
    if (is_html(el, AT_meter)) return WEB_CONTROL_METER;
    if (!is_html(el, AT_input)) return 0;
    const char *t = forms_input_type(el);
    if (!strcmp(t, "checkbox")) return WEB_CONTROL_CHECKBOX;
    if (!strcmp(t, "radio")) return WEB_CONTROL_RADIO;
    if (!strcmp(t, "submit") || !strcmp(t, "reset") || !strcmp(t, "button")) return WEB_CONTROL_BUTTON;
    if (!strcmp(t, "range")) return WEB_CONTROL_RANGE;
    if (!strcmp(t, "color")) return WEB_CONTROL_COLOR;
    if (!strcmp(t, "file")) return WEB_CONTROL_FILE;
    if (!strcmp(t, "image")) return 0;                      /* an image */
    return WEB_CONTROL_TEXT;
}

static LBox *element_box(Builder *bd, WebNode *el, const CStyle *s) {
    L *l = bd->l;
    int disp = s->display;
    LBox *b = NULL;
    if (is_replaced_el(el) && disp != D_CONTENTS) {
        b = box_new(l, BK_REPLACED, el, s);
        if (!b) return NULL;
        int ck = control_kind(el);
        if (ck) { b->flags |= BF_CONTROL; b->control = (uint8_t)ck; }
        if (disp == D_INLINE || disp == D_INLINE_BLOCK || disp == D_INLINE_FLEX || disp == D_INLINE_GRID || disp == D_INLINE_TABLE)
            b->flags |= BF_INLINE_LEVEL;
        /* floated and positioned replaced elements (img align=left, an absolutely placed input) */
        if (s->box->float_) b->flags = (b->flags & ~(uint32_t)BF_INLINE_LEVEL) | BF_FLOAT | BF_BFC;
        if (s->box->position == POS_ABSOLUTE || s->box->position == POS_FIXED) {
            b->flags = (b->flags & ~(uint32_t)(BF_INLINE_LEVEL | BF_FLOAT)) | BF_ABS | BF_BFC;
            if (s->box->position == POS_FIXED) b->flags |= BF_FIXED;
        }
        return b;
    }
    if (is_html(el, AT_br) && (disp == D_INLINE || disp == D_INLINE_BLOCK)) {
        b = box_new(l, BK_BR, el, s);
        if (b) b->flags |= BF_INLINE_LEVEL;
        return b;
    }
    switch (disp) {
    case D_INLINE: b = box_new(l, BK_INLINE, el, s); if (b) b->flags |= BF_INLINE_LEVEL; break;
    case D_INLINE_BLOCK: b = box_new(l, BK_BLOCK, el, s); if (b) b->flags |= BF_INLINE_LEVEL | BF_BFC; break;
    case D_FLEX: case D_INLINE_FLEX:
        b = box_new(l, BK_FLEX, el, s);
        if (b) b->flags |= BF_BFC | (disp == D_INLINE_FLEX ? BF_INLINE_LEVEL : 0);
        break;
    case D_GRID: case D_INLINE_GRID:
        b = box_new(l, BK_GRID, el, s);
        if (b) b->flags |= BF_BFC | (disp == D_INLINE_GRID ? BF_INLINE_LEVEL : 0);
        break;
    case D_TABLE: case D_INLINE_TABLE: {
        b = box_new(l, BK_TABLE_WRAPPER, el, s);
        if (!b) return NULL;
        b->flags |= BF_BFC | (disp == D_INLINE_TABLE ? BF_INLINE_LEVEL : 0);
        LBox *t = box_new(l, BK_TABLE, el, s);
        if (t) { t->flags |= BF_BFC; box_append(b, t); }
        break;
    }
    case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP: b = box_new(l, BK_ROW_GROUP, el, s); break;
    case D_TABLE_ROW: b = box_new(l, BK_ROW, el, s); break;
    case D_TABLE_CELL: b = box_new(l, BK_CELL, el, s); if (b) b->flags |= BF_BFC; break;
    case D_TABLE_COLUMN_GROUP: b = box_new(l, BK_COLGROUP, el, s); break;
    case D_TABLE_COLUMN: b = box_new(l, BK_COL, el, s); break;
    default: b = box_new(l, BK_BLOCK, el, s); break;      /* block, list-item, flow-root, caption */
    }
    if (!b) return NULL;
    const CBox *cb = s->box;
    if (cb->float_) b->flags |= BF_FLOAT | BF_BFC;
    if (cb->position == POS_ABSOLUTE || cb->position == POS_FIXED) {
        b->flags |= BF_ABS | BF_BFC;
        if (cb->position == POS_FIXED) b->flags |= BF_FIXED;
    }
    if (cb->overflow_x != OV_VISIBLE || cb->overflow_y != OV_VISIBLE) {
        if (b->kind == BK_BLOCK || b->kind == BK_FLEX || b->kind == BK_GRID) b->flags |= BF_SCROLL | BF_BFC;
    }
    if (disp == D_FLOW_ROOT) b->flags |= BF_BFC;
    return b;
}

static void build_element(Builder *bd, WebNode *el, LBox *parent) {
    L *l = bd->l;
    const CStyle *s = el->u.el.style;
    if (!s || s->display == D_NONE) return;
    if (l->depth > 400) return;
    if (el->ns == WEB_NS_SVG && el->name != AT_svg) return;   /* inside an svg: part of the svg's picture */
    enter_counters(bd, el, s);                               /* (they live until the end of the parent: build_children) */
    const CStyle *saved = bd->text_style;
    if (s->display == D_CONTENTS) {
        l->depth++;
        bd->text_style = s;
        build_pseudo(bd, el, parent, 0);
        build_children(bd, el, parent);
        build_pseudo(bd, el, parent, 1);
        bd->text_style = saved;
        l->depth--;
        return;
    }
    if (is_html(el, AT_wbr)) {                               /* a line break opportunity: a zero width space */
        LBox *t = text_box(l, "\xE2\x80\x8B", 3, parent->s, el);
        if (t) box_append(parent, t);
        return;
    }
    LBox *b = element_box(bd, el, s);
    if (!b) return;
    if (!el->box) el->box = b;
    if (s->display == D_LIST_ITEM) list_item(bd, el, b);
    box_append(parent, b);                                   /* positioned boxes stay here: their static position */
    if (b->kind == BK_REPLACED || b->kind == BK_BR) return;  /* replaced: its children are not boxes */
    LBox *inner = b->kind == BK_TABLE_WRAPPER ? b->first : b;
    l->depth++;
    bd->text_style = NULL;
    build_pseudo(bd, el, inner, 0);
    build_children(bd, el, inner);
    build_pseudo(bd, el, inner, 1);
    bd->text_style = saved;
    l->depth--;
}

static void build_node(Builder *bd, WebNode *n, LBox *parent) {
    if (n->type == WEB_NODE_ELEMENT) { build_element(bd, n, parent); return; }
    if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_CDATA) {
        if (!n->u.cd.len) return;
        const CStyle *ps = bd->text_style ? bd->text_style : parent->s;
        LBox *t = text_box(bd->l, n->u.cd.data, n->u.cd.len, ps, n);
        if (t) { box_append(parent, t); n->box = t; }
    }
}

static void build_children(Builder *bd, WebNode *el, LBox *box) {
    int mark = bd->counters.n;
    if (is_html(el, AT_br)) return;
    for (WebNode *k = el->first; k; k = k->next) build_node(bd, k, box);
    bd->counters.n = mark;                                   /* the counters created by the children end here */
}

/* ================================================================================================ fixups */

static bool collapsible_ws_text(const LBox *t) {
    if (t->kind != BK_TEXT) return false;
    int ws = t->s->inh->white_space;
    if (ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES) return false;
    for (uint32_t i = 0; i < t->tlen; i++) {
        char c = t->text[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') return false;
        if (c == '\n' && ws == WS_PRE_LINE) return false;
    }
    return true;
}

static bool is_table_part(int k) {
    return k == BK_ROW_GROUP || k == BK_ROW || k == BK_CELL || k == BK_COLGROUP || k == BK_COL;
}

static void fix_box(L *l, LBox *b, int depth);

/* wraps the children [from, to) of p in a new anonymous box w (inserted where they were) */
static LBox *wrap(L *l, LBox *p, LBox *from, LBox *to, int kind, int display) {
    LBox *w = box_new(l, kind, p->node, anon_style(l, p->s, display));
    if (!w) return NULL;
    w->flags |= BF_ANON;
    box_insert_before(p, w, from);
    for (LBox *c = from; c && c != to;) {
        LBox *nx = c->next;
        box_unlink(c);
        box_append(w, c);
        c = nx;
    }
    return w;
}

static void fix_table(L *l, LBox *table) {
    /* captions belong to the wrapper; rows/cells/other content get the missing parents */
    LBox *wrapper = table->parent && table->parent->kind == BK_TABLE_WRAPPER ? table->parent : NULL;
    for (LBox *c = table->first; c;) {
        LBox *nx = c->next;
        if (c->kind == BK_BLOCK && c->s->display == D_TABLE_CAPTION && wrapper) {
            box_unlink(c);
            if (c->s->inh->caption_side) box_append(wrapper, c);
            else box_insert_before(wrapper, c, table);
        } else if (c->kind == BK_TEXT && collapsible_ws_text(c)) {
            box_unlink(c);
        }
        c = nx;
    }
    for (LBox *c = table->first; c;) {
        if (c->kind == BK_ROW_GROUP || c->kind == BK_COLGROUP || c->kind == BK_COL) { c = c->next; continue; }
        LBox *start = c;
        while (c && !(c->kind == BK_ROW_GROUP || c->kind == BK_COLGROUP || c->kind == BK_COL)) c = c->next;
        wrap(l, table, start, c, BK_ROW_GROUP, D_TABLE_ROW_GROUP);
    }
    for (LBox *g = table->first; g; g = g->next) {
        if (g->kind != BK_ROW_GROUP) continue;
        for (LBox *c = g->first; c;) {
            LBox *nx = c->next;
            if (c->kind == BK_TEXT && collapsible_ws_text(c)) box_unlink(c);
            c = nx;
        }
        for (LBox *c = g->first; c;) {
            if (c->kind == BK_ROW) { c = c->next; continue; }
            LBox *start = c;
            while (c && c->kind != BK_ROW) c = c->next;
            wrap(l, g, start, c, BK_ROW, D_TABLE_ROW);
        }
        for (LBox *r = g->first; r; r = r->next) {
            for (LBox *c = r->first; c;) {
                LBox *nx = c->next;
                if (c->kind == BK_TEXT && collapsible_ws_text(c)) box_unlink(c);
                c = nx;
            }
            for (LBox *c = r->first; c;) {
                if (c->kind == BK_CELL) { c = c->next; continue; }
                LBox *start = c;
                while (c && c->kind != BK_CELL) c = c->next;
                LBox *cell = wrap(l, r, start, c, BK_CELL, D_TABLE_CELL);
                if (cell) cell->flags |= BF_BFC;
            }
        }
    }
    /* columns: a column group without col children spans one column per its span attribute (handled in table.c) */
}

/* table-internal boxes outside a table get an anonymous table around them */
static void wrap_orphans(L *l, LBox *b) {
    if (b->kind == BK_TABLE || b->kind == BK_ROW_GROUP || b->kind == BK_ROW) return;
    for (LBox *c = b->first; c;) {
        if (!is_table_part(c->kind)) { c = c->next; continue; }
        LBox *start = c;
        while (c && (is_table_part(c->kind) || (c->kind == BK_TEXT && collapsible_ws_text(c)))) c = c->next;
        LBox *w = wrap(l, b, start, c, BK_TABLE_WRAPPER, D_TABLE);
        if (!w) return;
        w->flags |= BF_BFC;
        LBox *t = wrap(l, w, w->first, NULL, BK_TABLE, D_TABLE);
        if (t) { t->flags |= BF_BFC; fix_table(l, t); }
    }
}

static bool has_block_children(const LBox *b) {
    for (const LBox *c = b->first; c; c = c->next)
        if (!out_of_flow(c) && box_is_block_level(c)) return true;
    return false;
}

static void fix_box(L *l, LBox *b, int depth) {
    if (depth > 500) return;
    for (LBox *c = b->first; c; c = c->next) fix_box(l, c, depth + 1);
    if (b->kind == BK_TABLE) { fix_table(l, b); return; }
    if (b->kind == BK_ROW_GROUP || b->kind == BK_ROW || b->kind == BK_COLGROUP || b->kind == BK_COL ||
        b->kind == BK_TABLE_WRAPPER || b->kind == BK_TEXT || b->kind == BK_REPLACED || b->kind == BK_BR) return;
    wrap_orphans(l, b);
    if (b->kind == BK_INLINE) {
        /* an inline box with block-level children becomes a block box */
        if (!has_block_children(b)) return;
        b->kind = BK_BLOCK;
        b->flags &= (uint32_t)~BF_INLINE_LEVEL;
    }
    if (b->kind == BK_FLEX || b->kind == BK_GRID) {
        /* every in-flow child is an item; text runs become anonymous block items, white space alone is dropped */
        for (LBox *c = b->first; c;) {
            if (c->kind != BK_TEXT && c->kind != BK_BR) {
                c->flags &= (uint32_t)~(BF_INLINE_LEVEL | BF_FLOAT);   /* items are blockified; float does not apply */
                if (c->kind == BK_INLINE) { c->kind = BK_BLOCK; c->flags |= BF_BFC; }
                c = c->next;
                continue;
            }
            LBox *start = c;
            bool only_ws = true;
            while (c && (c->kind == BK_TEXT || c->kind == BK_BR)) { if (!collapsible_ws_text(c)) only_ws = false; c = c->next; }
            if (only_ws) {
                for (LBox *k = start; k != c;) { LBox *nx = k->next; box_unlink(k); k = nx; }
            } else {
                LBox *w = wrap(l, b, start, c, BK_BLOCK, D_BLOCK);
                if (w) { w->flags |= BF_BFC | BF_INLINE_CONTENT; }
            }
        }
        for (LBox *c = b->first; c; c = c->next) if (c->kind == BK_BLOCK && !(c->flags & BF_INLINE_CONTENT) && !has_block_children(c)) {
            bool any = false;
            for (LBox *k = c->first; k; k = k->next) if (!out_of_flow(k)) any = true;
            if (any) c->flags |= BF_INLINE_CONTENT;
        }
        return;
    }
    if (b->kind != BK_BLOCK && b->kind != BK_CELL && b->kind != BK_VIEWPORT) return;
    if (!has_block_children(b)) {
        /* only inline-level children: an inline formatting context (white space alone makes no line boxes) */
        for (LBox *c = b->first; c; c = c->next)
            if (!out_of_flow(c)) { b->flags |= BF_INLINE_CONTENT; break; }
        return;
    }
    /* mixed: wrap the inline runs; runs of collapsible white space (and out-of-flow boxes among them) stay unwrapped */
    for (LBox *c = b->first; c;) {
        if (box_is_block_level(c) || out_of_flow(c)) { c = c->next; continue; }
        LBox *start = c;
        bool only_ws = true;
        while (c && (!box_is_block_level(c) || out_of_flow(c))) {
            if (!out_of_flow(c) && !(c->kind == BK_TEXT && collapsible_ws_text(c))) only_ws = false;
            c = c->next;
        }
        if (only_ws) {
            for (LBox *k = start; k != c;) { LBox *nx = k->next; if (k->kind == BK_TEXT) box_unlink(k); k = nx; }
            continue;
        }
        LBox *w = wrap(l, b, start, c, BK_BLOCK, D_BLOCK);
        if (w) w->flags |= BF_INLINE_CONTENT;
    }
}

/* ================================================================================================ the entry point */

LBox *boxes_build(L *l) {
    WebDoc *d = l->d;
    LBox *vp = box_new(l, BK_VIEWPORT, NULL, style_initial(d));
    if (!vp) return NULL;
    vp->flags |= BF_BFC;
    Builder bd;
    memset(&bd, 0, sizeof bd);
    bd.l = l;
    bd.list_item = atom_get(&d->atoms, "list-item", 9);
    /* clear stale box pointers */
    for (WebNode *n = d->root->first; n; n = dom_next(n, d->root)) n->box = NULL;
    WebNode *root = web_doc_element(d);
    if (root) build_node(&bd, root, vp);
    fix_box(l, vp, 0);
    return vp;
}
