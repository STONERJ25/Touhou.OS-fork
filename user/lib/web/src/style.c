/*
 * The cascade and computed values (CSS Cascading 4 and Values 4, CSS 2.1 chapter 6, HTML "rendering" for the
 * presentational hints).
 *
 * Sheets: the user-agent sheet (uasheet.c), the embedder's user sheet, and the author sheets of <style> and
 * <link rel=stylesheet> elements in tree order, with their @import children before their own rules. Every selector of
 * every rule whose @media conditions match goes into one bucket of a rule index, by its subject's id, else first class,
 * else tag, else "universal"; an element is matched against its id, class, tag and universal buckets only.
 *
 * Cascade order, lowest first: user-agent, user, presentational hints (author level, specificity 0), author rules by
 * specificity then order, the style attribute, then !important author, !important style attribute, !important user,
 * !important user-agent. Custom properties are cascaded first and their var() references resolved (cycles make the
 * property invalid), then font-size (em units depend on it), then everything else. "revert" goes back to the value after
 * the lower origins (a snapshot of the style taken between the levels).
 *
 * Memory: a computed style (CStyle) points to three groups, inherited text/font values (CInh), box values (CBox) and
 * visual values (CVis). A new style starts with its parent's CInh and the initial CBox/CVis and copies a group only when
 * a declaration writes to it, so most elements own little. An element whose matched rules equal its previous sibling's
 * (and that has no style attribute and no presentational hints) shares that sibling's CStyle. Styles live in one of two
 * arenas: a full restyle fills the other one and then releases the old one.
 */
#include "css.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern const char web_ua_css[];                              /* uasheet.c */
void css_parse_into(WebDoc *d, CssSheet *sheet, const char *text, size_t n, int depth);   /* css.c */
uint32_t res_request(WebDoc *d, WebResourceKind kind, const char *url, WebNode *owner, int mode, void *user);
bool css_named_color(const char *s, size_t n, WebColor *out);   /* cssval.c */
bool css_hex_color(const char *s, size_t n, WebColor *out);

/* ================================================================================================ the engine */

typedef struct RuleRef { CssRule *rule; const Selector *sel; uint32_t spec, order; uint8_t origin; } RuleRef;
typedef struct RuleVec { RuleRef *v; uint32_t n, cap; } RuleVec;
typedef struct Bucket { WebAtom *keys; RuleVec *vals; uint32_t cap, n; } Bucket;

typedef struct OwnedSheet { WebNode *owner; uint32_t hash; char *href; CssSheet *sheet; } OwnedSheet;

struct CssEngine {
    CssSheet *ua, *user;
    VEC(OwnedSheet) owned;
    VEC(CssSheet *) author;
    bool sheets_valid, index_valid;
    uint32_t dom_seen;
    WebArena iarena;                                         /* the rule index */
    Bucket ids, classes, tags;
    RuleVec universal;
    bool has_before, has_after, has_marker, has_placeholder, has_has, has_sibling, has_state;
    CInh init_inh;
    CBox init_box;
    CVis init_vis;
    CStyle init;
    WebArena sarena[2];
    int cur;
    size_t live;
    int vw_seen, vh_seen;
    bool any_vw;
    float root_fs;
};

static void init_styles(CssEngine *e) {
    CInh *i = &e->init_inh;
    memset(i, 0, sizeof *i);
    i->color = 0xFF000000u;
    i->font_size = 16;
    i->font_weight = 400;
    i->lh_kind = LH_NORMAL;
    i->text_align = TA_START;
    i->white_space = WS_NORMAL;
    i->list_type = LT_DISC;
    i->list_pos = 1;                                         /* outside */
    i->cursor = CUR_AUTO;
    i->tab_size = 8;
    i->empty_cells = 0;                                      /* show */
    static const char *const serif[] = { "serif" };
    static const FontList dflt = { serif, 1, 0 };
    i->family = &dflt;
    CBox *b = &e->init_box;
    memset(b, 0, sizeof *b);
    b->display = D_INLINE;
    b->z_auto = true;
    b->top.kind = b->right.kind = b->bottom.kind = b->left.kind = CL_AUTO;
    b->width.kind = b->height.kind = CL_AUTO;
    b->min_w.kind = b->min_h.kind = CL_AUTO;                 /* "auto": 0, except the automatic minimum of flex items */
    b->max_w.kind = b->max_h.kind = CL_NONE;
    b->flex_shrink = 1;
    b->flex_basis.kind = CL_AUTO;
    b->align_items = AL_NORMAL;
    b->align_self = AL_AUTO;
    b->justify_content = AL_NORMAL;
    b->align_content = AL_NORMAL;
    b->justify_items = AL_NORMAL;
    b->justify_self = AL_AUTO;
    b->gc_start.autoval = b->gc_end.autoval = b->gr_start.autoval = b->gr_end.autoval = true;
    CVis *v = &e->init_vis;
    memset(v, 0, sizeof *v);
    for (int s = 0; s < 4; s++) v->border_color[s] = COLOR_CURRENT;
    v->outline_color = COLOR_CURRENT;
    v->deco_color = COLOR_CURRENT;
    v->opacity = 1;
    v->bg_w.kind = v->bg_h.kind = CL_AUTO;
    v->bg_origin = 1;                                        /* padding-box */
    e->init.inh = &e->init_inh;
    e->init.box = &e->init_box;
    e->init.vis = &e->init_vis;
    e->init.display = D_INLINE;
}

static CssEngine *engine(WebDoc *d) {
    if (d->css) return d->css;
    CssEngine *e = wa_alloc(&d->arena, sizeof *e);
    if (!e) return NULL;
    wa_init(&e->iarena, &d->budget);
    wa_init(&e->sarena[0], &d->budget);
    wa_init(&e->sarena[1], &d->budget);
    init_styles(e);
    e->root_fs = 16;
    d->css = e;
    e->ua = css_parse_sheet(d, web_ua_css, strlen(web_ua_css), OR_UA, NULL, 0);
    if (d->cfg.user_stylesheet)
        e->user = css_parse_sheet(d, d->cfg.user_stylesheet, strlen(d->cfg.user_stylesheet), OR_USER, NULL, 0);
    return e;
}

void style_free(WebDoc *d) {
    CssEngine *e = d->css;
    if (!e) return;
    wa_destroy(&e->iarena);
    wa_destroy(&e->sarena[0]);
    wa_destroy(&e->sarena[1]);
}

const CStyle *style_initial(WebDoc *d) {
    CssEngine *e = engine(d);
    return e ? &e->init : NULL;
}

const CStyle *style_of(const WebNode *el) {
    return is_el(el) ? el->u.el.style : NULL;
}

void style_invalidate_all(WebDoc *d) {
    WebNode *root = web_doc_element(d);
    if (root) dom_mark_style_dirty(root);
    d->style_dirty = d->layout_dirty = true;
}

void style_sheets_changed(WebDoc *d) {
    CssEngine *e = engine(d);
    if (e) { e->sheets_valid = false; e->index_valid = false; }
    style_invalidate_all(d);
}

/* ================================================================================================ collecting sheets */

static bool rel_stylesheet(const char *rel) {
    bool sheet = false, alt = false;
    for (const char *p = rel; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t n = (size_t)(p - s);
        if (n == 10 && strncasecmp(s, "stylesheet", 10) == 0) sheet = true;
        if (n == 9 && strncasecmp(s, "alternate", 9) == 0) alt = true;
    }
    return sheet && !alt;
}

static OwnedSheet *owned_for(CssEngine *e, WebNode *n) {
    for (uint32_t i = 0; i < e->owned.n; i++) if (e->owned.v[i].owner == n) return &e->owned.v[i];
    return NULL;
}

static void collect_sheets(WebDoc *d, CssEngine *e) {
    e->author.n = 0;
    for (WebNode *n = d->root->first; n; n = dom_next(n, d->root)) {
        if (n->type != WEB_NODE_ELEMENT) continue;
        bool style = (n->ns == WEB_NS_HTML || n->ns == WEB_NS_SVG) && n->name == AT_style;
        bool link = is_html(n, AT_link) && dom_attr_val(n, AT_rel) && rel_stylesheet(dom_attr_val(n, AT_rel)) && dom_attr(n, AT_href);
        if (!style && !link) continue;
        if (link && dom_attr(n, AT_disabled)) continue;
        const char *type = dom_attr_val(n, AT_type);
        if (type && *type && strcasecmp(type, "text/css") != 0) continue;
        OwnedSheet *os = owned_for(e, n);
        if (style) {
            WebBuf b;
            buf_init(&b, &d->arena);
            dom_text_of(n, &b);
            uint32_t h = web_hash(b.p ? b.p : "", b.n) ^ b.n;
            if (!os || os->hash != h || !os->sheet) {
                CssSheet *s = wa_alloc(&d->arena, sizeof *s);
                if (s) {
                    s->origin = OR_AUTHOR;
                    s->owner = n;
                    css_parse_into(d, s, b.p ? b.p : "", b.n, 0);
                }
                if (!os) {
                    OwnedSheet rec = { n, h, NULL, s };
                    vec_push(&d->arena, e->owned, rec);
                    os = owned_for(e, n);
                } else {
                    os->hash = h;
                    os->sheet = s;
                }
            }
            buf_free(&b);
        } else {
            const char *href = dom_attr_val(n, AT_href);
            const char *url = doc_resolve_url(d, href, strlen(href), &d->arena);
            if (!url) continue;
            if (!os || !os->href || strcmp(os->href, url) != 0) {
                CssSheet *s = wa_alloc(&d->arena, sizeof *s);
                if (s) {
                    s->origin = OR_AUTHOR;
                    s->owner = n;
                    s->url = wa_strdup(&d->arena, url);
                    s->loading = true;
                }
                if (!os) {
                    OwnedSheet rec = { n, 0, wa_strdup(&d->arena, url), s };
                    vec_push(&d->arena, e->owned, rec);
                    os = owned_for(e, n);
                } else {
                    os->href = wa_strdup(&d->arena, url);
                    os->sheet = s;
                }
                if (s) {                                     /* (the loader may answer at once: the record exists) */
                    s->req = res_request(d, WEB_RES_STYLESHEET, url, n, 0, s);
                    if (!s->req) { s->loading = false; s->failed = true; }
                }
            }
        }
        if (os && os->sheet) {
            const char *media = dom_attr_val(n, AT_media);
            os->sheet->media = media ? media_parse_text(d, media, strlen(media)) : NULL;
            vec_push(&d->arena, e->author, os->sheet);
        }
    }
    e->sheets_valid = true;
}

/* a stylesheet (from <link> or @import) arrived */
void style_resource_done(WebDoc *d, void *user, int status, const void *data, size_t len) {
    CssSheet *s = user;
    if (!s) return;
    s->loading = false;
    if (status < 0 || !data) s->failed = true;
    else {
        /* CSS is decoded as UTF-8 (a @charset or a BOM naming another encoding is not honoured) */
        const char *p = data;
        if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) { p += 3; len -= 3; }
        css_parse_into(d, s, p, len, (int)s->version);
    }
    CssEngine *e = engine(d);
    if (e) e->index_valid = false;
    style_invalidate_all(d);
}

/* ================================================================================================ the rule index */

static RuleVec *bucket_get(CssEngine *e, Bucket *b, WebAtom key, bool create) {
    if (b->cap == 0) {
        if (!create) return NULL;
        b->cap = 64;
        b->keys = wa_alloc(&e->iarena, sizeof(WebAtom) * b->cap);
        b->vals = wa_alloc(&e->iarena, sizeof(RuleVec) * b->cap);
        if (!b->keys || !b->vals) { b->cap = 0; return NULL; }
    }
    uint32_t h = (key * 2654435761u) & (b->cap - 1);
    while (b->keys[h]) {
        if (b->keys[h] == key) return &b->vals[h];
        h = (h + 1) & (b->cap - 1);
    }
    if (!create) return NULL;
    if ((b->n + 1) * 2 > b->cap) {
        Bucket nb = { NULL, NULL, b->cap * 2, 0 };
        nb.keys = wa_alloc(&e->iarena, sizeof(WebAtom) * nb.cap);
        nb.vals = wa_alloc(&e->iarena, sizeof(RuleVec) * nb.cap);
        if (!nb.keys || !nb.vals) return NULL;
        for (uint32_t i = 0; i < b->cap; i++) {
            if (!b->keys[i]) continue;
            uint32_t k = (b->keys[i] * 2654435761u) & (nb.cap - 1);
            while (nb.keys[k]) k = (k + 1) & (nb.cap - 1);
            nb.keys[k] = b->keys[i];
            nb.vals[k] = b->vals[i];
            nb.n++;
        }
        *b = nb;
        return bucket_get(e, b, key, true);
    }
    b->keys[h] = key;
    memset(&b->vals[h], 0, sizeof(RuleVec));
    b->n++;
    return &b->vals[h];
}

/* does the selector (or a selector list inside it) depend on :hover, :active, :focus or :focus-within? */
static bool uses_state(const Selector *s, int depth) {
    if (depth > 8) return true;
    for (int k = 0; k < s->n; k++)
        for (int q = 0; q < s->c[k].npseudos; q++) {
            const SelPseudo *p = &s->c[k].pseudos[q];
            if (p->type == PC_HOVER || p->type == PC_ACTIVE || p->type == PC_FOCUS || p->type == PC_FOCUS_WITHIN ||
                p->type == PC_FOCUS_VISIBLE) return true;
            if (p->list)
                for (uint32_t i = 0; i < p->list->n; i++) if (uses_state(&p->list->v[i], depth + 1)) return true;
        }
    return false;
}

static void index_rule(CssEngine *e, CssRule *r, uint8_t origin) {
    for (uint32_t i = 0; i < r->sel.n; i++) {
        const Selector *s = &r->sel.v[i];
        const SelCompound *c = &s->c[0];
        RuleVec *v;
        if (c->id) v = bucket_get(e, &e->ids, c->id, true);
        else if (c->nclasses) v = bucket_get(e, &e->classes, c->classes[0], true);
        else if (c->tag) v = bucket_get(e, &e->tags, c->tag, true);
        else v = &e->universal;
        if (!v) continue;
        RuleRef ref = { r, s, s->spec, r->order, origin };
        vec_push(&e->iarena, *v, ref);
        if (s->pseudo_el == PSE_BEFORE) e->has_before = true;
        if (s->pseudo_el == PSE_AFTER) e->has_after = true;
        if (s->pseudo_el == PSE_MARKER) e->has_marker = true;
        if (s->pseudo_el == PSE_PLACEHOLDER) e->has_placeholder = true;
        if (!e->has_state && uses_state(s, 0)) e->has_state = true;
        for (int k = 0; k < s->n; k++) {
            if (s->c[k].comb == CB_ADJACENT || s->c[k].comb == CB_SIBLING) e->has_sibling = true;
            for (int q = 0; q < s->c[k].npseudos; q++) if (s->c[k].pseudos[q].type == PC_HAS) e->has_has = true;
        }
    }
}

