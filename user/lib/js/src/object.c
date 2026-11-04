/*
 * object.c: shapes, objects, dense elements, arrays, and the object internal methods (ECMA-262 10.1 ordinary objects,
 * 10.4.2 array exotic objects), with the hooks exotic classes plug into (kz_exotic[class]).
 *
 * Shapes (jsint.h) describe an object's own property keys, their attributes and the prototype; values are in the
 * object's slot array in shape order.  A shared shape never changes: adding a key moves the object to a child shape
 * found (or made) in the transition list.  Past DICT_THRESHOLD properties, on delete (except of the last key), on
 * attribute changes and on prototype changes, an object gets a private dictionary shape that it edits in place
 * (deleted entries are tombstones until a compaction).
 *
 * Integer keys (array indices) of ordinary objects live in the dense element vector while they are plain data
 * properties with all three attributes (or the uniform frozen/sealed attributes given by the object flags) and the
 * vector stays reasonably dense; otherwise they are ordinary shape entries (the shape is flagged SHAPE_HAS_INDEX).
 *
 * GC discipline (jsint.h): every allocation may collect.  New shapes are only weakly held by their parent until an
 * object points at them, so they are made with all buffers allocated first and the cell last, and nothing allocates
 * between making a shape and installing it.
 */
#include "jsint.h"
#include <stdlib.h>

#define DICT_THRESHOLD 128
#define HASH_MIN 8

const KzExotic *kz_exotic[KZ_CLASS_COUNT];

static inline uint32_t key_hash(JSValue k) { return (uint32_t)(((uint64_t)k * 0x9E3779B97F4A7C15ULL) >> 32); }

/* ================================================================== shapes */

static uint32_t pow2_at_least(uint32_t n) {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static void shape_rehash(JSShape *s) {
    if (!s->hash_size) return;
    memset(s->hash, 0, s->hash_size * sizeof(uint32_t));
    for (uint32_t i = 0; i < s->nprops; i++) {
        if (s->props[i].key == JS_EMPTY) continue;
        uint32_t h = key_hash(s->props[i].key) & (s->hash_size - 1);
        s->props[i].hnext = s->hash[h];
        s->hash[h] = i + 1;
    }
}

int kz_shape_find(const JSShape *s, JSValue key) {
    if (s->hash_size) {
        uint32_t i = s->hash[key_hash(key) & (s->hash_size - 1)];
        while (i) {
            if (s->props[i - 1].key == key) return (int)(i - 1);
            i = s->props[i - 1].hnext;
        }
        return -1;
    }
    for (uint32_t i = 0; i < s->nprops; i++)
        if (s->props[i].key == key) return (int)i;
    return -1;
}

/* A shape cell with room for `cap` properties (nprops = 0).  The buffers come first: they are raw and the
   collector leaves them alone; the cell comes last. */
static JSShape *shape_alloc(JSContext *ctx, JSValue proto, uint32_t cap, bool dict) {
    JSShapeProp *props = NULL;
    uint32_t *hash = NULL;
    uint32_t hsize = 0;
    if (cap) {
        props = kz_malloc(ctx, cap * sizeof(JSShapeProp));
        if (!props) return NULL;
    }
    if (cap > HASH_MIN || (dict && cap)) {
        hsize = pow2_at_least(cap * 2);
        hash = kz_mallocz(ctx, hsize * sizeof(uint32_t));
        if (!hash) { kz_free(ctx->rt, props); return NULL; }
    }
    JSShape *s = kz_gc_new(ctx, KZ_GC_SHAPE, sizeof(JSShape));
    if (!s) {
        kz_free(ctx->rt, props);
        kz_free(ctx->rt, hash);
        return NULL;
    }
    s->h.flags = dict ? SHAPE_DICT : 0;
    s->proto = proto;
    s->props = props;
    s->prop_cap = cap;
    s->hash = hash;
    s->hash_size = hsize;
    if (JV_IS_OBJECT(proto)) JV_OBJ(proto)->h.flags |= OBJ_IS_PROTO;
    return s;
}

/* The root-shape table: prototype object -> its empty shape (weak, open addressing on the prototype pointer). */
static uint32_t proto_hash(JSValue p) { return key_hash(p); }

static bool roots_grow(JSRuntime *rt) {
    uint32_t nc = rt->root_cap ? rt->root_cap * 2 : 256;
    JSShape **t = rt->sys_malloc(rt->sys_opaque, nc * sizeof(JSShape *));
    if (!t) return false;
    memset(t, 0, nc * sizeof(JSShape *));
    for (uint32_t i = 0; i < rt->root_cap; i++) {
        JSShape *s = rt->root_shapes[i];
        if (!s) continue;
        uint32_t j = proto_hash(s->proto) & (nc - 1);
        while (t[j]) j = (j + 1) & (nc - 1);
        t[j] = s;
    }
    if (rt->root_shapes) rt->sys_free(rt->sys_opaque, rt->root_shapes);
    rt->root_shapes = t;
    rt->root_cap = nc;
    return true;
}

bool kz_is_marked(JSRuntime *rt, const void *cell);

/* Called by the collector after marking: drop entries whose shape or prototype died. */
void kz_shapes_sweep(JSRuntime *rt);
void kz_shapes_sweep(JSRuntime *rt) {
    if (!rt->root_cap) return;
    uint32_t live = 0;
    for (uint32_t i = 0; i < rt->root_cap; i++) {
        JSShape *s = rt->root_shapes[i];
        if (!s) continue;
        if (!kz_is_marked(rt, s) || !JV_IS_OBJECT(s->proto) || !kz_is_marked(rt, JV_PTR(s->proto))) rt->root_shapes[i] = NULL;
        else live++;
    }
    /* reinsert the survivors so probe chains have no gaps */
    JSShape **old = rt->root_shapes;
    uint32_t cap = rt->root_cap;
    JSShape **t = rt->sys_malloc(rt->sys_opaque, cap * sizeof(JSShape *));
    if (!t) {                       /* keep the table; clear it entirely (shapes are re-made on demand) */
        memset(old, 0, cap * sizeof(JSShape *));
        rt->root_count = 0;
        return;
    }
    memset(t, 0, cap * sizeof(JSShape *));
    for (uint32_t i = 0; i < cap; i++) {
        JSShape *s = old[i];
        if (!s) continue;
        uint32_t j = proto_hash(s->proto) & (cap - 1);
        while (t[j]) j = (j + 1) & (cap - 1);
        t[j] = s;
    }
    rt->sys_free(rt->sys_opaque, old);
    rt->root_shapes = t;
    rt->root_count = live;
}

/* The empty shape for objects with prototype `proto`.  Only weakly held: use it before allocating again, or root it. */
JSShape *kz_root_shape(JSContext *ctx, JSValue proto) {
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(proto)) {
        if (!rt->null_root) rt->null_root = shape_alloc(ctx, JS_NULL, 0, false);
        return rt->null_root;
    }
    if (rt->root_cap) {
        uint32_t m = rt->root_cap - 1;
        for (uint32_t j = proto_hash(proto) & m; rt->root_shapes[j]; j = (j + 1) & m)
            if (rt->root_shapes[j]->proto == proto) return rt->root_shapes[j];
    }
    if ((rt->root_count + 1) * 2 > rt->root_cap && !roots_grow(rt)) { kz_throw_oom(ctx); return NULL; }
    JSShape *s = shape_alloc(ctx, proto, 0, false);
    if (!s) return NULL;
    /* the allocation may have collected and rebuilt the table: insert into whatever it is now */
    if ((rt->root_count + 1) * 2 > rt->root_cap && !roots_grow(rt)) { kz_throw_oom(ctx); return NULL; }
    uint32_t m = rt->root_cap - 1, j = proto_hash(proto) & m;
    while (rt->root_shapes[j]) j = (j + 1) & m;
    rt->root_shapes[j] = s;
    rt->root_count++;
    return s;
}

JSShape *kz_shape_add(JSContext *ctx, JSShape *s, JSValue key, uint32_t attrs) {
    for (uint32_t i = 0; i < s->ntrans; i++)
        if (s->trans[i].key == key && s->trans[i].attrs == attrs) return s->trans[i].child;
    if (s->ntrans == s->trans_cap) {
        uint32_t nc = s->trans_cap ? s->trans_cap * 2 : 2;
        JSShapeTrans *t = kz_realloc(ctx, s->trans, nc * sizeof(JSShapeTrans));
        if (!t) return NULL;
        s->trans = t;
        s->trans_cap = nc;
    }
    uint32_t n = s->nprops + 1;
    JSShape *c = shape_alloc(ctx, s->proto, n, false);
    if (!c) return NULL;
    if (s->nprops) memcpy(c->props, s->props, s->nprops * sizeof(JSShapeProp));
    c->props[n - 1].key = key;
    c->props[n - 1].attrs = attrs;
    c->nprops = n;
    c->parent = s;
    c->h.flags |= s->h.flags & SHAPE_HAS_INDEX;
    if (KZ_KEY_IS_INDEX(key)) c->h.flags |= SHAPE_HAS_INDEX;
    shape_rehash(c);
    s->trans[s->ntrans].key = key;
    s->trans[s->ntrans].attrs = attrs;
    s->trans[s->ntrans].child = c;
    s->ntrans++;
    return c;
}

