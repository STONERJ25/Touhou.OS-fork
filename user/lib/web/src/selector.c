/*
 * Selectors: parsing (Selectors Level 3, and from Level 4 the selector-list arguments of :not(), :is(), :where() with
 * forgiving parsing, :has() with relative selectors, :nth-child(An+B of S), the attribute case flags, :focus-within,
 * :focus-visible, :any-link, :placeholder-shown, :read-only/:read-write, :defined, :open), specificity, and matching.
 *
 * Matching goes right to left: the subject compound first, then each combinator walks to parents or previous siblings
 * (backtracking for the descendant and subsequent-sibling combinators). HTML rules apply: element and attribute names
 * are ASCII case-insensitive on HTML elements, the values of a list of legacy attributes compare case-insensitively,
 * and class and id selectors are case-insensitive in quirks mode. :visited never matches (privacy), :hover/:active/
 * :focus/:target follow the document's interaction state.
 */
#include "css.h"

#include <stdlib.h>

/* ================================================================================================ parsing */

typedef struct {
    WebDoc *d;
    WebArena *a;
    const CssTokens *t;
    uint32_t i, end;
    bool fail;
    int depth;
} SelP;

static const CssTok *sp_peek(SelP *p) { return p->i < p->end ? &p->t->v[p->i] : NULL; }
static bool sp_ws(SelP *p) {
    bool any = false;
    while (p->i < p->end && p->t->v[p->i].type == CT_WS) { p->i++; any = true; }
    return any;
}
static const char *sv(SelP *p, const CssTok *k) { return tok_val(p->t, k); }

static WebAtom lower_atom(SelP *p, const char *s, size_t n) { return atom_get_lower(&p->d->atoms, s, n); }

typedef struct {
    SelCompound c;
    VEC(WebAtom) classes;
    VEC(SelAttr) attrs;
    VEC(SelPseudo) pseudos;
} CompBuild;

static bool parse_list(SelP *p, SelList *out, bool forgiving, bool relative);