static void index_sheet(WebDoc *d, CssEngine *e, CssSheet *s, uint8_t origin, uint32_t *order, int depth) {
    if (!s || s->failed || s->disabled || depth > 16) return;
    if (!media_matches(d, s->media)) return;
    for (uint32_t i = 0; i < s->imports.n; i++) index_sheet(d, e, s->imports.v[i], origin, order, depth + 1);
    for (uint32_t i = 0; i < s->rules.n; i++) {
        CssRule *r = s->rules.v[i];
        if (r->cond && !media_cond_matches(d, r->cond)) continue;
        r->order = (*order)++;
        index_rule(e, r, origin);
    }
}

static void build_index(WebDoc *d, CssEngine *e) {
    wa_reset(&e->iarena);
    memset(&e->ids, 0, sizeof e->ids);
    memset(&e->classes, 0, sizeof e->classes);
    memset(&e->tags, 0, sizeof e->tags);
    memset(&e->universal, 0, sizeof e->universal);
    e->has_before = e->has_after = e->has_marker = e->has_placeholder = e->has_has = e->has_sibling = false;
    e->has_state = false;
    uint32_t order = 1;
    index_sheet(d, e, e->ua, OR_UA, &order, 0);
    index_sheet(d, e, e->user, OR_USER, &order, 0);
    for (uint32_t i = 0; i < e->author.n; i++) index_sheet(d, e, e->author.v[i], OR_AUTHOR, &order, 0);
    e->index_valid = true;
    e->vw_seen = d->vw;
    e->vh_seen = d->vh;
}

/* ================================================================================================ matching an element */

typedef struct Match { CssRule *rule; uint32_t spec, order; uint8_t origin; } Match;
typedef struct Matches { Match *v; uint32_t n, cap; } Matches;

static void add_matches(RuleVec *v, WebNode *el, MatchCtx *ctx, Matches *out, WebArena *a) {
    if (!v) return;
    for (uint32_t i = 0; i < v->n; i++) {
        RuleRef *r = &v->v[i];
        if (!sel_matches(r->sel, el, ctx)) continue;
        bool dup = false;                                    /* a rule matched by two selectors counts once */
        for (uint32_t k = 0; k < out->n; k++)
            if (out->v[k].rule == r->rule) { if (r->spec > out->v[k].spec) out->v[k].spec = r->spec; dup = true; break; }
        if (dup) continue;
        Match m = { r->rule, r->spec, r->order, r->origin };
        if (!vec_push(a, *out, m)) return;
    }
}

