/*
 * error.c: throwing, the Error constructors (ECMA-262 20.5), stack traces, and a small printf for messages.
 *
 * Every error object gets an own, non-enumerable `stack` string when it is made: "Name: message" and one
 * "    at function (file:line:column)" line per JavaScript frame (at most 10, like other engines).
 */
#include "jsint.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ formatting */

static size_t put_s(char *buf, size_t o, size_t n, const char *s) {
    while (*s) { if (o + 1 < n) buf[o] = *s; o++; s++; }
    return o;
}

static size_t put_u(char *buf, size_t o, size_t n, unsigned long long v, int base, bool upper) {
    char tmp[24];
    int t = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[t++] = digs[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    while (t) { if (o + 1 < n) buf[o] = tmp[t - 1]; o++; t--; }
    return o;
}

int kz_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int kz_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t o = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { if (o + 1 < n) buf[o] = *p; o++; continue; }
        p++;
        int lng = 0;
        bool z = false;
        while (*p == 'l') { lng++; p++; }
        if (*p == 'z') { z = true; p++; }
        switch (*p) {
        case 's': { const char *s = va_arg(ap, const char *); o = put_s(buf, o, n, s ? s : "(null)"); break; }
        case 'd': case 'i': {
            long long v = z ? (long long)va_arg(ap, long) : lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { if (o + 1 < n) buf[o] = '-'; o++; v = -v; }
            o = put_u(buf, o, n, (unsigned long long)v, 10, false);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v = z ? va_arg(ap, size_t) : lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            o = put_u(buf, o, n, v, *p == 'u' ? 10 : 16, *p == 'X');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); if (o + 1 < n) buf[o] = c; o++; break; }
        case '%': if (o + 1 < n) buf[o] = '%'; o++; break;
        default: if (o + 1 < n) buf[o] = *p; o++; break;
        }
        if (!*p) break;
    }
    if (n) buf[o < n ? o : n - 1] = 0;
    return (int)o;
}

static int kz_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kz_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* a short text for a value in a message (never runs JavaScript) */
void kz_format_value_for_msg(JSContext *ctx, JSValue v, char *buf, size_t cap) {
    if (JV_IS_STRING(v)) {
        JSString *s = JV_STR(v);
        if (s->h.flags & STR_ROPE) { kz_snprintf(buf, cap, "string"); return; }
        char tmp[64];
        kz_str_to_utf8_buf(s, tmp, sizeof tmp);
        kz_snprintf(buf, cap, "\"%s\"", tmp);
        return;
    }
    if (JV_IS_NUMBER(v)) {
        char tmp[40];
        int n = kz_dtoa(jv_num(v), tmp);
        tmp[n] = 0;
        kz_snprintf(buf, cap, "%s", tmp);
        return;
    }
    if (JV_IS_SYMBOL(v)) {
        JSValue d = JV_SYM(v)->description;
        char tmp[64] = "";
        if (JV_IS_STRING(d) && !(JV_STR(d)->h.flags & STR_ROPE)) kz_str_to_utf8_buf(JV_STR(d), tmp, sizeof tmp);
        kz_snprintf(buf, cap, "Symbol(%s)", tmp);
        return;
    }
    if (JV_IS_BIGINT(v)) { kz_snprintf(buf, cap, "a BigInt"); return; }
    if (JV_IS_OBJECT(v)) {
        JSObject *o = JV_OBJ(v);
        if (o->h.flags & OBJ_CALLABLE) {
            char tmp[64] = "";
            JSValue name = kz_func_name(ctx, o);
            if (JV_IS_STRING(name) && !(JV_STR(name)->h.flags & STR_ROPE)) kz_str_to_utf8_buf(JV_STR(name), tmp, sizeof tmp);
            kz_snprintf(buf, cap, tmp[0] ? "function %s" : "function", tmp);
            return;
        }
        kz_snprintf(buf, cap, kz_class(o) == KZ_CLASS_ARRAY ? "[object Array]" : "[object Object]");
        return;
    }
    switch (v) {
    case JS_UNDEFINED: kz_snprintf(buf, cap, "undefined"); return;
    case JS_NULL: kz_snprintf(buf, cap, "null"); return;
    case JS_TRUE: kz_snprintf(buf, cap, "true"); return;
    case JS_FALSE: kz_snprintf(buf, cap, "false"); return;
    default: kz_snprintf(buf, cap, "value"); return;
    }
}

