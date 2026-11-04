/*
 * typed.c: %TypedArray% and the eleven typed array constructors (ECMA-262 23.2) with the integer-indexed exotic object
 * (10.4.5).  A typed array is an object of one of the classes UINT8C_ARRAY .. BIGUINT64_ARRAY whose u.ta holds the buffer
 * object, the byte offset, the length in elements, and whether the length tracks a resizable buffer (the "auto" length).
 * Every access re-reads the buffer's state, because user code run in between (a valueOf, a callback) can detach the
 * buffer or resize it under the array.
 *
 * Property keys that are canonical numeric strings ("1", "-0", "1.5", "NaN") are elements, never ordinary properties:
 * they are looked up in the buffer, and an invalid index silently reads undefined and ignores writes.
 */
#include "jsint.h"
#include <stdlib.h>

#define NKINDS 11
static const char *const kind_names[NKINDS] = {
    "Uint8ClampedArray", "Int8Array", "Uint8Array", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
    "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array",
};

static int kind_of(const JSObject *o) { return (int)kz_class(o) - KZ_CLASS_UINT8C_ARRAY; }
static bool is_ta(JSValue v) { return JV_IS_OBJECT(v) && KZ_IS_TYPED_ARRAY(kz_class(JV_OBJ(v))); }
static bool is_big_kind(int kind) { return kind >= 9; }

/* ================================================================== state of a typed array and its buffer */

typedef struct TA {
    JSObject *o, *buf;
    int kind;
    uint32_t es;            /* element size */
    size_t len;             /* length in elements (0 when out of bounds) */
    bool oob;               /* detached, or no longer fits its buffer */
} TA;

static void ta_state(JSObject *o, TA *t) {
    t->o = o;
    t->kind = kind_of(o);
    t->es = kz_ta_size[t->kind];
    t->buf = JV_OBJ(o->u.ta.buffer);
    t->len = 0;
    t->oob = true;
    if (t->buf->u.ab.detached) return;
    size_t blen = t->buf->u.ab.len, off = o->u.ta.offset;
    if (o->u.ta.track) {
        if (off > blen) return;
        t->len = (blen - off) / t->es;
    } else {
        if (off + o->u.ta.length * t->es > blen) return;
        t->len = o->u.ta.length;
    }
    t->oob = false;
}

bool kz_is_detached(JSObject *ta) { return JV_OBJ(ta->u.ta.buffer)->u.ab.detached; }
bool kz_typed_oob(JSObject *ta) { TA t; ta_state(ta, &t); return t.oob; }
uint32_t kz_typed_length(JSObject *ta) { TA t; ta_state(ta, &t); return t.oob ? 0 : (uint32_t)t.len; }

static uint8_t *ta_data(const TA *t) { return t->buf->u.ab.data + t->o->u.ta.offset; }

/* the element at i (which must be valid right now) as a value */
static JSValue ta_load(JSContext *ctx, const TA *t, size_t i) {
    const uint8_t *p = ta_data(t) + i * t->es;
    return is_big_kind(t->kind) ? kz_elem_load_big(ctx, t->kind, p) : kz_elem_load(t->kind, p);
}

/* IsValidIntegerIndex (10.4.5.14) */
static bool valid_index(JSObject *o, double idx, TA *t) {
    ta_state(o, t);
    if (t->oob) return false;
    if (idx != idx || idx < 0 || idx >= (double)t->len) return false;
    if (idx != kz_math_floor(idx)) return false;
    return !(idx == 0 && __builtin_signbit(idx));
}

/* TypedArrayGetElement */
static JSValue ta_get_element(JSContext *ctx, JSObject *o, double idx) {
    TA t;
    if (!valid_index(o, idx, &t)) return JS_UNDEFINED;
    return ta_load(ctx, &t, (size_t)idx);
}

/* converts v for this array's content type: ToBigInt for the BigInt kinds, else ToNumber (as a value) */
static JSValue ta_convert(JSContext *ctx, int kind, JSValue v) {
    return is_big_kind(kind) ? kz_to_bigint(ctx, v) : kz_to_number_value(ctx, v);
}

static int ta_store(JSContext *ctx, const TA *t, size_t i, JSValue num) {
    uint8_t *p = ta_data(t) + i * t->es;
    if (is_big_kind(t->kind)) return kz_elem_store_big(ctx, t->kind, p, num);
    kz_elem_store_number(t->kind, p, jv_num(num));
    return 0;
}

/* TypedArraySetElement (10.4.5.16): converts, then writes if the index is (still) valid */
static int ta_set_element(JSContext *ctx, JSObject *o, double idx, JSValue v) {
    KZ_ROOT(ctx, rv);
    rv = ta_convert(ctx, kind_of(o), v);
    if (rv == JS_EXCEPTION) return -1;
    TA t;
    if (valid_index(o, idx, &t)) return ta_store(ctx, &t, (size_t)idx, rv);
    return 0;
}

/* ================================================================== the integer-indexed exotic object */

/* CanonicalNumericIndexString: 1 with the number in *d, 0 for an ordinary key, -1 on out of memory */
static int canon_numeric(JSContext *ctx, JSValue key, double *d) {
    if (KZ_KEY_IS_INDEX(key)) { *d = (double)KZ_KEY_INDEX(key); return 1; }
    if (!JV_IS_STRING(key)) return 0;
    JSString *s = JV_STR(key);
    uint32_t n = kz_str_len(s);
    if (!n) return 0;
    uint32_t c = kz_str_at(s, 0);
    if (!(c == '-' || (c >= '0' && c <= '9') || c == 'I' || c == 'N')) return 0;
    if (n == 2 && c == '-' && kz_str_at(s, 1) == '0') { *d = -0.0; return 1; }
    double v = kz_string_to_number(s);
    KZ_ROOT(ctx, t);
    t = kz_num_to_str(ctx, v);
    if (t == JS_EXCEPTION) return -1;
    JSString *ts = kz_str_flat(ctx, JV_STR(t));
    if (!ts) return -1;
    if (!kz_str_eq(ts, s)) return 0;
    *d = v;
    return 1;
}

static int xo_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    TA t;
    if (!valid_index(o, idx, &t)) return 0;
    d->value = ta_load(ctx, &t, (size_t)idx);
    if (d->value == JS_EXCEPTION) return -1;
    d->getter = d->setter = JS_UNDEFINED;
    d->flags = KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
    return 1;
}

