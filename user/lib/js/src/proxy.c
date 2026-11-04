/*
 * proxy.c: Proxy exotic objects (ECMA-262 10.5, 28.2) and the Reflect namespace (28.1).
 *
 * A proxy is an object of class PROXY holding its target and handler (handler null: revoked).  The object model calls
 * the KzExotic hooks below for every internal method; each one looks up its trap on the handler, forwards to the target
 * when there is none, and otherwise calls the trap and checks the result against the invariants the specification lists
 * (a trap may not lie about a non-configurable property of the target, an extensible target's keys, and so on).
 *
 * The hooks' contract (object.c): get_own returns 1 found, 0 absent, -1 exception; the boolean hooks return 1 or 0
 * (the caller throws for 0 when the operation was strict); own_keys appends the keys and returns 1 ("everything is in
 * `keys`"); get_proto/set_proto/is_extensible/prevent_ext follow the same 1/0/-1 convention.
 */
#include "jsint.h"
#include <stdlib.h>

#define PX(o) ((o)->u.proxy)

static int bad(JSContext *ctx, const char *msg) { kz_throw_type(ctx, "%s", msg); return -1; }

/* The target and handler of a live proxy; a TypeError when it was revoked.  Every internal method of a proxy starts
   here, and a proxy without a trap forwards to its target in C (a proxy of a proxy of ...): a long chain, or a prototype
   cycle through a proxy, must end in a RangeError and not in the end of the real stack. */
static int parts(JSContext *ctx, JSObject *o, const char *op, JSValue *target, JSValue *handler) {
    if (kz_check_stack(ctx)) return -1;
    if (PX(o).handler == JS_NULL) {
        kz_throw_type(ctx, "Cannot perform '%s' on a proxy that has been revoked", op);
        return -1;
    }
    *target = PX(o).target;
    *handler = PX(o).handler;
    return 0;
}

/* GetMethod(handler, name): undefined when the handler has no such trap */
static JSValue trap_of(JSContext *ctx, JSValue handler, int atom) { return kz_get_method(ctx, handler, ctx->rt->atom[atom]); }

static int same(JSContext *ctx, JSValue a, JSValue b) { return kz_same_value(ctx, a, b); }

/* IsCompatiblePropertyDescriptor (10.1.6.2): 1 yes, 0 no, -1 exception; cur == NULL stands for undefined */
static int compatible(JSContext *ctx, bool extensible, const KzPropDesc *desc, const KzPropDesc *cur) {
    if (!cur) return extensible ? 1 : 0;
    if (!(desc->flags & (PD_HAS_VALUE | PD_HAS_GET | PD_HAS_SET | PD_HAS_W | PD_HAS_E | PD_HAS_C))) return 1;
    if (!(cur->flags & KZ_PROP_C)) {
        if ((desc->flags & PD_HAS_C) && (desc->flags & KZ_PROP_C)) return 0;
        if ((desc->flags & PD_HAS_E) && ((desc->flags & KZ_PROP_E) != (cur->flags & KZ_PROP_E))) return 0;
        bool d_acc = (desc->flags & (PD_HAS_GET | PD_HAS_SET)) != 0, d_data = (desc->flags & (PD_HAS_VALUE | PD_HAS_W)) != 0;
        bool c_acc = (cur->flags & PD_ACCESSOR) != 0;
        if ((d_acc || d_data) && d_acc != c_acc) return 0;
        if (c_acc) {
            if (desc->flags & PD_HAS_GET) { int s = same(ctx, desc->getter, cur->getter); if (s <= 0) return s; }
            if (desc->flags & PD_HAS_SET) { int s = same(ctx, desc->setter, cur->setter); if (s <= 0) return s; }
        } else if (!(cur->flags & KZ_PROP_W)) {
            if ((desc->flags & PD_HAS_W) && (desc->flags & KZ_PROP_W)) return 0;
            if (desc->flags & PD_HAS_VALUE) { int s = same(ctx, desc->value, cur->value); if (s <= 0) return s; }
        }
    }
    return 1;
}

