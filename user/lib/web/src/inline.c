/*
 * Inline layout: the inline formatting context of CSS 2.1 9.4.2 and 10.8 (line boxes, vertical-align, line-height), with
 * CSS Text 3 white space processing (4.1), text-transform, letter- and word-spacing, text-indent and text-align, line
 * breaking at the opportunities of UAX #14 that matter for HTML (spaces, CJK ideographs with the basic kinsoku rules,
 * hyphens, U+200B, soft hyphens, around atomic inlines; word-break and overflow-wrap), floats and positioned boxes met
 * in the inline content, the outside list marker, and the min-/max-content widths of inline content.
 *
 * The inline tree of a block container (inline boxes, text, atomic inlines) is flattened into "pieces": a text piece is
 * a run between two break opportunities (a word with the spaces after it), inline boxes give an open and a close piece
 * (their start and end margins, borders, paddings), atomic inlines one piece each. A "unit" is the run of pieces between
 * break opportunities; lines are filled greedily with whole units. Every placed piece leaves a fragment (Frag) in the
 * container; when a line ends, its height and baseline come from the vertical alignment of the inline boxes and atomic
 * inlines on it, and the fragments get their final positions.
 */
#include "layout.h"

enum { IP_TEXT = 1, IP_OPEN, IP_CLOSE, IP_ATOMIC, IP_BR, IP_FLOAT, IP_ABS, IP_TAB };
enum { BRK_NONE = 0, BRK_ALLOW, BRK_FORCED, BRK_SHY };

typedef struct Piece {
    uint8_t kind, brk;             /* brk: the opportunity after the piece */
    bool space_only;               /* text: collapsible spaces only (dropped at the start of a line) */
    LBox *box;
    uint32_t start, len;           /* text: bytes of box->text (a newline ending the piece is not included) */
    uint32_t tsp;                  /* text: bytes of trailing spaces (they hang at the end of a line) */
    WebUnit w, wmin, tw;           /* advance; min-content advance (atomic inlines); the trailing spaces' advance */
} Piece;

typedef struct IL {
    L *l;
    LBox *b;
    VEC(Piece) p;
    bool intrinsic;                /* collecting for min/max-content: atomic inlines are not laid out */
    WebUnit cw, cbh;
    bool space, word_start;        /* white space processing state across the boxes of the context */
    uint32_t prev_cp;
} IL;

typedef struct VMet { WebUnit A, D, LH, hl, xh, fs; } VMet;

static void vmet(L *l, const CStyle *s, VMet *m) {
    FontRef *f = box_font(l, s);
    m->fs = to_unit(s->inh->font_size);
    if (f) { m->A = f->m.ascent; m->D = f->m.descent; m->xh = f->m.x_height; }
    else { m->A = m->fs * 3 / 4; m->D = m->fs - m->A; m->xh = m->fs / 2; }
    m->LH = line_height_of(l, s);
    m->hl = (m->LH - (m->A + m->D)) / 2;
}

static bool collapsible(const LBox *t) {
    int ws = t->s->inh->white_space;
    return ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
}

static bool wraps(const CStyle *s) { int ws = s->inh->white_space; return ws != WS_NOWRAP && ws != WS_PRE; }

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static bool is_alnum(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 0xC0 && c != 0xD7 && c != 0xF7 &&
           !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F));
}

/* East Asian ideographs, kana, hangul, fullwidth forms: UAX #14 class ID (break opportunities on both sides) */
static bool cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFD);
}

/* no break before: closing punctuation, small kana, iteration and prolonged sound marks (kinsoku), combining marks */
static bool no_break_before(uint32_t c) {
    switch (c) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF01: case 0xFF09:
    case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3009: case 0x300B: case 0xFF3D: case 0xFF5D: case 0x30FC:
    case 0x3005: case 0x301C: case 0x30FB: case 0x2026: case 0x2025: case 0x3041: case 0x3043: case 0x3045: case 0x3047:
    case 0x3049: case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E: case 0x30A1: case 0x30A3: case 0x30A5:
    case 0x30A7: case 0x30A9: case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
    case 0x3099: case 0x309A: case 0x309B: case 0x309C: case 0x200D:
    case ')': case ']': case '}': case ',': case '.': case ';': case ':': case '!': case '?': case '%':
        return true;
    default:
        return (c >= 0x300 && c <= 0x36F) || (c >= 0xFE00 && c <= 0xFE0F);
    }
}

static bool no_break_after(uint32_t c) {
    switch (c) {
    case 0xFF08: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0x3008: case 0x300A: case 0xFF3B: case 0xFF5B:
    case '(': case '[': case '{': case 0x200D:
        return true;
    default:
        return false;
    }
}

/* a break opportunity between two characters that are not white space (word-break: 1 break-all, 2 keep-all) */
static bool brk_between(uint32_t a, uint32_t b, int wb) {
    if (!a || !b || no_break_before(b) || no_break_after(a)) return false;
    if (wb == 1) return true;
    if (cjk(a) && cjk(b)) return wb != 2;
    return cjk(a) || cjk(b);
}

static uint32_t first_cp(const char *s, uint32_t n) { size_t len; return n ? utf8_decode(s, n, &len) : 0; }

static uint32_t last_cp(const char *s, uint32_t n) {
    if (!n) return 0;
    uint32_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    size_t len;
    return utf8_decode(s + i, n - i, &len);
}

/* ================================================================================================ white space processing */

typedef struct TOut { char *p; uint32_t *map; uint32_t n, cap; WebArena *a; } TOut;