JSShape *kz_shape_build(JSContext *ctx, JSValue proto, const JSValue *keys, int n, uint32_t attrs) {
    KZ_ROOT(ctx, cur);
    JSShape *s = kz_root_shape(ctx, proto);
    if (!s) return NULL;
    cur = JV_MKPTR(s);
    for (int i = 0; i < n; i++) {
        s = kz_shape_add(ctx, (JSShape *)JV_PTR(cur), keys[i], attrs);
        if (!s) return NULL;
        cur = JV_MKPTR(s);
    }
    return s;
}

/* ================================================================== object allocation */

static size_t class_payload(KzClassId cls) {
    switch (cls) {
    case KZ_CLASS_ARRAY: return sizeof(((JSObject *)0)->u.array);
    case KZ_CLASS_FUNCTION: return sizeof(((JSObject *)0)->u.func);
    case KZ_CLASS_NATIVE: return sizeof(((JSObject *)0)->u.native);
    case KZ_CLASS_BOUND: return sizeof(((JSObject *)0)->u.bound);
    case KZ_CLASS_BOOLEAN: case KZ_CLASS_NUMBER: case KZ_CLASS_STRING: case KZ_CLASS_SYMBOL: case KZ_CLASS_BIGINT:
        return sizeof(JSValue);
    case KZ_CLASS_DATE: return sizeof(double);
    case KZ_CLASS_REGEXP: return sizeof(((JSObject *)0)->u.regexp);
    case KZ_CLASS_MAP: case KZ_CLASS_SET: case KZ_CLASS_WEAKMAP: case KZ_CLASS_WEAKSET: return sizeof(void *);
    case KZ_CLASS_PROXY: return sizeof(((JSObject *)0)->u.proxy);
    case KZ_CLASS_ARRAYBUFFER: case KZ_CLASS_SHAREDARRAYBUFFER: return sizeof(KzArrayBuffer);
    case KZ_CLASS_GENERATOR: case KZ_CLASS_ASYNC_GENERATOR: case KZ_CLASS_ASYNC_FUNCTION: return sizeof(void *);
    case KZ_CLASS_PROMISE: return sizeof(((JSObject *)0)->u.promise);
    case KZ_CLASS_MAPPED_ARGUMENTS: return sizeof(((JSObject *)0)->u.margs);
    case KZ_CLASS_WEAKREF: return sizeof(((JSObject *)0)->u.weakref);
    case KZ_CLASS_FINREG: return sizeof(((JSObject *)0)->u.finreg);
    case KZ_CLASS_HOST: return sizeof(((JSObject *)0)->u.host);
    case KZ_CLASS_MODULE_NS: return sizeof(((JSObject *)0)->u.ns);
    case KZ_CLASS_FOR_IN_ITERATOR: return sizeof(((JSObject *)0)->u.forin);
    case KZ_CLASS_ARRAY_ITERATOR: case KZ_CLASS_STRING_ITERATOR: case KZ_CLASS_MAP_ITERATOR: case KZ_CLASS_SET_ITERATOR:
    case KZ_CLASS_REGEXP_STRING_ITERATOR: case KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR: case KZ_CLASS_ITERATOR_HELPER:
        return sizeof(((JSObject *)0)->u.any);
    default:
        if (KZ_IS_TYPED_ARRAY(cls) || cls == KZ_CLASS_DATAVIEW) return sizeof(((JSObject *)0)->u.ta);
        return 0;
    }
}

JSObject *kz_obj_alloc(JSContext *ctx, KzClassId cls, JSShape *shape, size_t payload) {
    KZ_ROOT(ctx, sh);
    sh = JV_MKPTR(shape);
    JSValue *slots = NULL;
    uint32_t cap = 0;
    if (shape->nprops) {
        cap = shape->nprops;
        slots = kz_malloc(ctx, cap * sizeof(JSValue));
        if (!slots) return NULL;
        for (uint32_t i = 0; i < cap; i++) slots[i] = JS_UNDEFINED;
    }
    size_t pl = payload ? payload : class_payload(cls);
    JSObject *o = kz_gc_new(ctx, KZ_GC_OBJECT, KZ_OBJ_BASE + pl);
    if (!o) { kz_free(ctx->rt, slots); return NULL; }
    o->h.aux = cls;
    o->h.flags = OBJ_EXTENSIBLE;
    o->shape = (JSShape *)JV_PTR(sh);
    o->slots = slots;
    o->slot_cap = cap;
    if (kz_exotic[cls]) o->h.flags |= OBJ_EXOTIC;
    switch (cls) {               /* JSValue fields that must not read as 0.0 garbage... 0.0 is a valid value, but
                                    undefined is what the code expects for "unset" */
    case KZ_CLASS_FUNCTION:
        o->u.func.home = JS_UNDEFINED;
        o->u.func.fields = JS_UNDEFINED;
        break;
    case KZ_CLASS_BOUND:
        o->u.bound.target = o->u.bound.this_val = JS_UNDEFINED;
        break;
    case KZ_CLASS_BOOLEAN: case KZ_CLASS_NUMBER: case KZ_CLASS_STRING: case KZ_CLASS_SYMBOL: case KZ_CLASS_BIGINT:
        o->u.prim = JS_UNDEFINED;
        break;
    case KZ_CLASS_REGEXP:
        o->u.regexp.source = o->u.regexp.flags = JS_UNDEFINED;
        break;
    case KZ_CLASS_PROXY:
        o->u.proxy.target = o->u.proxy.handler = JS_NULL;
        break;
    case KZ_CLASS_PROMISE:
        o->u.promise.result = JS_UNDEFINED;
        break;
    case KZ_CLASS_WEAKREF:
        o->u.weakref.target = o->u.weakref.token = JS_UNDEFINED;
        break;
    case KZ_CLASS_FINREG:
        o->u.finreg.callback = JS_UNDEFINED;
        break;
    case KZ_CLASS_FOR_IN_ITERATOR:
        o->u.forin.obj = o->u.forin.keys = JS_UNDEFINED;
        break;
    case KZ_CLASS_ARRAY_ITERATOR: case KZ_CLASS_STRING_ITERATOR: case KZ_CLASS_MAP_ITERATOR: case KZ_CLASS_SET_ITERATOR:
    case KZ_CLASS_REGEXP_STRING_ITERATOR: case KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR: case KZ_CLASS_ITERATOR_HELPER:
        o->u.any.a = o->u.any.b = o->u.any.c = JS_UNDEFINED;
        break;
    default:
        if (KZ_IS_TYPED_ARRAY(cls) || cls == KZ_CLASS_DATAVIEW) o->u.ta.buffer = JS_UNDEFINED;
        break;
    }
    return o;
}

JSValue kz_new_object_class(JSContext *ctx, KzClassId cls, JSValue proto) {
    JSShape *s = kz_root_shape(ctx, proto);
    if (!s) return JS_EXCEPTION;
    JSObject *o = kz_obj_alloc(ctx, cls, s, 0);
    return o ? JV_MKOBJ(o) : JS_EXCEPTION;
}

JSValue kz_new_object_proto(JSContext *ctx, JSValue proto) { return kz_new_object_class(ctx, KZ_CLASS_OBJECT, proto); }

JSValue kz_new_object(JSContext *ctx) {
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_OBJECT, ctx->shape_object, 0);
    return o ? JV_MKOBJ(o) : JS_EXCEPTION;
}

JSValue kz_new_plain_with_shape(JSContext *ctx, JSShape *shape) {
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_OBJECT, shape, 0);
    return o ? JV_MKOBJ(o) : JS_EXCEPTION;
}

JSValue kz_new_array(JSContext *ctx, uint32_t len) {
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_ARRAY, ctx->shape_array, 0);
    if (!o) return JS_EXCEPTION;
    o->u.array.length = len;
    return JV_MKOBJ(o);
}

int kz_elems_reserve(JSContext *ctx, JSObject *o, uint32_t cap) {
    if (cap <= o->elem_cap) return 0;
    uint32_t nc = o->elem_cap ? o->elem_cap : 4;
    while (nc < cap) nc = nc > 0x7FFFFFFF ? cap : nc * 2;
    if (nc > 0x10000000u && cap <= 0x10000000u) nc = 0x10000000u;
    JSValue *e = kz_realloc(ctx, o->elems, (size_t)nc * sizeof(JSValue));
    if (!e) return -1;
    o->elems = e;
    o->elem_cap = nc;
    o->h.flags |= OBJ_HAS_ELEMS;
    return 0;
}

JSValue kz_new_array_from(JSContext *ctx, uint32_t n, const JSValue *vals) {
    /* vals must stay rooted by the caller */
    JSValue *e = NULL;
    if (n) {
        e = kz_malloc(ctx, (size_t)n * sizeof(JSValue));
        if (!e) return JS_EXCEPTION;
        memcpy(e, vals, (size_t)n * sizeof(JSValue));
    }
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_ARRAY, ctx->shape_array, 0);
    if (!o) { kz_free(ctx->rt, e); return JS_EXCEPTION; }
    o->elems = e;
    o->elem_len = o->elem_cap = n;
    o->u.array.length = n;
    if (n) o->h.flags |= OBJ_HAS_ELEMS;
    return JV_MKOBJ(o);
}

JSValue kz_proto_of(JSObject *o) { return o->shape->proto; }

static int ensure_slots(JSContext *ctx, JSObject *o, uint32_t n) {
    if (n <= o->slot_cap) return 0;
    uint32_t nc = o->slot_cap ? o->slot_cap * 2 : 4;
    while (nc < n) nc *= 2;
    JSValue *s = kz_realloc(ctx, o->slots, nc * sizeof(JSValue));
    if (!s) return -1;
    o->slots = s;
    o->slot_cap = nc;
    return 0;
}

