/*
 * builtins_array.c: the Array constructor and Array.prototype (ECMA-262 23.1).
 *
 * Every method is written as the specification's generic algorithm over [[HasProperty]], [[Get]], [[Set]] and
 * [[Delete]], so it works on any array-like.  Dense arrays get fast paths where nothing observable can differ:
 * the array must be "fast" (kz_array_is_fast: no index properties in its shape, extensible, plain attributes) and
 * nothing on its prototype chain may have index properties (then a hole reads as undefined and a store cannot hit a
 * setter).
 */
#include "jsint.h"

#define MAX_SAFE 9007199254740991.0     /* 2^53 - 1 */

/* ================================================================== element access by a 64-bit index */

static JSValue idx_key(JSContext *ctx, uint64_t k) {
    if (k <= KZ_MAX_INDEX) return KZ_KEY_FROM_INDEX((uint32_t)k);
    return kz_key_from_value(ctx, jv_number((double)k));
}

static int a_has(JSContext *ctx, JSValue o, uint64_t k) {
    JSObject *p = JV_OBJ(o);
    if (k < p->elem_len && p->elems[k] != JS_HOLE && !(p->h.flags & OBJ_EXOTIC)) return 1;
    KZ_ROOT(ctx, key);
    key = idx_key(ctx, k);
    if (key == JS_EXCEPTION) return -1;
    return kz_has_property(ctx, p, key);
}

static JSValue a_get(JSContext *ctx, JSValue o, uint64_t k) {
    if (k <= KZ_MAX_INDEX) return kz_get_index(ctx, o, (uint32_t)k);
    KZ_ROOT(ctx, key);
    key = idx_key(ctx, k);
    if (key == JS_EXCEPTION) return key;
    return kz_get(ctx, o, key, o);
}

static int a_set(JSContext *ctx, JSValue o, uint64_t k, JSValue v) {
    if (k <= KZ_MAX_INDEX) return kz_set_index(ctx, o, (uint32_t)k, v, true) < 0 ? -1 : 0;
    KZ_ROOTS(ctx, r, 2);
    r[0] = v;
    r[1] = idx_key(ctx, k);
    if (r[1] == JS_EXCEPTION) return -1;
    return kz_set(ctx, o, r[1], r[0], o, true) < 0 ? -1 : 0;
}

static int a_del(JSContext *ctx, JSValue o, uint64_t k) {
    KZ_ROOT(ctx, key);
    key = idx_key(ctx, k);
    if (key == JS_EXCEPTION) return -1;
    return kz_delete_property(ctx, JV_OBJ(o), key, true) < 0 ? -1 : 0;
}

static int a_create(JSContext *ctx, JSValue o, uint64_t k, JSValue v) {
    if (k <= KZ_MAX_INDEX) return kz_create_data_property(ctx, o, KZ_KEY_FROM_INDEX((uint32_t)k), v);
    KZ_ROOTS(ctx, r, 2);
    r[0] = v;
    r[1] = idx_key(ctx, k);
    if (r[1] == JS_EXCEPTION) return -1;
    return kz_create_data_property(ctx, o, r[1], r[0]);
}

static int set_length(JSContext *ctx, JSValue o, uint64_t len) {
    if (kz_class(JV_OBJ(o)) == KZ_CLASS_ARRAY && kz_array_is_fast(JV_OBJ(o)) && len <= 0xFFFFFFFFu) {
        JSObject *a = JV_OBJ(o);
        if (len >= a->u.array.length) {
            a->u.array.length = (uint32_t)len;
            return 0;
        }
    }
    return kz_set(ctx, o, ctx->rt->atom[KZ_ATOM_length], jv_number((double)len), o, true) < 0 ? -1 : 0;
}

/* nothing on o's prototype chain has index properties (or exotic behaviour): holes read as undefined */
static bool protos_plain(JSObject *o) {
    JSValue p = o->shape->proto;
    for (int depth = 0; JV_IS_OBJECT(p); depth++) {
        JSObject *q = JV_OBJ(p);
        if (depth > 8 || (q->h.flags & OBJ_EXOTIC) || (q->shape->h.flags & SHAPE_HAS_INDEX)) return false;
        for (uint32_t i = 0; i < q->elem_len; i++)
            if (q->elems[i] != JS_HOLE) return false;
        p = q->shape->proto;
    }
    return true;
}

/* a dense fast array whose holes and stores behave plainly */
static bool fast_array(JSValue o) {
    if (!JV_IS_OBJECT(o)) return false;
    JSObject *a = JV_OBJ(o);
    return kz_class(a) == KZ_CLASS_ARRAY && kz_array_is_fast(a) && protos_plain(a);
}

static bool has_holes(JSObject *a, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++)
        if (a->elems[i] == JS_HOLE) return true;
    return false;
}

static int this_obj_len(JSContext *ctx, JSValue this_val, JSValue *obj, uint64_t *len) {
    *obj = kz_to_object(ctx, this_val);
    if (*obj == JS_EXCEPTION) return -1;
    return kz_length_of(ctx, *obj, len);
}

static double clamp_rel(double rel, double len) {
    if (rel < 0) { rel += len; return rel < 0 ? 0 : rel; }
    return rel > len ? len : rel;
}

static int rel_arg(JSContext *ctx, JSValue v, double len, double def, double *out) {
    if (v == JS_UNDEFINED) { *out = def; return 0; }
    double d;
    if (kz_to_integer_or_inf(ctx, v, &d)) return -1;
    *out = clamp_rel(d, len);
    return 0;
}

/* ================================================================== ArrayCreate, ArraySpeciesCreate */

JSValue kz_array_create(JSContext *ctx, uint64_t len, JSValue proto) {
    if (len > 0xFFFFFFFFu) return kz_throw_range(ctx, "Invalid array length");
    KZ_ROOT(ctx, rp);
    rp = proto;
    JSValue a = kz_new_array(ctx, (uint32_t)len);
    if (a == JS_EXCEPTION) return a;
    if (JV_IS_OBJECT(rp) && rp != kz_intr(ctx, KZ_I_ARRAY_PROTO)) {
        KZ_ROOT(ctx, ra);
        ra = a;
        if (kz_obj_set_proto_raw(ctx, JV_OBJ(ra), rp)) return JS_EXCEPTION;
        return ra;
    }
    return a;
}

