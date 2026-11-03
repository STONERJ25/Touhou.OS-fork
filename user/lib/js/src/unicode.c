/*
 * unicode.c: Unicode character data for the engine: identifier characters, white space, general categories, case
 * mapping and folding, the regular expression property escapes (\p{...}) and normalization (NFC, NFD, NFKC, NFKD).
 * The tables come from the Unicode Character Database through gen_unicode.py (see unicode_tables.h).
 */
#include "jsint.h"
#include "unicode_tables.h"

static bool in_pairs(const uint32_t *t, uint32_t n, uint32_t c) {
    /* t: inclusive ranges as start, end pairs; n = the number of uint32 values */
    uint32_t lo = 0, hi = n / 2;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < t[2 * mid]) hi = mid;
        else if (c > t[2 * mid + 1]) lo = mid + 1;
        else return true;
    }
    return false;
}

int kz_unicode_is_id_start(uint32_t c) { return in_pairs(kz_id_start, KZ_COUNTOF(kz_id_start), c); }
int kz_unicode_is_id_continue(uint32_t c) { return in_pairs(kz_id_continue, KZ_COUNTOF(kz_id_continue), c); }

int kz_unicode_is_space(uint32_t c) {
    /* WhiteSpace: TAB VT FF ZWNBSP and the Zs characters (a fixed set: Space_Separator is stable) */
    switch (c) {
    case 0x09: case 0x0B: case 0x0C: case 0x20: case 0xA0: case 0x1680: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return 1;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

int kz_unicode_category(uint32_t c) {
    uint32_t lo = 0, hi = KZ_COUNTOF(kz_gc_ranges);
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if ((kz_gc_ranges[mid] >> 5) <= c) lo = mid;
        else hi = mid;
    }
    return (int)(kz_gc_ranges[lo] & 31);
}

/* ================================================================== case mapping */

static uint32_t run_map(const KzCaseRun *t, uint32_t n, uint32_t c) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < t[mid].start) hi = mid;
        else lo = mid + 1;
    }
    if (lo == 0) return c;
    const KzCaseRun *r = &t[lo - 1];
    uint32_t off = c - r->start;
    if (off >= (uint32_t)r->len * r->step || off % r->step) return c;
    return (uint32_t)((int32_t)c + r->delta);
}

static const KzSpecialCase *special(const KzSpecialCase *t, uint32_t n, uint32_t c) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < t[mid].cp) hi = mid;
        else if (c > t[mid].cp) lo = mid + 1;
        else return &t[mid];
    }
    return NULL;
}

uint32_t kz_unicode_to_upper_simple(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    return run_map(kz_upper_runs, KZ_COUNTOF(kz_upper_runs), c);
}

uint32_t kz_unicode_to_lower_simple(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    return run_map(kz_lower_runs, KZ_COUNTOF(kz_lower_runs), c);
}

int kz_unicode_to_upper_full(uint32_t c, uint32_t out[3]) {
    if (c >= 0xDF) {
        const KzSpecialCase *s = special(kz_special_upper, KZ_COUNTOF(kz_special_upper), c);
        if (s) {
            for (uint32_t i = 0; i < s->n; i++) out[i] = s->m[i];
            return (int)s->n;
        }
    }
    out[0] = kz_unicode_to_upper_simple(c);
    return 1;
}

int kz_unicode_to_lower_full(uint32_t c, uint32_t out[3]) {
    if (c >= 0x130) {
        const KzSpecialCase *s = special(kz_special_lower, KZ_COUNTOF(kz_special_lower), c);
        if (s) {
            for (uint32_t i = 0; i < s->n; i++) out[i] = s->m[i];
            return (int)s->n;
        }
    }
    out[0] = kz_unicode_to_lower_simple(c);
    return 1;
}

uint32_t kz_unicode_fold_simple(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    return run_map(kz_fold_runs, KZ_COUNTOF(kz_fold_runs), c);
}

uint32_t kz_unicode_canonicalize_nonunicode(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    uint32_t u[3];
    if (kz_unicode_to_upper_full(c, u) != 1) return c;
    if (u[0] > 0xFFFF) return c;
    if (u[0] < 0x80) return c;
    return u[0];
}

