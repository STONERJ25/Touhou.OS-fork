/*
 * arraybuffer.c: ArrayBuffer and SharedArrayBuffer (ECMA-262 25.1, 25.2), DataView (25.3), Atomics (25.4), and the
 * element read/write helpers the typed arrays (typed.c) share.
 *
 * A buffer is an object of class ARRAYBUFFER or SHAREDARRAYBUFFER whose u.ab holds the bytes.  A resizable (or growable)
 * buffer reserves its maximum length up front, so resizing never moves the data.  Detaching frees the bytes.  The engine
 * is single-threaded, so a SharedArrayBuffer is a buffer that cannot be detached or transferred, and Atomics is the
 * ordinary read-modify-write on its elements.
 */
#include "jsint.h"

/* the element kinds, in the order of the typed array classes (KZ_CLASS_UINT8C_ARRAY ..) */
const uint8_t kz_ta_size[11] = { 1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8 };

#define MAX_BUFFER_BYTES 0x7FFFFFFFu        /* a buffer larger than 2 GiB is a RangeError, whatever the memory limit */

static bool is_ab(JSValue v) { return JV_IS_OBJECT(v) && kz_class(JV_OBJ(v)) == KZ_CLASS_ARRAYBUFFER; }
static bool is_sab(JSValue v) { return JV_IS_OBJECT(v) && kz_class(JV_OBJ(v)) == KZ_CLASS_SHAREDARRAYBUFFER; }

/* ================================================================== elements */

JSValue kz_elem_load(int kind, const uint8_t *p) {
    switch (kind) {
    case 0: case 2: return JV_MKINT(p[0]);
    case 1: return JV_MKINT((int8_t)p[0]);
    case 3: { int16_t v; memcpy(&v, p, 2); return JV_MKINT(v); }
    case 4: { uint16_t v; memcpy(&v, p, 2); return JV_MKINT(v); }
    case 5: { int32_t v; memcpy(&v, p, 4); return JV_MKINT(v); }
    case 6: { uint32_t v; memcpy(&v, p, 4); return jv_uint32(v); }
    case 7: { float v; memcpy(&v, p, 4); return jv_number((double)v); }
    case 8: { double v; memcpy(&v, p, 8); return jv_number(v); }
    default: return JS_UNDEFINED;               /* (BigInt kinds: kz_elem_load_big) */
    }
}

JSValue kz_elem_load_big(JSContext *ctx, int kind, const uint8_t *p) {
    if (kind == 9) { int64_t v; memcpy(&v, p, 8); return kz_bigint_from_int64(ctx, v); }
    uint64_t v;
    memcpy(&v, p, 8);
    return kz_bigint_from_uint64(ctx, v);
}

/* the conversion of ToNumber's result to an element (the specification's NumericToRawBytes) */
static uint8_t clamp8(double d) {
    if (d != d || d <= 0) return 0;
    if (d >= 255) return 255;
    double f = kz_math_floor(d);
    if (f + 0.5 < d) return (uint8_t)(f + 1);
    if (d < f + 0.5) return (uint8_t)f;
    return (uint8_t)(((int)f & 1) ? f + 1 : f);         /* a tie rounds to even */
}

/* writes the number d as an element of a non-BigInt kind */
void kz_elem_store_number(int kind, uint8_t *p, double d) {
    switch (kind) {
    case 0: p[0] = clamp8(d); break;
    case 1: case 2: p[0] = (uint8_t)kz_double_to_int32(d); break;
    case 3: case 4: { uint16_t v = (uint16_t)kz_double_to_int32(d); memcpy(p, &v, 2); break; }
    case 5: case 6: { int32_t v = kz_double_to_int32(d); memcpy(p, &v, 4); break; }
    case 7: { float v = (float)d; memcpy(p, &v, 4); break; }
    default: memcpy(p, &d, 8); break;
    }
}

int kz_elem_store_big(JSContext *ctx, int kind, uint8_t *p, JSValue big) {
    int64_t v;
    if (kz_bigint_to_int64(ctx, big, &v)) return -1;
    (void)kind;
    memcpy(p, &v, 8);
    return 0;
}

/* ================================================================== buffers */

bool kz_buffer_is_detached(JSObject *ab) { return ab->u.ab.detached; }