/* ------------------------------------------------------------------ dictionary mode */

static int to_dict(JSContext *ctx, JSObject *o) {
    JSShape *s = o->shape;
    if (s->h.flags & SHAPE_DICT) return 0;
    uint32_t cap = s->nprops < 8 ? 8 : s->nprops + s->nprops / 2;
    JSShape *d = shape_alloc(ctx, o->shape->proto, cap, true);
    if (!d) return -1;
    s = o->shape;
    if (s->nprops) memcpy(d->props, s->props, s->nprops * sizeof(JSShapeProp));
    d->nprops = s->nprops;
    d->h.flags |= s->h.flags & SHAPE_HAS_INDEX;
    shape_rehash(d);
    o->shape = d;
    return 0;
}

static int dict_grow(JSContext *ctx, JSShape *d) {
    uint32_t nc = d->prop_cap ? d->prop_cap * 2 : 8;
    JSShapeProp *p = kz_realloc(ctx, d->props, nc * sizeof(JSShapeProp));
    if (!p) return -1;
    d->props = p;
    d->prop_cap = nc;
    uint32_t hs = pow2_at_least(nc * 2);
    if (hs != d->hash_size) {
        uint32_t *h = kz_malloc(ctx, hs * sizeof(uint32_t));
        if (!h) return -1;
        kz_free(ctx->rt, d->hash);
        d->hash = h;
        d->hash_size = hs;
        shape_rehash(d);
    }
    return 0;
}

/* removes tombstones, moving slot values along */
static void dict_compact(JSObject *o) {
    JSShape *d = o->shape;
    uint32_t w = 0;
    for (uint32_t i = 0; i < d->nprops; i++) {
        if (d->props[i].key == JS_EMPTY) continue;
        d->props[w] = d->props[i];
        o->slots[w] = o->slots[i];
        w++;
    }
    d->nprops = w;
    d->ndeleted = 0;
    shape_rehash(d);
    d->version++;
}

/* ------------------------------------------------------------------ adding and removing shape properties */

int kz_obj_add_prop(JSContext *ctx, JSObject *o, JSValue key, uint32_t attrs, JSValue value) {
    JSShape *s = o->shape;
    if (!(s->h.flags & SHAPE_DICT) && s->nprops >= DICT_THRESHOLD) {
        if (to_dict(ctx, o)) return -1;
        s = o->shape;
    }
    if (s->h.flags & SHAPE_DICT) {
        if (s->nprops == s->prop_cap && dict_grow(ctx, s)) return -1;
        if (ensure_slots(ctx, o, s->nprops + 1)) return -1;
        s = o->shape;
        uint32_t i = s->nprops++;
        s->props[i].key = key;
        s->props[i].attrs = attrs;
        uint32_t h = key_hash(key) & (s->hash_size - 1);
        s->props[i].hnext = s->hash[h];
        s->hash[h] = i + 1;
        if (KZ_KEY_IS_INDEX(key)) s->h.flags |= SHAPE_HAS_INDEX;
        s->version++;
        o->slots[i] = value;
        return 0;
    }
    if (ensure_slots(ctx, o, s->nprops + 1)) return -1;
    JSShape *c = kz_shape_add(ctx, o->shape, key, attrs);
    if (!c) return -1;
    o->slots[c->nprops - 1] = value;
    o->shape = c;
    return 0;
}

static int remove_prop(JSContext *ctx, JSObject *o, int idx) {
    JSShape *s = o->shape;
    if (!(s->h.flags & SHAPE_DICT)) {
        if ((uint32_t)idx == s->nprops - 1 && s->parent && s->parent->nprops == s->nprops - 1) {
            o->slots[idx] = JS_UNDEFINED;
            o->shape = s->parent;
            return 0;
        }
        if (to_dict(ctx, o)) return -1;
        s = o->shape;
    }
    /* unlink from the hash chain */
    uint32_t h = key_hash(s->props[idx].key) & (s->hash_size - 1);
    uint32_t *link = &s->hash[h];
    while (*link && *link != (uint32_t)idx + 1) link = &s->props[*link - 1].hnext;
    if (*link) *link = s->props[idx].hnext;
    s->props[idx].key = JS_EMPTY;
    s->props[idx].attrs = 0;
    o->slots[idx] = JS_UNDEFINED;
    s->ndeleted++;
    s->version++;
    if ((uint32_t)idx == s->nprops - 1) { s->nprops--; s->ndeleted--; }
    if (s->ndeleted > 8 && s->ndeleted * 2 > s->nprops) dict_compact(o);
    return 0;
}

/* changes the attributes (and maybe the value) of an existing shape property */
static int change_prop(JSContext *ctx, JSObject *o, int idx, uint32_t attrs) {
    if (o->shape->props[idx].attrs == attrs) return 0;
    if (!(o->shape->h.flags & SHAPE_DICT) && to_dict(ctx, o)) return -1;
    o->shape->props[idx].attrs = attrs;
    o->shape->version++;
    return 0;
}

int kz_obj_set_proto_raw(JSContext *ctx, JSObject *o, JSValue proto) {
    JSShape *s = o->shape;
    if (s->proto == proto) return 0;
    if (s->nprops == 0 && !(s->h.flags & SHAPE_DICT)) {
        KZ_ROOT(ctx, p);
        p = proto;
        JSShape *r = kz_root_shape(ctx, p);
        if (!r) return -1;
        o->shape = r;
        return 0;
    }
    if (to_dict(ctx, o)) return -1;
    o->shape->proto = proto;
    o->shape->version++;
    if (JV_IS_OBJECT(proto)) JV_OBJ(proto)->h.flags |= OBJ_IS_PROTO;
    return 0;
}

/* ================================================================== elements */

static inline uint32_t elem_attrs(const JSObject *o) {
    if (o->h.flags & OBJ_ELEMS_FROZEN) return KZ_PROP_E;
    if (o->h.flags & OBJ_ELEMS_SEALED) return KZ_PROP_E | KZ_PROP_W;
    return KZ_PROP_WEC;
}

/* Can index idx be stored as an element of o (a plain data property with the uniform attributes)? */
static bool elem_ok(const JSObject *o, uint32_t idx) {
    if (idx < o->elem_len) return true;
    if (idx > KZ_MAX_INDEX) return false;
    /* keep the vector at least half full (small vectors may have up to 128 holes) */
    uint32_t limit = o->elem_len < 64 ? 128 : (o->elem_len > 0x3FFFFFFF ? 0x7FFFFFFF : o->elem_len * 2);
    return idx <= limit;
}


/* ================================================================== array length */

static int compare_desc_index(const void *a, const void *b) {
    uint32_t x = KZ_KEY_INDEX(*(const JSValue *)a), y = KZ_KEY_INDEX(*(const JSValue *)b);
    return x < y ? 1 : x > y ? -1 : 0;
}

/* ArraySetLength's deletion part: returns 1 when every index >= len went, 0 when a non-configurable one stopped it
   (then the length is one past it). */
int kz_array_set_length(JSContext *ctx, JSObject *a, uint32_t len) {
    uint32_t old = a->u.array.length;
    if (len >= old) { a->u.array.length = len; return 1; }
    /* shape indices first (they may be above the elements), in descending order */
    if (a->shape->h.flags & SHAPE_HAS_INDEX) {
        KZ_ROOT_VEC(ctx, keys);
        JSShape *s = a->shape;
        for (uint32_t i = 0; i < s->nprops; i++) {
            JSValue k = s->props[i].key;
            if (KZ_KEY_IS_INDEX(k) && KZ_KEY_INDEX(k) >= len && kz_vec_push(ctx, &keys, k)) return -1;
        }
        if (keys.len) {
            qsort(keys.v, keys.len, sizeof(JSValue), compare_desc_index);
            for (uint32_t i = 0; i < keys.len; i++) {
                int idx = kz_shape_find(a->shape, keys.v[i]);
                if (idx < 0) continue;
                if (!(a->shape->props[idx].attrs & KZ_PROP_C)) {
                    uint32_t stop = KZ_KEY_INDEX(keys.v[i]) + 1;
                    if (a->elem_len > stop) a->elem_len = stop;
                    a->u.array.length = stop;
                    return 0;
                }
                if (remove_prop(ctx, a, idx)) return -1;
            }
        }
    }
    if (a->elem_len > len) {
        if (a->h.flags & OBJ_ELEMS_SEALED) {
            /* non-configurable elements: the highest present one stops the deletion */
            for (uint32_t i = a->elem_len; i-- > len;)
                if (a->elems[i] != JS_HOLE) {
                    a->u.array.length = i + 1;
                    return 0;
                }
        }
        a->elem_len = len;
        if (len < a->elem_cap / 4 && a->elem_cap > 64) {
            JSValue *e = kz_rt_realloc(ctx->rt, a->elems, (len < 16 ? 16 : len) * sizeof(JSValue));
            if (e) { a->elems = e; a->elem_cap = len < 16 ? 16 : len; }
        }
    }
    a->u.array.length = len;
    return 1;
}

bool kz_array_is_fast(JSObject *a) {
    return kz_class(a) == KZ_CLASS_ARRAY && !(a->shape->h.flags & SHAPE_HAS_INDEX) &&
           (a->h.flags & (OBJ_EXTENSIBLE | OBJ_ELEMS_SEALED | OBJ_ELEMS_FROZEN | OBJ_LEN_RO)) == OBJ_EXTENSIBLE &&
           a->elem_len == a->u.array.length;
}