static void tput(TOut *t, char c, uint32_t src) {
    if (t->n >= t->cap) return;
    if (!t->map && src != t->n) {
        t->map = wa_alloc(t->a, sizeof(uint32_t) * (t->cap + 1));
        if (t->map) for (uint32_t k = 0; k < t->n; k++) t->map[k] = k;
    }
    if (t->map) t->map[t->n] = src;
    t->p[t->n++] = c;
}

/* text-transform for the scripts with simple one-to-one case pairs of the same UTF-8 length */
static uint32_t transform_cp(uint32_t cp, int tt, bool word_start) {
    if (tt == 1 || (tt == 3 && word_start)) {
        if (cp >= 'a' && cp <= 'z') return cp - 32;
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
        if (cp >= 0x3B1 && cp <= 0x3C9 && cp != 0x3C2) return cp - 32;
        if (cp >= 0x430 && cp <= 0x44F) return cp - 32;
    } else if (tt == 2) {
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
        if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 32;
        if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    }
    return cp;
}

static void text_state(IL *il, const LBox *t) {
    if (!t->tlen) return;
    uint32_t cp = last_cp(t->text, t->tlen);
    il->prev_cp = cp;
    il->space = collapsible(t) && (cp == ' ' || (cp == '\n' && t->s->inh->white_space == WS_PRE_LINE));
    if (cp == ' ' || cp == '\n' || cp == '\t') il->word_start = true;
    else if (is_alnum(cp)) il->word_start = false;
}

/* CSS Text 3 4.1.1 phase I, once per text box: collapsible white space runs become one space (or nothing after another
 * collapsible space, or between two East Asian characters across a line feed); pre-line keeps the line feeds; the
 * preserving values keep everything. Then text-transform. */
static void prepare_text(IL *il, LBox *t) {
    if (t->flags & BF_TEXT_READY) { text_state(il, t); return; }
    t->flags |= BF_TEXT_READY;
    const char *s = t->text;
    uint32_t n = t->tlen;
    int ws = t->s->inh->white_space, tt = t->s->inh->text_transform;
    bool coll = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    TOut o = { wa_alloc(il->l->a, n + 1), NULL, 0, n, il->l->a };
    if (!o.p) { text_state(il, t); return; }
    for (uint32_t i = 0; i < n;) {
        if (coll && is_ws(s[i])) {
            uint32_t j = i;
            bool nl = false;
            while (j < n && is_ws(s[j])) { if (s[j] == '\n') nl = true; j++; }
            if (ws == WS_PRE_LINE && nl) {
                for (uint32_t k = i; k < j; k++) if (s[k] == '\n') tput(&o, '\n', k);
                il->space = true;
                il->word_start = true;
                il->prev_cp = '\n';
            } else if (!il->space) {
                bool drop = nl && cjk(il->prev_cp) && j < n && cjk(first_cp(s + j, n - j));
                if (!drop) { tput(&o, ' ', i); il->prev_cp = ' '; il->space = true; }
                il->word_start = true;
            }
            i = j;
            continue;
        }
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        bool done = false;
        if (tt) {
            uint32_t tc = transform_cp(cp, tt, il->word_start);
            if (tc != cp) {
                char buf[4];
                int bn = utf8_encode(tc, buf);
                if ((size_t)bn == len) { for (int k = 0; k < bn; k++) tput(&o, buf[k], i + (uint32_t)k); done = true; }
            }
        }
        if (!done) for (size_t k = 0; k < len; k++) tput(&o, s[i + k], i + (uint32_t)k);
        if (cp == ' ' || cp == '\t' || cp == '\n') il->word_start = true;
        else if (is_alnum(cp)) il->word_start = false;
        il->space = false;
        il->prev_cp = cp;
        i += (uint32_t)len;
    }
    o.p[o.n] = 0;
    t->text = o.p;
    t->tlen = o.n;
    t->tmap = o.map;
}

/* ================================================================================================ pieces */

static WebUnit measure(L *l, const LBox *t, uint32_t start, uint32_t len) {
    if (!len) return 0;
    FontRef *f = box_font(l, t->s);
    const char *s = t->text + start;
    WebUnit w = f ? font_measure(l->d, f, s, len) : 0;
    float ls = t->s->inh->letter_spacing, wsp = t->s->inh->word_spacing;
    if (ls != 0 || wsp != 0) {
        int nc = 0, ns = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (((unsigned char)s[i] & 0xC0) != 0x80) nc++;
            if (s[i] == ' ') ns++;
        }
        w += to_unit(ls) * nc + to_unit(wsp) * ns;
    }
    return w;
}

static WebUnit space_adv(L *l, const LBox *t) {
    FontRef *f = box_font(l, t->s);
    return (f ? f->space_w : 0) + to_unit(t->s->inh->letter_spacing) + to_unit(t->s->inh->word_spacing);
}

static void push(IL *il, const Piece *p) { vec_push(il->l->a, il->p, *p); }

/* the break opportunity at the end of what was collected so far (before an atomic inline, between texts of two boxes) */
static void set_prev_brk(IL *il, uint8_t brk) {
    for (uint32_t i = il->p.n; i-- > 0;) {
        Piece *p = &il->p.v[i];
        if (p->kind == IP_OPEN || p->kind == IP_FLOAT || p->kind == IP_ABS) continue;
        if (p->brk == BRK_NONE) p->brk = brk;
        return;
    }
}

