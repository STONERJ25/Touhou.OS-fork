/*
 * collections.c: Map, Set, WeakMap, WeakSet (ECMA-262 24), WeakRef and FinalizationRegistry (26), and the
 * collector's weak processing (ephemerons, weak references, finalization).
 *
 * A Map or Set keeps its entries in insertion order in one array, with a hash index chained through the entries.
 * Deleting leaves a tombstone (key = EMPTY), so an iterator's position (an index) stays valid; the array is
 * compacted only while no iterator is live (the count of live iterators sits in the table; an iterator gives its
 * share back when it finishes or is collected).  Keys are normalized so SameValueZero is bit equality, except for
 * strings and BigInts, which compare by content.
 */
#include "jsint.h"

typedef struct KzMapEntry {
    JSValue key, value;
    uint32_t hash, next;        /* next: the following entry index in the bucket chain, or NONE */
} KzMapEntry;

#define NONE 0xFFFFFFFFu

struct KzMapData {
    KzMapEntry *e;
    uint32_t len, cap, count;   /* used slots (with tombstones), capacity, live entries */
    uint32_t *buckets;
    uint32_t nbuckets;          /* a power of two, or 0 */
    uint32_t iterators;         /* live iterators: no compaction while nonzero */
};

typedef struct KzFinCell {
    struct KzFinCell *next;
    JSValue target, held, token;   /* target and token are weak */
} KzFinCell;

/* ================================================================== keys */

static JSValue normalize_key(JSValue k) {
    if (JV_IS_DOUBLE(k)) {
        double d = jv_dbl(k);
        if (d == 0) return JV_MKINT(0);
        return jv_number(d);
    }
    return k;
}

/* the hash of a normalized key (strings must be flat) */
static uint32_t key_hash(JSValue k) {
    if (JV_IS_STRING(k)) return kz_str_hash(JV_STR(k));
    if (JV_IS_BIGINT(k)) return kz_bigint_hash(k);
    uint64_t x = k;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static bool key_eq(JSValue a, JSValue b) {
    if (a == b) return true;
    if (JV_IS_STRING(a) && JV_IS_STRING(b)) return kz_str_eq(JV_STR(a), JV_STR(b));
    if (JV_IS_BIGINT(a) && JV_IS_BIGINT(b)) return kz_bigint_eq(a, b);
    return false;
}

/* prepares a key for lookup: normalized, strings flattened; JS_EXCEPTION on OOM */
static JSValue prep_key(JSContext *ctx, JSValue k) {
    k = normalize_key(k);
    if (JV_IS_STRING(k)) return kz_str_flatten(ctx, k);
    return k;
}

/* ================================================================== the table */

static KzMapData *map_data(JSContext *ctx, JSObject *o) {
    if (o->u.map) return o->u.map;
    KzMapData *m = kz_mallocz(ctx, sizeof(KzMapData));
    if (!m) return NULL;
    o->u.map = m;
    return m;
}

static uint32_t find(KzMapData *m, JSValue key, uint32_t h) {
    if (!m || !m->nbuckets) return NONE;
    for (uint32_t i = m->buckets[h & (m->nbuckets - 1)]; i != NONE; i = m->e[i].next)
        if (m->e[i].hash == h && m->e[i].key != JS_EMPTY && key_eq(m->e[i].key, key)) return i;
    return NONE;
}

static void rehash(KzMapData *m) {
    for (uint32_t i = 0; i < m->nbuckets; i++) m->buckets[i] = NONE;
    for (uint32_t i = 0; i < m->len; i++) {
        if (m->e[i].key == JS_EMPTY) { m->e[i].next = NONE; continue; }
        uint32_t b = m->e[i].hash & (m->nbuckets - 1);
        m->e[i].next = m->buckets[b];
        m->buckets[b] = i;
    }
}

/* removes the tombstones (only with no live iterators) */
static void compact(KzMapData *m) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < m->len; i++)
        if (m->e[i].key != JS_EMPTY) m->e[w++] = m->e[i];
    m->len = w;
    rehash(m);
}

static int grow(JSContext *ctx, KzMapData *m) {
    if (m->iterators == 0 && m->len - m->count >= m->len / 2 && m->len >= 8) {
        compact(m);
        if (m->len < m->cap) return 0;
    }
    uint32_t nc = m->cap ? m->cap * 2 : 8;
    if (nc > 0x7FFFFFF) { kz_throw_range(ctx, "Map maximum size exceeded"); return -1; }
    KzMapEntry *e = kz_realloc(ctx, m->e, (size_t)nc * sizeof(KzMapEntry));
    if (!e) return -1;
    m->e = e;
    m->cap = nc;
    if (m->nbuckets < nc) {
        uint32_t *b = kz_realloc(ctx, m->buckets, (size_t)nc * sizeof(uint32_t));
        if (!b) return -1;
        m->buckets = b;
        m->nbuckets = nc;
        rehash(m);
    }
    return 0;
}

/* key and value must be rooted by the caller; key prepared */
static int table_set(JSContext *ctx, JSObject *o, JSValue key, JSValue value) {
    KzMapData *m = map_data(ctx, o);
    if (!m) return -1;
    uint32_t h = key_hash(key);
    uint32_t i = find(m, key, h);
    if (i != NONE) {
        m->e[i].value = value;
        return 0;
    }
    if (m->len == m->cap && grow(ctx, m)) return -1;
    m = o->u.map;
    i = m->len++;
    m->e[i].key = key;
    m->e[i].value = value;
    m->e[i].hash = h;
    uint32_t b = h & (m->nbuckets - 1);
    m->e[i].next = m->buckets[b];
    m->buckets[b] = i;
    m->count++;
    return 0;
}

