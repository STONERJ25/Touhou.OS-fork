/*
 * builtins_object.c: the Object constructor and Object.prototype (ECMA-262 20.1), plus the GroupBy helper that
 * Object.groupBy and Map.groupBy share.
 */
#include "jsint.h"

static JSValue this_object(JSContext *ctx, JSValue v) { return kz_to_object(ctx, v); }

/* ================================================================== the constructor */

static JSValue object_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    JSRuntime *rt = ctx->rt;
    JSValue nt = rt->new_target;
    if (nt != JS_UNDEFINED && nt != JV_MKOBJ(rt->cur_native))
        return kz_ordinary_create_from_ctor(ctx, nt, KZ_I_OBJECT_PROTO, KZ_CLASS_OBJECT);
    JSValue v = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JV_IS_NULLISH(v)) return kz_new_object(ctx);
    return kz_to_object(ctx, v);
}

/* ================================================================== Object.* */

static JSValue object_assign(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    for (int i = 1; i < argc; i++) {
        if (JV_IS_NULLISH(argv[i])) continue;
        r[1] = kz_to_object(ctx, argv[i]);
        if (r[1] == JS_EXCEPTION) return r[1];
        JSObject *from = JV_OBJ(r[1]);
        KZ_ROOT_VEC(ctx, keys);
        if (kz_own_keys(ctx, from, &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return JS_EXCEPTION;
        for (uint32_t k = 0; k < keys.len; k++) {
            KZ_ROOT_DESC(ctx, d);
            int h = kz_get_own_property(ctx, from, keys.v[k], &d);
            if (h < 0) return JS_EXCEPTION;
            if (!h || !(d.flags & KZ_PROP_E)) continue;
            r[2] = kz_get(ctx, r[1], keys.v[k], r[1]);
            if (r[2] == JS_EXCEPTION) return r[2];
            if (kz_set(ctx, r[0], keys.v[k], r[2], r[0], true) < 0) return JS_EXCEPTION;
        }
    }
    return r[0];
}

/* ObjectDefineProperties: the descriptors are all read before any is applied */
static int define_properties(JSContext *ctx, JSValue obj, JSValue props) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, props);
    if (r[0] == JS_EXCEPTION) return -1;
    JSObject *po = JV_OBJ(r[0]);
    KZ_ROOT_VEC(ctx, keys);
    KZ_ROOT_VEC(ctx, descs);           /* key, value, getter, setter, flags (an int) */
    if (kz_own_keys(ctx, po, &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
    for (uint32_t i = 0; i < keys.len; i++) {
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, po, keys.v[i], &d);
        if (h < 0) return -1;
        if (!h || !(d.flags & KZ_PROP_E)) continue;
        r[1] = kz_get(ctx, r[0], keys.v[i], r[0]);
        if (r[1] == JS_EXCEPTION) return -1;
        KZ_ROOT_DESC(ctx, nd);
        if (kz_to_property_descriptor(ctx, r[1], &nd)) return -1;
        if (kz_vec_push(ctx, &descs, keys.v[i]) || kz_vec_push(ctx, &descs, nd.value) ||
            kz_vec_push(ctx, &descs, nd.getter) || kz_vec_push(ctx, &descs, nd.setter) ||
            kz_vec_push(ctx, &descs, JV_MKINT((int32_t)nd.flags)))
            return -1;
    }
    for (uint32_t i = 0; i < descs.len; i += 5) {
        KzPropDesc d = { descs.v[i + 1], descs.v[i + 2], descs.v[i + 3], (uint32_t)JV_INT(descs.v[i + 4]) };
        if (kz_define_own_property(ctx, JV_OBJ(obj), descs.v[i], &d, true) < 0) return -1;
    }
    return 0;
}

static JSValue object_create(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSValue proto = argv[0];
    if (!JV_IS_OBJECT(proto) && proto != JS_NULL) return kz_throw_type(ctx, "Object prototype may only be an Object or null");
    KZ_ROOT(ctx, o);
    o = kz_new_object_proto(ctx, proto);
    if (o == JS_EXCEPTION) return o;
    if (argv[1] != JS_UNDEFINED && define_properties(ctx, o, argv[1])) return JS_EXCEPTION;
    return o;
}

static JSValue object_define_property(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_OBJECT(argv[0])) return kz_throw_type(ctx, "Object.defineProperty called on non-object");
    KZ_ROOT(ctx, key);
    key = kz_to_property_key(ctx, argv[1]);
    if (key == JS_EXCEPTION) return key;
    KZ_ROOT_DESC(ctx, d);
    if (kz_to_property_descriptor(ctx, argv[2], &d)) return JS_EXCEPTION;
    if (kz_define_own_property(ctx, JV_OBJ(argv[0]), key, &d, true) < 0) return JS_EXCEPTION;
    return argv[0];
}

