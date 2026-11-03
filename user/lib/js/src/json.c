/*
 * json.c: JSON.parse and JSON.stringify (ECMA-262 25.5).
 *
 * The parser reads the flat string directly (Latin-1 or UTF-16 units) and builds the values as it goes; strings
 * without escapes become substrings.  The serializer writes into one string builder; a property whose value would
 * serialize to undefined is detected before its key is written.
 */
#include "jsint.h"

/* ================================================================== parse */

typedef struct JParser {
    JSContext *ctx;
    JSString *s;                /* flat; re-read through sv after any allocation */
    JSValue *sv;                /* the rooted source string */
    uint32_t pos, len;
} JParser;

static uint32_t jp_at(JParser *p, uint32_t i) { return kz_str_at(JV_STR(*p->sv), i); }

static void jp_ws(JParser *p) {
    while (p->pos < p->len) {
        uint32_t c = jp_at(p, p->pos);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static JSValue jp_error(JParser *p, const char *what) {
    if (p->pos >= p->len) return kz_throw_syntax(p->ctx, "Unexpected end of JSON input");
    uint32_t c = jp_at(p, p->pos);
    if (what) return kz_throw_syntax(p->ctx, "%s in JSON at position %u", what, (unsigned)p->pos);
    if (c >= 0x20 && c < 0x7F) return kz_throw_syntax(p->ctx, "Unexpected token '%c' in JSON at position %u", (int)c, (unsigned)p->pos);
    return kz_throw_syntax(p->ctx, "Unexpected character in JSON at position %u", (unsigned)p->pos);
}

static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static JSValue jp_string(JParser *p) {
    /* at the opening quote */
    uint32_t start = ++p->pos;
    for (;;) {
        if (p->pos >= p->len) return jp_error(p, "Unterminated string");
        uint32_t c = jp_at(p, p->pos);
        if (c == '"') {
            p->pos++;
            return kz_str_sub(p->ctx, *p->sv, start, p->pos - 1);
        }
        if (c < 0x20) return jp_error(p, "Bad control character in string literal");
        if (c == '\\') break;
        p->pos++;
    }
    KzStrBuf sb;
    kz_sb_init(&sb, p->ctx);
    kz_sb_put_substr(&sb, JV_STR(*p->sv), start, p->pos);
    for (;;) {
        if (p->pos >= p->len) { kz_sb_free(&sb); return jp_error(p, "Unterminated string"); }
        uint32_t c = jp_at(p, p->pos);
        if (c == '"') { p->pos++; return kz_sb_finish(&sb); }
        if (c < 0x20) { kz_sb_free(&sb); return jp_error(p, "Bad control character in string literal"); }
        if (c != '\\') { kz_sb_putc(&sb, c); p->pos++; continue; }
        if (++p->pos >= p->len) { kz_sb_free(&sb); return jp_error(p, "Unterminated string"); }
        uint32_t e = jp_at(p, p->pos++);
        switch (e) {
        case '"': kz_sb_putc(&sb, '"'); break;
        case '\\': kz_sb_putc(&sb, '\\'); break;
        case '/': kz_sb_putc(&sb, '/'); break;
        case 'b': kz_sb_putc(&sb, 8); break;
        case 'f': kz_sb_putc(&sb, 12); break;
        case 'n': kz_sb_putc(&sb, 10); break;
        case 'r': kz_sb_putc(&sb, 13); break;
        case 't': kz_sb_putc(&sb, 9); break;
        case 'u': {
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int h = p->pos < p->len ? hexval(jp_at(p, p->pos)) : -1;
                if (h < 0) { kz_sb_free(&sb); return jp_error(p, "Bad Unicode escape"); }
                v = v * 16 + (uint32_t)h;
                p->pos++;
            }
            kz_sb_putc(&sb, v);
            break;
        }
        default:
            p->pos--;
            kz_sb_free(&sb);
            return jp_error(p, "Bad escaped character");
        }
    }
}

static JSValue jp_number(JParser *p) {
    uint32_t start = p->pos;
    if (jp_at(p, p->pos) == '-') p->pos++;
    if (p->pos >= p->len) return jp_error(p, "No number after minus sign");
    uint32_t c = jp_at(p, p->pos);
    if (c == '0') p->pos++;
    else if (c >= '1' && c <= '9') while (p->pos < p->len && (c = jp_at(p, p->pos)) >= '0' && c <= '9') p->pos++;
    else return jp_error(p, "No number after minus sign");
    bool frac = false;
    if (p->pos < p->len && jp_at(p, p->pos) == '.') {
        p->pos++;
        frac = true;
        uint32_t d = p->pos;
        while (p->pos < p->len && (c = jp_at(p, p->pos)) >= '0' && c <= '9') p->pos++;
        if (p->pos == d) return jp_error(p, "Unterminated fractional number");
    }
    if (p->pos < p->len && (jp_at(p, p->pos) | 32) == 'e') {
        p->pos++;
        frac = true;
        if (p->pos < p->len && (jp_at(p, p->pos) == '+' || jp_at(p, p->pos) == '-')) p->pos++;
        uint32_t d = p->pos;
        while (p->pos < p->len && (c = jp_at(p, p->pos)) >= '0' && c <= '9') p->pos++;
        if (p->pos == d) return jp_error(p, "Exponent part is missing a number");
    }
    uint32_t n = p->pos - start;
    /* small integers directly */
    if (!frac && n <= 9) {
        int32_t v = 0;
        bool neg = jp_at(p, start) == '-';
        for (uint32_t i = start + (neg ? 1 : 0); i < p->pos; i++) v = v * 10 + (int32_t)(jp_at(p, i) - '0');
        if (neg && v == 0) return jv_from_dbl(-0.0);
        return JV_MKINT(neg ? -v : v);
    }
    char small[64];
    char *buf = n < sizeof small ? small : kz_malloc(p->ctx, n + 1);
    if (!buf) return JS_EXCEPTION;
    for (uint32_t i = 0; i < n; i++) buf[i] = (char)jp_at(p, start + i);
    buf[n] = 0;
    bool neg = buf[0] == '-';
    double d = kz_strtod(buf + (neg ? 1 : 0), n - (neg ? 1 : 0));
    if (buf != small) kz_free(p->ctx->rt, buf);
    return jv_number(neg ? -d : d);
}

static JSValue jp_value(JParser *p);

static JSValue jp_array(JParser *p) {
    JSContext *ctx = p->ctx;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_array(ctx, 0);
    if (r[0] == JS_EXCEPTION) return r[0];
    p->pos++;
    jp_ws(p);
    if (p->pos < p->len && jp_at(p, p->pos) == ']') { p->pos++; return r[0]; }
    for (;;) {
        r[1] = jp_value(p);
        if (r[1] == JS_EXCEPTION) return r[1];
        if (kz_array_push(ctx, JV_OBJ(r[0]), r[1])) return JS_EXCEPTION;
        jp_ws(p);
        if (p->pos >= p->len) return jp_error(p, NULL);
        uint32_t c = jp_at(p, p->pos);
        if (c == ']') { p->pos++; return r[0]; }
        if (c != ',') return jp_error(p, NULL);
        p->pos++;
    }
}

static JSValue jp_object(JParser *p) {
    JSContext *ctx = p->ctx;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    p->pos++;
    jp_ws(p);
    if (p->pos < p->len && jp_at(p, p->pos) == '}') { p->pos++; return r[0]; }
    for (;;) {
        jp_ws(p);
        if (p->pos >= p->len || jp_at(p, p->pos) != '"') return jp_error(p, NULL);
        r[1] = jp_string(p);
        if (r[1] == JS_EXCEPTION) return r[1];
        r[1] = kz_key_from_value(ctx, r[1]);
        if (r[1] == JS_EXCEPTION) return r[1];
        jp_ws(p);
        if (p->pos >= p->len || jp_at(p, p->pos) != ':') return jp_error(p, NULL);
        p->pos++;
        r[2] = jp_value(p);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (kz_create_data_property(ctx, r[0], r[1], r[2])) return JS_EXCEPTION;
        jp_ws(p);
        if (p->pos >= p->len) return jp_error(p, NULL);
        uint32_t c = jp_at(p, p->pos);
        if (c == '}') { p->pos++; return r[0]; }
        if (c != ',') return jp_error(p, NULL);
        p->pos++;
    }
}

static bool jp_word(JParser *p, const char *w) {
    uint32_t n = (uint32_t)strlen(w);
    if (p->pos + n > p->len) return false;
    for (uint32_t i = 0; i < n; i++)
        if (jp_at(p, p->pos + i) != (uint8_t)w[i]) return false;
    p->pos += n;
    return true;
}

static JSValue jp_value(JParser *p) {
    if (kz_check_stack(p->ctx)) return JS_EXCEPTION;
    jp_ws(p);
    if (p->pos >= p->len) return jp_error(p, NULL);
    uint32_t c = jp_at(p, p->pos);
    switch (c) {
    case '{': return jp_object(p);
    case '[': return jp_array(p);
    case '"': return jp_string(p);
    case 't': if (jp_word(p, "true")) return JS_TRUE; break;
    case 'f': if (jp_word(p, "false")) return JS_FALSE; break;
    case 'n': if (jp_word(p, "null")) return JS_NULL; break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p);
        break;
    }
    return jp_error(p, NULL);
}