static int cmp_match(const void *a, const void *b) {
    const Match *x = a, *y = b;
    if (x->spec != y->spec) return x->spec < y->spec ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static void match_element(WebDoc *d, CssEngine *e, WebNode *el, int pseudo, Matches *out, WebArena *a) {
    MatchCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.pseudo = (uint8_t)pseudo;
    out->n = 0;
    add_matches(&e->universal, el, &ctx, out, a);
    WebAtom tag = el->name;
    if (el->ns != WEB_NS_HTML) tag = atom_get_lower(&d->atoms, atom_str(&d->atoms, el->name), atom_len(&d->atoms, el->name));
    add_matches(bucket_get(e, &e->tags, tag, false), el, &ctx, out, a);
    if (el->u.el.id) {
        add_matches(bucket_get(e, &e->ids, el->u.el.id, false), el, &ctx, out, a);
        if (d->quirks == 2) {
            WebAtom low = atom_get_lower(&d->atoms, atom_str(&d->atoms, el->u.el.id), atom_len(&d->atoms, el->u.el.id));
            if (low != el->u.el.id) add_matches(bucket_get(e, &e->ids, low, false), el, &ctx, out, a);
        }
    }
    for (int i = 0; i < el->u.el.nclasses; i++) add_matches(bucket_get(e, &e->classes, el->u.el.classes[i], false), el, &ctx, out, a);
    if (out->n > 1) qsort(out->v, out->n, sizeof(Match), cmp_match);
}

/* ================================================================================================ computing values */

typedef struct SCtx {
    WebDoc *d;
    CssEngine *e;
    WebNode *el;
    const CStyle *parent;
    CStyle *s;
    WebArena *a;
    bool own_inh, own_box, own_vis;
    bool is_root;
    float root_fs;
    CStyle snap_ua, snap_user;
    bool has_snap_ua, has_snap_user;
    CustomProps *vars;
    bool own_vars;
} SCtx;

static CInh *wi(SCtx *c) {
    if (!c->own_inh) {
        CInh *n = wa_alloc(c->a, sizeof *n);
        if (!n) return (CInh *)c->s->inh;
        *n = *c->s->inh;
        c->s->inh = n;
        c->own_inh = true;
    }
    return c->s->inh;
}

static CBox *wb(SCtx *c) {
    if (!c->own_box) {
        CBox *n = wa_alloc(c->a, sizeof *n);
        if (!n) return c->s->box;
        *n = *c->s->box;
        c->s->box = n;
        c->own_box = true;
    }
    return c->s->box;
}

static CVis *wv(SCtx *c) {
    if (!c->own_vis) {
        CVis *n = wa_alloc(c->a, sizeof *n);
        if (!n) return c->s->vis;
        *n = *c->s->vis;
        c->s->vis = n;
        c->own_vis = true;
    }
    return c->s->vis;
}

static float unit_px(SCtx *c, float v, int unit, bool font_size) {
    float em = font_size ? c->parent->inh->font_size : c->s->inh->font_size;
    switch (unit) {
    case SU_PX: return v;
    case SU_EM: return v * em;
    case SU_REM: return v * c->root_fs;
    case SU_EX: case SU_CH: return v * em * 0.5f;
    case SU_LH: return v * em * 1.2f;
    case SU_VW: c->s->has_vw = true; return v * (float)c->d->vw / 100;
    case SU_VH: c->s->has_vw = true; return v * (float)c->d->vh / 100;
    case SU_VMIN: c->s->has_vw = true; return v * (float)(c->d->vw < c->d->vh ? c->d->vw : c->d->vh) / 100;
    case SU_VMAX: c->s->has_vw = true; return v * (float)(c->d->vw > c->d->vh ? c->d->vw : c->d->vh) / 100;
    default: return v;
    }
}

static CalcEnv env_of(SCtx *c, bool font_size) {
    float em = font_size ? c->parent->inh->font_size : c->s->inh->font_size;
    CalcEnv e = { em, c->root_fs, (float)c->d->vw, (float)c->d->vh, em * 0.5f, em * 0.5f };
    return e;
}

static void to_len(SCtx *c, const SVal *v, CLen *out, bool font_size) {
    memset(out, 0, sizeof *out);
    switch (v->kind) {
    case SV_KW:
        switch (v->kw) {
        case K_AUTO: out->kind = CL_AUTO; break;
        case K_NONE: out->kind = CL_NONE; break;
        case K_MIN_CONTENT: out->kind = CL_MIN_CONTENT; break;
        case K_MAX_CONTENT: out->kind = CL_MAX_CONTENT; break;
        case K_FIT_CONTENT: out->kind = CL_FIT_CONTENT; break;
        case K_CONTENT: out->kind = CL_CONTENT; break;
        default: out->kind = CL_PX; break;                   /* normal (gaps): 0 */
        }
        return;
    case SV_LEN:
        if (v->unit == SU_PCT) out->pct = v->num;
        else out->px = unit_px(c, v->num, v->unit, font_size);
        return;
    case SV_NUM:
        out->px = v->num;
        return;
    case SV_CALC: {
        CalcEnv e = env_of(c, font_size);
        float px, pct;
        if (calc_linear(v->u.calc, &e, &px, &pct)) { out->px = px; out->pct = pct; return; }
        out->kind = CL_CALC;
        out->calc = calc_resolve(c->a, v->u.calc, &e);
        if (!out->calc) out->kind = CL_AUTO;
        return;
    }
    default:
        out->kind = CL_AUTO;
        return;
    }
}

float clen_resolve(const CLen *l, float basis, bool *is_auto) {
    if (is_auto) *is_auto = false;
    switch (l->kind) {
    case CL_PX: return l->px + (l->pct != 0 ? l->pct * basis / 100 : 0);
    case CL_CALC: {
        CalcEnv e = { 16, 16, 0, 0, 8, 8 };
        return l->calc ? calc_eval(l->calc, &e, basis) : 0;
    }
    default:
        if (is_auto) *is_auto = true;
        return 0;
    }
}

static int kw_to(int kw, const int *pairs) {      /* pairs: kw, value, kw, value, ..., 0 */
    for (int i = 0; pairs[i]; i += 2) if (pairs[i] == kw) return pairs[i + 1];
    return -1;
}

#define MAP(...) ((const int[]){ __VA_ARGS__, 0 })

static const int display_map[] = {
    K_NONE, D_NONE, K_INLINE, D_INLINE, K_BLOCK, D_BLOCK, K_INLINE_BLOCK, D_INLINE_BLOCK, K_LIST_ITEM, D_LIST_ITEM,
    K_FLEX, D_FLEX, K_INLINE_FLEX, D_INLINE_FLEX, K_GRID, D_GRID, K_INLINE_GRID, D_INLINE_GRID, K_TABLE, D_TABLE,
    K_INLINE_TABLE, D_INLINE_TABLE, K_TABLE_ROW_GROUP, D_TABLE_ROW_GROUP, K_TABLE_HEADER_GROUP, D_TABLE_HEADER_GROUP,
    K_TABLE_FOOTER_GROUP, D_TABLE_FOOTER_GROUP, K_TABLE_ROW, D_TABLE_ROW, K_TABLE_CELL, D_TABLE_CELL,
    K_TABLE_COLUMN_GROUP, D_TABLE_COLUMN_GROUP, K_TABLE_COLUMN, D_TABLE_COLUMN, K_TABLE_CAPTION, D_TABLE_CAPTION,
    K_CONTENTS, D_CONTENTS, K_FLOW_ROOT, D_FLOW_ROOT, K_RUN_IN, D_BLOCK, K_RUBY, D_INLINE, K_RUBY_TEXT, D_INLINE,
    K_FLOW, D_BLOCK, 0 };

static int align_of(int kw) {
    return kw_to(kw, MAP(K_AUTO, AL_AUTO, K_NORMAL, AL_NORMAL, K_STRETCH, AL_STRETCH, K_START, AL_START, K_END, AL_END,
                         K_CENTER, AL_CENTER, K_FLEX_START, AL_FLEX_START, K_FLEX_END, AL_FLEX_END, K_BASELINE, AL_BASELINE,
                         K_LAST, AL_LAST_BASELINE, K_SPACE_BETWEEN, AL_SPACE_BETWEEN, K_SPACE_AROUND, AL_SPACE_AROUND,
                         K_SPACE_EVENLY, AL_SPACE_EVENLY, K_LEFT, AL_LEFT, K_RIGHT, AL_RIGHT, K_SELF_START, AL_SELF_START,
                         K_SELF_END, AL_SELF_END));
}

static int list_type_of(int kw) {
    return kw_to(kw, MAP(K_NONE, LT_NONE, K_DISC, LT_DISC, K_CIRCLE, LT_CIRCLE, K_SQUARE, LT_SQUARE, K_DECIMAL, LT_DECIMAL,
                         K_DECIMAL_LEADING_ZERO, LT_DECIMAL_LZ, K_LOWER_ALPHA, LT_LOWER_ALPHA, K_LOWER_LATIN, LT_LOWER_ALPHA,
                         K_UPPER_ALPHA, LT_UPPER_ALPHA, K_UPPER_LATIN, LT_UPPER_ALPHA, K_LOWER_ROMAN, LT_LOWER_ROMAN,
                         K_UPPER_ROMAN, LT_UPPER_ROMAN, K_LOWER_GREEK, LT_LOWER_GREEK, K_HIRAGANA, LT_HIRAGANA,
                         K_KATAKANA, LT_KATAKANA, K_CJK_DECIMAL, LT_CJK_DECIMAL, K_DISCLOSURE_OPEN, LT_DISCLOSURE_OPEN,
                         K_DISCLOSURE_CLOSED, LT_DISCLOSURE_CLOSED));
}

static int border_style_of(int kw) {
    return kw_to(kw, MAP(K_NONE, WEB_BORDER_NONE, K_HIDDEN, WEB_BORDER_NONE, K_SOLID, WEB_BORDER_SOLID, K_DOTTED, WEB_BORDER_DOTTED,
                         K_DASHED, WEB_BORDER_DASHED, K_DOUBLE, WEB_BORDER_DOUBLE, K_GROOVE, WEB_BORDER_GROOVE,
                         K_RIDGE, WEB_BORDER_RIDGE, K_INSET, WEB_BORDER_INSET, K_OUTSET, WEB_BORDER_OUTSET, K_AUTO, WEB_BORDER_SOLID));
}

static float border_width_of(SCtx *c, const SVal *v) {
    if (v->kind == SV_KW) return v->kw == K_THIN ? 1 : v->kw == K_THICK ? 5 : 3;
    CLen l;
    to_len(c, v, &l, false);
    float w = l.px;
    if (w > 0 && w < 1) w = 1;                               /* a positive width is at least one device pixel */
    return w;
}

static const char *resolve_url_in(SCtx *c, const char *u) {
    if (!u) return NULL;
    return doc_resolve_url(c->d, u, strlen(u), &c->d->arena);
}

static uint16_t weight_of(SCtx *c, const SVal *v) {
    uint16_t pw = c->parent->inh->font_weight;
    if (v->kind == SV_NUM) return (uint16_t)(v->num < 1 ? 1 : v->num > 1000 ? 1000 : v->num);
    switch (v->kw) {
    case K_BOLD: return 700;
    case K_BOLDER: return pw < 350 ? 400 : pw < 550 ? 700 : pw < 900 ? 900 : pw;
    case K_LIGHTER: return pw < 100 ? pw : pw < 550 ? 100 : pw < 750 ? 400 : 700;
    default: return 400;
    }
}

static float font_size_of(SCtx *c, const SVal *v) {
    float pf = c->parent->inh->font_size;
    if (v->kind == SV_KW) {
        switch (v->kw) {
        case K_XX_SMALL: return 9;
        case K_X_SMALL: return 10;
        case K_SMALL: return 13;
        case K_MEDIUM: return 16;
        case K_LARGE: return 18;
        case K_X_LARGE: return 24;
        case K_XX_LARGE: return 32;
        case K_XXX_LARGE: return 48;
        case K_SMALLER: return pf / 1.2f;
        case K_LARGER: return pf * 1.2f;
        default: return pf;
        }
    }
    if (v->kind == SV_LEN && v->unit == SU_PCT) return pf * v->num / 100;
    CLen l;
    to_len(c, v, &l, true);
    float px = clen_resolve(&l, pf, NULL);
    return px < 0 ? 0 : px;
}

/* Writes one longhand's specified value (or its CSS-wide keyword) into the style being computed. */
static void apply_prop(SCtx *c, int p, const SVal *v0, const SVal *v1, int wide);

static void copy_from(SCtx *c, int p, const CStyle *src) {
    /* "inherit" and "revert": the property's value from another style */
    const CInh *si = src->inh;
    const CBox *sb = src->box;
    const CVis *sv = src->vis;
    if (p < CP_COUNT && css_prop_inherited[p]) {
        CInh *i = wi(c);
        switch (p) {
        case CP_COLOR: i->color = si->color; break;
        case CP_FONT_FAMILY: i->family = si->family; break;
        case CP_FONT_SIZE: i->font_size = si->font_size; break;
        case CP_FONT_WEIGHT: i->font_weight = si->font_weight; break;
        case CP_FONT_STYLE: i->font_style = si->font_style; break;
        case CP_FONT_VARIANT: i->small_caps = si->small_caps; break;
        case CP_LINE_HEIGHT: i->lh_kind = si->lh_kind; i->lh = si->lh; break;
        case CP_TEXT_ALIGN: i->text_align = si->text_align; break;
        case CP_TEXT_INDENT: i->text_indent = si->text_indent; break;
        case CP_TEXT_TRANSFORM: i->text_transform = si->text_transform; break;
        case CP_LETTER_SPACING: i->letter_spacing = si->letter_spacing; break;
        case CP_WORD_SPACING: i->word_spacing = si->word_spacing; break;
        case CP_WHITE_SPACE: i->white_space = si->white_space; break;
        case CP_WORD_BREAK: i->word_break = si->word_break; break;
        case CP_OVERFLOW_WRAP: i->overflow_wrap = si->overflow_wrap; break;
        case CP_DIRECTION: i->direction = si->direction; break;
        case CP_LIST_STYLE_TYPE: i->list_type = si->list_type; i->list_string = si->list_string; break;
        case CP_LIST_STYLE_POSITION: i->list_pos = si->list_pos; break;
        case CP_LIST_STYLE_IMAGE: i->list_image = si->list_image; break;
        case CP_VISIBILITY: i->visibility = si->visibility; break;
        case CP_CURSOR: i->cursor = si->cursor; break;
        case CP_POINTER_EVENTS: i->pointer_events = si->pointer_events; break;
        case CP_BORDER_COLLAPSE: i->border_collapse = si->border_collapse; break;
        case CP_BORDER_SPACING: i->bs_h = si->bs_h; i->bs_v = si->bs_v; break;
        case CP_CAPTION_SIDE: i->caption_side = si->caption_side; break;
        case CP_EMPTY_CELLS: i->empty_cells = si->empty_cells; break;
        case CP_QUOTES: i->quotes = si->quotes; break;
        case CP_TAB_SIZE: i->tab_size = si->tab_size; break;
        default: break;
        }
        return;
    }
    CBox *b;
    CVis *v;
    switch (p) {
    case CP_DISPLAY: b = wb(c); b->display = sb->display; break;
    case CP_POSITION: b = wb(c); b->position = sb->position; break;
    case CP_TOP: b = wb(c); b->top = sb->top; break;
    case CP_RIGHT: b = wb(c); b->right = sb->right; break;
    case CP_BOTTOM: b = wb(c); b->bottom = sb->bottom; break;
    case CP_LEFT: b = wb(c); b->left = sb->left; break;
    case CP_FLOAT: b = wb(c); b->float_ = sb->float_; break;
    case CP_CLEAR: b = wb(c); b->clear = sb->clear; break;
    case CP_Z_INDEX: b = wb(c); b->z_index = sb->z_index; b->z_auto = sb->z_auto; break;
    case CP_WIDTH: b = wb(c); b->width = sb->width; break;
    case CP_HEIGHT: b = wb(c); b->height = sb->height; break;
    case CP_MIN_WIDTH: b = wb(c); b->min_w = sb->min_w; break;
    case CP_MIN_HEIGHT: b = wb(c); b->min_h = sb->min_h; break;
    case CP_MAX_WIDTH: b = wb(c); b->max_w = sb->max_w; break;
    case CP_MAX_HEIGHT: b = wb(c); b->max_h = sb->max_h; break;
    case CP_MARGIN_TOP: case CP_MARGIN_RIGHT: case CP_MARGIN_BOTTOM: case CP_MARGIN_LEFT:
        b = wb(c); b->margin[p - CP_MARGIN_TOP] = sb->margin[p - CP_MARGIN_TOP]; break;
    case CP_PADDING_TOP: case CP_PADDING_RIGHT: case CP_PADDING_BOTTOM: case CP_PADDING_LEFT:
        b = wb(c); b->padding[p - CP_PADDING_TOP] = sb->padding[p - CP_PADDING_TOP]; break;
    case CP_BORDER_TOP_WIDTH: case CP_BORDER_RIGHT_WIDTH: case CP_BORDER_BOTTOM_WIDTH: case CP_BORDER_LEFT_WIDTH:
        v = wv(c); v->border_w[p - CP_BORDER_TOP_WIDTH] = sv->border_w[p - CP_BORDER_TOP_WIDTH]; break;
    case CP_BORDER_TOP_STYLE: case CP_BORDER_RIGHT_STYLE: case CP_BORDER_BOTTOM_STYLE: case CP_BORDER_LEFT_STYLE:
        v = wv(c); v->border_style[p - CP_BORDER_TOP_STYLE] = sv->border_style[p - CP_BORDER_TOP_STYLE]; break;
    case CP_BORDER_TOP_COLOR: case CP_BORDER_RIGHT_COLOR: case CP_BORDER_BOTTOM_COLOR: case CP_BORDER_LEFT_COLOR:
        v = wv(c); v->border_color[p - CP_BORDER_TOP_COLOR] = sv->border_color[p - CP_BORDER_TOP_COLOR]; break;
    case CP_BORDER_TOP_LEFT_RADIUS: case CP_BORDER_TOP_RIGHT_RADIUS: case CP_BORDER_BOTTOM_RIGHT_RADIUS:
    case CP_BORDER_BOTTOM_LEFT_RADIUS:
        v = wv(c); v->radius[p - CP_BORDER_TOP_LEFT_RADIUS] = sv->radius[p - CP_BORDER_TOP_LEFT_RADIUS]; break;
    case CP_OUTLINE_WIDTH: v = wv(c); v->outline_w = sv->outline_w; break;
    case CP_OUTLINE_STYLE: v = wv(c); v->outline_style = sv->outline_style; break;
    case CP_OUTLINE_COLOR: v = wv(c); v->outline_color = sv->outline_color; break;
    case CP_OUTLINE_OFFSET: v = wv(c); v->outline_offset = sv->outline_offset; break;
    case CP_BOX_SIZING: b = wb(c); b->box_sizing = sb->box_sizing; break;
    case CP_OVERFLOW_X: b = wb(c); b->overflow_x = sb->overflow_x; break;
    case CP_OVERFLOW_Y: b = wb(c); b->overflow_y = sb->overflow_y; break;
    case CP_OPACITY: v = wv(c); v->opacity = sv->opacity; break;
    case CP_BACKGROUND_COLOR: v = wv(c); v->bg_color = sv->bg_color; break;
    case CP_BACKGROUND_IMAGE:
        v = wv(c); v->bg_image = sv->bg_image; v->bg_gradient[0] = sv->bg_gradient[0]; v->bg_gradient[1] = sv->bg_gradient[1];
        v->bg_gradient_h = sv->bg_gradient_h; break;
    case CP_BACKGROUND_POSITION_X: v = wv(c); v->bg_pos_x = sv->bg_pos_x; break;
    case CP_BACKGROUND_POSITION_Y: v = wv(c); v->bg_pos_y = sv->bg_pos_y; break;
    case CP_BACKGROUND_SIZE: v = wv(c); v->bg_size = sv->bg_size; v->bg_w = sv->bg_w; v->bg_h = sv->bg_h; break;
    case CP_BACKGROUND_REPEAT: v = wv(c); v->bg_repeat = sv->bg_repeat; break;
    case CP_BACKGROUND_ATTACHMENT: v = wv(c); v->bg_attach = sv->bg_attach; break;
    case CP_BACKGROUND_CLIP: v = wv(c); v->bg_clip = sv->bg_clip; break;
    case CP_BACKGROUND_ORIGIN: v = wv(c); v->bg_origin = sv->bg_origin; break;
    case CP_TEXT_DECORATION_LINE: v = wv(c); v->deco_line = sv->deco_line; break;
    case CP_TEXT_DECORATION_COLOR: v = wv(c); v->deco_color = sv->deco_color; break;
    case CP_TEXT_DECORATION_STYLE: v = wv(c); v->deco_style = sv->deco_style; break;
    case CP_TEXT_OVERFLOW: b = wb(c); b->text_overflow = sb->text_overflow; break;
    case CP_VERTICAL_ALIGN: b = wb(c); b->va_kind = sb->va_kind; b->va_len = sb->va_len; break;
    case CP_TABLE_LAYOUT: b = wb(c); b->table_layout = sb->table_layout; break;
    case CP_CONTENT: b = wb(c); b->content = sb->content; break;
    case CP_COUNTER_RESET: b = wb(c); b->c_reset = sb->c_reset; break;
    case CP_COUNTER_INCREMENT: b = wb(c); b->c_inc = sb->c_inc; break;
    case CP_COUNTER_SET: b = wb(c); b->c_set = sb->c_set; break;
    case CP_FLEX_DIRECTION: b = wb(c); b->flex_dir = sb->flex_dir; break;
    case CP_FLEX_WRAP: b = wb(c); b->flex_wrap = sb->flex_wrap; break;
    case CP_FLEX_GROW: b = wb(c); b->flex_grow = sb->flex_grow; break;
    case CP_FLEX_SHRINK: b = wb(c); b->flex_shrink = sb->flex_shrink; break;
    case CP_FLEX_BASIS: b = wb(c); b->flex_basis = sb->flex_basis; break;
    case CP_ORDER: b = wb(c); b->order = sb->order; break;
    case CP_JUSTIFY_CONTENT: b = wb(c); b->justify_content = sb->justify_content; break;
    case CP_ALIGN_ITEMS: b = wb(c); b->align_items = sb->align_items; break;
    case CP_ALIGN_SELF: b = wb(c); b->align_self = sb->align_self; break;
    case CP_ALIGN_CONTENT: b = wb(c); b->align_content = sb->align_content; break;
    case CP_JUSTIFY_ITEMS: b = wb(c); b->justify_items = sb->justify_items; break;
    case CP_JUSTIFY_SELF: b = wb(c); b->justify_self = sb->justify_self; break;
    case CP_ROW_GAP: b = wb(c); b->row_gap = sb->row_gap; break;
    case CP_COLUMN_GAP: b = wb(c); b->col_gap = sb->col_gap; break;
    case CP_GRID_TEMPLATE_COLUMNS: b = wb(c); b->tpl_cols = sb->tpl_cols; break;
    case CP_GRID_TEMPLATE_ROWS: b = wb(c); b->tpl_rows = sb->tpl_rows; break;
    case CP_GRID_TEMPLATE_AREAS: b = wb(c); b->areas = sb->areas; break;
    case CP_GRID_AUTO_COLUMNS: b = wb(c); b->auto_cols = sb->auto_cols; break;
    case CP_GRID_AUTO_ROWS: b = wb(c); b->auto_rows = sb->auto_rows; break;
    case CP_GRID_AUTO_FLOW: b = wb(c); b->auto_flow = sb->auto_flow; break;
    case CP_GRID_COLUMN_START: b = wb(c); b->gc_start = sb->gc_start; break;
    case CP_GRID_COLUMN_END: b = wb(c); b->gc_end = sb->gc_end; break;
    case CP_GRID_ROW_START: b = wb(c); b->gr_start = sb->gr_start; break;
    case CP_GRID_ROW_END: b = wb(c); b->gr_end = sb->gr_end; break;
    case CP_ASPECT_RATIO: b = wb(c); b->aspect = sb->aspect; break;
    case CP_OBJECT_FIT: b = wb(c); b->object_fit = sb->object_fit; break;
    case CP_BOX_SHADOW: v = wv(c); v->shadow = sv->shadow; break;
    default: break;
    }
}

static void apply_prop(SCtx *c, int p, const SVal *v0, const SVal *v1, int wide) {
    if (p >= CP_COUNT) return;
    if (wide == K_UNSET) wide = css_prop_inherited[p] ? K_INHERIT : K_INITIAL;
    if (wide == K_INHERIT) { copy_from(c, p, c->parent); return; }
    if (wide == K_INITIAL) { copy_from(c, p, &c->e->init); return; }
    if (wide == K_REVERT) {
        /* back to the value after the lower origins; for the user-agent origin that is unset */
        const CStyle *snap = c->has_snap_user ? &c->snap_user : c->has_snap_ua ? &c->snap_ua : NULL;
        if (snap) copy_from(c, p, snap);
        else copy_from(c, p, css_prop_inherited[p] ? c->parent : &c->e->init);
        return;
    }
    CInh *i;
    CBox *b;
    CVis *v;
    int k = v0->kind == SV_KW ? v0->kw : -1;
    switch (p) {
    case CP_COLOR:
        i = wi(c);
        i->color = v0->u.color == COLOR_CURRENT ? c->parent->inh->color : v0->u.color;
        break;
    case CP_FONT_FAMILY: if (v0->kind == SV_PTR) { i = wi(c); i->family = v0->u.ptr; } break;
    case CP_FONT_SIZE: i = wi(c); i->font_size = font_size_of(c, v0); break;
    case CP_FONT_WEIGHT: i = wi(c); i->font_weight = weight_of(c, v0); break;
    case CP_FONT_STYLE: i = wi(c); i->font_style = k == K_ITALIC ? 1 : k == K_OBLIQUE ? 2 : 0; break;
    case CP_FONT_VARIANT: i = wi(c); i->small_caps = k == K_SMALL_CAPS; break;
    case CP_LINE_HEIGHT:
        i = wi(c);
        if (v0->kind == SV_KW) { i->lh_kind = LH_NORMAL; i->lh = 0; }
        else if (v0->kind == SV_NUM) { i->lh_kind = LH_NUMBER; i->lh = v0->num < 0 ? 0 : v0->num; }
        else {
            CLen l;
            to_len(c, v0, &l, false);
            i->lh_kind = LH_PX;
            i->lh = clen_resolve(&l, i->font_size, NULL);
            if (i->lh < 0) i->lh = 0;
        }
        break;
    case CP_TEXT_ALIGN:
        i = wi(c);
        i->text_align = (uint8_t)kw_to(k, MAP(K_START, TA_START, K_END, TA_END, K_LEFT, TA_LEFT, K_RIGHT, TA_RIGHT,
                                               K_CENTER, TA_CENTER, K_JUSTIFY, TA_JUSTIFY, K_MATCH_PARENT, TA_START,
                                               K_WEBKIT_CENTER, TA_WEBKIT_CENTER, K_WEBKIT_LEFT, TA_LEFT, K_WEBKIT_RIGHT, TA_RIGHT));
        if (k == K_MATCH_PARENT) i->text_align = c->parent->inh->text_align;
        break;
    case CP_TEXT_INDENT: i = wi(c); to_len(c, v0, &i->text_indent, false); break;
    case CP_TEXT_TRANSFORM: i = wi(c); i->text_transform = (uint8_t)(k == K_UPPERCASE ? 1 : k == K_LOWERCASE ? 2 : k == K_CAPITALIZE ? 3 : 0); break;
    case CP_LETTER_SPACING: case CP_WORD_SPACING: {
        i = wi(c);
        float px = 0;
        if (v0->kind != SV_KW) { CLen l; to_len(c, v0, &l, false); px = clen_resolve(&l, i->font_size, NULL); }
        if (p == CP_LETTER_SPACING) i->letter_spacing = px; else i->word_spacing = px;
        break;
    }
    case CP_WHITE_SPACE:
        i = wi(c);
        i->white_space = (uint8_t)kw_to(k, MAP(K_NORMAL, WS_NORMAL, K_PRE, WS_PRE, K_NOWRAP, WS_NOWRAP, K_PRE_WRAP, WS_PRE_WRAP,
                                                K_PRE_LINE, WS_PRE_LINE, K_BREAK_SPACES, WS_BREAK_SPACES));
        break;
    case CP_WORD_BREAK: i = wi(c); i->word_break = (uint8_t)(k == K_BREAK_ALL ? 1 : k == K_KEEP_ALL ? 2 : k == K_BREAK_WORD ? 3 : 0); break;
    case CP_OVERFLOW_WRAP: i = wi(c); i->overflow_wrap = (uint8_t)(k == K_BREAK_WORD ? 1 : k == K_ANYWHERE ? 2 : 0); break;
    case CP_DIRECTION: i = wi(c); i->direction = k == K_RTL; break;
    case CP_LIST_STYLE_TYPE:
        i = wi(c);
        if (v0->kind == SV_STR) { i->list_type = LT_STRING; i->list_string = v0->u.str; }
        else { int lt = list_type_of(k); i->list_type = (uint8_t)(lt < 0 ? LT_DECIMAL : lt); i->list_string = NULL; }
        break;
    case CP_LIST_STYLE_POSITION: i = wi(c); i->list_pos = k == K_OUTSIDE; break;
    case CP_LIST_STYLE_IMAGE: i = wi(c); i->list_image = v0->kind == SV_URL ? resolve_url_in(c, v0->u.str) : NULL; break;
    case CP_VISIBILITY: i = wi(c); i->visibility = (uint8_t)(k == K_HIDDEN ? 1 : k == K_COLLAPSE ? 2 : 0); break;
    case CP_CURSOR:
        i = wi(c);
        i->cursor = (uint8_t)kw_to(k, MAP(K_AUTO, CUR_AUTO, K_DEFAULT, CUR_DEFAULT, K_POINTER, CUR_POINTER, K_TEXT_KW, CUR_TEXT,
                                           K_MOVE, CUR_MOVE, K_WAIT, CUR_WAIT, K_PROGRESS, CUR_WAIT, K_HELP, CUR_HELP,
                                           K_NOT_ALLOWED, CUR_NOT_ALLOWED, K_CROSSHAIR, CUR_CROSSHAIR, K_GRAB, CUR_GRAB,
                                           K_GRABBING, CUR_GRAB, K_NONE, CUR_NONE));
        if (i->cursor == 255) i->cursor = CUR_DEFAULT;
        break;
    case CP_POINTER_EVENTS: i = wi(c); i->pointer_events = k == K_NONE; break;
    case CP_BORDER_COLLAPSE: i = wi(c); i->border_collapse = k == K_COLLAPSE; break;
    case CP_BORDER_SPACING: {
        i = wi(c);
        CLen h, vv;
        to_len(c, v0, &h, false);
        to_len(c, v1->kind ? v1 : v0, &vv, false);
        i->bs_h = h.px;
        i->bs_v = vv.px;
        break;
    }
    case CP_CAPTION_SIDE: i = wi(c); i->caption_side = k == K_BOTTOM; break;
    case CP_EMPTY_CELLS: i = wi(c); i->empty_cells = k == K_HIDE; break;
    case CP_QUOTES: i = wi(c); i->quotes = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_TAB_SIZE: i = wi(c); i->tab_size = (uint8_t)(v0->num < 0 ? 0 : v0->num > 64 ? 64 : v0->num); break;
    case CP_DISPLAY: { int dv = kw_to(k, display_map); if (dv >= 0) { b = wb(c); b->display = (uint8_t)dv; } break; }
    case CP_POSITION:
        b = wb(c);
        b->position = (uint8_t)kw_to(k, MAP(K_STATIC, POS_STATIC, K_RELATIVE, POS_RELATIVE, K_ABSOLUTE, POS_ABSOLUTE,
                                             K_FIXED, POS_FIXED, K_STICKY, POS_STICKY));
        break;
    case CP_TOP: b = wb(c); to_len(c, v0, &b->top, false); break;
    case CP_RIGHT: b = wb(c); to_len(c, v0, &b->right, false); break;
    case CP_BOTTOM: b = wb(c); to_len(c, v0, &b->bottom, false); break;
    case CP_LEFT: b = wb(c); to_len(c, v0, &b->left, false); break;
    case CP_FLOAT: b = wb(c); b->float_ = (uint8_t)(k == K_LEFT || k == K_INLINE_START ? 1 : k == K_RIGHT || k == K_INLINE_END ? 2 : 0); break;
    case CP_CLEAR:
        b = wb(c);
        b->clear = (uint8_t)(k == K_LEFT || k == K_INLINE_START ? 1 : k == K_RIGHT || k == K_INLINE_END ? 2 : k == K_BOTH ? 3 : 0);
        break;
    case CP_Z_INDEX: b = wb(c); b->z_auto = k == K_AUTO; b->z_index = k == K_AUTO ? 0 : (int32_t)v0->num; break;
    case CP_ORDER: b = wb(c); b->order = (int32_t)v0->num; break;
    case CP_WIDTH: b = wb(c); to_len(c, v0, &b->width, false); break;
    case CP_HEIGHT: b = wb(c); to_len(c, v0, &b->height, false); break;
    case CP_MIN_WIDTH: b = wb(c); to_len(c, v0, &b->min_w, false); break;
    case CP_MIN_HEIGHT: b = wb(c); to_len(c, v0, &b->min_h, false); break;
    case CP_MAX_WIDTH: b = wb(c); to_len(c, v0, &b->max_w, false); break;
    case CP_MAX_HEIGHT: b = wb(c); to_len(c, v0, &b->max_h, false); break;
    case CP_MARGIN_TOP: case CP_MARGIN_RIGHT: case CP_MARGIN_BOTTOM: case CP_MARGIN_LEFT:
        b = wb(c); to_len(c, v0, &b->margin[p - CP_MARGIN_TOP], false); break;
    case CP_PADDING_TOP: case CP_PADDING_RIGHT: case CP_PADDING_BOTTOM: case CP_PADDING_LEFT:
        b = wb(c); to_len(c, v0, &b->padding[p - CP_PADDING_TOP], false); break;
    case CP_BORDER_TOP_WIDTH: case CP_BORDER_RIGHT_WIDTH: case CP_BORDER_BOTTOM_WIDTH: case CP_BORDER_LEFT_WIDTH:
        v = wv(c); v->border_w[p - CP_BORDER_TOP_WIDTH] = border_width_of(c, v0); break;
    case CP_BORDER_TOP_STYLE: case CP_BORDER_RIGHT_STYLE: case CP_BORDER_BOTTOM_STYLE: case CP_BORDER_LEFT_STYLE: {
        int bs = border_style_of(k);
        v = wv(c);
        v->border_style[p - CP_BORDER_TOP_STYLE] = (uint8_t)(bs < 0 ? 0 : bs);
        break;
    }
    case CP_BORDER_TOP_COLOR: case CP_BORDER_RIGHT_COLOR: case CP_BORDER_BOTTOM_COLOR: case CP_BORDER_LEFT_COLOR:
        v = wv(c); v->border_color[p - CP_BORDER_TOP_COLOR] = v0->u.color; break;
    case CP_BORDER_TOP_LEFT_RADIUS: case CP_BORDER_TOP_RIGHT_RADIUS: case CP_BORDER_BOTTOM_RIGHT_RADIUS:
    case CP_BORDER_BOTTOM_LEFT_RADIUS:
        v = wv(c); to_len(c, v0, &v->radius[p - CP_BORDER_TOP_LEFT_RADIUS], false); break;
    case CP_OUTLINE_WIDTH: v = wv(c); v->outline_w = border_width_of(c, v0); break;
    case CP_OUTLINE_STYLE: { int bs = border_style_of(k); v = wv(c); v->outline_style = (uint8_t)(bs < 0 ? 0 : bs); break; }
    case CP_OUTLINE_COLOR: v = wv(c); v->outline_color = v0->u.color; break;
    case CP_OUTLINE_OFFSET: { CLen l; to_len(c, v0, &l, false); v = wv(c); v->outline_offset = l.px; break; }
    case CP_BOX_SIZING: b = wb(c); b->box_sizing = k == K_BORDER_BOX; break;
    case CP_OVERFLOW_X: case CP_OVERFLOW_Y: {
        int ov = kw_to(k, MAP(K_VISIBLE, OV_VISIBLE, K_HIDDEN, OV_HIDDEN, K_SCROLL, OV_SCROLL, K_AUTO, OV_AUTO, K_CLIP, OV_CLIP));
        b = wb(c);
        if (p == CP_OVERFLOW_X) b->overflow_x = (uint8_t)(ov < 0 ? 0 : ov); else b->overflow_y = (uint8_t)(ov < 0 ? 0 : ov);
        break;
    }
    case CP_OPACITY: v = wv(c); v->opacity = v0->num < 0 ? 0 : v0->num > 1 ? 1 : v0->num; break;
    case CP_BACKGROUND_COLOR: v = wv(c); v->bg_color = v0->u.color; break;
    case CP_BACKGROUND_IMAGE:
        v = wv(c);
        v->bg_image = NULL;
        v->bg_gradient[0] = v->bg_gradient[1] = 0;
        if (v0->kind == SV_URL) v->bg_image = resolve_url_in(c, v0->u.str);
        else if (v0->kind == SV_PTR && v0->kw == 1) {
            const struct { WebColor a, b; bool horizontal; } *g = v0->u.ptr;
            v->bg_gradient[0] = g->a;
            v->bg_gradient[1] = g->b;
            v->bg_gradient_h = g->horizontal;
        }
        break;
    case CP_BACKGROUND_POSITION_X: v = wv(c); to_len(c, v0, &v->bg_pos_x, false); break;
    case CP_BACKGROUND_POSITION_Y: v = wv(c); to_len(c, v0, &v->bg_pos_y, false); break;
    case CP_BACKGROUND_SIZE:
        v = wv(c);
        if (k == K_COVER) v->bg_size = BGS_COVER;
        else if (k == K_CONTAIN) v->bg_size = BGS_CONTAIN;
        else {
            to_len(c, v0, &v->bg_w, false);
            if (v1->kind) to_len(c, v1, &v->bg_h, false); else v->bg_h.kind = CL_AUTO;
            v->bg_size = (v->bg_w.kind == CL_AUTO && v->bg_h.kind == CL_AUTO) ? BGS_AUTO : BGS_LEN;
        }
        break;
    case CP_BACKGROUND_REPEAT:
        v = wv(c);
        v->bg_repeat = (uint8_t)kw_to(k, MAP(K_REPEAT, BG_REPEAT, K_REPEAT_X, BG_REPEAT_X, K_REPEAT_Y, BG_REPEAT_Y,
                                              K_NO_REPEAT, BG_NO_REPEAT, K_SPACE, BG_SPACE, K_ROUND, BG_ROUND));
        break;
    case CP_BACKGROUND_ATTACHMENT: v = wv(c); v->bg_attach = (uint8_t)(k == K_FIXED ? 1 : k == K_LOCAL ? 2 : 0); break;
    case CP_BACKGROUND_CLIP: v = wv(c); v->bg_clip = (uint8_t)(k == K_PADDING_BOX ? 1 : k == K_CONTENT_BOX ? 2 : 0); break;
    case CP_BACKGROUND_ORIGIN: v = wv(c); v->bg_origin = (uint8_t)(k == K_BORDER_BOX ? 0 : k == K_CONTENT_BOX ? 2 : 1); break;
    case CP_TEXT_DECORATION_LINE: v = wv(c); v->deco_line = (uint8_t)v0->num; break;
    case CP_TEXT_DECORATION_COLOR: v = wv(c); v->deco_color = v0->u.color; break;
    case CP_TEXT_DECORATION_STYLE:
        v = wv(c);
        v->deco_style = (uint8_t)kw_to(k, MAP(K_SOLID, WEB_BORDER_SOLID, K_DOUBLE, WEB_BORDER_DOUBLE, K_DOTTED, WEB_BORDER_DOTTED,
                                               K_DASHED, WEB_BORDER_DASHED, K_WAVY, WEB_BORDER_SOLID));
        break;
    case CP_TEXT_OVERFLOW: b = wb(c); b->text_overflow = k == K_ELLIPSIS; break;
    case CP_VERTICAL_ALIGN:
        b = wb(c);
        if (v0->kind == SV_KW)
            b->va_kind = (uint8_t)kw_to(k, MAP(K_BASELINE, VA_BASELINE, K_SUB, VA_SUB, K_SUPER, VA_SUPER, K_TEXT_TOP, VA_TEXT_TOP,
                                                K_TEXT_BOTTOM, VA_TEXT_BOTTOM, K_MIDDLE, VA_MIDDLE, K_TOP, VA_TOP, K_BOTTOM, VA_BOTTOM));
        else {
            b->va_kind = VA_LEN;
            to_len(c, v0, &b->va_len, false);
            if (b->va_len.pct != 0) {                        /* a percentage of line-height */
                float lh = c->s->inh->lh_kind == LH_PX ? c->s->inh->lh : c->s->inh->font_size * (c->s->inh->lh_kind == LH_NUMBER ? c->s->inh->lh : 1.2f);
                b->va_len.px += b->va_len.pct * lh / 100;
                b->va_len.pct = 0;
            }
        }
        break;
    case CP_TABLE_LAYOUT: b = wb(c); b->table_layout = k == K_FIXED; break;
    case CP_CONTENT:
        b = wb(c);
        if (v0->kind == SV_PTR) b->content = v0->u.ptr;
        else {
            static const CssContent none = { NULL, 0, true, false }, normal = { NULL, 0, false, true };
            b->content = k == K_NONE ? &none : &normal;
        }
        break;
    case CP_COUNTER_RESET: b = wb(c); b->c_reset = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_COUNTER_INCREMENT: b = wb(c); b->c_inc = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_COUNTER_SET: b = wb(c); b->c_set = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_FLEX_DIRECTION:
        b = wb(c); b->flex_dir = (uint8_t)kw_to(k, MAP(K_ROW, 0, K_ROW_REVERSE, 1, K_COLUMN, 2, K_COLUMN_REVERSE, 3)); break;
    case CP_FLEX_WRAP: b = wb(c); b->flex_wrap = (uint8_t)kw_to(k, MAP(K_NOWRAP, 0, K_WRAP, 1, K_WRAP_REVERSE, 2)); break;
    case CP_FLEX_GROW: b = wb(c); b->flex_grow = v0->num; break;
    case CP_FLEX_SHRINK: b = wb(c); b->flex_shrink = v0->num; break;
    case CP_FLEX_BASIS: b = wb(c); to_len(c, v0, &b->flex_basis, false); break;
    case CP_JUSTIFY_CONTENT: b = wb(c); b->justify_content = (uint8_t)align_of(k); break;
    case CP_ALIGN_ITEMS: b = wb(c); b->align_items = (uint8_t)align_of(k); break;
    case CP_ALIGN_SELF: b = wb(c); b->align_self = (uint8_t)align_of(k); break;
    case CP_ALIGN_CONTENT: b = wb(c); b->align_content = (uint8_t)align_of(k); break;
    case CP_JUSTIFY_ITEMS: b = wb(c); b->justify_items = (uint8_t)align_of(k); break;
    case CP_JUSTIFY_SELF: b = wb(c); b->justify_self = (uint8_t)align_of(k); break;
    case CP_ROW_GAP: b = wb(c); to_len(c, v0, &b->row_gap, false); break;
    case CP_COLUMN_GAP: b = wb(c); to_len(c, v0, &b->col_gap, false); break;
    case CP_GRID_TEMPLATE_COLUMNS: b = wb(c); b->tpl_cols = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_GRID_TEMPLATE_ROWS: b = wb(c); b->tpl_rows = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_GRID_TEMPLATE_AREAS: b = wb(c); b->areas = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_GRID_AUTO_COLUMNS: b = wb(c); b->auto_cols = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_GRID_AUTO_ROWS: b = wb(c); b->auto_rows = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    case CP_GRID_AUTO_FLOW: b = wb(c); b->auto_flow = (uint8_t)v0->num; break;
    case CP_GRID_COLUMN_START: case CP_GRID_COLUMN_END: case CP_GRID_ROW_START: case CP_GRID_ROW_END: {
        if (v0->kind != SV_PTR) break;
        b = wb(c);
        const GridLine *gl = v0->u.ptr;
        if (p == CP_GRID_COLUMN_START) b->gc_start = *gl;
        else if (p == CP_GRID_COLUMN_END) b->gc_end = *gl;
        else if (p == CP_GRID_ROW_START) b->gr_start = *gl;
        else b->gr_end = *gl;
        break;
    }
    case CP_ASPECT_RATIO: b = wb(c); b->aspect = v0->kind == SV_NUM ? v0->num : 0; break;
    case CP_OBJECT_FIT:
        b = wb(c); b->object_fit = (uint8_t)kw_to(k, MAP(K_FILL, 0, K_CONTAIN, 1, K_COVER, 2, K_NONE, 3, K_SCALE_DOWN, 4)); break;
    case CP_BOX_SHADOW: v = wv(c); v->shadow = v0->kind == SV_PTR ? v0->u.ptr : NULL; break;
    default: break;
    }
}

/* ================================================================================================ var() */

static const char *var_lookup(const CustomProps *v, WebAtom name) {
    if (!v) return NULL;
    for (int i = v->n - 1; i >= 0; i--) if (v->names[i] == name) return v->vals[i];
    return NULL;
}

/* Substitutes var(--x[, fallback]) in text; false when a reference without fallback is missing (invalid at computed
 * value time) or the nesting is too deep (cycles). */
static bool substitute(SCtx *c, const char *s, size_t n, WebBuf *out, int depth) {
    if (depth > 24) return false;
    size_t i = 0;
    while (i < n) {
        bool is_var = i + 4 <= n && strncasecmp(s + i, "var(", 4) == 0 && (i == 0 || !(((s[i - 1] | 0x20) >= 'a' && (s[i - 1] | 0x20) <= 'z') || s[i - 1] == '-'));
        bool is_env = i + 4 <= n && strncasecmp(s + i, "env(", 4) == 0 && (i == 0 || !(((s[i - 1] | 0x20) >= 'a' && (s[i - 1] | 0x20) <= 'z') || s[i - 1] == '-'));
        if (!is_var && !is_env) {
            if (s[i] == '"' || s[i] == '\'') {                 /* strings are copied as they are */
                char q = s[i];
                size_t st = i++;
                while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
                if (i < n) i++;
                buf_put(out, s + st, i - st);
                continue;
            }
            buf_putc(out, s[i++]);
            continue;
        }
        /* find the matching ')' */
        size_t a = i + 4, j = a;
        int level = 1;
        while (j < n && level) {
            if (s[j] == '(') level++;
            else if (s[j] == ')') { level--; if (!level) break; }
            else if (s[j] == '"' || s[j] == '\'') { char q = s[j++]; while (j < n && s[j] != q) j++; }
            j++;
        }
        if (level) return false;
        /* name [, fallback] */
        size_t ns = a;
        while (ns < j && (s[ns] == ' ' || s[ns] == '\t' || s[ns] == '\n')) ns++;
        size_t ne = ns;
        while (ne < j && s[ne] != ',' && s[ne] != ' ' && s[ne] != '\t' && s[ne] != '\n' && s[ne] != ')') ne++;
        size_t comma = ne;
        while (comma < j && s[comma] != ',') comma++;
        const char *val = NULL;
        if (is_var) {
            WebAtom name = atom_find(&c->d->atoms, s + ns, ne - ns);
            val = name ? var_lookup(c->vars, name) : NULL;
        }
        if (val && *val) {
            /* a value that cannot be substituted (a cycle, a missing reference) is the guaranteed-invalid value:
             * the fallback is used instead */
            uint32_t mark = out->n;
            if (!substitute(c, val, strlen(val), out, depth + 1)) { out->n = mark; val = NULL; }
        }
        if (!val || !*val) {
            if (comma >= j || !substitute(c, s + comma + 1, j - comma - 1, out, depth + 1)) return false;
        }
        i = j + 1;
    }
    return true;
}

static void apply_decl(SCtx *c, const CssDecl *dcl);

static void apply_pending(SCtx *c, const CssDecl *dcl) {
    WebBuf b;
    buf_init(&b, c->a);
    bool ok = substitute(c, dcl->v0.u.str, strlen(dcl->v0.u.str), &b, 0);
    CssDecls out = { NULL, 0, 0 };
    if (ok) ok = css_parse_value_text(c->d, dcl->via, b.p ? b.p : "", b.n, &out, dcl->important);
    if (ok) {
        for (uint32_t i = 0; i < out.n; i++)
            if (out.v[i].prop == dcl->prop) { apply_decl(c, &out.v[i]); break; }
    } else {
        /* invalid at computed-value time: the property behaves as unset */
        apply_prop(c, dcl->prop, NULL, NULL, K_UNSET);
    }
    if (out.v) wa_free(&c->d->arena, out.v, sizeof(CssDecl) * out.cap);
    buf_free(&b);
}

static void apply_decl(SCtx *c, const CssDecl *dcl) {
    if (dcl->prop == CP_CUSTOM) return;
    if (dcl->pending) { apply_pending(c, dcl); return; }
    apply_prop(c, dcl->prop, &dcl->v0, &dcl->v1, dcl->wide);
}

/* custom properties: the parent's table is shared until the element declares one; then it gets its own copy */
static void set_var(SCtx *c, WebAtom name, const char *val) {
    if (!c->own_vars) {
        CustomProps *n = wa_alloc(c->a, sizeof *n);
        if (!n) return;
        if (c->vars) *n = *c->vars;
        c->vars = n;
        c->s->vars = n;
        c->own_vars = true;
    }
    CustomProps *v = c->vars;
    for (int i = 0; i < v->n; i++)
        if (v->names[i] == name) {
            /* the arrays may still be the parent's: copy before writing */
            WebAtom *nn = wa_alloc(c->a, sizeof(WebAtom) * (size_t)v->n);
            const char **nv = wa_alloc(c->a, sizeof(char *) * (size_t)v->n);
            if (!nn || !nv) return;
            memcpy(nn, v->names, sizeof(WebAtom) * (size_t)v->n);
            memcpy(nv, v->vals, sizeof(char *) * (size_t)v->n);
            nv[i] = val;
            v->names = nn;
            v->vals = nv;
            return;
        }
    WebAtom *nn = wa_alloc(c->a, sizeof(WebAtom) * (size_t)(v->n + 1));
    const char **nv = wa_alloc(c->a, sizeof(char *) * (size_t)(v->n + 1));
    if (!nn || !nv) return;
    if (v->n) {
        memcpy(nn, v->names, sizeof(WebAtom) * (size_t)v->n);
        memcpy(nv, v->vals, sizeof(char *) * (size_t)v->n);
    }
    nn[v->n] = name;
    nv[v->n] = val;
    v->names = nn;
    v->vals = nv;
    v->n++;
}

/* ================================================================================================ presentational hints */

/* the HTML "rules for parsing a legacy colour value" */
static bool legacy_color(const char *s, WebColor *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\f' || s[n - 1] == '\r')) n--;
    if (!n || (n == 11 && strncasecmp(s, "transparent", 11) == 0)) return false;
    if (css_named_color(s, n, out)) return true;
    if (n == 4 && s[0] == '#' && css_hex_color(s + 1, 3, out)) return true;
    char buf[132];
    size_t k = 0;
    for (size_t i = 0; i < n && k < 128;) {
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        i += len;
        if (cp > 0xFFFF) { buf[k++] = '0'; if (k < 128) buf[k++] = '0'; }
        else buf[k++] = (char)(cp < 0x80 ? cp : 'x');
    }
    char *p = buf;
    size_t len = k;
    if (len && p[0] == '#') { p++; len--; }
    for (size_t i = 0; i < len; i++) {
        char ch = p[i];
        if (!((ch >= '0' && ch <= '9') || ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f'))) p[i] = '0';
    }
    while (len == 0 || len % 3) { if (p + len >= buf + sizeof buf - 1) break; p[len++] = '0'; }
    size_t part = len / 3;
    const char *r = p, *g = p + part, *b = p + 2 * part;
    size_t skip = part > 8 ? part - 8 : 0;
    r += skip; g += skip; b += skip;
    part -= skip;
    while (part > 2 && r[0] == '0' && g[0] == '0' && b[0] == '0') { r++; g++; b++; part--; }
    if (part > 2) part = 2;
    int hv[3] = { 0, 0, 0 };
    const char *comp[3] = { r, g, b };
    for (int ci = 0; ci < 3; ci++)
        for (size_t i = 0; i < part; i++) {
            char ch = comp[ci][i];
            int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch | 0x20) - 'a' + 10;
            hv[ci] = hv[ci] * 16 + v;
        }
    *out = 0xFF000000u | (uint32_t)hv[0] << 16 | (uint32_t)hv[1] << 8 | (uint32_t)hv[2];
    return true;
}

/* "rules for parsing dimension values": px or %, false when there are no digits */
static bool dimension(const char *s, float *v, bool *pct) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r') s++;
    if (*s < '0' || *s > '9') return false;
    double x = 0;
    while (*s >= '0' && *s <= '9') { x = x * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') { x += (*s - '0') * f; f /= 10; s++; }
    }
    *pct = *s == '%';
    *v = (float)x;
    return true;
}

static void hint_len(SCtx *c, int prop, const char *attr, bool nonzero) {
    float v;
    bool pct;
    if (!attr || !dimension(attr, &v, &pct)) return;
    if (nonzero && v == 0) return;
    SVal s;
    memset(&s, 0, sizeof s);
    s.kind = SV_LEN;
    s.unit = pct ? SU_PCT : SU_PX;
    s.num = v;
    apply_prop(c, prop, &s, &s, 0);
}

static void hint_color(SCtx *c, int prop, const char *attr) {
    WebColor col;
    if (!attr || !legacy_color(attr, &col)) return;
    SVal s;
    memset(&s, 0, sizeof s);
    s.kind = SV_COLOR;
    s.u.color = col;
    apply_prop(c, prop, &s, NULL, 0);
}

static void hint_kw(SCtx *c, int prop, int kw) {
    SVal s;
    memset(&s, 0, sizeof s);
    s.kind = SV_KW;
    s.kw = (uint16_t)kw;
    apply_prop(c, prop, &s, &s, 0);
}

static void hint_px(SCtx *c, int prop, float px) {
    SVal s;
    memset(&s, 0, sizeof s);
    s.kind = SV_LEN;
    s.unit = SU_PX;
    s.num = px;
    apply_prop(c, prop, &s, &s, 0);
}

static void hint_bg_image(SCtx *c, const char *url) {
    if (!url || !*url) return;
    SVal s;
    memset(&s, 0, sizeof s);
    s.kind = SV_URL;
    s.u.str = url;
    apply_prop(c, CP_BACKGROUND_IMAGE, &s, NULL, 0);
}

static WebNode *ancestor_table(WebNode *el) {
    for (WebNode *p = el->parent; p; p = p->parent) if (is_html(p, AT_table)) return p;
    return NULL;
}

static void hint_align_text(SCtx *c, const char *a, bool block_center) {
    if (!a) return;
    if (!strcasecmp(a, "left")) hint_kw(c, CP_TEXT_ALIGN, block_center ? K_WEBKIT_LEFT : K_LEFT);
    else if (!strcasecmp(a, "right")) hint_kw(c, CP_TEXT_ALIGN, block_center ? K_WEBKIT_RIGHT : K_RIGHT);
    else if (!strcasecmp(a, "center") || !strcasecmp(a, "middle")) hint_kw(c, CP_TEXT_ALIGN, block_center ? K_WEBKIT_CENTER : K_CENTER);
    else if (!strcasecmp(a, "justify")) hint_kw(c, CP_TEXT_ALIGN, K_JUSTIFY);
}

static void hint_valign(SCtx *c, const char *a) {
    if (!a) return;
    if (!strcasecmp(a, "top")) hint_kw(c, CP_VERTICAL_ALIGN, K_TOP);
    else if (!strcasecmp(a, "middle") || !strcasecmp(a, "center")) hint_kw(c, CP_VERTICAL_ALIGN, K_MIDDLE);
    else if (!strcasecmp(a, "bottom")) hint_kw(c, CP_VERTICAL_ALIGN, K_BOTTOM);
    else if (!strcasecmp(a, "baseline")) hint_kw(c, CP_VERTICAL_ALIGN, K_BASELINE);
}

static bool parse_uint(const char *s, long *out) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\f' || *s == '\r') s++;
    if (*s == '+') s++;
    if (*s < '0' || *s > '9') return false;
    long v = 0;
    while (*s >= '0' && *s <= '9' && v < 1000000) { v = v * 10 + (*s - '0'); s++; }
    *out = v;
    return true;
}

