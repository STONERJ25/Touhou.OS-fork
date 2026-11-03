/*
 * realm.c: creating a realm (ECMA-262 9.3 CreateRealm, SetDefaultGlobalBindings) and the function properties of the
 * global object (19.2: eval, isFinite, isNaN, parseFloat, parseInt, the URI functions; Annex B escape/unescape).
 */
#include "jsint.h"

int kz_init_function_proto(JSContext *ctx);
int kz_init_function_kinds(JSContext *ctx);
int kz_init_generators(JSContext *ctx);

/* ------------------------------------------------------------------ the global functions */

static JSValue global_eval(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_STRING(argv[0])) return argv[0];
    KZ_ROOT(ctx, s);
    s = argv[0];
    size_t len;
    char *src = kz_str_to_utf8(ctx, s, &len);
    if (!src) return JS_EXCEPTION;
    JSValue r = kz_eval_script(ctx, src, len, "<eval>", 0x100, NULL, 0);    /* 0x100: indirect eval */
    kz_free(ctx->rt, src);
    return r;
}

static JSValue global_is_nan(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    double d;
    if (kz_to_number(ctx, argv[0], &d)) return JS_EXCEPTION;
    return JV_MKBOOL(d != d);
}

static JSValue global_is_finite(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    double d;
    if (kz_to_number(ctx, argv[0], &d)) return JS_EXCEPTION;
    return JV_MKBOOL(d == d && d != __builtin_inf() && d != -__builtin_inf());
}

static JSValue global_parse_float(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSValue s = kz_to_string(ctx, argv[0]);
    if (s == JS_EXCEPTION) return s;
    KZ_ROOT(ctx, rs);
    rs = s;
    JSString *str = kz_str_flat(ctx, JV_STR(rs));
    if (!str) return JS_EXCEPTION;
    uint32_t end;
    double d = kz_parse_float(str, 0, &end);
    return jv_number(d);
}

static bool is_ws_lt(uint32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

static JSValue global_parse_int(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, rs);
    rs = kz_to_string(ctx, argv[0]);
    if (rs == JS_EXCEPTION) return rs;
    int32_t radix;
    if (kz_to_int32(ctx, argv[1], &radix)) return JS_EXCEPTION;
    JSString *s = kz_str_flat(ctx, JV_STR(rs));
    if (!s) return JS_EXCEPTION;
    uint32_t n = kz_str_len(s), i = 0;
    while (i < n && is_ws_lt(kz_str_at(s, i))) i++;
    bool neg = false;
    if (i < n && (kz_str_at(s, i) == '+' || kz_str_at(s, i) == '-')) { neg = kz_str_at(s, i) == '-'; i++; }
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return KZ_NAN_BITS;
        if (radix != 16) strip = false;
    } else radix = 10;
    if (strip && i + 1 < n && kz_str_at(s, i) == '0' && (kz_str_at(s, i + 1) | 0x20) == 'x') { i += 2; radix = 16; }
    uint32_t start = i;
    while (i < n) {
        uint32_t c = kz_str_at(s, i), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') d = (c | 0x20) - 'a' + 10;
        else break;
        if (d >= (uint32_t)radix) break;
        i++;
    }
    if (i == start) return KZ_NAN_BITS;
    uint32_t len = i - start;
    double v;
    /* small values exactly in a register; decimal ones through the correctly rounded decimal reader */
    char small[64];
    char *digs = len < sizeof small ? small : kz_malloc(ctx, len + 1);
    if (!digs) return JS_EXCEPTION;
    for (uint32_t k = 0; k < len; k++) digs[k] = (char)kz_str_at(s, start + k);
    if (radix == 10) v = kz_strtod(digs, len);
    else v = kz_parse_radix_int(digs, len, radix);
    if (digs != small) kz_free(ctx->rt, digs);
    if (neg) v = -v;
    return jv_number(v);
}

/* ------------------------------------------------------------------ URI encoding (19.2.6) */

static const char uri_reserved[] = ";/?:@&=+$,";
static const char uri_unescaped_extra[] = "-_.!~*'()";

