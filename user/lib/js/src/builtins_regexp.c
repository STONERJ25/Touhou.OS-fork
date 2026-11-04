/*
 * builtins_regexp.c: the RegExp constructor and RegExp.prototype (ECMA-262 22.2.4-22.2.9), the %RegExpStringIterator%
 * (22.2.9), and Annex B's RegExp.prototype.compile.  The pattern engine itself is regexp_parse.c / regexp_gen.c /
 * regexp_exec.c; the String methods that dispatch to the Symbol.match family are in builtins_string.c.
 *
 * A RegExp instance is an object of class REGEXP whose u.regexp holds the compiled code (shared and reference
 * counted), the source and the flags as given.  Its only own property is lastIndex, which is always slot 0 (it is
 * non-configurable, so it can neither move nor be removed), which lets exec read and write it without a lookup.
 *
 * The Symbol.match/replace/search/split methods follow the specification step by step, including its calls to
 * Get(rx, "exec") and the observable property reads, so subclasses and instrumented objects behave as specified;
 * results of the built-in exec are recognised and not copied needlessly.
 */
#include "regexp.h"

/* ================================================================== helpers */

static inline bool is_regexp_obj(JSValue v) { return kz_is_class(v, KZ_CLASS_REGEXP) && JV_OBJ(v)->u.regexp.code; }

static JSValue to_flat_string(JSContext *ctx, JSValue v) {
    JSValue s = kz_to_string(ctx, v);
    if (s == JS_EXCEPTION) return s;
    return kz_str_flatten(ctx, s);
}

static bool flags_have(JSString *f, uint32_t c) {
    for (uint32_t i = 0; i < kz_str_len(f); i++)
        if (kz_str_at(f, i) == c) return true;
    return false;
}

/* AdvanceStringIndex (22.2.7.3) */
static uint64_t advance_index(JSString *s, uint64_t index, bool unicode) {
    uint32_t len = kz_str_len(s);
    if (!unicode || index + 1 >= len) return index + 1;
    uint32_t c = kz_str_at(s, (uint32_t)index);
    if (c >= 0xD800 && c <= 0xDBFF) {
        uint32_t d = kz_str_at(s, (uint32_t)index + 1);
        if (d >= 0xDC00 && d <= 0xDFFF) return index + 2;
    }
    return index + 1;
}

static JSValue num_value(double d) { return jv_number(d); }

static int set_last_index(JSContext *ctx, JSValue r, uint64_t v) {
    return kz_set_prop(ctx, r, ctx->rt->atom[KZ_ATOM_lastIndex], num_value((double)v), true) < 0 ? -1 : 0;
}

/* Set(R, "lastIndex", n) on the built-in instance: slot 0, unless it was made read-only */
static int set_last_index_fast(JSContext *ctx, JSObject *r, JSValue v) {
    if (!(r->shape->props[0].attrs & KZ_PROP_W)) {
        kz_throw_type(ctx, "Cannot assign to read only property 'lastIndex' of object '[object RegExp]'");
        return -1;
    }
    r->slots[0] = v;
    return 0;
}

/* "Invalid regular expression: /pattern/flags: reason" */
static JSValue throw_regexp_syntax(JSContext *ctx, JSString *pattern, JSString *flags, const char *reason) {
    char pat[300], fl[16];
    kz_str_to_utf8_buf(pattern, pat, sizeof pat);
    kz_str_to_utf8_buf(flags, fl, sizeof fl);
    return kz_throw_syntax(ctx, "Invalid regular expression: /%s/%s: %s", pat, fl, reason);
}

/* ================================================================== creation (RegExpAlloc, RegExpInitialize) */

static JSValue regexp_alloc(JSContext *ctx, JSValue new_target) {
    KZ_ROOT(ctx, o);
    o = kz_ordinary_create_from_ctor(ctx, new_target, KZ_I_REGEXP_PROTO, KZ_CLASS_REGEXP);
    if (o == JS_EXCEPTION) return o;
    if (kz_obj_add_prop(ctx, JV_OBJ(o), ctx->rt->atom[KZ_ATOM_lastIndex], KZ_PROP_W, JV_MKINT(0))) return JS_EXCEPTION;
    return o;
}

/* P and F are undefined or any values; sets the pattern of obj and resets lastIndex */
static JSValue regexp_initialize(JSContext *ctx, JSValue obj, JSValue pattern, JSValue flags) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 4);
    r[0] = obj;
    r[1] = pattern == JS_UNDEFINED ? rt->atom[KZ_ATOM_empty] : to_flat_string(ctx, pattern);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = flags == JS_UNDEFINED ? rt->atom[KZ_ATOM_empty] : to_flat_string(ctx, flags);
    if (r[2] == JS_EXCEPTION) return r[2];
    uint32_t f;
    if (kz_regexp_parse_flags(JV_STR(r[2]), &f)) {
        char fl[32];
        kz_str_to_utf8_buf(JV_STR(r[2]), fl, sizeof fl);
        return kz_throw_syntax(ctx, "Invalid flags supplied to RegExp constructor '%s'", fl);
    }
    char err[96];
    KzRegExpCode *code = kz_regexp_get(ctx, JV_STR(r[1]), f, err, sizeof err);
    if (!code) {
        if (!err[0]) return JS_EXCEPTION;
        return throw_regexp_syntax(ctx, JV_STR(r[1]), JV_STR(r[2]), err);
    }
    JSObject *o = JV_OBJ(r[0]);
    if (o->u.regexp.code) kz_regexp_free(rt, o->u.regexp.code);
    o->u.regexp.code = code;
    o->u.regexp.source = r[1];
    o->u.regexp.flags = r[2];
    if (set_last_index_fast(ctx, o, JV_MKINT(0))) return JS_EXCEPTION;
    return r[0];
}

