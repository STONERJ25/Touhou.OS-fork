/*
 * conv.c: type conversion and comparison (ECMA-262 7.1, 7.2), the operators that apply them (13.15 and friends), and
 * the iterator protocol helpers (7.4).
 */
#include "jsint.h"

/* ------------------------------------------------------------------ ToPrimitive */

static JSValue ordinary_to_primitive(JSContext *ctx, JSValue o, int hint) {
    static const int order_num[2] = { KZ_ATOM_valueOf, KZ_ATOM_toString };
    static const int order_str[2] = { KZ_ATOM_toString, KZ_ATOM_valueOf };
    const int *order = hint == KZ_HINT_STRING ? order_str : order_num;
    for (int i = 0; i < 2; i++) {
        JSValue m = kz_get(ctx, o, ctx->rt->atom[order[i]], o);
        if (m == JS_EXCEPTION) return m;
        if (kz_is_callable(m)) {
            JSValue r = kz_call(ctx, m, o, 0, NULL);
            if (r == JS_EXCEPTION || !JV_IS_OBJECT(r)) return r;
        }
    }
    return kz_throw_type(ctx, "Cannot convert object to primitive value");
}

JSValue kz_to_primitive(JSContext *ctx, JSValue v, int hint) {
    if (!JV_IS_OBJECT(v)) return v;
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    KZ_ROOT(ctx, ex);
    ex = kz_get(ctx, v, ctx->rt->sym[KZ_SYM_toPrimitive], v);
    if (ex == JS_EXCEPTION) return ex;
    if (!JV_IS_NULLISH(ex)) {
        if (!kz_is_callable(ex)) return kz_throw_type(ctx, "Symbol.toPrimitive is not a function");
        JSValue h = ctx->rt->atom[hint == KZ_HINT_STRING ? KZ_ATOM_string : hint == KZ_HINT_NUMBER ? KZ_ATOM_number : KZ_ATOM_default];
        JSValue args[1] = { h };
        JSValue r = kz_call(ctx, ex, v, 1, args);
        if (r == JS_EXCEPTION) return r;
        if (JV_IS_OBJECT(r)) return kz_throw_type(ctx, "Cannot convert object to primitive value");
        return r;
    }
    return ordinary_to_primitive(ctx, v, hint == KZ_HINT_DEFAULT ? KZ_HINT_NUMBER : hint);
}

/* ------------------------------------------------------------------ ToBoolean, ToNumber, ToString */

bool kz_to_bool(JSValue v) {
    if (JV_IS_INT(v)) return JV_INT(v) != 0;
    if (JV_IS_DOUBLE(v)) { double d = jv_dbl(v); return d == d && d != 0; }
    switch (JV_TAG(v)) {
    case JV_TAG_STRING: return kz_str_len(JV_STR(v)) != 0;
    case JV_TAG_SPECIAL: return v == JS_TRUE;
    case JV_TAG_BIGINT: return !kz_bigint_is_zero(v);
    default: return true;           /* objects and symbols */
    }
}

int kz_to_number(JSContext *ctx, JSValue v, double *out) {
    for (;;) {
        if (JV_IS_INT(v)) { *out = JV_INT(v); return 0; }
        if (JV_IS_DOUBLE(v)) { *out = jv_dbl(v); return 0; }
        switch (JV_TAG(v)) {
        case JV_TAG_STRING: {
            JSString *s = kz_str_flat(ctx, JV_STR(v));
            if (!s) return -1;
            *out = kz_string_to_number(s);
            return 0;
        }
        case JV_TAG_SPECIAL:
            *out = v == JS_TRUE ? 1 : v == JS_FALSE ? 0 : v == JS_NULL ? 0 : __builtin_nan("");
            return 0;
        case JV_TAG_SYMBOL: kz_throw_type(ctx, "Cannot convert a Symbol value to a number"); return -1;
        case JV_TAG_BIGINT: kz_throw_type(ctx, "Cannot convert a BigInt value to a number"); return -1;
        default: {
            KZ_ROOT(ctx, p);
            p = kz_to_primitive(ctx, v, KZ_HINT_NUMBER);
            if (p == JS_EXCEPTION) return -1;
            v = p;
            if (JV_IS_OBJECT(v)) { kz_throw_type(ctx, "Cannot convert object to primitive value"); return -1; }
            /* loop once more on the primitive (it is rooted by being a non-heap value or by p) */
            if (JV_IS_STRING(v)) {
                JSString *s = kz_str_flat(ctx, JV_STR(v));
                if (!s) return -1;
                *out = kz_string_to_number(s);
                return 0;
            }
            continue;
        }
        }
    }
}

JSValue kz_to_number_value(JSContext *ctx, JSValue v) {
    if (JV_IS_NUMBER(v)) return v;
    double d;
    if (kz_to_number(ctx, v, &d)) return JS_EXCEPTION;
    return jv_number(d);
}

