#include "layout.h"
#include "news.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the operation list */

void lay_reset(Layout *L, int width) {
    L->n = 0;
    L->height = 0;
    L->width = width;
}

void lay_free(Layout *L) {
    free(L->ops);
    memset(L, 0, sizeof *L);
}

DrawOp *lay_op(Layout *L, OpKind kind) {
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 256;
        DrawOp *no = realloc(L->ops, (size_t)nc * sizeof *no);
        if (!no) return NULL;
        L->ops = no;
        L->cap = nc;
    }
    DrawOp *op = &L->ops[L->n++];
    memset(op, 0, sizeof *op);
    op->kind = (uint8_t)kind;
    op->scale = 1;
    return op;
}

void lay_text(Layout *L, int x, int y, const char *text, int scale, bool bold, uint32_t color) {
    DrawOp *op = lay_op(L, OP_TEXT);
    if (!op) return;
    op->x = x; op->y = y; op->text = text; op->len = (int)strlen(text); op->scale = (uint8_t)scale; op->bold = bold; op->color = color;
    op->w = (int)kc_utf8_len(text) * 8 * scale;
    op->h = 16 * scale;
    if (y + op->h > L->height) L->height = y + op->h;
}

void lay_text_centered(Layout *L, int x, int y, int w, const char *text, int scale, bool bold, uint32_t color) {
    int tw = (int)kc_utf8_len(text) * 8 * scale;
    lay_text(L, x + (w - tw) / 2, y, text, scale, bold, color);
}

/* Finds the next wrapped line of [*pp, end): at most `cols` code points, broken at a space when possible. */
static bool wrap_next(const char **pp, const char *end, int cols, const char **start, int *len);

int lay_banner(Layout *L, int x, int y, int w, const char *text, int scale, uint32_t color, bool center) {
    int cols = w / (8 * scale);
    const char *p = text, *end = text + strlen(text), *ls;
    int ll;
    while (wrap_next(&p, end, cols, &ls, &ll)) {
        DrawOp *op = lay_op(L, OP_TEXT);
        if (!op) break;
        int cells = 0;
        for (int i = 0; i < ll; ) { uint32_t cp; int b = kc_utf8_next(ls + i, &cp); if (b <= 0) break; i += b; cells++; }
        op->text = ls;
        op->len = ll;
        op->scale = (uint8_t)scale;
        op->bold = true;
        op->color = color;
        op->w = cells * 8 * scale;
        op->h = 16 * scale;
        op->x = center ? x + (w - op->w) / 2 : x;
        op->y = y;
        y += 16 * scale + 4;
    }
    if (y > L->height) L->height = y;
    return y;
}

void lay_icon(Layout *L, int x, int y, int size, int icon) {
    DrawOp *op = lay_op(L, OP_ICON);
    if (!op) return;
    op->x = x; op->y = y; op->w = size; op->h = size; op->cp = (uint32_t)icon;
    if (y + size > L->height) L->height = y + size;
}

void lay_hrule(Layout *L, int x, int y, int w, int thickness, uint32_t color) {
    DrawOp *op = lay_op(L, OP_HRULE);
    if (!op) return;
    op->x = x; op->y = y; op->w = w; op->h = thickness; op->color = color;
    if (y + thickness > L->height) L->height = y + thickness;
}

void lay_double_rule(Layout *L, int x, int y, int w, uint32_t color) {
    lay_hrule(L, x, y, w, 3, color);
    lay_hrule(L, x, y + 5, w, 1, color);
}

/* ---------------------------------------------------------------- flows */

static Item *flow_add(Flow *f, int type) {
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 32;
        Item *ni = realloc(f->it, (size_t)nc * sizeof *ni);
        if (!ni) return NULL;
        f->it = ni;
        f->cap = nc;
    }
    Item *it = &f->it[f->n++];
    memset(it, 0, sizeof *it);
    it->type = type;
    it->color = PAGE_INK;
    it->head_scale = 1;
    return it;
}

void flow_free(Flow *f) {
    free(f->it);
    memset(f, 0, sizeof *f);
}

void flow_article(Flow *f, const Article *a, int head_scale, bool dropcap) {
    Item *it = flow_add(f, IT_ARTICLE);
    if (!it) return;
    it->art = a;
    it->head_scale = head_scale;
    it->dropcap = dropcap;
    it->justify = true;
}