static int xo_has(JSContext *ctx, JSObject *o, JSValue key) {
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    TA t;
    return valid_index(o, idx, &t) ? 1 : 0;
}

static int xo_define_own(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d) {
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    TA t;
    if (!valid_index(o, idx, &t)) return 0;
    if ((d->flags & PD_HAS_C) && !(d->flags & KZ_PROP_C)) return 0;
    if ((d->flags & PD_HAS_E) && !(d->flags & KZ_PROP_E)) return 0;
    if (d->flags & (PD_HAS_GET | PD_HAS_SET)) return 0;
    if ((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W)) return 0;
    if (d->flags & PD_HAS_VALUE) return ta_set_element(ctx, o, idx, d->value) < 0 ? -1 : 1;
    return 1;
}

static int xo_get(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out) {
    (void)receiver;
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    *out = ta_get_element(ctx, o, idx);
    return *out == JS_EXCEPTION ? -1 : 1;
}

static int xo_set(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver) {
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    if (receiver == JV_MKOBJ(o)) return ta_set_element(ctx, o, idx, val) < 0 ? -1 : 1;
    TA t;
    if (!valid_index(o, idx, &t)) return 1;
    /* OrdinarySet's tail: the property is a writable data property here; write it to the receiver */
    if (!JV_IS_OBJECT(receiver)) return 0;
    KZ_ROOT_DESC(ctx, ex);
    int has = kz_get_own_property(ctx, JV_OBJ(receiver), key, &ex);
    if (has < 0) return -1;
    if (has) {
        if ((ex.flags & PD_ACCESSOR) || !(ex.flags & KZ_PROP_W)) return 0;
        KzPropDesc nd = { val, JS_UNDEFINED, JS_UNDEFINED, PD_HAS_VALUE };
        return kz_define_own_property(ctx, JV_OBJ(receiver), key, &nd, false);
    }
    KzPropDesc nd = { val, JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
    return kz_define_own_property(ctx, JV_OBJ(receiver), key, &nd, false);
}

static int xo_del(JSContext *ctx, JSObject *o, JSValue key) {
    double idx;
    int r = canon_numeric(ctx, key, &idx);
    if (r <= 0) return r < 0 ? -1 : KZ_FALLBACK;
    TA t;
    return valid_index(o, idx, &t) ? 0 : 1;
}

static int xo_own_keys(JSContext *ctx, JSObject *o, KzVec *keys) {
    TA t;
    ta_state(o, &t);
    for (size_t i = 0; i < t.len; i++)
        if (kz_vec_push(ctx, keys, KZ_KEY_FROM_INDEX(i))) return -1;
    return 0;
}

static const KzExotic typed_exotic = {
    .get_own = xo_get_own, .define_own = xo_define_own, .has = xo_has, .get = xo_get, .set = xo_set, .del = xo_del,
    .own_keys = xo_own_keys,
};

/* ================================================================== creation */

static KzIntrinsic proto_of_kind(int kind) { return (KzIntrinsic)(KZ_I_UINT8C_PROTO + kind); }
static KzIntrinsic ctor_of_kind(int kind) { return (KzIntrinsic)(KZ_I_UINT8C + kind); }

/* the typed array object for kind (no buffer yet), its prototype from new_target */
static JSValue ta_new_object(JSContext *ctx, int kind, JSValue new_target) {
    return kz_ordinary_create_from_ctor(ctx, new_target, proto_of_kind(kind), (KzClassId)(KZ_CLASS_UINT8C_ARRAY + kind));
}

/* AllocateTypedArrayBuffer: a fresh buffer of len elements for o */
static int ta_alloc_buffer(JSContext *ctx, JSObject *o, uint64_t len) {
    uint32_t es = kz_ta_size[kind_of(o)];
    if (len > 0x7FFFFFFFu / es) { kz_throw_range(ctx, "Invalid typed array length"); return -1; }
    JSValue b = kz_new_array_buffer(ctx, (size_t)len * es);
    if (b == JS_EXCEPTION) return -1;
    o->u.ta.buffer = b;
    o->u.ta.offset = 0;
    o->u.ta.length = (size_t)len;
    o->u.ta.track = false;
    return 0;
}

/* TypedArrayCreateFromConstructor: new C(args), validated; a length argument must be honoured */
static JSValue ta_create_from_ctor(JSContext *ctx, JSValue ctor, int argc, JSValue *argv) {
    KZ_ROOT(ctx, o);
    o = kz_construct(ctx, ctor, argc, argv, ctor);
    if (o == JS_EXCEPTION) return o;
    if (!is_ta(o)) return kz_throw_type(ctx, "The constructor did not return a typed array");
    TA t;
    ta_state(JV_OBJ(o), &t);
    if (t.oob) return kz_throw_type(ctx, "The constructor returned a detached or out-of-bounds typed array");
    if (argc == 1 && JV_IS_NUMBER(argv[0]) && (double)t.len < jv_num(argv[0])) return kz_throw_type(ctx, "The constructor returned a typed array that is too small");
    return o;
}

/* TypedArraySpeciesCreate */
static JSValue ta_species_create(JSContext *ctx, JSObject *exemplar, int argc, JSValue *argv) {
    KZ_ROOTS(ctx, r, 2);
    int kind = kind_of(exemplar);
    r[0] = kz_species_constructor(ctx, JV_MKOBJ(exemplar), kz_intr(ctx, ctor_of_kind(kind)));
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = ta_create_from_ctor(ctx, r[0], argc, argv);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (is_big_kind(kind_of(JV_OBJ(r[1]))) != is_big_kind(kind)) return kz_throw_type(ctx, "The species constructor returned a typed array of another content type");
    return r[1];
}

/* TypedArrayCreateSameType */
static JSValue ta_create_same(JSContext *ctx, JSObject *exemplar, uint64_t len) {
    JSValue arg = jv_number((double)len);
    return ta_create_from_ctor(ctx, kz_intr(ctx, ctor_of_kind(kind_of(exemplar))), 1, &arg);
}

static int init_from_typed_array(JSContext *ctx, JSObject *o, JSObject *src) {
    TA s;
    ta_state(src, &s);
    if (s.oob) { kz_throw_type(ctx, "Cannot construct a typed array from a detached or out-of-bounds typed array"); return -1; }
    int kind = kind_of(o);
    if (is_big_kind(kind) != is_big_kind(s.kind)) { kz_throw_type(ctx, "Cannot mix BigInt and other types, use explicit conversions"); return -1; }
    if (ta_alloc_buffer(ctx, o, s.len)) return -1;
    TA d;
    ta_state(o, &d);
    if (s.kind == kind) memcpy(ta_data(&d), ta_data(&s), s.len * s.es);
    else
        for (size_t i = 0; i < s.len; i++) {
            if (is_big_kind(kind)) memcpy(ta_data(&d) + i * d.es, ta_data(&s) + i * s.es, 8);
            else {
                JSValue v = kz_elem_load(s.kind, ta_data(&s) + i * s.es);
                kz_elem_store_number(kind, ta_data(&d) + i * d.es, jv_num(v));
            }
        }
    return 0;
}

static int init_from_buffer(JSContext *ctx, JSObject *o, JSValue bufv, JSValue byte_offset, JSValue length) {
    JSObject *buf = JV_OBJ(bufv);
    uint32_t es = kz_ta_size[kind_of(o)];
    uint64_t offset, new_len = 0;
    if (kz_to_index(ctx, byte_offset, &offset)) return -1;
    if (offset % es) { kz_throw_range(ctx, "start offset of %s should be a multiple of %d", kind_names[kind_of(o)], (int)es); return -1; }
    if (length != JS_UNDEFINED && kz_to_index(ctx, length, &new_len)) return -1;
    if (buf->u.ab.detached) { kz_throw_type(ctx, "Cannot construct a typed array on a detached ArrayBuffer"); return -1; }
    uint64_t blen = buf->u.ab.len;
    bool track = false;
    if (length == JS_UNDEFINED && buf->u.ab.resizable) {
        if (offset > blen) { kz_throw_range(ctx, "Start offset %d is outside the bounds of the buffer", (int)offset); return -1; }
        track = true;
    } else if (length == JS_UNDEFINED) {
        if (blen % es) { kz_throw_range(ctx, "byte length of %s should be a multiple of %d", kind_names[kind_of(o)], (int)es); return -1; }
        if (offset > blen) { kz_throw_range(ctx, "Start offset %d is outside the bounds of the buffer", (int)offset); return -1; }
        new_len = (blen - offset) / es;
    } else if (offset + new_len * es > blen) { kz_throw_range(ctx, "Invalid typed array length: %d", (int)new_len); return -1; }
    o->u.ta.buffer = bufv;
    o->u.ta.offset = (size_t)offset;
    o->u.ta.length = (size_t)new_len;
    o->u.ta.track = track;
    return 0;
}

static JSValue ta_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    JSRuntime *rt = ctx->rt;
    int kind = magic;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor %s requires 'new'", kind_names[kind]);
    KZ_ROOTS(ctx, r, 4);
    if (argc == 0 || !JV_IS_OBJECT(argv[0])) {
        uint64_t len = 0;
        if (argc > 0 && kz_to_index(ctx, argv[0], &len)) return JS_EXCEPTION;
        r[0] = ta_new_object(ctx, kind, rt->new_target);
        if (r[0] == JS_EXCEPTION) return r[0];
        if (ta_alloc_buffer(ctx, JV_OBJ(r[0]), len)) return JS_EXCEPTION;
        return r[0];
    }
    JSValue first = argv[0];
    r[0] = ta_new_object(ctx, kind, rt->new_target);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSObject *o = JV_OBJ(r[0]);
    if (is_ta(first)) {
        if (init_from_typed_array(ctx, o, JV_OBJ(first))) return JS_EXCEPTION;
        return r[0];
    }
    KzClassId fc = kz_class(JV_OBJ(first));
    if (fc == KZ_CLASS_ARRAYBUFFER || fc == KZ_CLASS_SHAREDARRAYBUFFER) {
        if (init_from_buffer(ctx, o, first, argv[1], argv[2])) return JS_EXCEPTION;
        return r[0];
    }
    r[1] = kz_get_method(ctx, first, rt->sym[KZ_SYM_iterator]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KZ_ROOT_VEC(ctx, list);
    uint64_t len;
    if (r[1] != JS_UNDEFINED) {
        KZ_ROOT_ITER(ctx, it);
        if (kz_get_iterator_from_method(ctx, first, r[1], &it)) return JS_EXCEPTION;
        for (;;) {
            int st = kz_iter_step(ctx, &it, &r[2]);
            if (st < 0) return JS_EXCEPTION;
            if (!st) break;
            if (kz_vec_push(ctx, &list, r[2])) return JS_EXCEPTION;
        }
        len = list.len;
        if (ta_alloc_buffer(ctx, o, len)) return JS_EXCEPTION;
        for (uint64_t k = 0; k < len; k++)
            if (ta_set_element(ctx, o, (double)k, list.v[k]) < 0) return JS_EXCEPTION;
        return r[0];
    }
    if (kz_length_of(ctx, first, &len)) return JS_EXCEPTION;
    if (ta_alloc_buffer(ctx, o, len)) return JS_EXCEPTION;
    for (uint64_t k = 0; k < len; k++) {
        r[2] = k <= KZ_MAX_INDEX ? kz_get_index(ctx, first, (uint32_t)k) : JS_UNDEFINED;
        if (r[2] == JS_EXCEPTION || ta_set_element(ctx, o, (double)k, r[2]) < 0) return JS_EXCEPTION;
    }
    return r[0];
}

static JSValue abstract_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return kz_throw_type(ctx, "Abstract class TypedArray not directly constructable");
}