JSValue kz_to_numeric(JSContext *ctx, JSValue v) {
    if (JV_IS_NUMBER(v) || JV_IS_BIGINT(v)) return v;
    KZ_ROOT(ctx, p);
    p = kz_to_primitive(ctx, v, KZ_HINT_NUMBER);
    if (p == JS_EXCEPTION) return p;
    if (JV_IS_BIGINT(p)) return p;
    return kz_to_number_value(ctx, p);
}

int kz_to_int32(JSContext *ctx, JSValue v, int32_t *out) {
    if (JV_IS_INT(v)) { *out = JV_INT(v); return 0; }
    double d;
    if (kz_to_number(ctx, v, &d)) return -1;
    *out = kz_double_to_int32(d);
    return 0;
}

int kz_to_uint32(JSContext *ctx, JSValue v, uint32_t *out) {
    int32_t i;
    if (kz_to_int32(ctx, v, &i)) return -1;
    *out = (uint32_t)i;
    return 0;
}

int kz_to_integer_or_inf(JSContext *ctx, JSValue v, double *out) {
    if (JV_IS_INT(v)) { *out = JV_INT(v); return 0; }
    double d;
    if (kz_to_number(ctx, v, &d)) return -1;
    if (d != d) d = 0;
    else d = kz_math_trunc(d);
    *out = d == 0 ? 0 : d;          /* -0 becomes +0 */
    return 0;
}

int kz_to_length(JSContext *ctx, JSValue v, uint64_t *out) {
    if (JV_IS_INT(v)) { *out = JV_INT(v) < 0 ? 0 : (uint64_t)JV_INT(v); return 0; }
    double d;
    if (kz_to_integer_or_inf(ctx, v, &d)) return -1;
    if (d <= 0) *out = 0;
    else if (d >= 9007199254740991.0) *out = 9007199254740991ULL;
    else *out = (uint64_t)d;
    return 0;
}

int kz_to_index(JSContext *ctx, JSValue v, uint64_t *out) {
    if (v == JS_UNDEFINED) { *out = 0; return 0; }
    double d;
    if (kz_to_integer_or_inf(ctx, v, &d)) return -1;
    if (d < 0 || d > 9007199254740991.0) { kz_throw_range(ctx, "Invalid index"); return -1; }
    *out = (uint64_t)d;
    return 0;
}

int kz_to_relative_index(JSContext *ctx, JSValue v, uint64_t len, uint64_t def, uint64_t *out) {
    if (v == JS_UNDEFINED) { *out = def; return 0; }
    double d;
    if (kz_to_integer_or_inf(ctx, v, &d)) return -1;
    if (d < 0) { d += (double)len; *out = d < 0 ? 0 : (uint64_t)d; }
    else *out = d > (double)len ? len : (uint64_t)d;
    return 0;
}

JSValue kz_to_string(JSContext *ctx, JSValue v) {
    if (JV_IS_STRING(v)) return v;
    if (JV_IS_INT(v)) return kz_int_to_string(ctx, JV_INT(v));
    if (JV_IS_DOUBLE(v)) return kz_num_to_str(ctx, jv_dbl(v));
    JSRuntime *rt = ctx->rt;
    switch (JV_TAG(v)) {
    case JV_TAG_SPECIAL:
        if (v == JS_UNDEFINED) return rt->atom[KZ_ATOM_undefined];
        if (v == JS_NULL) return rt->atom[KZ_ATOM_null];
        if (v == JS_TRUE) return rt->atom[KZ_ATOM_true];
        if (v == JS_FALSE) return rt->atom[KZ_ATOM_false];
        return kz_throw_type(ctx, "internal value");
    case JV_TAG_SYMBOL: return kz_throw_type(ctx, "Cannot convert a Symbol value to a string");
    case JV_TAG_BIGINT: return kz_bigint_to_string(ctx, v, 10);
    case JV_TAG_OBJECT: {
        KZ_ROOT(ctx, p);
        p = kz_to_primitive(ctx, v, KZ_HINT_STRING);
        if (p == JS_EXCEPTION) return p;
        return kz_to_string(ctx, p);
    }
    default: return kz_throw_type(ctx, "internal value");
    }
}