static void unlink_entry(KzMapData *m, uint32_t i) {
    uint32_t b = m->e[i].hash & (m->nbuckets - 1);
    uint32_t *p = &m->buckets[b];
    while (*p != NONE && *p != i) p = &m->e[*p].next;
    if (*p == i) *p = m->e[i].next;
    m->e[i].next = NONE;
    m->e[i].key = JS_EMPTY;
    m->e[i].value = JS_UNDEFINED;
    m->count--;
}

static bool table_delete(KzMapData *m, JSValue key) {
    uint32_t i = find(m, key, key_hash(key));
    if (i == NONE) return false;
    unlink_entry(m, i);
    if (m->iterators == 0 && m->len >= 16 && m->count < m->len / 4) compact(m);
    return true;
}

static void table_clear(KzMapData *m) {
    if (!m) return;
    if (m->iterators == 0) {
        m->len = m->count = 0;
        for (uint32_t i = 0; i < m->nbuckets; i++) m->buckets[i] = NONE;
        return;
    }
    for (uint32_t i = 0; i < m->len; i++) {
        m->e[i].key = JS_EMPTY;
        m->e[i].value = JS_UNDEFINED;
        m->e[i].next = NONE;
    }
    for (uint32_t i = 0; i < m->nbuckets; i++) m->buckets[i] = NONE;
    m->count = 0;
}

/* ------------------------------------------------------------------ the engine-internal API */

JSValue kz_new_map(JSContext *ctx) { return kz_new_object_class(ctx, KZ_CLASS_MAP, kz_intr(ctx, KZ_I_MAP_PROTO)); }

int kz_map_set(JSContext *ctx, JSObject *map, JSValue key, JSValue value) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = JV_MKOBJ(map);
    r[2] = value;
    r[1] = prep_key(ctx, key);
    if (r[1] == JS_EXCEPTION) return -1;
    return table_set(ctx, JV_OBJ(r[0]), r[1], r[2]);
}

JSValue kz_map_get(JSContext *ctx, JSObject *map, JSValue key) {
    KZ_ROOT(ctx, rm);
    rm = JV_MKOBJ(map);
    key = prep_key(ctx, key);
    if (key == JS_EXCEPTION) return key;
    KzMapData *m = JV_OBJ(rm)->u.map;
    uint32_t i = find(m, key, key_hash(key));
    return i == NONE ? JS_UNDEFINED : m->e[i].value;
}

void kz_map_trace(JSRuntime *rt, JSObject *o) {
    KzMapData *m = o->u.map;
    if (!m) return;
    for (uint32_t i = 0; i < m->len; i++) {
        if (m->e[i].key == JS_EMPTY) continue;
        kz_mark_value(rt, m->e[i].key);
        kz_mark_value(rt, m->e[i].value);
    }
}

void kz_map_free(JSRuntime *rt, JSObject *o) {
    KzMapData *m = o->u.map;
    if (!m) return;
    kz_free(rt, m->e);
    kz_free(rt, m->buckets);
    kz_free(rt, m);
    o->u.map = NULL;
}

bool kz_is_marked(JSRuntime *rt, const void *cell);
size_t kz_weak_count(JSRuntime *rt);
JSObject *kz_weak_at(JSRuntime *rt, size_t i);
void kz_weak_drain(JSRuntime *rt);

/* ================================================================== iterators */

/* u.any: a = the map or set (undefined when finished), b = the next index, c = the kind (0 keys, 1 values, 2 entries) */
static JSValue new_iterator(JSContext *ctx, JSValue coll, KzClassId cls, KzIntrinsic proto, int kind) {
    KZ_ROOT(ctx, rc);
    rc = coll;
    JSValue it = kz_new_object_class(ctx, cls, kz_intr(ctx, proto));
    if (it == JS_EXCEPTION) return it;
    KzMapData *m = map_data(ctx, JV_OBJ(rc));
    if (!m) return JS_EXCEPTION;
    m->iterators++;
    JSObject *o = JV_OBJ(it);
    o->u.any.a = rc;
    o->u.any.b = JV_MKINT(0);
    o->u.any.c = JV_MKINT(kind);
    return it;
}

static void iter_finish(JSObject *it) {
    if (JV_IS_OBJECT(it->u.any.a)) {
        KzMapData *m = JV_OBJ(it->u.any.a)->u.map;
        if (m && m->iterators) m->iterators--;
    }
    it->u.any.a = JS_UNDEFINED;
}

/* called by the collector when a map or set iterator dies */
void kz_map_iter_free(JSRuntime *rt, JSObject *it) {
    JSValue c = it->u.any.a;
    if (!JV_IS_OBJECT(c) || !kz_is_marked(rt, JV_PTR(c))) return;   /* the table died too */
    KzMapData *m = JV_OBJ(c)->u.map;
    if (m && m->iterators) m->iterators--;
}

