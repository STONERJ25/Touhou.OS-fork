/*
 * function.c: function objects (ECMA-262 10.2, 10.3 built-in functions, 10.4.1 bound functions, 10.4.4 arguments
 * exotic objects), Function and Function.prototype (20.2), and the helpers that install built-in methods.
 */
#include "jsint.h"
#include "bytecode.h"

/* ================================================================== native functions */

JSValue kz_new_native(JSContext *ctx, KzNative *fn, JSValue name, int length, int magic, int ndata, const JSValue *data) {
    KZ_ROOT(ctx, rn);
    rn = name;
    JSValue *d = NULL;
    if (ndata > 0) {
        d = kz_malloc(ctx, (size_t)ndata * sizeof(JSValue));
        if (!d) return JS_EXCEPTION;
        for (int i = 0; i < ndata; i++) d[i] = data ? data[i] : JS_UNDEFINED;
    }
    JSObject *o = kz_obj_alloc(ctx, KZ_CLASS_NATIVE, ctx->shape_func, 0);
    if (!o) { kz_free(ctx->rt, d); return JS_EXCEPTION; }
    o->h.flags |= OBJ_CALLABLE;
    o->u.native.fn = fn;
    o->u.native.magic = (int16_t)magic;
    o->u.native.nargs = (uint8_t)(length < 0 ? 0 : length > 255 ? 255 : length);
    o->u.native.realm = ctx;
    o->u.native.data = d;
    o->u.native.ndata = (uint16_t)(ndata > 0 ? ndata : 0);
    o->slots[0] = JV_MKINT(length);
    o->slots[1] = rn;
    return JV_MKOBJ(o);
}

JSValue kz_new_native_c(JSContext *ctx, KzNative *fn, const char *name, int length, int magic) {
    JSValue n = kz_atom(ctx, name);
    if (n == JS_EXCEPTION) return n;
    KZ_ROOT(ctx, rn);
    rn = n;
    return kz_new_native(ctx, fn, rn, length, magic, 0, NULL);
}

JSValue kz_native_data(JSContext *ctx, int i) {
    JSObject *f = ctx->rt->cur_native;
    if (!f || i < 0 || i >= f->u.native.ndata) return JS_UNDEFINED;
    return f->u.native.data[i];
}

void kz_native_set_data(JSContext *ctx, int i, JSValue v) {
    JSObject *f = ctx->rt->cur_native;
    if (f && i >= 0 && i < f->u.native.ndata) f->u.native.data[i] = v;
}

static const char *const wellknown_names[KZ_SYM_COUNT] = {
    "asyncIterator", "hasInstance", "isConcatSpreadable", "iterator", "match", "matchAll", "replace", "search",
    "species", "split", "toPrimitive", "toStringTag", "unscopables"
};

/* "[Symbol.iterator]" -> the symbol, anything else -> an atom */
JSValue kz_def_name_key(JSContext *ctx, const char *name) {
    if (name[0] == '[' && !strncmp(name, "[Symbol.", 8)) {
        size_t n = strlen(name);
        for (int i = 0; i < KZ_SYM_COUNT; i++) {
            size_t k = strlen(wellknown_names[i]);
            if (n == 8 + k + 1 && !strncmp(name + 8, wellknown_names[i], k)) return ctx->rt->sym[i];
        }
    }
    return kz_atom(ctx, name);
}

/* the "name" a property key gives a function (SetFunctionName's naming part) */
static JSValue key_function_name(JSContext *ctx, JSValue key, const char *prefix) {
    KZ_ROOTS(ctx, r, 1);
    if (JV_IS_SYMBOL(key)) {
        JSValue d = JV_SYM(key)->description;
        if (JV_SYM(key)->h.flags & SYM_PRIVATE) r[0] = d;
        else if (d == JS_UNDEFINED) r[0] = ctx->rt->atom[KZ_ATOM_empty];
        else {
            KzStrBuf sb;
            kz_sb_init(&sb, ctx);
            kz_sb_putc(&sb, '[');
            kz_sb_put_value(&sb, d);
            kz_sb_putc(&sb, ']');
            r[0] = kz_sb_finish(&sb);
        }
    } else r[0] = kz_key_to_value(ctx, key);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!prefix) return r[0];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_puts(&sb, prefix);
    kz_sb_putc(&sb, ' ');
    kz_sb_put_value(&sb, r[0]);
    return kz_sb_finish(&sb);
}