int kz_detach_array_buffer(JSContext *ctx, JSObject *ab) {
    if (kz_class(ab) != KZ_CLASS_ARRAYBUFFER) { kz_throw_type(ctx, "Cannot detach a shared buffer"); return -1; }
    if (ab->u.ab.data && !ab->u.ab.external) kz_free(ctx->rt, ab->u.ab.data);
    ab->u.ab.data = NULL;
    ab->u.ab.len = 0;
    ab->u.ab.detached = true;
    return 0;
}

/* AllocateArrayBuffer / AllocateSharedArrayBuffer: the prototype comes from `ctor` (a constructor, or undefined for the
   intrinsic); max < 0: not resizable */
static JSValue alloc_buffer(JSContext *ctx, JSValue ctor, bool shared, uint64_t len, int64_t max) {
    KZ_ROOTS(ctx, r, 2);
    KzIntrinsic proto = shared ? KZ_I_SHAREDARRAYBUFFER_PROTO : KZ_I_ARRAYBUFFER_PROTO;
    KzClassId cls = shared ? KZ_CLASS_SHAREDARRAYBUFFER : KZ_CLASS_ARRAYBUFFER;
    if (ctor == JS_UNDEFINED) r[0] = kz_new_object_class(ctx, cls, kz_intr(ctx, proto));
    else r[0] = kz_ordinary_create_from_ctor(ctx, ctor, proto, cls);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (max >= 0 && len > (uint64_t)max) return kz_throw_range(ctx, "Invalid array buffer max length");
    uint64_t cap = max >= 0 ? (uint64_t)max : len;
    if (cap > MAX_BUFFER_BYTES) return kz_throw_range(ctx, "Array buffer allocation failed");
    JSObject *b = JV_OBJ(r[0]);
    if (cap) {
        b->u.ab.data = kz_mallocz(ctx, (size_t)cap);
        if (!b->u.ab.data) return JS_EXCEPTION;
    }
    b->u.ab.len = (size_t)len;
    b->u.ab.max_len = (size_t)cap;
    b->u.ab.resizable = max >= 0;
    b->u.ab.shared = shared;
    return r[0];
}

JSValue kz_new_array_buffer(JSContext *ctx, size_t len) { return alloc_buffer(ctx, JS_UNDEFINED, false, len, -1); }

/* the maxByteLength option: -1 absent */
static int get_max_option(JSContext *ctx, JSValue options, int64_t *out) {
    *out = -1;
    if (!JV_IS_OBJECT(options)) return 0;
    KZ_ROOT(ctx, m);
    m = kz_get_atom(ctx, options, KZ_ATOM_maxByteLength);
    if (m == JS_EXCEPTION) return -1;
    if (m == JS_UNDEFINED) return 0;
    uint64_t v;
    if (kz_to_index(ctx, m, &v)) return -1;
    if (v > MAX_BUFFER_BYTES) { kz_throw_range(ctx, "Invalid array buffer max length"); return -1; }
    *out = (int64_t)v;
    return 0;
}

static JSValue buffer_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    bool shared = magic != 0;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor %s requires 'new'", shared ? "SharedArrayBuffer" : "ArrayBuffer");
    uint64_t len;
    if (kz_to_index(ctx, argv[0], &len)) return JS_EXCEPTION;
    int64_t max;
    if (get_max_option(ctx, argv[1], &max)) return JS_EXCEPTION;
    /* AllocateArrayBuffer compares the two before it creates the object (which reads new.target.prototype) */
    if (max >= 0 && len > (uint64_t)max) return kz_throw_range(ctx, "Invalid array buffer max length");
    return alloc_buffer(ctx, rt->new_target, shared, len, max);
}

static JSValue buffer_is_view(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)ctx; (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_OBJECT(argv[0])) return JS_FALSE;
    KzClassId c = kz_class(JV_OBJ(argv[0]));
    return JV_MKBOOL(KZ_IS_TYPED_ARRAY(c) || c == KZ_CLASS_DATAVIEW);
}

/* this must be a (non-shared / shared) buffer */
static JSObject *this_buffer(JSContext *ctx, JSValue v, bool shared, const char *what) {
    if (JV_IS_OBJECT(v) && kz_class(JV_OBJ(v)) == (shared ? KZ_CLASS_SHAREDARRAYBUFFER : KZ_CLASS_ARRAYBUFFER)) return JV_OBJ(v);
    kz_throw_type(ctx, "%s called on an incompatible receiver", what);
    return NULL;
}