static JSValue iter_next(JSContext *ctx, JSValue this_val, KzClassId cls) {
    if (!kz_is_class(this_val, cls)) return kz_throw_type(ctx, "next method called on an incompatible receiver");
    JSObject *it = JV_OBJ(this_val);
    if (!JV_IS_OBJECT(it->u.any.a)) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    KzMapData *m = JV_OBJ(it->u.any.a)->u.map;
    uint32_t i = (uint32_t)JV_INT(it->u.any.b);
    while (m && i < m->len && m->e[i].key == JS_EMPTY) i++;
    if (!m || i >= m->len) {
        iter_finish(it);
        return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    }
    it->u.any.b = JV_MKINT((int32_t)(i + 1));
    int kind = JV_INT(it->u.any.c);
    JSValue val = cls == KZ_CLASS_SET_ITERATOR ? m->e[i].key : m->e[i].value;
    if (kind == 0) return kz_create_iter_result(ctx, m->e[i].key, false);
    if (kind == 1) return kz_create_iter_result(ctx, val, false);
    JSValue pair[2] = { m->e[i].key, val };
    KZ_ROOT(ctx, a);
    a = kz_new_array_from(ctx, 2, pair);
    if (a == JS_EXCEPTION) return a;
    return kz_create_iter_result(ctx, a, false);
}

static JSValue map_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return iter_next(ctx, this_val, KZ_CLASS_MAP_ITERATOR);
}

static JSValue set_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return iter_next(ctx, this_val, KZ_CLASS_SET_ITERATOR);
}

/* ================================================================== Map and Set */

static const char *const coll_names[] = { "Map", "Set", "WeakMap", "WeakSet" };
static const KzClassId coll_class[] = { KZ_CLASS_MAP, KZ_CLASS_SET, KZ_CLASS_WEAKMAP, KZ_CLASS_WEAKSET };

static JSObject *this_coll(JSContext *ctx, JSValue v, int which, const char *method) {
    if (kz_is_class(v, coll_class[which])) return JV_OBJ(v);
    kz_throw_type(ctx, "%s.prototype.%s called on an incompatible receiver", coll_names[which], method);
    return NULL;
}

/* CanBeHeldWeakly */
static bool can_be_held_weakly(JSValue v) {
    if (JV_IS_OBJECT(v)) return true;
    return JV_IS_SYMBOL(v) && !(JV_SYM(v)->h.flags & (SYM_REGISTERED));
}

static JSValue map_set_native(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);
static JSValue set_add_native(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);

/* the constructors: magic = 0 Map, 1 Set, 2 WeakMap, 3 WeakSet */
static JSValue coll_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    JSRuntime *rt = ctx->rt;
    static const KzIntrinsic protos[] = { KZ_I_MAP_PROTO, KZ_I_SET_PROTO, KZ_I_WEAKMAP_PROTO, KZ_I_WEAKSET_PROTO };
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor %s requires 'new'", coll_names[magic]);
    KZ_ROOTS(ctx, r, 5);
    r[0] = kz_ordinary_create_from_ctor(ctx, rt->new_target, protos[magic], coll_class[magic]);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!map_data(ctx, JV_OBJ(r[0]))) return JS_EXCEPTION;
    if (JV_IS_NULLISH(argv[0])) return r[0];
    bool pairs = magic == 0 || magic == 2;
    r[1] = kz_get(ctx, r[0], rt->atom[pairs ? KZ_ATOM_set : KZ_ATOM_add], r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (!kz_is_callable(r[1])) return kz_throw_type(ctx, "'%s' returned for property '%s' of object is not a function", "value", pairs ? "set" : "add");
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, argv[0], &it, false)) return JS_EXCEPTION;
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[2]);
        if (s < 0) return JS_EXCEPTION;
        if (!s) return r[0];
        JSValue res;
        if (pairs) {
            if (!JV_IS_OBJECT(r[2])) {
                kz_throw_type(ctx, "Iterator value is not an entry object");
                kz_iter_close_on_throw(ctx, &it);
                return JS_EXCEPTION;
            }
            r[3] = kz_get_index(ctx, r[2], 0);
            if (r[3] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
            r[4] = kz_get_index(ctx, r[2], 1);
            if (r[4] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
            JSValue args[2] = { r[3], r[4] };
            res = kz_call(ctx, r[1], r[0], 2, args);
        } else {
            JSValue args[1] = { r[2] };
            res = kz_call(ctx, r[1], r[0], 1, args);
        }
        if (res == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
    }
}

static JSValue map_get_native(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "get");
    if (!o) return JS_EXCEPTION;
    if (magic == 2 && !can_be_held_weakly(argv[0])) return JS_UNDEFINED;
    return kz_map_get(ctx, o, argv[0]);
}

/* has (magic: the collection kind) */
static JSValue coll_has(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "has");
    if (!o) return JS_EXCEPTION;
    if (magic >= 2 && !can_be_held_weakly(argv[0])) return JS_FALSE;
    KZ_ROOT(ctx, ro);
    ro = this_val;
    JSValue k = prep_key(ctx, argv[0]);
    if (k == JS_EXCEPTION) return k;
    return JV_MKBOOL(find(JV_OBJ(ro)->u.map, k, key_hash(k)) != NONE);
}