int kz_set_function_name(JSContext *ctx, JSValue f, JSValue key, const char *prefix) {
    if (!JV_IS_OBJECT(f)) return 0;
    JSValue n = key_function_name(ctx, key, prefix);
    if (n == JS_EXCEPTION) return -1;
    KZ_ROOT(ctx, rn);
    rn = n;
    JSObject *o = JV_OBJ(f);
    int i = kz_shape_find(o->shape, ctx->rt->atom[KZ_ATOM_name]);
    if (i >= 0 && !(o->shape->props[i].attrs & KZ_PROP_ACCESSOR)) {
        /* classes with a static name member keep it */
        if (kz_class(o) == KZ_CLASS_FUNCTION && (o->shape->props[i].attrs & KZ_PROP_W)) return 0;
        o->slots[i] = rn;
        return 0;
    }
    if (i >= 0) return 0;
    return kz_define_value_atom(ctx, f, KZ_ATOM_name, rn, KZ_PROP_C);
}

JSValue kz_func_name(JSContext *ctx, JSObject *f) {
    int i = kz_shape_find(f->shape, ctx->rt->atom[KZ_ATOM_name]);
    if (i >= 0 && !(f->shape->props[i].attrs & KZ_PROP_ACCESSOR) && JV_IS_STRING(f->slots[i])) return f->slots[i];
    if (kz_class(f) == KZ_CLASS_FUNCTION) return f->u.func.b->name;
    return ctx->rt->atom[KZ_ATOM_empty];
}

int kz_define_functions(JSContext *ctx, JSValue obj, const KzFuncDef *defs, int n) {
    KZ_ROOTS(ctx, r, 4);
    for (int i = 0; i < n; i++) {
        const KzFuncDef *d = &defs[i];
        r[0] = kz_def_name_key(ctx, d->name);
        if (r[0] == JS_EXCEPTION) return -1;
        if (d->kind == KZ_DEF_METHOD) {
            r[1] = key_function_name(ctx, r[0], NULL);
            if (r[1] == JS_EXCEPTION) return -1;
            r[2] = kz_new_native(ctx, d->fn, r[1], d->length, d->magic, 0, NULL);
            if (r[2] == JS_EXCEPTION) return -1;
            if (d->nargs > d->length) JV_OBJ(r[2])->u.native.nargs = d->nargs;
            if (kz_define_value(ctx, obj, r[0], r[2], d->attrs ? (d->attrs & KZ_PROP_WEC) : (KZ_PROP_W | KZ_PROP_C))) return -1;
            continue;
        }
        r[1] = key_function_name(ctx, r[0], "get");
        if (r[1] == JS_EXCEPTION) return -1;
        r[2] = kz_new_native(ctx, d->fn, r[1], 0, d->magic, 0, NULL);
        if (r[2] == JS_EXCEPTION) return -1;
        r[3] = JS_UNDEFINED;
        if (d->kind == KZ_DEF_ACCESSOR && d->setter) {
            r[1] = key_function_name(ctx, r[0], "set");
            if (r[1] == JS_EXCEPTION) return -1;
            r[3] = kz_new_native(ctx, d->setter, r[1], 1, d->magic, 0, NULL);
            if (r[3] == JS_EXCEPTION) return -1;
        }
        if (kz_define_accessor(ctx, obj, r[0], r[2], r[3], d->attrs ? (d->attrs & (KZ_PROP_E | KZ_PROP_C)) : KZ_PROP_C)) return -1;
    }
    return 0;
}