/* InternalizeJSONProperty */
static JSValue internalize(JSContext *ctx, JSValue holder, JSValue name, JSValue reviver) {
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_get(ctx, holder, name, holder);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (JV_IS_OBJECT(r[0])) {
        int isa = kz_is_array(ctx, r[0]);
        if (isa < 0) return JS_EXCEPTION;
        KZ_ROOT_VEC(ctx, keys);
        if (isa) {
            uint64_t len;
            if (kz_length_of(ctx, r[0], &len)) return JS_EXCEPTION;
            for (uint64_t i = 0; i < len; i++) {
                r[1] = i <= KZ_MAX_INDEX ? KZ_KEY_FROM_INDEX((uint32_t)i) : kz_key_from_value(ctx, jv_number((double)i));
                if (r[1] == JS_EXCEPTION || kz_vec_push(ctx, &keys, r[1])) return JS_EXCEPTION;
            }
        } else {
            JSObject *o = JV_OBJ(r[0]);
            KZ_ROOT_VEC(ctx, all);
            if (kz_own_keys(ctx, o, &all, KZ_KEYS_STRINGS)) return JS_EXCEPTION;
            for (uint32_t i = 0; i < all.len; i++) {
                KZ_ROOT_DESC(ctx, d);
                int h = kz_get_own_property(ctx, o, all.v[i], &d);
                if (h < 0) return JS_EXCEPTION;
                if (h && (d.flags & KZ_PROP_E) && kz_vec_push(ctx, &keys, all.v[i])) return JS_EXCEPTION;
            }
        }
        for (uint32_t i = 0; i < keys.len; i++) {
            r[3] = internalize(ctx, r[0], keys.v[i], reviver);
            if (r[3] == JS_EXCEPTION) return r[3];
            if (r[3] == JS_UNDEFINED) {
                if (kz_delete_property(ctx, JV_OBJ(r[0]), keys.v[i], false) < 0) return JS_EXCEPTION;
            } else {
                /* CreateDataProperty (not OrThrow): a refusal is ignored */
                KzPropDesc d = { r[3], JS_UNDEFINED, JS_UNDEFINED, KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
                if (kz_define_own_property(ctx, JV_OBJ(r[0]), keys.v[i], &d, false) < 0) return JS_EXCEPTION;
            }
        }
    }
    r[1] = kz_key_to_value(ctx, name);
    if (r[1] == JS_EXCEPTION) return r[1];
    JSValue args[2] = { r[1], r[0] };
    return kz_call(ctx, reviver, holder, 2, args);
}

JSValue kz_json_parse(JSContext *ctx, JSValue text, JSValue reviver) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_string(ctx, text);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[0] = kz_str_flatten(ctx, r[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    JParser p = { ctx, JV_STR(r[0]), &r[0], 0, kz_str_len(JV_STR(r[0])) };
    r[1] = jp_value(&p);
    if (r[1] == JS_EXCEPTION) return r[1];
    jp_ws(&p);
    if (p.pos < p.len) return jp_error(&p, NULL);
    if (!kz_is_callable(reviver)) return r[1];
    r[2] = kz_new_object(ctx);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (kz_create_data_property(ctx, r[2], ctx->rt->atom[KZ_ATOM_empty], r[1])) return JS_EXCEPTION;
    return internalize(ctx, r[2], ctx->rt->atom[KZ_ATOM_empty], reviver);
}

/* ================================================================== stringify */

typedef struct JState {
    JSContext *ctx;
    KzStrBuf sb;
    JSValue replacer;           /* a function or undefined */
    KzVec *plist;               /* the property list, or NULL */
    KzVec *stack;               /* objects being serialized */
    JSValue gap;                /* a string ("" for none) */
    uint32_t depth;
} JState;

static void quote(KzStrBuf *sb, JSString *s) {
    static const char hex[] = "0123456789abcdef";
    uint32_t n = kz_str_len(s);
    kz_sb_putc(sb, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(s, i);
        switch (c) {
        case '"': kz_sb_puts(sb, "\\\""); continue;
        case '\\': kz_sb_puts(sb, "\\\\"); continue;
        case 8: kz_sb_puts(sb, "\\b"); continue;
        case 9: kz_sb_puts(sb, "\\t"); continue;
        case 10: kz_sb_puts(sb, "\\n"); continue;
        case 12: kz_sb_puts(sb, "\\f"); continue;
        case 13: kz_sb_puts(sb, "\\r"); continue;
        default: break;
        }
        bool esc = c < 0x20;
        if (c >= 0xD800 && c <= 0xDFFF) {
            /* lone surrogates are escaped; a valid pair passes through */
            if (c <= 0xDBFF && i + 1 < n) {
                uint32_t d = kz_str_at(s, i + 1);
                if (d >= 0xDC00 && d <= 0xDFFF) {
                    kz_sb_putc(sb, c);
                    kz_sb_putc(sb, d);
                    i++;
                    continue;
                }
            }
            esc = true;
        }
        if (esc) {
            char u[7] = { '\\', 'u', hex[(c >> 12) & 15], hex[(c >> 8) & 15], hex[(c >> 4) & 15], hex[c & 15], 0 };
            kz_sb_puts(sb, u);
        } else kz_sb_putc(sb, c);
    }
    kz_sb_putc(sb, '"');
}

static void newline_indent(JState *st) {
    kz_sb_putc(&st->sb, '\n');
    for (uint32_t i = 0; i < st->depth; i++) kz_sb_put_value(&st->sb, st->gap);
}

/* the value of holder[key] after toJSON, the replacer and unwrapping; *out is rooted by the caller */
static int prep(JState *st, JSValue holder, JSValue key, JSValue *out) {
    JSContext *ctx = st->ctx;
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_get(ctx, holder, key, holder);
    if (r[0] == JS_EXCEPTION) return -1;
    if (JV_IS_OBJECT(r[0]) || JV_IS_BIGINT(r[0])) {
        r[1] = kz_get(ctx, r[0], rt->atom[KZ_ATOM_toJSON], r[0]);
        if (r[1] == JS_EXCEPTION) return -1;
        if (kz_is_callable(r[1])) {
            JSValue k = kz_key_to_value(ctx, key);
            if (k == JS_EXCEPTION) return -1;
            JSValue args[1] = { k };
            r[0] = kz_call(ctx, r[1], r[0], 1, args);
            if (r[0] == JS_EXCEPTION) return -1;
        }
    }
    if (st->replacer != JS_UNDEFINED) {
        r[1] = kz_key_to_value(ctx, key);
        if (r[1] == JS_EXCEPTION) return -1;
        JSValue args[2] = { r[1], r[0] };
        r[0] = kz_call(ctx, st->replacer, holder, 2, args);
        if (r[0] == JS_EXCEPTION) return -1;
    }
    if (JV_IS_OBJECT(r[0])) {
        switch (kz_class(JV_OBJ(r[0]))) {
        case KZ_CLASS_NUMBER: {
            double d;
            if (kz_to_number(ctx, r[0], &d)) return -1;
            r[0] = jv_number(d);
            break;
        }
        case KZ_CLASS_STRING:
            r[0] = kz_to_string(ctx, r[0]);
            if (r[0] == JS_EXCEPTION) return -1;
            break;
        case KZ_CLASS_BOOLEAN: case KZ_CLASS_BIGINT:
            r[0] = JV_OBJ(r[0])->u.prim;
            break;
        default:
            break;
        }
    }
    *out = r[0];
    return 0;
}

static bool is_undef_like(JSValue v) {
    return v == JS_UNDEFINED || JV_IS_SYMBOL(v) || kz_is_callable(v);
}

static int write_value(JState *st, JSValue v);

static int write_object(JState *st, JSValue obj) {
    JSContext *ctx = st->ctx;
    KZ_ROOTS(ctx, r, 2);
    r[0] = obj;
    KZ_ROOT_VEC(ctx, keys);
    if (st->plist) {
        for (uint32_t i = 0; i < st->plist->len; i++)
            if (kz_vec_push(ctx, &keys, st->plist->v[i])) return -1;
    } else {
        JSObject *o = JV_OBJ(obj);
        if (!(o->h.flags & OBJ_EXOTIC)) {
            if (kz_own_keys(ctx, o, &keys, KZ_KEYS_STRINGS | KZ_KEYS_ENUM)) return -1;
        } else {
            KZ_ROOT_VEC(ctx, all);
            if (kz_own_keys(ctx, o, &all, KZ_KEYS_STRINGS)) return -1;
            for (uint32_t i = 0; i < all.len; i++) {
                KZ_ROOT_DESC(ctx, d);
                int h = kz_get_own_property(ctx, o, all.v[i], &d);
                if (h < 0) return -1;
                if (h && (d.flags & KZ_PROP_E) && kz_vec_push(ctx, &keys, all.v[i])) return -1;
            }
        }
    }
    bool gap = kz_str_len(JV_STR(st->gap)) > 0;
    uint32_t written = 0;
    kz_sb_putc(&st->sb, '{');
    st->depth++;
    for (uint32_t i = 0; i < keys.len; i++) {
        if (prep(st, r[0], keys.v[i], &r[1])) return -1;
        if (is_undef_like(r[1])) continue;
        if (written++) kz_sb_putc(&st->sb, ',');
        if (gap) newline_indent(st);
        JSValue ks = kz_key_to_value(ctx, keys.v[i]);
        if (ks == JS_EXCEPTION) return -1;
        ks = kz_str_flatten(ctx, ks);
        if (ks == JS_EXCEPTION) return -1;
        quote(&st->sb, JV_STR(ks));
        kz_sb_putc(&st->sb, ':');
        if (gap) kz_sb_putc(&st->sb, ' ');
        if (write_value(st, r[1])) return -1;
    }
    st->depth--;
    if (written && gap) newline_indent(st);
    kz_sb_putc(&st->sb, '}');
    return 0;
}

static int write_array(JState *st, JSValue arr) {
    JSContext *ctx = st->ctx;
    KZ_ROOTS(ctx, r, 2);
    r[0] = arr;
    uint64_t len;
    if (kz_length_of(ctx, arr, &len)) return -1;
    bool gap = kz_str_len(JV_STR(st->gap)) > 0;
    kz_sb_putc(&st->sb, '[');
    st->depth++;
    for (uint64_t i = 0; i < len; i++) {
        if (i) kz_sb_putc(&st->sb, ',');
        if (gap) newline_indent(st);
        JSValue key = i <= KZ_MAX_INDEX ? KZ_KEY_FROM_INDEX((uint32_t)i) : kz_key_from_value(ctx, jv_number((double)i));
        if (key == JS_EXCEPTION) return -1;
        if (prep(st, r[0], key, &r[1])) return -1;
        if (is_undef_like(r[1])) kz_sb_puts(&st->sb, "null");
        else if (write_value(st, r[1])) return -1;
        if ((i & 4095) == 4095) {
            kz_poll_interrupt(ctx);
            if (kz_has_exception(ctx)) return -1;
        }
    }
    st->depth--;
    if (len && gap) newline_indent(st);
    kz_sb_putc(&st->sb, ']');
    return 0;
}

static int write_value(JState *st, JSValue v) {
    JSContext *ctx = st->ctx;
    if (v == JS_NULL) { kz_sb_puts(&st->sb, "null"); return 0; }
    if (v == JS_TRUE) { kz_sb_puts(&st->sb, "true"); return 0; }
    if (v == JS_FALSE) { kz_sb_puts(&st->sb, "false"); return 0; }
    if (JV_IS_STRING(v)) {
        KZ_ROOT(ctx, s);
        s = kz_str_flatten(ctx, v);
        if (s == JS_EXCEPTION) return -1;
        quote(&st->sb, JV_STR(s));
        return 0;
    }
    if (JV_IS_INT(v)) {
        char buf[16];
        int n = kz_dtoa(JV_INT(v), buf);
        kz_sb_put_latin1(&st->sb, (const uint8_t *)buf, (uint32_t)n);
        return 0;
    }
    if (JV_IS_NUMBER(v)) {
        double d = jv_num(v);
        if (d != d || d == KZ_INFINITY || d == -KZ_INFINITY) { kz_sb_puts(&st->sb, "null"); return 0; }
        char buf[40];
        int n = kz_dtoa(d, buf);
        kz_sb_put_latin1(&st->sb, (const uint8_t *)buf, (uint32_t)n);
        return 0;
    }
    if (JV_IS_BIGINT(v)) { kz_throw_type(ctx, "Do not know how to serialize a BigInt"); return -1; }
    if (!JV_IS_OBJECT(v)) return 0;
    if (kz_check_stack(ctx)) return -1;
    for (uint32_t i = 0; i < st->stack->len; i++)
        if (st->stack->v[i] == v) { kz_throw_type(ctx, "Converting circular structure to JSON"); return -1; }
    if (kz_vec_push(ctx, st->stack, v)) return -1;
    int isa = kz_is_array(ctx, v);
    int rc = isa < 0 ? -1 : isa ? write_array(st, v) : write_object(st, v);
    st->stack->len--;
    return rc;
}

JSValue kz_json_stringify(JSContext *ctx, JSValue value, JSValue replacer, JSValue space) {
    KZ_ROOTS(ctx, r, 6);
    KZ_ROOT_VEC(ctx, plist);
    KZ_ROOT_VEC(ctx, stack);
    JState st;
    st.ctx = ctx;
    st.replacer = JS_UNDEFINED;
    st.plist = NULL;
    st.stack = &stack;
    st.depth = 0;
    if (kz_is_callable(replacer)) r[0] = st.replacer = replacer;
    else if (JV_IS_OBJECT(replacer)) {
        int isa = kz_is_array(ctx, replacer);
        if (isa < 0) return JS_EXCEPTION;
        if (isa) {
            uint64_t len;
            if (kz_length_of(ctx, replacer, &len)) return JS_EXCEPTION;
            for (uint64_t i = 0; i < len; i++) {
                r[1] = i <= KZ_MAX_INDEX ? kz_get_index(ctx, replacer, (uint32_t)i) : JS_UNDEFINED;
                if (r[1] == JS_EXCEPTION) return r[1];
                r[2] = JS_UNDEFINED;
                if (JV_IS_STRING(r[1])) r[2] = r[1];
                else if (JV_IS_NUMBER(r[1]) || kz_is_class(r[1], KZ_CLASS_NUMBER) || kz_is_class(r[1], KZ_CLASS_STRING)) {
                    r[2] = kz_to_string(ctx, r[1]);
                    if (r[2] == JS_EXCEPTION) return r[2];
                }
                if (r[2] == JS_UNDEFINED) continue;
                r[2] = kz_key_from_value(ctx, r[2]);
                if (r[2] == JS_EXCEPTION) return r[2];
                bool dup = false;
                for (uint32_t j = 0; j < plist.len; j++)
                    if (plist.v[j] == r[2]) { dup = true; break; }
                if (!dup && kz_vec_push(ctx, &plist, r[2])) return JS_EXCEPTION;
            }
            st.plist = &plist;
        }
    }
    r[3] = space;
    if (kz_is_class(r[3], KZ_CLASS_NUMBER)) {
        double d;
        if (kz_to_number(ctx, r[3], &d)) return JS_EXCEPTION;
        r[3] = jv_number(d);
    } else if (kz_is_class(r[3], KZ_CLASS_STRING)) {
        r[3] = kz_to_string(ctx, r[3]);
        if (r[3] == JS_EXCEPTION) return r[3];
    }
    r[4] = ctx->rt->atom[KZ_ATOM_empty];
    if (JV_IS_NUMBER(r[3])) {
        double n;
        if (kz_to_integer_or_inf(ctx, r[3], &n)) return JS_EXCEPTION;
        if (n > 10) n = 10;
        if (n >= 1) {
            static const char spaces[] = "          ";
            r[4] = kz_str_from_latin1(ctx, (const uint8_t *)spaces, (uint32_t)n);
            if (r[4] == JS_EXCEPTION) return r[4];
        }
    } else if (JV_IS_STRING(r[3])) {
        uint32_t n = kz_str_len(JV_STR(r[3]));
        r[4] = n <= 10 ? r[3] : kz_str_sub(ctx, r[3], 0, 10);
        if (r[4] == JS_EXCEPTION) return r[4];
        r[4] = kz_str_flatten(ctx, r[4]);
        if (r[4] == JS_EXCEPTION) return r[4];
    }
    st.gap = r[4];
    r[5] = kz_new_object(ctx);
    if (r[5] == JS_EXCEPTION) return r[5];
    if (kz_create_data_property(ctx, r[5], ctx->rt->atom[KZ_ATOM_empty], value)) return JS_EXCEPTION;
    kz_sb_init(&st.sb, ctx);
    if (prep(&st, r[5], ctx->rt->atom[KZ_ATOM_empty], &r[1])) { kz_sb_free(&st.sb); return JS_EXCEPTION; }
    if (is_undef_like(r[1])) { kz_sb_free(&st.sb); return JS_UNDEFINED; }
    if (write_value(&st, r[1])) { kz_sb_free(&st.sb); return JS_EXCEPTION; }
    return kz_sb_finish(&st.sb);
}

/* ================================================================== the JSON object */

static JSValue json_parse(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    return kz_json_parse(ctx, argv[0], argv[1]);
}

static JSValue json_stringify(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    return kz_json_stringify(ctx, argv[0], argv[1], argv[2]);
}

static const KzFuncDef json_funcs[] = {
    KZ_FN("parse", json_parse, 2),
    KZ_FN("stringify", json_stringify, 3),
};

int kz_init_json(JSContext *ctx) {
    KZ_ROOT(ctx, j);
    j = kz_new_object(ctx);
    if (j == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, j, json_funcs, KZ_COUNTOF(json_funcs))) return -1;
    JSValue tag = kz_atom(ctx, "JSON");
    if (tag == JS_EXCEPTION || kz_define_value(ctx, j, ctx->rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_JSON, j, KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