/* HTML 15.3 and 15.4: the attributes that act like author declarations with specificity 0 */
static bool hints(SCtx *c) {
    WebNode *el = c->el;
    if (el->ns != WEB_NS_HTML || !el->u.el.nattrs) return false;
    WebDoc *d = c->d;
    bool any = false;
    const char *v;
    switch (el->name) {
    case AT_body:
        if ((v = dom_attr_val(el, AT_bgcolor))) { hint_color(c, CP_BACKGROUND_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_text))) { hint_color(c, CP_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_background))) { hint_bg_image(c, v); any = true; }
        if ((v = dom_attr_val(el, AT_marginwidth))) { hint_len(c, CP_MARGIN_LEFT, v, false); hint_len(c, CP_MARGIN_RIGHT, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_marginheight))) { hint_len(c, CP_MARGIN_TOP, v, false); hint_len(c, CP_MARGIN_BOTTOM, v, false); any = true; }
        {
            static const char *const m[4] = { "topmargin", "rightmargin", "bottommargin", "leftmargin" };
            for (int s = 0; s < 4; s++) {
                WebAtom a = atom_find(&d->atoms, m[s], strlen(m[s]));
                if (a && (v = dom_attr_val(el, a))) { hint_len(c, CP_MARGIN_TOP + s, v, false); any = true; }
            }
        }
        break;
    case AT_a: case AT_area:
        if (dom_attr(el, AT_href)) {
            WebNode *body = web_doc_body(d);
            if (body && (v = dom_attr_val(body, AT_link))) { hint_color(c, CP_COLOR, v); any = true; }
        }
        break;
    case AT_table: {
        if ((v = dom_attr_val(el, AT_bgcolor))) { hint_color(c, CP_BACKGROUND_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_background))) { hint_bg_image(c, v); any = true; }
        if ((v = dom_attr_val(el, AT_width))) { hint_len(c, CP_WIDTH, v, true); any = true; }
        if ((v = dom_attr_val(el, AT_height))) { hint_len(c, CP_HEIGHT, v, false); any = true; }
        long sp;
        if ((v = dom_attr_val(el, AT_cellspacing)) && parse_uint(v, &sp)) { hint_px(c, CP_BORDER_SPACING, (float)sp); any = true; }
        if ((v = dom_attr_val(el, AT_border))) {
            long bw = 1;
            if (!parse_uint(v, &bw)) bw = 1;
            if (bw > 0) {
                for (int s = 0; s < 4; s++) {
                    hint_px(c, CP_BORDER_TOP_WIDTH + s, (float)bw);
                    hint_kw(c, CP_BORDER_TOP_STYLE + s, K_OUTSET);
                    SVal g;
                    memset(&g, 0, sizeof g);
                    g.kind = SV_COLOR;
                    g.u.color = 0xFF808080u;
                    apply_prop(c, CP_BORDER_TOP_COLOR + s, &g, NULL, 0);
                }
            }
            any = true;
        }
        if ((v = dom_attr_val(el, AT_align))) {
            if (!strcasecmp(v, "left")) hint_kw(c, CP_FLOAT, K_LEFT);
            else if (!strcasecmp(v, "right")) hint_kw(c, CP_FLOAT, K_RIGHT);
            else if (!strcasecmp(v, "center")) { hint_kw(c, CP_MARGIN_LEFT, K_AUTO); hint_kw(c, CP_MARGIN_RIGHT, K_AUTO); }
            any = true;
        }
        break;
    }
    case AT_td: case AT_th: {
        if ((v = dom_attr_val(el, AT_bgcolor))) { hint_color(c, CP_BACKGROUND_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_background))) { hint_bg_image(c, v); any = true; }
        const char *w = dom_attr_val(el, AT_width);
        if (w) { hint_len(c, CP_WIDTH, w, true); any = true; }
        if ((v = dom_attr_val(el, AT_height))) { hint_len(c, CP_HEIGHT, v, true); any = true; }
        if ((v = dom_attr_val(el, AT_align))) { hint_align_text(c, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_valign))) { hint_valign(c, v); any = true; }
        if (dom_attr(el, AT_nowrap)) {
            float wv;
            bool pct;
            if (!(w && dimension(w, &wv, &pct) && !pct && wv > 0)) hint_kw(c, CP_WHITE_SPACE, K_NOWRAP);
            any = true;
        }
        WebNode *t = ancestor_table(el);
        if (t) {
            long pad;
            if ((v = dom_attr_val(t, AT_cellpadding)) && parse_uint(v, &pad)) {
                for (int s = 0; s < 4; s++) hint_px(c, CP_PADDING_TOP + s, (float)pad);
                any = true;
            }
            long bw;
            if ((v = dom_attr_val(t, AT_border)) && (parse_uint(v, &bw) ? bw > 0 : true)) {
                for (int s = 0; s < 4; s++) {
                    hint_px(c, CP_BORDER_TOP_WIDTH + s, 1);
                    hint_kw(c, CP_BORDER_TOP_STYLE + s, K_INSET);
                    SVal g;
                    memset(&g, 0, sizeof g);
                    g.kind = SV_COLOR;
                    g.u.color = 0xFF808080u;
                    apply_prop(c, CP_BORDER_TOP_COLOR + s, &g, NULL, 0);
                }
                any = true;
            }
        }
        break;
    }
    case AT_tr: case AT_tbody: case AT_thead: case AT_tfoot:
        if ((v = dom_attr_val(el, AT_bgcolor))) { hint_color(c, CP_BACKGROUND_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_background))) { hint_bg_image(c, v); any = true; }
        if ((v = dom_attr_val(el, AT_align))) { hint_align_text(c, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_valign))) { hint_valign(c, v); any = true; }
        if (el->name == AT_tr && (v = dom_attr_val(el, AT_height))) { hint_len(c, CP_HEIGHT, v, false); any = true; }
        break;
    case AT_col: case AT_colgroup:
        if ((v = dom_attr_val(el, AT_width))) { hint_len(c, CP_WIDTH, v, true); any = true; }
        if ((v = dom_attr_val(el, AT_align))) { hint_align_text(c, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_valign))) { hint_valign(c, v); any = true; }
        break;
    case AT_img: case AT_object: case AT_embed: case AT_iframe: case AT_video: case AT_canvas: case AT_input: {
        if (el->name == AT_input) {
            const char *t = dom_attr_val(el, AT_type);
            if (!t || strcasecmp(t, "image")) break;
        }
        if ((v = dom_attr_val(el, AT_width))) { hint_len(c, CP_WIDTH, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_height))) { hint_len(c, CP_HEIGHT, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_hspace))) { hint_len(c, CP_MARGIN_LEFT, v, false); hint_len(c, CP_MARGIN_RIGHT, v, false); any = true; }
        if ((v = dom_attr_val(el, AT_vspace))) { hint_len(c, CP_MARGIN_TOP, v, false); hint_len(c, CP_MARGIN_BOTTOM, v, false); any = true; }
        if ((el->name == AT_img || el->name == AT_object) && (v = dom_attr_val(el, AT_border))) {
            long bw;
            if (parse_uint(v, &bw)) {
                for (int s = 0; s < 4; s++) { hint_px(c, CP_BORDER_TOP_WIDTH + s, (float)bw); hint_kw(c, CP_BORDER_TOP_STYLE + s, K_SOLID); }
                any = true;
            }
        }
        if (el->name == AT_iframe && (v = dom_attr_val(el, AT_frameborder)) && (!strcmp(v, "0") || !strcasecmp(v, "no"))) {
            for (int s = 0; s < 4; s++) hint_kw(c, CP_BORDER_TOP_STYLE + s, K_NONE);
            any = true;
        }
        if ((v = dom_attr_val(el, AT_align))) {
            if (!strcasecmp(v, "left")) hint_kw(c, CP_FLOAT, K_LEFT);
            else if (!strcasecmp(v, "right")) hint_kw(c, CP_FLOAT, K_RIGHT);
            else if (!strcasecmp(v, "top")) hint_kw(c, CP_VERTICAL_ALIGN, K_TOP);
            else if (!strcasecmp(v, "middle") || !strcasecmp(v, "absmiddle") || !strcasecmp(v, "center")) hint_kw(c, CP_VERTICAL_ALIGN, K_MIDDLE);
            else if (!strcasecmp(v, "bottom") || !strcasecmp(v, "baseline")) hint_kw(c, CP_VERTICAL_ALIGN, K_BASELINE);
            else if (!strcasecmp(v, "texttop")) hint_kw(c, CP_VERTICAL_ALIGN, K_TEXT_TOP);
            else if (!strcasecmp(v, "absbottom")) hint_kw(c, CP_VERTICAL_ALIGN, K_BOTTOM);
            any = true;
        }
        break;
    }
    case AT_hr: {
        if ((v = dom_attr_val(el, AT_width))) { hint_len(c, CP_WIDTH, v, true); any = true; }
        long sz;
        bool noshade = dom_attr(el, AT_noshade) != NULL;
        const char *col = dom_attr_val(el, AT_color);
        if ((v = dom_attr_val(el, AT_size)) && parse_uint(v, &sz) && sz > 0) {
            if (sz == 1 || noshade || col) { hint_px(c, CP_HEIGHT, (float)(sz > 2 ? sz - 2 : 0)); hint_px(c, CP_BORDER_BOTTOM_WIDTH, sz > 1 ? 1 : 0); }
            else hint_px(c, CP_HEIGHT, (float)(sz - 2 > 0 ? sz - 2 : 0));
            any = true;
        }
        if (col) {
            hint_color(c, CP_BACKGROUND_COLOR, col);
            for (int s = 0; s < 4; s++) { hint_color(c, CP_BORDER_TOP_COLOR + s, col); hint_kw(c, CP_BORDER_TOP_STYLE + s, K_SOLID); }
            any = true;
        } else if (noshade) {
            WebColor g = 0xFF808080u;
            SVal gv;
            memset(&gv, 0, sizeof gv);
            gv.kind = SV_COLOR;
            gv.u.color = g;
            apply_prop(c, CP_BACKGROUND_COLOR, &gv, NULL, 0);
            for (int s = 0; s < 4; s++) { apply_prop(c, CP_BORDER_TOP_COLOR + s, &gv, NULL, 0); hint_kw(c, CP_BORDER_TOP_STYLE + s, K_SOLID); }
            any = true;
        }
        if ((v = dom_attr_val(el, AT_align))) {
            if (!strcasecmp(v, "left")) { hint_px(c, CP_MARGIN_LEFT, 0); hint_kw(c, CP_MARGIN_RIGHT, K_AUTO); }
            else if (!strcasecmp(v, "right")) { hint_kw(c, CP_MARGIN_LEFT, K_AUTO); hint_px(c, CP_MARGIN_RIGHT, 0); }
            else if (!strcasecmp(v, "center")) { hint_kw(c, CP_MARGIN_LEFT, K_AUTO); hint_kw(c, CP_MARGIN_RIGHT, K_AUTO); }
            any = true;
        }
        break;
    }
    case AT_font: {
        if ((v = dom_attr_val(el, AT_color))) { hint_color(c, CP_COLOR, v); any = true; }
        if ((v = dom_attr_val(el, AT_face)) && *v) {
            CssDecls out = { NULL, 0, 0 };
            if (css_parse_value_text(d, CP_FONT_FAMILY, v, strlen(v), &out, false) && out.n) apply_decl(c, &out.v[0]);
            if (out.v) wa_free(&d->arena, out.v, sizeof(CssDecl) * out.cap);
            any = true;
        }
        if ((v = dom_attr_val(el, AT_size))) {
            /* the legacy font size: 1..7, or relative +n/-n to 3 */
            while (*v == ' ' || *v == '\t' || *v == '\n') v++;
            int mode = 0;
            if (*v == '+') { mode = 1; v++; } else if (*v == '-') { mode = -1; v++; }
            long n;
            if (parse_uint(v, &n)) {
                long sz = mode == 1 ? 3 + n : mode == -1 ? 3 - n : n;
                if (sz < 1) sz = 1;
                if (sz > 7) sz = 7;
                static const int kws[8] = { 0, K_X_SMALL, K_SMALL, K_MEDIUM, K_LARGE, K_X_LARGE, K_XX_LARGE, K_XXX_LARGE };
                hint_kw(c, CP_FONT_SIZE, kws[sz]);
                any = true;
            }
        }
        break;
    }
    case AT_div: case AT_p: case AT_h1: case AT_h2: case AT_h3: case AT_h4: case AT_h5: case AT_h6: case AT_caption:
    case AT_legend:
        if ((v = dom_attr_val(el, AT_align))) {
            hint_align_text(c, v, el->name == AT_div);
            any = true;
        }
        break;
    case AT_br:
        if ((v = dom_attr_val(el, AT_clear))) {
            if (!strcasecmp(v, "left")) hint_kw(c, CP_CLEAR, K_LEFT);
            else if (!strcasecmp(v, "right")) hint_kw(c, CP_CLEAR, K_RIGHT);
            else if (!strcasecmp(v, "all") || !strcasecmp(v, "both")) hint_kw(c, CP_CLEAR, K_BOTH);
            any = true;
        }
        break;
    case AT_ol: case AT_ul: case AT_li:
        if ((v = dom_attr_val(el, AT_type))) {
            int lt = -1;
            if (!strcmp(v, "1")) lt = K_DECIMAL;
            else if (!strcmp(v, "a")) lt = K_LOWER_ALPHA;
            else if (!strcmp(v, "A")) lt = K_UPPER_ALPHA;
            else if (!strcmp(v, "i")) lt = K_LOWER_ROMAN;
            else if (!strcmp(v, "I")) lt = K_UPPER_ROMAN;
            else if (!strcasecmp(v, "disc")) lt = K_DISC;
            else if (!strcasecmp(v, "circle")) lt = K_CIRCLE;
            else if (!strcasecmp(v, "square")) lt = K_SQUARE;
            else if (!strcasecmp(v, "none")) lt = K_NONE;
            if (lt >= 0) { hint_kw(c, CP_LIST_STYLE_TYPE, lt); any = true; }
        }
        break;
    case AT_textarea: case AT_pre:
        break;
    default:
        break;
    }
    return any;
}