static void text_piece(IL *il, LBox *t, uint32_t start, uint32_t end, uint32_t tsp, uint8_t brk, bool space_only) {
    Piece p;
    memset(&p, 0, sizeof p);
    p.kind = IP_TEXT;
    p.box = t;
    p.start = start;
    p.len = end - start;
    p.tsp = tsp;
    p.brk = brk;
    p.space_only = space_only;
    if (tsp) {
        p.tw = (WebUnit)tsp * space_adv(il->l, t);
        p.w = measure(il->l, t, start, p.len - tsp) + p.tw;
    } else p.w = measure(il->l, t, start, p.len);
    p.wmin = p.w;
    push(il, &p);
}

static void text_pieces(IL *il, LBox *t) {
    uint32_t prev = il->prev_cp;
    prepare_text(il, t);
    const char *s = t->text;
    uint32_t n = t->tlen;
    if (!n) return;
    bool wrap = wraps(t->s), coll = collapsible(t);
    int wb = t->s->inh->word_break == 3 ? 0 : t->s->inh->word_break;
    if (wrap && prev && !is_ws((char)prev) && brk_between(prev, first_cp(s, n), wb)) set_prev_brk(il, BRK_ALLOW);
    uint32_t start = 0, i = 0, p1 = prev;
    while (i < n) {
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        uint32_t nx = i + (uint32_t)len;
        if (cp == '\n') {                                    /* a preserved line feed: a forced break */
            text_piece(il, t, start, i, 0, BRK_FORCED, false);
            il->p.v[il->p.n - 1].len = i - start;
            start = i = nx;
            p1 = cp;
            continue;
        }
        if (cp == '\t' && !coll) {
            if (i > start) text_piece(il, t, start, i, 0, BRK_NONE, false);
            Piece p;
            memset(&p, 0, sizeof p);
            p.kind = IP_TAB;
            p.box = t;
            p.start = i;
            p.len = 1;
            p.brk = wrap ? BRK_ALLOW : BRK_NONE;
            p.w = p.wmin = space_adv(il->l, t) * (t->s->inh->tab_size ? t->s->inh->tab_size : 8);
            push(il, &p);
            start = i = nx;
            p1 = cp;
            continue;
        }
        if (cp == ' ') {
            uint32_t j = nx;
            while (j < n && s[j] == ' ') j++;
            text_piece(il, t, start, j, j - i, wrap ? BRK_ALLOW : BRK_NONE, coll && start == i);
            start = i = j;
            p1 = ' ';
            continue;
        }
        uint8_t brk = BRK_NONE;
        if (nx >= n && wrap) brk = cp == 0x200B ? BRK_ALLOW : cp == 0xAD ? BRK_SHY : BRK_NONE;
        if (nx < n && wrap) {
            uint32_t nc = first_cp(s + nx, n - nx);
            if (cp == 0x200B) brk = BRK_ALLOW;
            else if (cp == 0xAD) brk = BRK_SHY;
            else if ((cp == '-' || cp == 0x2010) && is_alnum(p1) && is_alnum(nc)) brk = BRK_ALLOW;
            else if (cp == 0x2013 || cp == 0x2014) brk = BRK_ALLOW;
            else if (nc != ' ' && nc != '\n' && nc != '\t' && brk_between(cp, nc, wb)) brk = BRK_ALLOW;
        }
        if (brk) { text_piece(il, t, start, nx, 0, brk, false); start = nx; }
        p1 = cp;
        i = nx;
    }
    if (start < n) text_piece(il, t, start, n, 0, BRK_NONE, false);
}

static void layout_atomic(IL *il, LBox *a) {
    Space s;
    memset(&s, 0, sizeof s);
    s.cb_w = il->cw;
    s.cb_h = il->cbh;
    s.force_w = s.force_h = -1;
    s.shrink = true;
    BFC own;
    memset(&own, 0, sizeof own);
    own.a = il->l->a;
    s.bfc = &own;
    layout_box(il->l, a, &s);
}

static void collect(IL *il, LBox *parent, int depth) {
    L *l = il->l;
    if (depth > 200) return;
    for (LBox *c = parent->first; c; c = c->next) {
        Piece p;
        memset(&p, 0, sizeof p);
        p.box = c;
        if (c->flags & (BF_FLOAT | BF_ABS)) {
            p.kind = (c->flags & BF_FLOAT) ? IP_FLOAT : IP_ABS;
            if (il->intrinsic && p.kind == IP_FLOAT) box_contribution(l, c, &p.wmin, &p.w);
            push(il, &p);
            continue;
        }
        switch (c->kind) {
        case BK_TEXT:
            text_pieces(il, c);
            break;
        case BK_BR:
            p.kind = IP_BR;
            p.brk = BRK_FORCED;
            push(il, &p);
            il->space = true;
            il->prev_cp = '\n';
            break;
        case BK_INLINE:
            layout_compute_box_edges(l, c, il->cw);
            layout_relative(c, il->cw, il->cbh);
            c->ref = NULL;
            p.kind = IP_OPEN;
            p.w = p.wmin = c->m[3] + c->b[3] + c->p[3];
            push(il, &p);
            collect(il, c, depth + 1);
            memset(&p, 0, sizeof p);
            p.box = c;
            p.kind = IP_CLOSE;
            p.w = p.wmin = c->m[1] + c->b[1] + c->p[1];
            push(il, &p);
            break;
        default: {
            /* an atomic inline (replaced, inline-block, inline-table, inline-flex...): opportunities on both sides */
            bool wrap = wraps(parent->s);
            if (wrap) set_prev_brk(il, BRK_ALLOW);
            p.kind = IP_ATOMIC;
            if (il->intrinsic) box_contribution(l, c, &p.wmin, &p.w);
            else {
                layout_atomic(il, c);
                p.w = p.wmin = c->m[3] + c->w + c->m[1];
            }
            p.brk = wrap ? BRK_ALLOW : BRK_NONE;
            push(il, &p);
            il->space = false;
            il->prev_cp = 0xFFFC;
            break;
        }
        }
    }
}