/* A short description for error messages; never runs JavaScript, never throws (OOM gives an empty string). */
JSValue kz_to_string_noexc(JSContext *ctx, JSValue v) {
    JSValue r;
    if (JV_IS_OBJECT(v)) {
        JSObject *o = JV_OBJ(v);
        if (o->h.flags & OBJ_CALLABLE) r = kz_str_from_cstr(ctx, "function");
        else if (kz_class(o) == KZ_CLASS_ARRAY) r = kz_str_from_cstr(ctx, "[object Array]");
        else r = kz_str_from_cstr(ctx, "[object Object]");
    } else if (JV_IS_SYMBOL(v)) r = kz_symbol_descriptive_string(ctx, v);
    else r = kz_to_string(ctx, v);
    if (r == JS_EXCEPTION) {
        kz_take_exception(ctx);
        return ctx->rt->atom[KZ_ATOM_empty];
    }
    return r;
}

JSValue kz_symbol_descriptive_string(JSContext *ctx, JSValue sym) {
    JSValue d = JV_SYM(sym)->description;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_puts(&sb, "Symbol(");
    if (JV_IS_STRING(d)) kz_sb_put_value(&sb, d);
    kz_sb_putc(&sb, ')');
    return kz_sb_finish(&sb);
}

/* ------------------------------------------------------------------ ToObject, ToPropertyKey */

static JSValue make_wrapper(JSContext *ctx, KzClassId cls, KzIntrinsic proto, JSValue prim) {
    KZ_ROOT(ctx, o);
    o = kz_new_object_class(ctx, cls, kz_intr(ctx, proto));
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.prim = prim;
    if (cls == KZ_CLASS_STRING &&
        kz_obj_add_prop(ctx, JV_OBJ(o), ctx->rt->atom[KZ_ATOM_length], 0, JV_MKINT((int32_t)kz_str_len(JV_STR(prim)))))
        return JS_EXCEPTION;
    return o;
}

JSValue kz_to_object(JSContext *ctx, JSValue v) {
    if (JV_IS_OBJECT(v)) return v;
    if (JV_IS_NUMBER(v)) return make_wrapper(ctx, KZ_CLASS_NUMBER, KZ_I_NUMBER_PROTO, v);
    switch (JV_TAG(v)) {
    case JV_TAG_STRING: return make_wrapper(ctx, KZ_CLASS_STRING, KZ_I_STRING_PROTO, v);
    case JV_TAG_SYMBOL: return make_wrapper(ctx, KZ_CLASS_SYMBOL, KZ_I_SYMBOL_PROTO, v);
    case JV_TAG_BIGINT: return make_wrapper(ctx, KZ_CLASS_BIGINT, KZ_I_BIGINT_PROTO, v);
    case JV_TAG_SPECIAL:
        if (JV_IS_BOOL(v)) return make_wrapper(ctx, KZ_CLASS_BOOLEAN, KZ_I_BOOLEAN_PROTO, v);
        return kz_throw_type(ctx, "Cannot convert undefined or null to object");
    default:
        return kz_throw_type(ctx, "Cannot convert to object");
    }
}

JSValue kz_to_property_key(JSContext *ctx, JSValue v) {
    if (JV_IS_OBJECT(v)) {
        KZ_ROOT(ctx, p);
        p = kz_to_primitive(ctx, v, KZ_HINT_STRING);
        if (p == JS_EXCEPTION) return p;
        return kz_key_from_value(ctx, p);
    }
    return kz_key_from_value(ctx, v);
}

/* ------------------------------------------------------------------ equality */

static int str_equal(JSContext *ctx, JSValue a, JSValue b) {
    JSString *sa = JV_STR(a), *sb = JV_STR(b);
    if (sa == sb) return 1;
    if (kz_str_len(sa) != kz_str_len(sb)) return 0;
    if ((sa->h.flags & STR_ATOM) && (sb->h.flags & STR_ATOM)) return 0;
    if ((sa->h.flags & STR_ROPE) && !kz_str_flat(ctx, sa)) return -1;
    if ((sb->h.flags & STR_ROPE) && !kz_str_flat(ctx, sb)) return -1;
    return kz_str_eq(sa, sb);
}

int kz_strict_eq(JSContext *ctx, JSValue a, JSValue b) {
    if (JV_IS_NUMBER(a)) {
        if (!JV_IS_NUMBER(b)) return 0;
        if (JV_IS_INT(a) && JV_IS_INT(b)) return a == b;
        return jv_num(a) == jv_num(b);
    }
    if (a == b) return 1;
    if (JV_IS_STRING(a) && JV_IS_STRING(b)) return str_equal(ctx, a, b);
    if (JV_IS_BIGINT(a) && JV_IS_BIGINT(b)) return kz_bigint_eq(a, b);
    return 0;
}

int kz_same_value(JSContext *ctx, JSValue a, JSValue b) {
    if (JV_IS_NUMBER(a) && JV_IS_NUMBER(b)) {
        if (JV_IS_INT(a) && JV_IS_INT(b)) return a == b;
        double x = jv_num(a), y = jv_num(b);
        if (x != x) return y != y;
        if (x == 0 && y == 0) return __builtin_signbit(x) == __builtin_signbit(y);
        return x == y;
    }
    return kz_strict_eq(ctx, a, b);
}

