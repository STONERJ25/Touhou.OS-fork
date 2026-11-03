/*
 * Stylesheets: "consume a list of rules" (CSS Syntax 3, section 5) over the token array, with the at-rules a document
 * needs: @media (Media Queries 4: types, and/or/not, the range syntax, the common features), @import (fetched through the
 * WebLoader, depth-limited), @supports (decided at parse time by parsing the declarations), @layer blocks (their rules are
 * used, without the layer ordering), and the rest skipped (@font-face, @keyframes, @page, @namespace, @container...).
 * Qualified rules are a selector list (selector.c) and a declaration list (cssval.c).
 */
#include "css.h"

uint32_t res_request(WebDoc *d, WebResourceKind kind, const char *url, WebNode *owner, int mode, void *user);   /* resource.c */
char *url_resolve(WebArena *a, const char *base, const char *rel, size_t n);                                   /* url.c */

/* ================================================================================================ media queries */

enum { ME_TRUE = 0, ME_FALSE, ME_AND, ME_OR, ME_NOT, ME_TYPE, ME_FEAT };
enum { MF_WIDTH = 1, MF_HEIGHT, MF_ASPECT, MF_ORIENTATION, MF_COLOR_SCHEME, MF_REDUCED_MOTION, MF_HOVER, MF_POINTER,
       MF_RESOLUTION, MF_COLOR, MF_MONOCHROME, MF_GRID, MF_SCRIPTING, MF_FORCED_COLORS, MF_CONTRAST, MF_DISPLAY_MODE,
       MF_UPDATE, MF_INVERTED, MF_REDUCED_TRANSPARENCY, MF_COLOR_INDEX, MF_DYNAMIC_RANGE };
enum { CMP_BOOL = 0, CMP_EQ, CMP_LT, CMP_LE, CMP_GT, CMP_GE };
enum { MT_ALL = 0, MT_SCREEN, MT_PRINT, MT_OTHER };

struct MExpr {
    uint8_t op, feat, cmp, unit;
    char kw[24];                    /* a keyword value (orientation: landscape, prefers-color-scheme: dark...) */
    float v;
    MExpr *a, *b;
};

typedef struct { const CssTokens *t; uint32_t i, end; WebDoc *d; } MC;

static const CssTok *mpeek(MC *c) {
    while (c->i < c->end && c->t->v[c->i].type == CT_WS) c->i++;
    return c->i < c->end ? &c->t->v[c->i] : NULL;
}

static const CssTok *mtake(MC *c) {
    const CssTok *k = mpeek(c);
    if (!k) return NULL;
    if (k->type == CT_FUNCTION || k->type == CT_LPAREN || k->type == CT_LBRACK || k->type == CT_LBRACE)
        c->i = k->close < c->end ? k->close + 1 : c->end;
    else c->i++;
    return k;
}

static bool mident(const MC *c, const CssTok *k, const char *s) { return k && tok_is_ident(c->t, k, s); }

static MExpr *mnew(WebDoc *d, int op) {
    MExpr *e = wa_alloc(&d->arena, sizeof *e);
    if (e) e->op = (uint8_t)op;
    return e;
}

