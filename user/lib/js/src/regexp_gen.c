/*
 * regexp_gen.c: from the syntax tree to code for the backtracking matcher (regexp_exec.c), plus the analyses that
 * speed up searching (the set of characters a match can start with, the minimum length, a leading ^), the compile
 * cache and the flag parser.  Specification: ECMA-262 22.2.2 (Pattern Semantics): the code implements the
 * continuation-passing matchers of the specification with an explicit backtrack stack.
 *
 * How the specification's operations map to code (see regexp.h for the instruction list):
 *   Disjunction          SPLIT_GREEDY next_alternative; ...; JMP end
 *   Group                SAVE_START n; ...; SAVE_END n   (a lookbehind body runs right to left: the other way round)
 *   RepeatMatcher        a counted loop with registers (LOOP_INIT/ENTER/END); `*` and `?` without a counter; a run of
 *                        a one-character atom (the common case) as one REPEAT_ATOM instruction.  Every iteration
 *                        first makes the groups inside it undefined (RESET_CAPS, step 4 of RepeatMatcher) and an
 *                        iteration that matched the empty string once the minimum is reached fails (EMPTY_CHECK).
 *   Lookaround           LOOK_POS/LOOK_NEG ... LOOK_END_*: the body is atomic; captures it set survive a positive one.
 */
#include "regexp_ast.h"

#define RE_MAX_CODE 0x1000000u      /* words: 64 MiB of code is far beyond any real pattern */

typedef struct ReGen {
    ReParser *P;
    JSContext *ctx;
    uint32_t *code;
    uint32_t n, cap;
    uint32_t nregs;
    KzReSet *sets;
    uint32_t nsets, setcap;
    uint32_t *ranges;
    uint32_t nranges, rangecap;     /* ranges (pairs) used / capacity in pairs */
    bool failed;                    /* an exception is pending */
    bool toolarge;
} ReGen;

static void put(ReGen *G, uint32_t w) {
    if (G->failed) return;
    if (G->n >= RE_MAX_CODE) { G->failed = G->toolarge = true; return; }
    if (G->n == G->cap) {
        uint32_t nc = G->cap ? G->cap * 2 : 256;
        uint32_t *nw = kz_realloc(G->ctx, G->code, (size_t)nc * sizeof(uint32_t));
        if (!nw) { G->failed = true; return; }
        G->code = nw;
        G->cap = nc;
    }
    G->code[G->n++] = w;
}

static uint32_t here(const ReGen *G) { return G->n; }
static uint32_t placeholder(ReGen *G) { uint32_t at = G->n; put(G, 0); return at; }
static void patch(ReGen *G, uint32_t at, uint32_t target) { if (!G->failed) G->code[at] = target; }

static uint32_t alloc_regs(ReGen *G, uint32_t n) { uint32_t r = G->nregs; G->nregs += n; return r; }

/* copies a set into the code's tables; returns its index */
static uint32_t emit_set(ReGen *G, ReSet *s) {
    re_set_norm(s);
    if (G->failed) return 0;
    if (G->nsets == G->setcap) {
        uint32_t nc = G->setcap ? G->setcap * 2 : 8;
        KzReSet *ns = kz_realloc(G->ctx, G->sets, (size_t)nc * sizeof(KzReSet));
        if (!ns) { G->failed = true; return 0; }
        G->sets = ns;
        G->setcap = nc;
    }
    if (G->nranges + s->n > G->rangecap) {
        uint32_t nc = G->rangecap ? G->rangecap * 2 : 64;
        while (nc < G->nranges + s->n) nc *= 2;
        uint32_t *nr = kz_realloc(G->ctx, G->ranges, (size_t)nc * 2 * sizeof(uint32_t));
        if (!nr) { G->failed = true; return 0; }
        G->ranges = nr;
        G->rangecap = nc;
    }
    KzReSet *out = &G->sets[G->nsets];
    memset(out, 0, sizeof *out);
    out->nranges = s->n;
    out->off = G->nranges;
    for (uint32_t i = 0; i < s->n; i++) {
        G->ranges[2 * (G->nranges + i)] = s->r[i].lo;
        G->ranges[2 * (G->nranges + i) + 1] = s->r[i].hi;
        for (uint32_t c = s->r[i].lo; c <= s->r[i].hi && c < 256; c++) out->bitmap[c >> 5] |= 1u << (c & 31);
    }
    G->nranges += s->n;
    return G->nsets++;
}