enum { BP_BYTELENGTH, BP_MAXBYTELENGTH, BP_RESIZABLE, BP_DETACHED };

static JSValue buffer_getter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    bool shared = (magic & 0x100) != 0;
    JSObject *b = this_buffer(ctx, this_val, shared, "ArrayBuffer getter");
    if (!b) return JS_EXCEPTION;
    switch (magic & 0xFF) {
    case BP_BYTELENGTH: return jv_number((double)b->u.ab.len);
    case BP_MAXBYTELENGTH: return jv_number((double)(b->u.ab.resizable ? (b->u.ab.detached ? 0 : b->u.ab.max_len) : b->u.ab.len));
    case BP_RESIZABLE: return JV_MKBOOL(b->u.ab.resizable);
    default: return JV_MKBOOL(b->u.ab.detached);
    }
}

static JSValue buffer_slice(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    bool shared = magic != 0;
    JSObject *b = this_buffer(ctx, this_val, shared, "slice");
    if (!b) return JS_EXCEPTION;
    if (b->u.ab.detached) return kz_throw_type(ctx, "Cannot perform slice on a detached ArrayBuffer");
    KZ_ROOTS(ctx, r, 3);
    uint64_t len = b->u.ab.len, first, final;
    if (kz_to_relative_index(ctx, argv[0], len, 0, &first)) return JS_EXCEPTION;
    if (kz_to_relative_index(ctx, argv[1], len, len, &final)) return JS_EXCEPTION;
    uint64_t new_len = final > first ? final - first : 0;
    r[0] = kz_species_constructor(ctx, this_val, kz_intr(ctx, shared ? KZ_I_SHAREDARRAYBUFFER : KZ_I_ARRAYBUFFER));
    if (r[0] == JS_EXCEPTION) return r[0];
    JSValue arg = jv_number((double)new_len);
    r[1] = kz_construct(ctx, r[0], 1, &arg, r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSObject *nb = this_buffer(ctx, r[1], shared, "slice");
    if (!nb) return JS_EXCEPTION;
    if (nb->u.ab.detached) return kz_throw_type(ctx, "slice: the new ArrayBuffer is detached");
    if (nb == b) return kz_throw_type(ctx, "slice: the species constructor returned the same ArrayBuffer");
    if (nb->u.ab.len < new_len) return kz_throw_type(ctx, "slice: the new ArrayBuffer is too small");
    if (b->u.ab.detached) return kz_throw_type(ctx, "Cannot perform slice on a detached ArrayBuffer");
    size_t cur = b->u.ab.len;
    if (first < cur) {
        size_t n = (size_t)(final < cur ? final : cur) - (size_t)first;
        if (n > new_len) n = (size_t)new_len;
        if (n) memmove(nb->u.ab.data, b->u.ab.data + first, n);
    }
    return r[1];
}

static JSValue buffer_resize(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    bool shared = magic != 0;
    JSObject *b = this_buffer(ctx, this_val, shared, shared ? "grow" : "resize");
    if (!b) return JS_EXCEPTION;
    if (!b->u.ab.resizable) return kz_throw_type(ctx, "Method called on a non-resizable buffer");
    uint64_t n;
    if (kz_to_index(ctx, argv[0], &n)) return JS_EXCEPTION;
    if (b->u.ab.detached) return kz_throw_type(ctx, "Cannot resize a detached ArrayBuffer");
    if (n > b->u.ab.max_len) return kz_throw_range(ctx, "Invalid array buffer length");
    if (shared && n < b->u.ab.len) return kz_throw_range(ctx, "SharedArrayBuffer.prototype.grow cannot shrink the buffer");
    if (n < b->u.ab.len) memset(b->u.ab.data + n, 0, b->u.ab.len - (size_t)n);
    b->u.ab.len = (size_t)n;
    return JS_UNDEFINED;
}

/* ArrayBuffer.prototype.transfer (magic 0: keeps resizability) and transferToFixedLength (1) */
static JSValue buffer_transfer(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSObject *b = this_buffer(ctx, this_val, false, "transfer");
    if (!b) return JS_EXCEPTION;
    uint64_t n;
    if (argv[0] == JS_UNDEFINED) n = b->u.ab.len;
    else if (kz_to_index(ctx, argv[0], &n)) return JS_EXCEPTION;
    if (b->u.ab.detached) return kz_throw_type(ctx, "Cannot transfer a detached ArrayBuffer");
    int64_t max = (magic == 0 && b->u.ab.resizable) ? (int64_t)b->u.ab.max_len : -1;
    KZ_ROOT(ctx, nb);
    nb = alloc_buffer(ctx, JS_UNDEFINED, false, n, max);
    if (nb == JS_EXCEPTION) return nb;
    size_t copy = b->u.ab.len < n ? b->u.ab.len : (size_t)n;
    if (copy) memcpy(JV_OBJ(nb)->u.ab.data, b->u.ab.data, copy);
    if (kz_detach_array_buffer(ctx, b)) return JS_EXCEPTION;
    return nb;
}

/* ================================================================== DataView */

static JSObject *this_view(JSContext *ctx, JSValue v) {
    if (JV_IS_OBJECT(v) && kz_class(JV_OBJ(v)) == KZ_CLASS_DATAVIEW) return JV_OBJ(v);
    kz_throw_type(ctx, "DataView method called on an incompatible receiver");
    return NULL;
}

/* the view's byte length now: false when the view is out of bounds (detached, or the buffer shrank) */
static bool view_size(JSObject *v, size_t *size) {
    JSObject *b = JV_OBJ(v->u.ta.buffer);
    if (b->u.ab.detached) return false;
    size_t blen = b->u.ab.len, off = v->u.ta.offset;
    if (v->u.ta.track) {
        if (off > blen) return false;
        *size = blen - off;
        return true;
    }
    if (off + v->u.ta.length > blen) return false;
    *size = v->u.ta.length;
    return true;
}

static JSValue dataview_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor DataView requires 'new'");
    KZ_ROOT(ctx, o);
    if (!is_ab(argv[0]) && !is_sab(argv[0])) return kz_throw_type(ctx, "First argument to DataView constructor must be an ArrayBuffer");
    JSObject *buf = JV_OBJ(argv[0]);
    uint64_t offset;
    if (kz_to_index(ctx, argv[1], &offset)) return JS_EXCEPTION;
    if (buf->u.ab.detached) return kz_throw_type(ctx, "Cannot construct a DataView on a detached ArrayBuffer");
    uint64_t blen = buf->u.ab.len;
    if (offset > blen) return kz_throw_range(ctx, "Start offset is outside the bounds of the buffer");
    bool fixed = !buf->u.ab.resizable;
    uint64_t vlen = 0;
    bool track = false;
    if (argv[2] == JS_UNDEFINED) {
        if (fixed) vlen = blen - offset;
        else track = true;
    } else {
        if (kz_to_index(ctx, argv[2], &vlen)) return JS_EXCEPTION;
        if (offset + vlen > blen) return kz_throw_range(ctx, "Invalid DataView length");
    }
    o = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_DATAVIEW_PROTO, KZ_CLASS_DATAVIEW);
    if (o == JS_EXCEPTION) return o;
    if (buf->u.ab.detached) return kz_throw_type(ctx, "Cannot construct a DataView on a detached ArrayBuffer");
    blen = buf->u.ab.len;
    if (offset > blen) return kz_throw_range(ctx, "Start offset is outside the bounds of the buffer");
    if (argv[2] != JS_UNDEFINED && offset + vlen > blen) return kz_throw_range(ctx, "Invalid DataView length");
    JSObject *v = JV_OBJ(o);
    v->u.ta.buffer = argv[0];
    v->u.ta.offset = (size_t)offset;
    v->u.ta.length = (size_t)vlen;
    v->u.ta.track = track;
    return o;
}