JSValue kz_array_species_create(JSContext *ctx, JSValue orig, uint64_t len) {
    int isa = kz_is_array(ctx, orig);
    if (isa < 0) return JS_EXCEPTION;
    if (!isa) return kz_array_create(ctx, len, JS_UNDEFINED);
    KZ_ROOT(ctx, c);
    c = kz_get(ctx, orig, ctx->rt->atom[KZ_ATOM_constructor], orig);
    if (c == JS_EXCEPTION) return c;
    if (kz_is_constructor(c)) {
        JSContext *realm = kz_function_realm(ctx, JV_OBJ(c));
        if (!realm) return JS_EXCEPTION;
        if (realm != ctx && c == kz_intr(realm, KZ_I_ARRAY)) c = JS_UNDEFINED;
    }
    if (JV_IS_OBJECT(c)) {
        c = kz_get(ctx, c, ctx->rt->sym[KZ_SYM_species], c);
        if (c == JS_EXCEPTION) return c;
        if (c == JS_NULL) c = JS_UNDEFINED;
    }
    if (c == JS_UNDEFINED) return kz_array_create(ctx, len, JS_UNDEFINED);
    if (!kz_is_constructor(c)) return kz_throw_type(ctx, "object.constructor[Symbol.species] is not a constructor");
    JSValue arg = jv_number((double)len);
    return kz_construct(ctx, c, 1, &arg, c);
}

/* ================================================================== the constructor and statics */

static JSValue array_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    JSValue nt = rt->new_target == JS_UNDEFINED ? JV_MKOBJ(rt->cur_native) : rt->new_target;
    if (kz_get_proto_from_ctor(ctx, nt, KZ_I_ARRAY_PROTO, &r[0])) return JS_EXCEPTION;
    if (argc == 0) return kz_array_create(ctx, 0, r[0]);
    if (argc == 1) {
        JSValue len = argv[0];
        r[1] = kz_array_create(ctx, 0, r[0]);
        if (r[1] == JS_EXCEPTION) return r[1];
        if (!JV_IS_NUMBER(len)) {
            if (kz_create_data_property(ctx, r[1], KZ_KEY_FROM_INDEX(0), len)) return JS_EXCEPTION;
            return r[1];
        }
        double d = jv_num(len);
        uint32_t n = (uint32_t)kz_double_to_int32(d);
        if ((double)n != d) return kz_throw_range(ctx, "Invalid array length");
        JV_OBJ(r[1])->u.array.length = n;
        return r[1];
    }
    r[1] = kz_array_create(ctx, 0, r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSObject *a = JV_OBJ(r[1]);
    if (kz_elems_reserve(ctx, a, (uint32_t)argc)) return JS_EXCEPTION;
    for (int i = 0; i < argc; i++) a->elems[i] = argv[i];
    a->elem_len = a->u.array.length = (uint32_t)argc;
    return r[1];
}

static JSValue array_is_array(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int r = kz_is_array(ctx, argv[0]);
    if (r < 0) return JS_EXCEPTION;
    return JV_MKBOOL(r);
}

static JSValue array_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOT(ctx, a);
    if (kz_is_constructor(this_val)) {
        JSValue n = JV_MKINT(argc);
        a = kz_construct(ctx, this_val, 1, &n, this_val);
    } else a = kz_array_create(ctx, (uint64_t)argc, JS_UNDEFINED);
    if (a == JS_EXCEPTION) return a;
    for (int i = 0; i < argc; i++)
        if (kz_create_data_property(ctx, a, KZ_KEY_FROM_INDEX((uint32_t)i), argv[i])) return JS_EXCEPTION;
    if (set_length(ctx, a, (uint64_t)argc)) return JS_EXCEPTION;
    return a;
}

static JSValue array_from(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSValue items = argv[0], mapfn = argv[1], this_arg = argv[2];
    bool mapping = mapfn != JS_UNDEFINED;
    if (mapping && !kz_is_callable(mapfn)) return kz_throw_not_function(ctx, mapfn);
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_get_method(ctx, items, ctx->rt->sym[KZ_SYM_iterator]);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (r[0] != JS_UNDEFINED) {
        r[1] = kz_is_constructor(this_val) ? kz_construct(ctx, this_val, 0, NULL, this_val) : kz_array_create(ctx, 0, JS_UNDEFINED);
        if (r[1] == JS_EXCEPTION) return r[1];
        KZ_ROOT_ITER(ctx, it);
        if (kz_get_iterator_from_method(ctx, items, r[0], &it)) return JS_EXCEPTION;
        for (double k = 0;; k++) {
            if (k >= MAX_SAFE) {
                kz_throw_type(ctx, "Array.from: too many elements");
                kz_iter_close_on_throw(ctx, &it);
                return JS_EXCEPTION;
            }
            int s = kz_iter_step(ctx, &it, &r[2]);
            if (s < 0) return JS_EXCEPTION;
            if (!s) {
                if (set_length(ctx, r[1], (uint64_t)k)) return JS_EXCEPTION;
                return r[1];
            }
            if (mapping) {
                JSValue args[2] = { r[2], jv_number(k) };
                r[2] = kz_call(ctx, mapfn, this_arg, 2, args);
                if (r[2] == JS_EXCEPTION) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
            }
            if (a_create(ctx, r[1], (uint64_t)k, r[2])) { kz_iter_close_on_throw(ctx, &it); return JS_EXCEPTION; }
        }
    }
    r[0] = kz_to_object(ctx, items);
    if (r[0] == JS_EXCEPTION) return r[0];
    uint64_t len;
    if (kz_length_of(ctx, r[0], &len)) return JS_EXCEPTION;
    if (kz_is_constructor(this_val)) {
        JSValue n = jv_number((double)len);
        r[1] = kz_construct(ctx, this_val, 1, &n, this_val);
    } else r[1] = kz_array_create(ctx, len, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint64_t k = 0; k < len; k++) {
        r[2] = a_get(ctx, r[0], k);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (mapping) {
            JSValue args[2] = { r[2], jv_number((double)k) };
            r[2] = kz_call(ctx, mapfn, this_arg, 2, args);
            if (r[2] == JS_EXCEPTION) return r[2];
        }
        if (a_create(ctx, r[1], k, r[2])) return JS_EXCEPTION;
    }
    if (set_length(ctx, r[1], len)) return JS_EXCEPTION;
    return r[1];
}

/* ================================================================== Array.prototype: access and search */

static JSValue proto_at(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOT(ctx, o);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &o, &len)) return JS_EXCEPTION;
    double rel;
    if (kz_to_integer_or_inf(ctx, argv[0], &rel)) return JS_EXCEPTION;
    double k = rel >= 0 ? rel : (double)len + rel;
    if (k < 0 || k >= (double)len) return JS_UNDEFINED;
    return a_get(ctx, o, (uint64_t)k);
}