/* a native constructor C with C.prototype = intrinsic[proto_id] and proto.constructor = C */
JSValue kz_make_ctor(JSContext *ctx, KzNative *fn, const char *name, int length, int magic, KzIntrinsic proto_id, int ctor_id) {
    KZ_ROOT(ctx, c);
    c = kz_new_native_c(ctx, fn, name, length, magic);
    if (c == JS_EXCEPTION) return c;
    JV_OBJ(c)->h.flags |= OBJ_CONSTRUCTOR;
    if ((int)proto_id >= 0) {
        JSValue proto = ctx->intrinsic[proto_id];
        if (JV_IS_OBJECT(proto)) {
            if (kz_define_value_atom(ctx, c, KZ_ATOM_prototype, proto, 0)) return JS_EXCEPTION;
            if (kz_define_value_atom(ctx, proto, KZ_ATOM_constructor, c, KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
        }
    }
    if (ctor_id >= 0) ctx->intrinsic[ctor_id] = c;
    return c;
}

/* ================================================================== arguments objects */

static int margs_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    if (!KZ_KEY_IS_INDEX(key)) return KZ_FALLBACK;
    uint32_t i = KZ_KEY_INDEX(key);
    if (i >= o->u.margs.nmapped || !o->u.margs.cells[i]) return KZ_FALLBACK;
    int r = KZ_FALLBACK;
    (void)ctx;
    /* the element exists in elems (the mapping is only for its value) */
    if (i < o->elem_len && o->elems[i] != JS_HOLE) {
        d->value = o->u.margs.cells[i]->value;
        d->getter = d->setter = JS_UNDEFINED;
        d->flags = KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
        r = 1;
    }
    return r;
}

static int margs_get(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out) {
    (void)ctx;
    (void)receiver;
    if (!KZ_KEY_IS_INDEX(key)) return KZ_FALLBACK;
    uint32_t i = KZ_KEY_INDEX(key);
    if (i >= o->u.margs.nmapped || !o->u.margs.cells[i] || i >= o->elem_len || o->elems[i] == JS_HOLE) return KZ_FALLBACK;
    *out = o->u.margs.cells[i]->value;
    return 1;
}

static int margs_set(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver) {
    (void)ctx;
    if (!KZ_KEY_IS_INDEX(key) || receiver != JV_MKOBJ(o)) return KZ_FALLBACK;
    uint32_t i = KZ_KEY_INDEX(key);
    if (i >= o->u.margs.nmapped || !o->u.margs.cells[i] || i >= o->elem_len || o->elems[i] == JS_HOLE) return KZ_FALLBACK;
    o->u.margs.cells[i]->value = val;
    o->elems[i] = val;
    return 1;
}

static int margs_define(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d) {
    if (!KZ_KEY_IS_INDEX(key)) return KZ_FALLBACK;
    uint32_t i = KZ_KEY_INDEX(key);
    if (i >= o->u.margs.nmapped || !o->u.margs.cells[i]) return KZ_FALLBACK;
    /* keep the element's current value in elems, then let the ordinary algorithm run without the mapping */
    if (i < o->elem_len && o->elems[i] != JS_HOLE) o->elems[i] = o->u.margs.cells[i]->value;
    JSVarCell *cell = o->u.margs.cells[i];
    o->u.margs.cells[i] = NULL;             /* unmapped for the moment: the call below takes the ordinary path */
    int r = kz_define_own_property(ctx, o, key, d, false);
    if (r <= 0) { o->u.margs.cells[i] = cell; return r; }
    /* ArgumentsDefineOwnProperty: a data descriptor keeps the mapping unless it makes the property read-only */
    bool accessor = d->flags & (PD_HAS_GET | PD_HAS_SET);
    if (!accessor) {
        if (d->flags & PD_HAS_VALUE) cell->value = d->value;
        if (!((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W))) o->u.margs.cells[i] = cell;
    }
    return 1;
}

static int margs_del(JSContext *ctx, JSObject *o, JSValue key) {
    (void)ctx;
    if (KZ_KEY_IS_INDEX(key)) {
        uint32_t i = KZ_KEY_INDEX(key);
        if (i < o->u.margs.nmapped && o->u.margs.cells[i]) {
            /* deleting unmaps (the ordinary delete runs next) */
            o->u.margs.cells[i] = NULL;
        }
    }
    return KZ_FALLBACK;
}

static const KzExotic margs_exotic = {
    .get_own = margs_get_own, .define_own = margs_define, .get = margs_get, .set = margs_set, .del = margs_del,
};

/* CreateUnmappedArgumentsObject / CreateMappedArgumentsObject */
JSValue kz_new_arguments(JSContext *ctx, JSFrame *f, bool mapped) {
    JSRuntime *rt = ctx->rt;
    kz_exotic[KZ_CLASS_MAPPED_ARGUMENTS] = &margs_exotic;
    uint32_t argc = f->argc;
    KZ_ROOT(ctx, o);
    JSVarCell **cells = NULL;
    uint32_t nmapped = 0;
    if (mapped) {
        nmapped = argc < f->b->nparams ? argc : f->b->nparams;
        if (nmapped) {
            cells = kz_mallocz(ctx, nmapped * sizeof(JSVarCell *));
            if (!cells) return JS_EXCEPTION;
            for (uint32_t i = 0; i < nmapped; i++)
                if (JV_IS_INTERNAL(f->regs[i])) cells[i] = JV_PTR(f->regs[i]);
        }
    }
    o = kz_new_object_class(ctx, mapped ? KZ_CLASS_MAPPED_ARGUMENTS : KZ_CLASS_ARGUMENTS, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (o == JS_EXCEPTION) { kz_free(rt, cells); return o; }
    JSObject *ao = JV_OBJ(o);
    if (mapped) {
        ao->u.margs.cells = cells;
        ao->u.margs.nmapped = nmapped;
    }
    if (argc) {
        if (kz_elems_reserve(ctx, ao, argc)) return JS_EXCEPTION;
        for (uint32_t i = 0; i < argc; i++) ao->elems[i] = f->argv[i];
        ao->elem_len = argc;
    }
    if (kz_obj_add_prop(ctx, ao, rt->atom[KZ_ATOM_length], KZ_PROP_W | KZ_PROP_C, JV_MKINT((int32_t)argc))) return JS_EXCEPTION;
    if (kz_obj_add_prop(ctx, ao, rt->sym[KZ_SYM_iterator], KZ_PROP_W | KZ_PROP_C, kz_intr(ctx, KZ_I_ARRAY_PROTO_VALUES))) return JS_EXCEPTION;
    if (mapped) {
        if (kz_obj_add_prop(ctx, ao, rt->atom[KZ_ATOM_callee], KZ_PROP_W | KZ_PROP_C, JV_MKOBJ(f->func))) return JS_EXCEPTION;
    } else {
        JSValue t = kz_intr(ctx, KZ_I_THROW_TYPE_ERROR);
        if (kz_define_accessor(ctx, o, rt->atom[KZ_ATOM_callee], t, t, 0)) return JS_EXCEPTION;
    }
    return o;
}

/* ================================================================== Function.prototype */

static JSValue fn_proto_call(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

static JSValue fn_call(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    if (!kz_is_callable(this_val)) return kz_throw_type(ctx, "Function.prototype.call called on a non-function");
    return kz_call(ctx, this_val, argc ? argv[0] : JS_UNDEFINED, argc > 0 ? argc - 1 : 0, argv + 1);
}

static JSValue fn_apply(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    (void)argc;
    if (!kz_is_callable(this_val)) return kz_throw_type(ctx, "Function.prototype.apply called on a non-function");
    if (JV_IS_NULLISH(argv[1])) return kz_call(ctx, this_val, argv[0], 0, NULL);
    KZ_ROOT_VEC(ctx, args);
    if (kz_create_list_from_array_like(ctx, argv[1], &args)) return JS_EXCEPTION;
    return kz_call(ctx, this_val, argv[0], (int)args.len, args.v);
}

static JSValue fn_bind(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!kz_is_callable(this_val)) return kz_throw_type(ctx, "Bind must be called on a function");
    KZ_ROOTS(ctx, r, 4);
    JSValue proto;
    if (kz_get_proto(ctx, JV_OBJ(this_val), &proto)) return JS_EXCEPTION;
    r[0] = proto;
    uint32_t nb = argc > 1 ? (uint32_t)argc - 1 : 0;
    JSValue *args = NULL;
    if (nb) {
        args = kz_malloc(ctx, nb * sizeof(JSValue));
        if (!args) return JS_EXCEPTION;
        memcpy(args, argv + 1, nb * sizeof(JSValue));
    }
    JSShape *s = kz_root_shape(ctx, r[0]);
    if (!s) { kz_free(rt, args); return JS_EXCEPTION; }
    JSObject *bf = kz_obj_alloc(ctx, KZ_CLASS_BOUND, s, 0);
    if (!bf) { kz_free(rt, args); return JS_EXCEPTION; }
    bf->h.flags |= OBJ_CALLABLE | (JV_OBJ(this_val)->h.flags & OBJ_CONSTRUCTOR);
    bf->u.bound.target = this_val;
    bf->u.bound.this_val = argc ? argv[0] : JS_UNDEFINED;
    bf->u.bound.args = args;
    bf->u.bound.argc = nb;
    r[1] = JV_MKOBJ(bf);
    /* length: max(0, target.length - args) when the target has an own numeric length */
    double len = 0;
    int h = kz_has_property(ctx, JV_OBJ(this_val), rt->atom[KZ_ATOM_length]);
    if (h < 0) return JS_EXCEPTION;
    KZ_ROOT_DESC(ctx, d);
    int own = kz_get_own_property(ctx, JV_OBJ(this_val), rt->atom[KZ_ATOM_length], &d);
    if (own < 0) return JS_EXCEPTION;
    if (own) {
        r[2] = kz_get(ctx, this_val, rt->atom[KZ_ATOM_length], this_val);
        if (r[2] == JS_EXCEPTION) return JS_EXCEPTION;
        if (JV_IS_NUMBER(r[2])) {
            double tl = jv_num(r[2]);
            if (tl == __builtin_inf()) len = tl;
            else if (tl == -__builtin_inf()) len = 0;
            else {
                double ti = tl != tl ? 0 : kz_math_trunc(tl);
                len = ti - nb;
                if (len < 0) len = 0;
            }
        }
    }
    if (kz_define_value_atom(ctx, r[1], KZ_ATOM_length, jv_number(len), KZ_PROP_C)) return JS_EXCEPTION;
    r[2] = kz_get(ctx, this_val, rt->atom[KZ_ATOM_name], this_val);
    if (r[2] == JS_EXCEPTION) return JS_EXCEPTION;
    if (!JV_IS_STRING(r[2])) r[2] = rt->atom[KZ_ATOM_empty];
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_puts(&sb, "bound ");
    kz_sb_put_value(&sb, r[2]);
    r[3] = kz_sb_finish(&sb);
    if (r[3] == JS_EXCEPTION) return JS_EXCEPTION;
    if (kz_define_value_atom(ctx, r[1], KZ_ATOM_name, r[3], KZ_PROP_C)) return JS_EXCEPTION;
    return r[1];
}

static JSValue fn_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!JV_IS_OBJECT(this_val) || !(JV_OBJ(this_val)->h.flags & OBJ_CALLABLE))
        return kz_throw_type(ctx, "Function.prototype.toString requires that 'this' be a Function");
    JSObject *f = JV_OBJ(this_val);
    if (kz_class(f) == KZ_CLASS_FUNCTION) {
        JSFunctionBytecode *b = f->u.func.b;
        if (JV_IS_INTERNAL(b->source)) {
            KzSource *src = JV_PTR(b->source);
            if (b->src_end <= src->len && b->src_start < b->src_end)
                return kz_str_from_utf8(ctx, src->text + b->src_start, b->src_end - b->src_start);
        }
    }
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_puts(&sb, "function ");
    if (kz_class(f) != KZ_CLASS_BOUND) {
        JSValue n = kz_func_name(ctx, f);
        if (JV_IS_STRING(n)) kz_sb_put_value(&sb, n);
    }
    kz_sb_puts(&sb, "() { [native code] }");
    return kz_sb_finish(&sb);
}

