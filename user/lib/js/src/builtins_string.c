/*
 * builtins_string.c: the String constructor, String.prototype (ECMA-262 22.1, with Annex B's substr and HTML
 * methods), the string iterator, and the String exotic object's internal methods (10.4.3).
 */
#include "jsint.h"

/* ================================================================== String exotic objects */

static int str_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    if (!KZ_KEY_IS_INDEX(key)) return KZ_FALLBACK;
    JSString *s = JV_STR(o->u.prim);
    uint32_t i = KZ_KEY_INDEX(key);
    if (i >= kz_str_len(s)) return KZ_FALLBACK;
    s = kz_str_flat(ctx, s);
    if (!s) return -1;
    uint32_t c = kz_str_at(s, i);
    d->value = c < 256 ? ctx->rt->char_str[c] : kz_str_sub(ctx, o->u.prim, i, i + 1);
    if (d->value == JS_EXCEPTION) return -1;
    d->getter = d->setter = JS_UNDEFINED;
    d->flags = KZ_PROP_E | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
    return 1;
}

static int str_define_own(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d) {
    if (!KZ_KEY_IS_INDEX(key) || KZ_KEY_INDEX(key) >= kz_str_len(JV_STR(o->u.prim))) return KZ_FALLBACK;
    /* IsCompatiblePropertyDescriptor against { value: char, writable: false, enumerable: true, configurable: false } */
    if ((d->flags & PD_HAS_C) && (d->flags & KZ_PROP_C)) return 0;
    if ((d->flags & PD_HAS_E) && !(d->flags & KZ_PROP_E)) return 0;
    if (d->flags & (PD_HAS_GET | PD_HAS_SET)) return 0;
    if ((d->flags & PD_HAS_W) && (d->flags & KZ_PROP_W)) return 0;
    if (d->flags & PD_HAS_VALUE) {
        KZ_ROOT_DESC(ctx, cur);
        if (str_get_own(ctx, o, key, &cur) < 0) return -1;
        int same = kz_same_value(ctx, d->value, cur.value);
        if (same < 0) return -1;
        if (!same) return 0;
    }
    return 1;
}

static int str_del(JSContext *ctx, JSObject *o, JSValue key) {
    (void)ctx;
    if (KZ_KEY_IS_INDEX(key) && KZ_KEY_INDEX(key) < kz_str_len(JV_STR(o->u.prim))) return 0;
    return KZ_FALLBACK;
}

static int str_own_keys(JSContext *ctx, JSObject *o, KzVec *keys) {
    uint32_t n = kz_str_len(JV_STR(o->u.prim));
    for (uint32_t i = 0; i < n; i++)
        if (kz_vec_push(ctx, keys, KZ_KEY_FROM_INDEX(i))) return -1;
    return 0;
}

static const KzExotic string_exotic = {
    .get_own = str_get_own, .define_own = str_define_own, .del = str_del, .own_keys = str_own_keys,
};

/* ================================================================== helpers */

/* RequireObjectCoercible(this) then ToString, flattened */
static JSValue this_str(JSContext *ctx, JSValue v, const char *method) {
    if (JV_IS_STRING(v)) return kz_str_flatten(ctx, v);
    if (JV_IS_NULLISH(v)) return kz_throw_type(ctx, "String.prototype.%s called on null or undefined", method);
    JSValue s = kz_to_string(ctx, v);
    if (s == JS_EXCEPTION) return s;
    return kz_str_flatten(ctx, s);
}

static JSValue to_flat_string(JSContext *ctx, JSValue v) {
    JSValue s = kz_to_string(ctx, v);
    if (s == JS_EXCEPTION) return s;
    return kz_str_flatten(ctx, s);
}

/* IsRegExp */
int kz_is_regexp(JSContext *ctx, JSValue v) {
    if (!JV_IS_OBJECT(v)) return 0;
    JSValue m = kz_get(ctx, v, ctx->rt->sym[KZ_SYM_match], v);
    if (m == JS_EXCEPTION) return -1;
    if (m != JS_UNDEFINED) return kz_to_bool(m);
    return kz_class(JV_OBJ(v)) == KZ_CLASS_REGEXP;
}

static bool is_trim_space(uint32_t c) {
    return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029 || kz_unicode_is_space(c);
}

static uint32_t code_point_at(JSString *s, uint32_t i, uint32_t *units) {
    uint32_t n = kz_str_len(s), c = kz_str_at(s, i);
    *units = 1;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
        uint32_t d = kz_str_at(s, i + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) {
            *units = 2;
            return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        }
    }
    return c;
}

/* ================================================================== the constructor and statics */

static JSValue string_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    if (argc == 0) r[0] = rt->atom[KZ_ATOM_empty];
    else {
        if (rt->new_target == JS_UNDEFINED && JV_IS_SYMBOL(argv[0])) return kz_symbol_descriptive_string(ctx, argv[0]);
        r[0] = kz_to_string(ctx, argv[0]);
        if (r[0] == JS_EXCEPTION) return r[0];
    }
    if (rt->new_target == JS_UNDEFINED) return r[0];
    if (kz_get_proto_from_ctor(ctx, rt->new_target, KZ_I_STRING_PROTO, &r[1])) return JS_EXCEPTION;
    JSValue o = kz_new_object_class(ctx, KZ_CLASS_STRING, r[1]);
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.prim = r[0];
    KZ_ROOT(ctx, ro);
    ro = o;
    if (kz_obj_add_prop(ctx, JV_OBJ(ro), rt->atom[KZ_ATOM_length], 0, JV_MKINT((int32_t)kz_str_len(JV_STR(r[0]))))) return JS_EXCEPTION;
    return ro;
}

static JSValue string_from_char_code(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (int i = 0; i < argc; i++) {
        uint32_t u;
        if (kz_to_uint32(ctx, argv[i], &u)) { kz_sb_free(&sb); return JS_EXCEPTION; }
        kz_sb_putc(&sb, u & 0xFFFF);
    }
    return kz_sb_finish(&sb);
}

