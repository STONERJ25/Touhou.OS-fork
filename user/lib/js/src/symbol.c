/*
 * symbol.c: the Symbol constructor, the global symbol registry and Symbol.prototype (ECMA-262 20.4).
 *
 * The registry (Symbol.for) is one per runtime, as the specification's GlobalSymbolRegistry is shared by all realms:
 * a null-prototype dictionary object from key strings to symbols.  A registered symbol's description is its key.
 */
#include "jsint.h"

static JSValue new_symbol(JSContext *ctx, JSValue desc, uint16_t flags) {
    KZ_ROOT(ctx, d);
    d = desc;
    JSSymbol *s = kz_gc_new(ctx, KZ_GC_SYMBOL, sizeof(JSSymbol));
    if (!s) return JS_EXCEPTION;
    s->description = d;
    s->h.flags = flags;
    s->hash = (uint32_t)((uintptr_t)s >> 4) * 2654435761u;
    return JV_MKSYM(s);
}

static JSValue symbol_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (ctx->rt->new_target != JS_UNDEFINED) return kz_throw_type(ctx, "Symbol is not a constructor");
    KZ_ROOT(ctx, d);
    d = JS_UNDEFINED;
    if (argv[0] != JS_UNDEFINED) {
        d = kz_to_string(ctx, argv[0]);
        if (d == JS_EXCEPTION) return d;
    }
    return new_symbol(ctx, d, 0);
}

static JSValue symbol_for(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_string(ctx, argv[0]);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_atom_from_str(ctx, r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (!JV_IS_OBJECT(rt->symbol_registry)) {
        JSValue reg = kz_new_object_proto(ctx, JS_NULL);
        if (reg == JS_EXCEPTION) return reg;
        rt->symbol_registry = reg;
    }
    /* the registry is keyed by the string itself: "1" must not become the index 1 and collide with nothing else,
       which it cannot (the dictionary only ever holds these keys) */
    KZ_ROOT_DESC(ctx, d);
    int h = kz_get_own_property(ctx, JV_OBJ(rt->symbol_registry), r[1], &d);
    if (h < 0) return JS_EXCEPTION;
    if (h) return d.value;
    r[2] = new_symbol(ctx, r[0], SYM_REGISTERED);
    if (r[2] == JS_EXCEPTION) return r[2];
    if (kz_obj_add_prop(ctx, JV_OBJ(rt->symbol_registry), r[1], KZ_PROP_WEC, r[2])) return JS_EXCEPTION;
    return r[2];
}

static JSValue symbol_key_for(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    if (!JV_IS_SYMBOL(argv[0])) return kz_throw_type(ctx, "Symbol.keyFor: %s is not a symbol", "argument");
    JSSymbol *s = JV_SYM(argv[0]);
    if (s->h.flags & SYM_REGISTERED) return s->description;
    return JS_UNDEFINED;
}

static int this_symbol(JSContext *ctx, JSValue v, JSValue *out) {
    if (JV_IS_SYMBOL(v)) { *out = v; return 0; }
    if (kz_is_class(v, KZ_CLASS_SYMBOL)) { *out = JV_OBJ(v)->u.prim; return 0; }
    kz_throw_type(ctx, "Symbol.prototype method called on an incompatible receiver");
    return -1;
}

static JSValue symbol_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue s;
    if (this_symbol(ctx, this_val, &s)) return JS_EXCEPTION;
    return kz_symbol_descriptive_string(ctx, s);
}

static JSValue symbol_value_of(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue s;
    if (this_symbol(ctx, this_val, &s)) return JS_EXCEPTION;
    return s;
}

static JSValue symbol_description(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSValue s;
    if (this_symbol(ctx, this_val, &s)) return JS_EXCEPTION;
    return JV_SYM(s)->description;
}

static const KzFuncDef symbol_funcs[] = {
    KZ_FN("for", symbol_for, 1),
    KZ_FN("keyFor", symbol_key_for, 1),
};

static const KzFuncDef symbol_proto_funcs[] = {
    KZ_FN("toString", symbol_to_string, 0),
    KZ_FN("valueOf", symbol_value_of, 0),
    KZ_GETTER("description", symbol_description),
};

int kz_init_symbol(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    static const char *const names[KZ_SYM_COUNT] = { "asyncIterator", "hasInstance", "isConcatSpreadable", "iterator",
                                                     "match", "matchAll", "replace", "search", "species", "split",
                                                     "toPrimitive", "toStringTag", "unscopables" };
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_SYMBOL_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, symbol_ctor, "Symbol", 0, 0, KZ_I_SYMBOL_PROTO, KZ_I_SYMBOL);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], symbol_funcs, KZ_COUNTOF(symbol_funcs))) return -1;
    for (int i = 0; i < KZ_SYM_COUNT; i++) {
        JSValue k = kz_atom(ctx, names[i]);
        if (k == JS_EXCEPTION || kz_define_value(ctx, r[1], k, rt->sym[i], 0)) return -1;
    }
    if (kz_define_functions(ctx, r[0], symbol_proto_funcs, KZ_COUNTOF(symbol_proto_funcs))) return -1;
    r[2] = kz_new_native_c(ctx, symbol_value_of, "[Symbol.toPrimitive]", 1, 0);
    if (r[2] == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_toPrimitive], r[2], KZ_PROP_C)) return -1;
    r[2] = kz_atom(ctx, "Symbol");
    if (r[2] == JS_EXCEPTION || kz_define_value(ctx, r[0], rt->sym[KZ_SYM_toStringTag], r[2], KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Symbol, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