void flow_para(Flow *f, const char *text, uint32_t color, bool bold, bool justify) {
    Item *it = flow_add(f, IT_PARA);
    if (!it) return;
    it->text = text;
    it->color = color;
    it->bold = bold;
    it->justify = justify;
}

void flow_boxed(Flow *f, const char *text, uint32_t color) {
    Item *it = flow_add(f, IT_PARA);
    if (!it) return;
    it->text = text;
    it->color = color;
    it->boxed = true;
}

void flow_custom(Flow *f, int height, CustomDraw draw, void *data) {
    Item *it = flow_add(f, IT_CUSTOM);
    if (!it) return;
    it->h = height;
    it->draw = draw;
    it->data = data;
}

void flow_gap(Flow *f, int h) {
    Item *it = flow_add(f, IT_GAP);
    if (it) it->h = h;
}

void flow_rule(Flow *f) { flow_add(f, IT_RULE); }

/* ---------------------------------------------------------------- atoms */

enum { A_LINE, A_CUSTOM, A_GAP, A_RULE };

typedef struct Atom {
    uint8_t kind, scale;
    bool bold, justify, center;
    const char *text;
    int len;
    int h;
    int indent;
    uint32_t color;
    uint32_t dropcp;
    int keep;                           /* how many atoms after this one must share its column */
    const Item *item;
    bool boxed;
    int col, y;
} Atom;

typedef struct AtomList { Atom *a; int n, cap; } AtomList;

static Atom *atom_add(AtomList *al, int kind) {
    if (al->n == al->cap) {
        int nc = al->cap ? al->cap * 2 : 128;
        Atom *na = realloc(al->a, (size_t)nc * sizeof *na);
        if (!na) return NULL;
        al->a = na;
        al->cap = nc;
    }
    Atom *a = &al->a[al->n++];
    memset(a, 0, sizeof *a);
    a->kind = (uint8_t)kind;
    a->scale = 1;
    return a;
}

/* Finds the next wrapped line of [*pp, end): at most `cols` code points, broken at a space when possible. */
static bool wrap_next(const char **pp, const char *end, int cols, const char **start, int *len) {
    const char *p = *pp;
    while (p < end && *p == ' ') p++;
    if (p >= end) { *pp = p; return false; }
    if (cols < 1) cols = 1;
    const char *q = p, *last_space = NULL;
    int count = 0;
    while (q < end && count < cols) {
        uint32_t cp;
        int b = kc_utf8_next(q, &cp);
        if (b <= 0) break;
        if (cp == ' ') last_space = q;
        q += b;
        count++;
    }
    const char *e = q;
    if (q < end && *q != ' ' && last_space && last_space > p) e = last_space;
    *start = p;
    *len = (int)(e - p);
    *pp = e;
    return true;
}

static const char *para_end(const char *p) {
    while (*p && *p != '\n') p++;
    return p;
}

/* adds the wrapped lines of one paragraph; returns how many atoms were added */
static int add_paragraph(AtomList *al, const char *text, int cols, int px_w, const Item *it, bool justify, bool bold, uint32_t color,
                         int scale, bool center, int dropcap_lines, uint32_t *dropcp, int lh) {
    int added = 0;
    const char *p = text, *end = para_end(text);
    const char *ls;
    int ll;
    int line_no = 0;
    (void)px_w;
    int cap_cols = dropcap_lines ? 3 : 0;
    while (true) {
        int c = cols - (line_no < dropcap_lines ? cap_cols + 1 : 0);
        if (!wrap_next(&p, end, c, &ls, &ll)) break;
        Atom *a = atom_add(al, A_LINE);
        if (!a) break;
        a->text = ls;
        a->len = ll;
        a->h = lh;
        a->scale = (uint8_t)scale;
        a->bold = bold;
        a->color = color;
        a->center = center;
        a->item = it;
        a->justify = justify;
        if (line_no < dropcap_lines) a->indent = (cap_cols + 1) * 8;
        if (line_no == 0 && dropcap_lines && dropcp) {                    /* the first letter becomes the big initial */
            uint32_t cp;
            int b = kc_utf8_next(ls, &cp);
            if (b > 0) { *dropcp = cp; a->dropcp = cp; a->text = ls + b; a->len = ll - b; }
        }
        line_no++;
        added++;
    }
    /* the last line of a paragraph is not stretched */
    if (added) al->a[al->n - 1].justify = false;
    return added;
}