static JSValue dataview_getter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    JSObject *v = this_view(ctx, this_val);
    if (!v) return JS_EXCEPTION;
    size_t size;
    switch (magic) {
    case 0: return v->u.ta.buffer;
    /* (ES2025 and test262: like the typed array getters, these read 0 for a detached or out-of-bounds view; the get<Type> and
       set<Type> methods still throw) */
    case 1:
        if (!view_size(v, &size)) return JV_MKINT(0);
        return jv_number((double)size);
    default:
        if (!view_size(v, &size)) return JV_MKINT(0);
        return jv_number((double)v->u.ta.offset);
    }
}

/* get<Type> (magic = kind | 0x100 for set) */
static JSValue dataview_access(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    bool is_set = (magic & 0x100) != 0;
    int kind = magic & 0xFF;
    JSObject *v = this_view(ctx, this_val);
    if (!v) return JS_EXCEPTION;
    uint64_t idx;
    if (kz_to_index(ctx, argv[0], &idx)) return JS_EXCEPTION;
    KZ_ROOT(ctx, num);
    if (is_set) {
        if (kind >= 9) num = kz_to_bigint(ctx, argv[1]);
        else num = kz_to_number_value(ctx, argv[1]);
        if (num == JS_EXCEPTION) return num;
    }
    bool little = kz_to_bool(argv[is_set ? 2 : 1]);
    size_t size;
    if (!view_size(v, &size)) return kz_throw_type(ctx, "Cannot perform DataView method on a detached or out-of-bounds view");
    size_t es = kz_ta_size[kind];
    if (idx + es > size) return kz_throw_range(ctx, "Offset is outside the bounds of the DataView");
    uint8_t *p = JV_OBJ(v->u.ta.buffer)->u.ab.data + v->u.ta.offset + idx;
    uint8_t tmp[8];
    if (!is_set) {
        for (size_t i = 0; i < es; i++) tmp[i] = little ? p[i] : p[es - 1 - i];
        if (kind >= 9) return kz_elem_load_big(ctx, kind, tmp);
        return kz_elem_load(kind, tmp);
    }
    if (kind >= 9) { if (kz_elem_store_big(ctx, kind, tmp, num)) return JS_EXCEPTION; }
    else kz_elem_store_number(kind, tmp, jv_num(num));
    for (size_t i = 0; i < es; i++) p[i] = little ? tmp[i] : tmp[es - 1 - i];
    return JS_UNDEFINED;
}