int kz_array_push(JSContext *ctx, JSObject *a, JSValue v) {
    uint32_t n = a->u.array.length;
    if (a->elem_len == n && n < 0xFFFFFFFEu && (a->h.flags & (OBJ_EXTENSIBLE | OBJ_ELEMS_FROZEN | OBJ_LEN_RO)) == OBJ_EXTENSIBLE &&
        !(a->shape->h.flags & SHAPE_HAS_INDEX)) {
        if (n == a->elem_cap) {
            KZ_ROOT(ctx, rv);
            rv = v;
            if (kz_elems_reserve(ctx, a, n + 1)) return -1;
            v = rv;
        }
        a->elems[n] = v;
        a->elem_len = n + 1;
        a->u.array.length = n + 1;
        return 0;
    }
    return kz_set_index(ctx, JV_MKOBJ(a), n, v, true) < 0 ? -1 : 0;
}

/* ================================================================== [[GetOwnProperty]] */

static int ordinary_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    if (KZ_KEY_IS_INDEX(key)) {
        uint32_t idx = KZ_KEY_INDEX(key);
        if (idx < o->elem_len && o->elems[idx] != JS_HOLE) {
            d->value = o->elems[idx];
            d->getter = d->setter = JS_UNDEFINED;
            d->flags = elem_attrs(o) | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
            return 1;
        }
        if (!(o->shape->h.flags & SHAPE_HAS_INDEX)) return 0;
    } else if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length]) {
        d->value = jv_uint32(o->u.array.length);
        d->getter = d->setter = JS_UNDEFINED;
        d->flags = ((o->h.flags & OBJ_LEN_RO) ? 0 : KZ_PROP_W) | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
        return 1;
    }
    int i = kz_shape_find(o->shape, key);
    if (i < 0) return 0;
    uint32_t a = o->shape->props[i].attrs;
    if (a & KZ_PROP_ACCESSOR) {
        JSAccessor *acc = JV_PTR(o->slots[i]);
        d->value = JS_UNDEFINED;
        d->getter = acc->getter;
        d->setter = acc->setter;
        d->flags = (a & (KZ_PROP_E | KZ_PROP_C)) | PD_ACCESSOR | PD_HAS_GET | PD_HAS_SET | PD_HAS_E | PD_HAS_C;
    } else {
        d->value = o->slots[i];
        d->getter = d->setter = JS_UNDEFINED;
        d->flags = (a & KZ_PROP_WEC) | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
    }
    return 1;
}

int kz_get_own_property(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->get_own) {
            int r = x->get_own(ctx, o, key, d);
            if (r != KZ_FALLBACK) return r;
        }
    }
    return ordinary_get_own(ctx, o, key, d);
}

/* ================================================================== [[DefineOwnProperty]] */

void kz_complete_descriptor(KzPropDesc *d) {
    if (!(d->flags & (PD_HAS_GET | PD_HAS_SET))) {
        if (!(d->flags & PD_HAS_VALUE)) { d->value = JS_UNDEFINED; d->flags |= PD_HAS_VALUE; }
        if (!(d->flags & PD_HAS_W)) d->flags |= PD_HAS_W;
    } else {
        if (!(d->flags & PD_HAS_GET)) { d->getter = JS_UNDEFINED; d->flags |= PD_HAS_GET; }
        if (!(d->flags & PD_HAS_SET)) { d->setter = JS_UNDEFINED; d->flags |= PD_HAS_SET; }
        d->flags |= PD_ACCESSOR;
    }
    d->flags |= PD_HAS_E | PD_HAS_C;
}

static bool is_accessor_desc(const KzPropDesc *d) { return d->flags & (PD_HAS_GET | PD_HAS_SET); }
static bool is_data_desc(const KzPropDesc *d) { return d->flags & (PD_HAS_VALUE | PD_HAS_W); }

static JSValue reject(JSContext *ctx, bool throw_err, JSValue key, const char *what) {
    if (!throw_err) return JS_FALSE;
    KZ_ROOT(ctx, name);
    name = kz_key_debug_name(ctx, key);
    if (name == JS_EXCEPTION) return JS_EXCEPTION;
    char buf[128];
    kz_str_to_utf8_buf(JV_STR(kz_str_flatten(ctx, name) == JS_EXCEPTION ? ctx->rt->atom[KZ_ATOM_empty] : name), buf, sizeof buf);
    kz_throw_type(ctx, what, buf);
    return JS_EXCEPTION;
}

static int refuse(JSContext *ctx, bool throw_err, JSValue key, const char *what) {
    return reject(ctx, throw_err, key, what) == JS_EXCEPTION ? -1 : 0;
}

/* stores a value + attributes for a key that is known to be absent */
static int add_own(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d) {
    uint32_t attrs = d->flags & KZ_PROP_WEC;
    if (is_accessor_desc(d)) {
        JSAccessor *acc = kz_gc_new(ctx, KZ_GC_ACCESSOR, sizeof(JSAccessor));
        if (!acc) return -1;
        acc->getter = (d->flags & PD_HAS_GET) ? d->getter : JS_UNDEFINED;
        acc->setter = (d->flags & PD_HAS_SET) ? d->setter : JS_UNDEFINED;
        KZ_ROOT(ctx, ra);
        ra = JV_MKPTR(acc);
        return kz_obj_add_prop(ctx, o, key, (attrs & ~KZ_PROP_W) | KZ_PROP_ACCESSOR, ra);
    }
    JSValue v = (d->flags & PD_HAS_VALUE) ? d->value : JS_UNDEFINED;
    if (KZ_KEY_IS_INDEX(key)) {
        uint32_t idx = KZ_KEY_INDEX(key);
        if (attrs == elem_attrs(o) && elem_ok(o, idx) && !(o->h.flags & OBJ_ELEMS_SEALED)) {
            if (idx >= o->elem_len) {
                if (idx >= o->elem_cap && kz_elems_reserve(ctx, o, idx + 1)) return -1;
                for (uint32_t i = o->elem_len; i < idx; i++) o->elems[i] = JS_HOLE;
                o->elem_len = idx + 1;
            }
            o->elems[idx] = (d->flags & PD_HAS_VALUE) ? d->value : JS_UNDEFINED;
            return 0;
        }
    }
    return kz_obj_add_prop(ctx, o, key, attrs, v);
}

/* ValidateAndApplyPropertyDescriptor for an ordinary object (with array index/length rules added by the caller) */
static int ordinary_define(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d, bool throw_err) {
    KZ_ROOT_DESC(ctx, cur);
    int has = ordinary_get_own(ctx, o, key, &cur);
    if (!has) {
        if (!(o->h.flags & OBJ_EXTENSIBLE)) return refuse(ctx, throw_err, key, "Cannot define property %s, object is not extensible");
        KzPropDesc full = *d;
        if (!is_accessor_desc(&full)) { if (!(full.flags & PD_HAS_VALUE)) full.value = JS_UNDEFINED; }
        return add_own(ctx, o, key, &full) ? -1 : 1;
    }
    bool configurable = cur.flags & KZ_PROP_C;
    if (!configurable) {
        if ((d->flags & PD_HAS_C) && (d->flags & KZ_PROP_C)) goto bad;
        if ((d->flags & PD_HAS_E) && ((d->flags & KZ_PROP_E) != (cur.flags & KZ_PROP_E))) goto bad;
        bool cur_acc = cur.flags & PD_ACCESSOR;
        if (is_accessor_desc(d) || is_data_desc(d)) {
            if (is_accessor_desc(d) != cur_acc) goto bad;
            if (cur_acc) {
                if ((d->flags & PD_HAS_GET) && d->getter != cur.getter) goto bad;
                if ((d->flags & PD_HAS_SET) && d->setter != cur.setter) goto bad;
            } else if (!(cur.flags & KZ_PROP_W)) {
                if ((d->flags & PD_HAS_W) && (d->flags & KZ_PROP_W)) goto bad;
                if (d->flags & PD_HAS_VALUE) {
                    int same = kz_same_value(ctx, d->value, cur.value);
                    if (same < 0) return -1;
                    if (!same) goto bad;
                }
            }
        }
    }
    /* apply */
    {
        uint32_t na = cur.flags & KZ_PROP_WEC;
        if (d->flags & PD_HAS_E) na = (na & ~KZ_PROP_E) | (d->flags & KZ_PROP_E);
        if (d->flags & PD_HAS_C) na = (na & ~KZ_PROP_C) | (d->flags & KZ_PROP_C);
        bool to_acc = is_accessor_desc(d), to_data = is_data_desc(d), was_acc = cur.flags & PD_ACCESSOR;
        bool acc = was_acc;
        if (to_acc) acc = true;
        else if (to_data) acc = false;
        if (!acc) {
            if (d->flags & PD_HAS_W) na = (na & ~KZ_PROP_W) | (d->flags & KZ_PROP_W);
            else if (was_acc) na &= ~KZ_PROP_W;
        }
        if (KZ_KEY_IS_INDEX(key)) {
            uint32_t idx = KZ_KEY_INDEX(key);
            if (idx < o->elem_len && o->elems[idx] != JS_HOLE) {
                if (!acc && na == elem_attrs(o)) {
                    if (d->flags & PD_HAS_VALUE) o->elems[idx] = d->value;
                    return 1;
                }
                /* the element needs attributes of its own: move it into the shape */
                KZ_ROOT(ctx, old);
                old = o->elems[idx];
                o->elems[idx] = JS_HOLE;
                if (idx == o->elem_len - 1) o->elem_len--;
                KzPropDesc nd;
                nd.value = (d->flags & PD_HAS_VALUE) ? d->value : old;
                nd.getter = (d->flags & PD_HAS_GET) ? d->getter : JS_UNDEFINED;
                nd.setter = (d->flags & PD_HAS_SET) ? d->setter : JS_UNDEFINED;
                nd.flags = na | (acc ? (PD_HAS_GET | PD_HAS_SET) : PD_HAS_VALUE);
                /* adding to the shape directly (add_own would put it back into the elements) */
                if (acc) {
                    if (add_own(ctx, o, key, &nd)) return -1;
                } else if (kz_obj_add_prop(ctx, o, key, na, nd.value)) return -1;
                return 1;
            }
        }
        if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length]) return 1;   /* handled by caller */
        int i = kz_shape_find(o->shape, key);
        if (i < 0) return 1;
        if (acc) {
            JSAccessor *a;
            if (was_acc) a = JV_PTR(o->slots[i]);
            else {
                a = kz_gc_new(ctx, KZ_GC_ACCESSOR, sizeof(JSAccessor));
                if (!a) return -1;
                a->getter = a->setter = JS_UNDEFINED;
                i = kz_shape_find(o->shape, key);
                o->slots[i] = JV_MKPTR(a);
            }
            if (d->flags & PD_HAS_GET) a->getter = d->getter;
            if (d->flags & PD_HAS_SET) a->setter = d->setter;
            if (change_prop(ctx, o, i, (na & ~KZ_PROP_W) | KZ_PROP_ACCESSOR)) return -1;
        } else {
            if (was_acc) o->slots[i] = JS_UNDEFINED;
            if (d->flags & PD_HAS_VALUE) o->slots[i] = d->value;
            if (change_prop(ctx, o, i, na)) return -1;
        }
        return 1;
    }