static JSValue string_from_code_point(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (int i = 0; i < argc; i++) {
        double d;
        if (kz_to_number(ctx, argv[i], &d)) { kz_sb_free(&sb); return JS_EXCEPTION; }
        if (d != kz_math_trunc(d) || d < 0 || d > 0x10FFFF) {
            kz_sb_free(&sb);
            return kz_throw_range(ctx, "Invalid code point");
        }
        kz_sb_put_cp(&sb, (uint32_t)d);
    }
    return kz_sb_finish(&sb);
}

static JSValue string_raw(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_object(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_get(ctx, r[0], ctx->rt->atom[KZ_ATOM_raw], r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[1] = kz_to_object(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    uint64_t n;
    if (kz_length_of(ctx, r[1], &n)) return JS_EXCEPTION;
    if (n == 0) return ctx->rt->atom[KZ_ATOM_empty];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (uint64_t i = 0;; i++) {
        r[2] = i <= KZ_MAX_INDEX ? kz_get_index(ctx, r[1], (uint32_t)i) : JS_UNDEFINED;
        if (r[2] == JS_EXCEPTION) goto fail;
        r[2] = kz_to_string(ctx, r[2]);
        if (r[2] == JS_EXCEPTION || kz_sb_put_value(&sb, r[2])) goto fail;
        if (i + 1 == n) break;
        if ((int64_t)i + 1 < argc) {
            r[2] = kz_to_string(ctx, argv[i + 1]);
            if (r[2] == JS_EXCEPTION || kz_sb_put_value(&sb, r[2])) goto fail;
        }
    }
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

/* ================================================================== simple accessors */

static JSValue thisStringValue(JSContext *ctx, JSValue v) {
    if (JV_IS_STRING(v)) return v;
    if (kz_is_class(v, KZ_CLASS_STRING)) return JV_OBJ(v)->u.prim;
    return kz_throw_type(ctx, "String.prototype.valueOf requires that 'this' be a String");
}

static JSValue proto_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return thisStringValue(ctx, this_val);
}

/* at (0), charAt (1), charCodeAt (2), codePointAt (3) */
static JSValue proto_char_at(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "at", "charAt", "charCodeAt", "codePointAt" };
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, names[magic]);
    if (sv == JS_EXCEPTION) return sv;
    JSString *s = JV_STR(sv);
    double pos;
    if (kz_to_integer_or_inf(ctx, argv[0], &pos)) return JS_EXCEPTION;
    double len = kz_str_len(s);
    if (magic == 0 && pos < 0) pos += len;
    if (pos < 0 || pos >= len) {
        if (magic == 1) return ctx->rt->atom[KZ_ATOM_empty];
        if (magic == 2) return jv_from_dbl(__builtin_nan(""));
        return JS_UNDEFINED;
    }
    s = JV_STR(sv);
    uint32_t i = (uint32_t)pos, c = kz_str_at(s, i);
    switch (magic) {
    case 2: return JV_MKINT((int32_t)c);
    case 3: { uint32_t u; return JV_MKINT((int32_t)code_point_at(s, i, &u)); }
    default: return c < 256 ? ctx->rt->char_str[c] : kz_str_sub(ctx, sv, i, i + 1);
    }
}

static JSValue proto_concat(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, "concat");
    if (r[0] == JS_EXCEPTION) return r[0];
    for (int i = 0; i < argc; i++) {
        r[1] = kz_to_string(ctx, argv[i]);
        if (r[1] == JS_EXCEPTION) return r[1];
        r[0] = kz_str_concat(ctx, r[0], r[1]);
        if (r[0] == JS_EXCEPTION) return r[0];
    }
    return r[0];
}

static bool region_eq(JSString *s, uint32_t at, JSString *t) {
    uint32_t n = kz_str_len(t);
    for (uint32_t i = 0; i < n; i++)
        if (kz_str_at(s, at + i) != kz_str_at(t, i)) return false;
    return true;
}

/* startsWith (0), endsWith (1), includes (2) */
static JSValue proto_starts_with(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "startsWith", "endsWith", "includes" };
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, names[magic]);
    if (r[0] == JS_EXCEPTION) return r[0];
    int isr = kz_is_regexp(ctx, argv[0]);
    if (isr < 0) return JS_EXCEPTION;
    if (isr) return kz_throw_type(ctx, "First argument to String.prototype.%s must not be a regular expression", names[magic]);
    r[1] = to_flat_string(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSString *s = JV_STR(r[0]), *t = JV_STR(r[1]);
    double len = kz_str_len(s), pos;
    if (magic == 1) {
        if (argv[1] == JS_UNDEFINED) pos = len;
        else if (kz_to_integer_or_inf(ctx, argv[1], &pos)) return JS_EXCEPTION;
    } else if (kz_to_integer_or_inf(ctx, argv[1], &pos)) return JS_EXCEPTION;
    pos = pos < 0 ? 0 : pos > len ? len : pos;
    s = JV_STR(r[0]);
    t = JV_STR(r[1]);
    uint32_t tl = kz_str_len(t);
    if (magic == 0) {
        if (pos + tl > len) return JS_FALSE;
        return JV_MKBOOL(region_eq(s, (uint32_t)pos, t));
    }
    if (magic == 1) {
        if (tl > pos) return JV_MKBOOL(tl == 0);
        return JV_MKBOOL(region_eq(s, (uint32_t)pos - tl, t));
    }
    return JV_MKBOOL(kz_str_index_of(s, t, (uint32_t)pos) >= 0);
}

static JSValue proto_index_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, "indexOf");
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = to_flat_string(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    double pos;
    if (kz_to_integer_or_inf(ctx, argv[1], &pos)) return JS_EXCEPTION;
    double len = kz_str_len(JV_STR(r[0]));
    pos = pos < 0 ? 0 : pos > len ? len : pos;
    return JV_MKINT(kz_str_index_of(JV_STR(r[0]), JV_STR(r[1]), (uint32_t)pos));
}