static uint32_t atom_flags(const ReGen *G, const ReNode *n, bool back) {
    return (back ? RX_BACK : 0) | (G->P->unicode ? RX_UNI : 0) | ((n->flags & RN_ICASE) ? RX_ICASE : 0);
}

static void gen(ReGen *G, const ReNode *n, bool back);

static void gen_repeat(ReGen *G, const ReNode *n, bool back) {
    uint32_t min = n->a, max = n->b;
    bool lazy = (n->flags & RN_LAZY) != 0;
    const ReNode *body = n->child;
    if (max == 0) return;                       /* x{0}: matches the empty string; the groups inside stay undefined */
    if (min == 1 && max == 1) { gen(G, body, back); return; }
    if (!back && (body->type == N_CHAR || body->type == N_ANY || body->type == N_SET)) {
        put(G, RX_REPEAT_ATOM | (lazy ? RX_FLAG_A : 0));
        put(G, min);
        put(G, max);
        uint32_t next = placeholder(G);
        gen(G, body, false);
        patch(G, next, here(G));
        return;
    }
    bool nullable = body->min_len == 0;
    uint32_t first = n->c, ncap = n->d;
    if (min == 0 && max <= 1) {                 /* x? */
        put(G, lazy ? RX_SPLIT_LAZY : RX_SPLIT_GREEDY);
        uint32_t exit = placeholder(G);
        uint32_t reg = 0;
        if (nullable) { reg = alloc_regs(G, 1); put(G, RX_SET_REG); put(G, reg); }
        gen(G, body, back);
        if (nullable) { put(G, RX_EMPTY_CHECK); put(G, reg); }
        patch(G, exit, here(G));
        return;
    }
    if (min == 0 && max == RX_INF) {            /* x* */
        uint32_t head = here(G);
        put(G, lazy ? RX_SPLIT_LAZY : RX_SPLIT_GREEDY);
        uint32_t exit = placeholder(G);
        if (ncap) { put(G, RX_RESET_CAPS); put(G, first); put(G, ncap); }
        uint32_t reg = 0;
        if (nullable) { reg = alloc_regs(G, 1); put(G, RX_SET_REG); put(G, reg); }
        gen(G, body, back);
        if (nullable) { put(G, RX_EMPTY_CHECK); put(G, reg); }
        put(G, RX_JMP);
        put(G, head);
        patch(G, exit, here(G));
        return;
    }
    /* x{min,max} and x+ : a counter in register r, the start of the current iteration in r + 1 */
    uint32_t r = alloc_regs(G, 2);
    put(G, RX_LOOP_INIT);
    put(G, r);
    uint32_t head = here(G);
    put(G, RX_LOOP_ENTER | (lazy ? RX_FLAG_A : 0));
    put(G, r);
    put(G, min);
    put(G, max);
    uint32_t exit = placeholder(G);
    if (ncap) { put(G, RX_RESET_CAPS); put(G, first); put(G, ncap); }
    put(G, RX_SET_REG);
    put(G, r + 1);
    gen(G, body, back);
    put(G, RX_LOOP_END);
    put(G, r);
    put(G, min);
    put(G, head);
    patch(G, exit, here(G));
}