bad:
    return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
}

static int array_define(JSContext *ctx, JSObject *a, JSValue key, const KzPropDesc *d, bool throw_err) {
    JSRuntime *rt = ctx->rt;
    if (key == rt->atom[KZ_ATOM_length]) {
        /* ArraySetLength */
        if (is_accessor_desc(d)) return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
        if ((d->flags & PD_HAS_C) && (d->flags & KZ_PROP_C)) return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
        if ((d->flags & PD_HAS_E) && (d->flags & KZ_PROP_E)) return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
        bool ro = a->h.flags & OBJ_LEN_RO;
        if (!(d->flags & PD_HAS_VALUE)) {
            if ((d->flags & PD_HAS_W) && (d->flags & KZ_PROP_W) && ro)
                return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
            if ((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W)) a->h.flags |= OBJ_LEN_RO;
            return 1;
        }
        /* the caller converted the value already (ToUint32 checks happen in kz_define_own_property) */
        double dl = jv_num(d->value);
        uint32_t nl = (uint32_t)dl;
        if (nl == a->u.array.length) {
            if ((d->flags & PD_HAS_W) && (d->flags & KZ_PROP_W) && ro) return refuse(ctx, throw_err, key, "Cannot redefine property: %s");
            if ((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W)) a->h.flags |= OBJ_LEN_RO;
            return 1;
        }
        if (ro) return refuse(ctx, throw_err, key, "Cannot assign to read only property '%s' of array");
        int r = kz_array_set_length(ctx, a, nl);
        if (r < 0) return -1;
        if ((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W)) a->h.flags |= OBJ_LEN_RO;
        if (!r) return refuse(ctx, throw_err, key, "Cannot delete array element while setting %s");
        return 1;
    }
    if (KZ_KEY_IS_INDEX(key)) {
        uint32_t idx = KZ_KEY_INDEX(key);
        if (idx >= a->u.array.length && (a->h.flags & OBJ_LEN_RO))
            return refuse(ctx, throw_err, key, "Cannot add property %s, array length is not writable");
        int r = ordinary_define(ctx, a, key, d, throw_err);
        if (r <= 0) return r;
        if (idx >= a->u.array.length) a->u.array.length = idx + 1;
        return 1;
    }
    return ordinary_define(ctx, a, key, d, throw_err);
}

int kz_define_own_property(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d, bool throw_err) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->define_own) {
            int r = x->define_own(ctx, o, key, d);
            if (r != KZ_FALLBACK) {
                if (r == 0 && throw_err) return refuse(ctx, true, key, "Cannot define property %s");
                return r;
            }
        }
    }
    if (kz_class(o) == KZ_CLASS_ARRAY) {
        if (key == ctx->rt->atom[KZ_ATOM_length] && (d->flags & PD_HAS_VALUE)) {
            /* ArraySetLength steps 3-5: ToUint32 and ToNumber must agree */
            KzPropDesc nd = *d;
            KZ_ROOTS(ctx, r, 1);
            r[0] = d->value;
            uint32_t len;
            double num;
            if (kz_to_uint32(ctx, r[0], &len)) return -1;
            if (kz_to_number(ctx, r[0], &num)) return -1;
            if ((double)len != num) { kz_throw_range(ctx, "Invalid array length"); return -1; }
            nd.value = jv_uint32(len);
            return array_define(ctx, o, key, &nd, throw_err);
        }
        return array_define(ctx, o, key, d, throw_err);
    }
    return ordinary_define(ctx, o, key, d, throw_err);
}

int kz_define_value(JSContext *ctx, JSValue obj, JSValue key, JSValue val, uint32_t attrs) {
    KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, (attrs & KZ_PROP_WEC) | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
    return kz_define_own_property(ctx, JV_OBJ(obj), key, &d, true) < 0 ? -1 : 0;
}

int kz_define_value_atom(JSContext *ctx, JSValue obj, int atom, JSValue val, uint32_t attrs) {
    return kz_define_value(ctx, obj, ctx->rt->atom[atom], val, attrs);
}

int kz_define_accessor(JSContext *ctx, JSValue obj, JSValue key, JSValue getter, JSValue setter, uint32_t attrs) {
    KzPropDesc d = { JS_UNDEFINED, getter, setter, (attrs & (KZ_PROP_E | KZ_PROP_C)) | PD_HAS_GET | PD_HAS_SET | PD_HAS_E | PD_HAS_C };
    return kz_define_own_property(ctx, JV_OBJ(obj), key, &d, true) < 0 ? -1 : 0;
}

int kz_create_data_property(JSContext *ctx, JSValue obj, JSValue key, JSValue val) {
    JSObject *o = JV_OBJ(obj);
    /* fast path: a new element at the end of a fast array, or a new string key on an ordinary extensible object */
    if (kz_class(o) == KZ_CLASS_ARRAY && KZ_KEY_IS_INDEX(key) && KZ_KEY_INDEX(key) == o->elem_len &&
        o->elem_len == o->u.array.length && !(o->h.flags & OBJ_EXOTIC) && kz_array_is_fast(o)) {
        uint32_t n = o->elem_len;
        if (n == o->elem_cap) {
            KZ_ROOT(ctx, rv);
            rv = val;
            if (kz_elems_reserve(ctx, o, n + 1)) return -1;
            val = rv;
        }
        o->elems[n] = val;
        o->elem_len = n + 1;
        o->u.array.length = n + 1;
        return 0;
    }
    KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
    int r = kz_define_own_property(ctx, o, key, &d, true);
    return r < 0 ? -1 : 0;
}

/* ================================================================== [[HasProperty]] */

int kz_has_property(JSContext *ctx, JSObject *o, JSValue key) {
    for (int depth = 0;; depth++) {
        if (depth > 100000) { kz_throw_range(ctx, "prototype chain too long"); return -1; }
        if (o->h.flags & OBJ_EXOTIC) {
            const KzExotic *x = kz_exotic[kz_class(o)];
            if (x && x->has) {
                int r = x->has(ctx, o, key);
                if (r != KZ_FALLBACK) return r;
            } else if (x && x->get_own) {
                KZ_ROOT_DESC(ctx, d);
                int r = x->get_own(ctx, o, key, &d);
                if (r < 0) return -1;
                if (r == 1) return 1;
            }
        }
        if (KZ_KEY_IS_INDEX(key)) {
            uint32_t idx = KZ_KEY_INDEX(key);
            if (idx < o->elem_len && o->elems[idx] != JS_HOLE) return 1;
            if ((o->shape->h.flags & SHAPE_HAS_INDEX) && kz_shape_find(o->shape, key) >= 0) return 1;
        } else {
            if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length]) return 1;
            if (kz_shape_find(o->shape, key) >= 0) return 1;
        }
        JSValue p;
        if (o->h.flags & OBJ_EXOTIC) {
            if (kz_get_proto(ctx, o, &p)) return -1;
        } else p = o->shape->proto;
        if (!JV_IS_OBJECT(p)) return 0;
        o = JV_OBJ(p);
    }
}

/* ================================================================== [[Get]] */

static JSValue get_primitive_proto(JSContext *ctx, JSValue v, JSValue key) {
    switch (JV_TAG(v)) {
    case JV_TAG_STRING: return kz_intr(ctx, KZ_I_STRING_PROTO);
    case JV_TAG_SYMBOL: return kz_intr(ctx, KZ_I_SYMBOL_PROTO);
    case JV_TAG_BIGINT: return kz_intr(ctx, KZ_I_BIGINT_PROTO);
    case JV_TAG_SPECIAL:
        if (JV_IS_BOOL(v)) return kz_intr(ctx, KZ_I_BOOLEAN_PROTO);
        {
            char what[96];
            KZ_ROOT(ctx, name);
            name = kz_key_debug_name(ctx, key);
            if (name == JS_EXCEPTION) return JS_EXCEPTION;
            if (kz_str_flatten(ctx, name) == JS_EXCEPTION) return JS_EXCEPTION;
            kz_str_to_utf8_buf(JV_STR(name), what, sizeof what);
            return kz_throw_type(ctx, "Cannot read properties of %s (reading '%s')", v == JS_NULL ? "null" : "undefined", what);
        }
    default:
        return kz_intr(ctx, KZ_I_NUMBER_PROTO);
    }
}

