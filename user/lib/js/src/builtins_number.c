/*
 * builtins_number.c: Number and Number.prototype (ECMA-262 21.1), Boolean (20.3) and the Math object (21.3).
 */
#include "jsint.h"

/* ================================================================== Number */

static JSValue number_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    JSRuntime *rt = ctx->rt;
    KZ_ROOT(ctx, n);
    n = JV_MKINT(0);
    if (argc > 0) {
        n = kz_to_numeric(ctx, argv[0]);
        if (n == JS_EXCEPTION) return n;
        if (JV_IS_BIGINT(n)) n = jv_number(kz_bigint_to_double(n));
    }
    if (rt->new_target == JS_UNDEFINED) return n;
    JSValue o = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_NUMBER_PROTO, KZ_CLASS_NUMBER);
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.prim = n;
    return o;
}

static int this_number(JSContext *ctx, JSValue v, double *out, const char *method) {
    if (JV_IS_NUMBER(v)) { *out = jv_num(v); return 0; }
    if (kz_is_class(v, KZ_CLASS_NUMBER)) { *out = jv_num(JV_OBJ(v)->u.prim); return 0; }
    kz_throw_type(ctx, "Number.prototype.%s requires that 'this' be a Number", method);
    return -1;
}

static bool is_finite(double d) { return d == d && d != KZ_INFINITY && d != -KZ_INFINITY; }

/* isFinite (0), isInteger (1), isNaN (2), isSafeInteger (3) */
static JSValue number_is(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)ctx; (void)this_val; (void)argc;
    JSValue v = argv[0];
    if (!JV_IS_NUMBER(v)) return JS_FALSE;
    double d = jv_num(v);
    switch (magic) {
    case 0: return JV_MKBOOL(is_finite(d));
    case 1: return JV_MKBOOL(is_finite(d) && kz_math_trunc(d) == d);
    case 2: return JV_MKBOOL(d != d);
    default: return JV_MKBOOL(is_finite(d) && kz_math_trunc(d) == d && d <= 9007199254740991.0 && d >= -9007199254740991.0);
    }
}

static JSValue proto_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double x;
    if (this_number(ctx, this_val, &x, "toString")) return JS_EXCEPTION;
    double radix = 10;
    if (argv[0] != JS_UNDEFINED && kz_to_integer_or_inf(ctx, argv[0], &radix)) return JS_EXCEPTION;
    if (radix < 2 || radix > 36) return kz_throw_range(ctx, "toString() radix must be between 2 and 36");
    if (radix == 10) return kz_num_to_str(ctx, x);
    return kz_num_to_str_radix(ctx, x, (int)radix);
}

/* the en-US style of Intl.NumberFormat's defaults: grouping and at most three fraction digits */
static JSValue proto_to_locale_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double x;
    if (this_number(ctx, this_val, &x, "toLocaleString")) return JS_EXCEPTION;
    if (x != x) return ctx->rt->atom[KZ_ATOM_NaN];
    if (!is_finite(x)) return kz_str_from_utf8(ctx, x < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E", x < 0 ? 4 : 3);
    KZ_ROOT(ctx, s);
    if (x >= 1e21 || x <= -1e21) s = kz_num_to_str(ctx, x);
    else s = kz_num_to_fixed(ctx, x, 3);
    if (s == JS_EXCEPTION) return s;
    s = kz_str_flatten(ctx, s);
    if (s == JS_EXCEPTION) return s;
    char buf[400], out[600];
    kz_str_to_utf8_buf(JV_STR(s), buf, sizeof buf);
    if (x >= 1e21 || x <= -1e21) return s;
    /* trim trailing fraction zeros */
    size_t n = strlen(buf);
    char *dot = strchr(buf, '.');
    if (dot) {
        while (n > 0 && buf[n - 1] == '0') buf[--n] = 0;
        if (n > 0 && buf[n - 1] == '.') buf[--n] = 0;
    }
    size_t o = 0, i = 0;
    if (buf[0] == '-') out[o++] = buf[i++];
    size_t int_end = i;
    while (buf[int_end] && buf[int_end] != '.') int_end++;
    size_t digits = int_end - i;
    for (; i < int_end; i++) {
        out[o++] = buf[i];
        size_t left = int_end - i - 1;
        if (left && left % 3 == 0 && digits > 3) out[o++] = ',';
    }
    while (buf[i]) out[o++] = buf[i++];
    return kz_str_from_latin1(ctx, (const uint8_t *)out, (uint32_t)o);
}

