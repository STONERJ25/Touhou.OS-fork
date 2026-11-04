/*
 * string.c: string values (ECMA-262 6.1.4: sequences of UTF-16 code units), ropes, the atom table, property keys,
 * the string builder and UTF-8 conversion.
 *
 * Representation (jsint.h): a flat string keeps its units inline, 8 bits each unless some unit is above 0xFF (then
 * 16 bits, STR_WIDE; the invariant "wide iff some unit > 0xFF" makes equality and hashing width-independent).
 * Concatenations of more than ROPE_MIN units make a rope node; the first operation that needs the characters
 * flattens it iteratively into a buffer the node then owns (STR_EXT), so a string built by a million `s += x` costs
 * one copy, not a million.
 */
#include "jsint.h"

#define ROPE_MIN 32

/* ------------------------------------------------------------------ allocation */

JSString *kz_str_alloc(JSContext *ctx, uint32_t len, bool wide) {
    if (len > KZ_STRING_MAX) { kz_throw_range(ctx, "Invalid string length"); return NULL; }
    size_t data = (size_t)len << (wide ? 1 : 0);
    size_t size = KZ_STR_HDR + data;
    if (size < sizeof(JSString)) size = sizeof(JSString);
    JSString *s = kz_gc_new(ctx, KZ_GC_STRING, size);
    if (!s) return NULL;
    s->h.aux = len;
    s->h.flags = wide ? STR_WIDE : 0;
    return s;
}

JSValue kz_str_from_latin1(JSContext *ctx, const uint8_t *p, uint32_t len) {
    if (len == 0) return ctx->rt->atom[KZ_ATOM_empty];
    if (len == 1 && ctx->rt->char_str[p[0]]) return ctx->rt->char_str[p[0]];
    JSString *s = kz_str_alloc(ctx, len, false);
    if (!s) return JS_EXCEPTION;
    memcpy(s->u.c8, p, len);
    return JV_MKSTR(s);
}

JSValue kz_str_from_utf16(JSContext *ctx, const uint16_t *p, uint32_t len) {
    bool wide = false;
    for (uint32_t i = 0; i < len; i++)
        if (p[i] > 0xFF) { wide = true; break; }
    if (!wide) {
        if (len == 0) return ctx->rt->atom[KZ_ATOM_empty];
        if (len == 1) return ctx->rt->char_str[p[0]];
        JSString *s = kz_str_alloc(ctx, len, false);
        if (!s) return JS_EXCEPTION;
        for (uint32_t i = 0; i < len; i++) s->u.c8[i] = (uint8_t)p[i];
        return JV_MKSTR(s);
    }
    JSString *s = kz_str_alloc(ctx, len, true);
    if (!s) return JS_EXCEPTION;
    memcpy(s->u.c16, p, (size_t)len * 2);
    return JV_MKSTR(s);
}

/* One code point from WTF-8 (UTF-8 that may encode lone surrogates).  Malformed bytes give U+FFFD and advance one. */
uint32_t kz_utf8_decode(const uint8_t *s, size_t len, size_t *pos) {
    size_t i = *pos;
    uint32_t c = s[i];
    if (c < 0x80) { *pos = i + 1; return c; }
    int n;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; min = 0x10000; }
    else { *pos = i + 1; return 0xFFFD; }
    for (int k = 1; k <= n; k++) {
        if (i + k >= len || (s[i + k] & 0xC0) != 0x80) { *pos = i + 1; return 0xFFFD; }
        c = (c << 6) | (s[i + k] & 0x3F);
    }
    if (c < min || c > 0x10FFFF) { *pos = i + 1; return 0xFFFD; }
    *pos = i + n + 1;
    return c;
}