static void atoms_for_item(AtomList *al, const Item *it, int col_w) {
    int cols = col_w / 8;
    switch (it->type) {
    case IT_GAP: { Atom *a = atom_add(al, A_GAP); if (a) a->h = it->h; break; }
    case IT_RULE: { Atom *a = atom_add(al, A_RULE); if (a) { a->h = 9; a->color = PAGE_RULE; } break; }
    case IT_CUSTOM: { Atom *a = atom_add(al, A_CUSTOM); if (a) { a->h = it->h; a->item = it; } break; }
    case IT_PARA: {
        int first = al->n;
        if (it->boxed) {
            Atom *g = atom_add(al, A_GAP);
            if (g) g->h = 6;
            int bcols = (col_w - 16) / 8;
            const char *p = it->text;
            while (*p) {
                int n = add_paragraph(al, p, bcols, col_w - 16, it, false, false, it->color, 1, false, 0, NULL, BODY_LH);
                for (int i = al->n - n; i < al->n; i++) { al->a[i].indent = 8; al->a[i].boxed = true; }
                p = para_end(p);
                if (*p == '\n') p++;
            }
            g = atom_add(al, A_GAP);
            if (g) g->h = 6;
            al->a[first].boxed = true;
            al->a[al->n - 1].boxed = true;
            al->a[first].keep = al->n - first - 1;                       /* the whole ad stays together */
        } else {
            const char *p = it->text;
            while (*p) {
                int n = add_paragraph(al, p, cols, col_w, it, it->justify, it->bold, it->color, 1, it->center, 0, NULL, BODY_LH);
                if (n > 1) al->a[first].keep = 0;
                p = para_end(p);
                if (*p == '\n') { p++; Atom *g = atom_add(al, A_GAP); if (g) g->h = 4; }
            }
        }
        break;
    }
    case IT_ARTICLE: {
        const Article *a = it->art;
        int first = al->n;
        if (a->kicker && a->kicker[0]) {
            Atom *k = atom_add(al, A_LINE);
            if (k) { k->text = a->kicker; k->len = (int)strlen(a->kicker); k->h = 16; k->bold = true; k->color = PAGE_ACCENT; k->item = it; }
        }
        int hs = it->head_scale > 1 ? 2 : 1;
        int hcols = cols / hs;
        const char *h = a->headline ? a->headline : "";
        const char *he = h + strlen(h);
        const char *ls;
        int ll;
        while (wrap_next(&h, he, hcols, &ls, &ll)) {
            Atom *l = atom_add(al, A_LINE);
            if (!l) break;
            l->text = ls; l->len = ll; l->scale = (uint8_t)hs; l->bold = true; l->color = PAGE_INK; l->item = it;
            l->h = hs == 2 ? 34 : 18;
        }
        if (a->byline && a->byline[0]) {
            Atom *b = atom_add(al, A_LINE);
            if (b) { b->text = a->byline; b->len = (int)strlen(a->byline); b->h = 18; b->color = PAGE_DIM; b->item = it; }
        }
        Atom *g = atom_add(al, A_GAP);
        if (g) g->h = 3;
        int head_atoms = al->n - first;
        int body_start = al->n;
        const char *p = a->body;
        bool first_para = true;
        uint32_t dropcp = 0;
        while (p && *p) {
            int n = add_paragraph(al, p, cols, col_w, it, it->justify, false, PAGE_INK, 1, false, first_para && it->dropcap ? 3 : 0, &dropcp, BODY_LH);
            (void)n;
            first_para = false;
            p = para_end(p);
            if (*p == '\n') { p++; if (*p) { Atom *gp = atom_add(al, A_GAP); if (gp) gp->h = 4; } }
        }
        int body_atoms = al->n - body_start;
        int keep_body = body_atoms < (it->dropcap ? 3 : 2) ? body_atoms : (it->dropcap ? 3 : 2);
        al->a[first].keep = head_atoms + keep_body - 1;
        Atom *tail = atom_add(al, A_GAP);                                /* space before whatever follows */
        if (tail) tail->h = 10;
        break;
    }
    }
}

/* ---------------------------------------------------------------- columns */

static int unit_len(const AtomList *al, int i) {
    int k = al->a[i].keep;
    if (i + k >= al->n) k = al->n - 1 - i;
    return k + 1;
}

static int unit_height(const AtomList *al, int i, int n) {
    int h = 0;
    for (int j = 0; j < n; j++) h += al->a[i + j].h;
    return h;
}