static JSValue proto_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double x;
    if (this_number(ctx, this_val, &x, "valueOf")) return JS_EXCEPTION;
    return jv_number(x);
}

static JSValue proto_to_fixed(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double x, f;
    if (this_number(ctx, this_val, &x, "toFixed")) return JS_EXCEPTION;
    if (kz_to_integer_or_inf(ctx, argv[0], &f)) return JS_EXCEPTION;
    if (!is_finite(f) || f < 0 || f > 100) return kz_throw_range(ctx, "toFixed() digits argument must be between 0 and 100");
    if (!is_finite(x)) return kz_num_to_str(ctx, x);
    return kz_num_to_fixed(ctx, x, (int)f);
}

static JSValue proto_to_exponential(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double x, f;
    if (this_number(ctx, this_val, &x, "toExponential")) return JS_EXCEPTION;
    if (kz_to_integer_or_inf(ctx, argv[0], &f)) return JS_EXCEPTION;
    if (!is_finite(x)) return kz_num_to_str(ctx, x);
    if (f < 0 || f > 100) return kz_throw_range(ctx, "toExponential() argument must be between 0 and 100");
    return kz_num_to_exponential(ctx, x, argv[0] == JS_UNDEFINED ? -1 : (int)f);
}

static JSValue proto_to_precision(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double x, p;
    if (this_number(ctx, this_val, &x, "toPrecision")) return JS_EXCEPTION;
    if (argv[0] == JS_UNDEFINED) return kz_num_to_str(ctx, x);
    if (kz_to_integer_or_inf(ctx, argv[0], &p)) return JS_EXCEPTION;
    if (!is_finite(x)) return kz_num_to_str(ctx, x);
    if (p < 1 || p > 100) return kz_throw_range(ctx, "toPrecision() argument must be between 1 and 100");
    return kz_num_to_precision(ctx, x, (int)p);
}

static const KzFuncDef number_funcs[] = {
    KZ_FN_MAGIC("isFinite", number_is, 1, 0),
    KZ_FN_MAGIC("isInteger", number_is, 1, 1),
    KZ_FN_MAGIC("isNaN", number_is, 1, 2),
    KZ_FN_MAGIC("isSafeInteger", number_is, 1, 3),
};

static const KzFuncDef number_proto_funcs[] = {
    KZ_FN("toExponential", proto_to_exponential, 1),
    KZ_FN("toFixed", proto_to_fixed, 1),
    KZ_FN("toLocaleString", proto_to_locale_string, 0),
    KZ_FN("toPrecision", proto_to_precision, 1),
    KZ_FN("toString", proto_to_string, 1),
    KZ_FN("valueOf", proto_value_of, 0),
};

/* ================================================================== Boolean */

static JSValue boolean_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    JSValue b = JV_MKBOOL(kz_to_bool(argv[0]));
    if (rt->new_target == JS_UNDEFINED) return b;
    JSValue o = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_BOOLEAN_PROTO, KZ_CLASS_BOOLEAN);
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.prim = b;
    return o;
}

static int this_boolean(JSContext *ctx, JSValue v, JSValue *out) {
    if (JV_IS_BOOL(v)) { *out = v; return 0; }
    if (kz_is_class(v, KZ_CLASS_BOOLEAN)) { *out = JV_OBJ(v)->u.prim; return 0; }
    kz_throw_type(ctx, "Boolean.prototype method requires that 'this' be a Boolean");
    return -1;
}

static JSValue boolean_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue b;
    if (this_boolean(ctx, this_val, &b)) return JS_EXCEPTION;
    return ctx->rt->atom[b == JS_TRUE ? KZ_ATOM_true : KZ_ATOM_false];
}

static JSValue boolean_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue b;
    if (this_boolean(ctx, this_val, &b)) return JS_EXCEPTION;
    return b;
}

static const KzFuncDef boolean_proto_funcs[] = {
    KZ_FN("toString", boolean_to_string, 0),
    KZ_FN("valueOf", boolean_value_of, 0),
};

/* ================================================================== Math */

typedef double (*MathFn1)(double);

static double m_abs(double x) { return __builtin_fabs(x); }
static double m_sqrt(double x) { return __builtin_sqrt(x); }
static double m_sign(double x) { return x != x ? x : x > 0 ? 1 : x < 0 ? -1 : x; }
static double m_fround(double x) { return (double)(float)x; }