/* ------------------------------------------------------------------ throwing */

JSValue kz_throw(JSContext *ctx, JSValue v) {
    JSRuntime *rt = ctx->rt;
    if (rt->exc_uncatchable) return JS_EXCEPTION;    /* an interrupt is on its way out: keep it */
    rt->exception = v;
    return JS_EXCEPTION;
}

JSValue kz_take_exception(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    JSValue e = rt->exception;
    rt->exception = JS_UNINIT;
    rt->exc_uncatchable = false;
    return e == JS_UNINIT ? JS_UNDEFINED : e;
}

static const int err_atom[KZ_ERR_COUNT] = { KZ_ATOM_Error, KZ_ATOM_EvalError, KZ_ATOM_RangeError, KZ_ATOM_ReferenceError,
                                            KZ_ATOM_SyntaxError, KZ_ATOM_TypeError, KZ_ATOM_URIError, KZ_ATOM_AggregateError };
static const KzIntrinsic err_proto[KZ_ERR_COUNT] = { KZ_I_ERROR_PROTO, KZ_I_EVAL_ERROR_PROTO, KZ_I_RANGE_ERROR_PROTO,
                                                     KZ_I_REFERENCE_ERROR_PROTO, KZ_I_SYNTAX_ERROR_PROTO, KZ_I_TYPE_ERROR_PROTO,
                                                     KZ_I_URI_ERROR_PROTO, KZ_I_AGGREGATE_ERROR_PROTO };

JSValue kz_new_error_value(JSContext *ctx, KzErr kind, JSValue message) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = message;
    JSValue proto = ctx->intrinsic[err_proto[kind]];
    if (!JV_IS_OBJECT(proto)) return r[0];          /* during realm setup: a plain string */
    r[1] = kz_new_object_class(ctx, KZ_CLASS_ERROR, proto);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (r[0] != JS_UNDEFINED && kz_obj_add_prop(ctx, JV_OBJ(r[1]), ctx->rt->atom[KZ_ATOM_message], KZ_PROP_W | KZ_PROP_C, r[0]))
        return JS_EXCEPTION;
    if (kz_capture_stack(ctx, r[1], 0)) return JS_EXCEPTION;
    return r[1];
}

static JSValue vnew_error(JSContext *ctx, KzErr kind, const char *fmt, va_list ap) {
    char msg[512];
    kz_vsnprintf(msg, sizeof msg, fmt, ap);
    KZ_ROOT(ctx, m);
    m = kz_str_from_cstr(ctx, msg);
    if (m == JS_EXCEPTION) return m;
    return kz_new_error_value(ctx, kind, m);
}

JSValue kz_new_error_msg(JSContext *ctx, KzErr kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    JSValue e = vnew_error(ctx, kind, fmt, ap);
    va_end(ap);
    return e;
}

static JSValue vthrow(JSContext *ctx, KzErr kind, const char *fmt, va_list ap) {
    if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
    JSValue e = vnew_error(ctx, kind, fmt, ap);
    if (e == JS_EXCEPTION) return e;        /* out of memory while making it: the OOM error is pending */
    return kz_throw(ctx, e);
}

JSValue kz_throw_error(JSContext *ctx, KzErr kind, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = vthrow(ctx, kind, fmt, ap); va_end(ap); return r;
}
JSValue kz_throw_type(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = vthrow(ctx, KZ_ERR_TYPE, fmt, ap); va_end(ap); return r;
}
JSValue kz_throw_range(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = vthrow(ctx, KZ_ERR_RANGE, fmt, ap); va_end(ap); return r;
}
JSValue kz_throw_reference(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = vthrow(ctx, KZ_ERR_REFERENCE, fmt, ap); va_end(ap); return r;
}
JSValue kz_throw_syntax(JSContext *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); JSValue r = vthrow(ctx, KZ_ERR_SYNTAX, fmt, ap); va_end(ap); return r;
}