JSValue kz_get(JSContext *ctx, JSValue obj, JSValue key, JSValue receiver) {
    JSObject *o;
    if (!JV_IS_OBJECT(obj)) {
        if (JV_IS_STRING(obj)) {
            JSString *s = JV_STR(obj);
            if (KZ_KEY_IS_INDEX(key)) {
                uint32_t idx = KZ_KEY_INDEX(key);
                if (idx < kz_str_len(s)) {
                    s = kz_str_flat(ctx, s);
                    if (!s) return JS_EXCEPTION;
                    uint32_t c = kz_str_at(s, idx);
                    if (c < 256) return ctx->rt->char_str[c];
                    return kz_str_sub(ctx, obj, idx, idx + 1);
                }
            } else if (key == ctx->rt->atom[KZ_ATOM_length]) return JV_MKINT((int32_t)kz_str_len(s));
        }
        JSValue p = get_primitive_proto(ctx, obj, key);
        if (p == JS_EXCEPTION) return p;
        o = JV_OBJ(p);
    } else o = JV_OBJ(obj);
    for (int depth = 0;; depth++) {
        if (o->h.flags & OBJ_EXOTIC) {
            if (depth > 100000) return kz_throw_range(ctx, "prototype chain too long");
            const KzExotic *x = kz_exotic[kz_class(o)];
            if (x && x->get) {
                JSValue out = JS_UNDEFINED;
                int r = x->get(ctx, o, key, receiver, &out);
                if (r < 0) return JS_EXCEPTION;
                if (r == 1) return out;
            } else if (x && x->get_own) {
                KZ_ROOT_DESC(ctx, d);
                int r = x->get_own(ctx, o, key, &d);
                if (r < 0) return JS_EXCEPTION;
                if (r == 1) {
                    if (d.flags & PD_ACCESSOR) {
                        if (!JV_IS_OBJECT(d.getter)) return JS_UNDEFINED;
                        return kz_call(ctx, d.getter, receiver, 0, NULL);
                    }
                    return d.value;
                }
            }
        }
        if (KZ_KEY_IS_INDEX(key)) {
            uint32_t idx = KZ_KEY_INDEX(key);
            if (idx < o->elem_len && o->elems[idx] != JS_HOLE) return o->elems[idx];
            if (!(o->shape->h.flags & SHAPE_HAS_INDEX)) goto next;
        } else if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length])
            return jv_uint32(o->u.array.length);
        {
            int i = kz_shape_find(o->shape, key);
            if (i >= 0) {
                if (o->shape->props[i].attrs & KZ_PROP_ACCESSOR) {
                    JSValue g = ((JSAccessor *)JV_PTR(o->slots[i]))->getter;
                    if (!JV_IS_OBJECT(g)) return JS_UNDEFINED;
                    return kz_call(ctx, g, receiver, 0, NULL);
                }
                return o->slots[i];
            }
        }
    next:;
        JSValue p;
        if (o->h.flags & OBJ_EXOTIC) {
            if (kz_get_proto(ctx, o, &p)) return JS_EXCEPTION;
        } else p = o->shape->proto;
        if (!JV_IS_OBJECT(p)) return JS_UNDEFINED;
        o = JV_OBJ(p);
    }
}

JSValue kz_get_prop(JSContext *ctx, JSValue obj, JSValue key) { return kz_get(ctx, obj, key, obj); }
JSValue kz_get_atom(JSContext *ctx, JSValue obj, int atom) { return kz_get(ctx, obj, ctx->rt->atom[atom], obj); }
JSValue kz_get_index(JSContext *ctx, JSValue obj, uint32_t idx) {
    if (JV_IS_OBJECT(obj)) {
        JSObject *o = JV_OBJ(obj);
        if (idx < o->elem_len && o->elems[idx] != JS_HOLE && !(o->h.flags & OBJ_EXOTIC)) return o->elems[idx];
    }
    if (idx > KZ_MAX_INDEX) {
        KZ_ROOT(ctx, k);
        k = kz_key_from_value(ctx, jv_from_dbl((double)idx));
        if (k == JS_EXCEPTION) return k;
        return kz_get(ctx, obj, k, obj);
    }
    return kz_get(ctx, obj, KZ_KEY_FROM_INDEX(idx), obj);
}

/* ================================================================== [[Set]] */

static int set_fail(JSContext *ctx, bool throw_err, JSValue key, const char *msg) { return refuse(ctx, throw_err, key, msg); }

/* OrdinarySetWithOwnDescriptor's receiver part: the property was not found as a setter or a read-only data property
   on the way up; write it to the receiver. */
static int set_on_receiver(JSContext *ctx, JSValue receiver, JSValue key, JSValue val, bool throw_err) {
    if (!JV_IS_OBJECT(receiver)) return set_fail(ctx, throw_err, key, "Cannot create property '%s' on primitive");
    JSObject *r = JV_OBJ(receiver);
    KZ_ROOT_DESC(ctx, ex);
    int has = kz_get_own_property(ctx, r, key, &ex);
    if (has < 0) return -1;
    if (has) {
        if (ex.flags & PD_ACCESSOR) return set_fail(ctx, throw_err, key, "Cannot set property %s which has only a getter");
        if (!(ex.flags & KZ_PROP_W)) return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s'");
        KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, PD_HAS_VALUE };
        return kz_define_own_property(ctx, r, key, &d, throw_err);
    }
    KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
    return kz_define_own_property(ctx, r, key, &d, throw_err);
}

int kz_set(JSContext *ctx, JSValue obj, JSValue key, JSValue val, JSValue receiver, bool throw_err) {
    JSObject *o;
    if (!JV_IS_OBJECT(obj)) {
        if (JV_IS_NULLISH(obj)) {
            char what[96];
            KZ_ROOT(ctx, name);
            name = kz_key_debug_name(ctx, key);
            if (name == JS_EXCEPTION || kz_str_flatten(ctx, name) == JS_EXCEPTION) return -1;
            kz_str_to_utf8_buf(JV_STR(name), what, sizeof what);
            kz_throw_type(ctx, "Cannot set properties of %s (setting '%s')", obj == JS_NULL ? "null" : "undefined", what);
            return -1;
        }
        if (JV_IS_STRING(obj)) {
            if ((KZ_KEY_IS_INDEX(key) && KZ_KEY_INDEX(key) < kz_str_len(JV_STR(obj))) || key == ctx->rt->atom[KZ_ATOM_length])
                return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s' of string");
        }
        JSValue p = get_primitive_proto(ctx, obj, key);
        if (p == JS_EXCEPTION) return -1;
        o = JV_OBJ(p);
    } else o = JV_OBJ(obj);
    for (int depth = 0;; depth++) {
        if (o->h.flags & OBJ_EXOTIC) {
            if (depth > 100000) { kz_throw_range(ctx, "prototype chain too long"); return -1; }
            const KzExotic *x = kz_exotic[kz_class(o)];
            if (x && x->set) {
                int r = x->set(ctx, o, key, val, receiver);
                if (r != KZ_FALLBACK) {
                    if (r == 0 && throw_err) return set_fail(ctx, true, key, "Cannot assign to property '%s'");
                    return r;
                }
            } else if (x && x->get_own) {
                KZ_ROOT_DESC(ctx, d);
                int r = x->get_own(ctx, o, key, &d);
                if (r < 0) return -1;
                if (r == 1) {
                    if (d.flags & PD_ACCESSOR) {
                        if (!JV_IS_OBJECT(d.setter)) return set_fail(ctx, throw_err, key, "Cannot set property %s which has only a getter");
                        JSValue args[1] = { val };
                        JSValue res = kz_call(ctx, d.setter, receiver, 1, args);
                        return res == JS_EXCEPTION ? -1 : 1;
                    }
                    if (!(d.flags & KZ_PROP_W)) return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s'");
                    return set_on_receiver(ctx, receiver, key, val, throw_err) < 0 ? -1 : 1;
                }
            }
        }
        if (KZ_KEY_IS_INDEX(key)) {
            uint32_t idx = KZ_KEY_INDEX(key);
            if (idx < o->elem_len && o->elems[idx] != JS_HOLE) {
                if (o->h.flags & OBJ_ELEMS_FROZEN) return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s'");
                if (receiver == JV_MKOBJ(o)) { o->elems[idx] = val; return 1; }
                return set_on_receiver(ctx, receiver, key, val, throw_err) < 0 ? -1 : 1;
            }
            if (!(o->shape->h.flags & SHAPE_HAS_INDEX)) goto next;
        } else if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length]) {
            if (o->h.flags & OBJ_LEN_RO) return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s' of array");
            if (receiver == JV_MKOBJ(o)) {
                KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, PD_HAS_VALUE };
                return kz_define_own_property(ctx, o, key, &d, throw_err);
            }
            return set_on_receiver(ctx, receiver, key, val, throw_err) < 0 ? -1 : 1;
        }
        {
            int i = kz_shape_find(o->shape, key);
            if (i >= 0) {
                uint32_t a = o->shape->props[i].attrs;
                if (a & KZ_PROP_ACCESSOR) {
                    JSValue s = ((JSAccessor *)JV_PTR(o->slots[i]))->setter;
                    if (!JV_IS_OBJECT(s)) return set_fail(ctx, throw_err, key, "Cannot set property %s which has only a getter");
                    JSValue args[1] = { val };
                    JSValue res = kz_call(ctx, s, receiver, 1, args);
                    return res == JS_EXCEPTION ? -1 : 1;
                }
                if (!(a & KZ_PROP_W)) return set_fail(ctx, throw_err, key, "Cannot assign to read only property '%s'");
                if (receiver == JV_MKOBJ(o)) { o->slots[i] = val; return 1; }
                return set_on_receiver(ctx, receiver, key, val, throw_err) < 0 ? -1 : 1;
            }
        }
    next:;
        JSValue p;
        if (o->h.flags & OBJ_EXOTIC) {
            if (kz_get_proto(ctx, o, &p)) return -1;
        } else p = o->shape->proto;
        if (!JV_IS_OBJECT(p)) break;
        o = JV_OBJ(p);
    }
    /* nothing on the chain: a new own property of the receiver */
    if (receiver == obj && JV_IS_OBJECT(receiver)) {
        JSObject *r = JV_OBJ(receiver);
        if (!(r->h.flags & OBJ_EXOTIC) && kz_class(r) != KZ_CLASS_ARRAY) {
            /* the walk started at the receiver, so the key is not an own property of it */
            if (!(r->h.flags & OBJ_EXTENSIBLE)) return set_fail(ctx, throw_err, key, "Cannot add property %s, object is not extensible");
            KzPropDesc d = { val, JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_WEC | PD_HAS_VALUE };
            return add_own(ctx, r, key, &d) ? -1 : 1;
        }
    }
    return set_on_receiver(ctx, receiver, key, val, throw_err) < 0 ? -1 : 1;
}