static const MathFn1 math_fn1[] = {
    m_abs, kz_math_acos, kz_math_acosh, kz_math_asin, kz_math_asinh, kz_math_atan, kz_math_atanh, kz_math_cbrt,
    kz_math_ceil, kz_math_cos, kz_math_cosh, kz_math_exp, kz_math_expm1, kz_math_floor, m_fround, kz_math_log,
    kz_math_log1p, kz_math_log10, kz_math_log2, kz_math_round, m_sign, kz_math_sin, kz_math_sinh, m_sqrt,
    kz_math_tan, kz_math_tanh, kz_math_trunc,
};
static const char *const math_fn1_names[] = {
    "abs", "acos", "acosh", "asin", "asinh", "atan", "atanh", "cbrt", "ceil", "cos", "cosh", "exp", "expm1", "floor",
    "fround", "log", "log1p", "log10", "log2", "round", "sign", "sin", "sinh", "sqrt", "tan", "tanh", "trunc",
};

static JSValue math_unary(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    double x;
    if (JV_IS_INT(argv[0])) {
        int32_t i = JV_INT(argv[0]);
        /* integers are fixed points of these */
        if (magic == 8 || magic == 13 || magic == 19 || magic == 26) return argv[0];
        if (magic == 0 && i != INT32_MIN) return JV_MKINT(i < 0 ? -i : i);
        x = i;
    } else if (kz_to_number(ctx, argv[0], &x)) return JS_EXCEPTION;
    return jv_number(math_fn1[magic](x));
}

static JSValue math_atan2(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    double y, x;
    if (kz_to_number(ctx, argv[0], &y) || kz_to_number(ctx, argv[1], &x)) return JS_EXCEPTION;
    return jv_number(kz_math_atan2(y, x));
}

static JSValue math_pow(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    double x, y;
    if (kz_to_number(ctx, argv[0], &x) || kz_to_number(ctx, argv[1], &y)) return JS_EXCEPTION;
    return jv_number(kz_math_pow(x, y));
}

/* max (0), min (1): every argument is converted first */
static JSValue math_max(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    double r = magic ? KZ_INFINITY : -KZ_INFINITY;
    bool nan = false;
    for (int i = 0; i < argc; i++) {
        double x;
        if (JV_IS_INT(argv[i])) x = JV_INT(argv[i]);
        else if (kz_to_number(ctx, argv[i], &x)) return JS_EXCEPTION;
        if (x != x) { nan = true; continue; }
        if (magic ? (x < r || (x == 0 && r == 0 && __builtin_signbit(x))) : (x > r || (x == 0 && r == 0 && !__builtin_signbit(x))))
            r = x;
    }
    if (nan) return jv_from_dbl(__builtin_nan(""));
    return jv_number(r);
}

static JSValue math_hypot(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    double small[16], *v = argc <= 16 ? small : kz_malloc(ctx, (size_t)argc * sizeof(double));
    if (!v) return JS_EXCEPTION;
    for (int i = 0; i < argc; i++)
        if (kz_to_number(ctx, argv[i], &v[i])) { if (v != small) kz_free(ctx->rt, v); return JS_EXCEPTION; }
    bool inf = false, nan = false;
    double m = 0;
    for (int i = 0; i < argc; i++) {
        double a = __builtin_fabs(v[i]);
        if (a == KZ_INFINITY) inf = true;
        else if (a != a) nan = true;
        else if (a > m) m = a;
    }
    double r;
    if (inf) r = KZ_INFINITY;
    else if (nan) r = __builtin_nan("");
    else if (m == 0) r = 0;
    else if (argc == 2) r = kz_math_hypot2(v[0], v[1]);
    else {
        /* scaled sum of squares with Kahan compensation */
        double sum = 0, comp = 0;
        for (int i = 0; i < argc; i++) {
            double t = v[i] / m, y = t * t - comp, s = sum + y;
            comp = (s - sum) - y;
            sum = s;
        }
        r = m * __builtin_sqrt(sum);
    }
    if (v != small) kz_free(ctx->rt, v);
    return jv_number(r);
}

static JSValue math_clz32(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    uint32_t u;
    if (kz_to_uint32(ctx, argv[0], &u)) return JS_EXCEPTION;
    return JV_MKINT(u ? __builtin_clz(u) : 32);
}

static JSValue math_imul(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    uint32_t a, b;
    if (kz_to_uint32(ctx, argv[0], &a) || kz_to_uint32(ctx, argv[1], &b)) return JS_EXCEPTION;
    return JV_MKINT((int32_t)(a * b));
}