JSValue kz_regexp_create(JSContext *ctx, JSValue pattern, JSValue flags) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = pattern;
    r[1] = flags;
    r[2] = regexp_alloc(ctx, kz_intr(ctx, KZ_I_REGEXP));
    if (r[2] == JS_EXCEPTION) return r[2];
    return regexp_initialize(ctx, r[2], r[0], r[1]);
}

static JSValue regexp_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    JSValue pattern = argv[0], flags = argv[1];
    KZ_ROOTS(ctx, r, 4);                            /* r[0] new target, r[1] P, r[2] F */
    int is_re = kz_is_regexp(ctx, pattern);
    if (is_re < 0) return JS_EXCEPTION;
    r[0] = rt->new_target;
    if (r[0] == JS_UNDEFINED) {
        r[0] = JV_MKOBJ(rt->cur_native);
        if (is_re && flags == JS_UNDEFINED) {
            r[3] = kz_get(ctx, pattern, rt->atom[KZ_ATOM_constructor], pattern);
            if (r[3] == JS_EXCEPTION) return r[3];
            if (r[3] == r[0]) return pattern;
        }
    }
    if (is_regexp_obj(pattern)) {
        r[1] = JV_OBJ(pattern)->u.regexp.source;
        r[2] = flags == JS_UNDEFINED ? JV_OBJ(pattern)->u.regexp.flags : flags;
    } else if (is_re) {
        r[1] = kz_get(ctx, pattern, rt->atom[KZ_ATOM_source], pattern);
        if (r[1] == JS_EXCEPTION) return r[1];
        if (flags == JS_UNDEFINED) {
            r[2] = kz_get(ctx, pattern, rt->atom[KZ_ATOM_flags], pattern);
            if (r[2] == JS_EXCEPTION) return r[2];
        } else r[2] = flags;
    } else {
        r[1] = pattern;
        r[2] = flags;
    }
    r[3] = regexp_alloc(ctx, r[0]);
    if (r[3] == JS_EXCEPTION) return r[3];
    return regexp_initialize(ctx, r[3], r[1], r[2]);
}

/* ================================================================== exec */

/* a null-prototype object { name: capture } for the named groups; indices: pairs instead of strings */
static JSValue make_groups(JSContext *ctx, const KzRegExpCode *code, JSValue sv, const int32_t *caps, bool indices) {
    KZ_ROOTS(ctx, r, 4);
    r[3] = sv;
    r[0] = kz_new_object_proto(ctx, JS_NULL);
    if (r[0] == JS_EXCEPTION) return r[0];
    for (uint32_t i = 1; i <= code->ncaps; i++) {
        if (!code->names[i].utf8) continue;
        r[1] = kz_atom_len(ctx, code->names[i].utf8, code->names[i].len);
        if (r[1] == JS_EXCEPTION) return r[1];
        bool matched = caps[2 * i] >= 0 && caps[2 * i + 1] >= 0;
        if (!matched && kz_shape_find(JV_OBJ(r[0])->shape, r[1]) >= 0) continue;     /* a duplicate name that already matched */
        if (!matched) r[2] = JS_UNDEFINED;
        else if (indices) {
            JSValue pair[2] = { JV_MKINT(caps[2 * i]), JV_MKINT(caps[2 * i + 1]) };
            r[2] = kz_new_array_from(ctx, 2, pair);
        } else r[2] = kz_str_sub(ctx, r[3], (uint32_t)caps[2 * i], (uint32_t)caps[2 * i + 1]);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_create_data_property(ctx, r[0], r[1], r[2])) return JS_EXCEPTION;
    }
    return r[0];
}

/* MakeMatchIndicesIndexPairArray */
static JSValue make_indices(JSContext *ctx, const KzRegExpCode *code, JSValue sv, const int32_t *caps) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_array(ctx, 0);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (kz_elems_reserve(ctx, JV_OBJ(r[0]), code->ncaps + 1)) return JS_EXCEPTION;
    for (uint32_t i = 0; i <= code->ncaps; i++) {
        if (caps[2 * i] < 0 || caps[2 * i + 1] < 0) r[1] = JS_UNDEFINED;
        else {
            JSValue pair[2] = { JV_MKINT(caps[2 * i]), JV_MKINT(caps[2 * i + 1]) };
            r[1] = kz_new_array_from(ctx, 2, pair);
            if (r[1] == JS_EXCEPTION) return r[1];
        }
        if (kz_array_push(ctx, JV_OBJ(r[0]), r[1])) return JS_EXCEPTION;
    }
    if (code->names) {
        r[2] = make_groups(ctx, code, sv, caps, true);
        if (r[2] == JS_EXCEPTION) return r[2];
    } else r[2] = JS_UNDEFINED;
    if (kz_obj_add_prop(ctx, JV_OBJ(r[0]), ctx->rt->atom[KZ_ATOM_groups], KZ_PROP_WEC, r[2])) return JS_EXCEPTION;
    return r[0];
}