/* indexOf (magic 0), lastIndexOf (1), includes (2) */
static JSValue proto_index_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    JSValue none = magic == 2 ? JS_FALSE : JV_MKINT(-1);
    if (len == 0) return none;
    double dlen = (double)len, k;
    if (magic == 1) {
        double n = dlen - 1;
        if (argc > 1 && kz_to_integer_or_inf(ctx, argv[1], &n)) return JS_EXCEPTION;
        if (n == -KZ_INFINITY) return none;
        k = n >= 0 ? (n < dlen - 1 ? n : dlen - 1) : dlen + n;
        JSValue target = argv[0];
        for (; k >= 0; k--) {
            int h = a_has(ctx, r[0], (uint64_t)k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
            r[1] = a_get(ctx, r[0], (uint64_t)k);
            if (r[1] == JS_EXCEPTION) return r[1];
            int e = kz_strict_eq(ctx, target, r[1]);
            if (e < 0) return JS_EXCEPTION;
            if (e) return jv_number(k);
        }
        return none;
    }
    double n = 0;
    if (kz_to_integer_or_inf(ctx, argv[1], &n)) return JS_EXCEPTION;
    if (n == KZ_INFINITY) return none;
    if (n == -KZ_INFINITY) n = 0;
    k = n >= 0 ? n : (dlen + n < 0 ? 0 : dlen + n);
    JSValue target = argv[0];
    /* the fast path: comparisons call no JavaScript, so the elements cannot change under us */
    if (fast_array(r[0])) {
        JSObject *a = JV_OBJ(r[0]);
        for (uint32_t i = (uint32_t)(k < a->elem_len ? k : a->elem_len); i < a->elem_len; i++) {
            JSValue e = a->elems[i];
            if (e == JS_HOLE) {
                if (magic == 2 && target == JS_UNDEFINED) return JS_TRUE;
                continue;
            }
            int eq = magic == 2 ? kz_same_value_zero(ctx, target, e) : kz_strict_eq(ctx, target, e);
            if (eq < 0) return JS_EXCEPTION;
            if (eq) return magic == 2 ? JS_TRUE : jv_number((double)i);
        }
        return none;
    }
    for (; k < dlen; k++) {
        if (magic != 2) {
            int h = a_has(ctx, r[0], (uint64_t)k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
        }
        r[1] = a_get(ctx, r[0], (uint64_t)k);
        if (r[1] == JS_EXCEPTION) return r[1];
        int e = magic == 2 ? kz_same_value_zero(ctx, target, r[1]) : kz_strict_eq(ctx, target, r[1]);
        if (e < 0) return JS_EXCEPTION;
        if (e) return magic == 2 ? JS_TRUE : jv_number(k);
    }
    return none;
}

/* find (0), findIndex (1), findLast (2), findLastIndex (3) */
static JSValue proto_find(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    JSValue pred = argv[0];
    if (!kz_is_callable(pred)) return kz_throw_not_function(ctx, pred);
    bool last = magic >= 2, want_index = magic & 1;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t k = last ? len - 1 - i : i;
        r[1] = a_get(ctx, r[0], k);
        if (r[1] == JS_EXCEPTION) return r[1];
        JSValue args[3] = { r[1], jv_number((double)k), r[0] };
        r[2] = kz_call(ctx, pred, argv[1], 3, args);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_to_bool(r[2])) return want_index ? jv_number((double)k) : r[1];
    }
    return want_index ? JV_MKINT(-1) : JS_UNDEFINED;
}

/* every (0), some (1), forEach (2), map (3), filter (4) */
static JSValue proto_iterate(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 4);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    JSValue cb = argv[0];
    if (!kz_is_callable(cb)) return kz_throw_not_function(ctx, cb);
    if (magic == 3) {
        r[3] = kz_array_species_create(ctx, r[0], len);
        if (r[3] == JS_EXCEPTION) return r[3];
    } else if (magic == 4) {
        r[3] = kz_array_species_create(ctx, r[0], 0);
        if (r[3] == JS_EXCEPTION) return r[3];
    }
    uint64_t to = 0;
    for (uint64_t k = 0; k < len; k++) {
        int h = a_has(ctx, r[0], k);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        r[1] = a_get(ctx, r[0], k);
        if (r[1] == JS_EXCEPTION) return r[1];
        JSValue args[3] = { r[1], jv_number((double)k), r[0] };
        r[2] = kz_call(ctx, cb, argv[1], 3, args);
        if (r[2] == JS_EXCEPTION) return r[2];
        switch (magic) {
        case 0: if (!kz_to_bool(r[2])) return JS_FALSE; break;
        case 1: if (kz_to_bool(r[2])) return JS_TRUE; break;
        case 3: if (a_create(ctx, r[3], k, r[2])) return JS_EXCEPTION; break;
        case 4:
            if (kz_to_bool(r[2])) {
                if (a_create(ctx, r[3], to, r[1])) return JS_EXCEPTION;
                to++;
            }
            break;
        default: break;
        }
    }
    switch (magic) {
    case 0: return JS_TRUE;
    case 1: return JS_FALSE;
    case 2: return JS_UNDEFINED;
    default: return r[3];
    }
}

/* reduce (0), reduceRight (1) */
static JSValue proto_reduce(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    JSValue cb = argv[0];
    if (!kz_is_callable(cb)) return kz_throw_not_function(ctx, cb);
    uint64_t i = 0;
    if (argc >= 2) r[1] = argv[1];
    else {
        bool found = false;
        for (; i < len && !found; i++) {
            uint64_t k = magic ? len - 1 - i : i;
            int h = a_has(ctx, r[0], k);
            if (h < 0) return JS_EXCEPTION;
            if (h) {
                found = true;
                r[1] = a_get(ctx, r[0], k);
                if (r[1] == JS_EXCEPTION) return r[1];
            }
        }
        if (!found) return kz_throw_type(ctx, "Reduce of empty array with no initial value");
    }
    for (; i < len; i++) {
        uint64_t k = magic ? len - 1 - i : i;
        int h = a_has(ctx, r[0], k);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        r[2] = a_get(ctx, r[0], k);
        if (r[2] == JS_EXCEPTION) return r[2];
        JSValue args[4] = { r[1], r[2], jv_number((double)k), r[0] };
        r[1] = kz_call(ctx, cb, JS_UNDEFINED, 4, args);
        if (r[1] == JS_EXCEPTION) return r[1];
    }
    return r[1];
}

/* ================================================================== join, toString, toLocaleString */