static JSValue coll_delete(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "delete");
    if (!o) return JS_EXCEPTION;
    if (magic >= 2 && !can_be_held_weakly(argv[0])) return JS_FALSE;
    KZ_ROOT(ctx, ro);
    ro = this_val;
    JSValue k = prep_key(ctx, argv[0]);
    if (k == JS_EXCEPTION) return k;
    KzMapData *m = JV_OBJ(ro)->u.map;
    if (!m) return JS_FALSE;
    return JV_MKBOOL(table_delete(m, k));
}

static JSValue map_set_native(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "set");
    if (!o) return JS_EXCEPTION;
    if (magic == 2 && !can_be_held_weakly(argv[0])) return kz_throw_type(ctx, "Invalid value used as weak map key");
    if (kz_map_set(ctx, o, argv[0], argv[1])) return JS_EXCEPTION;
    return this_val;
}

static JSValue set_add_native(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "add");
    if (!o) return JS_EXCEPTION;
    if (magic == 3 && !can_be_held_weakly(argv[0])) return kz_throw_type(ctx, "Invalid value used in weak set");
    if (kz_map_set(ctx, o, argv[0], JS_UNDEFINED)) return JS_EXCEPTION;
    return this_val;
}

static JSValue coll_clear(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    JSObject *o = this_coll(ctx, this_val, magic, "clear");
    if (!o) return JS_EXCEPTION;
    table_clear(o->u.map);
    return JS_UNDEFINED;
}

static JSValue coll_size(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    JSObject *o = this_coll(ctx, this_val, magic, "size");
    if (!o) return JS_EXCEPTION;
    return JV_MKINT(o->u.map ? (int32_t)o->u.map->count : 0);
}

static JSValue coll_for_each(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *o = this_coll(ctx, this_val, magic, "forEach");
    if (!o) return JS_EXCEPTION;
    if (!kz_is_callable(argv[0])) return kz_throw_not_function(ctx, argv[0]);
    KzMapData *m = map_data(ctx, o);
    if (!m) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 3);
    r[0] = this_val;
    m->iterators++;
    JSValue res = JS_UNDEFINED;
    for (uint32_t i = 0; i < JV_OBJ(r[0])->u.map->len; i++) {
        m = JV_OBJ(r[0])->u.map;
        if (m->e[i].key == JS_EMPTY) continue;
        r[1] = m->e[i].key;
        r[2] = magic == 0 ? m->e[i].value : r[1];
        JSValue args[3] = { r[2], r[1], r[0] };
        if (kz_call(ctx, argv[0], argv[1], 3, args) == JS_EXCEPTION) { res = JS_EXCEPTION; break; }
    }
    m = JV_OBJ(r[0])->u.map;
    m->iterators--;
    return res;
}

/* keys (0), values (1), entries (2); the collection kind in the upper bits */
static JSValue coll_iter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    int which = magic >> 4, kind = magic & 15;
    JSObject *o = this_coll(ctx, this_val, which, kind == 0 ? "keys" : kind == 1 ? "values" : "entries");
    if (!o) return JS_EXCEPTION;
    if (which == 1 && kind == 0) kind = 1;
    if (which == 0) return new_iterator(ctx, this_val, KZ_CLASS_MAP_ITERATOR, KZ_I_MAP_ITERATOR_PROTO, kind);
    return new_iterator(ctx, this_val, KZ_CLASS_SET_ITERATOR, KZ_I_SET_ITERATOR_PROTO, kind == 2 ? 2 : 1);
}

static JSValue map_group_by(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, out);
    if (kz_group_by(ctx, argv[0], argv[1], 1, &out)) return JS_EXCEPTION;
    return out;
}

/* ------------------------------------------------------------------ the Set methods (union, intersection, ...) */

typedef struct SetRec { JSValue set, has, keys; double size; } SetRec;

/* GetSetRecord; r points at three rooted slots */
static int get_set_record(JSContext *ctx, JSValue obj, JSValue *slots, double *size) {
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "The argument must be a set-like object"); return -1; }
    JSValue raw = kz_get(ctx, obj, rt->atom[KZ_ATOM_size], obj);
    if (raw == JS_EXCEPTION) return -1;
    double num;
    if (kz_to_number(ctx, raw, &num)) return -1;
    if (num != num) { kz_throw_type(ctx, "The 'size' property must be a number"); return -1; }
    double sz;
    if (kz_to_integer_or_inf(ctx, jv_number(num), &sz)) return -1;
    if (sz < 0) { kz_throw_range(ctx, "The 'size' property must not be negative"); return -1; }
    slots[0] = obj;
    slots[1] = kz_get(ctx, obj, rt->atom[KZ_ATOM_has], obj);
    if (slots[1] == JS_EXCEPTION) return -1;
    if (!kz_is_callable(slots[1])) { kz_throw_type(ctx, "The 'has' property must be a function"); return -1; }
    slots[2] = kz_get(ctx, obj, rt->atom[KZ_ATOM_keys], obj);
    if (slots[2] == JS_EXCEPTION) return -1;
    if (!kz_is_callable(slots[2])) { kz_throw_type(ctx, "The 'keys' property must be a function"); return -1; }
    *size = sz;
    return 0;
}