int kz_utf8_encode(uint32_t c, char *out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

JSValue kz_str_from_utf8(JSContext *ctx, const char *src, size_t len) {
    const uint8_t *s = (const uint8_t *)src;
    size_t units = 0;
    bool wide = false, ascii = true;
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x80) { i++; units++; continue; }
        ascii = false;
        uint32_t c = kz_utf8_decode(s, len, &i);
        units += c > 0xFFFF ? 2 : 1;
        if (c > 0xFF) wide = true;
    }
    if (ascii) {
        if (len > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
        return kz_str_from_latin1(ctx, s, (uint32_t)len);
    }
    if (units > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
    if (units == 1 && !wide) {
        size_t i = 0;
        return ctx->rt->char_str[kz_utf8_decode(s, len, &i)];
    }
    JSString *str = kz_str_alloc(ctx, (uint32_t)units, wide);
    if (!str) return JS_EXCEPTION;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = kz_utf8_decode(s, len, &i);
        if (!wide) str->u.c8[o++] = (uint8_t)c;
        else if (c > 0xFFFF) {
            c -= 0x10000;
            str->u.c16[o++] = (uint16_t)(0xD800 + (c >> 10));
            str->u.c16[o++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else str->u.c16[o++] = (uint16_t)c;
    }
    return JV_MKSTR(str);
}

JSValue kz_str_from_cstr(JSContext *ctx, const char *s) { return kz_str_from_utf8(ctx, s, strlen(s)); }

JSValue kz_str_from_char(JSContext *ctx, uint32_t c) {
    if (c < 256) return ctx->rt->char_str[c];
    uint16_t u[2];
    if (c > 0xFFFF) {
        c -= 0x10000;
        u[0] = (uint16_t)(0xD800 + (c >> 10));
        u[1] = (uint16_t)(0xDC00 + (c & 0x3FF));
        return kz_str_from_utf16(ctx, u, 2);
    }
    u[0] = (uint16_t)c;
    return kz_str_from_utf16(ctx, u, 1);
}

/* ------------------------------------------------------------------ ropes */

static void copy_flat(const JSString *s, void *dst, bool wide, uint32_t pos) {
    uint32_t n = kz_str_len(s);
    if (wide) {
        uint16_t *d = (uint16_t *)dst + pos;
        if (kz_str_wide(s)) memcpy(d, kz_str16(s), (size_t)n * 2);
        else {
            const uint8_t *p = kz_str8(s);
            for (uint32_t i = 0; i < n; i++) d[i] = p[i];
        }
    } else memcpy((uint8_t *)dst + pos, kz_str8(s), n);
}

/* Fills dst (len units) from the rope's leaves, right to left, with an explicit stack. */
static bool rope_fill(JSRuntime *rt, JSString *root, void *dst, bool wide) {
    JSString *small[64];
    JSString **stack = small;
    size_t sp = 0, cap = 64;
    uint32_t pos = kz_str_len(root);
    stack[sp++] = root;
    bool ok = true;
    while (sp) {
        JSString *n = stack[--sp];
        if (n->h.flags & STR_ROPE) {
            if (sp + 2 > cap) {
                size_t nc = cap * 2;
                JSString **ns = rt->sys_malloc(rt->sys_opaque, nc * sizeof(JSString *));
                if (!ns) { ok = false; break; }
                memcpy(ns, stack, sp * sizeof(JSString *));
                if (stack != small) rt->sys_free(rt->sys_opaque, stack);
                stack = ns;
                cap = nc;
            }
            stack[sp++] = n->u.rope.left;
            stack[sp++] = n->u.rope.right;
        } else {
            pos -= kz_str_len(n);
            copy_flat(n, dst, wide, pos);
        }
    }
    if (stack != small) rt->sys_free(rt->sys_opaque, stack);
    return ok;
}

JSString *kz_str_flat(JSContext *ctx, JSString *s) {
    if (!(s->h.flags & STR_ROPE)) return s;
    bool wide = kz_str_wide(s);
    uint32_t len = kz_str_len(s);
    void *buf = kz_malloc(ctx, (size_t)len << (wide ? 1 : 0));
    if (!buf) return NULL;
    if (!rope_fill(ctx->rt, s, buf, wide)) {
        kz_free(ctx->rt, buf);
        kz_throw_oom(ctx);
        return NULL;
    }
    s->h.flags = (uint16_t)((s->h.flags & ~STR_ROPE) | STR_EXT);
    s->u.ext = buf;
    s->depth = 0;
    return s;
}

JSValue kz_str_flatten(JSContext *ctx, JSValue v) {
    return kz_str_flat(ctx, JV_STR(v)) ? v : JS_EXCEPTION;
}

JSValue kz_str_concat(JSContext *ctx, JSValue a, JSValue b) {
    JSString *sa = JV_STR(a), *sb = JV_STR(b);
    uint32_t la = kz_str_len(sa), lb = kz_str_len(sb);
    if (!la) return b;
    if (!lb) return a;
    if ((uint64_t)la + lb > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
    bool wide = kz_str_wide(sa) || kz_str_wide(sb);
    uint32_t len = la + lb;
    if (len < ROPE_MIN && !(sa->h.flags & STR_ROPE) && !(sb->h.flags & STR_ROPE)) {
        JSString *s = kz_str_alloc(ctx, len, wide);
        if (!s) return JS_EXCEPTION;
        sa = JV_STR(a);
        sb = JV_STR(b);
        copy_flat(sa, s->u.c8, wide, 0);
        copy_flat(sb, s->u.c8, wide, la);
        return JV_MKSTR(s);
    }
    JSString *r = kz_gc_new(ctx, KZ_GC_STRING, sizeof(JSString));
    if (!r) return JS_EXCEPTION;
    r->h.aux = len;
    r->h.flags = (uint16_t)(STR_ROPE | (wide ? STR_WIDE : 0));
    r->u.rope.left = JV_STR(a);
    r->u.rope.right = JV_STR(b);
    uint32_t da = (sa->h.flags & STR_ROPE) ? sa->depth : 0, db = (sb->h.flags & STR_ROPE) ? sb->depth : 0;
    r->depth = (da > db ? da : db) + 1;
    return JV_MKSTR(r);
}

JSValue kz_str_sub(JSContext *ctx, JSValue v, uint32_t start, uint32_t end) {
    JSString *s = kz_str_flat(ctx, JV_STR(v));
    if (!s) return JS_EXCEPTION;
    uint32_t len = kz_str_len(s);
    if (end > len) end = len;
    if (start >= end) return ctx->rt->atom[KZ_ATOM_empty];
    if (start == 0 && end == len) return v;
    uint32_t n = end - start;
    if (!kz_str_wide(s)) {
        if (n == 1) return ctx->rt->char_str[kz_str8(s)[start]];
        JSString *r = kz_str_alloc(ctx, n, false);
        if (!r) return JS_EXCEPTION;
        memcpy(r->u.c8, kz_str8(JV_STR(v)) + start, n);
        return JV_MKSTR(r);
    }
    /* copy out first: the allocation may collect, and the source stays alive through v */
    const uint16_t *p = kz_str16(s) + start;
    bool wide = false;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] > 0xFF) { wide = true; break; }
    if (!wide && n == 1) return ctx->rt->char_str[p[0]];
    JSString *r = kz_str_alloc(ctx, n, wide);
    if (!r) return JS_EXCEPTION;
    p = kz_str16(JV_STR(v)) + start;
    if (wide) memcpy(r->u.c16, p, (size_t)n * 2);
    else for (uint32_t i = 0; i < n; i++) r->u.c8[i] = (uint8_t)p[i];
    return JV_MKSTR(r);
}

/* ------------------------------------------------------------------ comparison and search (flat strings) */

int kz_str_cmp(JSString *a, JSString *b) {
    uint32_t la = kz_str_len(a), lb = kz_str_len(b), n = la < lb ? la : lb;
    if (!kz_str_wide(a) && !kz_str_wide(b)) {
        int r = memcmp(kz_str8(a), kz_str8(b), n);
        if (r) return r < 0 ? -1 : 1;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ca = kz_str_at(a, i), cb = kz_str_at(b, i);
            if (ca != cb) return ca < cb ? -1 : 1;
        }
    }
    return la < lb ? -1 : la > lb ? 1 : 0;
}