/* the unit starting at piece i: through the first piece with an opportunity after it, and the closing pieces right after */
static uint32_t unit_end(const IL *il, uint32_t i, WebUnit *w, WebUnit *wmin, WebUnit *hang, uint8_t *brk) {
    uint32_t n = il->p.n, j = i;
    WebUnit sw = 0, sm = 0, hg = 0;
    uint8_t b = BRK_NONE;
    for (; j < n; j++) {
        const Piece *p = &il->p.v[j];
        if (p->kind == IP_FLOAT || p->kind == IP_ABS) continue;
        sw += p->w;
        sm += p->wmin;
        if (p->kind == IP_TEXT) hg = p->tw;
        else if (p->kind == IP_ATOMIC || p->kind == IP_TAB) hg = 0;
        if (p->brk) { b = p->brk; j++; break; }
    }
    while (j < n && il->p.v[j].kind == IP_CLOSE) { sw += il->p.v[j].w; sm += il->p.v[j].wmin; j++; }
    *w = sw;
    *wmin = sm;
    *hang = hg;
    *brk = b;
    return j;
}

/* ================================================================================================ lines */

typedef struct OpenBox { LBox *box; uint32_t frag; } OpenBox;
typedef struct AbsAt { LBox *box; WebUnit x; } AbsAt;

typedef struct LC {
    IL *il;
    L *l;
    LBox *b;
    const Space *sp;
    VEC(Frag) fr;
    VEC(Line) ln;
    OpenBox open[64];
    int nopen;
    WebUnit y, cl, cw, left, avail, x, indent, strut_lh;
    uint32_t f0;
    bool content, first_line;
    int32_t trail;                 /* the frag of the last text piece (its trailing spaces may hang), or -1 */
    WebUnit trail_tw;
    uint32_t trail_tsp;
    int32_t shy;                   /* the frag of a text piece that ended at a soft hyphen, when it is the last one */
    LBox *pending[32];             /* floats that go below the current line */
    int npending;
    AbsAt abs[32];
    int nabs;
} LC;

static void line_space(LC *c) {
    WebUnit l, r;
    const Space *sp = c->sp;
    float_avail(sp->bfc, c->y + sp->bfc_y, c->strut_lh > 0 ? c->strut_lh : 64, c->cl + sp->bfc_x, c->cl + c->cw + sp->bfc_x, &l, &r);
    c->left = l - sp->bfc_x;
    c->avail = r - l;
}

static Frag *new_frag(LC *c, int kind, LBox *box) {
    Frag f;
    memset(&f, 0, sizeof f);
    f.kind = (uint8_t)kind;
    f.box = box;
    f.line = (uint16_t)(c->ln.n > 65535 ? 65535 : c->ln.n);
    f.x = c->x;
    if (!vec_push(c->l->a, c->fr, f)) return NULL;
    return &c->fr.v[c->fr.n - 1];
}

static void line_begin(LC *c) {
    c->f0 = c->fr.n;
    c->x = c->first_line ? c->indent : 0;
    c->content = false;
    c->trail = c->shy = -1;
    c->nabs = 0;
    line_space(c);
    for (int i = 0; i < c->nopen; i++) {
        c->open[i].frag = c->fr.n;
        new_frag(c, FR_BOX, c->open[i].box);
    }
}

static void open_box(LC *c, LBox *ib, WebUnit w) {
    uint32_t idx = c->fr.n;
    Frag *f = new_frag(c, FR_BOX, ib);
    if (f) { f->flags = FRF_FIRST; f->x = c->x + ib->m[3]; }
    if (c->nopen < 64) { c->open[c->nopen].box = ib; c->open[c->nopen].frag = idx; c->nopen++; }
    c->x += w;
    if (w || ib->b[0] || ib->b[2] || ib->p[0] || ib->p[2]) c->content = true;
}

static void close_box(LC *c, LBox *ib, WebUnit w) {
    int k = c->nopen - 1;
    while (k >= 0 && c->open[k].box != ib) k--;
    WebUnit close_x = c->x + w - ib->m[1];
    c->x += w;
    if (w) c->content = true;
    if (k < 0) return;
    if (c->open[k].frag < c->fr.n) {
        Frag *f = &c->fr.v[c->open[k].frag];
        f->flags |= FRF_LAST;
        f->w = close_x;                                      /* a position until the line ends */
    }
    c->nopen = k;
}

static void put_text(LC *c, Piece *p) {
    L *l = c->l;
    if (!c->content && collapsible(p->box)) {
        /* collapsible spaces at the start of a line are removed */
        if (p->space_only) return;
        if (p->len && p->box->text[p->start] == ' ') {
            p->start++;
            p->len--;
            p->w -= space_adv(l, p->box);
            if (p->tsp > p->len) p->tsp = p->len;
        }
    }
    uint32_t idx = c->fr.n;
    Frag *f = new_frag(c, FR_TEXT, p->box);
    if (!f) return;
    f->start = p->start;
    f->len = p->len;
    f->w = p->w;
    c->x += p->w;
    if (!p->space_only || !collapsible(p->box)) c->content = true;
    c->trail = (int32_t)idx;
    c->trail_tw = p->tw;
    c->trail_tsp = p->tsp;
    c->shy = p->brk == BRK_SHY ? (int32_t)idx : -1;
}