static JSValue fn_has_instance(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    int r = kz_ordinary_has_instance(ctx, this_val, argv[0]);
    if (r < 0) return JS_EXCEPTION;
    return JV_MKBOOL(r);
}

static JSValue throw_type_error_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return kz_throw_type(ctx, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
}

/* ------------------------------------------------------------------ the Function constructor */

JSValue kz_compile_function_text(JSContext *ctx, const char *prefix, JSValue params, JSValue body, int kind);

/* CreateDynamicFunction: kind 0 function, 1 generator, 2 async, 3 async generator */
JSValue kz_function_ctor_impl(JSContext *ctx, int argc, JSValue *argv, int kind, JSValue new_target) {
    KZ_ROOTS(ctx, r, 3);
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    for (int i = 0; i + 1 < argc; i++) {
        JSValue s = kz_to_string(ctx, argv[i]);
        if (s == JS_EXCEPTION) { kz_sb_free(&sb); return s; }
        if (i) kz_sb_putc(&sb, ',');
        kz_sb_put_value(&sb, s);
    }
    r[0] = kz_sb_finish(&sb);
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    if (argc) {
        r[1] = kz_to_string(ctx, argv[argc - 1]);
        if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    } else r[1] = ctx->rt->atom[KZ_ATOM_empty];
    static const char *const prefixes[4] = { "function", "function*", "async function", "async function*" };
    r[2] = kz_compile_function_text(ctx, prefixes[kind & 3], r[0], r[1], kind);
    if (r[2] == JS_EXCEPTION) return JS_EXCEPTION;
    /* a subclass of Function: the prototype comes from new.target */
    if (JV_IS_OBJECT(new_target) && new_target != kz_intr(ctx, KZ_I_FUNCTION)) {
        static const KzIntrinsic protos[4] = { KZ_I_FUNCTION_PROTO, KZ_I_GENERATOR_FUNCTION_PROTO, KZ_I_ASYNC_FUNCTION_PROTO, KZ_I_ASYNC_GENERATOR_FUNCTION_PROTO };
        JSValue p;
        if (kz_get_proto_from_ctor(ctx, new_target, protos[kind & 3], &p)) return JS_EXCEPTION;
        if (kz_obj_set_proto_raw(ctx, JV_OBJ(r[2]), p)) return JS_EXCEPTION;
    }
    return r[2];
}