/* a SyntaxError at a source position: the stack line names the place in the script being compiled */
JSValue kz_throw_syntax_at(JSContext *ctx, JSValue filename, uint32_t line, uint32_t col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[400];
    kz_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
    JSValue e = kz_new_error_msg(ctx, KZ_ERR_SYNTAX, "%s", msg);
    if (e == JS_EXCEPTION) return e;
    if (JV_IS_OBJECT(e)) {
        KZ_ROOTS(ctx, r, 3);
        r[0] = e;
        char fn[200] = "<input>";
        if (JV_IS_STRING(filename)) kz_str_to_utf8_buf(JV_STR(filename), fn, sizeof fn);
        char st[700];
        kz_snprintf(st, sizeof st, "SyntaxError: %s\n    at %s:%u:%u", msg, fn, line, col);
        r[1] = kz_str_from_cstr(ctx, st);
        if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_stack, r[1], KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_lineNumber, JV_MKINT((int32_t)line), KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_columnNumber, JV_MKINT((int32_t)col), KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
        if (JV_IS_STRING(filename) && kz_define_value_atom(ctx, r[0], KZ_ATOM_fileName, filename, KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
        e = r[0];
    }
    return kz_throw(ctx, e);
}

JSValue kz_throw_not_function(JSContext *ctx, JSValue v) {
    char buf[96];
    kz_format_value_for_msg(ctx, v, buf, sizeof buf);
    return kz_throw_type(ctx, "%s is not a function", buf);
}

JSValue kz_throw_not_object(JSContext *ctx, JSValue v) {
    char buf[96];
    kz_format_value_for_msg(ctx, v, buf, sizeof buf);
    return kz_throw_type(ctx, "%s is not an object", buf);
}

/* ------------------------------------------------------------------ stack traces */

#define STACK_LIMIT 10

static void sb_put_value_utf8ish(KzStrBuf *sb, JSValue s) {
    if (JV_IS_STRING(s)) kz_sb_put_value(sb, s);
}

/* The current JavaScript frames as text, preceded by "Name: message" of err. */
int kz_capture_stack(JSContext *ctx, JSValue err, int skip) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = err;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    /* the header: the same text Error.prototype.toString would give, without calling anything */
    JSValue name = JS_UNDEFINED, msg = JS_UNDEFINED;
    JSObject *o = JV_OBJ(err);
    for (JSObject *p = o; p && !(p->h.flags & OBJ_EXOTIC); p = JV_IS_OBJECT(p->shape->proto) ? JV_OBJ(p->shape->proto) : NULL) {
        int i = kz_shape_find(p->shape, rt->atom[KZ_ATOM_name]);
        if (i >= 0 && !(p->shape->props[i].attrs & KZ_PROP_ACCESSOR)) { name = p->slots[i]; break; }
    }
    {
        int i = kz_shape_find(o->shape, rt->atom[KZ_ATOM_message]);
        if (i >= 0 && !(o->shape->props[i].attrs & KZ_PROP_ACCESSOR)) msg = o->slots[i];
    }
    r[1] = name;
    r[2] = msg;
    if (JV_IS_STRING(r[1])) kz_sb_put_value(&sb, r[1]); else kz_sb_puts(&sb, "Error");
    if (JV_IS_STRING(r[2]) && kz_str_len(JV_STR(r[2]))) { kz_sb_puts(&sb, ": "); kz_sb_put_value(&sb, r[2]); }
    int n = 0;
    for (JSFrame *f = rt->frame; f && n < STACK_LIMIT; f = f->prev) {
        if (!f->b) continue;
        if (skip > 0) { skip--; continue; }
        JSFunctionBytecode *b = f->b;
        uint32_t line, col, pc = (uint32_t)(f->pc - b->code);
        kz_line_of_pc(b, pc ? pc - 1 : 0, &line, &col);
        kz_sb_puts(&sb, "\n    at ");
        bool named = JV_IS_STRING(b->name) && kz_str_len(JV_STR(b->name)) > 0;
        int kind = kz_bc_kind(b);
        if (named) {
            if ((f->flags & FRAME_CONSTRUCT)) kz_sb_puts(&sb, "new ");
            sb_put_value_utf8ish(&sb, b->name);
            kz_sb_puts(&sb, " (");
        } else if (kind != KZ_FUNC_SCRIPT && kind != KZ_FUNC_EVAL && kind != KZ_FUNC_MODULE) kz_sb_puts(&sb, "<anonymous> (");
        if (JV_IS_STRING(b->filename)) kz_sb_put_value(&sb, b->filename); else kz_sb_puts(&sb, "<input>");
        char pos[32];
        kz_snprintf(pos, sizeof pos, ":%u:%u", line, col);
        kz_sb_puts(&sb, pos);
        if (named || (kind != KZ_FUNC_SCRIPT && kind != KZ_FUNC_EVAL && kind != KZ_FUNC_MODULE)) kz_sb_putc(&sb, ')');
        n++;
    }
    JSValue st = kz_sb_finish(&sb);
    if (st == JS_EXCEPTION) return -1;
    r[1] = st;
    JSObject *eo = JV_OBJ(r[0]);
    int i = kz_shape_find(eo->shape, rt->atom[KZ_ATOM_stack]);
    if (i >= 0) { eo->slots[i] = r[1]; return 0; }
    return kz_obj_add_prop(ctx, eo, rt->atom[KZ_ATOM_stack], KZ_PROP_W | KZ_PROP_C, r[1]);
}

/* ------------------------------------------------------------------ the constructors */

/* InstallErrorCause */
static int install_cause(JSContext *ctx, JSValue o, JSValue options) {
    if (!JV_IS_OBJECT(options)) return 0;
    int h = kz_has_property(ctx, JV_OBJ(options), ctx->rt->atom[KZ_ATOM_cause]);
    if (h <= 0) return h;
    JSValue c = kz_get(ctx, options, ctx->rt->atom[KZ_ATOM_cause], options);
    if (c == JS_EXCEPTION) return -1;
    KZ_ROOT(ctx, rc);
    rc = c;
    return kz_define_value_atom(ctx, o, KZ_ATOM_cause, rc, KZ_PROP_W | KZ_PROP_C);
}

static JSValue error_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JSValue nt = ctx->rt->new_target;
    KZ_ROOTS(ctx, r, 2);
    if (nt == JS_UNDEFINED) nt = JV_MKOBJ(ctx->rt->cur_native);  /* called as a function: newTarget = the active function */
    r[1] = nt;
    r[0] = kz_ordinary_create_from_ctor(ctx, r[1], err_proto[magic], KZ_CLASS_ERROR);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    int mi = magic == KZ_ERR_AGGREGATE ? 1 : 0;
    if (argv[mi] != JS_UNDEFINED) {
        JSValue m = kz_to_string(ctx, argv[mi]);
        if (m == JS_EXCEPTION) return m;
        KZ_ROOT(ctx, rm);
        rm = m;
        if (kz_obj_add_prop(ctx, JV_OBJ(r[0]), ctx->rt->atom[KZ_ATOM_message], KZ_PROP_W | KZ_PROP_C, rm)) return JS_EXCEPTION;
    }
    if (install_cause(ctx, r[0], argv[mi + 1])) return JS_EXCEPTION;
    if (magic == KZ_ERR_AGGREGATE) {
        KZ_ROOT_VEC(ctx, errs);
        if (kz_iterate_to_vec(ctx, argv[0], &errs)) return JS_EXCEPTION;
        JSValue arr = kz_new_array_from(ctx, errs.len, errs.v);
        if (arr == JS_EXCEPTION) return arr;
        KZ_ROOT(ctx, ra);
        ra = arr;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_errors, ra, KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
    }
    if (kz_capture_stack(ctx, r[0], 0)) return JS_EXCEPTION;
    (void)this_val;
    (void)argc;
    return r[0];
}