/*
 * For a case-insensitive character class: calls add(canon(c)) for every c in [lo, hi] whose canonical form differs
 * from c (unicode: simple case folding; otherwise the toUppercase-based Canonicalize of non-unicode patterns).
 * Only the code points the mapping tables cover can differ, so only those are visited.
 */
int kz_unicode_case_closure(uint32_t lo, uint32_t hi, bool unicode, int (*add)(void *, uint32_t), void *opaque) {
    if (unicode) {
        for (size_t i = 0; i < KZ_COUNTOF(kz_fold_runs); i++) {
            const KzCaseRun *r = &kz_fold_runs[i];
            uint32_t end = r->start + (uint32_t)(r->len - 1) * r->step;
            if (end < lo || r->start > hi) continue;
            for (uint32_t c = r->start; c <= end; c += r->step)
                if (c >= lo && c <= hi && add(opaque, (uint32_t)((int32_t)c + r->delta))) return -1;
        }
        /* ASCII (outside the tables) */
        for (uint32_t c = lo < 'A' ? 'A' : lo; c <= hi && c <= 'Z'; c++)
            if (add(opaque, c + 32)) return -1;
        return 0;
    }
    for (uint32_t c = lo < 'a' ? 'a' : lo; c <= hi && c <= 'z'; c++)
        if (add(opaque, c - 32)) return -1;
    for (size_t i = 0; i < KZ_COUNTOF(kz_upper_runs); i++) {
        const KzCaseRun *r = &kz_upper_runs[i];
        uint32_t end = r->start + (uint32_t)(r->len - 1) * r->step;
        if (end < lo || r->start > hi) continue;
        for (uint32_t c = r->start; c <= end; c += r->step) {
            if (c < lo || c > hi || c > 0xFFFF) continue;
            uint32_t k = kz_unicode_canonicalize_nonunicode(c);
            if (k != c && add(opaque, k)) return -1;
        }
    }
    return 0;
}

/* ================================================================== property escapes */

static int name_cmp(const char *a, const char *b, size_t blen) {
    /* a: NUL terminated table name; b: blen bytes */
    size_t i = 0;
    for (; i < blen && a[i]; i++)
        if ((unsigned char)a[i] != (unsigned char)b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    if (i < blen) return -1;
    return a[i] ? 1 : 0;
}

static const KzUName *find_name(const KzUName *t, uint32_t n, const char *s, size_t len) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = name_cmp(t[mid].name, s, len);
        if (c == 0) return &t[mid];
        if (c > 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static int find_binary(const char *s, size_t len) {
    for (;;) {
        uint32_t lo = 0, hi = KZ_COUNTOF(kz_binary_props);
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int c = name_cmp(kz_binary_props[mid].name, s, len);
            if (c == 0) return (int)mid;
            if (c > 0) hi = mid;
            else lo = mid + 1;
        }
        /* an alias? */
        lo = 0;
        hi = KZ_COUNTOF(kz_binary_aliases);
        const char *target = NULL;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int c = name_cmp(kz_binary_aliases[mid].name, s, len);
            if (c == 0) { target = kz_binary_aliases[mid].target; break; }
            if (c > 0) hi = mid;
            else lo = mid + 1;
        }
        if (!target) return -1;
        s = target;
        len = strlen(target);
    }
}

static uint32_t rd_varint(const uint8_t **p) {
    uint32_t v = 0;
    int shift = 0;
    for (;;) {
        uint8_t b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
}

static int emit_stream(uint32_t off, uint32_t count, int (*add)(void *, uint32_t, uint32_t), void *opaque) {
    const uint8_t *p = kz_uprop_stream + off;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = pos + rd_varint(&p);
        uint32_t b = a + rd_varint(&p);
        if (add(opaque, a, b)) return -1;
        pos = b + 1;
    }
    return 0;
}

static int emit_gc(uint32_t mask, int (*add)(void *, uint32_t, uint32_t), void *opaque) {
    uint32_t n = KZ_COUNTOF(kz_gc_ranges);
    for (uint32_t i = 0; i < n; i++) {
        if (!(mask & (1u << (kz_gc_ranges[i] & 31)))) continue;
        uint32_t a = kz_gc_ranges[i] >> 5, b = i + 1 < n ? (kz_gc_ranges[i + 1] >> 5) - 1 : 0x10FFFF;
        /* merge following ranges of other wanted categories */
        while (i + 1 < n && (mask & (1u << (kz_gc_ranges[i + 1] & 31)))) {
            i++;
            b = i + 1 < n ? (kz_gc_ranges[i + 1] >> 5) - 1 : 0x10FFFF;
        }
        if (add(opaque, a, b)) return -1;
    }
    return 0;
}