static JSValue function_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    return kz_function_ctor_impl(ctx, argc, argv, magic, ctx->rt->new_target);
}

static const KzFuncDef fn_proto_funcs[] = {
    KZ_FN_ARGS("call", fn_call, 1, 1),
    KZ_FN_ARGS("apply", fn_apply, 2, 2),
    KZ_FN_ARGS("bind", fn_bind, 1, 1),
    KZ_FN("toString", fn_to_string, 0),
};

/* Function.prototype exists before anything else (every native function's prototype) */
int kz_init_function_proto(JSContext *ctx);
int kz_init_function_proto(JSContext *ctx) {
    JSShape *s = kz_root_shape(ctx, kz_intr(ctx, KZ_I_OBJECT_PROTO));
    if (!s) return -1;
    JSObject *fp = kz_obj_alloc(ctx, KZ_CLASS_NATIVE, s, 0);
    if (!fp) return -1;
    fp->h.flags |= OBJ_CALLABLE;
    fp->u.native.fn = fn_proto_call;
    fp->u.native.realm = ctx;
    ctx->intrinsic[KZ_I_FUNCTION_PROTO] = JV_MKOBJ(fp);
    return 0;
}

int kz_init_function(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_intr(ctx, KZ_I_FUNCTION_PROTO);
    if (kz_define_value_atom(ctx, r[0], KZ_ATOM_length, JV_MKINT(0), KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, r[0], KZ_ATOM_name, rt->atom[KZ_ATOM_empty], KZ_PROP_C)) return -1;
    if (kz_define_functions(ctx, r[0], fn_proto_funcs, KZ_COUNTOF(fn_proto_funcs))) return -1;
    r[1] = kz_new_native(ctx, fn_has_instance, kz_str_from_cstr(ctx, "[Symbol.hasInstance]"), 1, 0, 0, NULL);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_hasInstance], r[1], 0)) return -1;
    /* %ThrowTypeError%: frozen, and the poison pills on Function.prototype */
    r[1] = kz_new_native(ctx, throw_type_error_fn, rt->atom[KZ_ATOM_empty], 0, 0, 0, NULL);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_THROW_TYPE_ERROR] = r[1];
    if (kz_set_integrity(ctx, JV_OBJ(r[1]), true)) return -1;
    if (kz_define_accessor(ctx, r[0], rt->atom[KZ_ATOM_caller], r[1], r[1], KZ_PROP_C)) return -1;
    if (kz_define_accessor(ctx, r[0], rt->atom[KZ_ATOM_arguments], r[1], r[1], KZ_PROP_C)) return -1;
    r[1] = kz_make_ctor(ctx, function_ctor, "Function", 1, 0, KZ_I_FUNCTION_PROTO, KZ_I_FUNCTION);
    if (r[1] == JS_EXCEPTION) return -1;
    JV_OBJ(r[1])->u.native.nargs = 0;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Function, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}