int kz_same_value_zero(JSContext *ctx, JSValue a, JSValue b) {
    if (JV_IS_NUMBER(a) && JV_IS_NUMBER(b)) {
        double x = jv_num(a), y = jv_num(b);
        if (x != x) return y != y;
        return x == y;
    }
    return kz_strict_eq(ctx, a, b);
}

/* IsLooselyEqual */
int kz_loose_eq(JSContext *ctx, JSValue a, JSValue b) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = a;
    r[1] = b;
    for (int guard = 0; guard < 8; guard++) {
        a = r[0];
        b = r[1];
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(b)) return jv_num(a) == jv_num(b);
        if (JV_IS_NULLISH(a) && JV_IS_NULLISH(b)) return 1;
        if (JV_TAG(a) == JV_TAG(b) && !JV_IS_NUMBER(a)) return kz_strict_eq(ctx, a, b);
        if (JV_IS_NULLISH(a) || JV_IS_NULLISH(b)) {
            /* (an object is never == null; no document.all here) */
            return 0;
        }
        if (JV_IS_NUMBER(a) && JV_IS_STRING(b)) {
            double d;
            if (kz_to_number(ctx, b, &d)) return -1;
            return jv_num(a) == d;
        }
        if (JV_IS_STRING(a) && JV_IS_NUMBER(b)) {
            double d;
            if (kz_to_number(ctx, a, &d)) return -1;
            return d == jv_num(b);
        }
        if (JV_IS_BIGINT(a) && JV_IS_STRING(b)) {
            JSString *s = kz_str_flat(ctx, JV_STR(b));
            if (!s) return -1;
            JSValue n = kz_bigint_from_string(ctx, s);
            if (n == JS_EXCEPTION) return -1;
            if (n == JS_UNDEFINED) return 0;
            return kz_bigint_eq(a, n);
        }
        if (JV_IS_STRING(a) && JV_IS_BIGINT(b)) { r[0] = b; r[1] = a; continue; }
        if (JV_IS_BOOL(a)) { r[0] = JV_MKINT(a == JS_TRUE); continue; }
        if (JV_IS_BOOL(b)) { r[1] = JV_MKINT(b == JS_TRUE); continue; }
        if ((JV_IS_NUMBER(a) || JV_IS_STRING(a) || JV_IS_BIGINT(a) || JV_IS_SYMBOL(a)) && JV_IS_OBJECT(b)) {
            JSValue p = kz_to_primitive(ctx, b, KZ_HINT_DEFAULT);
            if (p == JS_EXCEPTION) return -1;
            r[1] = p;
            continue;
        }
        if (JV_IS_OBJECT(a) && (JV_IS_NUMBER(b) || JV_IS_STRING(b) || JV_IS_BIGINT(b) || JV_IS_SYMBOL(b))) {
            JSValue p = kz_to_primitive(ctx, a, KZ_HINT_DEFAULT);
            if (p == JS_EXCEPTION) return -1;
            r[0] = p;
            continue;
        }
        if (JV_IS_BIGINT(a) && JV_IS_NUMBER(b)) {
            double d = jv_num(b);
            if (d != d || d == __builtin_inf() || d == -__builtin_inf()) return 0;
            return kz_bigint_cmp_double(a, d) == 0;
        }
        if (JV_IS_NUMBER(a) && JV_IS_BIGINT(b)) { r[0] = b; r[1] = a; continue; }
        return 0;
    }
    return 0;
}

/* IsLessThan(a, b): 1 true, 0 false, 2 undefined (a NaN was involved), -1 exception.  left_first: which side is
   converted first. */