/* ================================================================== %TypedArray%.from and of */

static JSValue ta_from(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!kz_is_constructor(this_val)) return kz_throw_type(ctx, "%%TypedArray%%.from: this is not a constructor");
    JSValue mapfn = argv[1], this_arg = argv[2];
    bool mapping = mapfn != JS_UNDEFINED;
    if (mapping && !kz_is_callable(mapfn)) return kz_throw_type(ctx, "%%TypedArray%%.from: the map function is not callable");
    KZ_ROOTS(ctx, r, 5);
    r[0] = kz_get_method(ctx, argv[0], rt->sym[KZ_SYM_iterator]);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_VEC(ctx, list);
    uint64_t len;
    bool use_list = r[0] != JS_UNDEFINED;
    if (use_list) {
        KZ_ROOT_ITER(ctx, it);
        if (kz_get_iterator_from_method(ctx, argv[0], r[0], &it)) return JS_EXCEPTION;
        for (;;) {
            int st = kz_iter_step(ctx, &it, &r[1]);
            if (st < 0) return JS_EXCEPTION;
            if (!st) break;
            if (kz_vec_push(ctx, &list, r[1])) return JS_EXCEPTION;
        }
        len = list.len;
    } else {
        r[2] = kz_to_object(ctx, argv[0]);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_length_of(ctx, r[2], &len)) return JS_EXCEPTION;
    }
    JSValue larg = jv_number((double)len);
    r[3] = ta_create_from_ctor(ctx, this_val, 1, &larg);
    if (r[3] == JS_EXCEPTION) return r[3];
    for (uint64_t k = 0; k < len; k++) {
        r[1] = use_list ? list.v[k] : (k <= KZ_MAX_INDEX ? kz_get_index(ctx, r[2], (uint32_t)k) : JS_UNDEFINED);
        if (r[1] == JS_EXCEPTION) return r[1];
        if (mapping) {
            JSValue args[2] = { r[1], jv_number((double)k) };
            r[1] = kz_call(ctx, mapfn, this_arg, 2, args);
            if (r[1] == JS_EXCEPTION) return r[1];
        }
        if (kz_set_index(ctx, r[3], (uint32_t)k, r[1], true) < 0) return JS_EXCEPTION;
    }
    return r[3];
}