/* ================================================================================================ computing a style */

static CssDecls *inline_decls(WebDoc *d, WebNode *el) {
    if (el->ns != WEB_NS_HTML && el->ns != WEB_NS_SVG) return NULL;
    const char *st = dom_attr_val(el, AT_style);
    if (!st) return NULL;
    if (!el->u.el.inline_decls) el->u.el.inline_decls = css_parse_style_attr(d, st, strlen(st));
    return el->u.el.inline_decls;
}

static bool decls_empty(const CssDecls *ds) { return !ds || !ds->n; }

typedef struct Applier {
    SCtx *c;
    const Matches *m;
    const CssDecls *inl;
} Applier;

/* applies the declarations of one cascade level: origin (or -1 for the style attribute), importance, and a filter */
enum { F_ALL = 0, F_CUSTOM, F_FONT_SIZE, F_REST };

static bool filter_ok(const CssDecl *d, int f) {
    if (f == F_CUSTOM) return d->prop == CP_CUSTOM;
    if (d->prop == CP_CUSTOM) return false;
    if (f == F_FONT_SIZE) return d->prop == CP_FONT_SIZE;
    if (f == F_REST) return d->prop != CP_FONT_SIZE;
    return true;
}

static void apply_one(SCtx *c, const CssDecl *d, int f) {
    if (f == F_CUSTOM) {
        set_var(c, d->name, d->v0.u.str);
        return;
    }
    apply_decl(c, d);
}