/*
 * \p{name} or \p{name=value}: calls add(lo, hi) for each range of the set in ascending order.  Returns 0, 1 for an
 * unknown property or value (a SyntaxError for the caller), or -1 when add failed.
 */
int kz_unicode_property(const char *name, size_t nlen, const char *value, size_t vlen,
                        int (*add)(void *, uint32_t, uint32_t), void *opaque) {
    if (value) {
        if (!name_cmp("General_Category", name, nlen) || !name_cmp("gc", name, nlen)) {
            const KzUName *g = find_name(kz_gc_names, KZ_COUNTOF(kz_gc_names), value, vlen);
            if (!g) return 1;
            return emit_gc(g->value, add, opaque);
        }
        bool scx = !name_cmp("Script_Extensions", name, nlen) || !name_cmp("scx", name, nlen);
        if (scx || !name_cmp("Script", name, nlen) || !name_cmp("sc", name, nlen)) {
            const KzUName *s = find_name(kz_script_names, KZ_COUNTOF(kz_script_names), value, vlen);
            if (!s) return 1;
            const KzUScript *sc = &kz_scripts[s->value];
            return scx ? emit_stream(sc->scx_off, sc->scx_count, add, opaque) : emit_stream(sc->off, sc->count, add, opaque);
        }
        return 1;
    }
    const KzUName *g = find_name(kz_gc_names, KZ_COUNTOF(kz_gc_names), name, nlen);
    if (g) return emit_gc(g->value, add, opaque);
    int b = find_binary(name, nlen);
    if (b < 0) return 1;
    return emit_stream(kz_binary_props[b].off, kz_binary_props[b].count, add, opaque);
}

/* membership in a binary property by name (for the few the engine needs at run time, e.g. Cased) */
typedef struct { uint32_t c; bool found; } MemberQ;
static int member_add(void *opaque, uint32_t a, uint32_t b) {
    MemberQ *q = opaque;
    if (q->c >= a && q->c <= b) { q->found = true; return -1; }
    return q->c < a ? -1 : 0;   /* past it: stop */
}

bool kz_unicode_has_binary(const char *name, uint32_t c) {
    int b = find_binary(name, strlen(name));
    if (b < 0) return false;
    MemberQ q = { c, false };
    emit_stream(kz_binary_props[b].off, kz_binary_props[b].count, member_add, &q);
    return q.found;
}

/* ================================================================== normalization */

#define SBASE 0xAC00
#define LBASE 0x1100
#define VBASE 0x1161
#define TBASE 0x11A7
#define LCOUNT 19
#define VCOUNT 21
#define TCOUNT 28
#define NCOUNT (VCOUNT * TCOUNT)
#define SCOUNT (LCOUNT * NCOUNT)

static int ccc_of(uint32_t c) {
    if (c < 0x300) return 0;
    uint32_t lo = 0, hi = KZ_COUNTOF(kz_ccc_ranges);
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if ((kz_ccc_ranges[mid] >> 8) <= c) lo = mid;
        else hi = mid;
    }
    return (int)(kz_ccc_ranges[lo] & 0xFF);
}

/* the decomposition of c (one level): returns the count (0: none) */
static int decompose1(uint32_t c, bool compat, uint32_t *out) {
    /* single code point mappings */
    uint32_t lo = 0, hi = KZ_COUNTOF(kz_decomp_runs);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c < kz_decomp_runs[mid].start) hi = mid;
        else lo = mid + 1;
    }
    if (lo > 0) {
        const KzDecompRun *r = &kz_decomp_runs[lo - 1];
        if (c - r->start < r->len) {
            if (r->compat && !compat) return 0;
            out[0] = (uint32_t)((int32_t)c + r->delta);
            return 1;
        }
    }
    lo = 0;
    hi = KZ_COUNTOF(kz_decomp_multi);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const KzDecomp *d = &kz_decomp_multi[mid];
        if (c < d->cp) hi = mid;
        else if (c > d->cp) lo = mid + 1;
        else {
            if (d->compat && !compat) return 0;
            for (int i = 0; i < d->len; i++) out[i] = kz_decomp_pool[d->off + i];
            return d->len;
        }
    }
    return 0;
}

