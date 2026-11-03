/*
 * private.c: class private elements (ECMA-262 7.3.26-7.3.32: PrivateFieldAdd, PrivateMethodOrAccessorAdd,
 * PrivateGet, PrivateSet, PrivateElementFind).
 *
 * A private name is a symbol flagged SYM_PRIVATE, made once per class evaluation.  The elements live in the object's
 * own shape like properties, but nothing that lists or proxies properties ever sees them, and these functions work
 * on the shape directly, past any exotic behaviour (a proxy has private elements of its own, as specified).
 * Methods are read-only data properties; accessors are accessor properties.
 */
#include "jsint.h"
#include "bytecode.h"

static int fail(JSContext *ctx, JSValue name, const char *fmt) {
    char buf[96] = "#?";
    JSValue d = JV_SYM(name)->description;
    if (JV_IS_STRING(d) && kz_str_flatten(ctx, d) != JS_EXCEPTION) kz_str_to_utf8_buf(JV_STR(d), buf, sizeof buf);
    kz_throw_type(ctx, fmt, buf);
    return -1;
}

static int find(JSValue obj, JSValue name) {
    if (!JV_IS_OBJECT(obj)) return -1;
    return kz_shape_find(JV_OBJ(obj)->shape, name);
}

int kz_private_has(JSValue obj, JSValue name) { return find(obj, name) >= 0; }

int kz_private_get(JSContext *ctx, JSValue obj, JSValue name, JSValue *out) {
    int i = find(obj, name);
    if (i < 0) return fail(ctx, name, "Cannot read private member %s from an object whose class did not declare it");
    JSObject *o = JV_OBJ(obj);
    if (o->shape->props[i].attrs & KZ_PROP_ACCESSOR) {
        JSAccessor *a = JV_PTR(o->slots[i]);
        if (!JV_IS_OBJECT(a->getter)) return fail(ctx, name, "'%s' was defined without a getter");
        JSValue v = kz_call(ctx, a->getter, obj, 0, NULL);
        if (v == JS_EXCEPTION) return -1;
        *out = v;
        return 0;
    }
    *out = o->slots[i];
    return 0;
}

int kz_private_set(JSContext *ctx, JSValue obj, JSValue name, JSValue val) {
    int i = find(obj, name);
    if (i < 0) return fail(ctx, name, "Cannot write private member %s to an object whose class did not declare it");
    JSObject *o = JV_OBJ(obj);
    uint32_t attrs = o->shape->props[i].attrs;
    if (attrs & KZ_PROP_ACCESSOR) {
        JSAccessor *a = JV_PTR(o->slots[i]);
        if (!JV_IS_OBJECT(a->setter)) return fail(ctx, name, "'%s' was defined without a setter");
        JSValue args[1] = { val };
        return kz_call(ctx, a->setter, obj, 1, args) == JS_EXCEPTION ? -1 : 0;
    }
    if (!(attrs & KZ_PROP_W)) return fail(ctx, name, "Private method %s is not writable");
    o->slots[i] = val;
    return 0;
}

int kz_private_define(JSContext *ctx, JSValue obj, JSValue name, JSValue val, int kind) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "Cannot define a private member on a non-object"); return -1; }
    KZ_ROOTS(ctx, r, 3);
    r[0] = obj;
    r[1] = name;
    r[2] = val;
    int i = find(obj, name);
    if (kind == FR_PRIVATE_GETTER || kind == FR_PRIVATE_SETTER) {
        if (i >= 0) {
            /* the other half of a getter/setter pair */
            JSObject *o = JV_OBJ(r[0]);
            if (!(o->shape->props[i].attrs & KZ_PROP_ACCESSOR)) return fail(ctx, name, "Cannot initialize %s twice on the same object");
            JSAccessor *a = JV_PTR(o->slots[i]);
            JSValue *slot = kind == FR_PRIVATE_GETTER ? &a->getter : &a->setter;
            if (*slot != JS_UNDEFINED) return fail(ctx, name, "Cannot initialize %s twice on the same object");
            *slot = r[2];
            return 0;
        }
        JSAccessor *a = kz_gc_new(ctx, KZ_GC_ACCESSOR, sizeof(JSAccessor));
        if (!a) return -1;
        a->getter = kind == FR_PRIVATE_GETTER ? r[2] : JS_UNDEFINED;
        a->setter = kind == FR_PRIVATE_SETTER ? r[2] : JS_UNDEFINED;
        KZ_ROOT(ctx, ra);
        ra = JV_MKPTR(a);
        return kz_obj_add_prop(ctx, JV_OBJ(r[0]), r[1], KZ_PROP_ACCESSOR, ra);
    }
    if (i >= 0) return fail(ctx, name, "Cannot initialize %s twice on the same object");
    return kz_obj_add_prop(ctx, JV_OBJ(r[0]), r[1], kind == FR_FIELD ? KZ_PROP_W : 0, r[2]);
}