/* ================================================================== Atomics */

/* ValidateIntegerTypedArray (25.4.3.1): the typed array object and its element kind, or NULL with an exception */
static JSObject *atomic_array(JSContext *ctx, JSValue ta, bool waitable, int *kind) {
    if (!JV_IS_OBJECT(ta) || !KZ_IS_TYPED_ARRAY(kz_class(JV_OBJ(ta)))) { kz_throw_type(ctx, "Atomics operation requires an integer typed array"); return NULL; }
    JSObject *o = JV_OBJ(ta);
    int k = (int)kz_class(o) - KZ_CLASS_UINT8C_ARRAY;
    if (waitable ? (k != 5 && k != 9) : (k == 0 || k == 7 || k == 8)) { kz_throw_type(ctx, "Atomics operation not supported on this typed array"); return NULL; }
    if (kz_is_detached(o) || kz_typed_oob(o)) { kz_throw_type(ctx, "Cannot perform Atomics operation on a detached or out-of-bounds typed array"); return NULL; }
    *kind = k;
    return o;
}

/* ValidateAtomicAccess: ToIndex and the range check against the length as it was before the conversion (which may run
   user code that resizes the buffer; the operation revalidates before it touches memory) */
static int atomic_index(JSContext *ctx, JSObject *o, JSValue index, uint64_t *i) {
    uint64_t len = kz_typed_length(o);
    if (kz_to_index(ctx, index, i)) return -1;
    if (*i >= len) { kz_throw_range(ctx, "Invalid atomic access index"); return -1; }
    return 0;
}

/* RevalidateAtomicAccess after the arguments were converted (user code may have detached or shrunk the buffer) */
static uint8_t *atomic_revalidate(JSContext *ctx, JSObject *o, int kind, uint64_t i) {
    if (kz_is_detached(o) || kz_typed_oob(o)) { kz_throw_type(ctx, "Cannot perform Atomics operation on a detached or out-of-bounds typed array"); return NULL; }
    if (i >= kz_typed_length(o)) { kz_throw_range(ctx, "Invalid atomic access index"); return NULL; }
    return JV_OBJ(o->u.ta.buffer)->u.ab.data + o->u.ta.offset + i * kz_ta_size[kind];
}

/* converts the operand: ToBigInt for the BigInt arrays, else ToIntegerOrInfinity (as a number) */
static int atomic_operand(JSContext *ctx, int kind, JSValue v, JSValue *out) {
    if (kind >= 9) { *out = kz_to_bigint(ctx, v); return *out == JS_EXCEPTION ? -1 : 0; }
    double d;
    if (kz_to_integer_or_inf(ctx, v, &d)) return -1;
    *out = jv_number(d == 0 ? 0 : d);
    return 0;
}