static void gen(ReGen *G, const ReNode *n, bool back) {
    if (G->failed) return;
    if (kz_check_stack(G->ctx)) { G->failed = true; return; }
    switch (n->type) {
    case N_EMPTY:
        return;
    case N_CHAR:
        put(G, RX_CHAR | atom_flags(G, n, back));
        put(G, (n->flags & RN_ICASE) ? n->b : n->a);
        return;
    case N_ANY:
        put(G, RX_ANY | atom_flags(G, n, back) | ((n->flags & RN_DOTALL) ? RX_FLAG_A : 0));
        return;
    case N_SET: {
        uint32_t idx = emit_set(G, n->set);
        put(G, RX_SET | atom_flags(G, n, back));
        put(G, idx);
        return;
    }
    case N_SEQ:
        if (back) for (uint32_t i = n->nkids; i-- > 0;) gen(G, n->kids[i], back);
        else for (uint32_t i = 0; i < n->nkids; i++) gen(G, n->kids[i], back);
        return;
    case N_ALT: {
        uint32_t *jumps = kz_malloc(G->ctx, (n->nkids ? n->nkids : 1) * sizeof(uint32_t));
        if (!jumps) { G->failed = true; return; }
        uint32_t nj = 0;
        for (uint32_t i = 0; i < n->nkids; i++) {
            uint32_t split = 0;
            bool last = i + 1 == n->nkids;
            if (!last) { put(G, RX_SPLIT_GREEDY); split = placeholder(G); }
            gen(G, n->kids[i], back);
            if (!last) {
                put(G, RX_JMP);
                jumps[nj++] = placeholder(G);
                patch(G, split, here(G));
            }
        }
        for (uint32_t i = 0; i < nj; i++) patch(G, jumps[i], here(G));
        kz_free(G->ctx->rt, jumps);
        return;
    }
    case N_GROUP:
        put(G, back ? RX_SAVE_END : RX_SAVE_START);
        put(G, n->a);
        gen(G, n->child, back);
        put(G, back ? RX_SAVE_START : RX_SAVE_END);
        put(G, n->a);
        return;
    case N_BACKREF:
        if (n->b) {
            put(G, RX_BACKREF_MULTI | atom_flags(G, n, back));
            put(G, n->b);
            for (uint32_t i = 0; i < n->b; i++) put(G, G->P->multi[n->c + i]);
        } else {
            put(G, RX_BACKREF | atom_flags(G, n, back));
            put(G, n->a);
        }
        return;
    case N_BOL:
        put(G, RX_BOL | ((n->flags & RN_MULTI) ? RX_FLAG_A : 0));
        return;
    case N_EOL:
        put(G, RX_EOL | ((n->flags & RN_MULTI) ? RX_FLAG_A : 0));
        return;
    case N_WORDB:
    case N_NWORDB:
        put(G, (n->type == N_WORDB ? RX_WORDB : RX_NWORDB) |
               (((n->flags & RN_ICASE) && G->P->unicode) ? (RX_ICASE | RX_UNI) : 0));
        return;
    case N_LOOK: {
        bool neg = (n->flags & RN_NEG) != 0;
        put(G, neg ? RX_LOOK_NEG : RX_LOOK_POS);
        uint32_t end = placeholder(G);
        gen(G, n->child, (n->flags & RN_BACK) != 0);
        put(G, neg ? RX_LOOK_END_NEG : RX_LOOK_END_POS);
        patch(G, end, here(G));
        return;
    }
    case N_REPEAT:
        gen_repeat(G, n, back);
        return;
    }
}

/* ================================================================== search analyses */

typedef struct FirstSet { uint8_t bm[32]; bool other, any; } FirstSet;

static void fs_set(FirstSet *f, uint32_t c) { f->bm[c >> 3] |= (uint8_t)(1u << (c & 7)); }

static uint32_t canon_for(const ReParser *P, uint32_t c) {
    return P->unicode ? kz_unicode_fold_simple(c) : kz_unicode_canonicalize_nonunicode(c);
}

