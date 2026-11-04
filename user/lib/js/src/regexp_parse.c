/*
 * regexp_parse.c: the parser of regular expression patterns (ECMA-262 22.2.1 "Patterns", with the web-compatible
 * extensions of Annex B.1.2 for patterns without the u and v flags, and the regular expression modifiers of
 * ES2025).  It builds the syntax tree regexp_gen.c turns into code.
 *
 * Source characters: a pattern is a string of UTF-16 code units.  With the u or v flag it is read as code points
 * (a surrogate pair is one character); without, every code unit is a character.
 *
 * Case folding: matching is by canonical form.  A character atom stores its canonical form; a set stores its members
 * together with the canonical forms of its members (the matcher canonicalises the input character and looks it up,
 * which is what CharacterSetMatcher's "there exists a member whose Canonicalize equals" comes to).  With the v flag
 * the sets are folded exactly (MaybeSimpleCaseFolding) because complements are taken at the set level.
 */
#include "regexp_ast.h"
#include <stdlib.h>

#define EOF_CH 0xFFFFFFFFu
#define MAXCP  0x10FFFFu

/* ================================================================== allocation */

static void *perr(ReParser *P, const char *msg) {
    if (!P->err && !P->failed) P->err = msg;
    return NULL;
}

/* a block that lives until the parser is freed */
static void *pmalloc(ReParser *P, size_t n) {
    void *p = kz_malloc(P->ctx, n ? n : 1);
    if (!p) { P->failed = true; return NULL; }
    if (P->nblocks == P->blockcap) {
        uint32_t nc = P->blockcap ? P->blockcap * 2 : 32;
        void **nb = kz_realloc(P->ctx, P->blocks, nc * sizeof(void *));
        if (!nb) { kz_free(P->ctx->rt, p); P->failed = true; return NULL; }
        P->blocks = nb;
        P->blockcap = nc;
    }
    P->blocks[P->nblocks++] = p;
    return p;
}

static ReNode *new_node(ReParser *P, int type) {
    ReNode *n = kz_mallocz(P->ctx, sizeof(ReNode));
    if (!n) { P->failed = true; return NULL; }
    n->type = (uint8_t)type;
    n->all = P->nodes;
    P->nodes = n;
    return n;
}

static int add_kid(ReParser *P, ReNode *parent, ReNode *kid) {
    if (parent->nkids == parent->kcap) {
        uint32_t nc = parent->kcap ? parent->kcap * 2 : 4;
        ReNode **nk = kz_realloc(P->ctx, parent->kids, nc * sizeof(ReNode *));
        if (!nk) { P->failed = true; return -1; }
        parent->kids = nk;
        parent->kcap = nc;
    }
    parent->kids[parent->nkids++] = kid;
    return 0;
}

static ReSet *new_set(ReParser *P) {
    ReSet *s = kz_mallocz(P->ctx, sizeof(ReSet));
    if (!s) { P->failed = true; return NULL; }
    if (P->nsets == P->setcap) {
        uint32_t nc = P->setcap ? P->setcap * 2 : 16;
        ReSet **ns = kz_realloc(P->ctx, P->sets, nc * sizeof(ReSet *));
        if (!ns) { kz_free(P->ctx->rt, s); P->failed = true; return NULL; }
        P->sets = ns;
        P->setcap = nc;
    }
    P->sets[P->nsets++] = s;
    return s;
}

static ReStrs *new_strs(ReParser *P) {
    ReStrs *s = kz_mallocz(P->ctx, sizeof(ReStrs));
    if (!s) { P->failed = true; return NULL; }
    if (P->nstrs == P->strscap) {
        uint32_t nc = P->strscap ? P->strscap * 2 : 8;
        ReStrs **ns = kz_realloc(P->ctx, P->strs, nc * sizeof(ReStrs *));
        if (!ns) { kz_free(P->ctx->rt, s); P->failed = true; return NULL; }
        P->strs = ns;
        P->strscap = nc;
    }
    P->strs[P->nstrs++] = s;
    return s;
}

void re_parser_init(ReParser *P, JSContext *ctx, JSString *pattern, uint32_t flags) {
    memset(P, 0, sizeof *P);
    P->ctx = ctx;
    P->wide = kz_str_wide(pattern);
    P->s8 = P->wide ? NULL : kz_str8(pattern);
    P->s16 = P->wide ? kz_str16(pattern) : NULL;
    P->len = kz_str_len(pattern);
    P->flags = flags;
    P->unicode = (flags & KZ_RE_EITHER_UNICODE) != 0;
    P->vmode = (flags & KZ_RE_UNICODESETS) != 0;
    P->cur = (uint8_t)(((flags & KZ_RE_IGNORECASE) ? RN_ICASE : 0) | ((flags & KZ_RE_MULTILINE) ? RN_MULTI : 0) |
                       ((flags & KZ_RE_DOTALL) ? RN_DOTALL : 0));
}

void re_parser_free(ReParser *P) {
    JSRuntime *rt = P->ctx->rt;
    for (ReNode *n = P->nodes; n;) {
        ReNode *next = n->all;
        kz_free(rt, n->kids);
        kz_free(rt, n);
        n = next;
    }
    for (uint32_t i = 0; i < P->nsets; i++) { kz_free(rt, P->sets[i]->r); kz_free(rt, P->sets[i]); }
    kz_free(rt, P->sets);
    for (uint32_t i = 0; i < P->nstrs; i++) { kz_free(rt, P->strs[i]->v); kz_free(rt, P->strs[i]); }
    kz_free(rt, P->strs);
    for (uint32_t i = 0; i < P->nblocks; i++) kz_free(rt, P->blocks[i]);
    kz_free(rt, P->blocks);
    kz_free(rt, P->gnames);
    kz_free(rt, P->refs);
    kz_free(rt, P->multi);
    kz_free(rt, P->path);
    memset(P, 0, sizeof *P);
}

/* ================================================================== character sets */

int re_set_push(ReParser *P, ReSet *s, uint32_t lo, uint32_t hi) {
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 8;
        ReRange *nr = kz_realloc(P->ctx, s->r, nc * sizeof(ReRange));
        if (!nr) { P->failed = true; return -1; }
        s->r = nr;
        s->cap = nc;
    }
    s->r[s->n].lo = lo;
    s->r[s->n].hi = hi;
    s->n++;
    return 0;
}

static int range_cmp(const void *a, const void *b) {
    const ReRange *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return x->hi < y->hi ? -1 : x->hi > y->hi ? 1 : 0;
}

/* sort and merge overlapping or touching ranges */
void re_set_norm(ReSet *s) {
    if (s->n < 2) return;
    bool sorted = true;
    for (uint32_t i = 1; i < s->n; i++)
        if (s->r[i].lo <= s->r[i - 1].hi + 1 || s->r[i].lo < s->r[i - 1].lo) { sorted = false; break; }
    if (sorted) return;
    qsort(s->r, s->n, sizeof(ReRange), range_cmp);
    uint32_t w = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        if (w > 0 && s->r[i].lo <= s->r[w - 1].hi + 1) {
            if (s->r[i].hi > s->r[w - 1].hi) s->r[w - 1].hi = s->r[i].hi;
        } else s->r[w++] = s->r[i];
    }
    s->n = w;
}

