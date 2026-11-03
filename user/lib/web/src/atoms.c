/*
 * Atoms: element and attribute names (and later class names and ids) are interned once per document, so that the tree
 * builder, the selector matcher and the style system compare small integers instead of strings. The names listed in
 * atomlist.h get fixed numbers (AT_div, AT_href ...) that the code can switch on; every other name gets the next number
 * of its document's table. Lookups are case-sensitive: the tokenizer lowercases HTML names, and SVG's camelCase names
 * ("foreignObject") are different atoms from their lowercase forms.
 */
#include "webint.h"

static const char *const static_names[] = {
#define X(id, s) s,
#include "atomlist.h"
#undef X
};

uint32_t web_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;                  /* FNV-1a */
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static bool rehash(AtomTable *t, uint32_t nslots) {
    uint32_t *slots = wa_alloc(t->a, (size_t)nslots * sizeof(uint32_t));
    if (!slots) return false;
    for (uint32_t id = 0; id < t->count; id++) {
        uint32_t i = t->names[id].hash & (nslots - 1);
        while (slots[i]) i = (i + 1) & (nslots - 1);
        slots[i] = id + 1;
    }
    if (t->slots) wa_free(t->a, t->slots, (size_t)t->nslots * sizeof(uint32_t));
    t->slots = slots;
    t->nslots = nslots;
    return true;
}

static WebAtom insert(AtomTable *t, const char *s, size_t n, uint32_t h, bool copy) {
    if (t->count + 1 > t->nslots / 2 && !rehash(t, t->nslots * 2)) return AT_NONE;
    if (t->count == t->cap) {
        uint32_t nc = t->cap * 2;
        AtomEntry *e = wa_realloc(t->a, t->names, (size_t)t->cap * sizeof(AtomEntry), (size_t)nc * sizeof(AtomEntry));
        if (!e) return AT_NONE;
        t->names = e;
        t->cap = nc;
    }
    const char *str = s;
    if (copy) {
        char *c = wa_strndup(t->a, s, n);
        if (!c) return AT_NONE;
        str = c;
    }
    WebAtom id = t->count++;
    t->names[id].s = str;
    t->names[id].len = (uint32_t)n;
    t->names[id].hash = h;
    uint32_t i = h & (t->nslots - 1);
    while (t->slots[i]) i = (i + 1) & (t->nslots - 1);
    t->slots[i] = id + 1;
    return id;
}

bool atoms_init(AtomTable *t, WebArena *a) {
    memset(t, 0, sizeof *t);
    t->a = a;
    t->cap = 512;
    t->names = wa_alloc(a, (size_t)t->cap * sizeof(AtomEntry));
    if (!t->names) return false;
    t->nslots = 0;
    if (!rehash(t, 1024)) return false;
    for (int i = 0; i < AT_STATIC_COUNT; i++) {
        const char *s = static_names[i];
        size_t n = strlen(s);
        if (insert(t, s, n, web_hash(s, n), false) != (WebAtom)i && i != 0) return false;
    }
    return true;
}

WebAtom atom_find(const AtomTable *t, const char *s, size_t n) {
    uint32_t h = web_hash(s, n);
    uint32_t i = h & (t->nslots - 1);
    while (t->slots[i]) {
        const AtomEntry *e = &t->names[t->slots[i] - 1];
        if (e->hash == h && e->len == n && memcmp(e->s, s, n) == 0) return t->slots[i] - 1;
        i = (i + 1) & (t->nslots - 1);
    }
    return AT_NONE;
}

WebAtom atom_get(AtomTable *t, const char *s, size_t n) {
    if (n == 0) return AT_NONE;
    uint32_t h = web_hash(s, n);
    uint32_t i = h & (t->nslots - 1);
    while (t->slots[i]) {
        const AtomEntry *e = &t->names[t->slots[i] - 1];
        if (e->hash == h && e->len == n && memcmp(e->s, s, n) == 0) return t->slots[i] - 1;
        i = (i + 1) & (t->nslots - 1);
    }
    return insert(t, s, n, h, true);
}

WebAtom atom_get_lower(AtomTable *t, const char *s, size_t n) {
    if (n == 0) return AT_NONE;
    char small[64];
    char *tmp = n <= sizeof small ? small : wa_alloc(t->a, n);
    if (!tmp) return AT_NONE;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        tmp[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    WebAtom r = atom_get(t, tmp, n);
    if (tmp != small) wa_free(t->a, tmp, n);
    return r;
}

const char *atom_str(const AtomTable *t, WebAtom a) {
    return a < t->count ? t->names[a].s : "";
}

uint32_t atom_len(const AtomTable *t, WebAtom a) {
    return a < t->count ? t->names[a].len : 0;
}