static JSValue proto_last_index_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, "lastIndexOf");
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = to_flat_string(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    double num, pos;
    if (kz_to_number(ctx, argv[1], &num)) return JS_EXCEPTION;
    if (num != num) pos = KZ_INFINITY;
    else pos = kz_math_trunc(num);
    JSString *s = JV_STR(r[0]), *t = JV_STR(r[1]);
    double len = kz_str_len(s), tl = kz_str_len(t);
    pos = pos < 0 ? 0 : pos > len ? len : pos;
    if (tl > len) return JV_MKINT(-1);
    double start = pos < len - tl ? pos : len - tl;
    for (int64_t n = (int64_t)start; n >= 0; n--)
        if (region_eq(s, (uint32_t)n, t)) return JV_MKINT((int32_t)n);
    return JV_MKINT(-1);
}

/* slice (0), substring (1), substr (2) */
static JSValue proto_slice(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "slice", "substring", "substr" };
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, names[magic]);
    if (sv == JS_EXCEPTION) return sv;
    double len = kz_str_len(JV_STR(sv)), a, b;
    if (kz_to_integer_or_inf(ctx, argv[0], &a)) return JS_EXCEPTION;
    if (magic == 2) {
        if (a < 0) a = a + len < 0 ? 0 : a + len;
        else if (a > len) a = len;
        double l;
        if (argv[1] == JS_UNDEFINED) l = len;
        else if (kz_to_integer_or_inf(ctx, argv[1], &l)) return JS_EXCEPTION;
        double end = a + l < len ? a + l : len;
        if (end <= a) return ctx->rt->atom[KZ_ATOM_empty];
        return kz_str_sub(ctx, sv, (uint32_t)a, (uint32_t)end);
    }
    if (argv[1] == JS_UNDEFINED) b = len;
    else if (kz_to_integer_or_inf(ctx, argv[1], &b)) return JS_EXCEPTION;
    if (magic == 0) {
        a = a < 0 ? (a + len < 0 ? 0 : a + len) : (a > len ? len : a);
        b = b < 0 ? (b + len < 0 ? 0 : b + len) : (b > len ? len : b);
        if (b <= a) return ctx->rt->atom[KZ_ATOM_empty];
        return kz_str_sub(ctx, sv, (uint32_t)a, (uint32_t)b);
    }
    a = a < 0 ? 0 : a > len ? len : a;
    b = b < 0 ? 0 : b > len ? len : b;
    if (a > b) { double t = a; a = b; b = t; }
    return kz_str_sub(ctx, sv, (uint32_t)a, (uint32_t)b);
}

/* trim (0), trimStart (1), trimEnd (2) */
static JSValue proto_trim(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    static const char *const names[] = { "trim", "trimStart", "trimEnd" };
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, names[magic]);
    if (sv == JS_EXCEPTION) return sv;
    JSString *s = JV_STR(sv);
    uint32_t a = 0, b = kz_str_len(s);
    if (magic != 2) while (a < b && is_trim_space(kz_str_at(s, a))) a++;
    if (magic != 1) while (b > a && is_trim_space(kz_str_at(s, b - 1))) b--;
    if (a == 0 && b == kz_str_len(s)) return sv;
    return kz_str_sub(ctx, sv, a, b);
}

/* padStart (0), padEnd (1) */
static JSValue proto_pad(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, magic ? "padEnd" : "padStart");
    if (r[0] == JS_EXCEPTION) return r[0];
    uint64_t max;
    if (kz_to_length(ctx, argv[0], &max)) return JS_EXCEPTION;
    uint32_t len = kz_str_len(JV_STR(r[0]));
    if (max <= len) return r[0];
    if (argv[1] == JS_UNDEFINED) r[1] = ctx->rt->char_str[' '];
    else {
        r[1] = to_flat_string(ctx, argv[1]);
        if (r[1] == JS_EXCEPTION) return r[1];
    }
    JSString *f = JV_STR(r[1]);
    uint32_t fl = kz_str_len(f);
    if (fl == 0) return r[0];
    if (max > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
    uint32_t fill = (uint32_t)max - len;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    if (magic) kz_sb_put_str(&sb, JV_STR(r[0]));
    for (uint32_t i = 0; i < fill; i++) kz_sb_putc(&sb, kz_str_at(JV_STR(r[1]), i % fl));
    if (!magic) kz_sb_put_str(&sb, JV_STR(r[0]));
    return kz_sb_finish(&sb);
}

static JSValue proto_repeat(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, "repeat");
    if (sv == JS_EXCEPTION) return sv;
    double n;
    if (kz_to_integer_or_inf(ctx, argv[0], &n)) return JS_EXCEPTION;
    if (n < 0 || n == KZ_INFINITY) return kz_throw_range(ctx, "Invalid count value: %s", n < 0 ? "negative" : "Infinity");
    uint32_t len = kz_str_len(JV_STR(sv));
    if (n == 0 || len == 0) return ctx->rt->atom[KZ_ATOM_empty];
    if (n * len > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (uint32_t i = 0; i < (uint32_t)n; i++) kz_sb_put_str(&sb, JV_STR(sv));
    return kz_sb_finish(&sb);
}

static JSValue proto_is_well_formed(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, magic ? "toWellFormed" : "isWellFormed");
    if (sv == JS_EXCEPTION) return sv;
    JSString *s = JV_STR(sv);
    uint32_t n = kz_str_len(s);
    bool ok = true;
    if (kz_str_wide(s)) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t u;
            uint32_t c = code_point_at(s, i, &u);
            if (c >= 0xD800 && c <= 0xDFFF) { ok = false; break; }
            i += u - 1;
        }
    }
    if (!magic) return JV_MKBOOL(ok);
    if (ok) return sv;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u;
        uint32_t c = code_point_at(JV_STR(sv), i, &u);
        kz_sb_put_cp(&sb, c >= 0xD800 && c <= 0xDFFF ? 0xFFFD : c);
        i += u - 1;
    }
    return kz_sb_finish(&sb);
}

/* ================================================================== case mapping */

static bool is_cased(uint32_t c) {
    if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
    return kz_unicode_has_binary("Cased", c);
}