static JSValue copy_set(JSContext *ctx, JSObject *src) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = JV_MKOBJ(src);
    r[1] = kz_new_object_class(ctx, KZ_CLASS_SET, kz_intr(ctx, KZ_I_SET_PROTO));
    if (r[1] == JS_EXCEPTION) return r[1];
    KzMapData *m = JV_OBJ(r[0])->u.map;
    for (uint32_t i = 0; m && i < m->len; i++) {
        if (m->e[i].key == JS_EMPTY) continue;
        if (table_set(ctx, JV_OBJ(r[1]), m->e[i].key, JS_UNDEFINED)) return JS_EXCEPTION;
        m = JV_OBJ(r[0])->u.map;
    }
    return r[1];
}

static bool set_contains(JSObject *s, JSValue key) {
    return find(s->u.map, key, key_hash(key)) != NONE;
}

/* union (0), intersection (1), difference (2), symmetricDifference (3), isSubsetOf (4), isSupersetOf (5),
   isDisjointFrom (6) */
static JSValue set_algebra(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "union", "intersection", "difference", "symmetricDifference", "isSubsetOf",
                                         "isSupersetOf", "isDisjointFrom" };
    JSObject *o = this_coll(ctx, this_val, 1, names[magic]);
    if (!o) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 8);
    r[0] = this_val;
    double other_size;
    if (get_set_record(ctx, argv[0], &r[1], &other_size)) return JS_EXCEPTION;
    KzMapData *m = map_data(ctx, JV_OBJ(r[0]));
    if (!m) return JS_EXCEPTION;
    double this_size = m->count;
    /* iterate this set with has() on the other (true), or iterate the other's keys (false) */
    bool use_has = false;
    switch (magic) {
    case 1: case 2: case 6: use_has = this_size <= other_size; break;
    case 4: use_has = true; break;
    case 0: case 3: case 5: use_has = false; break;
    }
    if (magic == 4 && this_size > other_size) return JS_FALSE;
    if (magic == 5 && this_size < other_size) return JS_FALSE;
    if (magic <= 3) {
        if (magic == 1) {
            r[4] = kz_new_object_class(ctx, KZ_CLASS_SET, kz_intr(ctx, KZ_I_SET_PROTO));
            if (r[4] == JS_EXCEPTION) return r[4];
        } else {
            r[4] = copy_set(ctx, JV_OBJ(r[0]));
            if (r[4] == JS_EXCEPTION) return r[4];
        }
    }
    if (use_has) {
        m = JV_OBJ(r[0])->u.map;
        m->iterators++;
        JSValue res = JS_UNDEFINED;
        for (uint32_t i = 0; i < JV_OBJ(r[0])->u.map->len; i++) {
            m = JV_OBJ(r[0])->u.map;
            if (m->e[i].key == JS_EMPTY) continue;
            r[5] = m->e[i].key;
            JSValue args[1] = { r[5] };
            r[6] = kz_call(ctx, r[2], r[1], 1, args);
            if (r[6] == JS_EXCEPTION) { res = JS_EXCEPTION; break; }
            bool in = kz_to_bool(r[6]);
            if (magic == 1 && in) { if (table_set(ctx, JV_OBJ(r[4]), r[5], JS_UNDEFINED)) { res = JS_EXCEPTION; break; } }
            else if (magic == 2 && in) { KzMapData *rm = JV_OBJ(r[4])->u.map; if (rm) table_delete(rm, r[5]); }
            else if (magic == 4 && !in) { res = JS_FALSE; break; }
            else if (magic == 6 && in) { res = JS_FALSE; break; }
        }
        JV_OBJ(r[0])->u.map->iterators--;
        if (res == JS_EXCEPTION || res == JS_FALSE) return res;
        if (magic == 4 || magic == 6) return JS_TRUE;
        return r[4];
    }
    /* iterate the other set's keys */
    r[5] = kz_call(ctx, r[3], r[1], 0, NULL);
    if (r[5] == JS_EXCEPTION) return r[5];
    if (!JV_IS_OBJECT(r[5])) return kz_throw_type(ctx, "keys() must return an object");
    KZ_ROOT_ITER(ctx, it);
    it.iter = r[5];
    it.next = kz_get(ctx, r[5], ctx->rt->atom[KZ_ATOM_next], r[5]);
    if (it.next == JS_EXCEPTION) return JS_EXCEPTION;
    if (!kz_is_callable(it.next)) return kz_throw_type(ctx, "The keys iterator's 'next' must be a function");
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[6]);
        if (s < 0) return JS_EXCEPTION;
        if (!s) break;
        r[6] = prep_key(ctx, r[6]);
        if (r[6] == JS_EXCEPTION) return r[6];
        switch (magic) {
        case 0:
            if (table_set(ctx, JV_OBJ(r[4]), r[6], JS_UNDEFINED)) return JS_EXCEPTION;
            break;
        case 1:
            if (set_contains(JV_OBJ(r[0]), r[6]) && table_set(ctx, JV_OBJ(r[4]), r[6], JS_UNDEFINED)) return JS_EXCEPTION;
            break;
        case 2: {
            KzMapData *rm = JV_OBJ(r[4])->u.map;
            if (rm) table_delete(rm, r[6]);
            break;
        }
        case 3:
            if (set_contains(JV_OBJ(r[0]), r[6])) {
                KzMapData *rm = JV_OBJ(r[4])->u.map;
                if (rm) table_delete(rm, r[6]);
            } else if (table_set(ctx, JV_OBJ(r[4]), r[6], JS_UNDEFINED)) return JS_EXCEPTION;
            break;
        case 5:
            if (!set_contains(JV_OBJ(r[0]), r[6])) return kz_iter_close(ctx, &it, JS_UNDEFINED) == JS_EXCEPTION ? JS_EXCEPTION : JS_FALSE;
            break;
        case 6:
            if (set_contains(JV_OBJ(r[0]), r[6])) return kz_iter_close(ctx, &it, JS_UNDEFINED) == JS_EXCEPTION ? JS_EXCEPTION : JS_FALSE;
            break;
        }
    }
    if (magic >= 5) return JS_TRUE;
    return r[4];
}