/* how many columns does a column height of `cap` need? */
static int columns_needed(const AtomList *al, int cap) {
    int cols = 1, acc = 0;
    for (int i = 0; i < al->n; ) {
        int n = unit_len(al, i);
        int h = unit_height(al, i, n);
        if (acc && acc + h > cap) { cols++; acc = 0; }
        acc += h;
        i += n;
    }
    return cols;
}

static void emit_line(Layout *L, const Atom *a, int x, int y, int w) {
    if (a->dropcp) {
        DrawOp *g = lay_op(L, OP_GLYPH);
        if (g) { g->x = x; g->y = y - 2; g->scale = 3; g->cp = a->dropcp; g->color = PAGE_ACCENT; g->bold = true; g->w = 24; g->h = 48; }
    }
    if (a->len <= 0) return;
    DrawOp *op = lay_op(L, OP_TEXT);
    if (!op) return;
    op->x = x + a->indent;
    op->y = y;
    op->text = a->text;
    op->len = a->len;
    op->scale = a->scale;
    op->bold = a->bold;
    op->color = a->color;
    op->justify = a->justify;
    op->center = a->center;
    op->w = w - a->indent - (a->boxed ? 8 : 0);
    op->h = a->h;
}

int lay_flow(Layout *L, int x, int y, int w, int ncols, int gutter, const Flow *f) {
    if (ncols < 1) ncols = 1;
    int col_w = (w - (ncols - 1) * gutter) / ncols;
    AtomList al = { 0 };
    for (int i = 0; i < f->n; i++) atoms_for_item(&al, &f->it[i], col_w);
    if (al.n == 0) { free(al.a); return y; }

    int total = 0, biggest = 0;
    for (int i = 0; i < al.n; ) {
        int n = unit_len(&al, i);
        int h = unit_height(&al, i, n);
        total += h;
        if (h > biggest) biggest = h;
        i += n;
    }
    int lo = biggest, hi = total;                                         /* smallest column height that fits in ncols columns */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (columns_needed(&al, mid) <= ncols) hi = mid; else lo = mid + 1;
    }
    int cap = lo;

    /* assign columns */
    int col = 0, acc = 0, tallest = 0;
    int heights[16] = { 0 };
    if (ncols > 16) ncols = 16;
    for (int i = 0; i < al.n; ) {
        int n = unit_len(&al, i);
        int h = unit_height(&al, i, n);
        if (acc && acc + h > cap && col < ncols - 1) { heights[col] = acc; col++; acc = 0; }
        for (int j = 0; j < n; j++) { al.a[i + j].col = col; al.a[i + j].y = acc; acc += al.a[i + j].h; }
        i += n;
    }
    heights[col] = acc;
    for (int c = 0; c <= col; c++) if (heights[c] > tallest) tallest = heights[c];

    /* emit */
    int box_top = -1, box_col = 0;
    for (int i = 0; i < al.n; i++) {
        Atom *a = &al.a[i];
        int cx = x + a->col * (col_w + gutter), cy = y + a->y;
        if (a->boxed && box_top < 0 && a->kind == A_GAP) { box_top = cy; box_col = a->col; }
        switch (a->kind) {
        case A_LINE: emit_line(L, a, cx, cy, col_w); break;
        case A_CUSTOM: {
            DrawOp *op = lay_op(L, OP_CUSTOM);
            if (op) { op->x = cx; op->y = cy; op->w = col_w; op->h = a->h; op->draw = a->item->draw; op->data = a->item->data; }
            break;
        }
        case A_RULE: lay_hrule(L, cx, cy + 4, col_w, 1, PAGE_RULE); break;
        default: break;
        }
        if (a->boxed && a->kind == A_GAP && box_top >= 0 && cy > box_top) {      /* the closing gap of a box */
            DrawOp *op = lay_op(L, OP_BOX);
            if (op) { op->x = x + box_col * (col_w + gutter) + 1; op->y = box_top; op->w = col_w - 2; op->h = cy + a->h - box_top; op->color = PAGE_RULE; op->dashed = true; }
            box_top = -1;
        }
    }
    for (int c = 0; c < col; c++) {                                       /* rules between the columns */
        DrawOp *op = lay_op(L, OP_VRULE);
        if (op) { op->x = x + (c + 1) * (col_w + gutter) - gutter / 2; op->y = y; op->h = tallest; op->w = 1; op->color = PAGE_RULE; }
    }
    free(al.a);
    if (y + tallest > L->height) L->height = y + tallest;
    return y + tallest;
}

/* ---------------------------------------------------------------- drawing */