/* read-modify-write: magic 0 add, 1 and, 2 exchange, 3 or, 4 sub, 5 xor; returns the old value */
static JSValue atomics_rmw(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], false, &kind);
    if (!o) return JS_EXCEPTION;
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    KZ_ROOT(ctx, v);
    if (atomic_operand(ctx, kind, argv[2], &v)) return JS_EXCEPTION;
    uint8_t *p = atomic_revalidate(ctx, o, kind, i);
    if (!p) return JS_EXCEPTION;
    size_t es = kz_ta_size[kind];
    uint8_t old[8], nw[8];
    memcpy(old, p, es);
    if (kind >= 9) {
        int64_t a, b;
        memcpy(&a, old, 8);
        if (kz_bigint_to_int64(ctx, v, &b)) return JS_EXCEPTION;
        uint64_t ua = (uint64_t)a, ub = (uint64_t)b, r;
        switch (magic) { case 0: r = ua + ub; break; case 1: r = ua & ub; break; case 2: r = ub; break; case 3: r = ua | ub; break; case 4: r = ua - ub; break; default: r = ua ^ ub; break; }
        memcpy(p, &r, 8);
        return kz_elem_load_big(ctx, kind, old);
    }
    int64_t a = (int64_t)jv_num(kz_elem_load(kind, old));
    int64_t b = kz_double_to_int32(jv_num(v));
    int64_t r;
    switch (magic) { case 0: r = a + b; break; case 1: r = a & b; break; case 2: r = b; break; case 3: r = a | b; break; case 4: r = a - b; break; default: r = a ^ b; break; }
    kz_elem_store_number(kind, nw, (double)r);
    memcpy(p, nw, es);
    return kz_elem_load(kind, old);
}

static JSValue atomics_load(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], false, &kind);
    if (!o) return JS_EXCEPTION;
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    uint8_t *p = atomic_revalidate(ctx, o, kind, i);
    if (!p) return JS_EXCEPTION;
    return kind >= 9 ? kz_elem_load_big(ctx, kind, p) : kz_elem_load(kind, p);
}

static JSValue atomics_store(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], false, &kind);
    if (!o) return JS_EXCEPTION;
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    KZ_ROOT(ctx, v);
    if (atomic_operand(ctx, kind, argv[2], &v)) return JS_EXCEPTION;
    uint8_t *p = atomic_revalidate(ctx, o, kind, i);
    if (!p) return JS_EXCEPTION;
    if (kind >= 9) { if (kz_elem_store_big(ctx, kind, p, v)) return JS_EXCEPTION; }
    else kz_elem_store_number(kind, p, jv_num(v));
    return v;
}

static JSValue atomics_compare_exchange(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], false, &kind);
    if (!o) return JS_EXCEPTION;
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 2);
    if (atomic_operand(ctx, kind, argv[2], &r[0]) || atomic_operand(ctx, kind, argv[3], &r[1])) return JS_EXCEPTION;
    uint8_t *p = atomic_revalidate(ctx, o, kind, i);
    if (!p) return JS_EXCEPTION;
    size_t es = kz_ta_size[kind];
    uint8_t old[8], expect[8], repl[8];
    memcpy(old, p, es);
    if (kind >= 9) {
        if (kz_elem_store_big(ctx, kind, expect, r[0]) || kz_elem_store_big(ctx, kind, repl, r[1])) return JS_EXCEPTION;
    } else {
        kz_elem_store_number(kind, expect, jv_num(r[0]));
        kz_elem_store_number(kind, repl, jv_num(r[1]));
    }
    if (!memcmp(old, expect, es)) memcpy(p, repl, es);
    return kind >= 9 ? kz_elem_load_big(ctx, kind, old) : kz_elem_load(kind, old);
}

static JSValue atomics_is_lock_free(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    double n;
    if (kz_to_integer_or_inf(ctx, argv[0], &n)) return JS_EXCEPTION;
    return JV_MKBOOL(n == 1 || n == 2 || n == 4 || n == 8);
}

/*
 * Blocks the (only) agent for t milliseconds, t may be infinite: nobody can wake it, so the wait ends when the time is up
 * or, for an infinite wait, when the host's interrupt handler stops the script.  Returns 0 when the time is up, -1 when
 * interrupted.  The host's sleep hook (if any) does the sleeping; without one this polls the clock.
 */