static int set_copy_into(ReParser *P, ReSet *dst, const ReSet *src) {
    for (uint32_t i = 0; i < src->n; i++)
        if (re_set_push(P, dst, src->r[i].lo, src->r[i].hi)) return -1;
    return 0;
}

/* dst = [0, max] minus src (src normalised, dst empty) */
static int set_complement(ReParser *P, ReSet *dst, const ReSet *src, uint32_t max) {
    uint32_t next = 0;
    for (uint32_t i = 0; i < src->n; i++) {
        if (src->r[i].lo > max) break;
        if (src->r[i].lo > next && re_set_push(P, dst, next, src->r[i].lo - 1)) return -1;
        next = src->r[i].hi + 1;
        if (src->r[i].hi >= max) return 0;
    }
    if (next <= max) return re_set_push(P, dst, next, max);
    return 0;
}

/* dst = a intersect b (both normalised, dst empty) */
static int set_intersect(ReParser *P, ReSet *dst, const ReSet *a, const ReSet *b) {
    uint32_t i = 0, j = 0;
    while (i < a->n && j < b->n) {
        uint32_t lo = a->r[i].lo > b->r[j].lo ? a->r[i].lo : b->r[j].lo;
        uint32_t hi = a->r[i].hi < b->r[j].hi ? a->r[i].hi : b->r[j].hi;
        if (lo <= hi && re_set_push(P, dst, lo, hi)) return -1;
        if (a->r[i].hi < b->r[j].hi) i++;
        else j++;
    }
    return 0;
}

/* dst = a minus b (both normalised, dst empty) */
static int set_subtract(ReParser *P, ReSet *dst, const ReSet *a, const ReSet *b) {
    uint32_t j = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        uint32_t lo = a->r[i].lo, hi = a->r[i].hi;
        while (j < b->n && b->r[j].hi < lo) j++;
        uint32_t k = j;
        bool dead = false;
        while (k < b->n && b->r[k].lo <= hi) {
            if (b->r[k].lo > lo && re_set_push(P, dst, lo, b->r[k].lo - 1)) return -1;
            if (b->r[k].hi >= hi) { dead = true; break; }
            lo = b->r[k].hi + 1;
            k++;
        }
        if (!dead && re_set_push(P, dst, lo, hi)) return -1;
    }
    return 0;
}

/* case folding of sets */
typedef struct ClosureCtx { ReParser *P; ReSet *s; } ClosureCtx;
static int closure_add(void *o, uint32_t c) { ClosureCtx *cc = o; return re_set_push(cc->P, cc->s, c, c); }
static int range_add(void *o, uint32_t lo, uint32_t hi) { ClosureCtx *cc = o; return re_set_push(cc->P, cc->s, lo, hi); }

/* s = s plus the canonical form of every member (the matcher canonicalises the input before looking it up) */
static int set_close_case(ReParser *P, ReSet *s) {
    re_set_norm(s);
    ClosureCtx cc = { P, s };
    uint32_t n0 = s->n;
    for (uint32_t i = 0; i < n0; i++) {
        uint32_t lo = s->r[i].lo, hi = s->r[i].hi;
        if (!P->unicode && lo > 0xFFFF) continue;
        if (!P->unicode && hi > 0xFFFF) hi = 0xFFFF;
        if (kz_unicode_case_closure(lo, hi, P->unicode, closure_add, &cc)) return -1;
    }
    re_set_norm(s);
    return 0;
}

/* the characters that are not their own canonical form, normalised */
static ReSet *case_domain(ReParser *P) {
    ReSet *f = new_set(P);
    if (!f) return NULL;
    ClosureCtx cc = { P, f };
    if (kz_unicode_case_domain(P->unicode, range_add, &cc)) return NULL;
    re_set_norm(f);
    return f;
}

/* MaybeSimpleCaseFolding for /v: dst = { scf(c) : c in src } (dst empty, src normalised) */
static int set_fold_exact(ReParser *P, ReSet *dst, const ReSet *src) {
    ReSet *f = case_domain(P);
    if (!f) return -1;
    if (set_subtract(P, dst, src, f)) return -1;
    ClosureCtx cc = { P, dst };
    for (uint32_t i = 0; i < src->n; i++)
        if (kz_unicode_case_closure(src->r[i].lo, src->r[i].hi, true, closure_add, &cc)) return -1;
    re_set_norm(dst);
    return 0;
}

/* CharacterComplement(rer, src): every character the pattern can see, minus src.  src normalised. */
static ReSet *complement_rer(ReParser *P, const ReSet *src, bool icase) {
    ReSet *t = new_set(P);
    if (!t) return NULL;
    if (set_complement(P, t, src, P->unicode ? MAXCP : 0xFFFF)) return NULL;
    if (P->vmode && icase) {
        ReSet *f = case_domain(P), *u = new_set(P);
        if (!f || !u) return NULL;
        if (set_subtract(P, u, t, f)) return NULL;
        return u;
    }
    return t;
}

/* ================================================================== reading the pattern */

static inline uint32_t src_at(const ReParser *P, uint32_t i) { return P->wide ? P->s16[i] : P->s8[i]; }

static uint32_t peek_at(const ReParser *P, uint32_t pos, uint32_t *n) {
    if (pos >= P->len) { *n = 0; return EOF_CH; }
    uint32_t c = src_at(P, pos);
    *n = 1;
    if (P->unicode && c >= 0xD800 && c <= 0xDBFF && pos + 1 < P->len) {
        uint32_t d = src_at(P, pos + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) { *n = 2; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
    }
    return c;
}
static uint32_t peek(const ReParser *P, uint32_t *n) { return peek_at(P, P->pos, n); }
static uint32_t cur(const ReParser *P) { uint32_t n; return peek_at(P, P->pos, &n); }
static uint32_t at_unit(const ReParser *P, uint32_t pos) { return pos < P->len ? src_at(P, pos) : EOF_CH; }

static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static bool in_str(uint32_t c, const char *s) { return c && c < 128 && strchr(s, (int)c) != NULL; }
static bool is_syntax_char(uint32_t c) { return in_str(c, "^$\\.*+?()[]{}|"); }
static bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }
static bool is_octal(uint32_t c) { return c >= '0' && c <= '7'; }
static bool is_ascii_letter(uint32_t c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }

/* the canonical form of c under the pattern's flags (Canonicalize, 22.2.2.7.3) */
static uint32_t canon_of(const ReParser *P, uint32_t c) {
    return P->unicode ? kz_unicode_fold_simple(c) : kz_unicode_canonicalize_nonunicode(c);
}

/* ================================================================== escapes */

/* After "\u": XXXX or (when unicode_rules) {X...}; surrogate pairs of escapes combine under unicode rules.
   1: *cp set, position after the escape; 0: not a valid escape (position unchanged). */