static void draw_justified(KCanvas *cv, int x, int y, int w, const char *s, int len, uint32_t color, bool bold) {
    /* words are placed with the leftover width shared out between the gaps */
    int words = 0, chars = 0;
    for (int i = 0; i < len; ) {
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        words++;
        while (i < len && s[i] != ' ') { uint32_t cp; int b = kc_utf8_next(s + i, &cp); if (b <= 0) b = 1; i += b; chars++; }
    }
    if (words < 2) { kc_text_n(cv, x, y, s, (size_t)len, color); if (bold) kc_text_n(cv, x + 1, y, s, (size_t)len, color); return; }
    int natural = chars * 8, gaps = words - 1;
    int extra = w - natural;
    if (extra < gaps * 4) extra = gaps * 8;                               /* never squeeze words together */
    int share = extra / gaps, rem = extra % gaps;
    int px = x, g = 0;
    for (int i = 0; i < len; ) {
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        int st = i, cc = 0;
        while (i < len && s[i] != ' ') { uint32_t cp; int b = kc_utf8_next(s + i, &cp); if (b <= 0) b = 1; i += b; cc++; }
        kc_text_n(cv, px, y, s + st, (size_t)(i - st), color);
        if (bold) kc_text_n(cv, px + 1, y, s + st, (size_t)(i - st), color);
        px += cc * 8 + (g < gaps ? share + (g < rem ? 1 : 0) : 0);
        g++;
    }
}

void lay_draw(KUI *ui, const Layout *L, int ox, int oy, KRect view) {
    KCanvas *cv = &ui->cv;
    for (int i = 0; i < L->n; i++) {
        const DrawOp *op = &L->ops[i];
        int x = ox + op->x, y = oy + op->y;
        int h = op->h > 0 ? op->h : 16;
        if (y + h < view.y || y > view.y + view.h) continue;
        switch (op->kind) {
        case OP_TEXT:
            if (op->justify && op->w > 0) draw_justified(cv, x, y, op->w, op->text, op->len, op->color, op->bold);
            else if (op->center && op->w > 0) {
                int tw = (int)kc_utf8_len(op->text) * 8;
                kc_text_n(cv, x + (op->w - tw) / 2, y, op->text, (size_t)op->len, op->color);
            } else if (op->scale > 1) {
                char buf[256];
                size_t n = (size_t)op->len < sizeof buf - 1 ? (size_t)op->len : sizeof buf - 1;
                memcpy(buf, op->text, n);
                buf[n] = 0;
                kc_text_scaled(cv, x, y, buf, op->color, op->scale);
                if (op->bold) kc_text_scaled(cv, x + 1, y, buf, op->color, op->scale);
            } else {
                kc_text_n(cv, x, y, op->text, (size_t)op->len, op->color);
                if (op->bold) kc_text_n(cv, x + 1, y, op->text, (size_t)op->len, op->color);
            }
            break;
        case OP_GLYPH:
            kc_glyph(cv, x, y, op->cp, op->color, op->scale);
            if (op->bold) kc_glyph(cv, x + 1, y, op->cp, op->color, op->scale);
            break;
        case OP_HRULE: kc_fill(cv, kr(x, y, op->w, op->h), op->color); break;
        case OP_VRULE: kc_fill(cv, kr(x, y, 1, op->h), op->color); break;
        case OP_BOX:
            if (op->dashed) {
                for (int dx = 0; dx < op->w; dx += 6) {
                    int len = op->w - dx < 3 ? op->w - dx : 3;
                    kc_hline(cv, x + dx, y, len, op->color);
                    kc_hline(cv, x + dx, y + op->h - 1, len, op->color);
                }
                for (int dy = 0; dy < op->h; dy += 6) {
                    int len = op->h - dy < 3 ? op->h - dy : 3;
                    kc_vline(cv, x, y + dy, len, op->color);
                    kc_vline(cv, x + op->w - 1, y + dy, len, op->color);
                }
            } else kc_outline(cv, kr(x, y, op->w, op->h), op->color);
            break;
        case OP_ICON: k_icon(cv, (KIcon)op->cp, x, y, op->w); break;
        case OP_CUSTOM:
            if (op->draw) {
                KRect old = cv->clip;
                kc_set_clip(cv, kr_intersect(old, kr(x, y, op->w, op->h)));
                op->draw(ui, kr(x, y, op->w, op->h), op->data);
                cv->clip = old;
            }
            break;
        }
    }
}