static const struct { const char *name; uint8_t feat; } mfeats[] = {
    {"width", MF_WIDTH}, {"height", MF_HEIGHT}, {"device-width", MF_WIDTH}, {"device-height", MF_HEIGHT},
    {"aspect-ratio", MF_ASPECT}, {"device-aspect-ratio", MF_ASPECT}, {"orientation", MF_ORIENTATION},
    {"prefers-color-scheme", MF_COLOR_SCHEME}, {"prefers-reduced-motion", MF_REDUCED_MOTION}, {"hover", MF_HOVER},
    {"any-hover", MF_HOVER}, {"pointer", MF_POINTER}, {"any-pointer", MF_POINTER}, {"resolution", MF_RESOLUTION},
    {"-webkit-device-pixel-ratio", MF_RESOLUTION}, {"device-pixel-ratio", MF_RESOLUTION}, {"color", MF_COLOR},
    {"monochrome", MF_MONOCHROME}, {"grid", MF_GRID}, {"scripting", MF_SCRIPTING}, {"forced-colors", MF_FORCED_COLORS},
    {"prefers-contrast", MF_CONTRAST}, {"display-mode", MF_DISPLAY_MODE}, {"update", MF_UPDATE},
    {"inverted-colors", MF_INVERTED}, {"prefers-reduced-transparency", MF_REDUCED_TRANSPARENCY},
    {"color-index", MF_COLOR_INDEX}, {"dynamic-range", MF_DYNAMIC_RANGE}, {"video-dynamic-range", MF_DYNAMIC_RANGE},
    {"color-gamut", MF_DYNAMIC_RANGE},
};

static int feature_of(const char *s, size_t n, int *cmp) {
    *cmp = CMP_EQ;
    if (n > 4 && strncasecmp(s, "min-", 4) == 0) { *cmp = CMP_GE; s += 4; n -= 4; }
    else if (n > 4 && strncasecmp(s, "max-", 4) == 0) { *cmp = CMP_LE; s += 4; n -= 4; }
    else if (n > 12 && strncasecmp(s, "-webkit-min-", 12) == 0) { *cmp = CMP_GE; s += 12; n -= 12; if (n == 17 && strncasecmp(s, "device-pixel-ratio", 17) == 0) return MF_RESOLUTION; }
    else if (n > 12 && strncasecmp(s, "-webkit-max-", 12) == 0) { *cmp = CMP_LE; s += 12; n -= 12; }
    for (size_t i = 0; i < sizeof mfeats / sizeof mfeats[0]; i++)
        if (strlen(mfeats[i].name) == n && strncasecmp(mfeats[i].name, s, n) == 0) return mfeats[i].feat;
    return 0;
}