static JSValue join_impl(JSContext *ctx, JSValue o, uint64_t len, JSValue sep, bool locale) {
    JSRuntime *rt = ctx->rt;
    for (KzJoinNode *n = rt->join_stack; n; n = n->prev)
        if (n->obj == o) return rt->atom[KZ_ATOM_empty];
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    KzJoinNode node = { rt->join_stack, o };
    rt->join_stack = &node;
    KZ_ROOT(ctx, e);
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    JSValue result = JS_EXCEPTION;
    JSString *sp = JV_STR(sep);
    for (uint64_t k = 0; k < len; k++) {
        if (k > 0 && sp->h.aux && kz_sb_put_str(&sb, sp)) goto done;
        e = a_get(ctx, o, k);
        if (e == JS_EXCEPTION) goto done;
        if (JV_IS_NULLISH(e)) continue;
        if (locale) {
            e = kz_invoke(ctx, e, rt->atom[KZ_ATOM_toLocaleString], 0, NULL);
            if (e == JS_EXCEPTION) goto done;
        }
        if (!JV_IS_STRING(e)) {
            e = kz_to_string(ctx, e);
            if (e == JS_EXCEPTION) goto done;
        }
        if (kz_sb_put_value(&sb, e)) goto done;
        if ((k & 1023) == 1023) kz_poll_interrupt(ctx);
        if (kz_has_exception(ctx)) goto done;
    }
    result = kz_sb_finish(&sb);
    sb.buf = NULL;
done:
    if (sb.buf) kz_sb_free(&sb);
    rt->join_stack = node.prev;
    return result;
}

static JSValue proto_join(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (argv[0] == JS_UNDEFINED) r[1] = kz_str_from_char(ctx, ',');
    else r[1] = kz_to_string(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[1] = kz_str_flatten(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    return join_impl(ctx, r[0], len, r[1], false);
}

static JSValue proto_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_get(ctx, r[0], ctx->rt->atom[KZ_ATOM_join], r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (!kz_is_callable(r[1])) return kz_object_to_string_tag(ctx, r[0]);
    return kz_call(ctx, r[1], r[0], 0, NULL);
}

static JSValue proto_to_locale_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    r[1] = kz_str_from_char(ctx, ',');
    if (r[1] == JS_EXCEPTION) return r[1];
    return join_impl(ctx, r[0], len, r[1], true);
}

/* ================================================================== mutators */

static JSValue proto_push(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOT(ctx, o);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &o, &len)) return JS_EXCEPTION;
    if ((double)len + argc > MAX_SAFE) return kz_throw_type(ctx, "Pushing %d elements on an array-like of length %s is disallowed",
                                                           argc, "2^53-1");
    if (fast_array(o) && len + (uint64_t)argc < 0xFFFFFFFFu) {
        JSObject *a = JV_OBJ(o);
        if (kz_elems_reserve(ctx, a, (uint32_t)len + (uint32_t)argc)) return JS_EXCEPTION;
        for (int i = 0; i < argc; i++) a->elems[len + (uint64_t)i] = argv[i];
        a->elem_len = a->u.array.length = (uint32_t)len + (uint32_t)argc;
        return jv_number((double)(len + (uint64_t)argc));
    }
    for (int i = 0; i < argc; i++, len++)
        if (a_set(ctx, o, len, argv[i])) return JS_EXCEPTION;
    if (set_length(ctx, o, len)) return JS_EXCEPTION;
    return jv_number((double)len);
}