/* xorshift128+ */
static JSValue math_random(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    uint64_t *s = ctx->rt->rand_state;
    uint64_t x = s[0], y = s[1];
    s[0] = y;
    x ^= x << 23;
    s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    uint64_t r = s[1] + y;
    return jv_from_dbl((double)(r >> 11) * (1.0 / 9007199254740992.0));
}

static const KzFuncDef math_funcs[] = {
    KZ_FN("atan2", math_atan2, 2),
    KZ_FN("clz32", math_clz32, 1),
    KZ_FN("hypot", math_hypot, 2),
    KZ_FN("imul", math_imul, 2),
    KZ_FN_MAGIC("max", math_max, 2, 0),
    KZ_FN_MAGIC("min", math_max, 2, 1),
    KZ_FN("pow", math_pow, 2),
    KZ_FN("random", math_random, 0),
};

int kz_init_number(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    /* Number */
    r[0] = kz_new_object_class(ctx, KZ_CLASS_NUMBER, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (r[0] == JS_EXCEPTION) return -1;
    JV_OBJ(r[0])->u.prim = JV_MKINT(0);
    ctx->intrinsic[KZ_I_NUMBER_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, number_ctor, "Number", 1, 0, KZ_I_NUMBER_PROTO, KZ_I_NUMBER);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], number_funcs, KZ_COUNTOF(number_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], number_proto_funcs, KZ_COUNTOF(number_proto_funcs))) return -1;
    static const struct { const char *name; double v; } consts[] = {
        { "EPSILON", 2.220446049250313e-16 }, { "MAX_SAFE_INTEGER", 9007199254740991.0 },
        { "MAX_VALUE", 1.7976931348623157e308 }, { "MIN_SAFE_INTEGER", -9007199254740991.0 },
        { "MIN_VALUE", 5e-324 }, { "NEGATIVE_INFINITY", -__builtin_inf() }, { "POSITIVE_INFINITY", __builtin_inf() },
    };
    for (size_t i = 0; i < KZ_COUNTOF(consts); i++) {
        JSValue k = kz_atom(ctx, consts[i].name);
        if (k == JS_EXCEPTION || kz_define_value(ctx, r[1], k, jv_number(consts[i].v), 0)) return -1;
    }
    if (kz_define_value_atom(ctx, r[1], KZ_ATOM_NaN, KZ_NAN_BITS, 0)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Number, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* Boolean */
    r[0] = kz_new_object_class(ctx, KZ_CLASS_BOOLEAN, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (r[0] == JS_EXCEPTION) return -1;
    JV_OBJ(r[0])->u.prim = JS_FALSE;
    ctx->intrinsic[KZ_I_BOOLEAN_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, boolean_ctor, "Boolean", 1, 0, KZ_I_BOOLEAN_PROTO, KZ_I_BOOLEAN);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], boolean_proto_funcs, KZ_COUNTOF(boolean_proto_funcs))) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Boolean, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    (void)rt;
    return 0;
}

int kz_init_math(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    for (size_t i = 0; i < KZ_COUNTOF(math_fn1); i++) {
        r[1] = kz_new_native_c(ctx, math_unary, math_fn1_names[i], 1, (int)i);
        if (r[1] == JS_EXCEPTION) return -1;
        JSValue k = kz_atom(ctx, math_fn1_names[i]);
        if (k == JS_EXCEPTION || kz_define_value(ctx, r[0], k, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    }
    if (kz_define_functions(ctx, r[0], math_funcs, KZ_COUNTOF(math_funcs))) return -1;
    static const struct { const char *name; uint64_t bits; } consts[] = {
        { "E", 0x4005bf0a8b145769ULL }, { "LN10", 0x40026bb1bbb55516ULL }, { "LN2", 0x3fe62e42fefa39efULL },
        { "LOG10E", 0x3fdbcb7b1526e50eULL }, { "LOG2E", 0x3ff71547652b82feULL }, { "PI", 0x400921fb54442d18ULL },
        { "SQRT1_2", 0x3fe6a09e667f3bcdULL }, { "SQRT2", 0x3ff6a09e667f3bcdULL },
    };
    for (size_t i = 0; i < KZ_COUNTOF(consts); i++) {
        JSValue k = kz_atom(ctx, consts[i].name);
        double d;
        memcpy(&d, &consts[i].bits, sizeof d);
        if (k == JS_EXCEPTION || kz_define_value(ctx, r[0], k, jv_from_raw_double(d), 0)) return -1;
    }
    r[1] = kz_atom(ctx, "Math");
    if (r[1] == JS_EXCEPTION || kz_define_value(ctx, r[0], rt->sym[KZ_SYM_toStringTag], r[1], KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Math, r[0], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