static bool in_set(uint32_t c, const char *set) { return c < 128 && c && strchr(set, (int)c); }
static bool is_alnum(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static JSValue uri_encode(JSContext *ctx, JSValue sv, bool component) {
    KZ_ROOT(ctx, rs);
    rs = kz_to_string(ctx, sv);
    if (rs == JS_EXCEPTION) return rs;
    JSString *s = kz_str_flat(ctx, JV_STR(rs));
    if (!s) return JS_EXCEPTION;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    uint32_t n = kz_str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        if (is_alnum(c) || in_set(c, uri_unescaped_extra) || (!component && (in_set(c, uri_reserved) || c == '#'))) {
            kz_sb_putc(&sb, c);
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) { kz_sb_free(&sb); return kz_throw_error(ctx, KZ_ERR_URI, "URI malformed"); }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n) { kz_sb_free(&sb); return kz_throw_error(ctx, KZ_ERR_URI, "URI malformed"); }
            uint32_t d = kz_str_at(s, i + 1);
            if (d < 0xDC00 || d > 0xDFFF) { kz_sb_free(&sb); return kz_throw_error(ctx, KZ_ERR_URI, "URI malformed"); }
            c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
            i++;
        }
        char u[4];
        int k = kz_utf8_encode(c, u);
        for (int j = 0; j < k; j++) {
            static const char hex[] = "0123456789ABCDEF";
            uint8_t byte = (uint8_t)u[j];
            kz_sb_putc(&sb, '%');
            kz_sb_putc(&sb, (uint8_t)hex[byte >> 4]);
            kz_sb_putc(&sb, (uint8_t)hex[byte & 15]);
        }
    }
    return kz_sb_finish(&sb);
}

static int hex_digit(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (int)((c | 0x20) - 'a' + 10);
    return -1;
}

static JSValue uri_decode(JSContext *ctx, JSValue sv, bool component) {
    KZ_ROOT(ctx, rs);
    rs = kz_to_string(ctx, sv);
    if (rs == JS_EXCEPTION) return rs;
    JSString *s = kz_str_flat(ctx, JV_STR(rs));
    if (!s) return JS_EXCEPTION;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    uint32_t n = kz_str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        if (c != '%') { kz_sb_putc(&sb, c); continue; }
        uint32_t start = i;
        if (i + 2 >= n) goto bad;
        int h1 = hex_digit(kz_str_at(s, i + 1)), h2 = hex_digit(kz_str_at(s, i + 2));
        if (h1 < 0 || h2 < 0) goto bad;
        uint32_t byte = (uint32_t)(h1 * 16 + h2);
        i += 2;
        if (byte < 0x80) {
            if (!component && (in_set(byte, uri_reserved) || byte == '#')) {
                for (uint32_t k = start; k <= i; k++) kz_sb_putc(&sb, kz_str_at(s, k));
            } else kz_sb_putc(&sb, byte);
            continue;
        }
        int extra;
        uint32_t cp, min;
        if ((byte & 0xE0) == 0xC0) { extra = 1; cp = byte & 0x1F; min = 0x80; }
        else if ((byte & 0xF0) == 0xE0) { extra = 2; cp = byte & 0x0F; min = 0x800; }
        else if ((byte & 0xF8) == 0xF0) { extra = 3; cp = byte & 0x07; min = 0x10000; }
        else goto bad;
        for (int k = 0; k < extra; k++) {
            /* the next "%XX" sits at i+1..i+3 */
            if (i + 3 >= n || kz_str_at(s, i + 1) != '%') goto bad;
            int a = hex_digit(kz_str_at(s, i + 2)), bb = hex_digit(kz_str_at(s, i + 3));
            if (a < 0 || bb < 0) goto bad;
            uint32_t cb = (uint32_t)(a * 16 + bb);
            if ((cb & 0xC0) != 0x80) goto bad;
            cp = (cp << 6) | (cb & 0x3F);
            i += 3;
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) goto bad;
        kz_sb_put_cp(&sb, cp);
    }
    return kz_sb_finish(&sb);
bad:
    kz_sb_free(&sb);
    return kz_throw_error(ctx, KZ_ERR_URI, "URI malformed");
}