static bool can_break_anywhere(const LBox *t) {
    return t->s->inh->overflow_wrap != 0 || t->s->inh->word_break == 3;
}

/* the bytes of p's text, from its start, that fit in room (whole characters) */
static uint32_t fit_prefix(L *l, const Piece *p, WebUnit room) {
    const char *s = p->box->text + p->start;
    WebUnit w = 0;
    uint32_t i = 0, best = 0;
    while (i < p->len) {
        size_t len;
        utf8_decode(s + i, p->len - i, &len);
        w += measure(l, p->box, p->start + i, (uint32_t)len);
        if (w > room) break;
        i += (uint32_t)len;
        best = i;
    }
    return best;
}

static void end_line(LC *c, bool justify);

/* text that may be broken anywhere when a word does not fit on a line by itself (overflow-wrap, word-break: break-word) */
static void put_text_wrapping(LC *c, Piece *p) {
    L *l = c->l;
    for (int guard = 0; guard < 100000; guard++) {
        if (c->x + p->w - p->tw <= c->avail || !can_break_anywhere(p->box) || p->len == 0) { put_text(c, p); return; }
        if (!c->content && collapsible(p->box) && p->box->text[p->start] == ' ') {
            p->start++; p->len--; p->w -= space_adv(l, p->box);
            continue;
        }
        uint32_t cut = fit_prefix(l, p, c->avail - c->x);
        if (cut == 0) {
            if (c->content) { end_line(c, true); line_begin(c); continue; }
            size_t len;
            utf8_decode(p->box->text + p->start, p->len, &len);
            cut = (uint32_t)len;
        }
        if (cut >= p->len - p->tsp) { put_text(c, p); return; }
        Piece head = *p;
        head.len = cut;
        head.tsp = 0;
        head.tw = 0;
        head.brk = BRK_ALLOW;
        head.w = measure(l, p->box, p->start, cut);
        put_text(c, &head);
        end_line(c, true);
        line_begin(c);
        p->start += cut;
        p->len -= cut;
        p->w = measure(l, p->box, p->start, p->len - p->tsp) + p->tw;
    }
}

static void put_tab(LC *c, Piece *p) {
    WebUnit sp = space_adv(c->l, p->box);
    WebUnit stop = sp * (p->box->s->inh->tab_size ? p->box->s->inh->tab_size : 8);
    if (stop <= 0) stop = 64;
    WebUnit x = c->x;
    WebUnit nx = (x / stop + 1) * stop;
    if (nx - x < sp / 2) nx += stop;
    Frag *f = new_frag(c, FR_TEXT, p->box);
    if (f) { f->start = p->start; f->len = 0; f->w = nx - x; }
    c->x = nx;
    c->content = true;
    c->trail = -1;
}

static void put_atomic(LC *c, Piece *p) {
    LBox *a = p->box;
    Frag *f = new_frag(c, FR_ATOMIC, a);
    if (f) { f->x = c->x + a->m[3]; f->w = a->w; }
    c->x += p->w;
    c->content = true;
    c->trail = -1;
    c->shy = -1;
}

static void inline_float(LC *c, LBox *f) {
    const Space *sp = c->sp;
    f->ref = c->b;
    float_layout(c->l, f, sp, c->cw);
    WebUnit mw = f->m[3] + f->w + f->m[1];
    if (!c->content || c->x + mw <= c->avail || c->npending >= 32) {
        float_position(c->l, f, sp, c->y + sp->bfc_y, c->cl + sp->bfc_x, c->cl + c->cw + sp->bfc_x);
        line_space(c);
    } else c->pending[c->npending++] = f;
}

static void inline_abs(LC *c, LBox *a) {
    a->ref = c->b;
    a->x = c->left + c->x;
    a->y = c->y;
    if (c->nabs < 32) { c->abs[c->nabs].box = a; c->abs[c->nabs].x = c->x; c->nabs++; }
    abs_defer(c->l, a);
}