static JSValue ta_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    if (!kz_is_constructor(this_val)) return kz_throw_type(ctx, "%%TypedArray%%.of: this is not a constructor");
    KZ_ROOT(ctx, o);
    JSValue larg = JV_MKINT(argc);
    o = ta_create_from_ctor(ctx, this_val, 1, &larg);
    if (o == JS_EXCEPTION) return o;
    for (int k = 0; k < argc; k++)
        if (kz_set_index(ctx, o, (uint32_t)k, argv[k], true) < 0) return JS_EXCEPTION;
    return o;
}

/* ================================================================== accessors */

/* ValidateTypedArray: this must be a typed array that is in bounds */
static int validate(JSContext *ctx, JSValue v, TA *t) {
    if (!is_ta(v)) { kz_throw_type(ctx, "this is not a typed array."); return -1; }
    ta_state(JV_OBJ(v), t);
    if (t->oob) { kz_throw_type(ctx, "Cannot perform the operation on a detached or out-of-bounds typed array"); return -1; }
    return 0;
}

enum { TG_BUFFER, TG_BYTELENGTH, TG_BYTEOFFSET, TG_LENGTH };

static JSValue ta_getter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    if (!is_ta(this_val)) return kz_throw_type(ctx, "this is not a typed array.");
    JSObject *o = JV_OBJ(this_val);
    TA t;
    ta_state(o, &t);
    switch (magic) {
    case TG_BUFFER: return o->u.ta.buffer;
    case TG_BYTELENGTH: return jv_number(t.oob ? 0 : (double)(t.len * t.es));
    case TG_BYTEOFFSET: return jv_number(t.oob ? 0 : (double)o->u.ta.offset);
    default: return jv_number(t.oob ? 0 : (double)t.len);
    }
}

static JSValue ta_to_string_tag(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!is_ta(this_val)) return JS_UNDEFINED;
    return kz_atom(ctx, kind_names[kind_of(JV_OBJ(this_val))]);
}

/* ================================================================== prototype methods */

#define VALIDATE(t) TA t; if (validate(ctx, this_val, &t)) return JS_EXCEPTION

static JSValue ta_at(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    double rel;
    if (kz_to_integer_or_inf(ctx, argv[0], &rel)) return JS_EXCEPTION;
    double k = rel >= 0 ? rel : (double)t.len + rel;
    if (k < 0 || k >= (double)t.len) return JS_UNDEFINED;
    return ta_get_element(ctx, t.o, k);
}

static JSValue ta_copy_within(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    VALIDATE(t);
    (void)argc;
    uint64_t len = t.len, to, from, fin;
    if (kz_to_relative_index(ctx, argv[0], len, 0, &to) || kz_to_relative_index(ctx, argv[1], len, 0, &from)) return JS_EXCEPTION;
    if (kz_to_relative_index(ctx, argv[2], len, len, &fin)) return JS_EXCEPTION;
    int64_t count = (int64_t)(fin - from);
    if ((int64_t)(len - to) < count) count = (int64_t)(len - to);
    if (count > 0) {
        TA n;
        ta_state(t.o, &n);
        if (n.oob) return kz_throw_type(ctx, "Cannot perform copyWithin on a detached or out-of-bounds typed array");
        uint64_t nlen = n.len;
        /* the new length may be shorter: copy only what is still inside */
        uint64_t limit = nlen;
        if (from >= limit || to >= limit) return this_val;
        if ((uint64_t)count > limit - from) count = (int64_t)(limit - from);
        if ((uint64_t)count > limit - to) count = (int64_t)(limit - to);
        memmove(ta_data(&n) + to * n.es, ta_data(&n) + from * n.es, (size_t)count * n.es);
    }
    return this_val;
}

static JSValue ta_iter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    VALIDATE(t);
    return kz_array_iterator_new(ctx, this_val, magic);
}

/* every (0), some (1), forEach (2), find (3), findIndex (4), findLast (5), findLastIndex (6) */
static JSValue ta_each(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    VALIDATE(t);
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "callback is not a function");
    KZ_ROOT(ctx, v);
    size_t len = t.len;
    bool reverse = magic >= 5;
    for (size_t n = 0; n < len; n++) {
        size_t k = reverse ? len - 1 - n : n;
        v = ta_get_element(ctx, t.o, (double)k);
        if (v == JS_EXCEPTION) return v;
        JSValue args[3] = { v, jv_number((double)k), this_val };
        JSValue res = kz_call(ctx, argv[0], argv[1], 3, args);
        if (res == JS_EXCEPTION) return res;
        bool b = kz_to_bool(res);
        switch (magic) {
        case 0: if (!b) return JS_FALSE; break;
        case 1: if (b) return JS_TRUE; break;
        case 3: case 5: if (b) return v; break;
        case 4: case 6: if (b) return jv_number((double)k); break;
        default: break;
        }
    }
    switch (magic) {
    case 0: return JS_TRUE;
    case 1: return JS_FALSE;
    case 4: case 6: return JV_MKINT(-1);
    default: return JS_UNDEFINED;
    }
}