static bool is_accessor(const KzPropDesc *d) { return (d->flags & PD_ACCESSOR) != 0; }

/* ================================================================== [[GetPrototypeOf]] .. [[PreventExtensions]] */

static int px_get_proto(JSContext *ctx, JSObject *o, JSValue *out) {
    KZ_ROOTS(ctx, r, 4);
    if (parts(ctx, o, "getPrototypeOf", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_getPrototypeOf);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_get_proto(ctx, JV_OBJ(r[0]), out) ? -1 : 1;
    JSValue args[1] = { r[0] };
    r[3] = kz_call(ctx, r[2], r[1], 1, args);
    if (r[3] == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(r[3]) && r[3] != JS_NULL) return bad(ctx, "'getPrototypeOf' on proxy: trap returned neither object nor null");
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    if (!ext) {
        JSValue tp;
        if (kz_get_proto(ctx, JV_OBJ(r[0]), &tp)) return -1;
        int s = same(ctx, r[3], tp);
        if (s < 0) return -1;
        if (!s) return bad(ctx, "'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype");
    }
    *out = r[3];
    return 1;
}

static int px_set_proto(JSContext *ctx, JSObject *o, JSValue proto) {
    KZ_ROOTS(ctx, r, 5);
    r[4] = proto;
    if (parts(ctx, o, "setPrototypeOf", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_setPrototypeOf);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_set_proto(ctx, JV_OBJ(r[0]), proto, false);
    JSValue args[2] = { r[0], proto };
    r[3] = kz_call(ctx, r[2], r[1], 2, args);
    if (r[3] == JS_EXCEPTION) return -1;
    if (!kz_to_bool(r[3])) return 0;
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    if (ext) return 1;
    JSValue tp;
    if (kz_get_proto(ctx, JV_OBJ(r[0]), &tp)) return -1;
    int s = same(ctx, proto, tp);
    if (s < 0) return -1;
    if (!s) return bad(ctx, "'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target");
    return 1;
}

static int px_is_extensible(JSContext *ctx, JSObject *o) {
    KZ_ROOTS(ctx, r, 4);
    if (parts(ctx, o, "isExtensible", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_isExtensible);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_is_extensible(ctx, JV_OBJ(r[0]));
    JSValue args[1] = { r[0] };
    r[3] = kz_call(ctx, r[2], r[1], 1, args);
    if (r[3] == JS_EXCEPTION) return -1;
    int res = kz_to_bool(r[3]) ? 1 : 0;
    int tr = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (tr < 0) return -1;
    if (res != tr) return bad(ctx, "'isExtensible' on proxy: trap result does not reflect extensibility of proxy target");
    return res;
}

static int px_prevent_ext(JSContext *ctx, JSObject *o) {
    KZ_ROOTS(ctx, r, 4);
    if (parts(ctx, o, "preventExtensions", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_preventExtensions);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_prevent_extensions(ctx, JV_OBJ(r[0]), false);
    JSValue args[1] = { r[0] };
    r[3] = kz_call(ctx, r[2], r[1], 1, args);
    if (r[3] == JS_EXCEPTION) return -1;
    int res = kz_to_bool(r[3]) ? 1 : 0;
    if (res) {
        int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
        if (ext < 0) return -1;
        if (ext) return bad(ctx, "'preventExtensions' on proxy: trap returned truish but the proxy target is extensible");
    }
    return res;
}

/* ================================================================== properties */

static int px_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    KZ_ROOTS(ctx, r, 6);
    if (parts(ctx, o, "getOwnPropertyDescriptor", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_getOwnPropertyDescriptor);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_get_own_property(ctx, JV_OBJ(r[0]), key, d);
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    JSValue args[2] = { r[0], r[3] };
    r[4] = kz_call(ctx, r[2], r[1], 2, args);
    if (r[4] == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(r[4]) && r[4] != JS_UNDEFINED) return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined for property");
    KZ_ROOT_DESC(ctx, td);
    int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
    if (has < 0) return -1;
    if (r[4] == JS_UNDEFINED) {
        if (!has) return 0;
        if (!(td.flags & KZ_PROP_C)) return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a non-configurable property of the proxy target");
        int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
        if (ext < 0) return -1;
        if (!ext) return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a property of a non-extensible proxy target");
        return 0;
    }
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    KZ_ROOT_DESC(ctx, rd);
    if (kz_to_property_descriptor(ctx, r[4], &rd)) return -1;
    kz_complete_descriptor(&rd);
    int c = compatible(ctx, ext != 0, &rd, has ? &td : NULL);
    if (c < 0) return -1;
    if (!c) return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap returned a descriptor incompatible with the proxy target's property");
    if (!(rd.flags & KZ_PROP_C)) {
        if (!has || (td.flags & KZ_PROP_C)) return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for a configurable or missing property");
        if ((rd.flags & PD_HAS_W) && !(rd.flags & KZ_PROP_W) && !is_accessor(&td) && (td.flags & KZ_PROP_W))
            return bad(ctx, "'getOwnPropertyDescriptor' on proxy: trap reported non-writability for a writable property of the proxy target");
    }
    *d = rd;
    return 1;
}

static int px_define_own(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *desc) {
    KZ_ROOTS(ctx, r, 7);
    if (parts(ctx, o, "defineProperty", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_defineProperty);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_define_own_property(ctx, JV_OBJ(r[0]), key, desc, false);
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    r[4] = kz_from_property_descriptor(ctx, desc);
    if (r[4] == JS_EXCEPTION) return -1;
    JSValue args[3] = { r[0], r[3], r[4] };
    r[5] = kz_call(ctx, r[2], r[1], 3, args);
    if (r[5] == JS_EXCEPTION) return -1;
    if (!kz_to_bool(r[5])) return 0;
    KZ_ROOT_DESC(ctx, td);
    int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
    if (has < 0) return -1;
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    bool setting_config_false = (desc->flags & PD_HAS_C) && !(desc->flags & KZ_PROP_C);
    if (!has) {
        if (!ext) return bad(ctx, "'defineProperty' on proxy: trap returned truish for adding a property to the non-extensible proxy target");
        if (setting_config_false) return bad(ctx, "'defineProperty' on proxy: trap returned truish for defining a non-configurable property that does not exist on the target");
    } else {
        int c = compatible(ctx, ext != 0, desc, &td);
        if (c < 0) return -1;
        if (!c) return bad(ctx, "'defineProperty' on proxy: trap returned truish for adding a property that is incompatible with the existing property of the proxy target");
        if (setting_config_false && (td.flags & KZ_PROP_C)) return bad(ctx, "'defineProperty' on proxy: trap returned truish for defining a non-configurable property that is configurable on the target");
        if (!is_accessor(&td) && !(td.flags & KZ_PROP_C) && (td.flags & KZ_PROP_W) && (desc->flags & PD_HAS_W) && !(desc->flags & KZ_PROP_W))
            return bad(ctx, "'defineProperty' on proxy: trap returned truish for defining a non-writable property that is writable on the non-configurable target");
    }
    return 1;
}

static int px_has(JSContext *ctx, JSObject *o, JSValue key) {
    KZ_ROOTS(ctx, r, 6);
    if (parts(ctx, o, "has", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_has);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_has_property(ctx, JV_OBJ(r[0]), key);
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    JSValue args[2] = { r[0], r[3] };
    r[4] = kz_call(ctx, r[2], r[1], 2, args);
    if (r[4] == JS_EXCEPTION) return -1;
    int res = kz_to_bool(r[4]) ? 1 : 0;
    if (!res) {
        KZ_ROOT_DESC(ctx, td);
        int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
        if (has < 0) return -1;
        if (has) {
            if (!(td.flags & KZ_PROP_C)) return bad(ctx, "'has' on proxy: trap returned falsish for a non-configurable property of the proxy target");
            int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
            if (ext < 0) return -1;
            if (!ext) return bad(ctx, "'has' on proxy: trap returned falsish for a property of a non-extensible proxy target");
        }
    }
    return res;
}

static int px_get(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out) {
    KZ_ROOTS(ctx, r, 7);
    r[6] = receiver;
    if (parts(ctx, o, "get", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_get);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) {
        r[3] = kz_get(ctx, r[0], key, receiver);
        if (r[3] == JS_EXCEPTION) return -1;
        *out = r[3];
        return 1;
    }
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    JSValue args[3] = { r[0], r[3], receiver };
    r[4] = kz_call(ctx, r[2], r[1], 3, args);
    if (r[4] == JS_EXCEPTION) return -1;
    KZ_ROOT_DESC(ctx, td);
    int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
    if (has < 0) return -1;
    if (has && !(td.flags & KZ_PROP_C)) {
        if (!is_accessor(&td) && !(td.flags & KZ_PROP_W)) {
            int s = same(ctx, r[4], td.value);
            if (s < 0) return -1;
            if (!s) return bad(ctx, "'get' on proxy: property is a read-only and non-configurable data property on the proxy target but the proxy did not return its actual value");
        }
        if (is_accessor(&td) && td.getter == JS_UNDEFINED && r[4] != JS_UNDEFINED)
            return bad(ctx, "'get' on proxy: property is a non-configurable accessor property on the proxy target and does not have a getter function, but the trap did not return 'undefined'");
    }
    *out = r[4];
    return 1;
}

static int px_set(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver) {
    KZ_ROOTS(ctx, r, 8);
    r[6] = val;
    r[7] = receiver;
    if (parts(ctx, o, "set", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_set);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_set(ctx, r[0], key, val, receiver, false);
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    JSValue args[4] = { r[0], r[3], val, receiver };
    r[4] = kz_call(ctx, r[2], r[1], 4, args);
    if (r[4] == JS_EXCEPTION) return -1;
    if (!kz_to_bool(r[4])) return 0;
    KZ_ROOT_DESC(ctx, td);
    int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
    if (has < 0) return -1;
    if (has && !(td.flags & KZ_PROP_C)) {
        if (!is_accessor(&td) && !(td.flags & KZ_PROP_W)) {
            int s = same(ctx, val, td.value);
            if (s < 0) return -1;
            if (!s) return bad(ctx, "'set' on proxy: trap returned truish for property which exists in the proxy target as a non-configurable and non-writable data property with a different value");
        }
        if (is_accessor(&td) && td.setter == JS_UNDEFINED)
            return bad(ctx, "'set' on proxy: trap returned truish for property which exists in the proxy target as a non-configurable and non-writable accessor property without a setter");
    }
    return 1;
}

static int px_del(JSContext *ctx, JSObject *o, JSValue key) {
    KZ_ROOTS(ctx, r, 6);
    if (parts(ctx, o, "deleteProperty", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_deleteProperty);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) return kz_delete_property(ctx, JV_OBJ(r[0]), key, false);
    r[3] = kz_key_to_value(ctx, key);
    if (r[3] == JS_EXCEPTION) return -1;
    JSValue args[2] = { r[0], r[3] };
    r[4] = kz_call(ctx, r[2], r[1], 2, args);
    if (r[4] == JS_EXCEPTION) return -1;
    if (!kz_to_bool(r[4])) return 0;
    KZ_ROOT_DESC(ctx, td);
    int has = kz_get_own_property(ctx, JV_OBJ(r[0]), key, &td);
    if (has < 0) return -1;
    if (!has) return 1;
    if (!(td.flags & KZ_PROP_C)) return bad(ctx, "'deleteProperty' on proxy: trap returned truish for property which is non-configurable in the proxy target");
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    if (!ext) return bad(ctx, "'deleteProperty' on proxy: trap returned truish for property but the proxy target is non-extensible");
    return 1;
}

static int cmp_keys(const void *a, const void *b) {
    JSValue x = *(const JSValue *)a, y = *(const JSValue *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int px_own_keys(JSContext *ctx, JSObject *o, KzVec *keys) {
    KZ_ROOTS(ctx, r, 5);
    if (parts(ctx, o, "ownKeys", &r[0], &r[1])) return -1;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_ownKeys);
    if (r[2] == JS_EXCEPTION) return -1;
    if (r[2] == JS_UNDEFINED) {
        if (kz_own_keys(ctx, JV_OBJ(r[0]), keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
        return 1;
    }
    JSValue args[1] = { r[0] };
    r[3] = kz_call(ctx, r[2], r[1], 1, args);
    if (r[3] == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(r[3])) return bad(ctx, "CreateListFromArrayLike called on non-object");
    KZ_ROOT_VEC(ctx, list);
    if (kz_create_list_from_array_like(ctx, r[3], &list)) return -1;
    KZ_ROOT_VEC(ctx, result);                   /* the trap's keys as property keys */
    for (uint32_t i = 0; i < list.len; i++) {
        if (!JV_IS_STRING(list.v[i]) && !JV_IS_SYMBOL(list.v[i])) return bad(ctx, "'ownKeys' on proxy: trap result must contain only strings and symbols");
        r[4] = kz_key_from_value(ctx, list.v[i]);
        if (r[4] == JS_EXCEPTION) return -1;
        if (kz_vec_push(ctx, &result, r[4])) return -1;
    }
    /* no duplicates */
    if (result.len > 1) {
        JSValue *copy = kz_malloc(ctx, result.len * sizeof(JSValue));
        if (!copy) return -1;
        memcpy(copy, result.v, result.len * sizeof(JSValue));
        qsort(copy, result.len, sizeof(JSValue), cmp_keys);
        bool dup = false;
        for (uint32_t i = 1; i < result.len; i++) if (copy[i] == copy[i - 1]) { dup = true; break; }
        kz_free(ctx->rt, copy);
        if (dup) return bad(ctx, "'ownKeys' on proxy: trap returned duplicate entries");
    }
    int ext = kz_is_extensible(ctx, JV_OBJ(r[0]));
    if (ext < 0) return -1;
    KZ_ROOT_VEC(ctx, tkeys);
    if (kz_own_keys(ctx, JV_OBJ(r[0]), &tkeys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
    KZ_ROOT_VEC(ctx, configurable);
    KZ_ROOT_VEC(ctx, nonconfigurable);
    for (uint32_t i = 0; i < tkeys.len; i++) {
        KZ_ROOT_DESC(ctx, td);
        int has = kz_get_own_property(ctx, JV_OBJ(r[0]), tkeys.v[i], &td);
        if (has < 0) return -1;
        if (has && !(td.flags & KZ_PROP_C)) { if (kz_vec_push(ctx, &nonconfigurable, tkeys.v[i])) return -1; }
        else if (kz_vec_push(ctx, &configurable, tkeys.v[i])) return -1;
    }
    if (!ext || nonconfigurable.len) {
        KZ_ROOT_VEC(ctx, unchecked);
        for (uint32_t i = 0; i < result.len; i++) if (kz_vec_push(ctx, &unchecked, result.v[i])) return -1;
        for (uint32_t i = 0; i < nonconfigurable.len; i++) {
            uint32_t at = unchecked.len;
            for (uint32_t k = 0; k < unchecked.len; k++) if (unchecked.v[k] == nonconfigurable.v[i]) { at = k; break; }
            if (at == unchecked.len) return bad(ctx, "'ownKeys' on proxy: trap result did not include a non-configurable key of the proxy target");
            unchecked.v[at] = unchecked.v[--unchecked.len];
        }
        if (!ext) {
            for (uint32_t i = 0; i < configurable.len; i++) {
                uint32_t at = unchecked.len;
                for (uint32_t k = 0; k < unchecked.len; k++) if (unchecked.v[k] == configurable.v[i]) { at = k; break; }
                if (at == unchecked.len) return bad(ctx, "'ownKeys' on proxy: trap result did not include all keys of the non-extensible proxy target");
                unchecked.v[at] = unchecked.v[--unchecked.len];
            }
            if (unchecked.len) return bad(ctx, "'ownKeys' on proxy: trap returned extra keys but the proxy target is non-extensible");
        }
    }
    for (uint32_t i = 0; i < result.len; i++) if (kz_vec_push(ctx, keys, result.v[i])) return -1;
    return 1;
}

static const KzExotic proxy_exotic = {
    .get_own = px_get_own, .define_own = px_define_own, .has = px_has, .get = px_get, .set = px_set, .del = px_del,
    .own_keys = px_own_keys, .get_proto = px_get_proto, .set_proto = px_set_proto, .is_extensible = px_is_extensible,
    .prevent_ext = px_prevent_ext,
};

/* ================================================================== [[Call]] and [[Construct]] */

JSValue kz_proxy_call(JSContext *ctx, JSObject *p, JSValue this_val, int argc, JSValue *argv) {
    KZ_ROOTS(ctx, r, 6);
    r[4] = this_val;
    if (parts(ctx, p, "apply", &r[0], &r[1])) return JS_EXCEPTION;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_apply);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (r[2] == JS_UNDEFINED) return kz_call(ctx, r[0], this_val, argc, argv);
    r[3] = kz_new_array_from(ctx, (uint32_t)argc, argv);
    if (r[3] == JS_EXCEPTION) return r[3];
    JSValue args[3] = { r[0], this_val, r[3] };
    return kz_call(ctx, r[2], r[1], 3, args);
}

JSValue kz_proxy_construct(JSContext *ctx, JSObject *p, int argc, JSValue *argv, JSValue new_target) {
    KZ_ROOTS(ctx, r, 6);
    r[4] = new_target;
    if (parts(ctx, p, "construct", &r[0], &r[1])) return JS_EXCEPTION;
    r[2] = trap_of(ctx, r[1], KZ_ATOM_construct);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (r[2] == JS_UNDEFINED) return kz_construct(ctx, r[0], argc, argv, new_target);
    r[3] = kz_new_array_from(ctx, (uint32_t)argc, argv);
    if (r[3] == JS_EXCEPTION) return r[3];
    JSValue args[3] = { r[0], r[3], new_target };
    r[5] = kz_call(ctx, r[2], r[1], 3, args);
    if (r[5] == JS_EXCEPTION) return r[5];
    if (!JV_IS_OBJECT(r[5])) return kz_throw_type(ctx, "proxy [[Construct]] must return an object");
    return r[5];
}

/* ================================================================== the Proxy constructor */

static JSValue proxy_create(JSContext *ctx, JSValue target, JSValue handler) {
    if (!JV_IS_OBJECT(target) || !JV_IS_OBJECT(handler)) return kz_throw_type(ctx, "Cannot create proxy with a non-object as target or handler");
    KZ_ROOTS(ctx, r, 3);
    r[0] = target;
    r[1] = handler;
    r[2] = kz_new_object_class(ctx, KZ_CLASS_PROXY, JS_NULL);
    if (r[2] == JS_EXCEPTION) return r[2];
    JSObject *p = JV_OBJ(r[2]);
    PX(p).target = target;
    PX(p).handler = handler;
    if (kz_is_callable(target)) p->h.flags |= OBJ_CALLABLE;
    if (kz_is_constructor(target)) p->h.flags |= OBJ_CONSTRUCTOR;
    return r[2];
}

static JSValue proxy_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (ctx->rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Constructor Proxy requires 'new'");
    return proxy_create(ctx, argv[0], argv[1]);
}

static JSValue proxy_revoke(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    JSValue p = kz_native_data(ctx, 0);
    if (p == JS_NULL || !JV_IS_OBJECT(p)) return JS_UNDEFINED;
    kz_native_set_data(ctx, 0, JS_NULL);
    PX(JV_OBJ(p)).target = JS_NULL;
    PX(JV_OBJ(p)).handler = JS_NULL;
    return JS_UNDEFINED;
}

static JSValue proxy_revocable(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    r[0] = proxy_create(ctx, argv[0], argv[1]);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSValue data[1] = { r[0] };
    r[1] = kz_new_native(ctx, proxy_revoke, ctx->rt->atom[KZ_ATOM_empty], 0, 0, 1, data);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = kz_new_object(ctx);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (kz_create_data_property(ctx, r[2], ctx->rt->atom[KZ_ATOM_proxy], r[0])) return JS_EXCEPTION;
    if (kz_create_data_property(ctx, r[2], ctx->rt->atom[KZ_ATOM_revoke], r[1])) return JS_EXCEPTION;
    return r[2];
}

/* ================================================================== Reflect */

static JSValue need_object(JSContext *ctx, JSValue v, const char *fn) {
    if (!JV_IS_OBJECT(v)) return kz_throw_type(ctx, "Reflect.%s called on non-object", fn);
    return v;
}

static JSValue reflect_apply(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "Reflect.apply target is not callable");
    if (!JV_IS_OBJECT(argv[2])) return kz_throw_type(ctx, "Reflect.apply: CreateListFromArrayLike called on non-object");
    KZ_ROOT_VEC(ctx, args);
    if (kz_create_list_from_array_like(ctx, argv[2], &args)) return JS_EXCEPTION;
    return kz_call(ctx, argv[0], argv[1], (int)args.len, args.v);
}

static JSValue reflect_construct(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    if (!kz_is_constructor(argv[0])) return kz_throw_type(ctx, "Reflect.construct target is not a constructor");
    JSValue nt = argc > 2 ? argv[2] : argv[0];
    if (!kz_is_constructor(nt)) return kz_throw_type(ctx, "Reflect.construct newTarget is not a constructor");
    if (!JV_IS_OBJECT(argv[1])) return kz_throw_type(ctx, "Reflect.construct: CreateListFromArrayLike called on non-object");
    KZ_ROOT_VEC(ctx, args);
    if (kz_create_list_from_array_like(ctx, argv[1], &args)) return JS_EXCEPTION;
    return kz_construct(ctx, argv[0], (int)args.len, args.v, nt);
}

static JSValue reflect_define_property(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "defineProperty") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 1);
    r[0] = kz_to_property_key(ctx, argv[1]);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_DESC(ctx, d);
    if (kz_to_property_descriptor(ctx, argv[2], &d)) return JS_EXCEPTION;
    int res = kz_define_own_property(ctx, JV_OBJ(argv[0]), r[0], &d, false);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_delete_property(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "deleteProperty") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, argv[1]);
    if (k == JS_EXCEPTION) return k;
    int res = kz_delete_property(ctx, JV_OBJ(argv[0]), k, false);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    if (need_object(ctx, argv[0], "get") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, argv[1]);
    if (k == JS_EXCEPTION) return k;
    return kz_get(ctx, argv[0], k, argc > 2 ? argv[2] : argv[0]);
}

static JSValue reflect_get_own_property_descriptor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "getOwnPropertyDescriptor") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, argv[1]);
    if (k == JS_EXCEPTION) return k;
    KZ_ROOT_DESC(ctx, d);
    int has = kz_get_own_property(ctx, JV_OBJ(argv[0]), k, &d);
    if (has < 0) return JS_EXCEPTION;
    if (!has) return JS_UNDEFINED;
    return kz_from_property_descriptor(ctx, &d);
}

static JSValue reflect_get_prototype_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "getPrototypeOf") == JS_EXCEPTION) return JS_EXCEPTION;
    JSValue p;
    if (kz_get_proto(ctx, JV_OBJ(argv[0]), &p)) return JS_EXCEPTION;
    return p;
}

static JSValue reflect_has(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "has") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, argv[1]);
    if (k == JS_EXCEPTION) return k;
    int res = kz_has_property(ctx, JV_OBJ(argv[0]), k);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_is_extensible(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "isExtensible") == JS_EXCEPTION) return JS_EXCEPTION;
    int res = kz_is_extensible(ctx, JV_OBJ(argv[0]));
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_own_keys(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "ownKeys") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT_VEC(ctx, keys);
    if (kz_own_keys(ctx, JV_OBJ(argv[0]), &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return JS_EXCEPTION;
    KZ_ROOT(ctx, arr);
    arr = kz_new_array(ctx, 0);
    if (arr == JS_EXCEPTION) return arr;
    KZ_ROOT(ctx, kv);
    for (uint32_t i = 0; i < keys.len; i++) {
        kv = kz_key_to_value(ctx, keys.v[i]);
        if (kv == JS_EXCEPTION || kz_array_push(ctx, JV_OBJ(arr), kv)) return JS_EXCEPTION;
    }
    return arr;
}

static JSValue reflect_prevent_extensions(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "preventExtensions") == JS_EXCEPTION) return JS_EXCEPTION;
    int res = kz_prevent_extensions(ctx, JV_OBJ(argv[0]), false);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    if (need_object(ctx, argv[0], "set") == JS_EXCEPTION) return JS_EXCEPTION;
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, argv[1]);
    if (k == JS_EXCEPTION) return k;
    int res = kz_set(ctx, argv[0], k, argv[2], argc > 3 ? argv[3] : argv[0], false);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static JSValue reflect_set_prototype_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (need_object(ctx, argv[0], "setPrototypeOf") == JS_EXCEPTION) return JS_EXCEPTION;
    if (!JV_IS_OBJECT(argv[1]) && argv[1] != JS_NULL) return kz_throw_type(ctx, "Object prototype may only be an Object or null");
    int res = kz_set_proto(ctx, JV_OBJ(argv[0]), argv[1], false);
    if (res < 0) return JS_EXCEPTION;
    return JV_MKBOOL(res);
}