int kz_less_than(JSContext *ctx, JSValue a, JSValue b, bool left_first) {
    KZ_ROOTS(ctx, r, 2);
    if (left_first) {
        r[0] = kz_to_primitive(ctx, a, KZ_HINT_NUMBER);
        if (r[0] == JS_EXCEPTION) return -1;
        r[1] = kz_to_primitive(ctx, b, KZ_HINT_NUMBER);
        if (r[1] == JS_EXCEPTION) return -1;
    } else {
        r[1] = kz_to_primitive(ctx, b, KZ_HINT_NUMBER);
        if (r[1] == JS_EXCEPTION) return -1;
        r[0] = kz_to_primitive(ctx, a, KZ_HINT_NUMBER);
        if (r[0] == JS_EXCEPTION) return -1;
    }
    JSValue px = r[0], py = r[1];
    if (JV_IS_STRING(px) && JV_IS_STRING(py)) {
        JSString *x = kz_str_flat(ctx, JV_STR(px));
        if (!x) return -1;
        JSString *y = kz_str_flat(ctx, JV_STR(py));
        if (!y) return -1;
        return kz_str_cmp(x, y) < 0;
    }
    if (JV_IS_BIGINT(px) && JV_IS_STRING(py)) {
        JSString *s = kz_str_flat(ctx, JV_STR(py));
        if (!s) return -1;
        JSValue n = kz_bigint_from_string(ctx, s);
        if (n == JS_EXCEPTION) return -1;
        if (n == JS_UNDEFINED) return 2;
        return kz_bigint_cmp(px, n) < 0;
    }
    if (JV_IS_STRING(px) && JV_IS_BIGINT(py)) {
        JSString *s = kz_str_flat(ctx, JV_STR(px));
        if (!s) return -1;
        JSValue n = kz_bigint_from_string(ctx, s);
        if (n == JS_EXCEPTION) return -1;
        if (n == JS_UNDEFINED) return 2;
        return kz_bigint_cmp(n, py) < 0;
    }
    JSValue nx = kz_to_numeric(ctx, px);
    if (nx == JS_EXCEPTION) return -1;
    r[0] = nx;
    JSValue ny = kz_to_numeric(ctx, py);
    if (ny == JS_EXCEPTION) return -1;
    nx = r[0];
    if (JV_IS_BIGINT(nx) && JV_IS_BIGINT(ny)) return kz_bigint_cmp(nx, ny) < 0;
    if (JV_IS_BIGINT(nx)) {
        double d = jv_num(ny);
        if (d != d) return 2;
        int c = kz_bigint_cmp_double(nx, d);
        return c == -1;
    }
    if (JV_IS_BIGINT(ny)) {
        double d = jv_num(nx);
        if (d != d) return 2;
        int c = kz_bigint_cmp_double(ny, d);
        return c == 1;
    }
    double x = jv_num(nx), y = jv_num(ny);
    if (x != x || y != y) return 2;
    return x < y;
}

JSValue kz_typeof(JSContext *ctx, JSValue v) {
    JSRuntime *rt = ctx->rt;
    if (JV_IS_NUMBER(v)) return rt->atom[KZ_ATOM_number];
    switch (JV_TAG(v)) {
    case JV_TAG_STRING: return rt->atom[KZ_ATOM_string];
    case JV_TAG_SYMBOL: return rt->atom[KZ_ATOM_symbol];
    case JV_TAG_BIGINT: return rt->atom[KZ_ATOM_bigint];
    case JV_TAG_OBJECT: return (JV_OBJ(v)->h.flags & OBJ_CALLABLE) ? rt->atom[KZ_ATOM_function] : rt->atom[KZ_ATOM_object];
    default:
        if (v == JS_UNDEFINED || v == JS_UNINIT) return rt->atom[KZ_ATOM_undefined];
        if (v == JS_NULL) return rt->atom[KZ_ATOM_object];
        return rt->atom[KZ_ATOM_boolean];
    }
}

/* ------------------------------------------------------------------ instanceof */

int kz_ordinary_has_instance(JSContext *ctx, JSValue c, JSValue o) {
    if (!kz_is_callable(c)) return 0;
    if (kz_class(JV_OBJ(c)) == KZ_CLASS_BOUND) return kz_instance_of(ctx, o, JV_OBJ(c)->u.bound.target);
    if (!JV_IS_OBJECT(o)) return 0;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_get(ctx, c, ctx->rt->atom[KZ_ATOM_prototype], c);
    if (r[0] == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(r[0])) { kz_throw_type(ctx, "Function has non-object prototype in instanceof check"); return -1; }
    r[1] = o;
    for (int depth = 0; depth < 1000000; depth++) {
        JSValue p;
        if (kz_get_proto(ctx, JV_OBJ(r[1]), &p)) return -1;
        if (!JV_IS_OBJECT(p)) return 0;
        if (p == r[0]) return 1;
        r[1] = p;
    }
    kz_throw_range(ctx, "prototype chain too long");
    return -1;
}

int kz_instance_of(JSContext *ctx, JSValue v, JSValue target) {
    if (!JV_IS_OBJECT(target)) { kz_throw_type(ctx, "Right-hand side of 'instanceof' is not an object"); return -1; }
    KZ_ROOT(ctx, h);
    h = kz_get_method(ctx, target, ctx->rt->sym[KZ_SYM_hasInstance]);
    if (h == JS_EXCEPTION) return -1;
    if (h != JS_UNDEFINED) {
        /* the built-in Function.prototype[@@hasInstance] is OrdinaryHasInstance: skip the call */
        JSValue args[1] = { v };
        JSValue r = kz_call(ctx, h, target, 1, args);
        if (r == JS_EXCEPTION) return -1;
        return kz_to_bool(r);
    }
    if (!kz_is_callable(target)) { kz_throw_type(ctx, "Right-hand side of 'instanceof' is not callable"); return -1; }
    return kz_ordinary_has_instance(ctx, target, v);
}