int kz_set_prop(JSContext *ctx, JSValue obj, JSValue key, JSValue val, bool throw_err) {
    return kz_set(ctx, obj, key, val, obj, throw_err);
}

int kz_set_index(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val, bool throw_err) {
    if (idx > KZ_MAX_INDEX) {
        KZ_ROOTS(ctx, r, 2);
        r[0] = val;
        r[1] = kz_key_from_value(ctx, jv_from_dbl((double)idx));
        if (r[1] == JS_EXCEPTION) return -1;
        return kz_set(ctx, obj, r[1], r[0], obj, throw_err);
    }
    return kz_set(ctx, obj, KZ_KEY_FROM_INDEX(idx), val, obj, throw_err);
}

/* ================================================================== [[Delete]] */

int kz_delete_property(JSContext *ctx, JSObject *o, JSValue key, bool throw_err) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->del) {
            int r = x->del(ctx, o, key);
            if (r != KZ_FALLBACK) {
                if (r == 0 && throw_err) return refuse(ctx, true, key, "Cannot delete property '%s'");
                return r;
            }
        }
    }
    if (KZ_KEY_IS_INDEX(key)) {
        uint32_t idx = KZ_KEY_INDEX(key);
        if (idx < o->elem_len && o->elems[idx] != JS_HOLE) {
            if (o->h.flags & OBJ_ELEMS_SEALED) return refuse(ctx, throw_err, key, "Cannot delete property '%s'");
            o->elems[idx] = JS_HOLE;
            if (idx == o->elem_len - 1) {
                uint32_t n = idx;
                while (n > 0 && o->elems[n - 1] == JS_HOLE) n--;
                o->elem_len = n;
            }
            return 1;
        }
        if (!(o->shape->h.flags & SHAPE_HAS_INDEX)) return 1;
    } else if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length])
        return refuse(ctx, throw_err, key, "Cannot delete property '%s'");
    int i = kz_shape_find(o->shape, key);
    if (i < 0) return 1;
    if (!(o->shape->props[i].attrs & KZ_PROP_C)) return refuse(ctx, throw_err, key, "Cannot delete property '%s'");
    return remove_prop(ctx, o, i) ? -1 : 1;
}

/* ================================================================== [[OwnPropertyKeys]] */

static int cmp_index_keys(const void *a, const void *b) {
    uint32_t x = KZ_KEY_INDEX(*(const JSValue *)a), y = KZ_KEY_INDEX(*(const JSValue *)b);
    return x < y ? -1 : x > y ? 1 : 0;
}

/* appends o's own keys to `keys` in the specified order: indices ascending, strings in creation order, symbols in
   creation order (filter: KZ_KEYS_*).  Index keys stay index-encoded (callers convert with kz_key_to_value). */
int kz_own_keys(JSContext *ctx, JSObject *o, KzVec *keys, int filter) {
    uint32_t start = keys->len;
    bool want_str = filter & KZ_KEYS_STRINGS, want_sym = filter & KZ_KEYS_SYMBOLS, enum_only = filter & KZ_KEYS_ENUM;
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->own_keys) {
            int r = x->own_keys(ctx, o, keys);
            if (r < 0) return -1;
            if (r == 1) {
                /* the hook gave everything (a proxy): apply the filter here */
                if (filter == (KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return 0;
                uint32_t w = start;
                for (uint32_t i = start; i < keys->len; i++) {
                    JSValue k = keys->v[i];
                    bool sym = JV_IS_SYMBOL(k);
                    if ((sym && !want_sym) || (!sym && !want_str)) continue;
                    if (enum_only) {
                        KZ_ROOT_DESC(ctx, d);
                        int h = kz_get_own_property(ctx, o, k, &d);
                        if (h < 0) return -1;
                        if (!h || !(d.flags & KZ_PROP_E)) continue;
                    }
                    keys->v[w++] = keys->v[i];
                }
                keys->len = w;
                return 0;
            }
            /* the hook added keys that come before the ordinary ones (a typed array's indexes: string keys, enumerable);
               a caller that asked for symbols only does not want them */
            if (!want_str) keys->len = start;
        }
    }
    if (want_str) {
        uint32_t idx_start = keys->len;
        for (uint32_t i = 0; i < o->elem_len; i++)
            if (o->elems[i] != JS_HOLE && kz_vec_push(ctx, keys, KZ_KEY_FROM_INDEX(i))) return -1;
        uint32_t elem_count = keys->len - idx_start;
        if (o->shape->h.flags & SHAPE_HAS_INDEX) {
            JSShape *s = o->shape;
            for (uint32_t i = 0; i < s->nprops; i++) {
                JSValue k = s->props[i].key;
                if (!KZ_KEY_IS_INDEX(k)) continue;
                if (enum_only && !(s->props[i].attrs & KZ_PROP_E)) continue;
                if (kz_vec_push(ctx, keys, k)) return -1;
                s = o->shape;
            }
            if (keys->len - idx_start > elem_count)
                qsort(keys->v + idx_start, keys->len - idx_start, sizeof(JSValue), cmp_index_keys);
        }
        if (enum_only && (o->h.flags & OBJ_ELEMS_SEALED) && !(elem_attrs(o) & KZ_PROP_E)) {
            /* (elements are always enumerable) */
        }
        if (kz_class(o) == KZ_CLASS_ARRAY && !enum_only && kz_vec_push(ctx, keys, ctx->rt->atom[KZ_ATOM_length])) return -1;
    }
    JSShape *s = o->shape;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && !want_str) continue;
        if (pass == 1 && !want_sym) continue;
        for (uint32_t i = 0; i < s->nprops; i++) {
            JSValue k = s->props[i].key;
            if (k == JS_EMPTY || KZ_KEY_IS_INDEX(k)) continue;
            bool sym = JV_IS_SYMBOL(k);
            if (sym != (pass == 1)) continue;
            if (sym && (JV_SYM(k)->h.flags & SYM_PRIVATE) && !(filter & KZ_KEYS_PRIVATE)) continue;
            if (enum_only && !(s->props[i].attrs & KZ_PROP_E)) continue;
            if (kz_vec_push(ctx, keys, k)) return -1;
            s = o->shape;
        }
    }
    return 0;
}

/* ================================================================== prototypes and extensibility */

int kz_get_proto(JSContext *ctx, JSObject *o, JSValue *out) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->get_proto) {
            int r = x->get_proto(ctx, o, out);
            if (r != KZ_FALLBACK) return r < 0 ? -1 : 0;
        }
    }
    *out = o->shape->proto;
    return 0;
}

int kz_set_proto(JSContext *ctx, JSObject *o, JSValue proto, bool throw_err) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->set_proto) {
            int r = x->set_proto(ctx, o, proto);
            if (r != KZ_FALLBACK) {
                if (r == 0 && throw_err) { kz_throw_type(ctx, "Cannot set prototype"); return -1; }
                return r;
            }
        }
    }
    JSValue cur = o->shape->proto;
    if (cur == proto) return 1;
    if (o->h.flags & OBJ_IMMUTABLE_PROTO) {
        if (throw_err) { kz_throw_type(ctx, "Immutable prototype object cannot have its prototype set"); return -1; }
        return 0;
    }
    if (!(o->h.flags & OBJ_EXTENSIBLE)) {
        if (throw_err) { kz_throw_type(ctx, "Cannot set prototype of a non-extensible object"); return -1; }
        return 0;
    }
    /* no cycles (stop at proxies, as the specification does) */
    for (JSValue p = proto; JV_IS_OBJECT(p);) {
        JSObject *po = JV_OBJ(p);
        if (po == o) {
            if (throw_err) { kz_throw_type(ctx, "Cyclic __proto__ value"); return -1; }
            return 0;
        }
        if (kz_class(po) == KZ_CLASS_PROXY) break;
        p = po->shape->proto;
    }
    return kz_obj_set_proto_raw(ctx, o, proto) ? -1 : 1;
}