static JSValue ta_fill(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    KZ_ROOT(ctx, v);
    v = ta_convert(ctx, t.kind, argv[0]);
    if (v == JS_EXCEPTION) return v;
    uint64_t len = t.len, k, fin;
    if (kz_to_relative_index(ctx, argv[1], len, 0, &k) || kz_to_relative_index(ctx, argv[2], len, len, &fin)) return JS_EXCEPTION;
    TA n;
    ta_state(t.o, &n);
    if (n.oob) return kz_throw_type(ctx, "Cannot perform fill on a detached or out-of-bounds typed array");
    if (fin > n.len) fin = n.len;
    for (; k < fin; k++)
        if (ta_store(ctx, &n, (size_t)k, v)) return JS_EXCEPTION;
    return this_val;
}

static JSValue ta_filter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "callback is not a function");
    KZ_ROOTS(ctx, r, 3);
    KZ_ROOT_VEC(ctx, kept);
    for (size_t k = 0; k < t.len; k++) {
        r[0] = ta_get_element(ctx, t.o, (double)k);
        if (r[0] == JS_EXCEPTION) return r[0];
        JSValue args[3] = { r[0], jv_number((double)k), this_val };
        r[1] = kz_call(ctx, argv[0], argv[1], 3, args);
        if (r[1] == JS_EXCEPTION) return r[1];
        if (kz_to_bool(r[1]) && kz_vec_push(ctx, &kept, r[0])) return JS_EXCEPTION;
    }
    JSValue larg = jv_number((double)kept.len);
    r[2] = ta_species_create(ctx, t.o, 1, &larg);
    if (r[2] == JS_EXCEPTION) return r[2];
    for (uint32_t n = 0; n < kept.len; n++)
        if (kz_set_index(ctx, r[2], n, kept.v[n], true) < 0) return JS_EXCEPTION;
    return r[2];
}

static JSValue ta_map(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "callback is not a function");
    KZ_ROOTS(ctx, r, 3);
    JSValue larg = jv_number((double)t.len);
    r[2] = ta_species_create(ctx, t.o, 1, &larg);
    if (r[2] == JS_EXCEPTION) return r[2];
    for (size_t k = 0; k < t.len; k++) {
        r[0] = ta_get_element(ctx, t.o, (double)k);
        if (r[0] == JS_EXCEPTION) return r[0];
        JSValue args[3] = { r[0], jv_number((double)k), this_val };
        r[1] = kz_call(ctx, argv[0], argv[1], 3, args);
        if (r[1] == JS_EXCEPTION || kz_set_index(ctx, r[2], (uint32_t)k, r[1], true) < 0) return JS_EXCEPTION;
    }
    return r[2];
}

/* reduce (0), reduceRight (1) */
static JSValue ta_reduce(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    VALIDATE(t);
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "callback is not a function");
    KZ_ROOTS(ctx, r, 2);
    size_t len = t.len, n = 0;
    bool right = magic == 1;
    if (argc >= 2) r[0] = argv[1];
    else {
        if (len == 0) return kz_throw_type(ctx, "Reduce of empty array with no initial value");
        r[0] = ta_get_element(ctx, t.o, (double)(right ? len - 1 : 0));
        if (r[0] == JS_EXCEPTION) return r[0];
        n = 1;
    }
    for (; n < len; n++) {
        size_t k = right ? len - 1 - n : n;
        r[1] = ta_get_element(ctx, t.o, (double)k);
        if (r[1] == JS_EXCEPTION) return r[1];
        JSValue args[4] = { r[0], r[1], jv_number((double)k), this_val };
        r[0] = kz_call(ctx, argv[0], JS_UNDEFINED, 4, args);
        if (r[0] == JS_EXCEPTION) return r[0];
    }
    return r[0];
}

/* includes (0), indexOf (1), lastIndexOf (2) */
static JSValue ta_search(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    VALIDATE(t);
    size_t len = t.len;
    if (len == 0) return magic == 0 ? JS_FALSE : JV_MKINT(-1);
    double n;
    if (magic == 2) {
        if (argc >= 2) { if (kz_to_integer_or_inf(ctx, argv[1], &n)) return JS_EXCEPTION; }
        else n = (double)len - 1;
        if (n == -KZ_INFINITY) return JV_MKINT(-1);
        double k = n >= 0 ? (n < (double)len - 1 ? n : (double)len - 1) : (double)len + n;
        TA c;
        for (; k >= 0; k--) {
            if (!valid_index(t.o, k, &c)) continue;
            JSValue v = ta_load(ctx, &c, (size_t)k);
            if (v == JS_EXCEPTION) return v;
            if (kz_strict_eq(ctx, v, argv[0]) == 1) return jv_number(k);
        }
        return JV_MKINT(-1);
    }
    if (kz_to_integer_or_inf(ctx, argv[1], &n)) return JS_EXCEPTION;
    if (n == KZ_INFINITY) return magic == 0 ? JS_FALSE : JV_MKINT(-1);
    if (n == -KZ_INFINITY) n = 0;
    double k = n >= 0 ? n : (double)len + n;
    if (k < 0) k = 0;
    TA c;
    for (; k < (double)len; k++) {
        if (magic == 0) {
            JSValue v = ta_get_element(ctx, t.o, k);
            if (v == JS_EXCEPTION) return v;
            if (kz_same_value_zero(ctx, v, argv[0]) == 1) return JS_TRUE;
        } else {
            if (!valid_index(t.o, k, &c)) continue;
            JSValue v = ta_load(ctx, &c, (size_t)k);
            if (v == JS_EXCEPTION) return v;
            if (kz_strict_eq(ctx, v, argv[0]) == 1) return jv_number(k);
        }
    }
    return magic == 0 ? JS_FALSE : JV_MKINT(-1);
}