/* the array RegExpBuiltinExec returns for a match (caps: the capture indices) */
static JSValue make_match_result(JSContext *ctx, const KzRegExpCode *code, JSValue sv, const int32_t *caps) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 4);
    r[3] = sv;
    r[0] = kz_new_array(ctx, 0);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSObject *a = JV_OBJ(r[0]);
    if (kz_elems_reserve(ctx, a, code->ncaps + 1)) return JS_EXCEPTION;
    for (uint32_t i = 0; i <= code->ncaps; i++) {
        if (caps[2 * i] < 0 || caps[2 * i + 1] < 0) r[1] = JS_UNDEFINED;
        else {
            r[1] = kz_str_sub(ctx, r[3], (uint32_t)caps[2 * i], (uint32_t)caps[2 * i + 1]);
            if (r[1] == JS_EXCEPTION) return r[1];
        }
        if (kz_array_push(ctx, a, r[1])) return JS_EXCEPTION;
    }
    if (kz_obj_add_prop(ctx, a, rt->atom[KZ_ATOM_index], KZ_PROP_WEC, JV_MKINT(caps[0]))) return JS_EXCEPTION;
    if (kz_obj_add_prop(ctx, a, rt->atom[KZ_ATOM_input], KZ_PROP_WEC, r[3])) return JS_EXCEPTION;
    if (code->names) {
        r[2] = make_groups(ctx, code, r[3], caps, false);
        if (r[2] == JS_EXCEPTION) return r[2];
    } else r[2] = JS_UNDEFINED;
    if (kz_obj_add_prop(ctx, a, rt->atom[KZ_ATOM_groups], KZ_PROP_WEC, r[2])) return JS_EXCEPTION;
    if (code->flags & KZ_RE_HASINDICES) {
        r[2] = make_indices(ctx, code, r[3], caps);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_obj_add_prop(ctx, a, rt->atom[KZ_ATOM_indices], KZ_PROP_WEC, r[2])) return JS_EXCEPTION;
    }
    return r[0];
}

/*
 * RegExpBuiltinExec (22.2.7.2).  lastIndex is converted first (user code can run there and even recompile the
 * object), then the compiled code is read; the matcher and the building of the result never run user code.
 */
static JSValue regexp_builtin_exec(JSContext *ctx, JSValue rv, JSValue sv) {
    JSObject *r = JV_OBJ(rv);
    JSString *s = kz_str_flat(ctx, JV_STR(sv));
    if (!s) return JS_EXCEPTION;
    uint64_t last;
    if (JV_IS_INT(r->slots[0]) && JV_INT(r->slots[0]) >= 0) last = (uint64_t)JV_INT(r->slots[0]);
    else if (kz_to_length(ctx, r->slots[0], &last)) return JS_EXCEPTION;
    KzRegExpCode *code = r->u.regexp.code;
    uint32_t nslots = 2 * (code->ncaps + 1);
    int32_t local[64];
    int32_t *caps = nslots <= 64 ? local : kz_malloc(ctx, nslots * sizeof(int32_t));
    if (!caps) return JS_EXCEPTION;
    bool global = (code->flags & KZ_RE_GLOBAL) != 0, sticky = (code->flags & KZ_RE_STICKY) != 0;
    JSValue result = JS_EXCEPTION;
    int rc;
    if (!global && !sticky) last = 0;
    if (last > kz_str_len(s)) rc = 0;
    else rc = kz_regexp_match(ctx, code, s, (uint32_t)last, sticky, caps);
    if (rc < 0) goto done;
    if (rc == 0) {
        if ((global || sticky) && set_last_index_fast(ctx, r, JV_MKINT(0))) goto done;
        result = JS_NULL;
        goto done;
    }
    if ((global || sticky) && set_last_index_fast(ctx, r, JV_MKINT(caps[1]))) goto done;
    result = make_match_result(ctx, code, sv, caps);
done:
    if (caps != local) kz_free(ctx->rt, caps);
    return result;
}

static JSValue regexp_proto_exec(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!is_regexp_obj(this_val)) return kz_throw_type(ctx, "RegExp.prototype.exec called on incompatible receiver");
    KZ_ROOT(ctx, s);
    s = kz_to_string(ctx, argv[0]);
    if (s == JS_EXCEPTION) return s;
    return regexp_builtin_exec(ctx, this_val, s);
}

/* RegExpExec (22.2.7.1): honours a user-defined exec; the result is an object or null */
static JSValue regexp_exec_abs(JSContext *ctx, JSValue r, JSValue s) {
    KZ_ROOT(ctx, exec);
    exec = kz_get(ctx, r, ctx->rt->atom[KZ_ATOM_exec], r);
    if (exec == JS_EXCEPTION) return exec;
    if (kz_is_callable(exec)) {
        JSObject *f = JV_OBJ(exec);
        if (kz_class(f) == KZ_CLASS_NATIVE && f->u.native.fn == regexp_proto_exec && is_regexp_obj(r))
            return regexp_builtin_exec(ctx, r, s);
        JSValue args[1] = { s };
        JSValue res = kz_call(ctx, exec, r, 1, args);
        if (res == JS_EXCEPTION) return res;
        if (!JV_IS_OBJECT(res) && res != JS_NULL) return kz_throw_type(ctx, "object or null expected from exec");
        return res;
    }
    if (!is_regexp_obj(r)) return kz_throw_type(ctx, "RegExp.prototype.exec called on incompatible receiver");
    return regexp_builtin_exec(ctx, r, s);
}