static JSValue proto_pop(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (len == 0) {
        if (set_length(ctx, r[0], 0)) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (fast_array(r[0])) {
        JSObject *a = JV_OBJ(r[0]);
        JSValue v = a->elems[len - 1];
        a->elem_len = a->u.array.length = (uint32_t)len - 1;
        return v == JS_HOLE ? JS_UNDEFINED : v;
    }
    r[1] = a_get(ctx, r[0], len - 1);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (a_del(ctx, r[0], len - 1) || set_length(ctx, r[0], len - 1)) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_shift(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (len == 0) {
        if (set_length(ctx, r[0], 0)) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (fast_array(r[0])) {
        JSObject *a = JV_OBJ(r[0]);
        JSValue first = a->elems[0];
        memmove(a->elems, a->elems + 1, (size_t)(len - 1) * sizeof(JSValue));
        a->elem_len = a->u.array.length = (uint32_t)len - 1;
        return first == JS_HOLE ? JS_UNDEFINED : first;
    }
    r[1] = a_get(ctx, r[0], 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint64_t k = 1; k < len; k++) {
        int h = a_has(ctx, r[0], k);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            r[2] = a_get(ctx, r[0], k);
            if (r[2] == JS_EXCEPTION || a_set(ctx, r[0], k - 1, r[2])) return JS_EXCEPTION;
        } else if (a_del(ctx, r[0], k - 1)) return JS_EXCEPTION;
    }
    if (a_del(ctx, r[0], len - 1) || set_length(ctx, r[0], len - 1)) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_unshift(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (argc > 0) {
        if ((double)len + argc > MAX_SAFE) return kz_throw_type(ctx, "Array length exceeds 2^53-1");
        if (fast_array(r[0]) && len + (uint64_t)argc < 0xFFFFFFFFu) {
            JSObject *a = JV_OBJ(r[0]);
            if (kz_elems_reserve(ctx, a, (uint32_t)len + (uint32_t)argc)) return JS_EXCEPTION;
            memmove(a->elems + argc, a->elems, (size_t)len * sizeof(JSValue));
            for (int i = 0; i < argc; i++) a->elems[i] = argv[i];
            a->elem_len = a->u.array.length = (uint32_t)len + (uint32_t)argc;
            return jv_number((double)(len + (uint64_t)argc));
        }
        for (uint64_t k = len; k > 0; k--) {
            int h = a_has(ctx, r[0], k - 1);
            if (h < 0) return JS_EXCEPTION;
            if (h) {
                r[1] = a_get(ctx, r[0], k - 1);
                if (r[1] == JS_EXCEPTION || a_set(ctx, r[0], k - 1 + (uint64_t)argc, r[1])) return JS_EXCEPTION;
            } else if (a_del(ctx, r[0], k - 1 + (uint64_t)argc)) return JS_EXCEPTION;
        }
        for (int j = 0; j < argc; j++)
            if (a_set(ctx, r[0], (uint64_t)j, argv[j])) return JS_EXCEPTION;
    }
    if (set_length(ctx, r[0], len + (uint64_t)argc)) return JS_EXCEPTION;
    return jv_number((double)(len + (uint64_t)argc));
}

static JSValue proto_splice(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double dlen = (double)len, start, del;
    if (rel_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, dlen, 0, &start)) return JS_EXCEPTION;
    if (argc > 0 && argv[0] == JS_UNDEFINED) start = 0;
    uint64_t item_count = argc > 2 ? (uint64_t)(argc - 2) : 0;
    if (argc == 0) del = 0;
    else if (argc == 1) del = dlen - start;
    else {
        double dc;
        if (kz_to_integer_or_inf(ctx, argv[1], &dc)) return JS_EXCEPTION;
        del = dc < 0 ? 0 : dc > dlen - start ? dlen - start : dc;
    }
    if (dlen + (double)item_count - del > MAX_SAFE) return kz_throw_type(ctx, "Array length exceeds 2^53-1");
    uint64_t as = (uint64_t)start, adc = (uint64_t)del;
    r[1] = kz_array_species_create(ctx, r[0], adc);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSValue *items = argv + 2;
    /* the fast path: a plain dense array, and the result is a plain new array */
    if (fast_array(r[0]) && fast_array(r[1]) && JV_OBJ(r[1])->u.array.length == adc && JV_OBJ(r[1])->elem_len == 0 &&
        len - adc + item_count < 0xFFFFFFFFu) {
        JSObject *a = JV_OBJ(r[0]), *res = JV_OBJ(r[1]);
        if (adc) {
            if (kz_elems_reserve(ctx, res, (uint32_t)adc)) return JS_EXCEPTION;
            a = JV_OBJ(r[0]);
            /* (holes read as undefined only when present on the prototypes; they stay holes in the result) */
            memcpy(res->elems, a->elems + as, (size_t)adc * sizeof(JSValue));
            res->elem_len = (uint32_t)adc;
            res->u.array.length = (uint32_t)adc;
        }
        uint64_t nlen = len - adc + item_count;
        if (item_count > adc && kz_elems_reserve(ctx, a, (uint32_t)nlen)) return JS_EXCEPTION;
        memmove(a->elems + as + item_count, a->elems + as + adc, (size_t)(len - as - adc) * sizeof(JSValue));
        for (uint64_t i = 0; i < item_count; i++) a->elems[as + i] = items[i];
        a->elem_len = a->u.array.length = (uint32_t)nlen;
        return r[1];
    }
    for (uint64_t k = 0; k < adc; k++) {
        int h = a_has(ctx, r[0], as + k);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            r[2] = a_get(ctx, r[0], as + k);
            if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], k, r[2])) return JS_EXCEPTION;
        }
    }
    if (set_length(ctx, r[1], adc)) return JS_EXCEPTION;
    if (item_count < adc) {
        for (uint64_t k = as; k < len - adc; k++) {
            int h = a_has(ctx, r[0], k + adc);
            if (h < 0) return JS_EXCEPTION;
            if (h) {
                r[2] = a_get(ctx, r[0], k + adc);
                if (r[2] == JS_EXCEPTION || a_set(ctx, r[0], k + item_count, r[2])) return JS_EXCEPTION;
            } else if (a_del(ctx, r[0], k + item_count)) return JS_EXCEPTION;
        }
        for (uint64_t k = len; k > len - adc + item_count; k--)
            if (a_del(ctx, r[0], k - 1)) return JS_EXCEPTION;
    } else if (item_count > adc) {
        for (uint64_t k = len - adc; k > as; k--) {
            int h = a_has(ctx, r[0], k + adc - 1);
            if (h < 0) return JS_EXCEPTION;
            if (h) {
                r[2] = a_get(ctx, r[0], k + adc - 1);
                if (r[2] == JS_EXCEPTION || a_set(ctx, r[0], k + item_count - 1, r[2])) return JS_EXCEPTION;
            } else if (a_del(ctx, r[0], k + item_count - 1)) return JS_EXCEPTION;
        }
    }
    for (uint64_t i = 0; i < item_count; i++)
        if (a_set(ctx, r[0], as + i, items[i])) return JS_EXCEPTION;
    if (set_length(ctx, r[0], len - adc + item_count)) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_reverse(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (fast_array(r[0]) && !has_holes(JV_OBJ(r[0]), 0, (uint32_t)len)) {
        JSValue *e = JV_OBJ(r[0])->elems;
        for (uint64_t lo = 0, hi = len ? len - 1 : 0; lo < hi; lo++, hi--) {
            JSValue t = e[lo];
            e[lo] = e[hi];
            e[hi] = t;
        }
        return r[0];
    }
    uint64_t middle = len / 2;
    for (uint64_t lower = 0; lower != middle; lower++) {
        uint64_t upper = len - lower - 1;
        int le = a_has(ctx, r[0], lower);
        if (le < 0) return JS_EXCEPTION;
        if (le) { r[1] = a_get(ctx, r[0], lower); if (r[1] == JS_EXCEPTION) return r[1]; }
        int ue = a_has(ctx, r[0], upper);
        if (ue < 0) return JS_EXCEPTION;
        if (ue) { r[2] = a_get(ctx, r[0], upper); if (r[2] == JS_EXCEPTION) return r[2]; }
        if (le && ue) {
            if (a_set(ctx, r[0], lower, r[2]) || a_set(ctx, r[0], upper, r[1])) return JS_EXCEPTION;
        } else if (ue) {
            if (a_set(ctx, r[0], lower, r[2]) || a_del(ctx, r[0], upper)) return JS_EXCEPTION;
        } else if (le) {
            if (a_del(ctx, r[0], lower) || a_set(ctx, r[0], upper, r[1])) return JS_EXCEPTION;
        }
    }
    return r[0];
}

static JSValue proto_fill(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOT(ctx, o);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &o, &len)) return JS_EXCEPTION;
    double k, fin;
    if (rel_arg(ctx, argv[1], (double)len, 0, &k) || rel_arg(ctx, argv[2], (double)len, (double)len, &fin)) return JS_EXCEPTION;
    for (; k < fin; k++)
        if (a_set(ctx, o, (uint64_t)k, argv[0])) return JS_EXCEPTION;
    return o;
}

static JSValue proto_copy_within(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double dlen = (double)len, to, from, fin;
    if (rel_arg(ctx, argv[0], dlen, 0, &to)) return JS_EXCEPTION;
    if (argv[0] == JS_UNDEFINED) to = 0;
    if (rel_arg(ctx, argv[1], dlen, 0, &from)) return JS_EXCEPTION;
    if (rel_arg(ctx, argv[2], dlen, dlen, &fin)) return JS_EXCEPTION;
    double count = fin - from < dlen - to ? fin - from : dlen - to;
    int dir = 1;
    if (from < to && to < from + count) {
        dir = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; count--, from += dir, to += dir) {
        int h = a_has(ctx, r[0], (uint64_t)from);
        if (h < 0) return JS_EXCEPTION;
        if (h) {
            r[1] = a_get(ctx, r[0], (uint64_t)from);
            if (r[1] == JS_EXCEPTION || a_set(ctx, r[0], (uint64_t)to, r[1])) return JS_EXCEPTION;
        } else if (a_del(ctx, r[0], (uint64_t)to)) return JS_EXCEPTION;
    }
    return r[0];
}