static bool is_case_ignorable(uint32_t c) {
    if (c < 0x80) return c == '\'' || c == '.' || c == ':' || c == '^' || c == '`';
    return kz_unicode_has_binary("Case_Ignorable", c);
}

/* the Final_Sigma condition at index i (of U+03A3) */
static bool final_sigma(JSString *s, uint32_t i) {
    uint32_t n = kz_str_len(s);
    /* before: a cased letter, skipping case-ignorable ones */
    bool before = false;
    for (int64_t j = (int64_t)i - 1; j >= 0; j--) {
        uint32_t c = kz_str_at(s, (uint32_t)j);
        if (c >= 0xDC00 && c <= 0xDFFF && j > 0) {
            uint32_t h = kz_str_at(s, (uint32_t)j - 1);
            if (h >= 0xD800 && h <= 0xDBFF) { c = 0x10000 + ((h - 0xD800) << 10) + (c - 0xDC00); j--; }
        }
        if (is_case_ignorable(c)) continue;
        before = is_cased(c);
        break;
    }
    if (!before) return false;
    for (uint32_t j = i + 1; j < n;) {
        uint32_t u;
        uint32_t c = code_point_at(s, j, &u);
        j += u;
        if (is_case_ignorable(c)) continue;
        return !is_cased(c);
    }
    return true;
}

static JSValue convert_case(JSContext *ctx, JSValue sv, bool upper) {
    JSString *s = JV_STR(sv);
    uint32_t n = kz_str_len(s);
    /* the common case: nothing changes */
    uint32_t i = 0;
    for (; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        if (c >= 0x80) break;
        if (upper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) break;
    }
    if (i == n) return sv;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_put_substr(&sb, s, 0, i);
    for (; i < n;) {
        s = JV_STR(sv);
        uint32_t u;
        uint32_t c = code_point_at(s, i, &u);
        uint32_t out[3];
        int k;
        if (!upper && c == 0x3A3) {
            out[0] = final_sigma(s, i) ? 0x3C2 : 0x3C3;
            k = 1;
        } else k = upper ? kz_unicode_to_upper_full(c, out) : kz_unicode_to_lower_full(c, out);
        for (int j = 0; j < k; j++) kz_sb_put_cp(&sb, out[j]);
        i += u;
    }
    return kz_sb_finish(&sb);
}

/* toLowerCase (0), toUpperCase (1), toLocaleLowerCase (2), toLocaleUpperCase (3) */
static JSValue proto_case(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    static const char *const names[] = { "toLowerCase", "toUpperCase", "toLocaleLowerCase", "toLocaleUpperCase" };
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, names[magic]);
    if (sv == JS_EXCEPTION) return sv;
    return convert_case(ctx, sv, magic & 1);
}

/* ================================================================== normalize, localeCompare */

static int str_to_cps(JSContext *ctx, JSString *s, uint32_t **out, uint32_t *n) {
    uint32_t len = kz_str_len(s), k = 0;
    uint32_t *v = kz_malloc(ctx, ((size_t)len + 1) * sizeof(uint32_t));
    if (!v) return -1;
    for (uint32_t i = 0; i < len;) {
        uint32_t u;
        v[k++] = code_point_at(s, i, &u);
        i += u;
    }
    *out = v;
    *n = k;
    return 0;
}

static JSValue cps_to_str(JSContext *ctx, const uint32_t *v, uint32_t n) {
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (uint32_t i = 0; i < n; i++) kz_sb_put_cp(&sb, v[i]);
    return kz_sb_finish(&sb);
}