JSValue kz_regexp_exec(JSContext *ctx, JSValue r, JSValue s) { return regexp_exec_abs(ctx, r, s); }

static JSValue regexp_proto_test(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = regexp_exec_abs(ctx, this_val, r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    return JV_MKBOOL(r[1] != JS_NULL);
}

/* ================================================================== accessors and toString */

static JSValue regexp_flag_getter(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    if (!is_regexp_obj(this_val)) {
        if (this_val == kz_intr(ctx, KZ_I_REGEXP_PROTO)) return JS_UNDEFINED;
        return kz_throw_type(ctx, "RegExp flag getter called on incompatible receiver");
    }
    return JV_MKBOOL((JV_OBJ(this_val)->u.regexp.code->flags & (uint32_t)magic) != 0);
}

static JSValue regexp_get_flags(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    static const struct { int atom; char ch; } flags[] = {
        { KZ_ATOM_hasIndices, 'd' }, { KZ_ATOM_global, 'g' }, { KZ_ATOM_ignoreCase, 'i' }, { KZ_ATOM_multiline, 'm' },
        { KZ_ATOM_dotAll, 's' }, { KZ_ATOM_unicode, 'u' }, { KZ_ATOM_unicodeSets, 'v' }, { KZ_ATOM_sticky, 'y' },
    };
    char out[8];
    int n = 0;
    KZ_ROOT(ctx, v);
    for (size_t i = 0; i < KZ_COUNTOF(flags); i++) {
        v = kz_get_atom(ctx, this_val, flags[i].atom);
        if (v == JS_EXCEPTION) return v;
        if (kz_to_bool(v)) out[n++] = flags[i].ch;
    }
    return kz_str_from_latin1(ctx, (const uint8_t *)out, (uint32_t)n);
}

/* EscapeRegExpPattern: a source that reads back as the same pattern between slashes */
static JSValue escape_pattern(JSContext *ctx, JSValue src_obj) {
    JSString *s = kz_str_flat(ctx, JV_STR(JV_OBJ(src_obj)->u.regexp.source));
    if (!s) return JS_EXCEPTION;
    uint32_t n = kz_str_len(s);
    if (n == 0) return kz_str_from_cstr(ctx, "(?:)");
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    int depth = 0;
    bool nested = flags_have(JV_STR(JV_OBJ(src_obj)->u.regexp.flags), 'v');     /* only /v classes nest */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        if (c == '\\' && i + 1 < n) {
            uint32_t d = kz_str_at(s, i + 1);
            i++;
            if (d == '\n') kz_sb_puts(&sb, "\\n");
            else if (d == '\r') kz_sb_puts(&sb, "\\r");
            else if (d == 0x2028) kz_sb_puts(&sb, "\\u2028");
            else if (d == 0x2029) kz_sb_puts(&sb, "\\u2029");
            else { kz_sb_putc(&sb, '\\'); kz_sb_putc(&sb, d); }
            continue;
        }
        if (c == '[') { if (nested || depth == 0) depth++; }
        else if (c == ']' && depth > 0) depth--;
        if (c == '/' && depth == 0) kz_sb_puts(&sb, "\\/");
        else if (c == '\n') kz_sb_puts(&sb, "\\n");
        else if (c == '\r') kz_sb_puts(&sb, "\\r");
        else if (c == 0x2028) kz_sb_puts(&sb, "\\u2028");
        else if (c == 0x2029) kz_sb_puts(&sb, "\\u2029");
        else kz_sb_putc(&sb, c);
    }
    return kz_sb_finish(&sb);
}

static JSValue regexp_get_source(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    if (!is_regexp_obj(this_val)) {
        if (this_val == kz_intr(ctx, KZ_I_REGEXP_PROTO)) return kz_str_from_cstr(ctx, "(?:)");
        return kz_throw_type(ctx, "RegExp.prototype.source getter called on incompatible receiver");
    }
    return escape_pattern(ctx, this_val);
}