bool kz_str_eq(JSString *a, JSString *b) {
    if (a == b) return true;
    uint32_t n = kz_str_len(a);
    if (n != kz_str_len(b) || kz_str_wide(a) != kz_str_wide(b)) return false;
    if ((a->h.flags & STR_ATOM) && (b->h.flags & STR_ATOM)) return false;
    if ((a->h.flags & STR_HASHED) && (b->h.flags & STR_HASHED) && a->hash != b->hash) return false;
    if (kz_str_wide(a)) return memcmp(kz_str16(a), kz_str16(b), (size_t)n * 2) == 0;
    return memcmp(kz_str8(a), kz_str8(b), n) == 0;
}

bool kz_str_eq_ascii(JSString *a, const char *s) {
    size_t n = strlen(s);
    if (kz_str_len(a) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (kz_str_at(a, (uint32_t)i) != (uint8_t)s[i]) return false;
    return true;
}

uint32_t kz_str_hash(JSString *s) {
    if (s->h.flags & STR_HASHED) return s->hash;
    uint32_t h = 2166136261u, n = kz_str_len(s);
    if (kz_str_wide(s)) {
        const uint16_t *p = kz_str16(s);
        for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    } else {
        const uint8_t *p = kz_str8(s);
        for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    }
    s->hash = h;
    s->h.flags |= STR_HASHED;
    return h;
}

int32_t kz_str_index_of(JSString *s, JSString *pat, uint32_t from) {
    uint32_t n = kz_str_len(s), m = kz_str_len(pat);
    if (from > n) return m == 0 ? (int32_t)n : -1;
    if (m == 0) return (int32_t)from;
    if (m > n) return -1;
    uint32_t first = kz_str_at(pat, 0);
    if (!kz_str_wide(s) && !kz_str_wide(pat)) {
        const uint8_t *a = kz_str8(s), *b = kz_str8(pat);
        for (uint32_t i = from; i + m <= n;) {
            const uint8_t *hit = memchr(a + i, (int)first, n - m + 1 - i);
            if (!hit) return -1;
            i = (uint32_t)(hit - a);
            if (memcmp(a + i, b, m) == 0) return (int32_t)i;
            i++;
        }
        return -1;
    }
    for (uint32_t i = from; i + m <= n; i++) {
        if (kz_str_at(s, i) != first) continue;
        uint32_t k = 1;
        while (k < m && kz_str_at(s, i + k) == kz_str_at(pat, k)) k++;
        if (k == m) return (int32_t)i;
    }
    return -1;
}

int32_t kz_str_last_index_of(JSString *s, JSString *pat, int32_t from) {
    int32_t n = (int32_t)kz_str_len(s), m = (int32_t)kz_str_len(pat);
    if (m > n) return -1;
    if (from > n - m) from = n - m;
    for (int32_t i = from; i >= 0; i--) {
        int32_t k = 0;
        while (k < m && kz_str_at(s, (uint32_t)(i + k)) == kz_str_at(pat, (uint32_t)k)) k++;
        if (k == m) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ UTF-8 out */

static size_t utf8_units(JSString *s, uint32_t *i, char *out, bool replace) {
    uint32_t n = kz_str_len(s);
    uint32_t c = kz_str_at(s, *i);
    (*i)++;
    if (c >= 0xD800 && c <= 0xDBFF && *i < n) {
        uint32_t d = kz_str_at(s, *i);
        if (d >= 0xDC00 && d <= 0xDFFF) {
            (*i)++;
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        }
    }
    if (replace && c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
    return (size_t)kz_utf8_encode(c, out);
}

static char *to_utf8(JSContext *ctx, JSValue v, size_t *plen, bool replace) {
    JSString *s = kz_str_flat(ctx, JV_STR(v));
    if (!s) return NULL;
    uint32_t n = kz_str_len(s);
    size_t total = 0;
    char tmp[4];
    if (!kz_str_wide(s)) {
        const uint8_t *p = kz_str8(s);
        for (uint32_t i = 0; i < n; i++) total += p[i] < 0x80 ? 1 : 2;
    } else {
        for (uint32_t i = 0; i < n;) total += utf8_units(s, &i, tmp, replace);
    }
    char *out = kz_malloc(ctx, total + 1);
    if (!out) return NULL;
    s = JV_STR(v);
    size_t o = 0;
    for (uint32_t i = 0; i < n;) {
        if (!kz_str_wide(s)) {
            uint32_t c = kz_str8(s)[i++];
            o += (size_t)kz_utf8_encode(c, out + o);
        } else o += utf8_units(s, &i, out + o, replace);
    }
    out[o] = 0;
    if (plen) *plen = o;
    return out;
}

char *kz_str_to_utf8(JSContext *ctx, JSValue v, size_t *plen) { return to_utf8(ctx, v, plen, false); }
char *kz_str_to_utf8_replace(JSContext *ctx, JSValue v, size_t *plen);
char *kz_str_to_utf8_replace(JSContext *ctx, JSValue v, size_t *plen) { return to_utf8(ctx, v, plen, true); }

size_t kz_str_to_utf8_buf(JSString *s, char *buf, size_t cap) {
    size_t o = 0;
    uint32_t n = kz_str_len(s);
    char tmp[4];
    for (uint32_t i = 0; i < n;) {
        size_t k = utf8_units(s, &i, tmp, true);
        if (o + k + 1 > cap) break;
        memcpy(buf + o, tmp, k);
        o += k;
    }
    if (cap) buf[o < cap ? o : cap - 1] = 0;
    return o;
}

/* ------------------------------------------------------------------ the atom table (weak) */

#define ATOM_DELETED ((JSString *)(uintptr_t)1)

static bool atom_rehash(JSRuntime *rt, uint32_t ncap) {
    JSString **t = rt->sys_malloc(rt->sys_opaque, ncap * sizeof(JSString *));
    if (!t) return false;
    memset(t, 0, ncap * sizeof(JSString *));
    uint32_t count = 0;
    for (uint32_t i = 0; i < rt->atom_cap; i++) {
        JSString *s = rt->atom_table[i];
        if (!s || s == ATOM_DELETED) continue;
        uint32_t j = s->hash & (ncap - 1);
        while (t[j]) j = (j + 1) & (ncap - 1);
        t[j] = s;
        count++;
    }
    if (rt->atom_table) rt->sys_free(rt->sys_opaque, rt->atom_table);
    rt->atom_table = t;
    rt->atom_cap = ncap;
    rt->atom_count = count;
    return true;
}

static JSString *atom_lookup(JSRuntime *rt, JSString *s, uint32_t h) {
    if (!rt->atom_cap) return NULL;
    uint32_t m = rt->atom_cap - 1;
    for (uint32_t j = h & m;; j = (j + 1) & m) {
        JSString *a = rt->atom_table[j];
        if (!a) return NULL;
        if (a != ATOM_DELETED && a->hash == h && kz_str_eq(a, s)) return a;
    }
}

static bool atom_insert(JSRuntime *rt, JSString *s) {
    /* atom_used counts deleted entries too: probing slows down with them */
    if ((rt->atom_count + 1) * 4 > rt->atom_cap * 3 || rt->atom_used * 4 > rt->atom_cap * 3) {
        if (!atom_rehash(rt, rt->atom_cap ? (rt->atom_count * 4 > rt->atom_cap ? rt->atom_cap * 2 : rt->atom_cap) : 1024))
            return false;
        rt->atom_used = rt->atom_count;
    }
    uint32_t m = rt->atom_cap - 1, j = s->hash & m;
    while (rt->atom_table[j] && rt->atom_table[j] != ATOM_DELETED) j = (j + 1) & m;
    if (!rt->atom_table[j]) rt->atom_used++;
    rt->atom_table[j] = s;
    rt->atom_count++;
    s->h.flags |= STR_ATOM;
    return true;
}

bool kz_is_marked(JSRuntime *rt, const void *cell);

void kz_atoms_sweep(JSRuntime *rt) {
    for (uint32_t i = 0; i < rt->atom_cap; i++) {
        JSString *a = rt->atom_table[i];
        if (!a || a == ATOM_DELETED) continue;
        if (!kz_is_marked(rt, a) && !(a->h.flags & STR_PERM)) {
            rt->atom_table[i] = ATOM_DELETED;
            rt->atom_count--;
        }
    }
}

JSValue kz_atom_find(JSContext *ctx, JSValue v) {
    JSString *s = JV_STR(v);
    if (s->h.flags & STR_ATOM) return v;
    if (s->h.flags & STR_ROPE) return JS_NOTFOUND;
    JSString *a = atom_lookup(ctx->rt, s, kz_str_hash(s));
    return a ? JV_MKSTR(a) : JS_NOTFOUND;
}

JSValue kz_atom_from_str(JSContext *ctx, JSValue v) {
    JSString *s = JV_STR(v);
    if (s->h.flags & STR_ATOM) return v;
    s = kz_str_flat(ctx, s);
    if (!s) return JS_EXCEPTION;
    uint32_t h = kz_str_hash(s);
    JSString *a = atom_lookup(ctx->rt, s, h);
    if (a) return JV_MKSTR(a);
    if (!atom_insert(ctx->rt, s)) return kz_throw_oom(ctx);
    return v;
}

JSValue kz_atom_len(JSContext *ctx, const char *p, size_t len) {
    JSValue s = kz_str_from_utf8(ctx, p, len);
    if (s == JS_EXCEPTION) return s;
    KZ_ROOT(ctx, r);
    r = s;
    return kz_atom_from_str(ctx, r);
}

JSValue kz_atom(JSContext *ctx, const char *s) { return kz_atom_len(ctx, s, strlen(s)); }

bool kz_atoms_init(JSRuntime *rt) {
    JSContext tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.rt = rt;
    static const char *const names[] = {
#define DEF(id, str) str,
#include "atoms.h"
#undef DEF
    };
    /* one-unit strings first (the atom for "" comes from the list) */
    for (int c = 0; c < 256; c++) {
        JSString *s = kz_str_alloc(&tmp, 1, false);
        if (!s) return false;
        s->u.c8[0] = (uint8_t)c;
        s->h.flags |= STR_PERM;
        kz_str_hash(s);
        if (!atom_insert(rt, s)) return false;
        rt->char_str[c] = JV_MKSTR(s);
    }
    for (int i = 0; i < KZ_ATOM_COUNT; i++) {
        const char *n = names[i];
        size_t len = strlen(n);
        if (len == 1) { rt->atom[i] = rt->char_str[(uint8_t)n[0]]; continue; }
        JSString *s = kz_str_alloc(&tmp, (uint32_t)len, false);
        if (!s) return false;
        memcpy(s->u.c8, n, len);
        s->h.flags |= STR_PERM;
        uint32_t h = kz_str_hash(s);
        JSString *a = atom_lookup(rt, s, h);
        if (a) { rt->atom[i] = JV_MKSTR(a); continue; }
        if (!atom_insert(rt, s)) return false;
        rt->atom[i] = JV_MKSTR(s);
    }
    return true;
}

/* ------------------------------------------------------------------ property keys */

bool kz_str_is_index(JSString *s, uint32_t *idx) {
    if (s->h.flags & STR_NOTIDX) return false;
    uint32_t n = kz_str_len(s);
    if (n == 0 || n > 10) goto no;
    uint32_t c = kz_str_at(s, 0);
    if (c < '0' || c > '9' || (c == '0' && n > 1)) goto no;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        c = kz_str_at(s, i);
        if (c < '0' || c > '9') goto no;
        v = v * 10 + (c - '0');
    }
    if (v > KZ_MAX_INDEX) goto no;
    *idx = (uint32_t)v;
    return true;
no:
    s->h.flags |= STR_NOTIDX;
    return false;
}

JSValue kz_int_to_string(JSContext *ctx, int64_t v) {
    char buf[24];
    int n = 0;
    bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)-(v + 1) + 1 : (uint64_t)v;
    char tmp[24];
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int o = 0;
    if (neg) buf[o++] = '-';
    while (n) buf[o++] = tmp[--n];
    return kz_str_from_latin1(ctx, (const uint8_t *)buf, (uint32_t)o);
}

/* ToPropertyKey for primitives (objects go through kz_to_property_key in conv.c first). */
JSValue kz_key_from_value(JSContext *ctx, JSValue v) {
    if (JV_IS_INT(v)) {
        int32_t i = JV_INT(v);
        if (i >= 0) return KZ_KEY_FROM_INDEX((uint32_t)i);
        JSValue s = kz_int_to_string(ctx, i);
        if (s == JS_EXCEPTION) return s;
        return kz_atom_from_str(ctx, s);
    }
    if (JV_IS_STRING(v)) {
        JSString *s = JV_STR(v);
        uint32_t idx;
        if (s->h.flags & STR_ATOM) {
            /* an atom is the canonical key for its text, except when the text spells an array index: the
               one-character atoms "0".."9" (the string table's own) and the atoms made for function names
               ("12") are index strings too. STR_NOTIDX makes the common case one flag test. */
            if (kz_str_is_index(s, &idx)) return KZ_KEY_FROM_INDEX(idx);
            return v;
        }
        s = kz_str_flat(ctx, s);
        if (!s) return JS_EXCEPTION;
        if (kz_str_is_index(s, &idx)) return KZ_KEY_FROM_INDEX(idx);
        return kz_atom_from_str(ctx, v);
    }
    if (JV_IS_DOUBLE(v)) {
        double d = jv_dbl(v);
        if (d >= 0 && d <= (double)KZ_MAX_INDEX && d == (double)(uint32_t)d) return KZ_KEY_FROM_INDEX((uint32_t)d);
        JSValue s = kz_num_to_str(ctx, d);
        if (s == JS_EXCEPTION) return s;
        return kz_atom_from_str(ctx, s);
    }
    if (JV_IS_SYMBOL(v)) return v;
    switch (v) {
    case JS_UNDEFINED: return ctx->rt->atom[KZ_ATOM_undefined];
    case JS_NULL: return ctx->rt->atom[KZ_ATOM_null];
    case JS_TRUE: return ctx->rt->atom[KZ_ATOM_true];
    case JS_FALSE: return ctx->rt->atom[KZ_ATOM_false];
    default: break;
    }
    if (JV_IS_BIGINT(v)) {
        JSValue s = kz_bigint_to_string(ctx, v, 10);
        if (s == JS_EXCEPTION) return s;
        return kz_key_from_value(ctx, s);
    }
    return kz_throw_type(ctx, "invalid property key");
}

JSValue kz_key_to_value(JSContext *ctx, JSValue key) {
    if (KZ_KEY_IS_INDEX(key)) return kz_int_to_string(ctx, (int64_t)KZ_KEY_INDEX(key));
    return key;
}

JSValue kz_key_debug_name(JSContext *ctx, JSValue key) {
    if (KZ_KEY_IS_INDEX(key)) return kz_int_to_string(ctx, (int64_t)KZ_KEY_INDEX(key));
    if (JV_IS_SYMBOL(key)) {
        JSValue d = JV_SYM(key)->description;
        KzStrBuf sb;
        kz_sb_init(&sb, ctx);
        kz_sb_putc(&sb, '[');
        if (JV_IS_STRING(d)) kz_sb_put_value(&sb, d);
        kz_sb_putc(&sb, ']');
        return kz_sb_finish(&sb);
    }
    return key;
}

/* ------------------------------------------------------------------ the string builder */

void kz_sb_init(KzStrBuf *sb, JSContext *ctx) {
    sb->ctx = ctx;
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    sb->wide = false;
    sb->failed = false;
}

void kz_sb_free(KzStrBuf *sb) {
    if (sb->buf) kz_free(sb->ctx->rt, sb->buf);
    sb->buf = NULL;
}

static int sb_fail(KzStrBuf *sb, bool range) {
    if (!sb->failed) {
        sb->failed = true;
        if (range) kz_throw_range(sb->ctx, "Invalid string length");
    }
    return -1;
}

static int sb_reserve(KzStrBuf *sb, uint32_t extra) {
    if (sb->failed) return -1;
    uint64_t need = (uint64_t)sb->len + extra;
    if (need <= sb->cap) return 0;
    if (need > KZ_STRING_MAX) return sb_fail(sb, true);
    uint64_t nc = sb->cap ? (uint64_t)sb->cap * 2 : 32;
    if (nc < need) nc = need;
    if (nc > KZ_STRING_MAX) nc = KZ_STRING_MAX;
    void *nb = kz_realloc(sb->ctx, sb->buf, (size_t)nc << (sb->wide ? 1 : 0));
    if (!nb) return sb_fail(sb, false);
    sb->buf = nb;
    sb->cap = (uint32_t)nc;
    return 0;
}

static int sb_widen(KzStrBuf *sb) {
    if (sb->failed) return -1;
    uint32_t cap = sb->cap < 16 ? 16 : sb->cap;
    uint16_t *nb = kz_malloc(sb->ctx, (size_t)cap * 2);
    if (!nb) return sb_fail(sb, false);
    for (uint32_t i = 0; i < sb->len; i++) nb[i] = sb->buf[i];
    if (sb->buf) kz_free(sb->ctx->rt, sb->buf);
    sb->buf = (uint8_t *)nb;
    sb->cap = cap;
    sb->wide = true;
    return 0;
}

int kz_sb_putc(KzStrBuf *sb, uint32_t c) {
    if (c > 0xFF && !sb->wide && sb_widen(sb)) return -1;
    if (sb->len == sb->cap && sb_reserve(sb, 1)) return -1;
    if (sb->failed) return -1;
    if (sb->wide) ((uint16_t *)sb->buf)[sb->len++] = (uint16_t)c;
    else sb->buf[sb->len++] = (uint8_t)c;
    return 0;
}

int kz_sb_put_cp(KzStrBuf *sb, uint32_t c) {
    if (c > 0xFFFF) {
        c -= 0x10000;
        if (kz_sb_putc(sb, 0xD800 + (c >> 10))) return -1;
        return kz_sb_putc(sb, 0xDC00 + (c & 0x3FF));
    }
    return kz_sb_putc(sb, c);
}

int kz_sb_put_latin1(KzStrBuf *sb, const uint8_t *s, uint32_t n) {
    if (sb_reserve(sb, n)) return -1;
    if (sb->wide) {
        uint16_t *d = (uint16_t *)sb->buf + sb->len;
        for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    } else memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    return 0;
}

int kz_sb_puts(KzStrBuf *sb, const char *s) { return kz_sb_put_latin1(sb, (const uint8_t *)s, (uint32_t)strlen(s)); }

int kz_sb_put_substr(KzStrBuf *sb, JSString *s, uint32_t from, uint32_t to) {
    if (to <= from) return sb->failed ? -1 : 0;
    uint32_t n = to - from;
    if (!kz_str_wide(s)) return kz_sb_put_latin1(sb, kz_str8(s) + from, n);
    if (!sb->wide && sb_widen(sb)) return -1;
    if (sb_reserve(sb, n)) return -1;
    memcpy((uint16_t *)sb->buf + sb->len, kz_str16(s) + from, (size_t)n * 2);
    sb->len += n;
    return 0;
}

int kz_sb_put_str(KzStrBuf *sb, JSString *s) { return kz_sb_put_substr(sb, s, 0, kz_str_len(s)); }

int kz_sb_put_value(KzStrBuf *sb, JSValue v) {
    JSString *s = kz_str_flat(sb->ctx, JV_STR(v));
    if (!s) return sb_fail(sb, false);
    return kz_sb_put_str(sb, s);
}

JSValue kz_sb_finish(KzStrBuf *sb) {
    JSContext *ctx = sb->ctx;
    if (sb->failed) {
        kz_sb_free(sb);
        return JS_EXCEPTION;
    }
    JSValue r;
    if (sb->len > 64 && sb->cap - sb->len < sb->len / 4) {
        /* adopt the buffer as the string's out-of-line data (a wide buffer always holds a unit above 0xFF:
           widening happens only when one is appended) */
        JSString *s = kz_gc_new(ctx, KZ_GC_STRING, sizeof(JSString));
        if (!s) { kz_sb_free(sb); return JS_EXCEPTION; }
        s->h.aux = sb->len;
        s->h.flags = (uint16_t)(STR_EXT | (sb->wide ? STR_WIDE : 0));
        s->u.ext = sb->buf;
        sb->buf = NULL;
        return JV_MKSTR(s);
    }
    if (sb->wide) r = kz_str_from_utf16(ctx, (const uint16_t *)sb->buf, sb->len);
    else r = kz_str_from_latin1(ctx, sb->buf, sb->len);
    kz_sb_free(sb);
    return r;
}