static int parse_u_escape(ReParser *P, bool unicode_rules, uint32_t *cp) {
    uint32_t save = P->pos;
    if (unicode_rules && cur(P) == '{') {
        P->pos++;
        uint32_t v = 0, nd = 0;
        int h;
        while (P->pos < P->len && (h = hexval(src_at(P, P->pos))) >= 0) {
            v = v * 16 + (uint32_t)h;
            if (v > MAXCP) { P->pos = save; return 0; }
            P->pos++;
            nd++;
        }
        if (nd == 0 || cur(P) != '}') { P->pos = save; return 0; }
        P->pos++;
        *cp = v;
        return 1;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = P->pos + (uint32_t)i < P->len ? hexval(src_at(P, P->pos + (uint32_t)i)) : -1;
        if (h < 0) { P->pos = save; return 0; }
        v = v * 16 + (uint32_t)h;
    }
    P->pos += 4;
    if (unicode_rules && v >= 0xD800 && v <= 0xDBFF && at_unit(P, P->pos) == '\\' && at_unit(P, P->pos + 1) == 'u') {
        uint32_t w = 0;
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            int h = P->pos + 2 + (uint32_t)i < P->len ? hexval(src_at(P, P->pos + 2 + (uint32_t)i)) : -1;
            if (h < 0) { ok = false; break; }
            w = w * 16 + (uint32_t)h;
        }
        if (ok && w >= 0xDC00 && w <= 0xDFFF) {
            v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
            P->pos += 6;
        }
    }
    *cp = v;
    return 1;
}

/*
 * CharacterEscape (and the Annex B legacy forms), with P->pos just after the backslash.  Returns 1 with *cp, or -1
 * with P->err.  For "\c" not followed by a control letter in a pattern without u/v, the backslash itself is the
 * character and nothing else is consumed.
 */