static void place_unit(LC *c, uint32_t i, uint32_t k) {
    IL *il = c->il;
    for (uint32_t q = i; q < k; q++) {
        Piece p = il->p.v[q];
        switch (p.kind) {
        case IP_TEXT: put_text_wrapping(c, &p); break;
        case IP_TAB: put_tab(c, &p); break;
        case IP_OPEN: open_box(c, p.box, p.w); break;
        case IP_CLOSE: close_box(c, p.box, p.w); break;
        case IP_ATOMIC: put_atomic(c, &p); break;
        case IP_BR: c->content = true; c->trail = -1; break;
        case IP_FLOAT: inline_float(c, p.box); break;
        case IP_ABS: inline_abs(c, p.box); break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ vertical alignment */

/* the shift (down positive) of a box's baseline from its parent's, for vertical-align (CSS 2.1 10.8.1); above/below
 * are its extents around its baseline (for inline boxes the half-leading box of its line-height) */
static WebUnit align_shift(const VMet *pm, const CBox *bx, WebUnit above, WebUnit below) {
    switch (bx->va_kind) {
    case VA_SUB: return pm->fs / 5 + 64;
    case VA_SUPER: return -(pm->fs / 3 + 64);
    case VA_TEXT_TOP: return above - pm->A;
    case VA_TEXT_BOTTOM: return pm->D - below;
    case VA_MIDDLE: return -pm->xh / 2 - (below - above) / 2;
    case VA_LEN: return -to_unit(bx->va_len.px);
    default: return 0;
    }
}

/* the baseline shift of an inline box (or the container: 0) from the line's root baseline */
static WebUnit inline_shift(LC *c, const LBox *ib) {
    WebUnit s = 0;
    int guard = 0;
    for (const LBox *k = ib; k && k != c->b && k->kind == BK_INLINE && guard < 64; k = k->parent, guard++) {
        const LBox *p = k->parent;
        if (!p) break;
        VMet km, pm;
        vmet(c->l, k->s, &km);
        vmet(c->l, p->s, &pm);
        s += align_shift(&pm, k->s->box, km.A + km.hl, km.D + km.hl);
    }
    return s;
}

static void atomic_extents(const LBox *a, WebUnit *asc, WebUnit *desc) {
    WebUnit mh = a->m[0] + a->h + a->m[2];
    WebUnit as = (a->has_baseline && !(a->flags & BF_SCROLL)) ? a->m[0] + a->baseline : mh;
    *asc = as;
    *desc = mh - as;
}

static WebUnit atomic_shift(LC *c, const LBox *a, WebUnit asc, WebUnit desc) {
    const LBox *p = a->parent ? a->parent : c->b;
    VMet pm;
    vmet(c->l, p->s, &pm);
    return inline_shift(c, p) + align_shift(&pm, a->s->box, asc, desc);
}

static WebUnit marker_width(L *l, const LBox *li, const CStyle **ms) {
    const CStyle *s = li->node && is_el(li->node) && li->node->u.el.pstyle[2] ? li->node->u.el.pstyle[2] : li->s;
    *ms = s;
    FontRef *f = box_font(l, s);
    if (!f) return 0;
    if (li->marker) return font_measure(l->d, f, li->marker, strlen(li->marker));
    return font_measure(l->d, f, "\xE2\x80\xA2 ", 4);
}

static void end_line(LC *c, bool justify) {
    L *l = c->l;
    LBox *b = c->b;
    uint32_t n = c->fr.n;
    if (!c->content) {
        /* a line with nothing that needs one (white space, empty inline boxes): it takes no height */
        c->fr.n = c->f0;
        for (int i = 0; i < c->nabs; i++) { c->abs[i].box->x = c->left; c->abs[i].box->y = c->y; }
        for (int i = 0; i < c->npending; i++)
            float_position(l, c->pending[i], c->sp, c->y + c->sp->bfc_y, c->cl + c->sp->bfc_x, c->cl + c->cw + c->sp->bfc_x);
        c->npending = 0;
        return;
    }
    Frag *fr = c->fr.v;
    /* the soft hyphen the line ends at shows a hyphen */
    if (c->shy >= (int32_t)c->f0 && c->shy == c->trail) {
        Frag *t = &fr[c->shy];
        FontRef *f = box_font(l, t->box->s);
        WebUnit hw = f ? font_measure(l->d, f, "-", 1) : 0;
        t->flags |= FRF_HYPHEN;
        t->w += hw;
        c->x += hw;
    }
    /* trailing spaces hang: they are removed from the line (and from the boxes that end after them) */
    WebUnit cut = 0, at = INT32_MAX;
    if (c->trail >= (int32_t)c->f0 && c->trail_tw > 0) {
        Frag *t = &fr[c->trail];
        at = t->x + t->w;
        t->w -= c->trail_tw;
        t->len -= c->trail_tsp <= t->len ? c->trail_tsp : t->len;
        cut = c->trail_tw;
    }
    for (uint32_t i = c->f0; i < n; i++) {
        Frag *f = &fr[i];
        if ((int32_t)i != c->trail && f->x >= at) f->x -= cut;
        if (f->kind == FR_BOX) {
            WebUnit end = (f->flags & FRF_LAST) ? f->w : c->x;
            if (end >= at) end -= cut;
            f->w = end - f->x;
            if (f->w < 0) f->w = 0;
        }
    }
    if (c->x >= at) c->x -= cut;
    for (int i = 0; i < c->nabs; i++) if (c->abs[i].x >= at) c->abs[i].x -= cut;
    /* the heights: the strut of the container, the inline boxes and the atomic inlines on the line */
    VMet rm;
    vmet(l, b->s, &rm);
    WebUnit mint = -rm.A - rm.hl, maxb = mint + rm.LH;
    for (uint32_t i = c->f0; i < n; i++) {
        Frag *f = &fr[i];
        if (f->kind == FR_BOX) {
            int va = f->box->s->box->va_kind;
            if (va == VA_TOP || va == VA_BOTTOM) continue;
            VMet m;
            vmet(l, f->box->s, &m);
            WebUnit s = inline_shift(c, f->box);
            f->baseline = s;
            WebUnit top = s - m.A - m.hl;
            if (top < mint) mint = top;
            if (top + m.LH > maxb) maxb = top + m.LH;
        } else if (f->kind == FR_ATOMIC) {
            int va = f->box->s->box->va_kind;
            if (va == VA_TOP || va == VA_BOTTOM) continue;
            WebUnit asc, desc;
            atomic_extents(f->box, &asc, &desc);
            WebUnit s = atomic_shift(c, f->box, asc, desc);
            f->baseline = s;
            if (s - asc < mint) mint = s - asc;
            if (s + desc > maxb) maxb = s + desc;
        }
    }
    for (int pass = 0; pass < 2; pass++)                     /* top-aligned boxes, then bottom-aligned ones */
        for (uint32_t i = c->f0; i < n; i++) {
            Frag *f = &fr[i];
            if (f->kind != FR_BOX && f->kind != FR_ATOMIC) continue;
            int va = f->box->s->box->va_kind;
            if (va != (pass ? VA_BOTTOM : VA_TOP)) continue;
            WebUnit h;
            if (f->kind == FR_BOX) { VMet m; vmet(l, f->box->s, &m); h = m.LH; }
            else h = f->box->m[0] + f->box->h + f->box->m[2];
            if (pass == 0 && mint + h > maxb) maxb = mint + h;
            if (pass == 1 && maxb - h < mint) mint = maxb - h;
        }
    WebUnit line_h = maxb - mint, base = c->y - mint;
    /* text-align */
    WebUnit free_w = c->avail - c->x;
    if (free_w < 0) free_w = 0;
    int ta = b->s->inh->text_align;
    bool rtl = b->s->inh->direction != 0;
    WebUnit off = 0;
    switch (ta) {
    case TA_RIGHT: off = free_w; break;
    case TA_CENTER: case TA_WEBKIT_CENTER: off = free_w / 2; break;
    case TA_END: off = rtl ? 0 : free_w; break;
    case TA_JUSTIFY: case TA_START: case TA_LEFT: default: off = (ta == TA_START || ta == TA_JUSTIFY) && rtl ? free_w : 0; break;
    }
    bool just = ta == TA_JUSTIFY && justify && free_w > 0;
    int nsp = 0;
    if (just) {
        for (uint32_t i = c->f0; i < n; i++)
            if (fr[i].kind == FR_TEXT)
                for (uint32_t k = 0; k < fr[i].len; k++) if (fr[i].box->text[fr[i].start + k] == ' ') nsp++;
        if (!nsp) just = false;
    }
    if (just) {
        /* the free space goes to the spaces, which end the text fragments (each a word): later fragments move */
        WebUnit per = free_w / nsp, rem = free_w - per * nsp;
        WebUnit ends[512], adds[512];
        int ne = 0;
        int given = 0;
        for (uint32_t i = c->f0; i < n && ne < 512; i++) {
            if (fr[i].kind != FR_TEXT) continue;
            int k = 0;
            for (uint32_t q = 0; q < fr[i].len; q++) if (fr[i].box->text[fr[i].start + q] == ' ') k++;
            if (!k) continue;
            WebUnit add = per * k;
            for (int q = 0; q < k; q++) if (given < rem) { add++; given++; }
            ends[ne] = fr[i].x + fr[i].w;
            adds[ne] = add;
            ne++;
        }
        for (uint32_t i = c->f0; i < n; i++) {
            Frag *f = &fr[i];
            WebUnit sx = 0, se = 0, end = f->x + f->w;
            for (int e = 0; e < ne; e++) {
                if (ends[e] <= f->x) sx += adds[e];
                if (ends[e] <= end) se += adds[e];
            }
            f->x += sx;
            f->w += se - sx;
        }
        for (int i = 0; i < c->nabs; i++) {
            WebUnit sx = 0;
            for (int e = 0; e < ne; e++) if (ends[e] <= c->abs[i].x) sx += adds[e];
            c->abs[i].x += sx;
        }
    } else {
        /* adjacent fragments of one text box merge into one run */
        uint32_t w = c->f0;
        for (uint32_t i = c->f0; i < n; i++) {
            Frag *f = &fr[i];
            if (w > c->f0) {
                Frag *p = &fr[w - 1];
                if (p->kind == FR_TEXT && f->kind == FR_TEXT && p->box == f->box && p->start + p->len == f->start &&
                    p->x + p->w == f->x && !(p->flags & FRF_HYPHEN) && f->len) {
                    p->len += f->len;
                    p->w += f->w;
                    p->flags |= f->flags;
                    continue;
                }
            }
            if (w != i) fr[w] = *f;
            w++;
        }
        n = w;
        c->fr.n = n;
    }
    /* final positions, relative to the container's border box */
    for (uint32_t i = c->f0; i < n; i++) {
        Frag *f = &fr[i];
        f->x += c->left + off;
        if (f->kind == FR_TEXT) {
            VMet m;
            vmet(l, f->box->s, &m);
            f->baseline = base + inline_shift(c, f->box->parent);
            f->y = f->baseline - m.A;
            f->h = m.A + m.D;
        } else if (f->kind == FR_BOX) {
            VMet m;
            vmet(l, f->box->s, &m);
            int va = f->box->s->box->va_kind;
            WebUnit s = va == VA_TOP ? mint + m.A + m.hl : va == VA_BOTTOM ? maxb - m.D - m.hl : f->baseline;
            f->baseline = base + s;
            f->y = f->baseline - m.A - f->box->p[0] - f->box->b[0];
            f->h = m.A + m.D + f->box->p[0] + f->box->p[2] + f->box->b[0] + f->box->b[2];
        } else if (f->kind == FR_ATOMIC) {
            LBox *a = f->box;
            WebUnit asc, desc;
            atomic_extents(a, &asc, &desc);
            int va = a->s->box->va_kind;
            WebUnit s = va == VA_TOP ? mint + asc : va == VA_BOTTOM ? maxb - desc : f->baseline;
            a->ref = b;
            a->x = f->x;
            a->y = base + s - asc + a->m[0];
            f->baseline = base + s;
            f->y = a->y;
            f->h = a->h;
        }
    }
    for (int i = 0; i < c->nabs; i++) { c->abs[i].box->x = c->left + off + c->abs[i].x; c->abs[i].box->y = c->y; }
    /* the marker of a list item goes beside its first line box */
    LBox *li = l->marker;
    if (li && !(li->flags & BF_MARKER_DONE)) {
        const CStyle *ms;
        WebUnit mw = marker_width(l, li, &ms);
        VMet m;
        vmet(l, ms, &m);
        Frag *f = new_frag(c, FR_MARKER, li);
        if (f) {
            fr = c->fr.v;
            f->x = c->cl - mw;
            f->w = mw;
            f->baseline = base;
            f->y = base - m.A;
            f->h = m.A + m.D;
            n = c->fr.n;
        }
        li->flags |= BF_MARKER_DONE;
        l->marker = NULL;
    }
    Line ln;
    ln.y = c->y;
    ln.h = line_h;
    ln.baseline = base;
    ln.left = c->left;
    ln.width = c->avail;
    ln.first = c->f0;
    ln.n = n - c->f0;
    vec_push(l->a, c->ln, ln);
    c->y += line_h;
    c->first_line = false;
    for (int i = 0; i < c->npending; i++)
        float_position(l, c->pending[i], c->sp, c->y + c->sp->bfc_y, c->cl + c->sp->bfc_x, c->cl + c->cw + c->sp->bfc_x);
    c->npending = 0;
}

/* when nothing fits beside the floats, the line moves down past them (CSS 2.1 9.5) */
static void avoid_floats(LC *c, WebUnit need) {
    const Space *sp = c->sp;
    for (int guard = 0; guard < 1000 && c->x + need > c->avail && c->avail < c->cw; guard++) {
        WebUnit nb = float_next_bottom(sp->bfc, c->y + sp->bfc_y, c->strut_lh > 0 ? c->strut_lh : 64);
        if (nb == INT32_MAX) break;
        c->y = nb - sp->bfc_y;
        line_space(c);
    }
}

void layout_inline(L *l, LBox *b, const Space *sp) {
    IL il;
    memset(&il, 0, sizeof il);
    il.l = l;
    il.b = b;
    il.word_start = true;
    il.cw = box_content_w(b);
    il.cbh = sp->cb_h;
    collect(&il, b, 0);
    LC c;
    memset(&c, 0, sizeof c);
    c.il = &il;
    c.l = l;
    c.b = b;
    c.sp = sp;
    c.cl = b->b[3] + b->p[3];
    c.cw = il.cw;
    c.y = b->b[0] + b->p[0];
    c.first_line = true;
    c.indent = resolve_len_or(&b->s->inh->text_indent, il.cw, 0);
    VMet sm;
    vmet(l, b->s, &sm);
    c.strut_lh = sm.LH;
    if (b->frags && b->cfrags) wa_free(l->a, b->frags, sizeof(Frag) * b->cfrags);
    if (b->lines && b->clines) wa_free(l->a, b->lines, sizeof(Line) * b->clines);
    b->frags = NULL;
    b->lines = NULL;
    b->nfrags = b->nlines = b->cfrags = b->clines = 0;
    line_begin(&c);
    for (uint32_t i = 0; i < il.p.n;) {
        WebUnit uw, um, hang;
        uint8_t brk;
        uint32_t k = unit_end(&il, i, &uw, &um, &hang, &brk);
        WebUnit fit = uw - hang;
        if (c.content && c.x + fit > c.avail) { end_line(&c, true); line_begin(&c); }
        if (!c.content && c.x + fit > c.avail && c.avail < c.cw) avoid_floats(&c, fit);
        place_unit(&c, i, k);
        if (brk == BRK_FORCED) { end_line(&c, false); line_begin(&c); }
        i = k;
    }
    end_line(&c, false);
    b->frags = c.fr.v;
    b->nfrags = c.fr.n;
    b->cfrags = c.fr.cap;
    b->lines = c.ln.v;
    b->nlines = c.ln.n;
    b->clines = c.ln.cap;
    if (b->nlines) {
        const Line *ln = (b->flags & BF_INLINE_LEVEL) ? &b->lines[b->nlines - 1] : &b->lines[0];
        b->baseline = ln->baseline;
        b->has_baseline = true;
    }
    vec_free(l->a, il.p);
}

/* ================================================================================================ intrinsic widths */

void inline_intrinsic(L *l, LBox *b, WebUnit *mn, WebUnit *mx) {
    IL il;
    memset(&il, 0, sizeof il);
    il.l = l;
    il.b = b;
    il.word_start = true;
    il.intrinsic = true;
    collect(&il, b, 0);
    const CLen *ti = &b->s->inh->text_indent;
    WebUnit indent = ti->kind == CL_PX && ti->pct == 0 ? to_unit(ti->px) : 0;
    WebUnit best_min = 0, best_max = 0, line = indent, last_hang = 0;
    bool started = false;
    for (uint32_t i = 0; i < il.p.n;) {
        const Piece *p = &il.p.v[i];
        if (!started && p->kind == IP_TEXT && p->space_only) { i++; continue; }
        WebUnit uw, um, hang;
        uint8_t brk;
        uint32_t k = unit_end(&il, i, &uw, &um, &hang, &brk);
        for (uint32_t q = i; q < k; q++) {
            const Piece *f = &il.p.v[q];
            if (f->kind != IP_FLOAT) continue;
            if (f->wmin > best_min) best_min = f->wmin;
            line += f->w;
        }
        if (um - hang > best_min) best_min = um - hang;
        line += uw;
        started = true;
        last_hang = hang;
        if (brk == BRK_FORCED) {
            if (line - hang > best_max) best_max = line - hang;
            line = 0;
            last_hang = 0;
            started = false;
        }
        i = k;
    }
    if (line - last_hang > best_max) best_max = line - last_hang;
    if (best_max < best_min) best_max = best_min;
    *mn = best_min;
    *mx = best_max;
    vec_free(l->a, il.p);
}