int kz_is_array(JSContext *ctx, JSValue v) {
    for (int depth = 0; depth < 100000; depth++) {
        if (!JV_IS_OBJECT(v)) return 0;
        JSObject *o = JV_OBJ(v);
        if (kz_class(o) == KZ_CLASS_ARRAY) return 1;
        if (kz_class(o) != KZ_CLASS_PROXY) return 0;
        if (o->u.proxy.handler == JS_NULL) { kz_throw_type(ctx, "Cannot perform 'IsArray' on a proxy that has been revoked"); return -1; }
        v = o->u.proxy.target;
    }
    kz_throw_range(ctx, "proxy chain too long");
    return -1;
}

int kz_length_of(JSContext *ctx, JSValue obj, uint64_t *out) {
    if (JV_IS_OBJECT(obj) && kz_class(JV_OBJ(obj)) == KZ_CLASS_ARRAY) { *out = JV_OBJ(obj)->u.array.length; return 0; }
    JSValue l = kz_get(ctx, obj, ctx->rt->atom[KZ_ATOM_length], obj);
    if (l == JS_EXCEPTION) return -1;
    return kz_to_length(ctx, l, out);
}

JSValue kz_get_method(JSContext *ctx, JSValue v, JSValue key) {
    JSValue f = kz_get(ctx, v, key, v);
    if (f == JS_EXCEPTION || JV_IS_NULLISH(f)) return f == JS_EXCEPTION ? f : JS_UNDEFINED;
    if (!kz_is_callable(f)) {
        KZ_ROOT(ctx, n);
        n = kz_key_debug_name(ctx, key);
        char buf[96] = "";
        if (n != JS_EXCEPTION && kz_str_flatten(ctx, n) != JS_EXCEPTION) kz_str_to_utf8_buf(JV_STR(n), buf, sizeof buf);
        return kz_throw_type(ctx, "%s is not a function", buf);
    }
    return f;
}

JSValue kz_species_constructor(JSContext *ctx, JSValue o, JSValue def) {
    KZ_ROOT(ctx, c);
    c = kz_get(ctx, o, ctx->rt->atom[KZ_ATOM_constructor], o);
    if (c == JS_EXCEPTION) return c;
    if (c == JS_UNDEFINED) return def;
    if (!JV_IS_OBJECT(c)) return kz_throw_type(ctx, "object.constructor is not an object");
    JSValue s = kz_get(ctx, c, ctx->rt->sym[KZ_SYM_species], c);
    if (s == JS_EXCEPTION) return s;
    if (JV_IS_NULLISH(s)) return def;
    if (kz_is_constructor(s)) return s;
    return kz_throw_type(ctx, "object.constructor[Symbol.species] is not a constructor");
}

/* ------------------------------------------------------------------ arithmetic on arbitrary values */

static JSValue number_op(int op, double x, double y) {
    switch (op) {
    case KZ_BIN_ADD: return jv_from_raw_double(x + y);
    case KZ_BIN_SUB: return jv_from_raw_double(x - y);
    case KZ_BIN_MUL: return jv_from_raw_double(x * y);
    case KZ_BIN_DIV: return jv_from_raw_double(x / y);
    case KZ_BIN_MOD: return jv_from_raw_double(kz_math_fmod(x, y));
    case KZ_BIN_POW: return jv_from_raw_double(kz_math_pow(x, y));
    case KZ_BIN_AND: return JV_MKINT(kz_double_to_int32(x) & kz_double_to_int32(y));
    case KZ_BIN_OR: return JV_MKINT(kz_double_to_int32(x) | kz_double_to_int32(y));
    case KZ_BIN_XOR: return JV_MKINT(kz_double_to_int32(x) ^ kz_double_to_int32(y));
    case KZ_BIN_SHL: return JV_MKINT((int32_t)((uint32_t)kz_double_to_int32(x) << (kz_double_to_int32(y) & 31)));
    case KZ_BIN_SAR: return JV_MKINT(kz_double_to_int32(x) >> (kz_double_to_int32(y) & 31));
    case KZ_BIN_SHR: return jv_uint32((uint32_t)kz_double_to_int32(x) >> (kz_double_to_int32(y) & 31));
    }
    return jv_from_dbl(0);
}

JSValue kz_binary_arith(JSContext *ctx, int op, JSValue a, JSValue b) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_numeric(ctx, a);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    r[1] = kz_to_numeric(ctx, b);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    bool ba = JV_IS_BIGINT(r[0]), bb = JV_IS_BIGINT(r[1]);
    if (ba || bb) {
        if (!(ba && bb)) return kz_throw_type(ctx, "Cannot mix BigInt and other types, use explicit conversions");
        return kz_bigint_binary(ctx, op, r[0], r[1]);
    }
    return number_op(op, jv_num(r[0]), jv_num(r[1]));
}