static int parse_char_escape(ReParser *P, bool in_class, uint32_t *cp) {
    uint32_t n, c = peek(P, &n);
    switch (c) {
    case 'f': *cp = 12; P->pos++; return 1;
    case 'n': *cp = 10; P->pos++; return 1;
    case 'r': *cp = 13; P->pos++; return 1;
    case 't': *cp = 9; P->pos++; return 1;
    case 'v': *cp = 11; P->pos++; return 1;
    case 'c': {
        uint32_t d = at_unit(P, P->pos + 1);
        if (d != EOF_CH && is_ascii_letter(d)) { *cp = d % 32; P->pos += 2; return 1; }
        if (!P->unicode && in_class && d != EOF_CH && (is_digit(d) || d == '_')) { *cp = d % 32; P->pos += 2; return 1; }
        if (P->unicode) { perr(P, "Invalid unicode escape"); return -1; }
        *cp = '\\';
        return 1;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        if (c == '0' && !is_digit(at_unit(P, P->pos + 1))) { *cp = 0; P->pos++; return 1; }
        if (P->unicode) { perr(P, c == '0' ? "Invalid decimal escape" : "Invalid escape"); return -1; }
        {
            uint32_t v = c - '0';
            P->pos++;
            if (is_octal(at_unit(P, P->pos))) {
                v = v * 8 + (at_unit(P, P->pos) - '0');
                P->pos++;
                if (c <= '3' && is_octal(at_unit(P, P->pos))) { v = v * 8 + (at_unit(P, P->pos) - '0'); P->pos++; }
            }
            *cp = v;
            return 1;
        }
    case '8': case '9':
        if (P->unicode) { perr(P, "Invalid escape"); return -1; }
        *cp = c;
        P->pos++;
        return 1;
    case 'x': {
        int h1 = P->pos + 1 < P->len ? hexval(src_at(P, P->pos + 1)) : -1;
        int h2 = P->pos + 2 < P->len ? hexval(src_at(P, P->pos + 2)) : -1;
        if (h1 >= 0 && h2 >= 0) { *cp = (uint32_t)(h1 * 16 + h2); P->pos += 3; return 1; }
        if (P->unicode) { perr(P, "Invalid escape"); return -1; }
        *cp = 'x';
        P->pos++;
        return 1;
    }
    case 'u':
        P->pos++;
        if (parse_u_escape(P, P->unicode, cp)) return 1;
        if (P->unicode) { perr(P, "Invalid Unicode escape"); return -1; }
        *cp = 'u';
        return 1;
    default:
        break;
    }
    if (c == EOF_CH) { perr(P, "\\ at end of pattern"); return -1; }
    if (P->unicode) {
        if (is_syntax_char(c) || c == '/' || (in_class && c == '-')) { *cp = c; P->pos += n; return 1; }
        perr(P, "Invalid escape");
        return -1;
    }
    if (c == 'k' && P->has_named) { perr(P, "Invalid named reference"); return -1; }
    *cp = c;
    P->pos += n;
    return 1;
}

/* \d \D \s \S \w \W into a fresh set (which is normalised and already complemented where needed) */
static ReSet *class_escape_set(ReParser *P, uint32_t ch) {
    ReSet *base = new_set(P);
    if (!base) return NULL;
    bool ok = true;
    switch (ch | 0x20) {
    case 'd':
        ok = !re_set_push(P, base, '0', '9');
        break;
    case 's':
        ok = !re_set_push(P, base, 9, 13) && !re_set_push(P, base, 32, 32) && !re_set_push(P, base, 0xA0, 0xA0) &&
             !re_set_push(P, base, 0x1680, 0x1680) && !re_set_push(P, base, 0x2000, 0x200A) &&
             !re_set_push(P, base, 0x2028, 0x2029) && !re_set_push(P, base, 0x202F, 0x202F) &&
             !re_set_push(P, base, 0x205F, 0x205F) && !re_set_push(P, base, 0x3000, 0x3000) &&
             !re_set_push(P, base, 0xFEFF, 0xFEFF);
        break;
    default:    /* w */
        ok = !re_set_push(P, base, '0', '9') && !re_set_push(P, base, 'A', 'Z') && !re_set_push(P, base, '_', '_') &&
             !re_set_push(P, base, 'a', 'z');
        /* WordCharacters adds the characters whose canonical form is a word character: with /iu, U+017F and U+212A */
        if (ok && P->unicode && (P->cur & RN_ICASE)) ok = !re_set_push(P, base, 0x17F, 0x17F) && !re_set_push(P, base, 0x212A, 0x212A);
        break;
    }
    if (!ok) return NULL;
    re_set_norm(base);
    if (ch >= 'A' && ch <= 'Z') return complement_rer(P, base, P->cur & RN_ICASE);
    return base;
}

/* after \p or \P: {Name} or {Name=Value} into a set (complemented for \P) */
static ReSet *property_set(ReParser *P, bool negate) {
    if (cur(P) != '{') return perr(P, "Invalid property name");
    P->pos++;
    char name[64], value[64];
    size_t nl = 0, vl = 0;
    bool has_value = false;
    for (;;) {
        uint32_t c = at_unit(P, P->pos);
        if (c == '}') { P->pos++; break; }
        if (c == '=' && !has_value) { has_value = true; P->pos++; continue; }
        if (c == EOF_CH || !(is_ascii_letter(c) || is_digit(c) || c == '_')) return perr(P, "Invalid property name");
        if ((has_value ? vl : nl) >= 63) return perr(P, "Invalid property name");
        if (has_value) value[vl++] = (char)c;
        else name[nl++] = (char)c;
        P->pos++;
    }
    if (nl == 0 || (has_value && vl == 0)) return perr(P, "Invalid property name");
    ReSet *s = new_set(P);
    if (!s) return NULL;
    ClosureCtx cc = { P, s };
    int rc = kz_unicode_property(name, nl, has_value ? value : NULL, vl, range_add, &cc);
    if (rc < 0) { P->failed = true; return NULL; }
    if (rc > 0) return perr(P, "Invalid property name");
    re_set_norm(s);
    bool icase = (P->cur & RN_ICASE) != 0;
    if (P->vmode && icase) {            /* MaybeSimpleCaseFolding */
        ReSet *f = new_set(P);
        if (!f || set_fold_exact(P, f, s)) { P->failed = true; return NULL; }
        s = f;
    }
    if (negate) return complement_rer(P, s, icase);
    return s;
}

/* ================================================================== group names */

/* after "(?<" or "\k<": the identifier up to '>', as UTF-8 */
static int parse_group_name(ReParser *P, char **out, uint32_t *outlen) {
    char buf[256];
    uint32_t n = 0;
    bool first = true;
    for (;;) {
        if (P->pos >= P->len) { perr(P, "Invalid capture group name"); return -1; }
        uint32_t c = src_at(P, P->pos);
        if (c == '>') { P->pos++; break; }
        if (c == '\\') {
            P->pos++;
            if (cur(P) != 'u') { perr(P, "Invalid Unicode escape"); return -1; }
            P->pos++;
            if (!parse_u_escape(P, true, &c)) { perr(P, "Invalid Unicode escape"); return -1; }
        } else if (c >= 0xD800 && c <= 0xDBFF && P->pos + 1 < P->len && src_at(P, P->pos + 1) >= 0xDC00 && src_at(P, P->pos + 1) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (src_at(P, P->pos + 1) - 0xDC00);
            P->pos += 2;
        } else P->pos++;
        bool ok = first ? (kz_unicode_is_id_start(c) || c == '$' || c == '_')
                        : (kz_unicode_is_id_continue(c) || c == '$' || c == 0x200C || c == 0x200D);
        if (!ok) { perr(P, "Invalid capture group name"); return -1; }
        first = false;
        if (n + 4 >= sizeof buf) { perr(P, "Invalid capture group name"); return -1; }
        n += (uint32_t)kz_utf8_encode(c, buf + n);
    }
    if (n == 0) { perr(P, "Invalid capture group name"); return -1; }
    char *s = pmalloc(P, n + 1);
    if (!s) return -1;
    memcpy(s, buf, n);
    s[n] = 0;
    *out = s;
    *outlen = n;
    return 0;
}

/* may two groups both take part in one match?  They cannot when some disjunction holds them in different alternatives */
static bool might_both_participate(const uint32_t *px, uint32_t nx, const uint32_t *py, uint32_t ny) {
    uint32_t n = nx < ny ? nx : ny;
    for (uint32_t i = 0; i < n; i += 2) {
        if (px[i] != py[i]) return true;
        if (px[i + 1] != py[i + 1]) return false;
    }
    return true;
}

static int define_group_name(ReParser *P, char *utf8, uint32_t len, uint32_t group) {
    for (uint32_t i = 0; i < P->ngnames; i++) {
        ReGroupName *g = &P->gnames[i];
        if (g->len == len && !memcmp(g->utf8, utf8, len) && might_both_participate(g->path, g->pathlen, P->path, P->pathlen)) {
            perr(P, "Duplicate capture group name");
            return -1;
        }
    }
    if (P->ngnames == P->gnamecap) {
        uint32_t nc = P->gnamecap ? P->gnamecap * 2 : 8;
        ReGroupName *ng = kz_realloc(P->ctx, P->gnames, nc * sizeof(ReGroupName));
        if (!ng) { P->failed = true; return -1; }
        P->gnames = ng;
        P->gnamecap = nc;
    }
    ReGroupName *g = &P->gnames[P->ngnames++];
    g->utf8 = utf8;
    g->len = len;
    g->group = group;
    g->pathlen = P->pathlen;
    g->path = pmalloc(P, (P->pathlen ? P->pathlen : 1) * sizeof(uint32_t));
    if (!g->path) return -1;
    if (P->pathlen) memcpy(g->path, P->path, P->pathlen * sizeof(uint32_t));
    return 0;
}

/* ================================================================== the grammar */

static ReNode *parse_disjunction(ReParser *P);

static ReNode *make_char(ReParser *P, uint32_t cp) {
    ReNode *n = new_node(P, N_CHAR);
    if (!n) return NULL;
    n->a = cp;
    n->b = cp;
    n->min_len = 1;
    /* a character that has no case variants is compared as it is */
    if ((P->cur & RN_ICASE) && (cp >= 128 || is_ascii_letter(cp))) {
        n->flags |= RN_ICASE;
        n->b = canon_of(P, cp);
    }
    return n;
}

static ReNode *make_set_node(ReParser *P, ReSet *s, bool fold) {
    ReNode *n = new_node(P, N_SET);
    if (!n) return NULL;
    n->set = s;
    n->min_len = 1;
    if (fold && (P->cur & RN_ICASE)) n->flags |= RN_ICASE;
    return n;
}

/* a set node for a set that still needs its case closure (non-v) */
static ReNode *finish_set(ReParser *P, ReSet *s) {
    re_set_norm(s);
    if ((P->cur & RN_ICASE) && !P->vmode && set_close_case(P, s)) return NULL;
    return make_set_node(P, s, true);
}

typedef struct ClassAtom { bool is_set; uint32_t cp; ReSet *set; } ClassAtom;

static int parse_class_atom(ReParser *P, ClassAtom *a) {
    uint32_t n, c = peek(P, &n);
    a->is_set = false;
    a->set = NULL;
    if (c == EOF_CH) { perr(P, "Unterminated character class"); return -1; }
    if (c != '\\') { a->cp = c; P->pos += n; return 0; }
    P->pos++;
    c = peek(P, &n);
    if (c == EOF_CH) { perr(P, "\\ at end of pattern"); return -1; }
    switch (c) {
    case 'b': a->cp = 8; P->pos++; return 0;
    case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
        P->pos++;
        a->is_set = true;
        a->set = class_escape_set(P, c);
        return a->set ? 0 : -1;
    case 'p': case 'P':
        if (P->unicode) {
            P->pos++;
            a->is_set = true;
            a->set = property_set(P, c == 'P');
            return a->set ? 0 : -1;
        }
        break;
    default:
        break;
    }
    return parse_char_escape(P, true, &a->cp) < 0 ? -1 : 0;
}

static int add_class_atom(ReParser *P, ReSet *s, const ClassAtom *a) {
    if (a->is_set) return set_copy_into(P, s, a->set);
    return re_set_push(P, s, a->cp, a->cp);
}

/* ---- the v flag: ClassSetExpression with nested classes, set operations and strings ---- */

typedef struct ReClass { ReSet *set; ReStrs *strs; bool may_strings; } ReClass;

static bool strs_has(const ReStrs *s, const uint32_t *cp, uint32_t len) {
    if (!s) return false;
    for (uint32_t i = 0; i < s->n; i++)
        if (s->v[i].len == len && (len == 0 || !memcmp(s->v[i].cp, cp, len * sizeof(uint32_t)))) return true;
    return false;
}

static int strs_add(ReParser *P, ReStrs *s, const uint32_t *cp, uint32_t len) {
    if (strs_has(s, cp, len)) return 0;
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 4;
        ReStr *nv = kz_realloc(P->ctx, s->v, nc * sizeof(ReStr));
        if (!nv) { P->failed = true; return -1; }
        s->v = nv;
        s->cap = nc;
    }
    uint32_t *copy = pmalloc(P, (len ? len : 1) * sizeof(uint32_t));
    if (!copy) return -1;
    if (len) memcpy(copy, cp, len * sizeof(uint32_t));
    s->v[s->n].cp = copy;
    s->v[s->n].len = len;
    s->n++;
    return 0;
}