static JSValue global_uri(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    switch (magic) {
    case 0: return uri_encode(ctx, argv[0], false);
    case 1: return uri_encode(ctx, argv[0], true);
    case 2: return uri_decode(ctx, argv[0], false);
    default: return uri_decode(ctx, argv[0], true);
    }
}

/* Annex B.2.1 */
static JSValue global_escape(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, rs);
    rs = kz_to_string(ctx, argv[0]);
    if (rs == JS_EXCEPTION) return rs;
    JSString *s = kz_str_flat(ctx, JV_STR(rs));
    if (!s) return JS_EXCEPTION;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < kz_str_len(s); i++) {
        uint32_t c = kz_str_at(s, i);
        if (is_alnum(c) || (c < 128 && strchr("@*_+-./", (int)c) && c)) kz_sb_putc(&sb, c);
        else if (c < 256) { kz_sb_putc(&sb, '%'); kz_sb_putc(&sb, (uint8_t)hex[c >> 4]); kz_sb_putc(&sb, (uint8_t)hex[c & 15]); }
        else {
            kz_sb_puts(&sb, "%u");
            for (int k = 12; k >= 0; k -= 4) kz_sb_putc(&sb, (uint8_t)hex[(c >> k) & 15]);
        }
    }
    return kz_sb_finish(&sb);
}

static JSValue global_unescape(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, rs);
    rs = kz_to_string(ctx, argv[0]);
    if (rs == JS_EXCEPTION) return rs;
    JSString *s = kz_str_flat(ctx, JV_STR(rs));
    if (!s) return JS_EXCEPTION;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    uint32_t n = kz_str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        if (c == '%') {
            if (i + 5 < n && kz_str_at(s, i + 1) == 'u') {
                int a = hex_digit(kz_str_at(s, i + 2)), bb = hex_digit(kz_str_at(s, i + 3));
                int cc = hex_digit(kz_str_at(s, i + 4)), d = hex_digit(kz_str_at(s, i + 5));
                if (a >= 0 && bb >= 0 && cc >= 0 && d >= 0) {
                    kz_sb_putc(&sb, (uint32_t)((a << 12) | (bb << 8) | (cc << 4) | d));
                    i += 5;
                    continue;
                }
            }
            if (i + 2 < n) {
                int a = hex_digit(kz_str_at(s, i + 1)), bb = hex_digit(kz_str_at(s, i + 2));
                if (a >= 0 && bb >= 0) {
                    kz_sb_putc(&sb, (uint32_t)(a * 16 + bb));
                    i += 2;
                    continue;
                }
            }
        }
        kz_sb_putc(&sb, c);
    }
    return kz_sb_finish(&sb);
}

static const KzFuncDef global_funcs[] = {
    KZ_FN("isNaN", global_is_nan, 1),
    KZ_FN("isFinite", global_is_finite, 1),
    KZ_FN("parseFloat", global_parse_float, 1),
    KZ_FN_ARGS("parseInt", global_parse_int, 2, 2),
    KZ_FN_MAGIC("encodeURI", global_uri, 1, 0),
    KZ_FN_MAGIC("encodeURIComponent", global_uri, 1, 1),
    KZ_FN_MAGIC("decodeURI", global_uri, 1, 2),
    KZ_FN_MAGIC("decodeURIComponent", global_uri, 1, 3),
    KZ_FN("escape", global_escape, 1),
    KZ_FN("unescape", global_unescape, 1),
};