static JSValue regexp_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_get_atom(ctx, this_val, KZ_ATOM_source);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[0] = kz_to_string(ctx, r[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_get_atom(ctx, this_val, KZ_ATOM_flags);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[1] = kz_to_string(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_putc(&sb, '/');
    kz_sb_put_value(&sb, r[0]);
    kz_sb_putc(&sb, '/');
    kz_sb_put_value(&sb, r[1]);
    return kz_sb_finish(&sb);
}

/* Annex B.2.4.1 RegExp.prototype.compile(pattern, flags) */
static JSValue regexp_compile(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!is_regexp_obj(this_val)) return kz_throw_type(ctx, "RegExp.prototype.compile called on incompatible receiver");
    KZ_ROOTS(ctx, r, 2);
    if (is_regexp_obj(argv[0])) {
        if (argv[1] != JS_UNDEFINED) return kz_throw_type(ctx, "RegExp.prototype.compile: flags must be undefined when the pattern is a RegExp");
        r[0] = JV_OBJ(argv[0])->u.regexp.source;
        r[1] = JV_OBJ(argv[0])->u.regexp.flags;
    } else {
        r[0] = argv[0];
        r[1] = argv[1];
    }
    return regexp_initialize(ctx, this_val, r[0], r[1]);
}

/* ================================================================== Symbol.match / matchAll / search */

/* the (flat) string value of Get(result, "0") */
static JSValue match_string(JSContext *ctx, JSValue result) {
    KZ_ROOT(ctx, v);
    v = kz_get_index(ctx, result, 0);
    if (v == JS_EXCEPTION) return v;
    return to_flat_string(ctx, v);
}

static JSValue regexp_symbol_match(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 5);        /* r[0] S, r[1] flags, r[2] result, r[3] A, r[4] matchStr */
    r[0] = to_flat_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_get_atom(ctx, this_val, KZ_ATOM_flags);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[1] = to_flat_string(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return r[1];
    bool global = flags_have(JV_STR(r[1]), 'g');
    if (!global) return regexp_exec_abs(ctx, this_val, r[0]);
    bool unicode = flags_have(JV_STR(r[1]), 'u') || flags_have(JV_STR(r[1]), 'v');
    if (set_last_index(ctx, this_val, 0)) return JS_EXCEPTION;
    r[3] = kz_new_array(ctx, 0);
    if (r[3] == JS_EXCEPTION) return r[3];
    uint32_t n = 0;
    for (;;) {
        r[2] = regexp_exec_abs(ctx, this_val, r[0]);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (r[2] == JS_NULL) return n == 0 ? JS_NULL : r[3];
        r[4] = match_string(ctx, r[2]);
        if (r[4] == JS_EXCEPTION) return r[4];
        if (kz_array_push(ctx, JV_OBJ(r[3]), r[4])) return JS_EXCEPTION;
        if (kz_str_len(JV_STR(r[4])) == 0) {
            uint64_t this_index;
            JSValue li = kz_get_atom(ctx, this_val, KZ_ATOM_lastIndex);
            if (li == JS_EXCEPTION || kz_to_length(ctx, li, &this_index)) return JS_EXCEPTION;
            if (set_last_index(ctx, this_val, advance_index(JV_STR(r[0]), this_index, unicode))) return JS_EXCEPTION;
        }
        n++;
    }
}

/* the %RegExpStringIterator%: u.any.a = matcher, b = string, c = flags (int: 1 global, 2 unicode, 4 done) */
static JSValue regexp_symbol_match_all(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 5);        /* r[0] S, r[1] C, r[2] flags, r[3] matcher, r[4] iterator */
    r[0] = to_flat_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_species_constructor(ctx, this_val, kz_intr(ctx, KZ_I_REGEXP));
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = kz_get_atom(ctx, this_val, KZ_ATOM_flags);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[2] = to_flat_string(ctx, r[2]);
    if (r[2] == JS_EXCEPTION) return r[2];
    JSValue args[2] = { this_val, r[2] };
    r[3] = kz_construct(ctx, r[1], 2, args, r[1]);
    if (r[3] == JS_EXCEPTION) return r[3];
    JSValue li = kz_get_atom(ctx, this_val, KZ_ATOM_lastIndex);
    uint64_t last;
    if (li == JS_EXCEPTION || kz_to_length(ctx, li, &last)) return JS_EXCEPTION;
    if (set_last_index(ctx, r[3], last)) return JS_EXCEPTION;
    int fl = (flags_have(JV_STR(r[2]), 'g') ? 1 : 0) | ((flags_have(JV_STR(r[2]), 'u') || flags_have(JV_STR(r[2]), 'v')) ? 2 : 0);
    r[4] = kz_new_object_class(ctx, KZ_CLASS_REGEXP_STRING_ITERATOR, kz_intr(ctx, KZ_I_REGEXP_STRING_ITERATOR_PROTO));
    if (r[4] == JS_EXCEPTION) return r[4];
    JV_OBJ(r[4])->u.any.a = r[3];
    JV_OBJ(r[4])->u.any.b = r[0];
    JV_OBJ(r[4])->u.any.c = JV_MKINT(fl);
    (void)rt;
    return r[4];
}

static JSValue regexp_string_iter_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_REGEXP_STRING_ITERATOR))
        return kz_throw_type(ctx, "next method called on an incompatible receiver");
    JSObject *it = JV_OBJ(this_val);
    int fl = JV_INT(it->u.any.c);
    if (fl & 4) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    KZ_ROOTS(ctx, r, 3);        /* r[0] matcher, r[1] S, r[2] match */
    r[0] = it->u.any.a;
    r[1] = it->u.any.b;
    r[2] = regexp_exec_abs(ctx, r[0], r[1]);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (r[2] == JS_NULL) {
        it->u.any.c = JV_MKINT(fl | 4);
        return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    }
    if (!(fl & 1)) {
        it->u.any.c = JV_MKINT(fl | 4);
        return kz_create_iter_result(ctx, r[2], false);
    }
    JSValue ms = match_string(ctx, r[2]);
    if (ms == JS_EXCEPTION) return ms;
    if (kz_str_len(JV_STR(ms)) == 0) {
        uint64_t this_index;
        JSValue li = kz_get_atom(ctx, r[0], KZ_ATOM_lastIndex);
        if (li == JS_EXCEPTION || kz_to_length(ctx, li, &this_index)) return JS_EXCEPTION;
        JSString *s = kz_str_flat(ctx, JV_STR(r[1]));
        if (!s) return JS_EXCEPTION;
        if (set_last_index(ctx, r[0], advance_index(s, this_index, (fl & 2) != 0))) return JS_EXCEPTION;
    }
    return kz_create_iter_result(ctx, r[2], false);
}