static JSValue object_define_properties(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_OBJECT(argv[0])) return kz_throw_type(ctx, "Object.defineProperties called on non-object");
    if (define_properties(ctx, argv[0], argv[1])) return JS_EXCEPTION;
    return argv[0];
}

/* EnumerableOwnProperties: kind 0 keys, 1 values, 2 entries */
static JSValue enumerable_own(JSContext *ctx, JSValue v, int kind) {
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_to_object(ctx, v);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSObject *o = JV_OBJ(r[0]);
    KZ_ROOT_VEC(ctx, keys);
    if (kind == 0 && !(o->h.flags & OBJ_EXOTIC)) {
        if (kz_own_keys(ctx, o, &keys, KZ_KEYS_STRINGS | KZ_KEYS_ENUM)) return JS_EXCEPTION;
        r[1] = kz_new_array(ctx, 0);
        if (r[1] == JS_EXCEPTION) return r[1];
        for (uint32_t i = 0; i < keys.len; i++) {
            r[2] = kz_key_to_value(ctx, keys.v[i]);
            if (r[2] == JS_EXCEPTION || kz_array_push(ctx, JV_OBJ(r[1]), r[2])) return JS_EXCEPTION;
        }
        return r[1];
    }
    if (kz_own_keys(ctx, o, &keys, KZ_KEYS_STRINGS)) return JS_EXCEPTION;
    r[1] = kz_new_array(ctx, 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint32_t i = 0; i < keys.len; i++) {
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, o, keys.v[i], &d);
        if (h < 0) return JS_EXCEPTION;
        if (!h || !(d.flags & KZ_PROP_E)) continue;
        if (kind == 0) {
            r[2] = kz_key_to_value(ctx, keys.v[i]);
            if (r[2] == JS_EXCEPTION || kz_array_push(ctx, JV_OBJ(r[1]), r[2])) return JS_EXCEPTION;
            continue;
        }
        r[2] = kz_get(ctx, r[0], keys.v[i], r[0]);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kind == 2) {
            r[3] = kz_key_to_value(ctx, keys.v[i]);
            if (r[3] == JS_EXCEPTION) return r[3];
            JSValue pair[2] = { r[3], r[2] };
            r[2] = kz_new_array_from(ctx, 2, pair);
            if (r[2] == JS_EXCEPTION) return r[2];
        }
        if (kz_array_push(ctx, JV_OBJ(r[1]), r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

static JSValue object_keys(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    return enumerable_own(ctx, argv[0], magic);
}

static JSValue object_integrity(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    /* magic: 0 freeze, 1 seal, 2 preventExtensions */
    if (!JV_IS_OBJECT(argv[0])) return argv[0];
    JSObject *o = JV_OBJ(argv[0]);
    if (magic == 2) {
        if (kz_prevent_extensions(ctx, o, true) < 0) return JS_EXCEPTION;
        return argv[0];
    }
    if (kz_set_integrity(ctx, o, magic == 0) < 0) return JS_EXCEPTION;
    return argv[0];
}

static JSValue object_test_integrity(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    /* magic: 0 isFrozen, 1 isSealed, 2 isExtensible */
    if (!JV_IS_OBJECT(argv[0])) return JV_MKBOOL(magic != 2);
    int r = magic == 2 ? kz_is_extensible(ctx, JV_OBJ(argv[0])) : kz_test_integrity(ctx, JV_OBJ(argv[0]), magic == 0);
    if (r < 0) return JS_EXCEPTION;
    return JV_MKBOOL(r);
}

static JSValue object_from_entries(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (JV_IS_NULLISH(argv[0])) return kz_throw_type(ctx, "Object.fromEntries requires an iterable");
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, argv[0], &it, false)) return JS_EXCEPTION;
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[1]);
        if (s < 0) return JS_EXCEPTION;
        if (!s) break;
        if (!JV_IS_OBJECT(r[1])) {
            kz_throw_type(ctx, "Iterator value %s is not an entry object", "");
            kz_iter_close_on_throw(ctx, &it);
            return JS_EXCEPTION;
        }
        r[2] = kz_get_index(ctx, r[1], 0);
        if (r[2] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
        r[3] = kz_get_index(ctx, r[1], 1);
        if (r[3] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
        r[2] = kz_to_property_key(ctx, r[2]);
        if (r[2] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
        if (kz_create_data_property(ctx, r[0], r[2], r[3])) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
    }
    return r[0];
}

static JSValue object_gopd(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_to_property_key(ctx, argv[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KZ_ROOT_DESC(ctx, d);
    int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[1], &d);
    if (h < 0) return JS_EXCEPTION;
    if (!h) return JS_UNDEFINED;
    return kz_from_property_descriptor(ctx, &d);
}

static JSValue object_gopds(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_VEC(ctx, keys);
    if (kz_own_keys(ctx, JV_OBJ(r[0]), &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return JS_EXCEPTION;
    r[1] = kz_new_object(ctx);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint32_t i = 0; i < keys.len; i++) {
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, JV_OBJ(r[0]), keys.v[i], &d);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        r[2] = kz_from_property_descriptor(ctx, &d);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_create_data_property(ctx, r[1], keys.v[i], r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

/* GetOwnPropertyKeys: magic 0 strings, 1 symbols */
static JSValue object_own_names(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_VEC(ctx, keys);
    if (kz_own_keys(ctx, JV_OBJ(r[0]), &keys, magic ? KZ_KEYS_SYMBOLS : KZ_KEYS_STRINGS)) return JS_EXCEPTION;
    r[1] = kz_new_array(ctx, 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (kz_elems_reserve(ctx, JV_OBJ(r[1]), keys.len)) return JS_EXCEPTION;
    for (uint32_t i = 0; i < keys.len; i++) {
        r[2] = kz_key_to_value(ctx, keys.v[i]);
        if (r[2] == JS_EXCEPTION || kz_array_push(ctx, JV_OBJ(r[1]), r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

static JSValue object_get_prototype_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (kz_get_proto(ctx, JV_OBJ(r[0]), &r[1])) return JS_EXCEPTION;
    return r[1];
}

static JSValue object_set_prototype_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (JV_IS_NULLISH(argv[0])) return kz_throw_type(ctx, "Object.setPrototypeOf called on null or undefined");
    if (!JV_IS_OBJECT(argv[1]) && argv[1] != JS_NULL) return kz_throw_type(ctx, "Object prototype may only be an Object or null");
    if (!JV_IS_OBJECT(argv[0])) return argv[0];
    if (kz_set_proto(ctx, JV_OBJ(argv[0]), argv[1], true) < 0) return JS_EXCEPTION;
    return argv[0];
}

static JSValue object_is(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int r = kz_same_value(ctx, argv[0], argv[1]);
    if (r < 0) return JS_EXCEPTION;
    return JV_MKBOOL(r);
}

static JSValue object_has_own(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_to_property_key(ctx, argv[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KZ_ROOT_DESC(ctx, d);
    int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[1], &d);
    if (h < 0) return JS_EXCEPTION;
    return JV_MKBOOL(h);
}

/*
 * GroupBy (7.3.35): calls cb(value, index) for each item and collects the values by key.  mode 0: property keys,
 * collected into the null-prototype object *out (created here); mode 1: SameValueZero keys into a Map (*out).
 */
int kz_group_by(JSContext *ctx, JSValue items, JSValue cb, int mode, JSValue *out) {
    if (JV_IS_NULLISH(items)) { kz_throw_type(ctx, "groupBy called on null or undefined"); return -1; }
    if (!kz_is_callable(cb)) { kz_throw_not_function(ctx, cb); return -1; }
    KZ_ROOTS(ctx, r, 5);
    r[0] = mode == 0 ? kz_new_object_proto(ctx, JS_NULL) : kz_new_map(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, items, &it, false)) return -1;
    for (double k = 0;; k++) {
        int s = kz_iter_step(ctx, &it, &r[1]);
        if (s < 0) return -1;
        if (!s) break;
        if (k >= 9007199254740991.0) {
            kz_throw_type(ctx, "too many elements");
            return kz_iter_close_on_throw(ctx, &it);
        }
        JSValue args[2] = { r[1], jv_number(k) };
        r[2] = kz_call(ctx, cb, JS_UNDEFINED, 2, args);
        if (r[2] == JS_EXCEPTION) return kz_iter_close_on_throw(ctx, &it);
        if (mode == 0) {
            r[2] = kz_to_property_key(ctx, r[2]);
            if (r[2] == JS_EXCEPTION) return kz_iter_close_on_throw(ctx, &it);
            KZ_ROOT_DESC(ctx, d);
            int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[2], &d);
            if (h < 0) return kz_iter_close_on_throw(ctx, &it);
            if (h) r[3] = d.value;
            else {
                r[3] = kz_new_array(ctx, 0);
                if (r[3] == JS_EXCEPTION || kz_create_data_property(ctx, r[0], r[2], r[3])) return kz_iter_close_on_throw(ctx, &it);
            }
        } else {
            if (JV_IS_NUMBER(r[2]) && jv_num(r[2]) == 0) r[2] = JV_MKINT(0);
            r[3] = kz_map_get(ctx, JV_OBJ(r[0]), r[2]);
            if (r[3] == JS_EXCEPTION) return kz_iter_close_on_throw(ctx, &it);
            if (r[3] == JS_UNDEFINED) {
                r[3] = kz_new_array(ctx, 0);
                if (r[3] == JS_EXCEPTION || kz_map_set(ctx, JV_OBJ(r[0]), r[2], r[3])) return kz_iter_close_on_throw(ctx, &it);
            }
        }
        if (kz_array_push(ctx, JV_OBJ(r[3]), r[1])) return kz_iter_close_on_throw(ctx, &it);
    }
    *out = r[0];
    return 0;
}

static JSValue object_group_by(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, out);
    if (kz_group_by(ctx, argv[0], argv[1], 0, &out)) return JS_EXCEPTION;
    return out;
}

/* ================================================================== Object.prototype */

static JSValue proto_has_own_property(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[1] = kz_to_property_key(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[0] = this_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_DESC(ctx, d);
    int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[1], &d);
    if (h < 0) return JS_EXCEPTION;
    return JV_MKBOOL(h);
}

static JSValue proto_is_prototype_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!JV_IS_OBJECT(argv[0])) return JS_FALSE;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = argv[0];
    for (int depth = 0;; depth++) {
        if (depth > 100000) return kz_throw_range(ctx, "prototype chain too long");
        if (kz_get_proto(ctx, JV_OBJ(r[1]), &r[1])) return JS_EXCEPTION;
        if (r[1] == JS_NULL) return JS_FALSE;
        if (r[1] == r[0]) return JS_TRUE;
    }
}

static JSValue proto_property_is_enumerable(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[1] = kz_to_property_key(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[0] = this_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    KZ_ROOT_DESC(ctx, d);
    int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[1], &d);
    if (h < 0) return JS_EXCEPTION;
    return JV_MKBOOL(h && (d.flags & KZ_PROP_E));
}

static JSValue proto_to_locale_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return kz_invoke(ctx, this_val, ctx->rt->atom[KZ_ATOM_toString], 0, NULL);
}

static const char *builtin_tag(JSContext *ctx, JSValue o, int *err) {
    *err = 0;
    int a = kz_is_array(ctx, o);
    if (a < 0) { *err = 1; return NULL; }
    if (a) return "Array";
    JSObject *p = JV_OBJ(o);
    switch (kz_class(p)) {
    case KZ_CLASS_ARGUMENTS:
    case KZ_CLASS_MAPPED_ARGUMENTS: return "Arguments";
    case KZ_CLASS_ERROR: return "Error";
    case KZ_CLASS_BOOLEAN: return "Boolean";
    case KZ_CLASS_NUMBER: return "Number";
    case KZ_CLASS_STRING: return "String";
    case KZ_CLASS_DATE: return "Date";
    case KZ_CLASS_REGEXP: return "RegExp";
    default: break;
    }
    if (p->h.flags & OBJ_CALLABLE) return "Function";
    return "Object";
}

JSValue kz_object_to_string_tag(JSContext *ctx, JSValue v) {
    if (v == JS_UNDEFINED) return kz_str_from_cstr(ctx, "[object Undefined]");
    if (v == JS_NULL) return kz_str_from_cstr(ctx, "[object Null]");
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, v);
    if (r[0] == JS_EXCEPTION) return r[0];
    int err;
    const char *tag = builtin_tag(ctx, r[0], &err);
    if (err) return JS_EXCEPTION;
    r[1] = kz_get(ctx, r[0], ctx->rt->sym[KZ_SYM_toStringTag], r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_puts(&sb, "[object ");
    if (JV_IS_STRING(r[1])) kz_sb_put_value(&sb, r[1]);
    else kz_sb_puts(&sb, tag);
    kz_sb_putc(&sb, ']');
    return kz_sb_finish(&sb);
}

static JSValue proto_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return kz_object_to_string_tag(ctx, this_val);
}

static JSValue proto_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return kz_to_object(ctx, this_val);
}

static JSValue proto_get_proto(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (kz_get_proto(ctx, JV_OBJ(r[0]), &r[1])) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_set_proto(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (JV_IS_NULLISH(this_val)) return kz_throw_type(ctx, "Object.prototype.__proto__ called on null or undefined");
    if (!JV_IS_OBJECT(argv[0]) && argv[0] != JS_NULL) return JS_UNDEFINED;
    if (!JV_IS_OBJECT(this_val)) return JS_UNDEFINED;
    if (kz_set_proto(ctx, JV_OBJ(this_val), argv[0], true) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* __defineGetter__ (magic 0) and __defineSetter__ (magic 1) */
static JSValue proto_define_accessor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!kz_is_callable(argv[1])) return kz_throw_type(ctx, "Object.prototype.__define%sSetter__: Expecting function", magic ? "S" : "G");
    r[1] = kz_to_property_key(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KzPropDesc d = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_E | KZ_PROP_C | PD_HAS_E | PD_HAS_C };
    if (magic) { d.setter = argv[1]; d.flags |= PD_HAS_SET; }
    else { d.getter = argv[1]; d.flags |= PD_HAS_GET; }
    if (kz_define_own_property(ctx, JV_OBJ(r[0]), r[1], &d, true) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* __lookupGetter__ (magic 0) and __lookupSetter__ (magic 1) */
static JSValue proto_lookup_accessor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_to_property_key(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (int depth = 0;; depth++) {
        if (depth > 100000) return kz_throw_range(ctx, "prototype chain too long");
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, JV_OBJ(r[0]), r[1], &d);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            if (!(d.flags & PD_ACCESSOR)) return JS_UNDEFINED;
            return magic ? d.setter : d.getter;
        }
        if (kz_get_proto(ctx, JV_OBJ(r[0]), &r[0])) return JS_EXCEPTION;
        if (r[0] == JS_NULL) return JS_UNDEFINED;
    }
}

static const KzFuncDef object_funcs[] = {
    KZ_FN_ARGS("assign", object_assign, 2, 1),
    KZ_FN("create", object_create, 2),
    KZ_FN("defineProperty", object_define_property, 3),
    KZ_FN("defineProperties", object_define_properties, 2),
    KZ_FN_MAGIC("entries", object_keys, 1, 2),
    KZ_FN_MAGIC("freeze", object_integrity, 1, 0),
    KZ_FN("fromEntries", object_from_entries, 1),
    KZ_FN("getOwnPropertyDescriptor", object_gopd, 2),
    KZ_FN("getOwnPropertyDescriptors", object_gopds, 1),
    KZ_FN_MAGIC("getOwnPropertyNames", object_own_names, 1, 0),
    KZ_FN_MAGIC("getOwnPropertySymbols", object_own_names, 1, 1),
    KZ_FN("getPrototypeOf", object_get_prototype_of, 1),
    KZ_FN("groupBy", object_group_by, 2),
    KZ_FN("hasOwn", object_has_own, 2),
    KZ_FN("is", object_is, 2),
    KZ_FN_MAGIC("isExtensible", object_test_integrity, 1, 2),
    KZ_FN_MAGIC("isFrozen", object_test_integrity, 1, 0),
    KZ_FN_MAGIC("isSealed", object_test_integrity, 1, 1),
    KZ_FN_MAGIC("keys", object_keys, 1, 0),
    KZ_FN_MAGIC("preventExtensions", object_integrity, 1, 2),
    KZ_FN_MAGIC("seal", object_integrity, 1, 1),
    KZ_FN("setPrototypeOf", object_set_prototype_of, 2),
    KZ_FN_MAGIC("values", object_keys, 1, 1),
};

static const KzFuncDef object_proto_funcs[] = {
    KZ_FN("hasOwnProperty", proto_has_own_property, 1),
    KZ_FN("isPrototypeOf", proto_is_prototype_of, 1),
    KZ_FN("propertyIsEnumerable", proto_property_is_enumerable, 1),
    KZ_FN("toLocaleString", proto_to_locale_string, 0),
    KZ_FN("valueOf", proto_value_of, 0),
    KZ_ACCESSOR("__proto__", proto_get_proto, proto_set_proto),
    KZ_FN_MAGIC("__defineGetter__", proto_define_accessor, 2, 0),
    KZ_FN_MAGIC("__defineSetter__", proto_define_accessor, 2, 1),
    KZ_FN_MAGIC("__lookupGetter__", proto_lookup_accessor, 1, 0),
    KZ_FN_MAGIC("__lookupSetter__", proto_lookup_accessor, 1, 1),
};

int kz_init_object(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 2);
    JSValue proto = kz_intr(ctx, KZ_I_OBJECT_PROTO);
    r[0] = kz_make_ctor(ctx, object_ctor, "Object", 1, 0, KZ_I_OBJECT_PROTO, KZ_I_OBJECT);
    if (r[0] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], object_funcs, KZ_COUNTOF(object_funcs))) return -1;
    if (kz_define_functions(ctx, proto, object_proto_funcs, KZ_COUNTOF(object_proto_funcs))) return -1;
    r[1] = kz_new_native_c(ctx, proto_to_string, "toString", 0, 0);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_OBJECT_PROTO_TOSTRING] = r[1];
    if (kz_define_value_atom(ctx, proto, KZ_ATOM_toString, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Object, r[0], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