JSValue kz_add(JSContext *ctx, JSValue a, JSValue b) {
    if (JV_IS_NUMBER(a) && JV_IS_NUMBER(b)) return jv_from_raw_double(jv_num(a) + jv_num(b));
    if (JV_IS_STRING(a) && JV_IS_STRING(b)) return kz_str_concat(ctx, a, b);
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_primitive(ctx, a, KZ_HINT_DEFAULT);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    r[1] = kz_to_primitive(ctx, b, KZ_HINT_DEFAULT);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (JV_IS_STRING(r[0]) || JV_IS_STRING(r[1])) {
        r[0] = kz_to_string(ctx, r[0]);
        if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
        r[1] = kz_to_string(ctx, r[1]);
        if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
        return kz_str_concat(ctx, r[0], r[1]);
    }
    return kz_binary_arith(ctx, KZ_BIN_ADD, r[0], r[1]);
}

/* ------------------------------------------------------------------ lists and invocation */

int kz_create_list_from_array_like(JSContext *ctx, JSValue obj, KzVec *out) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "CreateListFromArrayLike called on non-object"); return -1; }
    uint64_t len;
    if (kz_length_of(ctx, obj, &len)) return -1;
    if (len > 10000000) { kz_throw_range(ctx, "Too many arguments in function call"); return -1; }
    JSObject *o = JV_OBJ(obj);
    if (kz_class(o) == KZ_CLASS_ARRAY && o->elem_len == len && !(o->h.flags & OBJ_EXOTIC)) {
        bool holes = false;
        for (uint32_t i = 0; i < len; i++) if (o->elems[i] == JS_HOLE) { holes = true; break; }
        if (!holes) {
            for (uint32_t i = 0; i < len; i++) if (kz_vec_push(ctx, out, JV_OBJ(obj)->elems[i])) return -1;
            return 0;
        }
    }
    for (uint64_t i = 0; i < len; i++) {
        JSValue v = kz_get_index(ctx, obj, (uint32_t)i);
        if (v == JS_EXCEPTION || kz_vec_push(ctx, out, v)) return -1;
    }
    return 0;
}

JSValue kz_invoke(JSContext *ctx, JSValue obj, JSValue key, int argc, JSValue *argv) {
    KZ_ROOT(ctx, f);
    f = kz_get(ctx, obj, key, obj);
    if (f == JS_EXCEPTION) return f;
    return kz_call(ctx, f, obj, argc, argv);
}

/* ------------------------------------------------------------------ iterators */

int kz_get_iterator_from_method(JSContext *ctx, JSValue obj, JSValue method, KzIter *it) {
    JSValue iter = kz_call(ctx, method, obj, 0, NULL);
    if (iter == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(iter)) { kz_throw_type(ctx, "Result of the Symbol.iterator method is not an object"); return -1; }
    it->iter = iter;
    it->next = kz_get(ctx, iter, ctx->rt->atom[KZ_ATOM_next], iter);
    if (it->next == JS_EXCEPTION) return -1;
    it->done = false;
    return 0;
}

JSValue kz_async_from_sync_iterator(JSContext *ctx, KzIter *sync);

int kz_get_iterator(JSContext *ctx, JSValue obj, KzIter *it, bool async) {
    KZ_ROOT(ctx, m);
    if (async) {
        m = kz_get_method(ctx, obj, ctx->rt->sym[KZ_SYM_asyncIterator]);
        if (m == JS_EXCEPTION) return -1;
        if (m == JS_UNDEFINED) {
            KZ_ROOT_ITER(ctx, sync);
            m = kz_get_method(ctx, obj, ctx->rt->sym[KZ_SYM_iterator]);
            if (m == JS_EXCEPTION) return -1;
            if (m == JS_UNDEFINED) { kz_throw_type(ctx, "object is not async iterable"); return -1; }
            if (kz_get_iterator_from_method(ctx, obj, m, &sync)) return -1;
            JSValue a = kz_async_from_sync_iterator(ctx, &sync);
            if (a == JS_EXCEPTION) return -1;
            it->iter = a;
            it->next = kz_get(ctx, a, ctx->rt->atom[KZ_ATOM_next], a);
            if (it->next == JS_EXCEPTION) return -1;
            it->done = false;
            return 0;
        }
        return kz_get_iterator_from_method(ctx, obj, m, it);
    }
    m = kz_get_method(ctx, obj, ctx->rt->sym[KZ_SYM_iterator]);
    if (m == JS_EXCEPTION) return -1;
    if (m == JS_UNDEFINED) {
        char buf[64];
        kz_format_value_for_msg(ctx, obj, buf, sizeof buf);
        kz_throw_type(ctx, "%s is not iterable", buf);
        return -1;
    }
    return kz_get_iterator_from_method(ctx, obj, m, it);
}