static JSValue regexp_symbol_search(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 4);        /* r[0] S, r[1] previous, r[2] result, r[3] current */
    r[0] = to_flat_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_get_atom(ctx, this_val, KZ_ATOM_lastIndex);
    if (r[1] == JS_EXCEPTION) return r[1];
    int same = kz_same_value(ctx, r[1], JV_MKINT(0));
    if (same < 0) return JS_EXCEPTION;
    if (!same && kz_set_prop(ctx, this_val, ctx->rt->atom[KZ_ATOM_lastIndex], JV_MKINT(0), true) < 0) return JS_EXCEPTION;
    r[2] = regexp_exec_abs(ctx, this_val, r[0]);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[3] = kz_get_atom(ctx, this_val, KZ_ATOM_lastIndex);
    if (r[3] == JS_EXCEPTION) return r[3];
    same = kz_same_value(ctx, r[3], r[1]);
    if (same < 0) return JS_EXCEPTION;
    if (!same && kz_set_prop(ctx, this_val, ctx->rt->atom[KZ_ATOM_lastIndex], r[1], true) < 0) return JS_EXCEPTION;
    if (r[2] == JS_NULL) return JV_MKINT(-1);
    return kz_get_atom(ctx, r[2], KZ_ATOM_index);
}

/* ================================================================== Symbol.replace */

static JSValue regexp_symbol_replace(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 8);        /* r[0] S, r[1] replaceValue, r[2] flags, r[3] result, r[4] matched, r[5] named, r[6] cap, r[7] tmp */
    r[0] = to_flat_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    uint32_t length_s = kz_str_len(JV_STR(r[0]));
    bool functional = kz_is_callable(argv[1]);
    r[1] = argv[1];
    if (!functional) {
        r[1] = to_flat_string(ctx, argv[1]);
        if (r[1] == JS_EXCEPTION) return r[1];
    }
    r[2] = kz_get_atom(ctx, this_val, KZ_ATOM_flags);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[2] = to_flat_string(ctx, r[2]);
    if (r[2] == JS_EXCEPTION) return r[2];
    bool global = flags_have(JV_STR(r[2]), 'g');
    bool unicode = flags_have(JV_STR(r[2]), 'u') || flags_have(JV_STR(r[2]), 'v');
    if (global && set_last_index(ctx, this_val, 0)) return JS_EXCEPTION;
    KZ_ROOT_VEC(ctx, results);
    for (;;) {
        r[3] = regexp_exec_abs(ctx, this_val, r[0]);
        if (r[3] == JS_EXCEPTION) return r[3];
        if (r[3] == JS_NULL) break;
        if (kz_vec_push(ctx, &results, r[3])) return JS_EXCEPTION;
        if (!global) break;
        r[4] = match_string(ctx, r[3]);
        if (r[4] == JS_EXCEPTION) return r[4];
        if (kz_str_len(JV_STR(r[4])) == 0) {
            uint64_t this_index;
            JSValue li = kz_get_atom(ctx, this_val, KZ_ATOM_lastIndex);
            if (li == JS_EXCEPTION || kz_to_length(ctx, li, &this_index)) return JS_EXCEPTION;
            if (set_last_index(ctx, this_val, advance_index(JV_STR(r[0]), this_index, unicode))) return JS_EXCEPTION;
        }
    }
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    uint32_t next_pos = 0;
    for (uint32_t k = 0; k < results.len; k++) {
        r[3] = results.v[k];
        uint64_t result_len;
        if (kz_length_of(ctx, r[3], &result_len)) goto fail;
        uint64_t ncap = result_len > 0 ? result_len - 1 : 0;
        r[4] = match_string(ctx, r[3]);
        if (r[4] == JS_EXCEPTION) goto fail;
        uint32_t match_len = kz_str_len(JV_STR(r[4]));
        r[7] = kz_get_atom(ctx, r[3], KZ_ATOM_index);
        if (r[7] == JS_EXCEPTION) goto fail;
        double posd;
        if (kz_to_integer_or_inf(ctx, r[7], &posd)) goto fail;
        uint32_t position = posd < 0 ? 0 : posd > (double)length_s ? length_s : (uint32_t)posd;
        KZ_ROOT_VEC(ctx, caps);
        for (uint64_t n = 1; n <= ncap; n++) {
            r[6] = n <= KZ_MAX_INDEX ? kz_get_index(ctx, r[3], (uint32_t)n) : JS_UNDEFINED;
            if (r[6] == JS_EXCEPTION) goto fail;
            if (r[6] != JS_UNDEFINED) {
                r[6] = kz_to_string(ctx, r[6]);
                if (r[6] == JS_EXCEPTION) goto fail;
            }
            if (kz_vec_push(ctx, &caps, r[6])) goto fail;
        }
        r[5] = kz_get_atom(ctx, r[3], KZ_ATOM_groups);
        if (r[5] == JS_EXCEPTION) goto fail;
        JSValue replacement;
        if (functional) {
            KZ_ROOT_VEC(ctx, args);
            if (kz_vec_push(ctx, &args, r[4])) goto fail;
            for (uint32_t i = 0; i < caps.len; i++)
                if (kz_vec_push(ctx, &args, caps.v[i])) goto fail;
            if (kz_vec_push(ctx, &args, jv_number(position)) || kz_vec_push(ctx, &args, r[0])) goto fail;
            if (r[5] != JS_UNDEFINED && kz_vec_push(ctx, &args, r[5])) goto fail;
            r[7] = kz_call(ctx, r[1], JS_UNDEFINED, (int)args.len, args.v);
            if (r[7] == JS_EXCEPTION) goto fail;
            replacement = kz_to_string(ctx, r[7]);
        } else {
            if (r[5] != JS_UNDEFINED) {
                r[5] = kz_to_object(ctx, r[5]);
                if (r[5] == JS_EXCEPTION) goto fail;
            }
            replacement = kz_get_substitution(ctx, r[4], r[0], position, caps.v, caps.len, r[5], r[1]);
        }
        if (replacement == JS_EXCEPTION) goto fail;
        r[7] = replacement;
        if (position >= next_pos) {
            kz_sb_put_substr(&sb, JV_STR(r[0]), next_pos, position);
            kz_sb_put_value(&sb, r[7]);
            next_pos = position + match_len;
        }
    }
    if (next_pos < length_s) kz_sb_put_substr(&sb, JV_STR(r[0]), next_pos, length_s);
    (void)rt;
    return kz_sb_finish(&sb);