typedef struct UBuf { uint32_t *v; uint32_t n, cap; JSContext *ctx; bool failed; } UBuf;

static void ub_put(UBuf *b, uint32_t c) {
    if (b->failed) return;
    if (b->n == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 64;
        uint32_t *nv = kz_realloc(b->ctx, b->v, (size_t)nc * sizeof(uint32_t));
        if (!nv) { b->failed = true; return; }
        b->v = nv;
        b->cap = nc;
    }
    b->v[b->n++] = c;
}

static void decompose_full(UBuf *b, uint32_t c, bool compat, int depth) {
    if (c >= SBASE && c < SBASE + SCOUNT) {
        uint32_t s = c - SBASE;
        ub_put(b, LBASE + s / NCOUNT);
        ub_put(b, VBASE + (s % NCOUNT) / TCOUNT);
        if (s % TCOUNT) ub_put(b, TBASE + s % TCOUNT);
        return;
    }
    uint32_t tmp[18];
    int n = depth < 8 ? decompose1(c, compat, tmp) : 0;
    if (!n) { ub_put(b, c); return; }
    for (int i = 0; i < n; i++) decompose_full(b, tmp[i], compat, depth + 1);
}

static uint32_t compose_pair(uint32_t a, uint32_t b) {
    if (a >= LBASE && a < LBASE + LCOUNT && b >= VBASE && b < VBASE + VCOUNT)
        return SBASE + ((a - LBASE) * VCOUNT + (b - VBASE)) * TCOUNT;
    if (a >= SBASE && a < SBASE + SCOUNT && (a - SBASE) % TCOUNT == 0 && b > TBASE && b < TBASE + TCOUNT)
        return a + (b - TBASE);
    uint32_t lo = 0, hi = KZ_COUNTOF(kz_compose);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const KzCompose *p = &kz_compose[mid];
        if (a < p->first || (a == p->first && b < p->second)) hi = mid;
        else if (a > p->first || b > p->second) lo = mid + 1;
        else return p->composite;
    }
    return 0;
}

/* form: 0 NFC, 1 NFD, 2 NFKC, 3 NFKD.  *out is kz_malloc'd (the caller frees it). */
int kz_unicode_normalize(JSContext *ctx, const uint32_t *in, uint32_t n, int form, KzVec *unused, uint32_t **out, uint32_t *outn) {
    (void)unused;
    bool compat = form >= 2, comp = form == 0 || form == 2;
    UBuf b = { NULL, 0, 0, ctx, false };
    for (uint32_t i = 0; i < n; i++) decompose_full(&b, in[i], compat, 0);
    if (b.failed) { kz_free(ctx->rt, b.v); return -1; }
    /* canonical ordering: a stable insertion sort of each run of non-starters by combining class */
    for (uint32_t i = 1; i < b.n; i++) {
        int cc = ccc_of(b.v[i]);
        if (cc == 0) continue;
        uint32_t x = b.v[i], j = i;
        while (j > 0) {
            int pc = ccc_of(b.v[j - 1]);
            if (pc <= cc) break;
            b.v[j] = b.v[j - 1];
            j--;
        }
        b.v[j] = x;
    }
    if (comp && b.n > 1) {
        /* canonical composition (UAX #15): each character combines with the last starter unless blocked */
        /* last_cc: the class of the last character kept after the starter (0: the starter itself is the last) */
        uint32_t w = 0;
        int32_t starter = -1;
        int last_cc = 256;
        for (uint32_t i = 0; i < b.n; i++) {
            uint32_t c = b.v[i];
            int cc = ccc_of(c);
            if (starter >= 0 && (last_cc < cc || last_cc == 0)) {
                uint32_t p = compose_pair(b.v[starter], c);
                if (p) {
                    b.v[starter] = p;
                    continue;
                }
            }
            if (cc == 0) { starter = (int32_t)w; last_cc = 0; }
            else last_cc = cc;
            b.v[w++] = c;
        }
        b.n = w;
    }
    *out = b.v;
    *outn = b.n;
    return 0;
}