int kz_iter_step(JSContext *ctx, KzIter *it, JSValue *value) {
    KZ_ROOT(ctx, res);
    res = kz_call(ctx, it->next, it->iter, 0, NULL);
    if (res == JS_EXCEPTION) { it->done = true; return -1; }
    if (!JV_IS_OBJECT(res)) { it->done = true; kz_throw_type(ctx, "Iterator result is not an object"); return -1; }
    JSValue done = kz_get(ctx, res, ctx->rt->atom[KZ_ATOM_done], res);
    if (done == JS_EXCEPTION) { it->done = true; return -1; }
    if (kz_to_bool(done)) { it->done = true; return 0; }
    JSValue v = kz_get(ctx, res, ctx->rt->atom[KZ_ATOM_value], res);
    if (v == JS_EXCEPTION) { it->done = true; return -1; }
    *value = v;
    return 1;
}

JSValue kz_iter_close(JSContext *ctx, KzIter *it, JSValue completion) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = completion;
    r[1] = kz_get_method(ctx, it->iter, ctx->rt->atom[KZ_ATOM_return]);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (r[1] == JS_UNDEFINED) return r[0];
    JSValue inner = kz_call(ctx, r[1], it->iter, 0, NULL);
    if (inner == JS_EXCEPTION) return JS_EXCEPTION;
    if (!JV_IS_OBJECT(inner)) return kz_throw_type(ctx, "Iterator result is not an object");
    return r[0];
}

int kz_iter_close_on_throw(JSContext *ctx, KzIter *it) {
    JSRuntime *rt = ctx->rt;
    if (rt->exc_uncatchable) return -1;
    KZ_ROOTS(ctx, r, 2);
    r[0] = rt->exception;
    rt->exception = JS_UNINIT;
    r[1] = kz_get_method(ctx, it->iter, rt->atom[KZ_ATOM_return]);
    if (r[1] != JS_EXCEPTION && r[1] != JS_UNDEFINED) kz_call(ctx, r[1], it->iter, 0, NULL);
    if (rt->exc_uncatchable) return -1;      /* an interrupt wins */
    rt->exception = r[0];
    return -1;
}

/* AsyncIteratorClose (7.4.13) for a normal, break, continue or return completion, up to the Await: GetMethod(iterator,
   "return") and its call.  JS_EMPTY: there is no return method, so nothing is awaited.  Otherwise the result of the call,
   which the generated code awaits and checks to be an object (iter_close_check).  An error here propagates. */
JSValue kz_iter_close_async_begin(JSContext *ctx, JSValue iter) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = iter;
    r[1] = kz_get_method(ctx, r[0], ctx->rt->atom[KZ_ATOM_return]);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (r[1] == JS_UNDEFINED) return JS_EMPTY;
    return kz_call(ctx, r[1], r[0], 0, NULL);
}

/* The same for a throw completion (the exception is not pending: the handler holds it on the stack): the errors of
   GetMethod and of the call are ignored, so is the Await's, and the original exception is thrown again afterwards.
   JS_EXCEPTION only when an interrupt is pending, which nothing may swallow. */
JSValue kz_iter_close_async_on_throw(JSContext *ctx, JSValue iter) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    r[0] = iter;
    r[1] = kz_get_method(ctx, r[0], rt->atom[KZ_ATOM_return]);
    JSValue res = JS_EMPTY;
    if (r[1] != JS_EXCEPTION && r[1] != JS_UNDEFINED) res = kz_call(ctx, r[1], r[0], 0, NULL);
    if (r[1] == JS_EXCEPTION || res == JS_EXCEPTION) {
        if (rt->exc_uncatchable) return JS_EXCEPTION;
        kz_take_exception(ctx);
        return JS_EMPTY;
    }
    return res;
}

JSValue kz_create_iter_result(JSContext *ctx, JSValue value, bool done) {
    KZ_ROOT(ctx, v);
    v = value;
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_OBJECT, ctx->shape_iter_result, 0);
    if (!o) return JS_EXCEPTION;
    o->slots[0] = v;
    o->slots[1] = JV_MKBOOL(done);
    return JV_MKOBJ(o);
}

int kz_iterate_to_vec(JSContext *ctx, JSValue iterable, KzVec *out) {
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, iterable, &it, false)) return -1;
    for (;;) {
        JSValue v;
        int r = kz_iter_step(ctx, &it, &v);
        if (r < 0) return -1;
        if (!r) return 0;
        if (kz_vec_push(ctx, out, v)) return kz_iter_close_on_throw(ctx, &it);
    }
}