fail:
    kz_sb_free(&sb);
    return JS_EXCEPTION;
}

/* ================================================================== Symbol.split */

static JSValue regexp_symbol_split(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_not_object(ctx, this_val);
    KZ_ROOTS(ctx, r, 7);    /* r[0] S, r[1] C, r[2] flags, r[3] splitter, r[4] A, r[5] z/T, r[6] tmp */
    r[0] = to_flat_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_species_constructor(ctx, this_val, kz_intr(ctx, KZ_I_REGEXP));
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = kz_get_atom(ctx, this_val, KZ_ATOM_flags);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[2] = to_flat_string(ctx, r[2]);
    if (r[2] == JS_EXCEPTION) return r[2];
    bool unicode = flags_have(JV_STR(r[2]), 'u') || flags_have(JV_STR(r[2]), 'v');
    if (!flags_have(JV_STR(r[2]), 'y')) {
        r[2] = kz_str_concat(ctx, r[2], kz_str_from_char(ctx, 'y'));
        if (r[2] == JS_EXCEPTION) return r[2];
        r[2] = kz_str_flatten(ctx, r[2]);
        if (r[2] == JS_EXCEPTION) return r[2];
    }
    JSValue cargs[2] = { this_val, r[2] };
    r[3] = kz_construct(ctx, r[1], 2, cargs, r[1]);
    if (r[3] == JS_EXCEPTION) return r[3];
    r[4] = kz_new_array(ctx, 0);
    if (r[4] == JS_EXCEPTION) return r[4];
    JSObject *a = JV_OBJ(r[4]);
    uint32_t lim = 0xFFFFFFFFu;
    if (argv[1] != JS_UNDEFINED && kz_to_uint32(ctx, argv[1], &lim)) return JS_EXCEPTION;
    if (lim == 0) return r[4];
    uint32_t size = kz_str_len(JV_STR(r[0]));
    if (size == 0) {
        r[5] = regexp_exec_abs(ctx, r[3], r[0]);
        if (r[5] == JS_EXCEPTION) return r[5];
        if (r[5] != JS_NULL) return r[4];
        if (kz_array_push(ctx, a, r[0])) return JS_EXCEPTION;
        return r[4];
    }
    uint64_t p = 0, q = 0;
    while (q < size) {
        if (set_last_index(ctx, r[3], q)) return JS_EXCEPTION;
        r[5] = regexp_exec_abs(ctx, r[3], r[0]);
        if (r[5] == JS_EXCEPTION) return r[5];
        if (r[5] == JS_NULL) { q = advance_index(JV_STR(r[0]), q, unicode); continue; }
        uint64_t e;
        JSValue li = kz_get_atom(ctx, r[3], KZ_ATOM_lastIndex);
        if (li == JS_EXCEPTION || kz_to_length(ctx, li, &e)) return JS_EXCEPTION;
        if (e > size) e = size;
        if (e == p) { q = advance_index(JV_STR(r[0]), q, unicode); continue; }
        r[6] = kz_str_sub(ctx, r[0], (uint32_t)p, (uint32_t)q);
        if (r[6] == JS_EXCEPTION || kz_array_push(ctx, a, r[6])) return JS_EXCEPTION;
        if (a->u.array.length == lim) return r[4];
        p = e;
        uint64_t ncap;
        if (kz_length_of(ctx, r[5], &ncap)) return JS_EXCEPTION;
        ncap = ncap > 0 ? ncap - 1 : 0;
        for (uint64_t i = 1; i <= ncap; i++) {
            r[6] = i <= KZ_MAX_INDEX ? kz_get_index(ctx, r[5], (uint32_t)i) : JS_UNDEFINED;
            if (r[6] == JS_EXCEPTION || kz_array_push(ctx, a, r[6])) return JS_EXCEPTION;
            if (a->u.array.length == lim) return r[4];
        }
        q = p;
    }
    r[6] = kz_str_sub(ctx, r[0], (uint32_t)p, size);
    if (r[6] == JS_EXCEPTION || kz_array_push(ctx, a, r[6])) return JS_EXCEPTION;
    (void)rt;
    return r[4];
}

/* ================================================================== RegExp.escape (ES2025) */

