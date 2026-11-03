/*
 * stubs.c: placeholders for the parts of the engine that are not written yet (each one is replaced by its real
 * module as it lands; this file shrinks to nothing).
 */
#include "jsint.h"

#define STUB_INIT(name) int name(JSContext *ctx) { (void)ctx; return 0; }
STUB_INIT(kz_init_regexp)
STUB_INIT(kz_init_date)
STUB_INIT(kz_init_proxy)
STUB_INIT(kz_init_typed)
STUB_INIT(kz_init_bigint)
STUB_INIT(kz_init_intl)

static JSValue unsupported(JSContext *ctx, const char *what) { return kz_throw_type(ctx, "%s is not supported yet", what); }

/* BigInt */
JSValue kz_bigint_from_double(JSContext *ctx, double d) { (void)d; return unsupported(ctx, "BigInt"); }
JSValue kz_bigint_from_int64(JSContext *ctx, int64_t v) { (void)v; return unsupported(ctx, "BigInt"); }
JSValue kz_bigint_from_string(JSContext *ctx, JSString *s) { (void)s; return unsupported(ctx, "BigInt"); }
JSValue kz_bigint_to_string(JSContext *ctx, JSValue b, int radix) { (void)b; (void)radix; return unsupported(ctx, "BigInt"); }
double kz_bigint_to_double(JSValue b) { (void)b; return 0; }
int kz_bigint_cmp(JSValue a, JSValue b) { (void)a; (void)b; return 0; }
int kz_bigint_cmp_double(JSValue a, double d) { (void)a; (void)d; return 2; }
JSValue kz_bigint_binary(JSContext *ctx, int op, JSValue a, JSValue b) { (void)op; (void)a; (void)b; return unsupported(ctx, "BigInt"); }
JSValue kz_bigint_neg(JSContext *ctx, JSValue a) { (void)a; return unsupported(ctx, "BigInt"); }
JSValue kz_bigint_not(JSContext *ctx, JSValue a) { (void)a; return unsupported(ctx, "BigInt"); }
bool kz_bigint_is_zero(JSValue a) { (void)a; return true; }
bool kz_bigint_eq(JSValue a, JSValue b) { return a == b; }
uint32_t kz_bigint_hash(JSValue a) { (void)a; return 0; }
JSValue kz_bigint_literal(JSContext *ctx, const char *digits, size_t len, int radix) { (void)digits; (void)len; (void)radix; return unsupported(ctx, "BigInt"); }

/* RegExp */
JSValue kz_regexp_create(JSContext *ctx, JSValue pattern, JSValue flags) { (void)pattern; (void)flags; return unsupported(ctx, "RegExp"); }
int kz_regexp_syntax_check(JSContext *ctx, JSString *pattern, JSString *flags, char *err, size_t errcap) {
    (void)ctx; (void)pattern; (void)flags; (void)err; (void)errcap;
    return 0;
}
void kz_regexp_free(JSRuntime *rt, KzRegExpCode *c) { (void)rt; (void)c; }

/* proxies */
JSValue kz_proxy_call(JSContext *ctx, JSObject *p, JSValue this_val, int argc, JSValue *argv) {
    (void)p; (void)this_val; (void)argc; (void)argv;
    return unsupported(ctx, "Proxy");
}
JSValue kz_proxy_construct(JSContext *ctx, JSObject *p, int argc, JSValue *argv, JSValue new_target) {
    (void)p; (void)argc; (void)argv; (void)new_target;
    return unsupported(ctx, "Proxy");
}

/* typed arrays */
bool kz_is_detached(JSObject *ta) { (void)ta; return false; }
uint32_t kz_typed_length(JSObject *ta) { (void)ta; return 0; }

/* modules */
void kz_module_trace(JSRuntime *rt, KzModule *m) { (void)rt; (void)m; }
void kz_module_free(JSRuntime *rt, KzModule *m) { (void)rt; (void)m; }
JSValue kz_dynamic_import(JSContext *ctx, JSValue specifier, JSValue referrer) { (void)specifier; (void)referrer; return unsupported(ctx, "import()"); }
JSValue kz_import_meta(JSContext *ctx, JSFunctionBytecode *b) { (void)b; return unsupported(ctx, "import.meta"); }
JSValue kz_module_evaluate_source(JSContext *ctx, const char *src, size_t len, const char *filename) {
    (void)src; (void)len; (void)filename;
    return unsupported(ctx, "modules");
}