int kz_init_global(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    JSValue g = ctx->global;
    if (kz_define_functions(ctx, g, global_funcs, KZ_COUNTOF(global_funcs))) return -1;
    /* Number.parseFloat and Number.parseInt are the same function objects as the globals */
    if (JV_IS_OBJECT(kz_intr(ctx, KZ_I_NUMBER))) {
        static const char *const shared[2] = { "parseFloat", "parseInt" };
        for (int i = 0; i < 2; i++) {
            JSValue k = kz_atom(ctx, shared[i]);
            if (k == JS_EXCEPTION) return -1;
            JSValue f = kz_get(ctx, g, k, g);
            if (f == JS_EXCEPTION || kz_define_value(ctx, kz_intr(ctx, KZ_I_NUMBER), k, f, KZ_PROP_W | KZ_PROP_C)) return -1;
        }
    }
    if (kz_define_value_atom(ctx, g, KZ_ATOM_NaN, KZ_NAN_BITS, 0)) return -1;
    if (kz_define_value_atom(ctx, g, KZ_ATOM_Infinity, jv_from_dbl(__builtin_inf()), 0)) return -1;
    if (kz_define_value_atom(ctx, g, KZ_ATOM_undefined, JS_UNDEFINED, 0)) return -1;
    if (kz_define_value_atom(ctx, g, KZ_ATOM_globalThis, g, KZ_PROP_W | KZ_PROP_C)) return -1;
    KZ_ROOT(ctx, ev);
    ev = kz_new_native(ctx, global_eval, rt->atom[KZ_ATOM_eval], 1, 0, 0, NULL);
    if (ev == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_EVAL] = ev;
    if (kz_define_value_atom(ctx, g, KZ_ATOM_eval, ev, KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}

/* ------------------------------------------------------------------ CreateRealm */

bool kz_realm_init(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    /* %Object.prototype%: an immutable-prototype object with a null prototype */
    JSShape *ns = kz_root_shape(ctx, JS_NULL);
    if (!ns) return false;
    JSObject *op = kz_obj_alloc(ctx, KZ_CLASS_OBJECT, ns, 0);
    if (!op) return false;
    op->h.flags |= OBJ_IMMUTABLE_PROTO;
    ctx->intrinsic[KZ_I_OBJECT_PROTO] = JV_MKOBJ(op);
    if (kz_init_function_proto(ctx)) return false;
    /* the shapes every function and plain object starts with */
    ctx->shape_object = kz_root_shape(ctx, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (!ctx->shape_object) return false;
    JSValue fkeys[2] = { rt->atom[KZ_ATOM_length], rt->atom[KZ_ATOM_name] };
    ctx->shape_func = kz_shape_build(ctx, kz_intr(ctx, KZ_I_FUNCTION_PROTO), fkeys, 2, KZ_PROP_C);
    if (!ctx->shape_func) return false;
    ctx->shape_func_proto = kz_shape_add(ctx, ctx->shape_func, rt->atom[KZ_ATOM_prototype], KZ_PROP_W);
    if (!ctx->shape_func_proto) return false;
    JSValue ikeys[2] = { rt->atom[KZ_ATOM_value], rt->atom[KZ_ATOM_done] };
    ctx->shape_iter_result = kz_shape_build(ctx, kz_intr(ctx, KZ_I_OBJECT_PROTO), ikeys, 2, KZ_PROP_WEC);
    if (!ctx->shape_iter_result) return false;
    /* %Array.prototype% is itself an array */
    JSObject *ap = kz_obj_alloc(ctx, KZ_CLASS_ARRAY, ctx->shape_object, 0);
    if (!ap) return false;
    ctx->intrinsic[KZ_I_ARRAY_PROTO] = JV_MKOBJ(ap);
    ctx->shape_array = kz_root_shape(ctx, JV_MKOBJ(ap));
    if (!ctx->shape_array) return false;
    /* the global object and the global lexical environment */
    r[0] = kz_new_object_class(ctx, KZ_CLASS_GLOBAL, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (r[0] == JS_EXCEPTION) return false;
    ctx->global = r[0];
    r[1] = kz_new_object_proto(ctx, JS_NULL);
    if (r[1] == JS_EXCEPTION) return false;
    ctx->global_lex = r[1];
    if (kz_init_object(ctx) || kz_init_function(ctx) || kz_init_error(ctx) || kz_init_symbol(ctx) ||
        kz_init_iterators(ctx) || kz_init_array(ctx) || kz_init_string(ctx) || kz_init_number(ctx) ||
        kz_init_math(ctx) || kz_init_json(ctx) || kz_init_function_kinds(ctx) || kz_init_generators(ctx) ||
        kz_init_regexp(ctx) || kz_init_date(ctx) || kz_init_map(ctx) || kz_init_promise(ctx) || kz_init_proxy(ctx) ||
        kz_init_typed(ctx) || kz_init_bigint(ctx) || kz_init_intl(ctx) || kz_init_global(ctx))
        return false;
    return true;
}