/* the hidden constructors of generator and async functions */
static JSValue gen_function_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val;
    return kz_function_ctor_impl(ctx, argc, argv, magic, ctx->rt->new_target);
}

int kz_init_function_kinds(JSContext *ctx);
int kz_init_function_kinds(JSContext *ctx) {
    static const struct { const char *name; int kind; KzIntrinsic proto; KzIntrinsic ctor; const char *tag; } k[] = {
        { "GeneratorFunction", 1, KZ_I_GENERATOR_FUNCTION_PROTO, KZ_I_GENERATOR_FUNCTION, "GeneratorFunction" },
        { "AsyncFunction", 2, KZ_I_ASYNC_FUNCTION_PROTO, KZ_I_ASYNC_FUNCTION, "AsyncFunction" },
        { "AsyncGeneratorFunction", 3, KZ_I_ASYNC_GENERATOR_FUNCTION_PROTO, KZ_I_ASYNC_GENERATOR_FUNCTION, "AsyncGeneratorFunction" },
    };
    KZ_ROOTS(ctx, r, 2);
    for (size_t i = 0; i < KZ_COUNTOF(k); i++) {
        r[0] = kz_new_object_proto(ctx, kz_intr(ctx, KZ_I_FUNCTION_PROTO));
        if (r[0] == JS_EXCEPTION) return -1;
        ctx->intrinsic[k[i].proto] = r[0];
        r[1] = kz_make_ctor(ctx, gen_function_ctor, k[i].name, 1, k[i].kind, k[i].proto, k[i].ctor);
        if (r[1] == JS_EXCEPTION) return -1;
        JV_OBJ(r[1])->u.native.nargs = 0;
        if (kz_set_proto(ctx, JV_OBJ(r[1]), kz_intr(ctx, KZ_I_FUNCTION), true) < 0) return -1;
        /* the constructor link is not writable for these */
        if (kz_define_value_atom(ctx, r[0], KZ_ATOM_constructor, r[1], KZ_PROP_C)) return -1;
        JSValue tag = kz_str_from_cstr(ctx, k[i].tag);
        if (tag == JS_EXCEPTION) return -1;
        if (kz_define_value(ctx, r[0], ctx->rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    }
    return 0;
}