static int class_init(ReParser *P, ReClass *c) {
    c->set = new_set(P);
    c->strs = new_strs(P);
    c->may_strings = false;
    return c->set && c->strs ? 0 : -1;
}

static int parse_class_contents_v(ReParser *P, ReClass *out);

/* a ClassSetCharacter at P->pos: 1 and *cp, 0 when the next thing is not one, -1 on a syntax error */
static int parse_class_set_character(ReParser *P, uint32_t *cp) {
    uint32_t n, c = peek(P, &n);
    if (c == EOF_CH) return 0;
    if (c == '\\') {
        uint32_t save = P->pos;
        P->pos++;
        uint32_t d = cur(P);
        if (d == 'b') { *cp = 8; P->pos++; return 1; }
        if (in_str(d, "&-!#%,:;<=>@`~")) { *cp = d; P->pos++; return 1; }
        if (d == 'd' || d == 'D' || d == 's' || d == 'S' || d == 'w' || d == 'W' || d == 'p' || d == 'P' || d == 'q') {
            P->pos = save;
            return 0;
        }
        return parse_char_escape(P, true, cp) < 0 ? -1 : 1;
    }
    if (in_str(c, "()[]{}/-|")) return 0;
    /* ClassSetReservedDoublePunctuator: && !! ## $$ %% ** ++ ,, .. :: ;; << == >> ?? @@ ^^ `` ~~ */
    if (in_str(c, "&!#$%*+,.:;<=>?@^`~") && at_unit(P, P->pos + 1) == c) {
        perr(P, "Invalid set operation in character class");
        return -1;
    }
    *cp = c;
    P->pos += n;
    return 1;
}

/* adds the characters lo..hi to s, folded (MaybeSimpleCaseFolding) under /vi */
static int class_add_cp(ReParser *P, ReSet *s, uint32_t lo, uint32_t hi) {
    if (!(P->cur & RN_ICASE)) return re_set_push(P, s, lo, hi);
    if (lo == hi) {
        uint32_t f = kz_unicode_fold_simple(lo);
        return re_set_push(P, s, f, f);
    }
    ReSet *tmp = new_set(P), *f = new_set(P);
    if (!tmp || !f || re_set_push(P, tmp, lo, hi)) return -1;
    if (set_fold_exact(P, f, tmp)) return -1;
    return set_copy_into(P, s, f);
}

/* ClassString: characters up to '|' or '}' */
static int parse_class_string_disjunction(ReParser *P, ReClass *out) {
    /* after "\q{" */
    uint32_t buf[256];
    uint32_t len = 0;
    for (;;) {
        uint32_t c = cur(P);
        if (c == '|' || c == '}') {
            if (len == 1) {
                if (re_set_push(P, out->set, buf[0], buf[0])) return -1;
            } else {
                if (len != 1) out->may_strings = true;
                if (strs_add(P, out->strs, buf, len)) return -1;
            }
            len = 0;
            P->pos++;
            if (c == '}') return 0;
            continue;
        }
        uint32_t cp;
        int rc = parse_class_set_character(P, &cp);
        if (rc < 0) return -1;
        if (rc == 0) { perr(P, "Invalid escape"); return -1; }
        if (len >= 256) { perr(P, "Regular expression too large"); return -1; }
        buf[len++] = (P->cur & RN_ICASE) ? kz_unicode_fold_simple(cp) : cp;
    }
}

static int class_union_into(ReParser *P, ReClass *dst, const ReClass *src) {
    if (set_copy_into(P, dst->set, src->set)) return -1;
    re_set_norm(dst->set);
    for (uint32_t i = 0; i < src->strs->n; i++)
        if (strs_add(P, dst->strs, src->strs->v[i].cp, src->strs->v[i].len)) return -1;
    if (src->may_strings) dst->may_strings = true;
    return 0;
}

/* one ClassSetOperand: nested class, \q{...}, class escape, or a character (then *is_char is set and *cp holds it) */
static int parse_class_set_operand(ReParser *P, ReClass *out, bool *is_char, uint32_t *cp) {
    *is_char = false;
    if (class_init(P, out)) return -1;
    uint32_t c = cur(P);
    if (c == EOF_CH) { perr(P, "Unterminated character class"); return -1; }
    if (c == '[') {
        P->pos++;
        return parse_class_contents_v(P, out);
    }
    if (c == '\\') {
        uint32_t d = at_unit(P, P->pos + 1);
        if (d == 'q') {
            if (at_unit(P, P->pos + 2) != '{') { perr(P, "Invalid escape"); return -1; }
            P->pos += 3;
            return parse_class_string_disjunction(P, out);
        }
        if (d == 'd' || d == 'D' || d == 's' || d == 'S' || d == 'w' || d == 'W') {
            P->pos += 2;
            ReSet *s = class_escape_set(P, d);
            if (!s) return -1;
            if (P->cur & RN_ICASE) {
                ReSet *f = new_set(P);
                if (!f || set_fold_exact(P, f, s)) return -1;
                s = f;
            }
            if (set_copy_into(P, out->set, s)) return -1;
            re_set_norm(out->set);
            return 0;
        }
        if (d == 'p' || d == 'P') {
            P->pos += 2;
            ReSet *s = property_set(P, d == 'P');
            if (!s) return -1;
            if (set_copy_into(P, out->set, s)) return -1;
            re_set_norm(out->set);
            return 0;
        }
    }
    int rc = parse_class_set_character(P, cp);
    if (rc < 0) return -1;
    if (rc == 0) { perr(P, c == ']' ? "Invalid character class" : "Invalid character in character class"); return -1; }
    *is_char = true;
    return 0;
}