/* ================================================================== WeakRef and FinalizationRegistry */

static int keep_alive(JSContext *ctx, JSValue v) { return kz_vec_push(ctx, &ctx->rt->kept_alive, v); }

static JSValue weakref_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor WeakRef requires 'new'");
    if (!can_be_held_weakly(argv[0])) return kz_throw_type(ctx, "WeakRef: invalid target");
    KZ_ROOT(ctx, o);
    o = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_WEAKREF_PROTO, KZ_CLASS_WEAKREF);
    if (o == JS_EXCEPTION) return o;
    if (keep_alive(ctx, argv[0])) return JS_EXCEPTION;
    JV_OBJ(o)->u.weakref.target = argv[0];
    return o;
}

static JSValue weakref_deref(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_WEAKREF)) return kz_throw_type(ctx, "WeakRef.prototype.deref called on an incompatible receiver");
    JSValue t = JV_OBJ(this_val)->u.weakref.target;
    if (t != JS_UNDEFINED && keep_alive(ctx, t)) return JS_EXCEPTION;
    return t;
}

static JSValue finreg_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor FinalizationRegistry requires 'new'");
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "FinalizationRegistry: cleanup must be callable");
    JSValue o = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_FINREG_PROTO, KZ_CLASS_FINREG);
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.finreg.callback = argv[0];
    return o;
}

static JSValue finreg_register(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_FINREG)) return kz_throw_type(ctx, "FinalizationRegistry.prototype.register called on an incompatible receiver");
    JSValue target = argv[0], held = argv[1], token = argv[2];
    if (!can_be_held_weakly(target)) return kz_throw_type(ctx, "FinalizationRegistry.prototype.register: invalid target");
    if (target == held) return kz_throw_type(ctx, "FinalizationRegistry.prototype.register: target and holdings must not be same");
    if (!can_be_held_weakly(token) && token != JS_UNDEFINED) return kz_throw_type(ctx, "FinalizationRegistry.prototype.register: invalid unregister token");
    KzFinCell *c = kz_malloc(ctx, sizeof(KzFinCell));
    if (!c) return JS_EXCEPTION;
    JSObject *o = JV_OBJ(this_val);
    c->target = target;
    c->held = held;
    c->token = token;
    c->next = o->u.finreg.cells;
    o->u.finreg.cells = c;
    return JS_UNDEFINED;
}

static JSValue finreg_unregister(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_FINREG)) return kz_throw_type(ctx, "FinalizationRegistry.prototype.unregister called on an incompatible receiver");
    if (!can_be_held_weakly(argv[0])) return kz_throw_type(ctx, "FinalizationRegistry.prototype.unregister: invalid token");
    JSObject *o = JV_OBJ(this_val);
    bool removed = false;
    for (KzFinCell **p = &o->u.finreg.cells; *p;) {
        if ((*p)->token == argv[0]) {
            KzFinCell *dead = *p;
            *p = dead->next;
            kz_free(ctx->rt, dead);
            removed = true;
        } else p = &(*p)->next;
    }
    return JV_MKBOOL(removed);
}

void kz_finreg_trace(JSRuntime *rt, JSObject *o) {
    kz_mark_value(rt, o->u.finreg.callback);
    for (KzFinCell *c = o->u.finreg.cells; c; c = c->next) kz_mark_value(rt, c->held);
}

void kz_finreg_free(JSRuntime *rt, JSObject *o) {
    for (KzFinCell *c = o->u.finreg.cells; c;) {
        KzFinCell *n = c->next;
        kz_free(rt, c);
        c = n;
    }
    o->u.finreg.cells = NULL;
}

/* cleanup cells whose targets died wait here (target set to EMPTY) until the host runs jobs */
static bool value_marked(JSRuntime *rt, JSValue v) {
    if (JV_IS_OBJECT(v) || JV_IS_SYMBOL(v)) return kz_is_marked(rt, JV_PTR(v));
    return true;
}

static void finreg_note_pending(JSRuntime *rt, JSObject *o) {
    for (uint32_t i = 0; i < rt->finreg_npending; i++)
        if (rt->finreg_pending[i] == o) return;
    if (rt->finreg_npending == rt->finreg_cap) {
        uint32_t nc = rt->finreg_cap ? rt->finreg_cap * 2 : 8;
        JSObject **n = rt->sys_realloc(rt->sys_opaque, rt->finreg_pending, nc * sizeof(JSObject *));
        if (!n) return;            /* the cells stay pending; they are seen at the next collection */
        rt->finreg_pending = n;
        rt->finreg_cap = nc;
    }
    rt->finreg_pending[rt->finreg_npending++] = o;
}