static JSValue proto_normalize(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, "normalize");
    if (r[0] == JS_EXCEPTION) return r[0];
    int form = 0;
    if (argv[0] != JS_UNDEFINED) {
        r[1] = to_flat_string(ctx, argv[0]);
        if (r[1] == JS_EXCEPTION) return r[1];
        JSString *f = JV_STR(r[1]);
        if (kz_str_eq_ascii(f, "NFC")) form = 0;
        else if (kz_str_eq_ascii(f, "NFD")) form = 1;
        else if (kz_str_eq_ascii(f, "NFKC")) form = 2;
        else if (kz_str_eq_ascii(f, "NFKD")) form = 3;
        else return kz_throw_range(ctx, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    JSString *s = JV_STR(r[0]);
    /* ASCII is invariant under every form */
    uint32_t len = kz_str_len(s), i = 0;
    for (; i < len && kz_str_at(s, i) < 0x80; i++) {}
    if (i == len) return r[0];
    uint32_t *cps, n, *out, outn;
    if (str_to_cps(ctx, s, &cps, &n)) return JS_EXCEPTION;
    int rc = kz_unicode_normalize(ctx, cps, n, form, NULL, &out, &outn);
    kz_free(ctx->rt, cps);
    if (rc) return JS_EXCEPTION;
    JSValue res = cps_to_str(ctx, out, outn);
    kz_free(ctx->rt, out);
    return res;
}

/*
 * localeCompare: a simplified version of the Unicode Collation Algorithm's default ordering, good for Latin,
 * Greek and Cyrillic text: compare the NFD forms first by base letters ignoring case and marks, then by marks,
 * then by case (lower before upper), then by code points.  Canonically equivalent strings compare equal.
 */
typedef struct CollKey { uint32_t *base, *marks, *cases, *all; uint32_t nb, nm, nc, na; } CollKey;

static int coll_key(JSContext *ctx, JSString *s, CollKey *k) {
    uint32_t *cps, n;
    if (str_to_cps(ctx, s, &cps, &n)) return -1;
    uint32_t *d, dn;
    int rc = kz_unicode_normalize(ctx, cps, n, 1, NULL, &d, &dn);
    kz_free(ctx->rt, cps);
    if (rc) return -1;
    k->all = d;
    k->na = dn;
    k->base = kz_malloc(ctx, ((size_t)dn + 1) * 3 * sizeof(uint32_t));
    if (!k->base) { kz_free(ctx->rt, d); return -1; }
    k->marks = k->base + dn + 1;
    k->cases = k->marks + dn + 1;
    k->nb = k->nm = k->nc = 0;
    for (uint32_t i = 0; i < dn; i++) {
        uint32_t c = d[i];
        int cat = kz_unicode_category(c);
        if (cat == 6 /* Mn */ || cat == 7 /* Mc */ || cat == 8 /* Me */) { k->marks[k->nm++] = c; continue; }
        uint32_t f = kz_unicode_fold_simple(c);
        k->base[k->nb++] = f;
        k->cases[k->nc++] = f == c ? 0 : 1;
    }
    return 0;
}

static int cmp_u32(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
    uint32_t n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return na < nb ? -1 : na > nb ? 1 : 0;
}

/* primary weights: letters and digits sort after punctuation and spaces, digits before letters */
static uint32_t primary(uint32_t c) {
    int cat = kz_unicode_category(c);
    uint32_t group;
    if (cat >= 1 && cat <= 5) group = 3;           /* letters */
    else if (cat >= 9 && cat <= 11) group = 2;     /* numbers */
    else if (cat >= 23 && cat <= 25) group = 0;    /* separators */
    else group = 1;                                /* punctuation, symbols, others */
    return (group << 24) | c;
}

int kz_locale_compare(JSContext *ctx, JSString *a, JSString *b) {
    CollKey ka, kb;
    if (coll_key(ctx, a, &ka)) return -2;
    if (coll_key(ctx, b, &kb)) { kz_free(ctx->rt, ka.base); kz_free(ctx->rt, ka.all); return -2; }
    for (uint32_t i = 0; i < ka.nb; i++) ka.base[i] = primary(ka.base[i]);
    for (uint32_t i = 0; i < kb.nb; i++) kb.base[i] = primary(kb.base[i]);
    int r = cmp_u32(ka.base, ka.nb, kb.base, kb.nb);
    if (!r) r = cmp_u32(ka.marks, ka.nm, kb.marks, kb.nm);
    if (!r) r = cmp_u32(ka.cases, ka.nc, kb.cases, kb.nc);
    if (!r) r = cmp_u32(ka.all, ka.na, kb.all, kb.na);
    kz_free(ctx->rt, ka.base); kz_free(ctx->rt, ka.all);
    kz_free(ctx->rt, kb.base); kz_free(ctx->rt, kb.all);
    return r;
}

static JSValue proto_locale_compare(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, "localeCompare");
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = to_flat_string(ctx, argv[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    int c = kz_locale_compare(ctx, JV_STR(r[0]), JV_STR(r[1]));
    if (c == -2) return JS_EXCEPTION;
    return JV_MKINT(c);
}

/* ================================================================== GetSubstitution and the regexp-dispatching methods */

/* GetSubstitution (22.1.3.19.1); captures: n values (strings or undefined); named: undefined or an object */
JSValue kz_get_substitution(JSContext *ctx, JSValue matched, JSValue str, uint32_t position, const JSValue *captures,
                            uint32_t ncaptures, JSValue named, JSValue tmpl) {
    KZ_ROOTS(ctx, r, 2);
    JSString *t = JV_STR(tmpl), *s = JV_STR(str), *m = JV_STR(matched);
    uint32_t tl = kz_str_len(t), sl = kz_str_len(s);
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (uint32_t i = 0; i < tl;) {
        t = JV_STR(tmpl); s = JV_STR(str); m = JV_STR(matched);
        uint32_t c = kz_str_at(t, i);
        if (c != '$' || i + 1 >= tl) { kz_sb_putc(&sb, c); i++; continue; }
        uint32_t d = kz_str_at(t, i + 1);
        if (d == '$') { kz_sb_putc(&sb, '$'); i += 2; continue; }
        if (d == '&') { kz_sb_put_str(&sb, m); i += 2; continue; }
        if (d == '`') { kz_sb_put_substr(&sb, s, 0, position < sl ? position : sl); i += 2; continue; }
        if (d == '\'') {
            uint32_t tail = position + kz_str_len(m);
            if (tail < sl) kz_sb_put_substr(&sb, s, tail, sl);
            i += 2;
            continue;
        }
        if (d >= '0' && d <= '9') {
            uint32_t digits = 1, index = d - '0';
            if (i + 2 < tl) {
                uint32_t e = kz_str_at(t, i + 2);
                if (e >= '0' && e <= '9') { digits = 2; index = index * 10 + (e - '0'); }
            }
            if (digits == 2 && index > ncaptures) { digits = 1; index = d - '0'; }
            if (index >= 1 && index <= ncaptures) {
                JSValue cap = captures[index - 1];
                if (cap != JS_UNDEFINED) kz_sb_put_value(&sb, cap);
            } else kz_sb_put_substr(&sb, t, i, i + 1 + digits);
            i += 1 + digits;
            continue;
        }
        if (d == '<') {
            int32_t gt = -1;
            for (uint32_t j = i + 2; j < tl; j++)
                if (kz_str_at(t, j) == '>') { gt = (int32_t)j; break; }
            if (gt < 0 || named == JS_UNDEFINED) { kz_sb_puts(&sb, "$<"); i += 2; continue; }
            r[0] = kz_str_sub(ctx, tmpl, i + 2, (uint32_t)gt);
            if (r[0] == JS_EXCEPTION) goto fail;
            r[0] = kz_key_from_value(ctx, r[0]);
            if (r[0] == JS_EXCEPTION) goto fail;
            r[1] = kz_get(ctx, named, r[0], named);
            if (r[1] == JS_EXCEPTION) goto fail;
            if (r[1] != JS_UNDEFINED) {
                r[1] = kz_to_string(ctx, r[1]);
                if (r[1] == JS_EXCEPTION || kz_sb_put_value(&sb, r[1])) goto fail;
            }
            i = (uint32_t)gt + 1;
            continue;
        }
        kz_sb_putc(&sb, '$');
        i++;
    }
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

/* match (0), matchAll (1), search (2) */
static JSValue proto_match(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "match", "matchAll", "search" };
    static const int syms[] = { KZ_SYM_match, KZ_SYM_matchAll, KZ_SYM_search };
    JSRuntime *rt = ctx->rt;
    if (JV_IS_NULLISH(this_val)) return kz_throw_type(ctx, "String.prototype.%s called on null or undefined", names[magic]);
    KZ_ROOTS(ctx, r, 3);
    JSValue rx = argv[0];
    /* (ES2025: only an object's @@match / @@matchAll / @@search is looked up, a primitive argument is not asked) */
    if (JV_IS_OBJECT(rx)) {
        if (magic == 1) {
            int isr = kz_is_regexp(ctx, rx);
            if (isr < 0) return JS_EXCEPTION;
            if (isr) {
                r[0] = kz_get(ctx, rx, rt->atom[KZ_ATOM_flags], rx);
                if (r[0] == JS_EXCEPTION) return r[0];
                if (JV_IS_NULLISH(r[0])) return kz_throw_type(ctx, "String.prototype.matchAll: flags is null or undefined");
                r[0] = to_flat_string(ctx, r[0]);
                if (r[0] == JS_EXCEPTION) return r[0];
                JSString *f = JV_STR(r[0]);
                bool g = false;
                for (uint32_t i = 0; i < kz_str_len(f); i++)
                    if (kz_str_at(f, i) == 'g') g = true;
                if (!g) return kz_throw_type(ctx, "String.prototype.matchAll called with a non-global RegExp argument");
            }
        }
        r[0] = kz_get_method(ctx, rx, rt->sym[syms[magic]]);
        if (r[0] == JS_EXCEPTION) return r[0];
        if (r[0] != JS_UNDEFINED) {
            JSValue args[1] = { this_val };
            return kz_call(ctx, r[0], rx, 1, args);
        }
    }
    r[1] = kz_to_string(ctx, this_val);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = kz_regexp_create(ctx, rx, magic == 1 ? kz_str_from_char(ctx, 'g') : JS_UNDEFINED);
    if (r[2] == JS_EXCEPTION) return r[2];
    JSValue args[1] = { r[1] };
    return kz_invoke(ctx, r[2], rt->sym[syms[magic]], 1, args);
}

/* replace (0), replaceAll (1) */
static JSValue proto_replace(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSRuntime *rt = ctx->rt;
    const char *name = magic ? "replaceAll" : "replace";
    if (JV_IS_NULLISH(this_val)) return kz_throw_type(ctx, "String.prototype.%s called on null or undefined", name);
    KZ_ROOTS(ctx, r, 5);
    JSValue sv = argv[0], rv = argv[1];
    if (JV_IS_OBJECT(sv)) {
        if (magic) {
            int isr = kz_is_regexp(ctx, sv);
            if (isr < 0) return JS_EXCEPTION;
            if (isr) {
                r[0] = kz_get(ctx, sv, rt->atom[KZ_ATOM_flags], sv);
                if (r[0] == JS_EXCEPTION) return r[0];
                if (JV_IS_NULLISH(r[0])) return kz_throw_type(ctx, "String.prototype.replaceAll: flags is null or undefined");
                r[0] = to_flat_string(ctx, r[0]);
                if (r[0] == JS_EXCEPTION) return r[0];
                JSString *f = JV_STR(r[0]);
                bool g = false;
                for (uint32_t i = 0; i < kz_str_len(f); i++)
                    if (kz_str_at(f, i) == 'g') g = true;
                if (!g) return kz_throw_type(ctx, "replaceAll must be called with a global RegExp");
            }
        }
        r[0] = kz_get_method(ctx, sv, rt->sym[KZ_SYM_replace]);
        if (r[0] == JS_EXCEPTION) return r[0];
        if (r[0] != JS_UNDEFINED) {
            JSValue args[2] = { this_val, rv };
            return kz_call(ctx, r[0], sv, 2, args);
        }
    }
    r[1] = to_flat_string(ctx, this_val);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = to_flat_string(ctx, sv);
    if (r[2] == JS_EXCEPTION) return r[2];
    bool functional = kz_is_callable(rv);
    if (!functional) {
        r[3] = to_flat_string(ctx, rv);
        if (r[3] == JS_EXCEPTION) return r[3];
    }
    uint32_t slen = kz_str_len(JV_STR(r[2])), len = kz_str_len(JV_STR(r[1]));
    uint32_t advance = slen ? slen : 1;
    int32_t pos = kz_str_index_of(JV_STR(r[1]), JV_STR(r[2]), 0);
    if (pos < 0) return r[1];
    KZ_ROOT_VEC(ctx, positions);
    while (pos >= 0) {
        if (kz_vec_push(ctx, &positions, JV_MKINT(pos))) return JS_EXCEPTION;
        if (!magic) break;
        if ((uint64_t)pos + advance > len) break;
        pos = kz_str_index_of(JV_STR(r[1]), JV_STR(r[2]), (uint32_t)pos + advance);
    }
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    uint32_t end_last = 0;
    for (uint32_t i = 0; i < positions.len; i++) {
        uint32_t p = (uint32_t)JV_INT(positions.v[i]);
        kz_sb_put_substr(&sb, JV_STR(r[1]), end_last, p);
        if (functional) {
            JSValue args[3] = { r[2], JV_MKINT((int32_t)p), r[1] };
            r[4] = kz_call(ctx, rv, JS_UNDEFINED, 3, args);
            if (r[4] == JS_EXCEPTION) goto fail;
            r[4] = kz_to_string(ctx, r[4]);
        } else r[4] = kz_get_substitution(ctx, r[2], r[1], p, NULL, 0, JS_UNDEFINED, r[3]);
        if (r[4] == JS_EXCEPTION || kz_sb_put_value(&sb, r[4])) goto fail;
        end_last = p + slen;
    }
    if (end_last < len) kz_sb_put_substr(&sb, JV_STR(r[1]), end_last, len);
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

static JSValue proto_split(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (JV_IS_NULLISH(this_val)) return kz_throw_type(ctx, "String.prototype.split called on null or undefined");
    KZ_ROOTS(ctx, r, 4);
    JSValue sep = argv[0];
    if (JV_IS_OBJECT(sep)) {
        r[0] = kz_get_method(ctx, sep, rt->sym[KZ_SYM_split]);
        if (r[0] == JS_EXCEPTION) return r[0];
        if (r[0] != JS_UNDEFINED) {
            JSValue args[2] = { this_val, argv[1] };
            return kz_call(ctx, r[0], sep, 2, args);
        }
    }
    r[1] = to_flat_string(ctx, this_val);
    if (r[1] == JS_EXCEPTION) return r[1];
    uint32_t lim = 0xFFFFFFFFu;
    if (argv[1] != JS_UNDEFINED && kz_to_uint32(ctx, argv[1], &lim)) return JS_EXCEPTION;
    r[2] = to_flat_string(ctx, sep);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[3] = kz_new_array(ctx, 0);
    if (r[3] == JS_EXCEPTION) return r[3];
    if (lim == 0) return r[3];
    JSObject *a = JV_OBJ(r[3]);
    if (sep == JS_UNDEFINED) {
        if (kz_array_push(ctx, a, r[1])) return JS_EXCEPTION;
        return r[3];
    }
    uint32_t sl = kz_str_len(JV_STR(r[1])), pl = kz_str_len(JV_STR(r[2]));
    if (pl == 0) {
        uint32_t n = lim < sl ? lim : sl;
        if (kz_elems_reserve(ctx, a, n)) return JS_EXCEPTION;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = kz_str_at(JV_STR(r[1]), i);
            JSValue ch = c < 256 ? rt->char_str[c] : kz_str_sub(ctx, r[1], i, i + 1);
            if (ch == JS_EXCEPTION || kz_array_push(ctx, a, ch)) return JS_EXCEPTION;
        }
        return r[3];
    }
    if (sl == 0) {
        if (kz_array_push(ctx, a, r[1])) return JS_EXCEPTION;
        return r[3];
    }
    uint32_t i = 0;
    int32_t j = kz_str_index_of(JV_STR(r[1]), JV_STR(r[2]), 0);
    while (j >= 0) {
        r[0] = kz_str_sub(ctx, r[1], i, (uint32_t)j);
        if (r[0] == JS_EXCEPTION || kz_array_push(ctx, a, r[0])) return JS_EXCEPTION;
        if (a->u.array.length == lim) return r[3];
        i = (uint32_t)j + pl;
        j = kz_str_index_of(JV_STR(r[1]), JV_STR(r[2]), i);
    }
    r[0] = kz_str_sub(ctx, r[1], i, sl);
    if (r[0] == JS_EXCEPTION || kz_array_push(ctx, a, r[0])) return JS_EXCEPTION;
    return r[3];
}

/* ================================================================== Annex B: HTML methods */

static const struct { const char *tag, *attr; } html_methods[] = {
    { "a", "name" }, { "big", NULL }, { "blink", NULL }, { "b", NULL }, { "tt", NULL }, { "font", "color" },
    { "font", "size" }, { "i", NULL }, { "a", "href" }, { "small", NULL }, { "strike", NULL }, { "sub", NULL },
    { "sup", NULL },
};

static JSValue proto_html(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 2);
    r[0] = this_str(ctx, this_val, html_methods[magic].tag);
    if (r[0] == JS_EXCEPTION) return r[0];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_putc(&sb, '<');
    kz_sb_puts(&sb, html_methods[magic].tag);
    if (html_methods[magic].attr) {
        r[1] = to_flat_string(ctx, argv[0]);
        if (r[1] == JS_EXCEPTION) { kz_sb_free(&sb); return r[1]; }
        kz_sb_putc(&sb, ' ');
        kz_sb_puts(&sb, html_methods[magic].attr);
        kz_sb_puts(&sb, "=\"");
        JSString *v = JV_STR(r[1]);
        for (uint32_t i = 0; i < kz_str_len(v); i++) {
            uint32_t c = kz_str_at(v, i);
            if (c == '"') kz_sb_puts(&sb, "&quot;");
            else kz_sb_putc(&sb, c);
        }
        kz_sb_putc(&sb, '"');
    }
    kz_sb_putc(&sb, '>');
    kz_sb_put_str(&sb, JV_STR(r[0]));
    kz_sb_puts(&sb, "</");
    kz_sb_puts(&sb, html_methods[magic].tag);
    kz_sb_putc(&sb, '>');
    return kz_sb_finish(&sb);
}

/* ================================================================== the string iterator */

static JSValue proto_iterator(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOT(ctx, sv);
    sv = this_str(ctx, this_val, "[Symbol.iterator]");
    if (sv == JS_EXCEPTION) return sv;
    JSValue it = kz_new_object_class(ctx, KZ_CLASS_STRING_ITERATOR, kz_intr(ctx, KZ_I_STRING_ITERATOR_PROTO));
    if (it == JS_EXCEPTION) return it;
    JV_OBJ(it)->u.any.a = sv;
    JV_OBJ(it)->u.any.b = JV_MKINT(0);
    return it;
}

static JSValue string_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_STRING_ITERATOR)) return kz_throw_type(ctx, "next method called on an incompatible receiver");
    JSObject *it = JV_OBJ(this_val);
    JSValue sv = it->u.any.a;
    if (sv == JS_UNDEFINED) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    JSString *s = JV_STR(sv);
    uint32_t pos = (uint32_t)JV_INT(it->u.any.b);
    if (pos >= kz_str_len(s)) {
        it->u.any.a = JS_UNDEFINED;
        return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    }
    uint32_t u;
    uint32_t c = code_point_at(s, pos, &u);
    it->u.any.b = JV_MKINT((int32_t)(pos + u));
    KZ_ROOT(ctx, ch);
    ch = c < 256 ? ctx->rt->char_str[c] : kz_str_sub(ctx, sv, pos, pos + u);
    if (ch == JS_EXCEPTION) return ch;
    return kz_create_iter_result(ctx, ch, false);
}