/* after '[' : [^]? ClassContents ']' with the v flag.  The result is a class value (complemented for ^). */
static int parse_class_contents_v(ReParser *P, ReClass *out) {
    if (kz_check_stack(P->ctx)) { P->failed = true; return -1; }
    bool negate = false;
    if (cur(P) == '^') { negate = true; P->pos++; }
    ReClass acc;
    if (class_init(P, &acc)) return -1;
    bool icase = (P->cur & RN_ICASE) != 0;
    if (cur(P) == ']') P->pos++;
    else {
        ReClass op;
        bool is_char;
        uint32_t cp;
        if (parse_class_set_operand(P, &op, &is_char, &cp)) return -1;
        uint32_t c1 = cur(P), c2 = at_unit(P, P->pos + 1);
        if (c1 == '&' && c2 == '&') {
            if (is_char) { if (class_add_cp(P, op.set, cp, cp)) return -1; re_set_norm(op.set); }
            if (class_union_into(P, &acc, &op)) return -1;
            bool may = acc.may_strings;
            while (cur(P) == '&' && at_unit(P, P->pos + 1) == '&') {
                P->pos += 2;
                if (cur(P) == '&') { perr(P, "Invalid set operation in character class"); return -1; }
                ReClass rhs;
                if (parse_class_set_operand(P, &rhs, &is_char, &cp)) return -1;
                if (is_char) { if (class_add_cp(P, rhs.set, cp, cp)) return -1; re_set_norm(rhs.set); }
                ReClass res;
                if (class_init(P, &res)) return -1;
                if (set_intersect(P, res.set, acc.set, rhs.set)) return -1;
                for (uint32_t i = 0; i < acc.strs->n; i++)
                    if (strs_has(rhs.strs, acc.strs->v[i].cp, acc.strs->v[i].len) &&
                        strs_add(P, res.strs, acc.strs->v[i].cp, acc.strs->v[i].len)) return -1;
                may = may && rhs.may_strings;
                acc = res;
            }
            acc.may_strings = may;
            if (cur(P) != ']') { perr(P, "Invalid set operation in character class"); return -1; }
            P->pos++;
        } else if (c1 == '-' && c2 == '-') {
            if (is_char) { if (class_add_cp(P, op.set, cp, cp)) return -1; re_set_norm(op.set); }
            if (class_union_into(P, &acc, &op)) return -1;
            bool may = acc.may_strings;
            while (cur(P) == '-' && at_unit(P, P->pos + 1) == '-') {
                P->pos += 2;
                ReClass rhs;
                if (parse_class_set_operand(P, &rhs, &is_char, &cp)) return -1;
                if (is_char) { if (class_add_cp(P, rhs.set, cp, cp)) return -1; re_set_norm(rhs.set); }
                ReClass res;
                if (class_init(P, &res)) return -1;
                if (set_subtract(P, res.set, acc.set, rhs.set)) return -1;
                for (uint32_t i = 0; i < acc.strs->n; i++)
                    if (!strs_has(rhs.strs, acc.strs->v[i].cp, acc.strs->v[i].len) &&
                        strs_add(P, res.strs, acc.strs->v[i].cp, acc.strs->v[i].len)) return -1;
                acc = res;
            }
            acc.may_strings = may;
            if (cur(P) != ']') { perr(P, "Invalid set operation in character class"); return -1; }
            P->pos++;
        } else {
            /* a union of operands and ranges */
            for (;;) {
                if (is_char) {
                    uint32_t hi = cp;
                    if (cur(P) == '-' && at_unit(P, P->pos + 1) != '-') {       /* a range */
                        P->pos++;
                        int rc = parse_class_set_character(P, &hi);
                        if (rc < 0) return -1;
                        if (rc == 0) { perr(P, "Invalid character class"); return -1; }
                        if (cp > hi) { perr(P, "Range out of order in character class"); return -1; }
                    }
                    if (class_add_cp(P, acc.set, cp, hi)) return -1;
                } else if (class_union_into(P, &acc, &op)) return -1;
                re_set_norm(acc.set);
                uint32_t c = cur(P);
                if (c == ']') { P->pos++; break; }
                if ((c == '&' && at_unit(P, P->pos + 1) == '&') || (c == '-' && at_unit(P, P->pos + 1) == '-')) {
                    perr(P, "Invalid set operation in character class");
                    return -1;
                }
                if (parse_class_set_operand(P, &op, &is_char, &cp)) return -1;
            }
        }
    }
    re_set_norm(acc.set);
    if (negate) {
        if (acc.may_strings) { perr(P, "Negated character class may contain strings"); return -1; }
        ReSet *t = complement_rer(P, acc.set, icase);
        if (!t) return -1;
        acc.set = t;
        acc.may_strings = false;
    }
    *out = acc;
    return 0;
}

static ReNode *class_to_node(ReParser *P, const ReClass *c) {
    ReNode *setn = make_set_node(P, c->set, true);
    if (!setn) return NULL;
    if (c->strs->n == 0) return setn;
    /* strings, longest first; the single characters go where the one-character strings would; "" last */
    ReNode *alt = new_node(P, N_ALT);
    if (!alt) return NULL;
    uint32_t maxlen = 0;
    bool has_empty = false;
    for (uint32_t i = 0; i < c->strs->n; i++) {
        if (c->strs->v[i].len > maxlen) maxlen = c->strs->v[i].len;
        if (c->strs->v[i].len == 0) has_empty = true;
    }
    uint32_t minlen = 0xFFFFFFFF;
    for (uint32_t len = maxlen; len >= 2; len--) {
        for (uint32_t i = 0; i < c->strs->n; i++) {
            if (c->strs->v[i].len != len) continue;
            ReNode *seq = new_node(P, N_SEQ);
            if (!seq) return NULL;
            for (uint32_t k = 0; k < len; k++) {
                ReNode *ch = make_char(P, c->strs->v[i].cp[k]);
                if (!ch || add_kid(P, seq, ch)) return NULL;
                seq->min_len += ch->min_len;
            }
            if (add_kid(P, alt, seq)) return NULL;
            if (seq->min_len < minlen) minlen = seq->min_len;
        }
    }
    if (c->set->n) {
        if (add_kid(P, alt, setn)) return NULL;
        if (minlen > 1) minlen = 1;
    }
    if (has_empty) {
        ReNode *e = new_node(P, N_EMPTY);
        if (!e || add_kid(P, alt, e)) return NULL;
        minlen = 0;
    }
    alt->min_len = minlen == 0xFFFFFFFF ? 0 : minlen;
    return alt;
}

static ReNode *parse_class(ReParser *P) {
    /* after '[' */
    if (P->vmode) {
        ReClass c;
        if (parse_class_contents_v(P, &c)) return NULL;
        return class_to_node(P, &c);
    }
    bool invert = false;
    if (cur(P) == '^') { invert = true; P->pos++; }
    ReSet *s = new_set(P);
    if (!s) return NULL;
    for (;;) {
        uint32_t c = cur(P);
        if (c == EOF_CH) return perr(P, "Unterminated character class");
        if (c == ']') { P->pos++; break; }
        ClassAtom a;
        if (parse_class_atom(P, &a)) return NULL;
        if (cur(P) == '-' && P->pos + 1 < P->len && at_unit(P, P->pos + 1) != ']') {
            P->pos++;
            ClassAtom b;
            if (parse_class_atom(P, &b)) return NULL;
            if (a.is_set || b.is_set) {
                if (P->unicode) return perr(P, "Invalid character class");
                if (add_class_atom(P, s, &a) || re_set_push(P, s, '-', '-') || add_class_atom(P, s, &b)) return NULL;
            } else {
                if (a.cp > b.cp) return perr(P, "Range out of order in character class");
                if (re_set_push(P, s, a.cp, b.cp)) return NULL;
            }
        } else if (add_class_atom(P, s, &a)) return NULL;
    }
    re_set_norm(s);
    if ((P->cur & RN_ICASE) && set_close_case(P, s)) return NULL;
    if (invert) {
        ReSet *t = new_set(P);
        if (!t || set_complement(P, t, s, P->unicode ? MAXCP : 0xFFFF)) return NULL;
        s = t;
    }
    return make_set_node(P, s, true);
}

/* ---- terms ---- */