/* ================================================================== copies */

static JSValue proto_slice(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double k, fin;
    if (rel_arg(ctx, argv[0], (double)len, 0, &k) || rel_arg(ctx, argv[1], (double)len, (double)len, &fin)) return JS_EXCEPTION;
    uint64_t count = fin > k ? (uint64_t)(fin - k) : 0;
    r[1] = kz_array_species_create(ctx, r[0], count);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (count && fast_array(r[0]) && fast_array(r[1]) && JV_OBJ(r[1])->elem_len == 0 && JV_OBJ(r[1])->u.array.length == count &&
        k + count <= JV_OBJ(r[0])->elem_len) {
        JSObject *res = JV_OBJ(r[1]);
        if (kz_elems_reserve(ctx, res, (uint32_t)count)) return JS_EXCEPTION;
        memcpy(res->elems, JV_OBJ(r[0])->elems + (uint64_t)k, (size_t)count * sizeof(JSValue));
        res->elem_len = (uint32_t)count;
        return r[1];
    }
    uint64_t n = 0;
    for (; k < fin; k++, n++) {
        int h = a_has(ctx, r[0], (uint64_t)k);
        if (h < 0) return JS_EXCEPTION;
        if (!h) continue;
        r[2] = a_get(ctx, r[0], (uint64_t)k);
        if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], n, r[2])) return JS_EXCEPTION;
    }
    if (set_length(ctx, r[1], n)) return JS_EXCEPTION;
    return r[1];
}

static int is_concat_spreadable(JSContext *ctx, JSValue v) {
    if (!JV_IS_OBJECT(v)) return 0;
    JSValue s = kz_get(ctx, v, ctx->rt->sym[KZ_SYM_isConcatSpreadable], v);
    if (s == JS_EXCEPTION) return -1;
    if (s != JS_UNDEFINED) return kz_to_bool(s);
    return kz_is_array(ctx, v);
}

static JSValue proto_concat(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_array_species_create(ctx, r[0], 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    uint64_t n = 0;
    for (int i = -1; i < argc; i++) {
        JSValue e = i < 0 ? r[0] : argv[i];
        int sp = is_concat_spreadable(ctx, e);
        if (sp < 0) return JS_EXCEPTION;
        if (!sp) {
            if ((double)n >= MAX_SAFE) return kz_throw_type(ctx, "Array length exceeds 2^53-1");
            if (a_create(ctx, r[1], n, e)) return JS_EXCEPTION;
            n++;
            continue;
        }
        uint64_t len;
        if (kz_length_of(ctx, e, &len)) return JS_EXCEPTION;
        if ((double)n + (double)len > MAX_SAFE) return kz_throw_type(ctx, "Array length exceeds 2^53-1");
        if (fast_array(e) && fast_array(r[1]) && JV_OBJ(r[1])->elem_len == n && n + len < 0xFFFFFFFFu) {
            JSObject *src = JV_OBJ(e), *dst = JV_OBJ(r[1]);
            if (!has_holes(src, 0, (uint32_t)len)) {
                if (kz_elems_reserve(ctx, dst, (uint32_t)(n + len))) return JS_EXCEPTION;
                memcpy(dst->elems + n, src->elems, (size_t)len * sizeof(JSValue));
                n += len;
                dst->elem_len = dst->u.array.length = (uint32_t)n;
                continue;
            }
        }
        for (uint64_t k = 0; k < len; k++, n++) {
            int h = a_has(ctx, e, k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
            r[2] = a_get(ctx, e, k);
            if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], n, r[2])) return JS_EXCEPTION;
        }
    }
    if (set_length(ctx, r[1], n)) return JS_EXCEPTION;
    return r[1];
}

/* FlattenIntoArray: returns the next target index, or -1 */
static double flatten_into(JSContext *ctx, JSValue target, JSValue source, uint64_t source_len, double start, double depth,
                           JSValue mapper, JSValue this_arg) {
    if (kz_check_stack(ctx)) return -1;
    KZ_ROOTS(ctx, r, 1);
    double ti = start;
    for (uint64_t si = 0; si < source_len; si++) {
        int h = a_has(ctx, source, si);
        if (h < 0) return -1;
        if (!h) continue;
        r[0] = a_get(ctx, source, si);
        if (r[0] == JS_EXCEPTION) return -1;
        if (mapper != JS_UNDEFINED) {
            JSValue args[3] = { r[0], jv_number((double)si), source };
            r[0] = kz_call(ctx, mapper, this_arg, 3, args);
            if (r[0] == JS_EXCEPTION) return -1;
        }
        int flat = 0;
        if (depth > 0) {
            flat = kz_is_array(ctx, r[0]);
            if (flat < 0) return -1;
        }
        if (flat) {
            uint64_t el;
            if (kz_length_of(ctx, r[0], &el)) return -1;
            ti = flatten_into(ctx, target, r[0], el, ti, depth == KZ_INFINITY ? depth : depth - 1, JS_UNDEFINED, JS_UNDEFINED);
            if (ti < 0) return -1;
        } else {
            if (ti >= MAX_SAFE) { kz_throw_type(ctx, "Array length exceeds 2^53-1"); return -1; }
            if (a_create(ctx, target, (uint64_t)ti, r[0])) return -1;
            ti++;
        }
    }
    return ti;
}