/* a feature value: a length (px), a number, a ratio, a resolution (dppx) or a keyword */
static bool mvalue(MC *c, MExpr *e) {
    const CssTok *k = mtake(c);
    if (!k) return false;
    if (k->type == CT_IDENT) {
        size_t n = k->vn < sizeof e->kw - 1 ? k->vn : sizeof e->kw - 1;
        for (size_t i = 0; i < n; i++) {
            char ch = tok_val(c->t, k)[i];
            e->kw[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        }
        e->kw[n] = 0;
        return true;
    }
    if (k->type == CT_NUMBER) {
        e->v = (float)k->num;
        const CssTok *s = mpeek(c);
        if (s && s->type == CT_DELIM && s->delim == '/') {      /* a ratio */
            mtake(c);
            const CssTok *b = mtake(c);
            if (!b || b->type != CT_NUMBER || b->num == 0) return false;
            e->v = (float)(k->num / b->num);
        }
        return true;
    }
    if (k->type == CT_DIMENSION) {
        const char *u = tok_val(c->t, k);
        double v = k->num;
        if (k->vn == 2 && strncasecmp(u, "px", 2) == 0) {}
        else if ((k->vn == 2 && strncasecmp(u, "em", 2) == 0) || (k->vn == 3 && strncasecmp(u, "rem", 3) == 0)) v *= 16;
        else if (k->vn == 2 && strncasecmp(u, "ex", 2) == 0) v *= 8;
        else if (k->vn == 2 && strncasecmp(u, "ch", 2) == 0) v *= 8;
        else if (k->vn == 2 && strncasecmp(u, "pt", 2) == 0) v *= 96.0 / 72;
        else if (k->vn == 2 && strncasecmp(u, "in", 2) == 0) v *= 96;
        else if (k->vn == 2 && strncasecmp(u, "cm", 2) == 0) v *= 96 / 2.54;
        else if (k->vn == 2 && strncasecmp(u, "mm", 2) == 0) v *= 96 / 25.4;
        else if (k->vn == 2 && strncasecmp(u, "pc", 2) == 0) v *= 16;
        else if ((k->vn == 4 && strncasecmp(u, "dppx", 4) == 0) || (k->vn == 1 && (u[0] | 0x20) == 'x')) {}
        else if (k->vn == 3 && strncasecmp(u, "dpi", 3) == 0) v /= 96;
        else if (k->vn == 4 && strncasecmp(u, "dpcm", 4) == 0) v /= 96 / 2.54;
        else return false;
        e->v = (float)v;
        return true;
    }
    return false;
}

static int mcmp_op(MC *c) {
    const CssTok *k = mpeek(c);
    if (!k || k->type != CT_DELIM) return -1;
    int ch = k->delim;
    if (ch != '<' && ch != '>' && ch != '=') return -1;
    mtake(c);
    if (ch == '=') return CMP_EQ;
    const CssTok *e = c->i < c->end ? &c->t->v[c->i] : NULL;
    bool eq = e && e->type == CT_DELIM && e->delim == '=';
    if (eq) c->i++;
    if (ch == '<') return eq ? CMP_LE : CMP_LT;
    return eq ? CMP_GE : CMP_GT;
}

static int flip(int cmp) {
    switch (cmp) {
    case CMP_LT: return CMP_GT;
    case CMP_LE: return CMP_GE;
    case CMP_GT: return CMP_LT;
    case CMP_GE: return CMP_LE;
    default: return cmp;
    }
}

static MExpr *mcond(MC *c, bool allow_or);

/* the inside of ( ): a nested condition or a feature */
static MExpr *mfeature(MC *c) {
    const CssTok *k = mpeek(c);
    if (!k) return NULL;
    if (k->type == CT_LPAREN || mident(c, k, "not")) return mcond(c, true);
    MExpr *e = mnew(c->d, ME_FEAT);
    if (!e) return NULL;
    if (k->type == CT_IDENT) {
        mtake(c);
        int cmp;
        int f = feature_of(tok_val(c->t, k), k->vn, &cmp);
        e->feat = (uint8_t)f;
        const CssTok *s = mpeek(c);
        if (!s) { e->cmp = CMP_BOOL; return f ? e : mnew(c->d, ME_FALSE); }
        if (s->type == CT_COLON) {
            mtake(c);
            e->cmp = (uint8_t)cmp;
            if (!mvalue(c, e) || mpeek(c)) return mnew(c->d, ME_FALSE);
            return f ? e : mnew(c->d, ME_FALSE);
        }
        int op = mcmp_op(c);                                  /* width >= 600px */
        if (op < 0 || cmp != CMP_EQ) return mnew(c->d, ME_FALSE);
        e->cmp = (uint8_t)op;
        if (!mvalue(c, e) || mpeek(c)) return mnew(c->d, ME_FALSE);
        return f ? e : mnew(c->d, ME_FALSE);
    }
    /* 400px < width [<= 700px] */
    MExpr lo;
    memset(&lo, 0, sizeof lo);
    if (!mvalue(c, &lo)) return mnew(c->d, ME_FALSE);
    int op1 = mcmp_op(c);
    const CssTok *nm = mtake(c);
    if (op1 < 0 || !nm || nm->type != CT_IDENT) return mnew(c->d, ME_FALSE);
    int cmp;
    int f = feature_of(tok_val(c->t, nm), nm->vn, &cmp);
    if (!f) return mnew(c->d, ME_FALSE);
    e->feat = (uint8_t)f;
    e->cmp = (uint8_t)flip(op1);
    e->v = lo.v;
    if (!mpeek(c)) return e;
    int op2 = mcmp_op(c);
    MExpr *e2 = mnew(c->d, ME_FEAT);
    if (op2 < 0 || !e2) return mnew(c->d, ME_FALSE);
    e2->feat = (uint8_t)f;
    e2->cmp = (uint8_t)op2;
    if (!mvalue(c, e2) || mpeek(c)) return mnew(c->d, ME_FALSE);
    MExpr *and = mnew(c->d, ME_AND);
    if (!and) return NULL;
    and->a = e;
    and->b = e2;
    return and;
}

static MExpr *min_parens(MC *c) {
    const CssTok *k = mpeek(c);
    if (!k) return NULL;
    if (k->type == CT_LPAREN) {
        mtake(c);
        MC in = { c->t, (uint32_t)(k - c->t->v) + 1, k->close < c->end ? k->close : c->end, c->d };
        MExpr *e = mfeature(&in);
        if (!e || mpeek(&in)) return mnew(c->d, ME_FALSE);
        return e;
    }
    if (k->type == CT_FUNCTION) { mtake(c); return mnew(c->d, ME_FALSE); }  /* <general-enclosed> */
    return NULL;
}

static MExpr *mcond(MC *c, bool allow_or) {
    const CssTok *k = mpeek(c);
    if (mident(c, k, "not")) {
        mtake(c);
        MExpr *in = min_parens(c);
        MExpr *e = mnew(c->d, ME_NOT);
        if (!in || !e) return NULL;
        e->a = in;
        return e;
    }
    MExpr *l = min_parens(c);
    if (!l) return NULL;
    for (;;) {
        k = mpeek(c);
        int op = mident(c, k, "and") ? ME_AND : (allow_or && mident(c, k, "or")) ? ME_OR : -1;
        if (op < 0) return l;
        mtake(c);
        MExpr *r = min_parens(c);
        MExpr *e = mnew(c->d, op);
        if (!r || !e) return NULL;
        e->a = l;
        e->b = r;
        l = e;
    }
}

static MExpr *mquery(MC *c) {
    const CssTok *k = mpeek(c);
    if (!k) return NULL;
    bool neg = false;
    if (k->type == CT_IDENT) {
        if (mident(c, k, "not") || mident(c, k, "only")) {
            /* "not" before a type negates the whole query; before "(" it is a condition */
            MC look = *c;
            mtake(&look);
            const CssTok *nx = mpeek(&look);
            if (nx && nx->type == CT_IDENT) {
                neg = mident(c, k, "not");
                *c = look;
                k = nx;
            } else if (mident(c, k, "only")) return mnew(c->d, ME_FALSE);
        }
    }
    MExpr *e;
    if (k->type == CT_IDENT && !mident(c, k, "not")) {
        mtake(c);
        e = mnew(c->d, ME_TYPE);
        if (!e) return NULL;
        const char *s = tok_val(c->t, k);
        e->feat = (k->vn == 3 && strncasecmp(s, "all", 3) == 0) ? MT_ALL :
                  (k->vn == 6 && strncasecmp(s, "screen", 6) == 0) ? MT_SCREEN :
                  (k->vn == 5 && strncasecmp(s, "print", 5) == 0) ? MT_PRINT : MT_OTHER;
        if (mident(c, mpeek(c), "and")) {
            mtake(c);
            MExpr *cond = mcond(c, false);
            MExpr *and = mnew(c->d, ME_AND);
            if (!cond || !and) return NULL;
            and->a = e;
            and->b = cond;
            e = and;
        }
    } else {
        e = mcond(c, true);
    }
    if (!e || mpeek(c)) return mnew(c->d, ME_FALSE);
    if (neg) {
        MExpr *n = mnew(c->d, ME_NOT);
        if (!n) return NULL;
        n->a = e;
        e = n;
    }
    return e;
}

static MediaList *media_parse(WebDoc *d, const CssTokens *t, uint32_t s, uint32_t e) {
    MediaList *ml = wa_alloc(&d->arena, sizeof *ml);
    if (!ml) return NULL;
    MExpr *q[32];
    int n = 0;
    uint32_t i = s;
    while (i < e && n < 32) {
        uint32_t j = i;
        while (j < e && t->v[j].type != CT_COMMA) {
            const CssTok *k = &t->v[j];
            if (k->type == CT_FUNCTION || k->type == CT_LPAREN || k->type == CT_LBRACK || k->type == CT_LBRACE)
                j = k->close < e ? k->close + 1 : e;
            else j++;
        }
        MC c = { t, i, j, d };
        if (mpeek(&c)) {
            MExpr *x = mquery(&c);
            q[n++] = x ? x : mnew(d, ME_FALSE);
        } else if (j < e) q[n++] = mnew(d, ME_FALSE);        /* an empty query in a list never matches */
        i = j + 1;
    }
    if (!n) return ml;                                       /* empty list: matches */
    ml->q = wa_alloc(&d->arena, sizeof(MExpr *) * (size_t)n);
    if (!ml->q) return ml;
    memcpy(ml->q, q, sizeof(MExpr *) * (size_t)n);
    ml->n = n;
    return ml;
}

MediaList *media_parse_text(WebDoc *d, const char *s, size_t n) {
    CssTokens t;
    if (!css_tokenize(&d->arena, s, n, &t)) return NULL;
    MediaList *m = media_parse(d, &t, 0, t.n);
    css_tokens_free(&d->arena, &t);
    return m;
}

static bool cmpv(int cmp, float a, float b) {
    switch (cmp) {
    case CMP_EQ: return a > b - 0.01f && a < b + 0.01f;
    case CMP_LT: return a < b;
    case CMP_LE: return a <= b + 0.001f;
    case CMP_GT: return a > b;
    case CMP_GE: return a >= b - 0.001f;
    default: return a != 0;
    }
}

static bool meval(WebDoc *d, const MExpr *e) {
    if (!e) return false;
    float w = (float)d->vw, h = (float)d->vh;
    switch (e->op) {
    case ME_TRUE: return true;
    case ME_FALSE: return false;
    case ME_AND: return meval(d, e->a) && meval(d, e->b);
    case ME_OR: return meval(d, e->a) || meval(d, e->b);
    case ME_NOT: return !meval(d, e->a);
    case ME_TYPE:
        if (e->feat == MT_ALL) return true;
        if (e->feat == MT_PRINT) return d->cfg.print;
        if (e->feat == MT_SCREEN) return !d->cfg.print;
        return false;
    case ME_FEAT: break;
    default: return false;
    }
    bool b = e->cmp == CMP_BOOL;
    switch (e->feat) {
    case MF_WIDTH: return b ? w > 0 : cmpv(e->cmp, w, e->v);
    case MF_HEIGHT: return b ? h > 0 : cmpv(e->cmp, h, e->v);
    case MF_ASPECT: return b ? true : cmpv(e->cmp, h > 0 ? w / h : 0, e->v);
    case MF_ORIENTATION: return b ? true : strcmp(e->kw, h >= w ? "portrait" : "landscape") == 0;
    case MF_COLOR_SCHEME: return b ? true : strcmp(e->kw, d->cfg.prefers_dark ? "dark" : "light") == 0;
    case MF_REDUCED_MOTION: return b ? false : strcmp(e->kw, "no-preference") == 0;
    case MF_REDUCED_TRANSPARENCY: return b ? false : strcmp(e->kw, "no-preference") == 0;
    case MF_HOVER: return b ? true : strcmp(e->kw, "hover") == 0;
    case MF_POINTER: return b ? true : strcmp(e->kw, "fine") == 0;
    case MF_RESOLUTION: return b ? true : cmpv(e->cmp, (float)d->cfg.scale, e->v);
    case MF_COLOR: return b ? true : cmpv(e->cmp, 8, e->v);
    case MF_MONOCHROME: return b ? false : cmpv(e->cmp, 0, e->v);
    case MF_COLOR_INDEX: return b ? false : cmpv(e->cmp, 0, e->v);
    case MF_GRID: return b ? false : cmpv(e->cmp, 0, e->v);
    case MF_SCRIPTING: return b ? d->scripting : strcmp(e->kw, d->scripting ? "enabled" : "none") == 0;
    case MF_FORCED_COLORS: return b ? false : strcmp(e->kw, "none") == 0;
    case MF_INVERTED: return b ? false : strcmp(e->kw, "none") == 0;
    case MF_CONTRAST: return b ? false : strcmp(e->kw, "no-preference") == 0;
    case MF_DISPLAY_MODE: return b ? true : strcmp(e->kw, "browser") == 0;
    case MF_UPDATE: return b ? true : strcmp(e->kw, "fast") == 0;
    case MF_DYNAMIC_RANGE: return b ? true : strcmp(e->kw, "standard") == 0 || strcmp(e->kw, "srgb") == 0;
    default: return false;
    }
}

bool media_matches(WebDoc *d, const MediaList *m) {
    if (!m || !m->n) return true;
    for (int i = 0; i < m->n; i++)
        if (meval(d, m->q[i])) return true;
    return false;
}

bool media_cond_matches(WebDoc *d, const MediaCond *c) {
    for (; c; c = c->parent)
        if (!media_matches(d, c->m)) return false;
    return true;
}

/* ================================================================================================ @supports */

typedef struct SP {
    WebDoc *d;
    CssTokens t;
    CssSheet *sheet;
    int depth;
    bool rules_seen;
    const char *base;
} SP;

static bool sup_cond(SP *p, uint32_t s, uint32_t e);

static bool sup_in_parens(SP *p, uint32_t *i, uint32_t e) {
    const CssTokens *t = &p->t;
    while (*i < e && t->v[*i].type == CT_WS) (*i)++;
    if (*i >= e) return false;
    const CssTok *k = &t->v[*i];
    if (k->type == CT_FUNCTION) {
        uint32_t in = *i + 1, out = k->close;
        *i = k->close + 1;
        if (k->vn == 8 && strncasecmp(tok_val(t, k), "selector", 8) == 0) {
            SelList sl;
            WebArena tmp;
            wa_init(&tmp, &p->d->budget);
            bool ok = sel_parse(p->d, &tmp, t, in, out, &sl);
            wa_destroy(&tmp);
            return ok;
        }
        return false;                                         /* font-tech(), font-format(): not supported */
    }
    if (k->type != CT_LPAREN) return false;
    uint32_t in = *i + 1, out = k->close < e ? k->close : e;
    *i = out + 1;
    /* a nested condition, or a declaration "name: value" */
    uint32_t j = in;
    while (j < out && t->v[j].type == CT_WS) j++;
    if (j < out && (t->v[j].type == CT_LPAREN || tok_is_ident(t, &t->v[j], "not") || t->v[j].type == CT_FUNCTION))
        return sup_cond(p, in, out);
    if (j >= out || t->v[j].type != CT_IDENT) return false;
    uint32_t name = j++;
    while (j < out && t->v[j].type == CT_WS) j++;
    if (j >= out || t->v[j].type != CT_COLON) return false;
    j++;
    const char *nm = tok_val(t, &t->v[name]);
    uint32_t nn = t->v[name].vn;
    if (nn > 2 && nm[0] == '-' && nm[1] == '-') return true;      /* custom properties are always supported */
    /* parse "name: value" as a declaration list of one */
    CssDecls tmp = { NULL, 0, 0 };
    css_parse_decls(p->d, t, name, out, &tmp);
    bool ok = tmp.n > 0;
    if (tmp.v) wa_free(&p->d->arena, tmp.v, sizeof(CssDecl) * tmp.cap);
    return ok;
}

static bool sup_cond(SP *p, uint32_t s, uint32_t e) {
    const CssTokens *t = &p->t;
    uint32_t i = s;
    while (i < e && t->v[i].type == CT_WS) i++;
    if (i < e && tok_is_ident(t, &t->v[i], "not")) {
        i++;
        return !sup_in_parens(p, &i, e);
    }
    bool v = sup_in_parens(p, &i, e);
    for (;;) {
        while (i < e && t->v[i].type == CT_WS) i++;
        if (i >= e) return v;
        if (tok_is_ident(t, &t->v[i], "and")) { i++; bool r = sup_in_parens(p, &i, e); v = v && r; }
        else if (tok_is_ident(t, &t->v[i], "or")) { i++; bool r = sup_in_parens(p, &i, e); v = v || r; }
        else return false;
    }
}

/* ================================================================================================ rules */

static void rule_list(SP *p, uint32_t i, uint32_t end, const MediaCond *cond, bool top);

static uint32_t skip_component(const CssTokens *t, uint32_t i, uint32_t end) {
    const CssTok *k = &t->v[i];
    if (k->type == CT_FUNCTION || k->type == CT_LPAREN || k->type == CT_LBRACK || k->type == CT_LBRACE)
        return k->close < end ? k->close + 1 : end;
    return i + 1;
}

static void at_import(SP *p, uint32_t ps, uint32_t pe) {
    const CssTokens *t = &p->t;
    if (p->rules_seen) return;                               /* @import after other rules is invalid */
    uint32_t i = ps;
    while (i < pe && t->v[i].type == CT_WS) i++;
    if (i >= pe) return;
    const CssTok *k = &t->v[i];
    const char *u = NULL;
    size_t un = 0;
    if (k->type == CT_STRING || k->type == CT_URL) { u = tok_val(t, k); un = k->vn; i++; }
    else if (k->type == CT_FUNCTION && k->vn == 3 && strncasecmp(tok_val(t, k), "url", 3) == 0) {
        uint32_t j = i + 1;
        while (j < k->close && t->v[j].type == CT_WS) j++;
        if (j < k->close && t->v[j].type == CT_STRING) { u = tok_val(t, &t->v[j]); un = t->v[j].vn; }
        i = k->close + 1;
    }
    if (!u) return;
    /* skip layer(...) and supports(...), keep the media query list */
    while (i < pe) {
        while (i < pe && t->v[i].type == CT_WS) i++;
        if (i < pe && (tok_is_ident(t, &t->v[i], "layer") ||
                       (t->v[i].type == CT_FUNCTION && ((t->v[i].vn == 5 && strncasecmp(tok_val(t, &t->v[i]), "layer", 5) == 0) ||
                                                         (t->v[i].vn == 8 && strncasecmp(tok_val(t, &t->v[i]), "supports", 8) == 0))))) {
            if (t->v[i].type == CT_FUNCTION && t->v[i].vn == 8 && !sup_cond(p, i + 1, t->v[i].close)) return;
            i = skip_component(t, i, pe);
            continue;
        }
        break;
    }
    WebDoc *d = p->d;
    CssSheet *child = wa_alloc(&d->arena, sizeof *child);
    if (!child) return;
    child->origin = p->sheet->origin;
    child->url = url_resolve(&d->arena, p->base, u, un);
    child->media = media_parse(d, t, i, pe);
    child->owner = p->sheet->owner;
    if (!child->url || !vec_push(&d->arena, p->sheet->imports, child)) return;
    if (p->depth >= 8) { child->failed = true; return; }
    child->loading = true;
    child->version = (uint32_t)p->depth + 1;                /* the import depth, for the nested parse */
    child->req = res_request(d, WEB_RES_STYLESHEET, child->url, child->owner, 0, child);
    if (!child->req) { child->loading = false; child->failed = true; }
}

static void qualified_rule(SP *p, uint32_t ps, uint32_t pe, uint32_t bs, uint32_t be, const MediaCond *cond) {
    WebDoc *d = p->d;
    p->rules_seen = true;
    CssRule *r = wa_alloc(&d->arena, sizeof *r);
    if (!r) return;
    if (!sel_parse(d, &d->arena, &p->t, ps, pe, &r->sel)) {
        wa_free(&d->arena, r, sizeof *r);
        return;
    }
    css_parse_decls(d, &p->t, bs, be, &r->decls);
    r->cond = cond;
    vec_push(&d->arena, p->sheet->rules, r);
}

static void at_rule(SP *p, uint32_t at, uint32_t ps, uint32_t pe, int block, const MediaCond *cond) {
    const CssTokens *t = &p->t;
    const char *n = tok_val(t, &t->v[at]);
    size_t nn = t->v[at].vn;
#define IS(s) (nn == sizeof(s) - 1 && strncasecmp(n, s, nn) == 0)
    if (IS("import")) {
        if (block < 0) at_import(p, ps, pe);
        return;
    }
    if (IS("charset") || IS("namespace")) return;
    if (IS("layer")) {
        if (block < 0) return;                               /* a statement: layer order, ignored */
        p->rules_seen = true;
        rule_list(p, (uint32_t)block + 1, t->v[block].close, cond, false);
        return;
    }
    p->rules_seen = true;
    if (block < 0) return;
    if (IS("media")) {
        MediaCond *mc = wa_alloc(&p->d->arena, sizeof *mc);
        if (!mc) return;
        mc->m = media_parse(p->d, t, ps, pe);
        mc->parent = cond;
        rule_list(p, (uint32_t)block + 1, t->v[block].close, mc, false);
        return;
    }
    if (IS("supports")) {
        if (sup_cond(p, ps, pe)) rule_list(p, (uint32_t)block + 1, t->v[block].close, cond, false);
        return;
    }
    if (IS("document") || IS("-moz-document") || IS("scope") || IS("starting-style")) return;
    /* @font-face, @keyframes, @page, @container, @counter-style, @property, @font-feature-values... */
#undef IS
}

static void rule_list(SP *p, uint32_t i, uint32_t end, const MediaCond *cond, bool top) {
    const CssTokens *t = &p->t;
    while (i < end) {
        const CssTok *k = &t->v[i];
        if (k->type == CT_WS || (top && (k->type == CT_CDO || k->type == CT_CDC))) { i++; continue; }
        if (k->type == CT_AT) {
            uint32_t at = i++;
            uint32_t ps = i;
            while (i < end && t->v[i].type != CT_SEMI && t->v[i].type != CT_LBRACE) i = skip_component(t, i, end);
            uint32_t pe = i;
            int block = -1;
            if (i < end && t->v[i].type == CT_LBRACE) { block = (int)i; i = skip_component(t, i, end); }
            else if (i < end) i++;
            at_rule(p, at, ps, pe, block, cond);
            continue;
        }
        /* a qualified rule: prelude up to a {} block */
        uint32_t ps = i;
        while (i < end && t->v[i].type != CT_LBRACE) i = skip_component(t, i, end);
        if (i >= end) return;                                /* no block: dropped */
        uint32_t pe = i, bs = i + 1, be = t->v[i].close < end ? t->v[i].close : end;
        i = skip_component(t, i, end);
        qualified_rule(p, ps, pe, bs, be, cond);
    }
}

void css_parse_into(WebDoc *d, CssSheet *sheet, const char *text, size_t n, int depth) {
    SP p;
    memset(&p, 0, sizeof p);
    p.d = d;
    p.sheet = sheet;
    p.depth = depth;
    p.base = sheet->url ? sheet->url : d->base_url;
    WebArena tmp;
    wa_init(&tmp, &d->budget);
    if (!css_tokenize(&tmp, text, n, &p.t)) { wa_destroy(&tmp); return; }
    /* the token array is temporary but its strings are referenced by declarations only through copies */
    rule_list(&p, 0, p.t.n, NULL, true);
    wa_destroy(&tmp);
}

CssSheet *css_parse_sheet(WebDoc *d, const char *text, size_t n, uint8_t origin, const char *base_url, int depth) {
    CssSheet *s = wa_alloc(&d->arena, sizeof *s);
    if (!s) return NULL;
    s->origin = origin;
    s->url = base_url ? wa_strdup(&d->arena, base_url) : NULL;
    css_parse_into(d, s, text, n, depth);
    return s;
}