static JSValue error_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Error.prototype.toString called on non-object");
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_get(ctx, this_val, ctx->rt->atom[KZ_ATOM_name], this_val);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    r[0] = r[0] == JS_UNDEFINED ? ctx->rt->atom[KZ_ATOM_Error] : kz_to_string(ctx, r[0]);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    r[1] = kz_get(ctx, this_val, ctx->rt->atom[KZ_ATOM_message], this_val);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    r[1] = r[1] == JS_UNDEFINED ? ctx->rt->atom[KZ_ATOM_empty] : kz_to_string(ctx, r[1]);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (!kz_str_len(JV_STR(r[0]))) return r[1];
    if (!kz_str_len(JV_STR(r[1]))) return r[0];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_put_value(&sb, r[0]);
    kz_sb_puts(&sb, ": ");
    kz_sb_put_value(&sb, r[1]);
    (void)argc; (void)argv; (void)magic;
    return kz_sb_finish(&sb);
}

/* Error.captureStackTrace(obj): the V8 extension many libraries call */
static JSValue error_capture(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    if (!JV_IS_OBJECT(argv[0])) return kz_throw_type(ctx, "Invalid argument");
    if (kz_capture_stack(ctx, argv[0], 0)) return JS_EXCEPTION;
    (void)this_val; (void)argc; (void)magic;
    return JS_UNDEFINED;
}