/* after the main marking: ephemerons to a fixpoint, then clearing */
void kz_weak_process(JSRuntime *rt) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < kz_weak_count(rt); i++) {
            JSObject *o = kz_weak_at(rt, i);
            if (kz_class(o) != KZ_CLASS_WEAKMAP || !o->u.map) continue;
            KzMapData *m = o->u.map;
            for (uint32_t j = 0; j < m->len; j++) {
                JSValue k = m->e[j].key, v = m->e[j].value;
                if (k == JS_EMPTY || !value_marked(rt, k)) continue;
                if ((JV_IS_OBJECT(v) || JV_IS_STRING(v) || JV_IS_SYMBOL(v) || JV_IS_BIGINT(v)) && !kz_is_marked(rt, JV_PTR(v))) {
                    kz_mark_value(rt, v);
                    changed = true;
                }
            }
        }
        if (changed) kz_weak_drain(rt);
    }
    for (size_t i = 0; i < kz_weak_count(rt); i++) {
        JSObject *o = kz_weak_at(rt, i);
        switch (kz_class(o)) {
        case KZ_CLASS_WEAKMAP: case KZ_CLASS_WEAKSET: {
            KzMapData *m = o->u.map;
            if (!m) break;
            for (uint32_t j = 0; j < m->len; j++)
                if (m->e[j].key != JS_EMPTY && !value_marked(rt, m->e[j].key)) unlink_entry(m, j);
            break;
        }
        case KZ_CLASS_WEAKREF:
            if (!value_marked(rt, o->u.weakref.target)) o->u.weakref.target = JS_UNDEFINED;
            break;
        case KZ_CLASS_FINREG: {
            bool any = false;
            for (KzFinCell *c = o->u.finreg.cells; c; c = c->next) {
                if (c->target != JS_EMPTY && !value_marked(rt, c->target)) { c->target = JS_EMPTY; any = true; }
                if (c->target == JS_EMPTY) any = true;
                if (c->token != JS_UNDEFINED && !value_marked(rt, c->token)) c->token = JS_UNDEFINED;
            }
            if (any) finreg_note_pending(rt, o);
            break;
        }
        default:
            break;
        }
    }
}

/* runs the cleanup callbacks of registries whose targets died (from js_run_jobs) */
int kz_run_finalizers(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    int ran = 0;
    while (rt->finreg_npending) {
        JSObject *o = rt->finreg_pending[--rt->finreg_npending];
        KZ_ROOTS(ctx, r, 2);
        r[0] = JV_MKOBJ(o);
        for (;;) {
            KzFinCell **p = &JV_OBJ(r[0])->u.finreg.cells;
            while (*p && (*p)->target != JS_EMPTY) p = &(*p)->next;
            if (!*p) break;
            KzFinCell *c = *p;
            *p = c->next;
            r[1] = c->held;
            kz_free(rt, c);
            JSValue args[1] = { r[1] };
            JSValue res = kz_call(ctx, JV_OBJ(r[0])->u.finreg.callback, JS_UNDEFINED, 1, args);
            ran++;
            if (res == JS_EXCEPTION) return -1;
        }
    }
    return ran;
}

/* ================================================================== tables */

static const KzFuncDef map_proto_funcs[] = {
    KZ_FN_MAGIC("clear", coll_clear, 0, 0),
    KZ_FN_MAGIC("delete", coll_delete, 1, 0),
    KZ_FN_MAGIC_ARGS("forEach", coll_for_each, 1, 2, 0),
    KZ_FN_MAGIC("get", map_get_native, 1, 0),
    KZ_FN_MAGIC("has", coll_has, 1, 0),
    KZ_FN_MAGIC("keys", coll_iter, 0, 0x00),
    KZ_FN_MAGIC("set", map_set_native, 2, 0),
    KZ_GETTER_MAGIC("size", coll_size, 0),
    KZ_FN_MAGIC("values", coll_iter, 0, 0x01),
};

static const KzFuncDef set_proto_funcs[] = {
    KZ_FN_MAGIC("add", set_add_native, 1, 1),
    KZ_FN_MAGIC("clear", coll_clear, 0, 1),
    KZ_FN_MAGIC("delete", coll_delete, 1, 1),
    KZ_FN_MAGIC("difference", set_algebra, 1, 2),
    KZ_FN_MAGIC("entries", coll_iter, 0, 0x12),
    KZ_FN_MAGIC_ARGS("forEach", coll_for_each, 1, 2, 1),
    KZ_FN_MAGIC("has", coll_has, 1, 1),
    KZ_FN_MAGIC("intersection", set_algebra, 1, 1),
    KZ_FN_MAGIC("isDisjointFrom", set_algebra, 1, 6),
    KZ_FN_MAGIC("isSubsetOf", set_algebra, 1, 4),
    KZ_FN_MAGIC("isSupersetOf", set_algebra, 1, 5),
    KZ_GETTER_MAGIC("size", coll_size, 1),
    KZ_FN_MAGIC("symmetricDifference", set_algebra, 1, 3),
    KZ_FN_MAGIC("union", set_algebra, 1, 0),
};

static const KzFuncDef weakmap_proto_funcs[] = {
    KZ_FN_MAGIC("delete", coll_delete, 1, 2),
    KZ_FN_MAGIC("get", map_get_native, 1, 2),
    KZ_FN_MAGIC("has", coll_has, 1, 2),
    KZ_FN_MAGIC("set", map_set_native, 2, 2),
};