static int block_for(JSContext *ctx, double t) {
    JSRuntime *rt = ctx->rt;
    double start = rt->hooks.now_ms ? rt->hooks.now_ms(rt->hooks.opaque) : 0;
    for (;;) {
        kz_poll_interrupt(ctx);
        if (kz_has_exception(ctx)) return -1;
        double now = rt->hooks.now_ms ? rt->hooks.now_ms(rt->hooks.opaque) : start + t;       /* without a clock the time passes at once */
        double left = t - (now - start);
        if (left <= 0) return 0;
        if (rt->hooks.sleep_ms) rt->hooks.sleep_ms(rt->hooks.opaque, left < 20 ? left : 20);
    }
}

/* DoWait (25.4.3.14) in its synchronous form; the order of the steps is the specification's: every argument is
   validated and converted before anything blocks */
static JSValue atomics_wait(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], true, &kind);
    if (!o) return JS_EXCEPTION;
    if (kz_class(JV_OBJ(o->u.ta.buffer)) != KZ_CLASS_SHAREDARRAYBUFFER) return kz_throw_type(ctx, "Atomics.wait requires a shared typed array");
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    KZ_ROOT(ctx, v);
    int64_t want;
    if (kind >= 9) {
        v = kz_to_bigint(ctx, argv[2]);
        if (v == JS_EXCEPTION || kz_bigint_to_int64(ctx, v, &want)) return JS_EXCEPTION;
    } else {
        double d;
        if (kz_to_number(ctx, argv[2], &d)) return JS_EXCEPTION;
        want = kz_double_to_int32(d);
    }
    double q, t;
    if (kz_to_number(ctx, argv[3], &q)) return JS_EXCEPTION;
    t = (q != q || q == KZ_INFINITY) ? KZ_INFINITY : q < 0 ? 0 : q;
    uint8_t *p = atomic_revalidate(ctx, o, kind, i);
    if (!p) return JS_EXCEPTION;
    int64_t cur;
    if (kind >= 9) memcpy(&cur, p, 8);
    else { int32_t c32; memcpy(&c32, p, 4); cur = c32; }
    if (cur != want) return kz_str_from_cstr(ctx, "not-equal");
    if (block_for(ctx, t)) return JS_EXCEPTION;
    return kz_str_from_cstr(ctx, "timed-out");
}

static JSValue atomics_notify(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind;
    JSObject *o = atomic_array(ctx, argv[0], true, &kind);
    if (!o) return JS_EXCEPTION;
    uint64_t i;
    if (atomic_index(ctx, o, argv[1], &i)) return JS_EXCEPTION;
    if (argv[2] != JS_UNDEFINED) {
        double c;
        if (kz_to_integer_or_inf(ctx, argv[2], &c)) return JS_EXCEPTION;
    }
    return JV_MKINT(0);                                 /* nobody is waiting: this agent cannot block */
}

/* ================================================================== setup */

static const KzFuncDef ab_funcs[] = {
    KZ_FN("isView", buffer_is_view, 1),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};
static const KzFuncDef ab_proto_funcs[] = {
    KZ_GETTER_MAGIC("byteLength", buffer_getter, BP_BYTELENGTH),
    KZ_GETTER_MAGIC("detached", buffer_getter, BP_DETACHED),
    KZ_GETTER_MAGIC("maxByteLength", buffer_getter, BP_MAXBYTELENGTH),
    KZ_GETTER_MAGIC("resizable", buffer_getter, BP_RESIZABLE),
    KZ_FN_MAGIC("resize", buffer_resize, 1, 0),
    KZ_FN_MAGIC_ARGS("slice", buffer_slice, 2, 2, 0),
    KZ_FN_MAGIC("transfer", buffer_transfer, 0, 0),
    KZ_FN_MAGIC("transferToFixedLength", buffer_transfer, 0, 1),
};
static const KzFuncDef sab_funcs[] = { KZ_GETTER("[Symbol.species]", kz_native_return_this) };
static const KzFuncDef sab_proto_funcs[] = {
    KZ_GETTER_MAGIC("byteLength", buffer_getter, BP_BYTELENGTH | 0x100),
    KZ_FN_MAGIC("grow", buffer_resize, 1, 1),
    KZ_GETTER_MAGIC("growable", buffer_getter, BP_RESIZABLE | 0x100),
    KZ_GETTER_MAGIC("maxByteLength", buffer_getter, BP_MAXBYTELENGTH | 0x100),
    KZ_FN_MAGIC_ARGS("slice", buffer_slice, 2, 2, 1),
};