static bool braced_quantifier_at(const ReParser *P, uint32_t pos, uint32_t *min, uint32_t *max, uint32_t *end) {
    /* "{n}", "{n,}", "{n,m}" starting at the '{' */
    uint32_t i = pos + 1;
    if (!is_digit(at_unit(P, i))) return false;
    uint64_t a = 0, b = 0;
    while (is_digit(at_unit(P, i))) { if (a < 0x7FFFFFFE) a = a * 10 + (at_unit(P, i) - '0'); i++; }
    if (a > 0x7FFFFFFE) a = 0x7FFFFFFE;
    uint32_t hi;
    if (at_unit(P, i) == ',') {
        i++;
        if (at_unit(P, i) == '}') hi = RX_INF;
        else {
            if (!is_digit(at_unit(P, i))) return false;
            while (is_digit(at_unit(P, i))) { if (b < 0x7FFFFFFE) b = b * 10 + (at_unit(P, i) - '0'); i++; }
            if (b > 0x7FFFFFFE) b = 0x7FFFFFFE;
            hi = (uint32_t)b;
        }
    } else hi = (uint32_t)a;
    if (at_unit(P, i) != '}') return false;
    *min = (uint32_t)a;
    *max = hi;
    *end = i + 1;
    return true;
}

static uint32_t sat_add(uint32_t a, uint32_t b) { uint64_t s = (uint64_t)a + b; return s > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)s; }
static uint32_t sat_mul(uint32_t a, uint32_t b) { uint64_t s = (uint64_t)a * b; return s > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)s; }

static ReNode *parse_alternative(ReParser *P);

static ReNode *wrap_group(ReParser *P, ReNode *body, uint32_t index) {
    ReNode *g = new_node(P, N_GROUP);
    if (!g) return NULL;
    g->a = index;
    g->child = body;
    g->min_len = body->min_len;
    return g;
}

static ReNode *parse_paren(ReParser *P, bool *quantifiable) {
    /* at '(' */
    P->pos++;
    uint8_t saved_cur = P->cur;
    ReNode *body;
    if (cur(P) == '?') {
        uint32_t c2 = at_unit(P, P->pos + 1);
        bool look = false, neg = false, behind = false;
        char *name = NULL;
        uint32_t namelen = 0;
        if (c2 == ':') P->pos += 2;
        else if (c2 == '=' || c2 == '!') { look = true; neg = c2 == '!'; P->pos += 2; }
        else if (c2 == '<') {
            uint32_t c3 = at_unit(P, P->pos + 2);
            if (c3 == '=' || c3 == '!') { look = behind = true; neg = c3 == '!'; P->pos += 3; }
            else {
                P->pos += 2;
                if (parse_group_name(P, &name, &namelen)) return NULL;
            }
        } else if (c2 == 'i' || c2 == 'm' || c2 == 's' || c2 == '-') {
            /* (?ims-ims: ... ) */
            P->pos++;
            uint8_t add = 0, remove = 0;
            bool dash = false;
            for (;;) {
                uint32_t c = cur(P);
                uint8_t bit = c == 'i' ? RN_ICASE : c == 'm' ? RN_MULTI : c == 's' ? RN_DOTALL : 0;
                if (c == ':') { P->pos++; break; }
                if (c == '-' && !dash) { dash = true; P->pos++; continue; }
                if (!bit) return perr(P, "Invalid group");
                if ((add | remove) & bit) return perr(P, "Repeated flag in modifiers");
                if (dash) remove |= bit;
                else add |= bit;
                P->pos++;
            }
            if (!add && !remove) return perr(P, "Invalid group");
            P->cur = (uint8_t)((P->cur | add) & ~remove);
        } else return perr(P, "Invalid group");
        uint32_t index = 0;
        if (name) {
            index = ++P->ngroups;
            if (define_group_name(P, name, namelen, index)) return NULL;
        }
        body = parse_disjunction(P);
        if (!body) return NULL;
        if (cur(P) != ')') return perr(P, "Unterminated group");
        P->pos++;
        P->cur = saved_cur;
        if (look) {
            ReNode *l = new_node(P, N_LOOK);
            if (!l) return NULL;
            l->child = body;
            l->flags = (uint8_t)((neg ? RN_NEG : 0) | (behind ? RN_BACK : 0));
            *quantifiable = !behind && !P->unicode;         /* Annex B: a lookahead may be quantified outside u/v mode */
            return l;
        }
        if (name) return wrap_group(P, body, index);
        return body;
    }
    uint32_t index = ++P->ngroups;
    body = parse_disjunction(P);
    if (!body) return NULL;
    if (cur(P) != ')') return perr(P, "Unterminated group");
    P->pos++;
    return wrap_group(P, body, index);
}

static ReNode *parse_atom_escape(ReParser *P, bool *quantifiable) {
    /* after the backslash */
    uint32_t n, c = peek(P, &n);
    if (c == EOF_CH) return perr(P, "\\ at end of pattern");
    switch (c) {
    case 'b': case 'B': {
        P->pos++;
        ReNode *w = new_node(P, c == 'b' ? N_WORDB : N_NWORDB);
        if (!w) return NULL;
        if (P->cur & RN_ICASE) w->flags |= RN_ICASE;
        *quantifiable = false;
        return w;
    }
    case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
        P->pos++;
        ReSet *s = class_escape_set(P, c);
        if (!s) return NULL;
        if (P->vmode && (P->cur & RN_ICASE)) {      /* the escape's set is folded exactly under /vi */
            ReSet *f = new_set(P);
            if (!f || set_fold_exact(P, f, s)) { P->failed = true; return NULL; }
            s = f;
            return make_set_node(P, s, true);
        }
        return finish_set(P, s);
    }
    case 'p': case 'P':
        if (P->unicode) {
            P->pos++;
            ReSet *s = property_set(P, c == 'P');
            if (!s) return NULL;
            return P->vmode ? make_set_node(P, s, true) : finish_set(P, s);
        }
        break;
    case 'k':
        if (P->has_named || P->unicode) {
            P->pos++;
            if (cur(P) != '<') return perr(P, "Invalid named reference");
            P->pos++;
            char *name;
            uint32_t namelen;
            if (parse_group_name(P, &name, &namelen)) return NULL;
            ReNode *r = new_node(P, N_BACKREF);
            if (!r) return NULL;
            if (P->cur & RN_ICASE) r->flags |= RN_ICASE;
            if (P->nrefs == P->refcap) {
                uint32_t nc = P->refcap ? P->refcap * 2 : 8;
                ReRef *nr = kz_realloc(P->ctx, P->refs, nc * sizeof(ReRef));
                if (!nr) { P->failed = true; return NULL; }
                P->refs = nr;
                P->refcap = nc;
            }
            P->refs[P->nrefs].utf8 = name;
            P->refs[P->nrefs].len = namelen;
            P->refs[P->nrefs].node = r;
            P->nrefs++;
            return r;
        }
        break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
        uint32_t save = P->pos, value = 0;
        while (is_digit(at_unit(P, P->pos))) {
            if (value < 100000000) value = value * 10 + (at_unit(P, P->pos) - '0');
            P->pos++;
        }
        if (value <= P->total_groups) {
            ReNode *r = new_node(P, N_BACKREF);
            if (!r) return NULL;
            r->a = value;
            if (P->cur & RN_ICASE) r->flags |= RN_ICASE;
            return r;
        }
        if (P->unicode) return perr(P, "Invalid escape");
        P->pos = save;
        break;
    }
    default:
        break;
    }
    uint32_t cp;
    if (parse_char_escape(P, false, &cp) < 0) return NULL;
    return make_char(P, cp);
}

