/*
 * iterators.c: %IteratorPrototype%, %AsyncIteratorPrototype% (ECMA-262 27.1) and the array iterators (23.1.5),
 * which also serve typed arrays and arguments objects.
 */
#include "jsint.h"

/* array iterator state in u.any: a = the object, b = the next index (int), c = the kind (0 keys, 1 values, 2 entries) */

static JSValue array_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);

JSValue kz_array_iterator_new(JSContext *ctx, JSValue obj, int kind) {
    KZ_ROOT(ctx, ro);
    ro = obj;
    JSValue it = kz_new_object_class(ctx, KZ_CLASS_ARRAY_ITERATOR, kz_intr(ctx, KZ_I_ARRAY_ITERATOR_PROTO));
    if (it == JS_EXCEPTION) return it;
    JSObject *o = JV_OBJ(it);
    o->u.any.a = ro;
    o->u.any.b = JV_MKINT(0);
    o->u.any.c = JV_MKINT(kind);
    return it;
}

static JSValue array_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_ARRAY_ITERATOR)) return kz_throw_type(ctx, "next method called on an incompatible receiver");
    JSObject *it = JV_OBJ(this_val);
    JSValue a = it->u.any.a;
    if (a == JS_UNDEFINED) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    double index = jv_num(it->u.any.b);
    int kind = JV_INT(it->u.any.c);
    uint64_t len;
    JSObject *ao = JV_OBJ(a);
    if (KZ_IS_TYPED_ARRAY(kz_class(ao))) {
        /* (a typed array that went out of bounds, or lost its buffer, since the last step: TypeError, not the end) */
        if (kz_is_detached(ao) || kz_typed_oob(ao)) return kz_throw_type(ctx, "Cannot perform %%ArrayIteratorPrototype%%.next on a detached or out-of-bounds typed array");
        len = kz_typed_length(ao);
    } else if (kz_class(ao) == KZ_CLASS_ARRAY) len = ao->u.array.length;
    else if (kz_length_of(ctx, a, &len)) return JS_EXCEPTION;
    if (index >= (double)len) {
        it->u.any.a = JS_UNDEFINED;
        return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    }
    it->u.any.b = jv_number(index + 1);
    JSValue key = jv_number(index);
    if (kind == 0) return kz_create_iter_result(ctx, key, false);
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_get_index(ctx, a, (uint32_t)index);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    if (kind == 1) return kz_create_iter_result(ctx, r[0], false);
    JSValue pair[2] = { key, r[0] };
    r[1] = kz_new_array_from(ctx, 2, pair);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    return kz_create_iter_result(ctx, r[1], false);
}

/* is %ArrayIteratorPrototype%.next still the built-in one? (lets spread take the fast path) */
bool kz_array_iterator_pristine(JSContext *ctx) {
    JSObject *p = JV_OBJ(kz_intr(ctx, KZ_I_ARRAY_ITERATOR_PROTO));
    int i = kz_shape_find(p->shape, ctx->rt->atom[KZ_ATOM_next]);
    if (i < 0 || (p->shape->props[i].attrs & KZ_PROP_ACCESSOR)) return false;
    JSValue f = p->slots[i];
    return kz_is_class(f, KZ_CLASS_NATIVE) && JV_OBJ(f)->u.native.fn == array_iter_next;
}

JSValue kz_native_return_this(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)ctx; (void)argc; (void)argv; (void)magic;
    return this_val;
}

static const KzFuncDef iterator_proto_funcs[] = {
    KZ_FN("[Symbol.iterator]", kz_native_return_this, 0),
};

static const KzFuncDef async_iterator_proto_funcs[] = {
    KZ_FN("[Symbol.asyncIterator]", kz_native_return_this, 0),
};

static const KzFuncDef array_iter_funcs[] = {
    KZ_FN("next", array_iter_next, 0),
};

int kz_init_iterators(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ITERATOR_PROTO] = r[0];
    if (kz_define_functions(ctx, r[0], iterator_proto_funcs, KZ_COUNTOF(iterator_proto_funcs))) return -1;
    r[1] = kz_new_object(ctx);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ASYNC_ITERATOR_PROTO] = r[1];
    if (kz_define_functions(ctx, r[1], async_iterator_proto_funcs, KZ_COUNTOF(async_iterator_proto_funcs))) return -1;
    r[1] = kz_new_object_proto(ctx, r[0]);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ARRAY_ITERATOR_PROTO] = r[1];
    if (kz_define_functions(ctx, r[1], array_iter_funcs, KZ_COUNTOF(array_iter_funcs))) return -1;
    JSValue tag = kz_str_from_cstr(ctx, "Array Iterator");
    if (tag == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[1], ctx->rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    return 0;
}