int kz_is_extensible(JSContext *ctx, JSObject *o) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->is_extensible) {
            int r = x->is_extensible(ctx, o);
            if (r != KZ_FALLBACK) return r;
        }
    }
    return (o->h.flags & OBJ_EXTENSIBLE) ? 1 : 0;
}

int kz_prevent_extensions(JSContext *ctx, JSObject *o, bool throw_err) {
    if (o->h.flags & OBJ_EXOTIC) {
        const KzExotic *x = kz_exotic[kz_class(o)];
        if (x && x->prevent_ext) {
            int r = x->prevent_ext(ctx, o);
            if (r != KZ_FALLBACK) {
                if (r == 0 && throw_err) { kz_throw_type(ctx, "Cannot prevent extensions"); return -1; }
                return r;
            }
        }
    }
    o->h.flags &= ~OBJ_EXTENSIBLE;
    return 1;
}

/* SetIntegrityLevel */
static bool is_private_key(JSValue k) { return JV_IS_SYMBOL(k) && (JV_SYM(k)->h.flags & SYM_PRIVATE); }

int kz_set_integrity(JSContext *ctx, JSObject *o, bool frozen) {
    if (kz_prevent_extensions(ctx, o, true) < 0) return -1;
    if (o->h.flags & OBJ_EXOTIC) {
        /* the generic algorithm over the internal methods */
        KZ_ROOT_VEC(ctx, keys);
        if (kz_own_keys(ctx, o, &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
        for (uint32_t i = 0; i < keys.len; i++) {
            KzPropDesc d = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, PD_HAS_C };
            if (frozen) {
                KZ_ROOT_DESC(ctx, cur);
                int h = kz_get_own_property(ctx, o, keys.v[i], &cur);
                if (h < 0) return -1;
                if (!h) continue;
                if (!(cur.flags & PD_ACCESSOR)) d.flags |= PD_HAS_W;
            }
            if (kz_define_own_property(ctx, o, keys.v[i], &d, true) < 0) return -1;
        }
        return 0;
    }
    if (o->elem_len) {
        o->h.flags |= OBJ_ELEMS_SEALED;
        if (frozen) o->h.flags |= OBJ_ELEMS_FROZEN;
    }
    if (kz_class(o) == KZ_CLASS_ARRAY && frozen) o->h.flags |= OBJ_LEN_RO;
    JSShape *s = o->shape;
    bool change = false;
    for (uint32_t i = 0; i < s->nprops; i++) {
        if (s->props[i].key == JS_EMPTY || is_private_key(s->props[i].key)) continue;
        uint32_t a = s->props[i].attrs, na = a & ~KZ_PROP_C;
        if (frozen && !(a & KZ_PROP_ACCESSOR)) na &= ~KZ_PROP_W;
        if (na != a) { change = true; break; }
    }
    if (!change) return 0;
    if (to_dict(ctx, o)) return -1;
    s = o->shape;
    for (uint32_t i = 0; i < s->nprops; i++) {
        if (s->props[i].key == JS_EMPTY || is_private_key(s->props[i].key)) continue;
        uint32_t a = s->props[i].attrs & ~KZ_PROP_C;
        if (frozen && !(a & KZ_PROP_ACCESSOR)) a &= ~KZ_PROP_W;
        s->props[i].attrs = a;
    }
    s->version++;
    return 0;
}

/* TestIntegrityLevel: 1 yes, 0 no, -1 */
int kz_test_integrity(JSContext *ctx, JSObject *o, bool frozen) {
    int ext = kz_is_extensible(ctx, o);
    if (ext < 0) return -1;
    if (ext) return 0;
    if (!(o->h.flags & OBJ_EXOTIC)) {
        if (o->elem_len) {
            for (uint32_t i = 0; i < o->elem_len; i++)
                if (o->elems[i] != JS_HOLE) {
                    if (!(o->h.flags & OBJ_ELEMS_SEALED)) return 0;
                    if (frozen && !(o->h.flags & OBJ_ELEMS_FROZEN)) return 0;
                    break;
                }
        }
        if (kz_class(o) == KZ_CLASS_ARRAY && frozen && !(o->h.flags & OBJ_LEN_RO)) return 0;
        JSShape *s = o->shape;
        for (uint32_t i = 0; i < s->nprops; i++) {
            if (s->props[i].key == JS_EMPTY || is_private_key(s->props[i].key)) continue;
            uint32_t a = s->props[i].attrs;
            if (a & KZ_PROP_C) return 0;
            if (frozen && !(a & KZ_PROP_ACCESSOR) && (a & KZ_PROP_W)) return 0;
        }
        return 1;
    }
    KZ_ROOT_VEC(ctx, keys);
    if (kz_own_keys(ctx, o, &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
    for (uint32_t i = 0; i < keys.len; i++) {
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, o, keys.v[i], &d);
        if (h < 0) return -1;
        if (!h) continue;
        if (d.flags & KZ_PROP_C) return 0;
        if (frozen && !(d.flags & PD_ACCESSOR) && (d.flags & KZ_PROP_W)) return 0;
    }
    return 1;
}

/* ================================================================== descriptors as objects */

JSValue kz_from_property_descriptor(JSContext *ctx, const KzPropDesc *d) {
    KZ_ROOT(ctx, obj);
    obj = kz_new_object(ctx);
    if (obj == JS_EXCEPTION) return obj;
    JSRuntime *rt = ctx->rt;
    if (d->flags & PD_ACCESSOR) {
        if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_get], d->getter)) return JS_EXCEPTION;
        if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_set], d->setter)) return JS_EXCEPTION;
    } else {
        if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_value], d->value)) return JS_EXCEPTION;
        if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_writable], JV_MKBOOL(d->flags & KZ_PROP_W))) return JS_EXCEPTION;
    }
    if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_enumerable], JV_MKBOOL(d->flags & KZ_PROP_E))) return JS_EXCEPTION;
    if (kz_create_data_property(ctx, obj, rt->atom[KZ_ATOM_configurable], JV_MKBOOL(d->flags & KZ_PROP_C))) return JS_EXCEPTION;
    return obj;
}

/* ToPropertyDescriptor: d must be rooted by the caller (KZ_ROOT_DESC) */
int kz_to_property_descriptor(JSContext *ctx, JSValue obj, KzPropDesc *d) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "Property description must be an object"); return -1; }
    JSRuntime *rt = ctx->rt;
    JSObject *o = JV_OBJ(obj);
    d->flags = 0;
    d->value = d->getter = d->setter = JS_UNDEFINED;
    static const int atoms[6] = { KZ_ATOM_enumerable, KZ_ATOM_configurable, KZ_ATOM_value, KZ_ATOM_writable, KZ_ATOM_get, KZ_ATOM_set };
    for (int i = 0; i < 6; i++) {
        JSValue key = rt->atom[atoms[i]];
        int h = kz_has_property(ctx, o, key);
        if (h < 0) return -1;
        if (!h) continue;
        JSValue v = kz_get(ctx, obj, key, obj);
        if (v == JS_EXCEPTION) return -1;
        switch (i) {
        case 0: d->flags |= PD_HAS_E | (kz_to_bool(v) ? KZ_PROP_E : 0); break;
        case 1: d->flags |= PD_HAS_C | (kz_to_bool(v) ? KZ_PROP_C : 0); break;
        case 2: d->flags |= PD_HAS_VALUE; d->value = v; break;
        case 3: d->flags |= PD_HAS_W | (kz_to_bool(v) ? KZ_PROP_W : 0); break;
        case 4:
            if (v != JS_UNDEFINED && !kz_is_callable(v)) { kz_throw_type(ctx, "Getter must be a function"); return -1; }
            d->flags |= PD_HAS_GET; d->getter = v; break;
        case 5:
            if (v != JS_UNDEFINED && !kz_is_callable(v)) { kz_throw_type(ctx, "Setter must be a function"); return -1; }
            d->flags |= PD_HAS_SET; d->setter = v; break;
        }
    }
    if ((d->flags & (PD_HAS_GET | PD_HAS_SET)) && (d->flags & (PD_HAS_VALUE | PD_HAS_W))) {
        kz_throw_type(ctx, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return -1;
    }
    if (d->flags & (PD_HAS_GET | PD_HAS_SET)) d->flags |= PD_ACCESSOR;
    return 0;
}

/* ================================================================== construction helpers */

int kz_get_proto_from_ctor(JSContext *ctx, JSValue new_target, KzIntrinsic def, JSValue *out) {
    if (JV_IS_OBJECT(new_target)) {
        JSValue p = kz_get(ctx, new_target, ctx->rt->atom[KZ_ATOM_prototype], new_target);
        if (p == JS_EXCEPTION) return -1;
        if (JV_IS_OBJECT(p)) { *out = p; return 0; }
        /* the realm of new_target */
        JSContext *realm = kz_function_realm(ctx, JV_OBJ(new_target));
        if (!realm) return -1;
        *out = realm->intrinsic[def];
        return 0;
    }
    *out = ctx->intrinsic[def];
    return 0;
}

JSValue kz_ordinary_create_from_ctor(JSContext *ctx, JSValue new_target, KzIntrinsic def, KzClassId cls) {
    KZ_ROOT(ctx, proto);
    if (kz_get_proto_from_ctor(ctx, new_target, def, &proto)) return JS_EXCEPTION;
    return kz_new_object_class(ctx, cls, proto);
}