static const KzFuncDef weakset_proto_funcs[] = {
    KZ_FN_MAGIC("add", set_add_native, 1, 3),
    KZ_FN_MAGIC("delete", coll_delete, 1, 3),
    KZ_FN_MAGIC("has", coll_has, 1, 3),
};

static const KzFuncDef species_funcs[] = {
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef map_funcs[] = {
    KZ_FN("groupBy", map_group_by, 2),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef map_iter_funcs[] = { KZ_FN("next", map_iter_next, 0) };
static const KzFuncDef set_iter_funcs[] = { KZ_FN("next", set_iter_next, 0) };
static const KzFuncDef weakref_proto_funcs[] = { KZ_FN("deref", weakref_deref, 0) };
static const KzFuncDef finreg_proto_funcs[] = {
    KZ_FN_ARGS("register", finreg_register, 2, 3),
    KZ_FN("unregister", finreg_unregister, 1),
};

static int set_tag(JSContext *ctx, JSValue obj, const char *tag) {
    JSValue t = kz_atom(ctx, tag);
    if (t == JS_EXCEPTION) return -1;
    return kz_define_value(ctx, obj, ctx->rt->sym[KZ_SYM_toStringTag], t, KZ_PROP_C);
}

static int make_iter_proto(JSContext *ctx, KzIntrinsic id, const KzFuncDef *f, const char *tag) {
    JSValue p = kz_new_object_proto(ctx, kz_intr(ctx, KZ_I_ITERATOR_PROTO));
    if (p == JS_EXCEPTION) return -1;
    ctx->intrinsic[id] = p;
    if (kz_define_functions(ctx, p, f, 1)) return -1;
    return set_tag(ctx, p, tag);
}

int kz_init_map(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    static const KzIntrinsic protos[] = { KZ_I_MAP_PROTO, KZ_I_SET_PROTO, KZ_I_WEAKMAP_PROTO, KZ_I_WEAKSET_PROTO };
    static const int ctor_ids[] = { KZ_I_MAP, KZ_I_SET, -1, -1 };
    static const int atoms[] = { KZ_ATOM_Map, KZ_ATOM_Set, KZ_ATOM_WeakMap, KZ_ATOM_WeakSet };
    for (int k = 0; k < 4; k++) {
        r[0] = kz_new_object(ctx);
        if (r[0] == JS_EXCEPTION) return -1;
        ctx->intrinsic[protos[k]] = r[0];
        r[1] = kz_make_ctor(ctx, coll_ctor, coll_names[k], 0, k, protos[k], ctor_ids[k]);
        if (r[1] == JS_EXCEPTION) return -1;
        static const KzFuncDef *const tables[] = { map_proto_funcs, set_proto_funcs, weakmap_proto_funcs, weakset_proto_funcs };
        static const int sizes[] = { KZ_COUNTOF(map_proto_funcs), KZ_COUNTOF(set_proto_funcs), KZ_COUNTOF(weakmap_proto_funcs),
                                     KZ_COUNTOF(weakset_proto_funcs) };
        if (kz_define_functions(ctx, r[0], tables[k], sizes[k])) return -1;
        if (k == 0 && kz_define_functions(ctx, r[1], map_funcs, KZ_COUNTOF(map_funcs))) return -1;
        if (k == 1 && kz_define_functions(ctx, r[1], species_funcs, KZ_COUNTOF(species_funcs))) return -1;
        if (set_tag(ctx, r[0], coll_names[k])) return -1;
        if (kz_define_value_atom(ctx, ctx->global, atoms[k], r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
        if (k == 0) {
            /* Map.prototype[@@iterator] is entries */
            r[2] = kz_new_native_c(ctx, coll_iter, "entries", 0, 0x02);
            if (r[2] == JS_EXCEPTION) return -1;
            if (kz_define_value_atom(ctx, r[0], KZ_ATOM_entries, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
            if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_iterator], r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
        } else if (k == 1) {
            /* Set.prototype.keys, values and @@iterator are one function */
            r[2] = kz_new_native_c(ctx, coll_iter, "values", 0, 0x11);
            if (r[2] == JS_EXCEPTION) return -1;
            if (kz_define_value_atom(ctx, r[0], KZ_ATOM_values, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
            if (kz_define_value_atom(ctx, r[0], KZ_ATOM_keys, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
            if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_iterator], r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
        }
    }
    if (make_iter_proto(ctx, KZ_I_MAP_ITERATOR_PROTO, map_iter_funcs, "Map Iterator")) return -1;
    if (make_iter_proto(ctx, KZ_I_SET_ITERATOR_PROTO, set_iter_funcs, "Set Iterator")) return -1;
    /* WeakRef */
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_WEAKREF_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, weakref_ctor, "WeakRef", 1, 0, KZ_I_WEAKREF_PROTO, -1);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], weakref_proto_funcs, KZ_COUNTOF(weakref_proto_funcs))) return -1;
    if (set_tag(ctx, r[0], "WeakRef")) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_WeakRef, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* FinalizationRegistry */
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_FINREG_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, finreg_ctor, "FinalizationRegistry", 1, 0, KZ_I_FINREG_PROTO, -1);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], finreg_proto_funcs, KZ_COUNTOF(finreg_proto_funcs))) return -1;
    if (set_tag(ctx, r[0], "FinalizationRegistry")) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_FinalizationRegistry, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