#define DV(name, kind) KZ_FN_MAGIC_ARGS("get" name, dataview_access, 1, 2, kind), KZ_FN_MAGIC_ARGS("set" name, dataview_access, 2, 3, (kind) | 0x100)
static const KzFuncDef dv_proto_funcs[] = {
    KZ_GETTER_MAGIC("buffer", dataview_getter, 0),
    KZ_GETTER_MAGIC("byteLength", dataview_getter, 1),
    KZ_GETTER_MAGIC("byteOffset", dataview_getter, 2),
    DV("BigInt64", 9), DV("BigUint64", 10), DV("Float32", 7), DV("Float64", 8), DV("Int8", 1), DV("Int16", 3),
    DV("Int32", 5), DV("Uint8", 2), DV("Uint16", 4), DV("Uint32", 6),
};

static const KzFuncDef atomics_funcs[] = {
    KZ_FN_MAGIC("add", atomics_rmw, 3, 0), KZ_FN_MAGIC("and", atomics_rmw, 3, 1),
    KZ_FN("compareExchange", atomics_compare_exchange, 4), KZ_FN_MAGIC("exchange", atomics_rmw, 3, 2),
    KZ_FN("isLockFree", atomics_is_lock_free, 1), KZ_FN("load", atomics_load, 2), KZ_FN("notify", atomics_notify, 3),
    KZ_FN_MAGIC("or", atomics_rmw, 3, 3), KZ_FN("store", atomics_store, 3), KZ_FN_MAGIC("sub", atomics_rmw, 3, 4),
    KZ_FN("wait", atomics_wait, 4), KZ_FN_MAGIC("xor", atomics_rmw, 3, 5),
};

static int tag(JSContext *ctx, JSValue obj, const char *name) {
    JSValue t = kz_atom(ctx, name);
    if (t == JS_EXCEPTION) return -1;
    return kz_define_value(ctx, obj, ctx->rt->sym[KZ_SYM_toStringTag], t, KZ_PROP_C);
}

int kz_init_typed(JSContext *ctx);
int kz_init_typed_arrays(JSContext *ctx);       /* typed.c */

int kz_init_typed(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 3);
    /* ArrayBuffer */
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ARRAYBUFFER_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, buffer_ctor, "ArrayBuffer", 1, 0, KZ_I_ARRAYBUFFER_PROTO, KZ_I_ARRAYBUFFER);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], ab_funcs, KZ_COUNTOF(ab_funcs)) || kz_define_functions(ctx, r[0], ab_proto_funcs, KZ_COUNTOF(ab_proto_funcs))) return -1;
    if (tag(ctx, r[0], "ArrayBuffer")) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_ArrayBuffer, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* SharedArrayBuffer */
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_SHAREDARRAYBUFFER_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, buffer_ctor, "SharedArrayBuffer", 1, 1, KZ_I_SHAREDARRAYBUFFER_PROTO, -1);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_SHAREDARRAYBUFFER_PROTO] = r[0];
    if (kz_define_functions(ctx, r[1], sab_funcs, KZ_COUNTOF(sab_funcs)) || kz_define_functions(ctx, r[0], sab_proto_funcs, KZ_COUNTOF(sab_proto_funcs))) return -1;
    if (tag(ctx, r[0], "SharedArrayBuffer")) return -1;
    ctx->intrinsic[KZ_I_SHAREDARRAYBUFFER] = r[1];
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_SharedArrayBuffer, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* DataView */
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_DATAVIEW_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, dataview_ctor, "DataView", 1, 0, KZ_I_DATAVIEW_PROTO, -1);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], dv_proto_funcs, KZ_COUNTOF(dv_proto_funcs))) return -1;
    if (tag(ctx, r[0], "DataView")) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_DataView, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* Atomics */
    r[2] = kz_new_object(ctx);
    if (r[2] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[2], atomics_funcs, KZ_COUNTOF(atomics_funcs)) || tag(ctx, r[2], "Atomics")) return -1;
    JSValue k = kz_atom(ctx, "Atomics");
    if (k == JS_EXCEPTION || kz_define_value(ctx, ctx->global, k, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    return kz_init_typed_arrays(ctx);
}