/* ================================================================== tables */

static const KzFuncDef string_funcs[] = {
    KZ_FN("fromCharCode", string_from_char_code, 1),
    KZ_FN("fromCodePoint", string_from_code_point, 1),
    KZ_FN("raw", string_raw, 1),
};

static const KzFuncDef string_proto_funcs[] = {
    KZ_FN_MAGIC("at", proto_char_at, 1, 0),
    KZ_FN_MAGIC("charAt", proto_char_at, 1, 1),
    KZ_FN_MAGIC("charCodeAt", proto_char_at, 1, 2),
    KZ_FN_MAGIC("codePointAt", proto_char_at, 1, 3),
    KZ_FN("concat", proto_concat, 1),
    KZ_FN_MAGIC_ARGS("endsWith", proto_starts_with, 1, 2, 1),
    KZ_FN_MAGIC_ARGS("includes", proto_starts_with, 1, 2, 2),
    KZ_FN_ARGS("indexOf", proto_index_of, 1, 2),
    KZ_FN_MAGIC("isWellFormed", proto_is_well_formed, 0, 0),
    KZ_FN_ARGS("lastIndexOf", proto_last_index_of, 1, 2),
    KZ_FN("localeCompare", proto_locale_compare, 1),
    KZ_FN_MAGIC("match", proto_match, 1, 0),
    KZ_FN_MAGIC("matchAll", proto_match, 1, 1),
    KZ_FN_ARGS("normalize", proto_normalize, 0, 1),
    KZ_FN_MAGIC_ARGS("padEnd", proto_pad, 1, 2, 1),
    KZ_FN_MAGIC_ARGS("padStart", proto_pad, 1, 2, 0),
    KZ_FN("repeat", proto_repeat, 1),
    KZ_FN_MAGIC("replace", proto_replace, 2, 0),
    KZ_FN_MAGIC("replaceAll", proto_replace, 2, 1),
    KZ_FN_MAGIC("search", proto_match, 1, 2),
    KZ_FN_MAGIC("slice", proto_slice, 2, 0),
    KZ_FN("split", proto_split, 2),
    KZ_FN_MAGIC_ARGS("startsWith", proto_starts_with, 1, 2, 0),
    KZ_FN_MAGIC("substr", proto_slice, 2, 2),
    KZ_FN_MAGIC("substring", proto_slice, 2, 1),
    KZ_FN_MAGIC("toLocaleLowerCase", proto_case, 0, 2),
    KZ_FN_MAGIC("toLocaleUpperCase", proto_case, 0, 3),
    KZ_FN_MAGIC("toLowerCase", proto_case, 0, 0),
    KZ_FN("toString", proto_value_of, 0),
    KZ_FN_MAGIC("toUpperCase", proto_case, 0, 1),
    KZ_FN_MAGIC("toWellFormed", proto_is_well_formed, 0, 1),
    KZ_FN_MAGIC("trim", proto_trim, 0, 0),
    KZ_FN("valueOf", proto_value_of, 0),
    KZ_FN("[Symbol.iterator]", proto_iterator, 0),
    KZ_FN_MAGIC("anchor", proto_html, 1, 0),
    KZ_FN_MAGIC("big", proto_html, 0, 1),
    KZ_FN_MAGIC("blink", proto_html, 0, 2),
    KZ_FN_MAGIC("bold", proto_html, 0, 3),
    KZ_FN_MAGIC("fixed", proto_html, 0, 4),
    KZ_FN_MAGIC("fontcolor", proto_html, 1, 5),
    KZ_FN_MAGIC("fontsize", proto_html, 1, 6),
    KZ_FN_MAGIC("italics", proto_html, 0, 7),
    KZ_FN_MAGIC("link", proto_html, 1, 8),
    KZ_FN_MAGIC("small", proto_html, 0, 9),
    KZ_FN_MAGIC("strike", proto_html, 0, 10),
    KZ_FN_MAGIC("sub", proto_html, 0, 11),
    KZ_FN_MAGIC("sup", proto_html, 0, 12),
};