static bool nth_text(const char *s, size_t n, int *a, int *b) {
    char buf[64];
    size_t k = 0;
    for (size_t i = 0; i < n && k < sizeof buf - 1; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n') buf[k++] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    buf[k] = 0;
    if (!strcmp(buf, "odd")) { *a = 2; *b = 1; return true; }
    if (!strcmp(buf, "even")) { *a = 2; *b = 0; return true; }
    char *nn = strchr(buf, 'n');
    if (!nn) {
        char *e;
        long v = strtol(buf, &e, 10);
        if (e == buf || *e) return false;
        *a = 0;
        *b = (int)v;
        return true;
    }
    /* [+-]?digits? n ([+-] digits)? */
    if (nn == buf) *a = 1;
    else if (nn == buf + 1 && (buf[0] == '+' || buf[0] == '-')) *a = buf[0] == '-' ? -1 : 1;
    else {
        char *e;
        long v = strtol(buf, &e, 10);
        if (e != nn) return false;
        *a = (int)v;
    }
    char *rest = nn + 1;
    if (!*rest) { *b = 0; return true; }
    if (*rest != '+' && *rest != '-') return false;
    if (!rest[1] || rest[1] < '0' || rest[1] > '9') return false;
    char *e;
    long v = strtol(rest, &e, 10);
    if (*e) return false;
    *b = (int)v;
    return true;
}

static const struct { const char *name; uint8_t pc; } pseudo_classes[] = {
    {"root", PC_ROOT}, {"first-child", PC_FIRST_CHILD}, {"last-child", PC_LAST_CHILD}, {"only-child", PC_ONLY_CHILD},
    {"first-of-type", PC_FIRST_OF_TYPE}, {"last-of-type", PC_LAST_OF_TYPE}, {"only-of-type", PC_ONLY_OF_TYPE},
    {"empty", PC_EMPTY}, {"link", PC_LINK}, {"visited", PC_VISITED}, {"any-link", PC_ANY_LINK},
    {"-webkit-any-link", PC_ANY_LINK}, {"hover", PC_HOVER}, {"active", PC_ACTIVE}, {"focus", PC_FOCUS},
    {"focus-within", PC_FOCUS_WITHIN}, {"focus-visible", PC_FOCUS_VISIBLE}, {"target", PC_TARGET},
    {"checked", PC_CHECKED}, {"disabled", PC_DISABLED}, {"enabled", PC_ENABLED}, {"required", PC_REQUIRED},
    {"optional", PC_OPTIONAL}, {"read-only", PC_READ_ONLY}, {"read-write", PC_READ_WRITE},
    {"placeholder-shown", PC_PLACEHOLDER_SHOWN}, {"default", PC_DEFAULT}, {"indeterminate", PC_INDETERMINATE},
    {"scope", PC_SCOPE}, {"defined", PC_DEFINED}, {"open", PC_OPEN}, {"popover-open", PC_NEVER}, {"modal", PC_NEVER},
    {"fullscreen", PC_NEVER}, {"autofill", PC_NEVER}, {"-webkit-autofill", PC_NEVER}, {"user-invalid", PC_NEVER},
    {"invalid", PC_NEVER}, {"valid", PC_DEFINED}, {"in-range", PC_DEFINED}, {"out-of-range", PC_NEVER},
    {"user-valid", PC_NEVER}, {"playing", PC_NEVER}, {"paused", PC_NEVER}, {"target-within", PC_NEVER},
    {"blank", PC_EMPTY}, {"local-link", PC_NEVER}, {"current", PC_NEVER}, {"past", PC_NEVER}, {"future", PC_NEVER},
};

static const struct { const char *name; uint8_t pe; bool legacy; } pseudo_elements[] = {
    {"before", PSE_BEFORE, true}, {"after", PSE_AFTER, true}, {"first-line", PSE_FIRST_LINE, true},
    {"first-letter", PSE_FIRST_LETTER, true}, {"marker", PSE_MARKER, false}, {"placeholder", PSE_PLACEHOLDER, false},
    {"selection", PSE_SELECTION, false}, {"backdrop", PSE_OTHER, false}, {"file-selector-button", PSE_OTHER, false},
    {"cue", PSE_OTHER, false}, {"-webkit-input-placeholder", PSE_PLACEHOLDER, false},
    {"-moz-placeholder", PSE_PLACEHOLDER, false}, {"-ms-input-placeholder", PSE_PLACEHOLDER, false},
    {"-webkit-scrollbar", PSE_OTHER, false}, {"-webkit-scrollbar-thumb", PSE_OTHER, false},
    {"-webkit-scrollbar-track", PSE_OTHER, false}, {"-webkit-details-marker", PSE_OTHER, false},
    {"-moz-selection", PSE_SELECTION, false}, {"details-content", PSE_OTHER, false}, {"picker-icon", PSE_OTHER, false},
    {"checkmark", PSE_OTHER, false},
};

static bool parse_compound(SelP *p, CompBuild *b, uint8_t *pseudo_el, uint32_t *spec) {
    memset(&b->c, 0, sizeof b->c);
    bool any = false;
    const CssTok *k = sp_peek(p);
    if (!k) return false;
    /* type selector, universal, with an optional namespace prefix (ns|E, *|E, |E: the namespace is ignored) */
    if (k->type == CT_IDENT || (k->type == CT_DELIM && (k->delim == '*' || k->delim == '|'))) {
        uint32_t save = p->i;
        const CssTok *first = k;
        p->i++;
        const CssTok *bar = sp_peek(p);
        if (bar && bar->type == CT_DELIM && bar->delim == '|' && !(first->type == CT_DELIM && first->delim == '|')) {
            p->i++;
            const CssTok *nm = sp_peek(p);
            if (nm && (nm->type == CT_IDENT || (nm->type == CT_DELIM && nm->delim == '*'))) { p->i++; first = nm; }
            else { p->i = save; return false; }
        } else if (first->type == CT_DELIM && first->delim == '|') {
            const CssTok *nm = sp_peek(p);
            if (nm && (nm->type == CT_IDENT || (nm->type == CT_DELIM && nm->delim == '*'))) { p->i++; first = nm; }
            else return false;
        }
        if (first->type == CT_IDENT) {
            b->c.tag = lower_atom(p, sv(p, first), first->vn);
            b->c.tag_cs = atom_get(&p->d->atoms, sv(p, first), first->vn);
            *spec += 1;
        }
        any = true;
    }
    for (;;) {
        k = sp_peek(p);
        if (!k) break;
        /* after a pseudo-element only pseudo-classes may follow (::before:hover) */
        if (*pseudo_el && (k->type == CT_HASH || k->type == CT_LBRACK || (k->type == CT_DELIM && k->delim == '.'))) return false;
        if (k->type == CT_HASH) {
            if (!(k->flags & CTF_ID)) return false;
            p->i++;
            b->c.id = atom_get(&p->d->atoms, sv(p, k), k->vn);
            *spec += 1 << 20;
            any = true;
            continue;
        }
        if (k->type == CT_DELIM && k->delim == '.') {
            p->i++;
            const CssTok *nm = sp_peek(p);
            if (!nm || nm->type != CT_IDENT) return false;
            p->i++;
            vec_push(p->a, b->classes, atom_get(&p->d->atoms, sv(p, nm), nm->vn));
            *spec += 1 << 10;
            any = true;
            continue;
        }
        if (k->type == CT_LBRACK) {
            SelAttr at;
            memset(&at, 0, sizeof at);
            uint32_t j = p->i + 1, e = k->close;
            while (j < e && p->t->v[j].type == CT_WS) j++;
            /* [ns|attr]: skip a namespace prefix (but [attr|=value] is the dash-match operator) */
            if (j + 2 < e && p->t->v[j + 1].type == CT_DELIM && p->t->v[j + 1].delim == '|' && p->t->v[j + 2].type == CT_IDENT &&
                (p->t->v[j].type == CT_IDENT || (p->t->v[j].type == CT_DELIM && p->t->v[j].delim == '*'))) j += 2;
            else if (j + 1 < e && p->t->v[j].type == CT_DELIM && p->t->v[j].delim == '|' && p->t->v[j + 1].type == CT_IDENT) j++;
            if (j >= e || p->t->v[j].type != CT_IDENT) return false;
            at.name = lower_atom(p, sv(p, &p->t->v[j]), p->t->v[j].vn);
            j++;
            while (j < e && p->t->v[j].type == CT_WS) j++;
            if (j < e) {
                const CssTok *op = &p->t->v[j];
                if (op->type != CT_DELIM) return false;
                int ch = op->delim;
                if (ch == '=') at.op = SA_EQ;
                else {
                    if (j + 1 >= e || p->t->v[j + 1].type != CT_DELIM || p->t->v[j + 1].delim != '=') return false;
                    at.op = ch == '~' ? SA_INCLUDES : ch == '|' ? SA_DASH : ch == '^' ? SA_PREFIX : ch == '$' ? SA_SUFFIX :
                            ch == '*' ? SA_SUBSTR : 255;
                    if (at.op == 255) return false;
                    j++;
                }
                j++;
                while (j < e && p->t->v[j].type == CT_WS) j++;
                if (j >= e || (p->t->v[j].type != CT_IDENT && p->t->v[j].type != CT_STRING)) return false;
                at.val = wa_strndup(&p->d->arena, sv(p, &p->t->v[j]), p->t->v[j].vn);
                at.vlen = p->t->v[j].vn;
                j++;
                while (j < e && p->t->v[j].type == CT_WS) j++;
                if (j < e && p->t->v[j].type == CT_IDENT && p->t->v[j].vn == 1) {
                    char f = (char)(sv(p, &p->t->v[j])[0] | 0x20);
                    if (f == 'i') at.ci = true;
                    else if (f != 's') return false;
                    j++;
                    while (j < e && p->t->v[j].type == CT_WS) j++;
                }
                if (j < e) return false;
            }
            p->i = e + 1;
            vec_push(p->a, b->attrs, at);
            *spec += 1 << 10;
            any = true;
            continue;
        }
        if (k->type == CT_COLON) {
            p->i++;
            const CssTok *nm = sp_peek(p);
            bool element = false;
            if (nm && nm->type == CT_COLON) { element = true; p->i++; nm = sp_peek(p); }
            if (!nm || (nm->type != CT_IDENT && nm->type != CT_FUNCTION)) return false;
            const char *s = sv(p, nm);
            size_t n = nm->vn;
            /* pseudo-elements (and the legacy one-colon forms of the CSS 2 ones) */
            int pe = -1;
            for (size_t i = 0; i < sizeof pseudo_elements / sizeof pseudo_elements[0]; i++)
                if (strlen(pseudo_elements[i].name) == n && strncasecmp(pseudo_elements[i].name, s, n) == 0 &&
                    (element || pseudo_elements[i].legacy)) { pe = pseudo_elements[i].pe; break; }
            if (pe >= 0 && nm->type == CT_IDENT) {
                p->i++;
                if (*pseudo_el) return false;
                *pseudo_el = (uint8_t)pe;
                *spec += 1;
                any = true;
                continue;
            }
            if (element) {
                if (nm->type == CT_FUNCTION && n == 4 && strncasecmp(s, "part", 4) == 0) { p->i = nm->close + 1; *pseudo_el = PSE_OTHER; any = true; continue; }
                if (nm->type == CT_FUNCTION && n == 7 && strncasecmp(s, "slotted", 7) == 0) { p->i = nm->close + 1; *pseudo_el = PSE_OTHER; any = true; continue; }
                return false;
            }
            SelPseudo ps;
            memset(&ps, 0, sizeof ps);
            if (nm->type == CT_IDENT) {
                p->i++;
                ps.type = PC_NONE;
                for (size_t i = 0; i < sizeof pseudo_classes / sizeof pseudo_classes[0]; i++)
                    if (strlen(pseudo_classes[i].name) == n && strncasecmp(pseudo_classes[i].name, s, n) == 0) { ps.type = pseudo_classes[i].pc; break; }
                if (ps.type == PC_NONE) {
                    if (n > 1 && s[0] == '-') ps.type = PC_NEVER;     /* vendor pseudo-classes: valid here, never match */
                    else return false;
                }
                vec_push(p->a, b->pseudos, ps);
                *spec += 1 << 10;
                any = true;
                continue;
            }
            /* functional pseudo-classes */
            uint32_t in = (uint32_t)(nm - p->t->v) + 1, close = nm->close;
            p->i = close + 1;
            any = true;
            bool is_nth = (n == 9 && strncasecmp(s, "nth-child", 9) == 0) || (n == 14 && strncasecmp(s, "nth-last-child", 14) == 0) ||
                          (n == 11 && strncasecmp(s, "nth-of-type", 11) == 0) || (n == 16 && strncasecmp(s, "nth-last-of-type", 16) == 0) ||
                          (n == 7 && strncasecmp(s, "nth-col", 7) == 0) || (n == 12 && strncasecmp(s, "nth-last-col", 12) == 0);
            if (is_nth) {
                ps.type = n == 9 ? PC_NTH_CHILD : n == 14 ? PC_NTH_LAST_CHILD : n == 11 ? PC_NTH_OF_TYPE :
                          n == 16 ? PC_NTH_LAST_OF_TYPE : PC_NEVER;
                /* "An+B" or "An+B of S" */
                uint32_t of = close;
                for (uint32_t j = in; j < close; j++) if (tok_is_ident(p->t, &p->t->v[j], "of")) { of = j; break; }
                uint32_t ts = in, te = of;
                while (ts < te && p->t->v[ts].type == CT_WS) ts++;
                while (te > ts && p->t->v[te - 1].type == CT_WS) te--;
                if (ts >= te) return false;
                const char *src = p->t->src + p->t->v[ts].pos;
                size_t len = p->t->v[te - 1].end - p->t->v[ts].pos;
                if (!nth_text(src, len, &ps.a, &ps.b)) return false;
                if (of < close) {
                    if (ps.type != PC_NTH_CHILD && ps.type != PC_NTH_LAST_CHILD) return false;
                    SelP sub = *p;
                    sub.i = of + 1;
                    sub.end = close;
                    sub.depth = p->depth + 1;
                    ps.list = wa_alloc(p->a, sizeof(SelList));
                    if (!ps.list || sub.depth > 16 || !parse_list(&sub, ps.list, false, false)) return false;
                    uint32_t mx = 0;
                    for (uint32_t k2 = 0; k2 < ps.list->n; k2++) if (ps.list->v[k2].spec > mx) mx = ps.list->v[k2].spec;
                    *spec += mx;
                }
                vec_push(p->a, b->pseudos, ps);
                *spec += 1 << 10;
                continue;
            }
            bool is_not = n == 3 && strncasecmp(s, "not", 3) == 0;
            bool is_is = (n == 2 && strncasecmp(s, "is", 2) == 0) || (n == 3 && strncasecmp(s, "any", 3) == 0) ||
                         (n == 11 && strncasecmp(s, "-webkit-any", 11) == 0) || (n == 8 && strncasecmp(s, "-moz-any", 8) == 0) ||
                         (n == 7 && strncasecmp(s, "matches", 7) == 0);
            bool is_where = n == 5 && strncasecmp(s, "where", 5) == 0;
            bool is_has = n == 3 && strncasecmp(s, "has", 3) == 0;
            if (is_not || is_is || is_where || is_has) {
                ps.type = is_not ? PC_NOT : is_where ? PC_WHERE : is_has ? PC_HAS : PC_IS;
                SelP sub = *p;
                sub.i = in;
                sub.end = close;
                sub.depth = p->depth + 1;
                ps.list = wa_alloc(p->a, sizeof(SelList));
                if (!ps.list || sub.depth > 16) return false;
                if (!parse_list(&sub, ps.list, is_is || is_where, is_has)) return false;
                if (!is_where) {
                    uint32_t mx = 0;
                    for (uint32_t k2 = 0; k2 < ps.list->n; k2++) if (ps.list->v[k2].spec > mx) mx = ps.list->v[k2].spec;
                    *spec += mx;
                }
                vec_push(p->a, b->pseudos, ps);
                continue;
            }
            if ((n == 4 && strncasecmp(s, "lang", 4) == 0) || (n == 3 && strncasecmp(s, "dir", 3) == 0)) {
                ps.type = n == 4 ? PC_LANG : PC_DIR;
                uint32_t j = in;
                while (j < close && p->t->v[j].type == CT_WS) j++;
                if (j >= close || (p->t->v[j].type != CT_IDENT && p->t->v[j].type != CT_STRING)) return false;
                ps.arg = wa_strndup(&p->d->arena, sv(p, &p->t->v[j]), p->t->v[j].vn);
                vec_push(p->a, b->pseudos, ps);
                *spec += 1 << 10;
                continue;
            }
            if (n > 1 && s[0] == '-') {                       /* -webkit-... functional pseudo-classes: never match */
                ps.type = PC_NEVER;
                vec_push(p->a, b->pseudos, ps);
                *spec += 1 << 10;
                continue;
            }
            if ((n == 5 && strncasecmp(s, "state", 5) == 0) || (n == 4 && strncasecmp(s, "host", 4) == 0) ||
                (n == 12 && strncasecmp(s, "host-context", 12) == 0)) {
                ps.type = PC_NEVER;
                vec_push(p->a, b->pseudos, ps);
                continue;
            }
            return false;
        }
        break;
    }
    return any;
}

static bool finish_compound(SelP *p, CompBuild *b, SelCompound *out) {
    *out = b->c;
    if (b->classes.n > 255 || b->attrs.n > 255 || b->pseudos.n > 255) return false;
    out->classes = b->classes.v;
    out->nclasses = (uint8_t)b->classes.n;
    out->attrs = b->attrs.v;
    out->nattrs = (uint8_t)b->attrs.n;
    out->pseudos = b->pseudos.v;
    out->npseudos = (uint8_t)b->pseudos.n;
    return true;
}

/* One complex selector [ts, te). relative: it may begin with a combinator (:has()), stored as the leftmost compound's
 * comb with a flag in pseudo_el's upper bit. */
static bool parse_complex(SelP *p, Selector *out, bool relative) {
    SelCompound comps[64];
    int n = 0;
    uint32_t spec = 0;
    uint8_t pseudo_el = 0;
    int lead = -1;
    sp_ws(p);
    if (relative) {
        const CssTok *k = sp_peek(p);
        if (k && k->type == CT_DELIM && (k->delim == '>' || k->delim == '+' || k->delim == '~')) {
            lead = k->delim == '>' ? CB_CHILD : k->delim == '+' ? CB_ADJACENT : CB_SIBLING;
            p->i++;
            sp_ws(p);
        } else lead = CB_DESCENDANT;
    }
    for (;;) {
        if (n == 64) return false;
        CompBuild b;
        memset(&b, 0, sizeof b);
        if (pseudo_el) return false;                         /* nothing may follow a pseudo-element's compound */
        if (!parse_compound(p, &b, &pseudo_el, &spec)) return false;
        if (!finish_compound(p, &b, &comps[n])) return false;
        n++;
        bool ws = sp_ws(p);
        const CssTok *k = sp_peek(p);
        if (!k || k->type == CT_COMMA) break;
        int comb = CB_DESCENDANT;
        if (k->type == CT_DELIM && (k->delim == '>' || k->delim == '+' || k->delim == '~')) {
            comb = k->delim == '>' ? CB_CHILD : k->delim == '+' ? CB_ADJACENT : CB_SIBLING;
            p->i++;
            sp_ws(p);
        } else if (!ws) return false;
        comps[n - 1].comb = (uint8_t)comb;                   /* the combinator after compound n-1 (to its right) */
    }
    /* store right to left: c[0] is the subject; c[i].comb is the combinator between c[i] and c[i+1] */
    SelCompound *v = wa_alloc(&p->d->arena, sizeof(SelCompound) * (size_t)n);
    if (!v) return false;
    for (int i = 0; i < n; i++) {
        v[i] = comps[n - 1 - i];
        v[i].comb = i + 1 < n ? comps[n - 2 - i].comb : 0;
    }
    if (relative) v[n - 1].comb = (uint8_t)lead;             /* the leading combinator, relative to the :has() anchor */
    out->c = v;
    out->n = (uint16_t)n;
    out->pseudo_el = pseudo_el;
    out->spec = spec;
    return true;
}

static bool parse_list(SelP *p, SelList *out, bool forgiving, bool relative) {
    VEC(Selector) v = { NULL, 0, 0 };
    for (;;) {
        sp_ws(p);
        uint32_t start = p->i;
        /* the extent of this complex selector: up to the next top-level comma */
        uint32_t j = start;
        while (j < p->end && p->t->v[j].type != CT_COMMA) {
            const CssTok *k = &p->t->v[j];
            if (k->type == CT_FUNCTION || k->type == CT_LPAREN || k->type == CT_LBRACK || k->type == CT_LBRACE) j = k->close + 1;
            else j++;
        }
        SelP sub = *p;
        sub.i = start;
        sub.end = j;
        Selector s;
        bool ok = j > start && parse_complex(&sub, &s, relative) && (sp_ws(&sub), sub.i == sub.end);
        if (ok) vec_push(p->a, v, s);
        else if (!forgiving) return false;
        p->i = j;
        if (p->i >= p->end) break;
        p->i++;                                              /* the comma */
        if (p->i >= p->end && !forgiving) return false;
    }
    if (!v.n && !forgiving) return false;
    out->v = v.v;
    out->n = v.n;
    return true;
}

bool sel_parse(WebDoc *d, WebArena *a, const CssTokens *t, uint32_t start, uint32_t end, SelList *out) {
    SelP p = { d, &d->arena, t, start, end, false, 0 };
    (void)a;
    memset(out, 0, sizeof *out);
    while (p.end > p.i && t->v[p.end - 1].type == CT_WS) p.end--;
    if (p.i >= p.end) return false;
    return parse_list(&p, out, false, false);
}

bool sel_parse_text(WebDoc *d, WebArena *a, const char *s, size_t n, SelList *out) {
    CssTokens t;
    WebArena tmp;
    wa_init(&tmp, &d->budget);
    if (!css_tokenize(&tmp, s, n, &t)) { wa_destroy(&tmp); return false; }
    bool ok = sel_parse(d, a, &t, 0, t.n, out);
    wa_destroy(&tmp);
    return ok;
}

/* ================================================================================================ matching */

static WebNode *parent_el(const WebNode *n) {
    WebNode *p = n->parent;
    return p && p->type == WEB_NODE_ELEMENT ? p : NULL;
}

static WebNode *prev_el(const WebNode *n) {
    for (WebNode *s = n->prev; s; s = s->prev)
        if (s->type == WEB_NODE_ELEMENT) return s;
    return NULL;
}

static WebNode *next_el(const WebNode *n) {
    for (WebNode *s = n->next; s; s = s->next)
        if (s->type == WEB_NODE_ELEMENT) return s;
    return NULL;
}

/* HTML attributes whose values selectors compare ASCII case-insensitively (HTML standard, "case-sensitivity of
 * selectors") */
static bool ci_attr(WebAtom a, const AtomTable *t) {
    static const char *const list[] = { "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset",
        "checked", "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction", "disabled", "enctype",
        "face", "frame", "hreflang", "http-equiv", "lang", "language", "link", "media", "method", "multiple", "nohref",
        "noresize", "noshade", "nowrap", "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected", "shape",
        "target", "text", "type", "valign", "valuetype", "vlink" };
    const char *s = atom_str(t, a);
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

static bool eq_n(const char *a, size_t an, const char *b, size_t bn, bool ci) {
    if (an != bn) return false;
    return ci ? strncasecmp(a, b, an) == 0 : memcmp(a, b, an) == 0;
}

static bool attr_match(const SelAttr *sa, const WebNode *el) {
    const WebAttr *a = NULL;
    for (int i = 0; i < el->u.el.nattrs; i++) {
        const WebAttr *x = &el->u.el.attrs[i];
        if (x->ns != ATTR_NS_NONE) {
            /* [xlink:href] style names do not exist in CSS; xml:lang etc. match by local name */
            if (x->name == sa->name) { a = x; break; }
            continue;
        }
        if (x->name == sa->name) { a = x; break; }
        if (el->ns != WEB_NS_HTML && strcasecmp(atom_str(&el->doc->atoms, x->name), atom_str(&el->doc->atoms, sa->name)) == 0) { a = x; break; }
    }
    if (!a) return false;
    if (sa->op == SA_EXISTS) return true;
    bool ci = sa->ci || (el->ns == WEB_NS_HTML && ci_attr(sa->name, &el->doc->atoms));
    const char *v = a->value;
    size_t vn = a->len, sn = sa->vlen;
    const char *s = sa->val;
    switch (sa->op) {
    case SA_EQ: return eq_n(v, vn, s, sn, ci);
    case SA_INCLUDES:
        if (!sn) return false;
        for (size_t i = 0; i < vn;) {
            while (i < vn && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r')) i++;
            size_t st = i;
            while (i < vn && !(v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r')) i++;
            if (i > st && eq_n(v + st, i - st, s, sn, ci)) return true;
        }
        return false;
    case SA_DASH: return eq_n(v, vn, s, sn, ci) || (vn > sn && v[sn] == '-' && eq_n(v, sn, s, sn, ci));
    case SA_PREFIX: return sn && vn >= sn && eq_n(v, sn, s, sn, ci);
    case SA_SUFFIX: return sn && vn >= sn && eq_n(v + vn - sn, sn, s, sn, ci);
    case SA_SUBSTR:
        if (!sn || vn < sn) return false;
        for (size_t i = 0; i + sn <= vn; i++) if (eq_n(v + i, sn, s, sn, ci)) return true;
        return false;
    default: return false;
    }
}

static bool same_type(const WebNode *a, const WebNode *b) { return a->name == b->name && a->ns == b->ns; }

static bool nth_ok(int a, int b, int pos) {     /* pos is 1-based: an+b == pos for some n >= 0 */
    if (a == 0) return pos == b;
    int d = pos - b;
    if (d % a) return false;
    return d / a >= 0;
}

static bool is_link(const WebNode *el) {
    return el->ns == WEB_NS_HTML && (el->name == AT_a || el->name == AT_area || el->name == AT_link) && dom_attr(el, AT_href);
}

bool control_checked(const WebNode *el);       /* forms.c */
bool control_value_empty(const WebNode *el);   /* forms.c */

static bool is_form_control(const WebNode *el) {
    if (el->ns != WEB_NS_HTML) return false;
    switch (el->name) {
    case AT_button: case AT_input: case AT_select: case AT_textarea: case AT_optgroup: case AT_option: case AT_fieldset:
        return true;
    default:
        return false;
    }
}

static bool disabled(const WebNode *el) {
    if (!is_form_control(el)) return false;
    if (dom_attr(el, AT_disabled)) return true;
    if (el->name == AT_option)
        for (const WebNode *p = parent_el(el); p; p = parent_el(p)) if (is_html(p, AT_optgroup) && dom_attr(p, AT_disabled)) return true;
    for (const WebNode *p = parent_el(el); p; p = parent_el(p))
        if (is_html(p, AT_fieldset) && dom_attr(p, AT_disabled)) {
            /* the first legend's contents are not disabled */
            const WebNode *legend = NULL;
            for (const WebNode *k = p->first; k; k = k->next) if (is_html(k, AT_legend)) { legend = k; break; }
            if (legend && web_node_contains(legend, el)) continue;
            return true;
        }
    return false;
}

static bool lang_match(const WebNode *el, const char *want) {
    for (const WebNode *n = el; n; n = parent_el(n)) {
        const char *l = NULL;
        for (int i = 0; i < n->u.el.nattrs; i++) {
            const WebAttr *a = &n->u.el.attrs[i];
            if (a->name == AT_lang && (a->ns == ATTR_NS_NONE || a->ns == ATTR_NS_XML)) { l = a->value; if (a->ns == ATTR_NS_XML) break; }
        }
        if (!l) continue;
        size_t wn = strlen(want), ln = strlen(l);
        if (!wn) return false;
        return (ln == wn && strncasecmp(l, want, wn) == 0) || (ln > wn && l[wn] == '-' && strncasecmp(l, want, wn) == 0);
    }
    return false;
}

static bool match_from(const Selector *s, int idx, WebNode *el, MatchCtx *ctx, int depth);

/* :has(): some element matches the relative selector with its leftmost compound related to the anchor by the leading
 * combinator (the check is in match_from). Candidates: the anchor's descendants for "> x" and "x", and for "+ x" and
 * "~ x" the following siblings and everything inside them. */
static bool rel_match(const SelList *l, WebNode *anchor, MatchCtx *ctx, int depth) {
    for (uint32_t i = 0; i < l->n; i++) {
        const Selector *s = &l->v[i];
        int lead = s->c[s->n - 1].comb;
        MatchCtx sub = *ctx;
        sub.pseudo = PSE_NONE;
        sub.anchor = anchor;
        sub.anchor_comb = (uint8_t)lead;
        if (lead == CB_DESCENDANT || lead == CB_CHILD) {
            for (WebNode *n = anchor->first; n; n = dom_next(n, anchor))
                if (n->type == WEB_NODE_ELEMENT && match_from(s, 0, n, &sub, depth + 1)) return true;
        } else {
            for (WebNode *sib = anchor->next; sib; sib = sib->next) {
                if (sib->type != WEB_NODE_ELEMENT) continue;
                if (match_from(s, 0, sib, &sub, depth + 1)) return true;
                for (WebNode *n = sib->first; n; n = dom_next(n, sib))
                    if (n->type == WEB_NODE_ELEMENT && match_from(s, 0, n, &sub, depth + 1)) return true;
            }
        }
    }
    return false;
}

/* the leftmost compound of a relative selector must stand in the leading combinator's relation to the anchor */
static bool anchor_ok(const MatchCtx *ctx, WebNode *el) {
    WebNode *a = ctx->anchor;
    if (!a) return true;
    switch (ctx->anchor_comb) {
    case CB_CHILD: return el->parent == a;
    case CB_DESCENDANT: return el != a && web_node_contains(a, el);
    case CB_ADJACENT: return prev_el(el) == a;
    case CB_SIBLING:
        for (WebNode *p = prev_el(el); p; p = prev_el(p)) if (p == a) return true;
        return false;
    default: return false;
    }
}

static bool pseudo_match(const SelPseudo *ps, WebNode *el, MatchCtx *ctx, int depth) {
    WebDoc *d = el->doc;
    switch (ps->type) {
    case PC_ROOT: return !parent_el(el) && el->parent && el->parent->type == WEB_NODE_DOCUMENT;
    case PC_FIRST_CHILD: return !prev_el(el) && parent_el(el);
    case PC_LAST_CHILD: return !next_el(el) && parent_el(el);
    case PC_ONLY_CHILD: return !prev_el(el) && !next_el(el) && parent_el(el);
    case PC_NTH_CHILD: case PC_NTH_LAST_CHILD: {
        if (!el->parent) return false;
        if (ps->list) {
            MatchCtx sub = *ctx;
            sub.pseudo = PSE_NONE;
            if (!sel_list_matches(ps->list, el, &sub)) return false;
        }
        int pos = 1;
        bool last = ps->type == PC_NTH_LAST_CHILD;
        for (WebNode *s = last ? next_el(el) : prev_el(el); s; s = last ? next_el(s) : prev_el(s)) {
            if (ps->list) {
                MatchCtx sub = *ctx;
                sub.pseudo = PSE_NONE;
                if (!sel_list_matches(ps->list, s, &sub)) continue;
            }
            pos++;
        }
        return nth_ok(ps->a, ps->b, pos);
    }
    case PC_FIRST_OF_TYPE: case PC_LAST_OF_TYPE: case PC_ONLY_OF_TYPE: case PC_NTH_OF_TYPE: case PC_NTH_LAST_OF_TYPE: {
        if (!el->parent) return false;
        int before = 1, after = 1;
        for (WebNode *s = prev_el(el); s; s = prev_el(s)) if (same_type(s, el)) before++;
        for (WebNode *s = next_el(el); s; s = next_el(s)) if (same_type(s, el)) after++;
        switch (ps->type) {
        case PC_FIRST_OF_TYPE: return before == 1;
        case PC_LAST_OF_TYPE: return after == 1;
        case PC_ONLY_OF_TYPE: return before == 1 && after == 1;
        case PC_NTH_OF_TYPE: return nth_ok(ps->a, ps->b, before);
        default: return nth_ok(ps->a, ps->b, after);
        }
    }
    case PC_EMPTY:
        for (WebNode *k = el->first; k; k = k->next) {
            if (k->type == WEB_NODE_ELEMENT) return false;
            if ((k->type == WEB_NODE_TEXT || k->type == WEB_NODE_CDATA) && k->u.cd.len) return false;
        }
        return true;
    case PC_LINK: case PC_ANY_LINK: return is_link(el);
    case PC_VISITED: return false;
    case PC_HOVER: ctx->uses_hover = true; return (el->flags & NF_HOVER) != 0;
    case PC_ACTIVE: ctx->uses_hover = true; return (el->flags & NF_ACTIVE) != 0;
    case PC_FOCUS: case PC_FOCUS_VISIBLE: return d->focus == el;
    case PC_FOCUS_WITHIN: return d->focus && web_node_contains(el, d->focus);
    case PC_TARGET: return d->target == el;
    case PC_CHECKED: return control_checked(el);
    case PC_DISABLED: return disabled(el);
    case PC_ENABLED: return is_form_control(el) && el->name != AT_fieldset && !disabled(el);
    case PC_REQUIRED: case PC_OPTIONAL: {
        bool can = is_html(el, AT_input) || is_html(el, AT_select) || is_html(el, AT_textarea);
        if (!can) return false;
        bool req = dom_attr(el, AT_required) != NULL;
        return ps->type == PC_REQUIRED ? req : !req;
    }
    case PC_READ_WRITE: case PC_READ_ONLY: {
        bool rw = false;
        if (is_html(el, AT_textarea)) rw = !dom_attr(el, AT_readonly) && !disabled(el);
        else if (is_html(el, AT_input)) {
            const char *t = dom_attr_val(el, AT_type);
            bool texty = !t || !*t || !strcasecmp(t, "text") || !strcasecmp(t, "search") || !strcasecmp(t, "url") ||
                         !strcasecmp(t, "tel") || !strcasecmp(t, "email") || !strcasecmp(t, "password") ||
                         !strcasecmp(t, "number") || !strcasecmp(t, "date") || !strcasecmp(t, "time");
            rw = texty && !dom_attr(el, AT_readonly) && !disabled(el);
        } else {
            for (const WebNode *n = el; n; n = parent_el(n)) {
                const char *ce = dom_attr_val(n, AT_contenteditable);
                if (ce) { rw = strcasecmp(ce, "false") != 0; break; }
            }
        }
        return ps->type == PC_READ_WRITE ? rw : !rw;
    }
    case PC_PLACEHOLDER_SHOWN:
        return (is_html(el, AT_input) || is_html(el, AT_textarea)) && dom_attr(el, AT_placeholder) && control_value_empty(el);
    case PC_DEFAULT:                                         /* checked by default: the attributes, not the state */
        if (is_html(el, AT_option)) return dom_attr(el, AT_selected) != NULL;
        return is_html(el, AT_input) && dom_attr(el, AT_checked) != NULL;
    case PC_INDETERMINATE: return is_html(el, AT_progress) && !dom_attr(el, AT_value);
    case PC_LANG: return ps->arg && lang_match(el, ps->arg);
    case PC_DIR: {
        for (const WebNode *n = el; n; n = parent_el(n)) {
            const char *dir = dom_attr_val(n, AT_dir);
            if (dir && (!strcasecmp(dir, "rtl") || !strcasecmp(dir, "ltr"))) return ps->arg && !strcasecmp(dir, ps->arg);
        }
        return ps->arg && !strcasecmp(ps->arg, "ltr");
    }
    case PC_SCOPE: return ctx->scope ? el == ctx->scope : (!parent_el(el));
    case PC_DEFINED: return true;
    case PC_OPEN: return (is_html(el, AT_details) || is_html(el, AT_dialog)) && dom_attr(el, AT_open);
    case PC_NOT: {
        MatchCtx sub = *ctx;
        sub.pseudo = PSE_NONE;
        sub.anchor = NULL;
        return !sel_list_matches(ps->list, el, &sub);
    }
    case PC_IS: case PC_WHERE: {
        MatchCtx sub = *ctx;
        sub.pseudo = PSE_NONE;
        sub.anchor = NULL;
        return sel_list_matches(ps->list, el, &sub);
    }
    case PC_HAS: return depth < 8 && rel_match(ps->list, el, ctx, depth);
    default: return false;
    }
}

static bool atom_ieq(const WebDoc *d, WebAtom a, WebAtom b) {
    return a == b || strcasecmp(atom_str(&d->atoms, a), atom_str(&d->atoms, b)) == 0;
}

static bool match_compound(const SelCompound *c, WebNode *el, MatchCtx *ctx, int depth) {
    WebDoc *d = el->doc;
    if (c->tag) {
        if (el->ns == WEB_NS_HTML) { if (el->name != c->tag) return false; }
        else if (el->name != c->tag_cs && el->name != c->tag) return false;
    }
    bool quirks = d->quirks == 2;
    if (c->id) {
        if (!el->u.el.id) return false;
        if (el->u.el.id != c->id && !(quirks && atom_ieq(d, el->u.el.id, c->id))) return false;
    }
    for (int i = 0; i < c->nclasses; i++) {
        bool found = false;
        for (int k = 0; k < el->u.el.nclasses; k++)
            if (el->u.el.classes[k] == c->classes[i] || (quirks && atom_ieq(d, el->u.el.classes[k], c->classes[i]))) { found = true; break; }
        if (!found) return false;
    }
    for (int i = 0; i < c->nattrs; i++)
        if (!attr_match(&c->attrs[i], el)) return false;
    for (int i = 0; i < c->npseudos; i++)
        if (!pseudo_match(&c->pseudos[i], el, ctx, depth)) return false;
    return true;
}

static bool match_from(const Selector *s, int idx, WebNode *el, MatchCtx *ctx, int depth) {
    if (!match_compound(&s->c[idx], el, ctx, depth)) return false;
    if (idx == s->n - 1) return anchor_ok(ctx, el);
    switch (s->c[idx].comb) {
    case CB_CHILD: {
        WebNode *p = parent_el(el);
        return p && match_from(s, idx + 1, p, ctx, depth);
    }
    case CB_DESCENDANT:
        for (WebNode *p = parent_el(el); p; p = parent_el(p))
            if (match_from(s, idx + 1, p, ctx, depth)) return true;
        return false;
    case CB_ADJACENT: {
        WebNode *p = prev_el(el);
        return p && match_from(s, idx + 1, p, ctx, depth);
    }
    case CB_SIBLING:
        for (WebNode *p = prev_el(el); p; p = prev_el(p))
            if (match_from(s, idx + 1, p, ctx, depth)) return true;
        return false;
    default:
        return false;
    }
}

bool sel_matches(const Selector *s, WebNode *el, MatchCtx *ctx) {
    if (s->pseudo_el != ctx->pseudo) return false;
    if (s->pseudo_el == PSE_OTHER) return false;
    return match_from(s, 0, el, ctx, 0);
}

bool sel_list_matches(const SelList *l, WebNode *el, MatchCtx *ctx) {
    for (uint32_t i = 0; i < l->n; i++)
        if (sel_matches(&l->v[i], el, ctx)) return true;
    return false;
}

/* ================================================================================================ the API */

static bool compile(WebDoc *d, const char *sel, SelList *out, int *error) {
    if (error) *error = WEB_OK;
    if (!sel || !sel_parse_text(d, &d->arena, sel, strlen(sel), out)) {
        if (error) *error = WEB_E_ARG;
        return false;
    }
    return true;
}

bool web_element_matches(WebNode *el, const char *sel, int *error) {
    SelList l;
    if (!is_el(el) || !compile(el->doc, sel, &l, error)) return false;
    MatchCtx ctx = { el, PSE_NONE, false };
    return sel_list_matches(&l, el, &ctx);
}

WebNode *web_element_closest(WebNode *el, const char *sel, int *error) {
    SelList l;
    if (!is_el(el) || !compile(el->doc, sel, &l, error)) return NULL;
    MatchCtx ctx = { el, PSE_NONE, false };
    for (WebNode *n = el; n && n->type == WEB_NODE_ELEMENT; n = n->parent)
        if (sel_list_matches(&l, n, &ctx)) return n;
    return NULL;
}

WebNode *web_query_selector(WebNode *root, const char *sel, int *error) {
    SelList l;
    if (!compile(root->doc, sel, &l, error)) return NULL;
    MatchCtx ctx = { root->type == WEB_NODE_ELEMENT ? root : NULL, PSE_NONE, false };
    for (WebNode *n = root->first; n; n = dom_next(n, root))
        if (n->type == WEB_NODE_ELEMENT && sel_list_matches(&l, n, &ctx)) return n;
    return NULL;
}

WebNode **web_query_selector_all(WebNode *root, const char *sel, int *count, int *error) {
    SelList l;
    *count = 0;
    WebDoc *d = root->doc;
    if (!compile(d, sel, &l, error)) return NULL;
    MatchCtx ctx = { root->type == WEB_NODE_ELEMENT ? root : NULL, PSE_NONE, false };
    int n = 0, cap = 0;
    WebNode **v = NULL;
    for (WebNode *k = root->first; k; k = dom_next(k, root)) {
        if (k->type != WEB_NODE_ELEMENT || !sel_list_matches(&l, k, &ctx)) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            WebNode **nv = web_raw_alloc(&d->budget, sizeof(WebNode *) * (size_t)nc);
            if (!nv) break;
            if (v) { memcpy(nv, v, sizeof(WebNode *) * (size_t)n); web_raw_free(&d->budget, v); }
            v = nv;
            cap = nc;
        }
        v[n++] = k;
    }
    *count = n;
    return v;
}