static JSValue regexp_escape(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_STRING(argv[0])) return kz_throw_type(ctx, "RegExp.escape requires a string");
    KZ_ROOT(ctx, sv);
    sv = kz_str_flatten(ctx, argv[0]);
    if (sv == JS_EXCEPTION) return sv;
    JSString *s = JV_STR(sv);
    uint32_t n = kz_str_len(s);
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        uint32_t cp = c;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && kz_str_at(s, i + 1) >= 0xDC00 && kz_str_at(s, i + 1) <= 0xDFFF) {
            cp = 0x10000 + ((c - 0xD800) << 10) + (kz_str_at(s, i + 1) - 0xDC00);
            i++;
        }
        if (i == 0 && cp < 128 && ((cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z'))) {
            kz_sb_puts(&sb, "\\x");
            kz_sb_putc(&sb, (uint8_t)hex[cp >> 4]);
            kz_sb_putc(&sb, (uint8_t)hex[cp & 15]);
            continue;
        }
        if (cp < 128 && cp && strchr("^$\\.*+?()[]{}|/", (int)cp)) { kz_sb_putc(&sb, '\\'); kz_sb_putc(&sb, cp); continue; }
        switch (cp) {
        case 9: kz_sb_puts(&sb, "\\t"); continue;
        case 10: kz_sb_puts(&sb, "\\n"); continue;
        case 11: kz_sb_puts(&sb, "\\v"); continue;
        case 12: kz_sb_puts(&sb, "\\f"); continue;
        case 13: kz_sb_puts(&sb, "\\r"); continue;
        default: break;
        }
        bool other = cp < 128 && cp && strchr(",-=<>#&!%:;@~'`\"", (int)cp);
        if (other || kz_unicode_is_space(cp) || cp == 0x2028 || cp == 0x2029 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            if (cp <= 0xFF) {
                kz_sb_puts(&sb, "\\x");
                kz_sb_putc(&sb, (uint8_t)hex[cp >> 4]);
                kz_sb_putc(&sb, (uint8_t)hex[cp & 15]);
            } else {
                uint32_t units[2], nu = 0;
                if (cp > 0xFFFF) { units[nu++] = 0xD800 + ((cp - 0x10000) >> 10); units[nu++] = 0xDC00 + ((cp - 0x10000) & 0x3FF); }
                else units[nu++] = cp;
                for (uint32_t k = 0; k < nu; k++) {
                    kz_sb_puts(&sb, "\\u");
                    for (int sh = 12; sh >= 0; sh -= 4) kz_sb_putc(&sb, (uint8_t)hex[(units[k] >> sh) & 15]);
                }
            }
            continue;
        }
        kz_sb_put_cp(&sb, cp);
    }
    return kz_sb_finish(&sb);
}

/* ================================================================== setup */

static const KzFuncDef regexp_funcs[] = {
    KZ_FN("escape", regexp_escape, 1),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef regexp_proto_funcs[] = {
    KZ_FN("exec", regexp_proto_exec, 1),
    KZ_FN("test", regexp_proto_test, 1),
    KZ_FN("toString", regexp_to_string, 0),
    KZ_FN("compile", regexp_compile, 2),
    KZ_GETTER_MAGIC("dotAll", regexp_flag_getter, KZ_RE_DOTALL),
    KZ_GETTER("flags", regexp_get_flags),
    KZ_GETTER_MAGIC("global", regexp_flag_getter, KZ_RE_GLOBAL),
    KZ_GETTER_MAGIC("hasIndices", regexp_flag_getter, KZ_RE_HASINDICES),
    KZ_GETTER_MAGIC("ignoreCase", regexp_flag_getter, KZ_RE_IGNORECASE),
    KZ_GETTER_MAGIC("multiline", regexp_flag_getter, KZ_RE_MULTILINE),
    KZ_GETTER("source", regexp_get_source),
    KZ_GETTER_MAGIC("sticky", regexp_flag_getter, KZ_RE_STICKY),
    KZ_GETTER_MAGIC("unicode", regexp_flag_getter, KZ_RE_UNICODE),
    KZ_GETTER_MAGIC("unicodeSets", regexp_flag_getter, KZ_RE_UNICODESETS),
    KZ_FN("[Symbol.match]", regexp_symbol_match, 1),
    KZ_FN("[Symbol.matchAll]", regexp_symbol_match_all, 1),
    KZ_FN("[Symbol.replace]", regexp_symbol_replace, 2),
    KZ_FN("[Symbol.search]", regexp_symbol_search, 1),
    KZ_FN("[Symbol.split]", regexp_symbol_split, 2),
};

static const KzFuncDef regexp_iter_funcs[] = { KZ_FN("next", regexp_string_iter_next, 0) };

int kz_init_regexp(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);                      /* %RegExp.prototype% is an ordinary object */
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_REGEXP_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, regexp_ctor, "RegExp", 2, 0, KZ_I_REGEXP_PROTO, KZ_I_REGEXP);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], regexp_funcs, KZ_COUNTOF(regexp_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], regexp_proto_funcs, KZ_COUNTOF(regexp_proto_funcs))) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_RegExp, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    /* %RegExpStringIteratorPrototype% */
    r[2] = kz_new_object_proto(ctx, kz_intr(ctx, KZ_I_ITERATOR_PROTO));
    if (r[2] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_REGEXP_STRING_ITERATOR_PROTO] = r[2];
    if (kz_define_functions(ctx, r[2], regexp_iter_funcs, KZ_COUNTOF(regexp_iter_funcs))) return -1;
    r[1] = kz_atom(ctx, "RegExp String Iterator");
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[2], rt->sym[KZ_SYM_toStringTag], r[1], KZ_PROP_C)) return -1;
    return 0;
}