static JSValue ta_join(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    KZ_ROOTS(ctx, r, 2);
    r[0] = argv[0] == JS_UNDEFINED ? kz_str_from_char(ctx, ',') : kz_to_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[0] = kz_str_flatten(ctx, r[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (size_t k = 0; k < t.len; k++) {
        if (k) kz_sb_put_str(&sb, JV_STR(r[0]));
        r[1] = ta_get_element(ctx, t.o, (double)k);
        if (r[1] == JS_EXCEPTION) goto fail;
        if (r[1] != JS_UNDEFINED) {
            r[1] = kz_to_string(ctx, r[1]);
            if (r[1] == JS_EXCEPTION || kz_sb_put_value(&sb, r[1])) goto fail;
        }
    }
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

static JSValue ta_to_locale_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    VALIDATE(t);
    KZ_ROOT(ctx, e);
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (size_t k = 0; k < t.len; k++) {
        if (k) kz_sb_putc(&sb, ',');
        e = ta_get_element(ctx, t.o, (double)k);
        if (e == JS_EXCEPTION) goto fail;
        if (JV_IS_NULLISH(e)) continue;
        e = kz_invoke(ctx, e, ctx->rt->atom[KZ_ATOM_toLocaleString], 0, NULL);
        if (e == JS_EXCEPTION) goto fail;
        e = kz_to_string(ctx, e);
        if (e == JS_EXCEPTION || kz_sb_put_value(&sb, e)) goto fail;
    }
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

static JSValue ta_reverse(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    VALIDATE(t);
    uint8_t *d = ta_data(&t), tmp[8];
    for (size_t lo = 0, hi = t.len; lo + 1 < hi; lo++, hi--) {
        uint8_t *a = d + lo * t.es, *b = d + (hi - 1) * t.es;
        memcpy(tmp, a, t.es);
        memcpy(a, b, t.es);
        memcpy(b, tmp, t.es);
    }
    return this_val;
}

/* %TypedArray%.prototype.set(source, offset) */
static JSValue ta_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!is_ta(this_val)) return kz_throw_type(ctx, "this is not a typed array.");
    JSObject *target = JV_OBJ(this_val);
    double off;
    if (kz_to_integer_or_inf(ctx, argv[1], &off)) return JS_EXCEPTION;
    if (off < 0) return kz_throw_range(ctx, "offset is out of bounds");
    TA t;
    ta_state(target, &t);
    if (t.oob) return kz_throw_type(ctx, "Cannot perform set on a detached or out-of-bounds typed array");
    if (is_ta(argv[0])) {
        TA s;
        ta_state(JV_OBJ(argv[0]), &s);
        if (s.oob) return kz_throw_type(ctx, "Cannot perform set from a detached or out-of-bounds typed array");
        if (is_big_kind(t.kind) != is_big_kind(s.kind)) return kz_throw_type(ctx, "Cannot mix BigInt and other types, use explicit conversions");
        if (off == KZ_INFINITY || (double)s.len + off > (double)t.len) return kz_throw_range(ctx, "offset is out of bounds");
        size_t o = (size_t)off;
        if (s.kind == t.kind) {
            memmove(ta_data(&t) + o * t.es, ta_data(&s), s.len * s.es);
            return JS_UNDEFINED;
        }
        /* different types: through a copy of the source bytes when the two share a buffer */
        uint8_t *src = ta_data(&s);
        uint8_t *copy = NULL;
        if (s.buf == t.buf) {
            copy = kz_malloc(ctx, s.len * s.es + 1);
            if (!copy) return JS_EXCEPTION;
            memcpy(copy, src, s.len * s.es);
            src = copy;
        }
        for (size_t i = 0; i < s.len; i++) {
            uint8_t *dp = ta_data(&t) + (o + i) * t.es;
            if (is_big_kind(t.kind)) memcpy(dp, src + i * s.es, 8);
            else kz_elem_store_number(t.kind, dp, jv_num(kz_elem_load(s.kind, src + i * s.es)));
        }
        kz_free(ctx->rt, copy);
        return JS_UNDEFINED;
    }
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    uint64_t slen;
    if (kz_length_of(ctx, r[0], &slen)) return JS_EXCEPTION;
    if (off == KZ_INFINITY || (double)slen + off > (double)t.len) return kz_throw_range(ctx, "offset is out of bounds");
    for (uint64_t k = 0; k < slen; k++) {
        r[1] = k <= KZ_MAX_INDEX ? kz_get_index(ctx, r[0], (uint32_t)k) : JS_UNDEFINED;
        if (r[1] == JS_EXCEPTION || ta_set_element(ctx, target, off + (double)k, r[1]) < 0) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

static JSValue ta_slice(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    uint64_t len = t.len, k, fin;
    if (kz_to_relative_index(ctx, argv[0], len, 0, &k) || kz_to_relative_index(ctx, argv[1], len, len, &fin)) return JS_EXCEPTION;
    uint64_t count = fin > k ? fin - k : 0;
    KZ_ROOT(ctx, a);
    JSValue larg = jv_number((double)count);
    a = ta_species_create(ctx, t.o, 1, &larg);
    if (a == JS_EXCEPTION) return a;
    if (count > 0) {
        TA n;
        ta_state(t.o, &n);
        if (n.oob) return kz_throw_type(ctx, "Cannot perform slice on a detached or out-of-bounds typed array");
        if (fin > n.len) fin = n.len;
        count = fin > k ? fin - k : 0;
        TA d;
        ta_state(JV_OBJ(a), &d);
        if (d.kind == n.kind) {
            size_t bytes = (size_t)count * n.es;
            size_t room = d.oob ? 0 : d.len * d.es;
            if (bytes > room) bytes = room;
            uint8_t *dst = ta_data(&d);
            const uint8_t *src = ta_data(&n) + k * n.es;
            if (bytes && d.buf == n.buf) {
                /* the species constructor may hand back a view on the same buffer: the specification copies byte by byte
                   from the lowest address up, so an overlap repeats bytes (memmove would copy "as if" through a temporary) */
                for (size_t i = 0; i < bytes; i++) dst[i] = src[i];
            } else if (bytes) memcpy(dst, src, bytes);
        } else {
            KZ_ROOT(ctx, v);
            for (uint64_t i = 0; i < count; i++) {
                v = ta_get_element(ctx, t.o, (double)(k + i));
                if (v == JS_EXCEPTION || kz_set_index(ctx, a, (uint32_t)i, v, true) < 0) return JS_EXCEPTION;
            }
        }
    }
    return a;
}

/* SortCompare for the default order: numbers ascending, -0 before +0, NaN last (BigInts compare by value) */
static int default_compare(JSContext *ctx, JSValue x, JSValue y) {
    if (JV_IS_BIGINT(x)) return kz_bigint_cmp(x, y);
    double a = jv_num(x), b = jv_num(y);
    if (a != a) return b != b ? 0 : 1;
    if (b != b) return -1;
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == 0 && b == 0) {
        bool na = __builtin_signbit(a), nb = __builtin_signbit(b);
        if (na && !nb) return -1;
        if (!na && nb) return 1;
    }
    (void)ctx;
    return 0;
}

typedef struct SortCtx { JSContext *ctx; JSValue cmp; bool failed; } SortCtx;

static int sort_compare(SortCtx *sc, JSValue x, JSValue y) {
    if (sc->cmp == JS_UNDEFINED) return default_compare(sc->ctx, x, y);
    JSValue args[2] = { x, y };
    JSValue r = kz_call(sc->ctx, sc->cmp, JS_UNDEFINED, 2, args);
    if (r == JS_EXCEPTION) { sc->failed = true; return 0; }
    double d;
    if (kz_to_number(sc->ctx, r, &d)) { sc->failed = true; return 0; }
    if (d != d) return 0;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* a stable merge sort of vals[0..n) (tmp is scratch of the same size) */
static void merge_sort(SortCtx *sc, JSValue *vals, JSValue *tmp, size_t n) {
    for (size_t width = 1; width < n && !sc->failed; width *= 2) {
        for (size_t lo = 0; lo < n && !sc->failed; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi && !sc->failed) tmp[k++] = sort_compare(sc, vals[j], vals[i]) < 0 ? vals[j++] : vals[i++];
            while (i < mid) tmp[k++] = vals[i++];
            while (j < hi) tmp[k++] = vals[j++];
        }
        memcpy(vals, tmp, n * sizeof(JSValue));
    }
}

/* sorts the elements of t into a fresh value array; the array is a rooted vector the caller owns */
static int collect_sorted(JSContext *ctx, const TA *t, JSValue cmp, KzVec *out) {
    for (size_t k = 0; k < t->len; k++) {
        JSValue v = ta_load(ctx, t, k);
        if (v == JS_EXCEPTION || kz_vec_push(ctx, out, v)) return -1;
    }
    if (out->len < 2) return 0;
    /* (every value stays in out->v, which is rooted, while tmp holds copies during a pass) */
    JSValue *tmp = kz_malloc(ctx, out->len * sizeof(JSValue));
    if (!tmp) return -1;
    SortCtx sc = { ctx, cmp, false };
    merge_sort(&sc, out->v, tmp, out->len);
    kz_free(ctx->rt, tmp);
    return sc.failed ? -1 : 0;
}

static JSValue ta_sort(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (argv[0] != JS_UNDEFINED && !kz_is_callable(argv[0])) return kz_throw_type(ctx, "The comparison function must be either a function or undefined");
    VALIDATE(t);
    KZ_ROOT_VEC(ctx, vals);
    if (collect_sorted(ctx, &t, argv[0], &vals)) return JS_EXCEPTION;
    TA n;
    ta_state(t.o, &n);
    for (uint32_t k = 0; k < vals.len && k < n.len; k++)
        if (ta_store(ctx, &n, k, vals.v[k])) return JS_EXCEPTION;
    return this_val;
}

static JSValue ta_to_sorted(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (argv[0] != JS_UNDEFINED && !kz_is_callable(argv[0])) return kz_throw_type(ctx, "The comparison function must be either a function or undefined");
    VALIDATE(t);
    KZ_ROOTS(ctx, r, 1);
    r[0] = ta_create_same(ctx, t.o, t.len);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_VEC(ctx, vals);
    if (collect_sorted(ctx, &t, argv[0], &vals)) return JS_EXCEPTION;
    TA d;
    ta_state(JV_OBJ(r[0]), &d);
    for (uint32_t k = 0; k < vals.len && k < d.len; k++)
        if (ta_store(ctx, &d, k, vals.v[k])) return JS_EXCEPTION;
    return r[0];
}

static JSValue ta_to_reversed(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    VALIDATE(t);
    KZ_ROOT(ctx, a);
    a = ta_create_same(ctx, t.o, t.len);
    if (a == JS_EXCEPTION) return a;
    TA d, s;
    ta_state(JV_OBJ(a), &d);
    ta_state(t.o, &s);
    for (size_t k = 0; k < t.len && k < d.len && k < s.len; k++)
        memcpy(ta_data(&d) + k * d.es, ta_data(&s) + (s.len - 1 - k) * s.es, d.es);
    return a;
}

static JSValue ta_with(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    VALIDATE(t);
    double rel;
    if (kz_to_integer_or_inf(ctx, argv[0], &rel)) return JS_EXCEPTION;
    double actual = rel >= 0 ? rel : (double)t.len + rel;
    KZ_ROOTS(ctx, r, 2);
    r[0] = ta_convert(ctx, t.kind, argv[1]);
    if (r[0] == JS_EXCEPTION) return r[0];
    TA c;
    if (!valid_index(t.o, actual, &c)) return kz_throw_range(ctx, "Invalid typed array index");
    r[1] = ta_create_same(ctx, t.o, t.len);
    if (r[1] == JS_EXCEPTION) return r[1];
    /* Get(O, k) then Set(A, k, ...): when the conversions above shrank the buffer, the missing elements read as undefined
       and become NaN (a TypeError for the BigInt kinds, which cannot convert undefined) */
    TA d, s;
    ta_state(JV_OBJ(r[1]), &d);
    ta_state(t.o, &s);
    bool plain = !s.oob && s.len >= t.len && !d.oob && d.len >= t.len && d.kind == s.kind;
    for (size_t k = 0; k < t.len; k++) {
        if ((double)k == actual) { if (ta_store(ctx, &d, k, r[0])) return JS_EXCEPTION; continue; }
        if (plain) { memcpy(ta_data(&d) + k * d.es, ta_data(&s) + k * s.es, d.es); continue; }
        JSValue v = ta_get_element(ctx, t.o, (double)k);
        if (v == JS_EXCEPTION || ta_set_element(ctx, JV_OBJ(r[1]), (double)k, v) < 0) return JS_EXCEPTION;
    }
    return r[1];
}

static JSValue ta_subarray(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!is_ta(this_val)) return kz_throw_type(ctx, "this is not a typed array.");
    JSObject *o = JV_OBJ(this_val);
    TA t;
    ta_state(o, &t);
    uint64_t src_len = t.oob ? 0 : t.len, begin, fin;
    if (kz_to_relative_index(ctx, argv[0], src_len, 0, &begin)) return JS_EXCEPTION;
    JSValue args[3] = { o->u.ta.buffer, jv_number((double)(o->u.ta.offset + begin * t.es)), JS_UNDEFINED };
    if (o->u.ta.track && argv[1] == JS_UNDEFINED) return ta_species_create(ctx, o, 2, args);
    if (kz_to_relative_index(ctx, argv[1], src_len, src_len, &fin)) return JS_EXCEPTION;
    uint64_t new_len = fin > begin ? fin - begin : 0;
    args[2] = jv_number((double)new_len);
    return ta_species_create(ctx, o, 3, args);
}

/* ================================================================== setup */

static const KzFuncDef typed_statics[] = {
    KZ_FN_ARGS("from", ta_from, 1, 3),
    KZ_FN("of", ta_of, 0),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef typed_proto_funcs[] = {
    KZ_FN("at", ta_at, 1),
    KZ_GETTER_MAGIC("buffer", ta_getter, TG_BUFFER),
    KZ_GETTER_MAGIC("byteLength", ta_getter, TG_BYTELENGTH),
    KZ_GETTER_MAGIC("byteOffset", ta_getter, TG_BYTEOFFSET),
    KZ_FN_ARGS("copyWithin", ta_copy_within, 2, 3),
    KZ_FN_MAGIC("entries", ta_iter, 0, 2),
    KZ_FN_MAGIC_ARGS("every", ta_each, 1, 2, 0),
    KZ_FN_ARGS("fill", ta_fill, 1, 3),
    KZ_FN_ARGS("filter", ta_filter, 1, 2),
    KZ_FN_MAGIC_ARGS("find", ta_each, 1, 2, 3),
    KZ_FN_MAGIC_ARGS("findIndex", ta_each, 1, 2, 4),
    KZ_FN_MAGIC_ARGS("findLast", ta_each, 1, 2, 5),
    KZ_FN_MAGIC_ARGS("findLastIndex", ta_each, 1, 2, 6),
    KZ_FN_MAGIC_ARGS("forEach", ta_each, 1, 2, 2),
    KZ_FN_MAGIC_ARGS("includes", ta_search, 1, 2, 0),
    KZ_FN_MAGIC_ARGS("indexOf", ta_search, 1, 2, 1),
    KZ_FN("join", ta_join, 1),
    KZ_FN_MAGIC("keys", ta_iter, 0, 0),
    KZ_FN_MAGIC_ARGS("lastIndexOf", ta_search, 1, 2, 2),
    KZ_GETTER_MAGIC("length", ta_getter, TG_LENGTH),
    KZ_FN_ARGS("map", ta_map, 1, 2),
    KZ_FN_MAGIC_ARGS("reduce", ta_reduce, 1, 2, 0),
    KZ_FN_MAGIC_ARGS("reduceRight", ta_reduce, 1, 2, 1),
    KZ_FN("reverse", ta_reverse, 0),
    KZ_FN_ARGS("set", ta_set, 1, 2),
    KZ_FN_ARGS("slice", ta_slice, 2, 2),
    KZ_FN_MAGIC_ARGS("some", ta_each, 1, 2, 1),
    KZ_FN("sort", ta_sort, 1),
    KZ_FN_ARGS("subarray", ta_subarray, 2, 2),
    KZ_FN("toLocaleString", ta_to_locale_string, 0),
    KZ_FN("toReversed", ta_to_reversed, 0),
    KZ_FN("toSorted", ta_to_sorted, 1),
    KZ_FN_ARGS("with", ta_with, 2, 2),
    KZ_GETTER("[Symbol.toStringTag]", ta_to_string_tag),
};

int kz_init_typed_arrays(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    for (int i = 0; i < NKINDS; i++) kz_exotic[KZ_CLASS_UINT8C_ARRAY + i] = &typed_exotic;
    KZ_ROOTS(ctx, r, 5);
    r[0] = kz_new_object(ctx);                              /* %TypedArray%.prototype */
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_TYPEDARRAY_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, abstract_ctor, "TypedArray", 0, 0, KZ_I_TYPEDARRAY_PROTO, KZ_I_TYPEDARRAY);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], typed_statics, KZ_COUNTOF(typed_statics))) return -1;
    if (kz_define_functions(ctx, r[0], typed_proto_funcs, KZ_COUNTOF(typed_proto_funcs))) return -1;
    /* toString is Array.prototype.toString; values is @@iterator */
    r[2] = kz_get(ctx, kz_intr(ctx, KZ_I_ARRAY_PROTO), rt->atom[KZ_ATOM_toString], kz_intr(ctx, KZ_I_ARRAY_PROTO));
    if (r[2] == JS_EXCEPTION || kz_define_value_atom(ctx, r[0], KZ_ATOM_toString, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    r[2] = kz_new_native_c(ctx, ta_iter, "values", 0, 1);
    if (r[2] == JS_EXCEPTION) return -1;
    if (kz_define_value_atom(ctx, r[0], KZ_ATOM_values, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_iterator], r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    for (int i = 0; i < NKINDS; i++) {
        r[2] = kz_new_object_proto(ctx, r[0]);
        if (r[2] == JS_EXCEPTION) return -1;
        ctx->intrinsic[proto_of_kind(i)] = r[2];
        r[3] = kz_make_ctor(ctx, ta_ctor, kind_names[i], 3, i, proto_of_kind(i), ctor_of_kind(i));
        if (r[3] == JS_EXCEPTION) return -1;
        if (kz_obj_set_proto_raw(ctx, JV_OBJ(r[3]), r[1])) return -1;
        JSValue bpe = JV_MKINT((int)kz_ta_size[i]);
        if (kz_define_value_atom(ctx, r[3], KZ_ATOM_BYTES_PER_ELEMENT, bpe, 0)) return -1;
        if (kz_define_value_atom(ctx, r[2], KZ_ATOM_BYTES_PER_ELEMENT, bpe, 0)) return -1;
        JSValue name = kz_atom(ctx, kind_names[i]);
        if (name == JS_EXCEPTION || kz_define_value(ctx, ctx->global, name, r[3], KZ_PROP_W | KZ_PROP_C)) return -1;
    }
    return 0;
}