static bool set_contains(const ReSet *s, uint32_t c) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < s->r[mid].lo) hi = mid;
        else if (c > s->r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

/* adds what n can start with to f; true when n can also match the empty string (what follows contributes too) */
static bool first_of(ReGen *G, const ReNode *n, FirstSet *f) {
    const ReParser *P = G->P;
    switch (n->type) {
    case N_CHAR:
        if (n->flags & RN_ICASE) {
            for (uint32_t c = 0; c < 256; c++) if (canon_for(P, c) == n->b) fs_set(f, c);
            f->other = true;
        } else if (n->a < 256) fs_set(f, n->a);
        else f->other = true;
        return false;
    case N_ANY:
        for (uint32_t c = 0; c < 256; c++) if ((n->flags & RN_DOTALL) || (c != 10 && c != 13)) fs_set(f, c);
        f->other = true;
        return false;
    case N_SET:
        for (uint32_t c = 0; c < 256; c++) {
            uint32_t k = (n->flags & RN_ICASE) ? canon_for(P, c) : c;
            if (set_contains(n->set, k)) fs_set(f, c);
        }
        if ((n->flags & RN_ICASE) || (n->set->n && n->set->r[n->set->n - 1].hi >= 256)) f->other = true;
        return false;
    case N_BACKREF:
        f->any = true;
        return true;
    case N_SEQ:
        for (uint32_t i = 0; i < n->nkids; i++)
            if (!first_of(G, n->kids[i], f)) return false;
        return true;
    case N_ALT: {
        bool empty = false;
        for (uint32_t i = 0; i < n->nkids; i++)
            if (first_of(G, n->kids[i], f)) empty = true;
        return empty;
    }
    case N_GROUP:
        return first_of(G, n->child, f);
    case N_REPEAT: {
        if (n->b == 0) return true;
        bool r = first_of(G, n->child, f);
        return n->a == 0 || r;
    }
    default:                    /* N_EMPTY, assertions, lookarounds: nothing consumed */
        return true;
    }
}

static bool starts_with_bol(const ReNode *n) {
    switch (n->type) {
    case N_BOL: return !(n->flags & RN_MULTI);
    case N_SEQ: return n->nkids > 0 && starts_with_bol(n->kids[0]);
    case N_GROUP: return starts_with_bol(n->child);
    case N_ALT:
        for (uint32_t i = 0; i < n->nkids; i++) if (!starts_with_bol(n->kids[i])) return false;
        return n->nkids > 0;
    case N_REPEAT: return n->a >= 1 && n->b > 0 && starts_with_bol(n->child);
    default: return false;
    }
}

/* ================================================================== the compiled object */

static void free_code(JSRuntime *rt, KzRegExpCode *c) {
    kz_free(rt, c->code);
    kz_free(rt, c->sets);
    kz_free(rt, c->ranges);
    if (c->names) {
        kz_free(rt, c->names[0].utf8);      /* names[0] holds the blob all the names point into */
        kz_free(rt, c->names);
    }
    kz_free(rt, c->src);
    kz_free(rt, c);
}

void kz_regexp_retain(KzRegExpCode *c) { c->refcount++; }

void kz_regexp_free(JSRuntime *rt, KzRegExpCode *c) {
    if (!c) return;
    if (--c->refcount == 0) free_code(rt, c);
}

void kz_regexp_cache_free(JSRuntime *rt) {
    for (int i = 0; i < 32; i++) {
        if (rt->re_cache[i]) kz_regexp_free(rt, rt->re_cache[i]);
        rt->re_cache[i] = NULL;
    }
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (!cap) return;
    size_t i = 0;
    for (; msg[i] && i + 1 < cap; i++) err[i] = msg[i];
    err[i] = 0;
}

int kz_regexp_parse_flags(JSString *flags, uint32_t *out) {
    uint32_t f = 0, n = kz_str_len(flags);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(flags, i), bit;
        switch (c) {
        case 'd': bit = KZ_RE_HASINDICES; break;
        case 'g': bit = KZ_RE_GLOBAL; break;
        case 'i': bit = KZ_RE_IGNORECASE; break;
        case 'm': bit = KZ_RE_MULTILINE; break;
        case 's': bit = KZ_RE_DOTALL; break;
        case 'u': bit = KZ_RE_UNICODE; break;
        case 'v': bit = KZ_RE_UNICODESETS; break;
        case 'y': bit = KZ_RE_STICKY; break;
        default: return -1;
        }
        if (f & bit) return -1;
        f |= bit;
    }
    if ((f & KZ_RE_UNICODE) && (f & KZ_RE_UNICODESETS)) return -1;
    *out = f;
    return 0;
}

/*
 * Compiles a pattern (a flat string).  NULL means failure: err[0] != 0 is a SyntaxError message, err[0] == 0 means an
 * exception (out of memory, stack) is pending.  The result has one reference, the caller's.
 */