static JSValue proto_flat(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double depth = 1;
    if (argv[0] != JS_UNDEFINED) {
        if (kz_to_integer_or_inf(ctx, argv[0], &depth)) return JS_EXCEPTION;
        if (depth < 0) depth = 0;
    }
    r[1] = kz_array_species_create(ctx, r[0], 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (flatten_into(ctx, r[1], r[0], len, 0, depth, JS_UNDEFINED, JS_UNDEFINED) < 0) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_flat_map(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    if (!kz_is_callable(argv[0])) return kz_throw_not_function(ctx, argv[0]);
    r[1] = kz_array_species_create(ctx, r[0], 0);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (flatten_into(ctx, r[1], r[0], len, 0, 1, argv[0], argv[1]) < 0) return JS_EXCEPTION;
    return r[1];
}

static JSValue proto_to_reversed(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    r[1] = kz_array_create(ctx, len, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint64_t k = 0; k < len; k++) {
        r[2] = a_get(ctx, r[0], len - k - 1);
        if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], k, r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

static JSValue proto_with(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double rel;
    if (kz_to_integer_or_inf(ctx, argv[0], &rel)) return JS_EXCEPTION;
    double idx = rel >= 0 ? rel : (double)len + rel;
    if (idx >= (double)len || idx < 0) return kz_throw_range(ctx, "Invalid index");
    r[1] = kz_array_create(ctx, len, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return r[1];
    for (uint64_t k = 0; k < len; k++) {
        if ((double)k == idx) r[2] = argv[1];
        else {
            r[2] = a_get(ctx, r[0], k);
            if (r[2] == JS_EXCEPTION) return r[2];
        }
        if (a_create(ctx, r[1], k, r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

static JSValue proto_to_spliced(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOTS(ctx, r, 3);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    double dlen = (double)len, start, skip;
    if (rel_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, dlen, 0, &start)) return JS_EXCEPTION;
    uint64_t insert = argc > 2 ? (uint64_t)(argc - 2) : 0;
    if (argc == 0) skip = 0;
    else if (argc == 1) skip = dlen - start;
    else {
        double sc;
        if (kz_to_integer_or_inf(ctx, argv[1], &sc)) return JS_EXCEPTION;
        skip = sc < 0 ? 0 : sc > dlen - start ? dlen - start : sc;
    }
    double nlen = dlen + (double)insert - skip;
    if (nlen > MAX_SAFE) return kz_throw_type(ctx, "Array length exceeds 2^53-1");
    r[1] = kz_array_create(ctx, (uint64_t)nlen, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return r[1];
    uint64_t i = 0, as = (uint64_t)start, rd = as + (uint64_t)skip;
    for (; i < as; i++) {
        r[2] = a_get(ctx, r[0], i);
        if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], i, r[2])) return JS_EXCEPTION;
    }
    for (uint64_t j = 0; j < insert; j++, i++)
        if (a_create(ctx, r[1], i, argv[2 + j])) return JS_EXCEPTION;
    for (; i < (uint64_t)nlen; i++, rd++) {
        r[2] = a_get(ctx, r[0], rd);
        if (r[2] == JS_EXCEPTION || a_create(ctx, r[1], i, r[2])) return JS_EXCEPTION;
    }
    return r[1];
}

/* ================================================================== sorting (a stable merge sort) */

typedef struct SortCtx {
    JSContext *ctx;
    JSValue cmp;                /* the comparator, or undefined */
    JSValue *items;
    JSValue *keys;              /* the ToString of each item (default order) */
} SortCtx;

/* SortCompare(items[a], items[b]) < 0 ? returns 1 when b must come before a (strictly greater), 0, or -1 */
static int sort_greater(SortCtx *sc, uint32_t a, uint32_t b) {
    JSContext *ctx = sc->ctx;
    if (sc->cmp == JS_UNDEFINED) {
        JSString *x = JV_STR(sc->keys[a]), *y = JV_STR(sc->keys[b]);
        return kz_str_cmp(x, y) > 0;
    }
    JSValue args[2] = { sc->items[a], sc->items[b] };
    JSValue v = kz_call(ctx, sc->cmp, JS_UNDEFINED, 2, args);
    if (v == JS_EXCEPTION) return -1;
    double d;
    if (JV_IS_INT(v)) return JV_INT(v) > 0;
    if (kz_to_number(ctx, v, &d)) return -1;
    return d > 0;
}

static int merge_sort(SortCtx *sc, uint32_t *idx, uint32_t *tmp, uint32_t n) {
    /* insertion sort on runs of 8, then bottom-up merges */
    const uint32_t RUN = 8;
    for (uint32_t lo = 0; lo < n; lo += RUN) {
        uint32_t hi = lo + RUN < n ? lo + RUN : n;
        for (uint32_t i = lo + 1; i < hi; i++) {
            uint32_t x = idx[i];
            uint32_t j = i;
            while (j > lo) {
                int g = sort_greater(sc, idx[j - 1], x);
                if (g < 0) return -1;
                if (!g) break;
                idx[j] = idx[j - 1];
                j--;
            }
            idx[j] = x;
        }
    }
    uint32_t *src = idx, *dst = tmp;
    for (uint32_t width = RUN; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int g = sort_greater(sc, src[i], src[j]);
                if (g < 0) return -1;
                dst[k++] = g ? src[j++] : src[i++];
            }
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        uint32_t *t = src;
        src = dst;
        dst = t;
        if (width > 0x7FFFFFFFu / 2) break;
    }
    if (src != idx) memcpy(idx, src, (size_t)n * sizeof(uint32_t));
    return 0;
}

/* SortIndexedProperties then sorting: the sorted values in *out (rooted by the caller), holes skipped when skip */
static int sort_values(JSContext *ctx, JSValue o, uint64_t len, JSValue cmp, bool skip_holes, KzVec *out) {
    KZ_ROOT_VEC(ctx, items);
    KZ_ROOT_VEC(ctx, keys);
    KZ_ROOT(ctx, v);
    uint32_t undef = 0;
    for (uint64_t k = 0; k < len; k++) {
        if (skip_holes) {
            int h = a_has(ctx, o, k);
            if (h < 0) return -1;
            if (!h) continue;
        }
        v = a_get(ctx, o, k);
        if (v == JS_EXCEPTION) return -1;
        if (v == JS_UNDEFINED) { undef++; continue; }
        if (kz_vec_push(ctx, &items, v)) return -1;
    }
    uint32_t n = items.len;
    if (cmp == JS_UNDEFINED) {
        for (uint32_t i = 0; i < n; i++) {
            v = kz_to_string(ctx, items.v[i]);
            if (v == JS_EXCEPTION) return -1;
            v = kz_str_flatten(ctx, v);
            if (v == JS_EXCEPTION || kz_vec_push(ctx, &keys, v)) return -1;
        }
    }
    uint32_t *idx = n ? kz_malloc(ctx, (size_t)n * 2 * sizeof(uint32_t)) : NULL;
    if (n && !idx) return -1;
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    SortCtx sc = { ctx, cmp, items.v, keys.v };
    int rc = n > 1 ? merge_sort(&sc, idx, idx + n, n) : 0;
    if (rc == 0) {
        for (uint32_t i = 0; i < n && rc == 0; i++) rc = kz_vec_push(ctx, out, items.v[idx[i]]);
        for (uint32_t i = 0; i < undef && rc == 0; i++) rc = kz_vec_push(ctx, out, JS_UNDEFINED);
    }
    kz_free(ctx->rt, idx);
    return rc;
}

static JSValue proto_sort(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSValue cmp = argv[0];
    if (cmp != JS_UNDEFINED && !kz_is_callable(cmp)) return kz_throw_type(ctx, "The comparison function must be either a function or undefined");
    KZ_ROOT(ctx, o);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &o, &len)) return JS_EXCEPTION;
    KZ_ROOT_VEC(ctx, sorted);
    if (sort_values(ctx, o, len, cmp, true, &sorted)) return JS_EXCEPTION;
    uint64_t j = 0;
    for (; j < sorted.len; j++)
        if (a_set(ctx, o, j, sorted.v[j])) return JS_EXCEPTION;
    for (; j < len; j++) {
        /* the holes, now at the end (only an issue when they were skipped) */
        if (a_del(ctx, o, j)) return JS_EXCEPTION;
    }
    return o;
}

static JSValue proto_to_sorted(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSValue cmp = argv[0];
    if (cmp != JS_UNDEFINED && !kz_is_callable(cmp)) return kz_throw_type(ctx, "The comparison function must be either a function or undefined");
    KZ_ROOTS(ctx, r, 2);
    uint64_t len;
    if (this_obj_len(ctx, this_val, &r[0], &len)) return JS_EXCEPTION;
    r[1] = kz_array_create(ctx, len, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return r[1];
    KZ_ROOT_VEC(ctx, sorted);
    if (sort_values(ctx, r[0], len, cmp, false, &sorted)) return JS_EXCEPTION;
    for (uint32_t j = 0; j < sorted.len; j++)
        if (a_create(ctx, r[1], j, sorted.v[j])) return JS_EXCEPTION;
    return r[1];
}

/* ================================================================== iterators */

static JSValue proto_iter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    KZ_ROOT(ctx, o);
    o = kz_to_object(ctx, this_val);
    if (o == JS_EXCEPTION) return o;
    return kz_array_iterator_new(ctx, o, magic);
}

static const KzFuncDef array_funcs[] = {
    KZ_FN_ARGS("from", array_from, 1, 3),
    KZ_FN("isArray", array_is_array, 1),
    KZ_FN("of", array_of, 0),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef array_proto_funcs[] = {
    KZ_FN("at", proto_at, 1),
    KZ_FN("concat", proto_concat, 1),
    KZ_FN_ARGS("copyWithin", proto_copy_within, 2, 3),
    KZ_FN_MAGIC("entries", proto_iter, 0, 2),
    KZ_FN_MAGIC_ARGS("every", proto_iterate, 1, 2, 0),
    KZ_FN_ARGS("fill", proto_fill, 1, 3),
    KZ_FN_MAGIC_ARGS("filter", proto_iterate, 1, 2, 4),
    KZ_FN_MAGIC_ARGS("find", proto_find, 1, 2, 0),
    KZ_FN_MAGIC_ARGS("findIndex", proto_find, 1, 2, 1),
    KZ_FN_MAGIC_ARGS("findLast", proto_find, 1, 2, 2),
    KZ_FN_MAGIC_ARGS("findLastIndex", proto_find, 1, 2, 3),
    KZ_FN_ARGS("flat", proto_flat, 0, 1),
    KZ_FN_ARGS("flatMap", proto_flat_map, 1, 2),
    KZ_FN_MAGIC_ARGS("forEach", proto_iterate, 1, 2, 2),
    KZ_FN_MAGIC_ARGS("includes", proto_index_of, 1, 2, 2),
    KZ_FN_MAGIC_ARGS("indexOf", proto_index_of, 1, 2, 0),
    KZ_FN("join", proto_join, 1),
    KZ_FN_MAGIC("keys", proto_iter, 0, 0),
    KZ_FN_MAGIC_ARGS("lastIndexOf", proto_index_of, 1, 2, 1),
    KZ_FN_MAGIC_ARGS("map", proto_iterate, 1, 2, 3),
    KZ_FN("pop", proto_pop, 0),
    KZ_FN("push", proto_push, 1),
    KZ_FN_MAGIC_ARGS("reduce", proto_reduce, 1, 2, 0),
    KZ_FN_MAGIC_ARGS("reduceRight", proto_reduce, 1, 2, 1),
    KZ_FN("reverse", proto_reverse, 0),
    KZ_FN("shift", proto_shift, 0),
    KZ_FN("slice", proto_slice, 2),
    KZ_FN_MAGIC_ARGS("some", proto_iterate, 1, 2, 1),
    KZ_FN("sort", proto_sort, 1),
    KZ_FN("splice", proto_splice, 2),
    KZ_FN("toLocaleString", proto_to_locale_string, 0),
    KZ_FN("toReversed", proto_to_reversed, 0),
    KZ_FN("toSorted", proto_to_sorted, 1),
    KZ_FN("toSpliced", proto_to_spliced, 2),
    KZ_FN("toString", proto_to_string, 0),
    KZ_FN("unshift", proto_unshift, 1),
    KZ_FN("with", proto_with, 2),
};

int kz_init_array(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    JSValue proto = kz_intr(ctx, KZ_I_ARRAY_PROTO);
    r[0] = kz_make_ctor(ctx, array_ctor, "Array", 1, 0, KZ_I_ARRAY_PROTO, KZ_I_ARRAY);
    if (r[0] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], array_funcs, KZ_COUNTOF(array_funcs))) return -1;
    if (kz_define_functions(ctx, proto, array_proto_funcs, KZ_COUNTOF(array_proto_funcs))) return -1;
    r[1] = kz_new_native_c(ctx, proto_iter, "values", 0, 1);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ARRAY_PROTO_VALUES] = r[1];
    if (kz_define_value_atom(ctx, proto, KZ_ATOM_values, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    if (kz_define_value(ctx, proto, rt->sym[KZ_SYM_iterator], r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* Array.prototype[@@unscopables] */
    r[2] = kz_new_object_proto(ctx, JS_NULL);
    if (r[2] == JS_EXCEPTION) return -1;
    static const char *const unscopable[] = { "at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast",
                                              "findLastIndex", "flat", "flatMap", "includes", "keys", "toReversed",
                                              "toSorted", "toSpliced", "values" };
    for (size_t i = 0; i < KZ_COUNTOF(unscopable); i++) {
        JSValue k = kz_atom(ctx, unscopable[i]);
        if (k == JS_EXCEPTION || kz_create_data_property(ctx, r[2], k, JS_TRUE)) return -1;
    }
    if (kz_define_value(ctx, proto, rt->sym[KZ_SYM_unscopables], r[2], KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Array, r[0], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