/* quantifier after an atom: 1 parsed, 0 none (a '{' that is only a character), -1 error */
static int parse_quantifier(ReParser *P, uint32_t *min, uint32_t *max, bool *lazy) {
    uint32_t c = cur(P);
    if (c == '*') { *min = 0; *max = RX_INF; P->pos++; }
    else if (c == '+') { *min = 1; *max = RX_INF; P->pos++; }
    else if (c == '?') { *min = 0; *max = 1; P->pos++; }
    else if (c == '{') {
        uint32_t end;
        if (!braced_quantifier_at(P, P->pos, min, max, &end)) {
            if (P->unicode) { perr(P, "Incomplete quantifier"); return -1; }
            return 0;
        }
        if (*max < *min) { perr(P, "numbers out of order in {} quantifier"); return -1; }
        P->pos = end;
    } else return 0;
    *lazy = false;
    if (cur(P) == '?') { *lazy = true; P->pos++; }
    return 1;
}

static ReNode *parse_term(ReParser *P) {
    if (kz_check_stack(P->ctx)) { P->failed = true; return NULL; }
    uint32_t n, c = peek(P, &n);
    uint32_t first_cap = P->ngroups + 1;
    ReNode *atom = NULL;
    bool quantifiable = true;
    switch (c) {
    case '^': case '$':
        P->pos++;
        atom = new_node(P, c == '^' ? N_BOL : N_EOL);
        if (atom) atom->flags = P->cur & RN_MULTI;
        quantifiable = false;
        break;
    case '(':
        atom = parse_paren(P, &quantifiable);
        break;
    case '.':
        P->pos++;
        atom = new_node(P, N_ANY);
        if (atom) { atom->flags = P->cur & RN_DOTALL; atom->min_len = 1; }
        break;
    case '[':
        P->pos++;
        atom = parse_class(P);
        break;
    case '\\':
        P->pos++;
        atom = parse_atom_escape(P, &quantifiable);
        break;
    case '*': case '+': case '?':
        return perr(P, "Nothing to repeat");
    case '{': {
        uint32_t mn, mx, end;
        if (P->unicode) return perr(P, "Nothing to repeat");
        if (braced_quantifier_at(P, P->pos, &mn, &mx, &end)) return perr(P, "Nothing to repeat");
        P->pos++;
        atom = make_char(P, '{');
        break;
    }
    case '}': case ']':
        if (P->unicode) return perr(P, "Lone quantifier brackets");
        P->pos++;
        atom = make_char(P, c);
        break;
    default:
        P->pos += n;
        atom = make_char(P, c);
        break;
    }
    if (!atom) return NULL;
    uint32_t min, max;
    bool lazy;
    int q = parse_quantifier(P, &min, &max, &lazy);
    if (q < 0) return NULL;
    if (q == 0) return atom;
    if (!quantifiable) return perr(P, "Nothing to repeat");
    ReNode *r = new_node(P, N_REPEAT);
    if (!r) return NULL;
    r->a = min;
    r->b = max;
    r->c = first_cap;
    r->d = P->ngroups + 1 - first_cap;
    r->child = atom;
    if (lazy) r->flags |= RN_LAZY;
    r->min_len = sat_mul(min, atom->min_len);
    return r;
}

static ReNode *parse_alternative(ReParser *P) {
    ReNode *seq = new_node(P, N_SEQ);
    if (!seq) return NULL;
    for (;;) {
        uint32_t c = cur(P);
        if (c == EOF_CH || c == '|' || c == ')') break;
        ReNode *t = parse_term(P);
        if (!t) return NULL;
        if (t->type == N_EMPTY) continue;
        if (add_kid(P, seq, t)) return NULL;
        seq->min_len = sat_add(seq->min_len, t->min_len);
    }
    if (seq->nkids == 0) {
        seq->type = N_EMPTY;
        return seq;
    }
    if (seq->nkids == 1) return seq->kids[0];
    return seq;
}

static ReNode *parse_disjunction(ReParser *P) {
    if (kz_check_stack(P->ctx)) { P->failed = true; return NULL; }
    uint32_t id = P->next_disj++;
    if (P->pathlen + 2 > P->pathcap) {
        uint32_t nc = P->pathcap ? P->pathcap * 2 : 32;
        uint32_t *np = kz_realloc(P->ctx, P->path, nc * sizeof(uint32_t));
        if (!np) { P->failed = true; return NULL; }
        P->path = np;
        P->pathcap = nc;
    }
    uint32_t slot = P->pathlen;
    P->path[P->pathlen++] = id;
    P->path[P->pathlen++] = 0;
    ReNode *first = parse_alternative(P);
    if (!first) return NULL;
    ReNode *result = first;
    if (cur(P) == '|') {
        ReNode *alt = new_node(P, N_ALT);
        if (!alt || add_kid(P, alt, first)) return NULL;
        alt->min_len = first->min_len;
        uint32_t k = 0;
        while (cur(P) == '|') {
            P->pos++;
            P->path[slot + 1] = ++k;
            ReNode *next = parse_alternative(P);
            if (!next || add_kid(P, alt, next)) return NULL;
            if (next->min_len < alt->min_len) alt->min_len = next->min_len;
        }
        result = alt;
    }
    P->pathlen = slot;
    return result;
}

static void prescan(ReParser *P) {
    uint32_t groups = 0;
    bool named = false;
    int depth = 0;
    for (uint32_t i = 0; i < P->len; i++) {
        uint32_t c = src_at(P, i);
        if (c == '\\') { i++; continue; }
        if (depth > 0) {
            if (c == ']') depth--;
            else if (c == '[' && P->vmode) depth++;
            continue;
        }
        if (c == '[') { depth = 1; continue; }
        if (c != '(') continue;
        if (at_unit(P, i + 1) == '?') {
            if (at_unit(P, i + 2) == '<') {
                uint32_t d = at_unit(P, i + 3);
                if (d != '=' && d != '!') { groups++; named = true; }
            }
        } else groups++;
    }
    P->total_groups = groups;
    P->has_named = named;
}

ReNode *re_parse(ReParser *P) {
    prescan(P);
    ReNode *root = parse_disjunction(P);
    if (!root) return NULL;
    if (P->pos < P->len) return perr(P, "Unmatched ')'");
    /* named references may point forward: resolve them now */
    for (uint32_t i = 0; i < P->nrefs; i++) {
        ReRef *r = &P->refs[i];
        uint32_t count = 0, first = 0;
        for (uint32_t k = 0; k < P->ngnames; k++)
            if (P->gnames[k].len == r->len && !memcmp(P->gnames[k].utf8, r->utf8, r->len)) {
                if (!count) first = P->gnames[k].group;
                count++;
            }
        if (!count) return perr(P, "Invalid named capture referenced");
        if (count == 1) { r->node->a = first; continue; }
        if (P->nmulti + count > P->multicap) {
            uint32_t nc = P->multicap ? P->multicap * 2 : 16;
            while (nc < P->nmulti + count) nc *= 2;
            uint32_t *nm = kz_realloc(P->ctx, P->multi, nc * sizeof(uint32_t));
            if (!nm) { P->failed = true; return NULL; }
            P->multi = nm;
            P->multicap = nc;
        }
        r->node->b = count;
        r->node->c = P->nmulti;
        for (uint32_t k = 0; k < P->ngnames; k++)
            if (P->gnames[k].len == r->len && !memcmp(P->gnames[k].utf8, r->utf8, r->len)) P->multi[P->nmulti++] = P->gnames[k].group;
    }
    return root;
}