static const KzFuncDef reflect_funcs[] = {
    KZ_FN("apply", reflect_apply, 3),
    KZ_FN_ARGS("construct", reflect_construct, 2, 3),
    KZ_FN("defineProperty", reflect_define_property, 3),
    KZ_FN("deleteProperty", reflect_delete_property, 2),
    KZ_FN_ARGS("get", reflect_get, 2, 3),
    KZ_FN("getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2),
    KZ_FN("getPrototypeOf", reflect_get_prototype_of, 1),
    KZ_FN("has", reflect_has, 2),
    KZ_FN("isExtensible", reflect_is_extensible, 1),
    KZ_FN("ownKeys", reflect_own_keys, 1),
    KZ_FN("preventExtensions", reflect_prevent_extensions, 1),
    KZ_FN_ARGS("set", reflect_set, 3, 4),
    KZ_FN("setPrototypeOf", reflect_set_prototype_of, 2),
};

static const KzFuncDef proxy_funcs[] = { KZ_FN("revocable", proxy_revocable, 2) };

int kz_init_proxy(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    kz_exotic[KZ_CLASS_PROXY] = &proxy_exotic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_make_ctor(ctx, proxy_ctor, "Proxy", 2, 0, (KzIntrinsic)-1, KZ_I_PROXY);       /* (no prototype property) */
    if (r[0] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], proxy_funcs, KZ_COUNTOF(proxy_funcs))) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Proxy, r[0], KZ_PROP_W | KZ_PROP_C)) return -1;
    r[1] = kz_new_object(ctx);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], reflect_funcs, KZ_COUNTOF(reflect_funcs))) return -1;
    JSValue tag = kz_atom(ctx, "Reflect");
    if (tag == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[1], rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Reflect, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