static const KzFuncDef error_proto_funcs[] = {
    KZ_FN("toString", error_to_string, 0),
};

static const KzFuncDef error_static_funcs[] = {
    KZ_FN("captureStackTrace", error_capture, 1),
};

int kz_init_error(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    static const char *const names[KZ_ERR_COUNT] = { "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError",
                                                     "TypeError", "URIError", "AggregateError" };
    KZ_ROOTS(ctx, r, 2);
    for (int k = 0; k < KZ_ERR_COUNT; k++) {
        /* prototypes: Error.prototype is an ordinary object; the others inherit from it */
        JSValue proto_parent = k == KZ_ERR_ERROR ? kz_intr(ctx, KZ_I_OBJECT_PROTO) : kz_intr(ctx, KZ_I_ERROR_PROTO);
        r[0] = kz_new_object_proto(ctx, proto_parent);
        if (r[0] == JS_EXCEPTION) return -1;
        ctx->intrinsic[err_proto[k]] = r[0];
        /* (argv holds message and options, plus errors first for AggregateError) */
        r[1] = kz_make_ctor(ctx, error_ctor, names[k], k == KZ_ERR_AGGREGATE ? 2 : 1, k, err_proto[k], KZ_I_ERROR + k);
        if (r[1] == JS_EXCEPTION) return -1;
        JV_OBJ(r[1])->u.native.nargs = 3;
        if (k != KZ_ERR_ERROR && kz_set_proto(ctx, JV_OBJ(r[1]), kz_intr(ctx, KZ_I_ERROR), true) < 0) return -1;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_name, rt->atom[err_atom[k]], KZ_PROP_W | KZ_PROP_C)) return -1;
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_message, rt->atom[KZ_ATOM_empty], KZ_PROP_W | KZ_PROP_C)) return -1;
        if (kz_define_value_atom(ctx, ctx->global, err_atom[k], r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
        if (k == KZ_ERR_ERROR) {
            if (kz_define_functions(ctx, r[0], error_proto_funcs, KZ_COUNTOF(error_proto_funcs))) return -1;
            if (kz_define_functions(ctx, r[1], error_static_funcs, KZ_COUNTOF(error_static_funcs))) return -1;
        }
    }
    /* the preallocated out-of-memory error (throwing it allocates nothing) */
    if (!JV_IS_OBJECT(rt->oom_error)) {
        r[0] = kz_str_from_cstr(ctx, "out of memory");
        if (r[0] == JS_EXCEPTION) return -1;
        r[1] = kz_new_object_class(ctx, KZ_CLASS_ERROR, kz_intr(ctx, KZ_I_RANGE_ERROR_PROTO));
        if (r[1] == JS_EXCEPTION) return -1;
        if (kz_obj_add_prop(ctx, JV_OBJ(r[1]), rt->atom[KZ_ATOM_message], KZ_PROP_W | KZ_PROP_C, r[0])) return -1;
        r[0] = kz_str_from_cstr(ctx, "RangeError: out of memory");
        if (r[0] == JS_EXCEPTION) return -1;
        if (kz_obj_add_prop(ctx, JV_OBJ(r[1]), rt->atom[KZ_ATOM_stack], KZ_PROP_W | KZ_PROP_C, r[0])) return -1;
        rt->oom_error = r[1];
    }
    return 0;
}