static const KzFuncDef string_iter_funcs[] = {
    KZ_FN("next", string_iter_next, 0),
};

int kz_init_string(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    kz_exotic[KZ_CLASS_STRING] = &string_exotic;
    KZ_ROOTS(ctx, r, 3);
    /* String.prototype is a String object for "" */
    r[0] = kz_new_object_class(ctx, KZ_CLASS_STRING, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (r[0] == JS_EXCEPTION) return -1;
    JV_OBJ(r[0])->u.prim = rt->atom[KZ_ATOM_empty];
    if (kz_obj_add_prop(ctx, JV_OBJ(r[0]), rt->atom[KZ_ATOM_length], 0, JV_MKINT(0))) return -1;
    ctx->intrinsic[KZ_I_STRING_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, string_ctor, "String", 1, 0, KZ_I_STRING_PROTO, KZ_I_STRING);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], string_funcs, KZ_COUNTOF(string_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], string_proto_funcs, KZ_COUNTOF(string_proto_funcs))) return -1;
    /* trimStart/trimLeft and trimEnd/trimRight are the same function objects */
    static const char *const pairs[2][2] = { { "trimStart", "trimLeft" }, { "trimEnd", "trimRight" } };
    for (int i = 0; i < 2; i++) {
        r[2] = kz_new_native_c(ctx, proto_trim, pairs[i][0], 0, i + 1);
        if (r[2] == JS_EXCEPTION) return -1;
        for (int j = 0; j < 2; j++) {
            JSValue k = kz_atom(ctx, pairs[i][j]);
            if (k == JS_EXCEPTION || kz_define_value(ctx, r[0], k, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
        }
    }
    /* %StringIteratorPrototype% */
    r[2] = kz_new_object_proto(ctx, kz_intr(ctx, KZ_I_ITERATOR_PROTO));
    if (r[2] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_STRING_ITERATOR_PROTO] = r[2];
    if (kz_define_functions(ctx, r[2], string_iter_funcs, KZ_COUNTOF(string_iter_funcs))) return -1;
    JSValue tag = kz_str_from_cstr(ctx, "String Iterator");
    if (tag == JS_EXCEPTION || kz_define_value(ctx, r[2], rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_String, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