static void level(Applier *ap, int origin, bool important, int f) {
    SCtx *c = ap->c;
    if (origin < 0) {
        if (!ap->inl) return;
        for (uint32_t i = 0; i < ap->inl->n; i++) {
            const CssDecl *d = &ap->inl->v[i];
            if ((bool)d->important == important && filter_ok(d, f)) apply_one(c, d, f);
        }
        return;
    }
    for (uint32_t r = 0; r < ap->m->n; r++) {
        const Match *mt = &ap->m->v[r];
        if (mt->origin != origin) continue;
        const CssDecls *ds = &mt->rule->decls;
        for (uint32_t i = 0; i < ds->n; i++) {
            const CssDecl *d = &ds->v[i];
            if ((bool)d->important == important && filter_ok(d, f)) apply_one(c, d, f);
        }
    }
}

/* the whole cascade for one filter */
static void cascade(Applier *ap, int f, bool with_hints) {
    SCtx *c = ap->c;
    level(ap, OR_UA, false, f);
    if (f == F_REST) { c->snap_ua = *c->s; c->has_snap_ua = true; c->own_inh = c->own_box = c->own_vis = false; }
    level(ap, OR_USER, false, f);
    if (f == F_REST) { c->snap_user = *c->s; c->has_snap_user = true; c->own_inh = c->own_box = c->own_vis = false; }
    if (with_hints && f == F_REST) hints(c);
    level(ap, OR_AUTHOR, false, f);
    level(ap, -1, false, f);
    level(ap, OR_AUTHOR, true, f);
    level(ap, -1, true, f);
    level(ap, OR_USER, true, f);
    level(ap, OR_UA, true, f);
}