KzRegExpCode *kz_regexp_compile(JSContext *ctx, JSString *pattern, uint32_t flags, char *err, size_t errcap) {
    JSRuntime *rt = ctx->rt;
    if (errcap) err[0] = 0;
    ReParser P;
    re_parser_init(&P, ctx, pattern, flags);
    ReGen G;
    memset(&G, 0, sizeof G);
    G.P = &P;
    G.ctx = ctx;
    KzRegExpCode *c = NULL;
    ReNode *root = re_parse(&P);
    if (!root) {
        if (P.err) set_err(err, errcap, P.err);
        goto done;
    }
    gen(&G, root, false);
    put(&G, RX_MATCH);
    if (G.failed) {
        if (G.toolarge) set_err(err, errcap, "Regular expression too large");
        goto done;
    }
    c = kz_mallocz(ctx, sizeof *c);
    if (!c) goto done;
    c->refcount = 1;
    c->flags = flags;
    c->ncaps = P.ngroups;
    c->nregs = G.nregs;
    c->code = G.code; c->ncode = G.n; G.code = NULL;
    c->sets = G.sets; c->nsets = G.nsets; G.sets = NULL;
    c->ranges = G.ranges; c->nranges_total = G.nranges; G.ranges = NULL;
    c->min_len = root->min_len;
    c->anchored = starts_with_bol(root);
    FirstSet fs;
    memset(&fs, 0, sizeof fs);
    bool empty = first_of(&G, root, &fs);
    c->first_any = empty || fs.any;
    c->first_other = fs.other;
    memcpy(c->first, fs.bm, sizeof c->first);
    /* the pattern, for the cache */
    uint32_t n = kz_str_len(pattern);
    c->srclen = n;
    c->src = kz_malloc(ctx, (n ? n : 1) * sizeof(uint16_t));
    if (!c->src) { free_code(rt, c); c = NULL; goto done; }
    uint32_t h = 2166136261u ^ flags;
    for (uint32_t i = 0; i < n; i++) {
        c->src[i] = (uint16_t)kz_str_at(pattern, i);
        h = (h ^ c->src[i]) * 16777619u;
    }
    c->hash = h;
    if (P.ngnames) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < P.ngnames; i++) total += P.gnames[i].len + 1;
        c->names = kz_mallocz(ctx, (size_t)(c->ncaps + 1) * sizeof(KzReName));
        char *blob = c->names ? kz_malloc(ctx, total) : NULL;
        if (!c->names || !blob) {
            if (blob) kz_free(rt, blob);
            kz_free(rt, c->names);
            c->names = NULL;
            free_code(rt, c);
            c = NULL;
            goto done;
        }
        c->names[0].utf8 = blob;
        uint32_t off = 0;
        for (uint32_t i = 0; i < P.ngnames; i++) {
            ReGroupName *g = &P.gnames[i];
            memcpy(blob + off, g->utf8, g->len);
            blob[off + g->len] = 0;
            c->names[g->group].utf8 = blob + off;
            c->names[g->group].len = g->len;
            off += g->len + 1;
        }
    }
done:
    kz_free(rt, G.code);
    kz_free(rt, G.sets);
    kz_free(rt, G.ranges);
    re_parser_free(&P);
    return c;
}

/* compile through the runtime's cache: a literal evaluated in a loop compiles once */
KzRegExpCode *kz_regexp_get(JSContext *ctx, JSString *pattern, uint32_t flags, char *err, size_t errcap) {
    JSRuntime *rt = ctx->rt;
    uint32_t n = kz_str_len(pattern);
    uint32_t h = 2166136261u ^ flags;
    for (uint32_t i = 0; i < n; i++) h = (h ^ (uint16_t)kz_str_at(pattern, i)) * 16777619u;
    KzRegExpCode **slot = &rt->re_cache[h & 31];
    KzRegExpCode *c = *slot;
    if (c && c->hash == h && c->flags == flags && c->srclen == n) {
        bool same = true;
        for (uint32_t i = 0; i < n; i++)
            if (c->src[i] != kz_str_at(pattern, i)) { same = false; break; }
        if (same) { c->refcount++; return c; }
    }
    c = kz_regexp_compile(ctx, pattern, flags, err, errcap);
    if (!c) return NULL;
    slot = &rt->re_cache[h & 31];       /* (the compile may have run a collection, but never touches the cache) */
    if (*slot) kz_regexp_free(rt, *slot);
    *slot = c;
    c->refcount++;
    return c;
}

/* Early errors for a regular expression literal: 0 valid, 1 a SyntaxError (message in err), -1 exception pending */
int kz_regexp_syntax_check(JSContext *ctx, JSString *pattern, JSString *flags, char *err, size_t errcap) {
    uint32_t f;
    if (kz_regexp_parse_flags(flags, &f)) {
        set_err(err, errcap, "Invalid regular expression flags");
        return 1;
    }
    char reason[96];
    KzRegExpCode *c = kz_regexp_get(ctx, pattern, f, reason, sizeof reason);
    if (c) { kz_regexp_free(ctx->rt, c); return 0; }
    if (!reason[0]) return -1;
    /* "Invalid regular expression: /pattern/flags: reason" */
    size_t w = 0;
    const char *head = "Invalid regular expression: /";
    for (; *head && w + 1 < errcap; head++) err[w++] = *head;
    if (errcap > w + 1) w += kz_str_to_utf8_buf(pattern, err + w, errcap - w > 300 ? 300 : errcap - w);
    if (w + 1 < errcap) err[w++] = '/';
    for (uint32_t i = 0; i < kz_str_len(flags) && w + 1 < errcap; i++) err[w++] = (char)kz_str_at(flags, i);
    if (w + 2 < errcap) { err[w++] = ':'; err[w++] = ' '; }
    for (const char *r = reason; *r && w + 1 < errcap; r++) err[w++] = *r;
    err[w] = 0;
    return 1;
}