static int blockify(int d) {
    switch (d) {
    case D_INLINE: case D_INLINE_BLOCK: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP:
    case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_COLUMN_GROUP: case D_TABLE_COLUMN: case D_TABLE_CAPTION:
        return D_BLOCK;
    case D_INLINE_FLEX: return D_FLEX;
    case D_INLINE_GRID: return D_GRID;
    case D_INLINE_TABLE: return D_TABLE;
    default: return d;
    }
}

/* the adjustments of CSS 2.1 9.7 and of flex/grid items, after the cascade */
static void fixup(SCtx *c) {
    CBox *b = c->s->box;
    int disp = b->display;
    bool changed = false;
    if (disp != D_NONE) {
        bool abs = b->position == POS_ABSOLUTE || b->position == POS_FIXED;
        if (abs && b->float_) { b = wb(c); b->float_ = 0; changed = true; }
        int pd = c->parent ? c->parent->display : D_BLOCK;
        bool item = c->el && !c->is_root && (pd == D_FLEX || pd == D_INLINE_FLEX || pd == D_GRID || pd == D_INLINE_GRID) &&
                    c->s->pseudo != PSE_MARKER;
        int nd = disp;
        if (abs || b->float_ || c->is_root || item) nd = blockify(disp);
        if (c->is_root && disp == D_CONTENTS) nd = D_BLOCK;
        if (item && b->float_) { b = wb(c); b->float_ = 0; }
        if (nd != disp) { b = wb(c); b->display = (uint8_t)nd; changed = true; }
    }
    (void)changed;
    /* a border whose style is none has no width */
    const CVis *v = c->s->vis;
    for (int s = 0; s < 4; s++)
        if (v->border_style[s] == WEB_BORDER_NONE && v->border_w[s] != 0) { CVis *w = wv(c); w->border_w[s] = 0; v = w; }
    if (v->outline_style == WEB_BORDER_NONE && v->outline_w != 0) { CVis *w = wv(c); w->outline_w = 0; }
    c->s->display = c->s->box->display;
}

static CStyle *new_style(SCtx *c, const CStyle *parent) {
    CStyle *s = wa_alloc(c->a, sizeof *s);
    if (!s) return NULL;
    s->inh = parent ? parent->inh : &c->e->init_inh;
    s->box = &c->e->init_box;
    s->vis = &c->e->init_vis;
    s->vars = parent ? parent->vars : NULL;
    s->display = D_INLINE;
    return s;
}

/* computes the style of el (pseudo: PSE_NONE, or a pseudo-element of el) from its matches */
static CStyle *compute(WebDoc *d, CssEngine *e, WebNode *el, const CStyle *parent, int pseudo, const Matches *m,
                       const CssDecls *inl, bool is_root, WebArena *a) {
    SCtx c;
    memset(&c, 0, sizeof c);
    c.d = d;
    c.e = e;
    c.el = el;
    c.parent = parent ? parent : &e->init;
    c.a = a;
    c.is_root = is_root && pseudo == PSE_NONE;
    c.root_fs = c.is_root ? 16 : e->root_fs;
    c.s = new_style(&c, parent);
    if (!c.s) return NULL;
    c.s->pseudo = (uint8_t)pseudo;
    c.vars = (CustomProps *)c.s->vars;
    Applier ap = { &c, m, inl };
    cascade(&ap, F_CUSTOM, false);
    cascade(&ap, F_FONT_SIZE, false);
    if (c.is_root) c.root_fs = c.s->inh->font_size;
    c.has_snap_ua = c.has_snap_user = false;
    cascade(&ap, F_REST, pseudo == PSE_NONE);
    if (c.is_root) e->root_fs = c.s->inh->font_size;
    fixup(&c);
    if (c.s->has_vw) e->any_vw = true;
    return c.s;
}

/* ================================================================================================ restyling the tree */

typedef struct Restyle {
    WebDoc *d;
    CssEngine *e;
    WebArena *a;
    Matches m, pm;
    WebArena tmp;
} Restyle;

static bool same_matches(const Matches *a, const Match *b, uint32_t bn) {
    if (a->n != bn) return false;
    for (uint32_t i = 0; i < bn; i++)
        if (a->v[i].rule != b[i].rule || a->v[i].spec != b[i].spec) return false;
    return true;
}

static void pseudo_styles(Restyle *r, WebNode *el) {
    CssEngine *e = r->e;
    const CStyle *s = el->u.el.style;
    for (int k = 0; k < 4; k++) el->u.el.pstyle[k] = NULL;
    if (!s || s->display == D_NONE) return;
    static const uint8_t which[4] = { PSE_BEFORE, PSE_AFTER, PSE_MARKER, PSE_PLACEHOLDER };
    bool has[4] = { e->has_before, e->has_after, e->has_marker, e->has_placeholder };
    for (int k = 0; k < 4; k++) {
        if (!has[k]) continue;
        if (k == 3 && !(is_html(el, AT_input) || is_html(el, AT_textarea))) continue;
        if (k == 2 && s->display != D_LIST_ITEM) continue;
        match_element(r->d, e, el, which[k], &r->pm, &r->tmp);
        if (!r->pm.n) continue;
        CStyle *ps = compute(r->d, e, el, s, which[k], &r->pm, NULL, false, r->a);
        if (!ps) continue;
        if (k < 2) {
            const CssContent *ct = ps->box->content;
            if (!ct || ct->none || ct->normal || !ct->n || ps->display == D_NONE) continue;
        }
        el->u.el.pstyle[k] = ps;
    }
}

/* could the element have presentational hints (or anything else beyond its matched rules that feeds its style)? */
static bool may_have_hints(const WebNode *el) {
    if (el->ns != WEB_NS_HTML) return true;
    switch (el->name) {
    case AT_body: case AT_td: case AT_th: case AT_table: case AT_font: case AT_hr: return true;
    case AT_a: case AT_area: return dom_attr(el, AT_href) != NULL;
    default: break;
    }
    static const WebAtom attrs[] = { AT_bgcolor, AT_background, AT_width, AT_height, AT_align, AT_valign, AT_border,
                                     AT_hspace, AT_vspace, AT_type, AT_clear, AT_frameborder, AT_color, AT_size };
    for (int i = 0; i < el->u.el.nattrs; i++)
        for (size_t k = 0; k < sizeof attrs / sizeof attrs[0]; k++)
            if (el->u.el.attrs[i].name == attrs[k]) return true;
    return false;
}

static void restyle_el(Restyle *r, WebNode *el, const CStyle *parent, bool force, int depth, const Match **prev_m,
                       uint32_t *prev_n, CStyle **prev_style) {
    WebDoc *d = r->d;
    CssEngine *e = r->e;
    bool dirty = force || (el->flags & NF_STYLE_DIRTY) || !el->u.el.style;
    if (depth > 600) { el->u.el.style = NULL; return; }
    if (dirty) {
        match_element(d, e, el, PSE_NONE, &r->m, &r->tmp);
        const CssDecls *inl = inline_decls(d, el);
        bool simple = decls_empty(inl) && !may_have_hints(el);
        CStyle *s = NULL;
        if (simple && *prev_style && *prev_m && same_matches(&r->m, *prev_m, *prev_n)) {
            s = *prev_style;                                 /* style sharing with the previous sibling (same parent) */
        } else {
            s = compute(d, e, el, parent, PSE_NONE, &r->m, inl, el->parent && el->parent->type == WEB_NODE_DOCUMENT, r->a);
            if (simple && s) {
                /* remember this sibling's matches for the next one */
                Match *copy = wa_alloc(&r->tmp, sizeof(Match) * (r->m.n ? r->m.n : 1));
                if (copy) { memcpy(copy, r->m.v, sizeof(Match) * r->m.n); *prev_m = copy; *prev_n = r->m.n; *prev_style = s; }
            } else { *prev_m = NULL; *prev_style = NULL; }
        }
        el->u.el.style = s;
        pseudo_styles(r, el);
    }
    el->flags &= ~NF_STYLE_DIRTY;
    bool children = dirty || (el->flags & NF_CHILD_DIRTY);
    el->flags &= ~NF_CHILD_DIRTY;
    if (!children) return;
    const CStyle *mine = el->u.el.style;
    const Match *pm = NULL;
    uint32_t pn = 0;
    CStyle *ps = NULL;
    for (WebNode *k = el->first; k; k = k->next) {
        if (k->type != WEB_NODE_ELEMENT) continue;
        if (!mine || mine->display == D_NONE) {
            /* not rendered: no styles below (getComputedStyle computes on demand) */
            restyle_el(r, k, mine ? mine : &e->init, dirty, depth + 1, &pm, &pn, &ps);
            continue;
        }
        restyle_el(r, k, mine, dirty, depth + 1, &pm, &pn, &ps);
    }
}

void style_update(WebDoc *d) {
    CssEngine *e = engine(d);
    if (!e) return;
    WebNode *root = web_doc_element(d);
    if (!e->sheets_valid || e->dom_seen != d->dom_version) {
        collect_sheets(d, e);
        e->dom_seen = d->dom_version;
        e->index_valid = false;
    }
    bool full = false;
    if (!e->index_valid || e->vw_seen != d->vw || e->vh_seen != d->vh) {
        build_index(d, e);
        full = true;
    }
    if (!root) { d->style_dirty = false; return; }
    if (!full && !d->style_dirty) return;
    /* incremental restyles leave old styles behind in the current arena: start over when that garbage gets large */
    if (!full && e->sarena[e->cur].total > 2 * e->live + ((size_t)4 << 20)) full = true;
    Restyle r;
    memset(&r, 0, sizeof r);
    r.d = d;
    r.e = e;
    wa_init(&r.tmp, &d->budget);
    if (full) {
        int nxt = e->cur ^ 1;
        wa_reset(&e->sarena[nxt]);
        r.a = &e->sarena[nxt];
        e->any_vw = false;
        e->root_fs = 16;
    } else {
        r.a = &e->sarena[e->cur];
    }
    const Match *pm = NULL;
    uint32_t pn = 0;
    CStyle *ps = NULL;
    restyle_el(&r, root, NULL, full, 0, &pm, &pn, &ps);
    wa_destroy(&r.tmp);
    if (full) {
        wa_reset(&e->sarena[e->cur]);
        e->cur ^= 1;
        e->live = e->sarena[e->cur].total;
    }
    d->style_dirty = false;
    d->layout_dirty = true;
}

/* ================================================================================================ the API */

/* the style attribute as text: name: value pairs */
static bool find_decl_text(const char *s, const char *prop, size_t *vs, size_t *ve, size_t *ds, size_t *de) {
    size_t n = strlen(s), pn = strlen(prop);
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        int paren = 0;
        char q = 0;
        while (i < n) {
            char ch = s[i];
            if (q) { if (ch == q) q = 0; else if (ch == '\\') i++; }
            else if (ch == '"' || ch == '\'') q = ch;
            else if (ch == '(') paren++;
            else if (ch == ')') paren--;
            else if (ch == ';' && paren <= 0) break;
            i++;
        }
        size_t en = i;
        if (i < n) i++;
        size_t a = st;
        while (a < en && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n')) a++;
        size_t colon = a;
        while (colon < en && s[colon] != ':') colon++;
        if (colon >= en) continue;
        size_t ne = colon;
        while (ne > a && (s[ne - 1] == ' ' || s[ne - 1] == '\t' || s[ne - 1] == '\n')) ne--;
        if (ne - a == pn && strncasecmp(s + a, prop, pn) == 0) {
            size_t b = colon + 1;
            while (b < en && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n')) b++;
            size_t e = en;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n')) e--;
            *vs = b;
            *ve = e;
            *ds = st;
            *de = i;
            return true;
        }
    }
    return false;
}

char *web_element_style_get(WebNode *el, const char *prop) {
    const char *st = is_el(el) ? dom_attr_val(el, AT_style) : NULL;
    size_t vs, ve, ds, de;
    if (!st || !find_decl_text(st, prop, &vs, &ve, &ds, &de)) return NULL;
    size_t len = ve - vs;
    if (len >= 10 && strncasecmp(st + ve - 10, "!important", 10) == 0) {
        len -= 10;
        while (len && (st[vs + len - 1] == ' ' || st[vs + len - 1] == '\t')) len--;
    }
    char *r = web_raw_alloc(&el->doc->budget, len + 1);
    if (r) memcpy(r, st + vs, len);
    return r;
}

int web_element_style_set(WebNode *el, const char *prop, const char *value, bool important) {
    if (!is_el(el) || !prop) return WEB_E_ARG;
    WebDoc *d = el->doc;
    const char *st = dom_attr_val(el, AT_style);
    WebBuf b;
    buf_init(&b, &d->arena);
    size_t vs, ve, ds, de;
    if (st && find_decl_text(st, prop, &vs, &ve, &ds, &de)) {
        buf_put(&b, st, ds);
        if (value) {
            buf_puts(&b, prop);
            buf_puts(&b, ": ");
            buf_puts(&b, value);
            if (important) buf_puts(&b, " !important");
            buf_puts(&b, "; ");
        }
        buf_puts(&b, st + de);
    } else {
        if (st) {
            buf_puts(&b, st);
            size_t n = strlen(st);
            while (n && (st[n - 1] == ' ' || st[n - 1] == '\t')) n--;
            if (n && st[n - 1] != ';') buf_puts(&b, "; ");
        }
        if (value) {
            buf_puts(&b, prop);
            buf_puts(&b, ": ");
            buf_puts(&b, value);
            if (important) buf_puts(&b, " !important");
            buf_putc(&b, ';');
        }
    }
    int rc = web_element_set_attr(el, "style", b.p ? b.p : "");
    buf_free(&b);
    return rc;
}

static void put_len(WebBuf *b, const CLen *l) {
    char tmp[64];
    switch (l->kind) {
    case CL_AUTO: buf_puts(b, "auto"); return;
    case CL_NONE: buf_puts(b, "none"); return;
    case CL_MIN_CONTENT: buf_puts(b, "min-content"); return;
    case CL_MAX_CONTENT: buf_puts(b, "max-content"); return;
    case CL_FIT_CONTENT: buf_puts(b, "fit-content"); return;
    case CL_CONTENT: buf_puts(b, "content"); return;
    case CL_CALC: buf_puts(b, "calc(...)"); return;
    default: break;
    }
    if (l->pct != 0 && l->px != 0) snprintf(tmp, sizeof tmp, "calc(%g%% + %gpx)", (double)l->pct, (double)l->px);
    else if (l->pct != 0) snprintf(tmp, sizeof tmp, "%g%%", (double)l->pct);
    else snprintf(tmp, sizeof tmp, "%gpx", (double)l->px);
    buf_puts(b, tmp);
}

static void put_color(WebBuf *b, WebColor c, WebColor current) {
    char tmp[64];
    if (c == COLOR_CURRENT) c = current;
    unsigned a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255;
    if (a == 255) snprintf(tmp, sizeof tmp, "rgb(%u, %u, %u)", r, g, bl);
    else snprintf(tmp, sizeof tmp, "rgba(%u, %u, %u, %g)", r, g, bl, (double)((float)a / 255));
    buf_puts(b, tmp);
}

char *style_serialize(WebDoc *d, const CStyle *s, int p) {
    WebBuf b;
    buf_init(&b, &d->arena);
    char tmp[64];
    static const char *const displays[] = { "none", "inline", "block", "inline-block", "list-item", "flex", "inline-flex",
        "grid", "inline-grid", "table", "inline-table", "table-row-group", "table-header-group", "table-footer-group",
        "table-row", "table-cell", "table-column-group", "table-column", "table-caption", "contents", "flow-root" };
    static const char *const positions[] = { "static", "relative", "absolute", "fixed", "sticky" };
    static const char *const borders[] = { "none", "solid", "dotted", "dashed", "double", "groove", "ridge", "inset", "outset" };
    switch (p) {
    case CP_DISPLAY: buf_puts(&b, displays[s->display < 21 ? s->display : 0]); break;
    case CP_POSITION: buf_puts(&b, positions[s->box->position < 5 ? s->box->position : 0]); break;
    case CP_COLOR: put_color(&b, s->inh->color, s->inh->color); break;
    case CP_BACKGROUND_COLOR: put_color(&b, s->vis->bg_color, s->inh->color); break;
    case CP_BORDER_TOP_COLOR: case CP_BORDER_RIGHT_COLOR: case CP_BORDER_BOTTOM_COLOR: case CP_BORDER_LEFT_COLOR:
        put_color(&b, s->vis->border_color[p - CP_BORDER_TOP_COLOR], s->inh->color); break;
    case CP_BORDER_TOP_STYLE: case CP_BORDER_RIGHT_STYLE: case CP_BORDER_BOTTOM_STYLE: case CP_BORDER_LEFT_STYLE:
        buf_puts(&b, borders[s->vis->border_style[p - CP_BORDER_TOP_STYLE] < 9 ? s->vis->border_style[p - CP_BORDER_TOP_STYLE] : 0]); break;
    case CP_BORDER_TOP_WIDTH: case CP_BORDER_RIGHT_WIDTH: case CP_BORDER_BOTTOM_WIDTH: case CP_BORDER_LEFT_WIDTH:
        snprintf(tmp, sizeof tmp, "%gpx", (double)s->vis->border_w[p - CP_BORDER_TOP_WIDTH]); buf_puts(&b, tmp); break;
    case CP_FONT_SIZE: snprintf(tmp, sizeof tmp, "%gpx", (double)s->inh->font_size); buf_puts(&b, tmp); break;
    case CP_FONT_WEIGHT: snprintf(tmp, sizeof tmp, "%u", s->inh->font_weight); buf_puts(&b, tmp); break;
    case CP_FONT_STYLE: buf_puts(&b, s->inh->font_style == 1 ? "italic" : s->inh->font_style == 2 ? "oblique" : "normal"); break;
    case CP_FONT_FAMILY:
        for (int i = 0; s->inh->family && i < s->inh->family->n; i++) { if (i) buf_puts(&b, ", "); buf_puts(&b, s->inh->family->names[i]); }
        break;
    case CP_LINE_HEIGHT:
        if (s->inh->lh_kind == LH_NORMAL) buf_puts(&b, "normal");
        else if (s->inh->lh_kind == LH_NUMBER) { snprintf(tmp, sizeof tmp, "%g", (double)s->inh->lh); buf_puts(&b, tmp); }
        else { snprintf(tmp, sizeof tmp, "%gpx", (double)s->inh->lh); buf_puts(&b, tmp); }
        break;
    case CP_WIDTH: put_len(&b, &s->box->width); break;
    case CP_HEIGHT: put_len(&b, &s->box->height); break;
    case CP_MIN_WIDTH: put_len(&b, &s->box->min_w); break;
    case CP_MIN_HEIGHT: put_len(&b, &s->box->min_h); break;
    case CP_MAX_WIDTH: put_len(&b, &s->box->max_w); break;
    case CP_MAX_HEIGHT: put_len(&b, &s->box->max_h); break;
    case CP_TOP: put_len(&b, &s->box->top); break;
    case CP_RIGHT: put_len(&b, &s->box->right); break;
    case CP_BOTTOM: put_len(&b, &s->box->bottom); break;
    case CP_LEFT: put_len(&b, &s->box->left); break;
    case CP_MARGIN_TOP: case CP_MARGIN_RIGHT: case CP_MARGIN_BOTTOM: case CP_MARGIN_LEFT:
        put_len(&b, &s->box->margin[p - CP_MARGIN_TOP]); break;
    case CP_PADDING_TOP: case CP_PADDING_RIGHT: case CP_PADDING_BOTTOM: case CP_PADDING_LEFT:
        put_len(&b, &s->box->padding[p - CP_PADDING_TOP]); break;
    case CP_FLOAT: buf_puts(&b, s->box->float_ == 1 ? "left" : s->box->float_ == 2 ? "right" : "none"); break;
    case CP_VISIBILITY: buf_puts(&b, s->inh->visibility == 1 ? "hidden" : s->inh->visibility == 2 ? "collapse" : "visible"); break;
    case CP_OPACITY: snprintf(tmp, sizeof tmp, "%g", (double)s->vis->opacity); buf_puts(&b, tmp); break;
    case CP_Z_INDEX: if (s->box->z_auto) buf_puts(&b, "auto"); else { snprintf(tmp, sizeof tmp, "%d", s->box->z_index); buf_puts(&b, tmp); } break;
    case CP_TEXT_ALIGN: {
        static const char *const ta[] = { "start", "left", "right", "center", "justify", "end", "center" };
        buf_puts(&b, ta[s->inh->text_align < 7 ? s->inh->text_align : 0]);
        break;
    }
    case CP_WHITE_SPACE: {
        static const char *const ws[] = { "normal", "pre", "nowrap", "pre-wrap", "pre-line", "break-spaces" };
        buf_puts(&b, ws[s->inh->white_space < 6 ? s->inh->white_space : 0]);
        break;
    }
    case CP_BOX_SIZING: buf_puts(&b, s->box->box_sizing ? "border-box" : "content-box"); break;
    case CP_VERTICAL_ALIGN: {
        static const char *const va[] = { "baseline", "sub", "super", "text-top", "text-bottom", "middle", "top", "bottom" };
        if (s->box->va_kind == VA_LEN) put_len(&b, &s->box->va_len);
        else buf_puts(&b, va[s->box->va_kind < 8 ? s->box->va_kind : 0]);
        break;
    }
    case CP_LIST_STYLE_TYPE: {
        static const char *const lt[] = { "none", "disc", "circle", "square", "decimal", "decimal-leading-zero",
            "lower-alpha", "upper-alpha", "lower-roman", "upper-roman", "lower-greek", "hiragana", "katakana",
            "cjk-decimal", "disclosure-open", "disclosure-closed", "string" };
        buf_puts(&b, lt[s->inh->list_type < 17 ? s->inh->list_type : 0]);
        break;
    }
    case CP_LIST_STYLE_POSITION: buf_puts(&b, s->inh->list_pos ? "outside" : "inside"); break;
    case CP_BACKGROUND_IMAGE: if (s->vis->bg_image) { buf_puts(&b, "url(\""); buf_puts(&b, s->vis->bg_image); buf_puts(&b, "\")"); } else buf_puts(&b, "none"); break;
    case CP_TEXT_DECORATION_LINE: {
        int dl = s->vis->deco_line;
        if (!dl) buf_puts(&b, "none");
        if (dl & DECO_UNDERLINE) buf_puts(&b, "underline");
        if (dl & DECO_OVERLINE) buf_puts(&b, (dl & DECO_UNDERLINE) ? " overline" : "overline");
        if (dl & DECO_LINE_THROUGH) buf_puts(&b, (dl & 3) ? " line-through" : "line-through");
        break;
    }
    case CP_CURSOR: {
        static const char *const cu[] = { "auto", "default", "pointer", "text", "move", "wait", "help", "not-allowed",
                                          "crosshair", "grab", "none" };
        buf_puts(&b, cu[s->inh->cursor < 11 ? s->inh->cursor : 0]);
        break;
    }
    case CP_BORDER_SPACING: snprintf(tmp, sizeof tmp, "%gpx %gpx", (double)s->inh->bs_h, (double)s->inh->bs_v); buf_puts(&b, tmp); break;
    case CP_TEXT_INDENT: put_len(&b, &s->inh->text_indent); break;
    case CP_LETTER_SPACING: snprintf(tmp, sizeof tmp, "%gpx", (double)s->inh->letter_spacing); buf_puts(&b, tmp); break;
    case CP_ORDER: snprintf(tmp, sizeof tmp, "%d", s->box->order); buf_puts(&b, tmp); break;
    case CP_FLEX_DIRECTION: {
        static const char *const fd[] = { "row", "row-reverse", "column", "column-reverse" };
        buf_puts(&b, fd[s->box->flex_dir & 3]);
        break;
    }
    case CP_FLEX_WRAP: buf_puts(&b, s->box->flex_wrap == 1 ? "wrap" : s->box->flex_wrap == 2 ? "wrap-reverse" : "nowrap"); break;
    case CP_ROW_GAP: put_len(&b, &s->box->row_gap); break;
    case CP_COLUMN_GAP: put_len(&b, &s->box->col_gap); break;
    case CP_BORDER_TOP_LEFT_RADIUS: case CP_BORDER_TOP_RIGHT_RADIUS: case CP_BORDER_BOTTOM_RIGHT_RADIUS:
    case CP_BORDER_BOTTOM_LEFT_RADIUS: put_len(&b, &s->vis->radius[p - CP_BORDER_TOP_LEFT_RADIUS]); break;
    case CP_OUTLINE_WIDTH: snprintf(tmp, sizeof tmp, "%gpx", (double)s->vis->outline_w); buf_puts(&b, tmp); break;
    case CP_BORDER_COLLAPSE: buf_puts(&b, s->inh->border_collapse ? "collapse" : "separate"); break;
    case CP_TABLE_LAYOUT: buf_puts(&b, s->box->table_layout ? "fixed" : "auto"); break;
    case CP_CLEAR: buf_puts(&b, s->box->clear == 1 ? "left" : s->box->clear == 2 ? "right" : s->box->clear == 3 ? "both" : "none"); break;
    case CP_FLEX_GROW: snprintf(tmp, sizeof tmp, "%g", (double)s->box->flex_grow); buf_puts(&b, tmp); break;
    case CP_FLEX_SHRINK: snprintf(tmp, sizeof tmp, "%g", (double)s->box->flex_shrink); buf_puts(&b, tmp); break;
    case CP_FLEX_BASIS: put_len(&b, &s->box->flex_basis); break;
    case CP_OVERFLOW_X: case CP_OVERFLOW_Y: {
        static const char *const ov[] = { "visible", "hidden", "scroll", "auto", "clip" };
        int o = p == CP_OVERFLOW_X ? s->box->overflow_x : s->box->overflow_y;
        buf_puts(&b, ov[o < 5 ? o : 0]);
        break;
    }
    default: buf_free(&b); return NULL;
    }
    char *r = web_raw_alloc(&d->budget, (size_t)b.n + 1);
    if (r && b.n) memcpy(r, b.p, b.n);
    buf_free(&b);
    return r;
}

char *web_element_computed(WebNode *el, const char *prop) {
    if (!is_el(el) || !el->u.el.style) return NULL;
    int p = css_property(prop, strlen(prop));
    if (p < 0) return NULL;
    return style_serialize(el->doc, el->u.el.style, p);
}

/* Interaction states change what :hover, :active, :focus and :focus-within match. The state flag of an element and its
 * ancestors (the "hover chain") changes from the old target up to the common ancestor and from there down to the new
 * one; restyling the topmost changed element covers both chains (and its parent too when rules use sibling
 * combinators). Nothing is restyled when no rule looks at these states. */
static void mark_state_change(WebDoc *d, WebNode *top) {
    CssEngine *e = d->css;
    if (!top || (e && e->index_valid && !e->has_state)) return;
    WebNode *t = top;
    if ((!e || e->has_sibling || e->has_has) && t->parent && t->parent->type == WEB_NODE_ELEMENT) t = t->parent;
    if (e && e->has_has) t = web_doc_element(d);              /* :has() can depend on any descendant */
    dom_mark_style_dirty(t);
}

static void change_chain(WebDoc *d, WebNode *old, WebNode *now, uint16_t flag) {
    for (WebNode *p = old; p && p->type == WEB_NODE_ELEMENT; p = p->parent) p->flags &= (uint16_t)~flag;
    for (WebNode *p = now; p && p->type == WEB_NODE_ELEMENT; p = p->parent) p->flags |= flag;
    /* the topmost element whose flag changed: the child of the common ancestor on either side */
    WebNode *top = NULL;
    for (WebNode *p = old; p && p->type == WEB_NODE_ELEMENT; p = p->parent)
        if (!now || !web_node_contains(p, now)) top = p;
    WebNode *top2 = NULL;
    for (WebNode *p = now; p && p->type == WEB_NODE_ELEMENT; p = p->parent)
        if (!old || !web_node_contains(p, old)) top2 = p;
    mark_state_change(d, top);
    mark_state_change(d, top2);
}

void web_doc_set_hover(WebDoc *d, WebNode *n) {
    while (n && n->type != WEB_NODE_ELEMENT) n = n->parent;
    if (d->hover == n) return;
    WebNode *old = d->hover;
    d->hover = n;
    change_chain(d, old, n, NF_HOVER);
}

void web_doc_set_active(WebDoc *d, WebNode *n) {
    while (n && n->type != WEB_NODE_ELEMENT) n = n->parent;
    if (d->active == n) return;
    WebNode *old = d->active;
    d->active = n;
    change_chain(d, old, n, NF_ACTIVE);
}

void web_doc_set_focus(WebDoc *d, WebNode *n) {
    while (n && n->type != WEB_NODE_ELEMENT) n = n->parent;
    if (d->focus == n) return;
    WebNode *old = d->focus;
    d->focus = n;
    change_chain(d, old, n, NF_FOCUS);                       /* :focus-within follows the same chains */
}

WebNode *web_doc_focus(WebDoc *d) { return d->focus; }

void web_doc_set_target(WebDoc *d, const char *frag) {
    WebNode *t = NULL;
    if (frag && *frag) {
        if (*frag == '#') frag++;
        t = web_doc_get_element_by_id(d, frag);
        if (!t) {
            WebAtom a = atom_find(&d->atoms, frag, strlen(frag));
            for (WebNode *n = d->root->first; n && a; n = dom_next(n, d->root))
                if (is_html(n, AT_a) && dom_attr_val(n, AT_name) && strcmp(dom_attr_val(n, AT_name), frag) == 0) { t = n; break; }
        }
    }
    if (d->target == t) return;
    if (d->target) dom_mark_style_dirty(d->target);
    d->target = t;
    if (t) dom_mark_style_dirty(t);
}
