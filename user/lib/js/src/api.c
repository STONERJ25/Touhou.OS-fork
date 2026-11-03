/*
 * api.c: the embedding API (include/js.h) over the internals, plus the runtime and context lifecycle and the job
 * queue.  Every value an API function returns is registered in the context's current scope (ctx->api_scope, a
 * rooted vector), which is what lets an embedder hold JSValues in C variables without thinking about the collector.
 */
#include "jsint.h"
#include <stdarg.h>
#include <stdlib.h>

extern void kz_keywords_init(JSRuntime *rt);
extern int kz_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ------------------------------------------------------------------ the default allocator */

static void *def_malloc(void *opaque, size_t n) { (void)opaque; return malloc(n); }
static void *def_realloc(void *opaque, void *p, size_t n) { (void)opaque; return realloc(p, n); }
static void def_free(void *opaque, void *p) { (void)opaque; free(p); }

/* ------------------------------------------------------------------ scopes */

static JSValue keep(JSContext *ctx, JSValue v) {
    if (v == JS_EXCEPTION || !JV_IS_HEAP(v)) return v;
    if (kz_vec_push(ctx, &ctx->api_scope, v)) return JS_EXCEPTION;
    return v;
}

int js__scope_open_internal(JSContext *ctx);
void js__scope_close_internal(JSContext *ctx, int mark);
int js__scope_open_internal(JSContext *ctx) { return (int)ctx->api_scope.len; }
void js__scope_close_internal(JSContext *ctx, int mark) {
    if (mark >= 0 && (uint32_t)mark <= ctx->api_scope.len) ctx->api_scope.len = (uint32_t)mark;
}

int js_scope_open(JSContext *ctx) { return js__scope_open_internal(ctx); }
void js_scope_close(JSContext *ctx, int mark) { js__scope_close_internal(ctx, mark); }
JSValue js_scope_keep(JSContext *ctx, JSValue v) { return keep(ctx, v); }
void js_pin(JSContext *ctx, JSValue v) { kz_pin(ctx->rt, v); }
void js_unpin(JSContext *ctx, JSValue v) { kz_unpin(ctx->rt, v); }

/* ------------------------------------------------------------------ the runtime */

static void init_symbols(JSRuntime *rt, JSContext *tmp) {
    static const int atoms[KZ_SYM_COUNT] = {
        KZ_ATOM_Symbol_asyncIterator, KZ_ATOM_Symbol_hasInstance, KZ_ATOM_Symbol_isConcatSpreadable,
        KZ_ATOM_Symbol_iterator, KZ_ATOM_Symbol_match, KZ_ATOM_Symbol_matchAll, KZ_ATOM_Symbol_replace,
        KZ_ATOM_Symbol_search, KZ_ATOM_Symbol_species, KZ_ATOM_Symbol_split, KZ_ATOM_Symbol_toPrimitive,
        KZ_ATOM_Symbol_toStringTag, KZ_ATOM_Symbol_unscopables
    };
    for (int i = 0; i < KZ_SYM_COUNT; i++) {
        JSSymbol *s = kz_gc_new(tmp, KZ_GC_SYMBOL, sizeof(JSSymbol));
        if (!s) return;
        s->description = rt->atom[atoms[i]];
        s->h.flags = SYM_WELLKNOWN;
        s->hash = (uint32_t)(i * 2654435761u);
        rt->sym[i] = JV_MKSYM(s);
    }
}

JSRuntime *js_runtime_new(const JSRuntimeOptions *opts) {
    JSRuntimeOptions o = { 0 };
    if (opts) o = *opts;
    if (!o.malloc_fn || !o.free_fn || !o.realloc_fn) { o.malloc_fn = def_malloc; o.realloc_fn = def_realloc; o.free_fn = def_free; }
    JSRuntime *rt = o.malloc_fn(o.malloc_opaque, sizeof(JSRuntime));
    if (!rt) return NULL;
    memset(rt, 0, sizeof *rt);
    rt->sys_malloc = o.malloc_fn;
    rt->sys_realloc = o.realloc_fn;
    rt->sys_free = o.free_fn;
    rt->sys_opaque = o.malloc_opaque;
    rt->mem_limit = o.memory_limit;
    rt->exception = JS_UNINIT;
    rt->new_target = JS_UNDEFINED;
    rt->oom_error = JS_UNDEFINED;
    rt->symbol_registry = JS_UNDEFINED;
    rt->suspend_value = JS_UNDEFINED;
    rt->interrupt_count = 10000;
    rt->max_depth = o.max_call_depth > 0 ? o.max_call_depth : 10000;
    size_t vs = o.stack_size ? o.stack_size : (1u << 20);
    rt->vstack = rt->sys_malloc(rt->sys_opaque, vs);
    if (!rt->vstack) { rt->sys_free(rt->sys_opaque, rt); return NULL; }
    rt->vstack_top = rt->vstack;
    rt->vstack_end = rt->vstack + vs;
    rt->c_stack_size = o.c_stack_size ? o.c_stack_size : (512u << 10);
    js_runtime_set_stack_base(rt);
    uint64_t seed = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)rt;
    rt->rand_state[0] = seed ? seed : 1;
    rt->rand_state[1] = seed * 0xBF58476D1CE4E5B9ULL + 1;
    if (!kz_rt_init_alloc(rt) || !kz_atoms_init(rt)) { js_runtime_free(rt); return NULL; }
    kz_keywords_init(rt);
    JSContext tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.rt = rt;
    init_symbols(rt, &tmp);
    if (!JV_IS_SYMBOL(rt->sym[KZ_SYM_COUNT - 1])) { js_runtime_free(rt); return NULL; }
    rt->exception = JS_UNINIT;
    return rt;
}

void js_runtime_set_stack_base(JSRuntime *rt) {
    /* the frame address, not a local's: with AddressSanitizer's fake stacks a local may live on the heap */
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    rt->c_stack_limit = here > rt->c_stack_size ? here - rt->c_stack_size : 0;
}

void js_runtime_free(JSRuntime *rt) {
    if (!rt) return;
    while (rt->contexts) js_context_free(rt->contexts);
    /* pending jobs */
    KzJob *j = rt->job_head;
    while (j) { KzJob *n = j->next; rt->sys_free(rt->sys_opaque, j); j = n; }
    rt->job_head = rt->job_tail = NULL;
    kz_rt_free_all(rt);
    if (rt->atom_table) rt->sys_free(rt->sys_opaque, rt->atom_table);
    if (rt->root_shapes) rt->sys_free(rt->sys_opaque, rt->root_shapes);
    if (rt->host_classes) rt->sys_free(rt->sys_opaque, (void *)rt->host_classes);
    if (rt->vstack) rt->sys_free(rt->sys_opaque, rt->vstack);
    rt->sys_free(rt->sys_opaque, rt);
}

void js_runtime_set_memory_limit(JSRuntime *rt, size_t bytes) { rt->mem_limit = bytes; }
void js_runtime_set_interrupt_handler(JSRuntime *rt, int (*fn)(JSRuntime *, void *), void *opaque) {
    rt->interrupt = fn;
    rt->interrupt_opaque = opaque;
}
void js_runtime_set_host_hooks(JSRuntime *rt, const JSHostHooks *hooks) {
    if (hooks) rt->hooks = *hooks;
    else memset(&rt->hooks, 0, sizeof rt->hooks);
}
void js_gc(JSRuntime *rt) { kz_gc_collect(rt); }
void js_runtime_set_gc_stress(JSRuntime *rt, bool on) { js_runtime_set_gc_stress_every(rt, on ? 1 : 0); }
void js_runtime_set_gc_stress_every(JSRuntime *rt, unsigned every) {
    rt->gc_stress = every != 0;
    rt->gc_stress_every = every;
    rt->gc_stress_count = 0;
}

void js_memory_usage(JSRuntime *rt, JSMemoryUsage *out) {
    out->heap_bytes = rt->mem_used;
    out->peak_bytes = rt->mem_peak;
    out->limit = rt->mem_limit;
    out->gc_count = rt->gc_count;
    out->objects = rt->live_objects;
    out->strings = rt->live_strings;
}

/* ------------------------------------------------------------------ contexts */

JSContext *js_context_new(JSRuntime *rt) {
    JSContext *ctx = rt->sys_malloc(rt->sys_opaque, sizeof(JSContext));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof *ctx);
    ctx->rt = rt;
    ctx->global = ctx->global_lex = JS_UNDEFINED;
    for (int i = 0; i < KZ_I_COUNT; i++) ctx->intrinsic[i] = JS_UNDEFINED;
    ctx->template_cache = JS_UNDEFINED;
    ctx->next = rt->contexts;
    rt->contexts = ctx;
    bool saved = rt->gc_stress;
    rt->gc_stress = false;                  /* building a realm under stress collection would take minutes */
    bool ok = kz_realm_init(ctx);
    rt->gc_stress = saved;
    if (!ok) {
        kz_take_exception(ctx);
        js_context_free(ctx);
        return NULL;
    }
    return ctx;
}

void js_context_free(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    for (JSContext **p = &rt->contexts; *p; p = &(*p)->next)
        if (*p == ctx) { *p = ctx->next; break; }
    if (ctx->api_scope.v) kz_free(rt, ctx->api_scope.v);
    if (ctx->modules.v) kz_free(rt, ctx->modules.v);
    /* jobs of this realm can no longer run */
    KzJob **jp = &rt->job_head;
    rt->job_tail = NULL;
    while (*jp) {
        if ((*jp)->ctx == ctx) { KzJob *d = *jp; *jp = d->next; rt->sys_free(rt->sys_opaque, d); continue; }
        rt->job_tail = *jp;
        jp = &(*jp)->next;
    }
    rt->sys_free(rt->sys_opaque, ctx);
    if (!rt->contexts) kz_gc_collect(rt);
}

JSRuntime *js_context_runtime(JSContext *ctx) { return ctx->rt; }
void js_context_set_opaque(JSContext *ctx, void *opaque) { ctx->opaque = opaque; }
void *js_context_opaque(JSContext *ctx) { return ctx->opaque; }
JSValue js_global_object(JSContext *ctx) { return ctx->global; }

/* ------------------------------------------------------------------ jobs */

int kz_enqueue_job(JSContext *ctx, KzNative *fn, int argc, const JSValue *argv) {
    JSRuntime *rt = ctx->rt;
    KzJob *j = rt->sys_malloc(rt->sys_opaque, sizeof(KzJob) + (size_t)argc * sizeof(JSValue));
    if (!j) { kz_throw_oom(ctx); return -1; }
    j->next = NULL;
    j->ctx = ctx;
    j->fn = fn;
    j->argc = (uint32_t)argc;
    for (int i = 0; i < argc; i++) j->args[i] = argv[i];
    if (rt->job_tail) rt->job_tail->next = j; else rt->job_head = j;
    rt->job_tail = j;
    return 0;
}

bool js_has_jobs(JSRuntime *rt) { return rt->job_head != NULL || rt->finreg_npending; }

static void report(JSContext *ctx, JSValue err, bool rejection) {
    JSRuntime *rt = ctx->rt;
    if (rt->hooks.report_error) {
        int mark = js_scope_open(ctx);
        keep(ctx, err);
        rt->hooks.report_error(ctx, err, rejection, rt->hooks.opaque);
        js_scope_close(ctx, mark);
    }
}

void kz_report_unhandled(JSContext *ctx, JSValue err, bool rejection);
void kz_report_unhandled(JSContext *ctx, JSValue err, bool rejection) { report(ctx, err, rejection); }

int js_run_jobs(JSRuntime *rt, int max) {
    int n = 0;
    while (rt->job_head && (max <= 0 || n < max)) {
        KzJob *j = rt->job_head;
        rt->job_head = j->next;
        if (!rt->job_head) rt->job_tail = NULL;
        JSContext *ctx = j->ctx;
        /* the arguments stay rooted while the job runs */
        KzRootFrame rf = { rt->roots, rt, j->args, j->argc, NULL };
        rt->roots = &rf;
        int mark = js_scope_open(ctx);
        JSValue r = j->fn(ctx, JS_UNDEFINED, (int)j->argc, j->args, 0);
        if (r == JS_EXCEPTION) {
            bool fatal = rt->exc_uncatchable;
            JSValue e = kz_take_exception(ctx);
            KZ_ROOT(ctx, re);
            re = e;
            if (!fatal) report(ctx, re, false);
        }
        js_scope_close(ctx, mark);
        rt->roots = rf.prev;
        rt->sys_free(rt->sys_opaque, j);
        rt->kept_alive.len = 0;             /* ClearKeptObjects */
        n++;
    }
    if (!rt->job_head) kz_report_rejections(rt);
    /* FinalizationRegistry cleanups for targets the collector found dead */
    if (rt->finreg_npending && rt->contexts && (max <= 0 || n < max)) {
        JSContext *ctx = rt->contexts;
        int mark = js_scope_open(ctx);
        int k = kz_run_finalizers(ctx);
        if (k < 0) {
            bool fatal = rt->exc_uncatchable;
            JSValue e = kz_take_exception(ctx);
            KZ_ROOT(ctx, re);
            re = e;
            if (!fatal) report(ctx, re, false);
        } else n += k;
        js_scope_close(ctx, mark);
    }
    return n;
}

/* ------------------------------------------------------------------ evaluation */

extern JSValue kz_module_evaluate_source(JSContext *ctx, const char *src, size_t len, const char *filename);

JSValue js_eval(JSContext *ctx, const char *utf8, size_t len, const char *filename, int flags) {
    if (len == (size_t)-1) len = strlen(utf8);
    JSValue r;
    if (flags & JS_EVAL_MODULE) r = kz_module_evaluate_source(ctx, utf8, len, filename);
    else r = kz_eval_script(ctx, utf8, len, filename, flags & (JS_EVAL_STRICT | JS_EVAL_COMPILE), NULL, 0);
    return keep(ctx, r);
}

/* ------------------------------------------------------------------ exceptions */

bool js_has_exception(JSContext *ctx) { return kz_has_exception(ctx); }
JSValue js_take_exception(JSContext *ctx) { return keep(ctx, kz_take_exception(ctx)); }
JSValue js_throw(JSContext *ctx, JSValue error) { return kz_throw(ctx, error); }

JSValue js_new_error(JSContext *ctx, JSErrorKind kind, const char *message) {
    KZ_ROOT(ctx, m);
    m = kz_str_from_cstr(ctx, message ? message : "");
    if (m == JS_EXCEPTION) return m;
    return keep(ctx, kz_new_error_value(ctx, (KzErr)kind, m));
}

JSValue js_throw_error(JSContext *ctx, JSErrorKind kind, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    kz_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return kz_throw_error(ctx, (KzErr)kind, "%s", msg);
}

char *js_error_report(JSContext *ctx, JSValue error) {
    int mark = js_scope_open(ctx);
    KZ_ROOT(ctx, s);
    s = JS_UNDEFINED;
    if (JV_IS_OBJECT(error)) {
        JSValue st = kz_get(ctx, error, ctx->rt->atom[KZ_ATOM_stack], error);
        if (st != JS_EXCEPTION && JV_IS_STRING(st)) s = st;
        else if (st == JS_EXCEPTION) kz_take_exception(ctx);
    }
    if (!JV_IS_STRING(s)) {
        JSValue t = kz_to_string(ctx, error);
        if (t == JS_EXCEPTION) { kz_take_exception(ctx); t = kz_to_string_noexc(ctx, error); }
        s = t;
    }
    char *out = NULL;
    if (JV_IS_STRING(s)) {
        extern char *kz_str_to_utf8_replace(JSContext *ctx, JSValue v, size_t *plen);
        out = kz_str_to_utf8_replace(ctx, s, NULL);
        if (!out) kz_take_exception(ctx);
    }
    js_scope_close(ctx, mark);
    return out;
}

/* ------------------------------------------------------------------ values */

bool js_is_undefined(JSValue v) { return v == JS_UNDEFINED; }
bool js_is_null(JSValue v) { return v == JS_NULL; }
bool js_is_bool(JSValue v) { return JV_IS_BOOL(v); }
bool js_is_number(JSValue v) { return JV_IS_NUMBER(v); }
bool js_is_string(JSValue v) { return JV_IS_STRING(v); }
bool js_is_symbol(JSValue v) { return JV_IS_SYMBOL(v); }
bool js_is_bigint(JSValue v) { return JV_IS_BIGINT(v); }
bool js_is_object(JSValue v) { return JV_IS_OBJECT(v); }
bool js_is_function(JSContext *ctx, JSValue v) { (void)ctx; return kz_is_callable(v); }
bool js_is_constructor(JSContext *ctx, JSValue v) { (void)ctx; return kz_is_constructor(v); }
bool js_is_array(JSContext *ctx, JSValue v) {
    int r = kz_is_array(ctx, v);
    if (r < 0) { kz_take_exception(ctx); return false; }
    return r;
}
bool js_is_error(JSContext *ctx, JSValue v) { (void)ctx; return kz_is_class(v, KZ_CLASS_ERROR); }
bool js_is_promise(JSContext *ctx, JSValue v) { (void)ctx; return kz_is_class(v, KZ_CLASS_PROMISE); }

const char *js_typeof(JSContext *ctx, JSValue v) {
    JSValue t = kz_typeof(ctx, v);
    JSRuntime *rt = ctx->rt;
    static const struct { int atom; const char *s; } names[] = {
        { KZ_ATOM_undefined, "undefined" }, { KZ_ATOM_object, "object" }, { KZ_ATOM_boolean, "boolean" },
        { KZ_ATOM_number, "number" }, { KZ_ATOM_string, "string" }, { KZ_ATOM_symbol, "symbol" },
        { KZ_ATOM_bigint, "bigint" }, { KZ_ATOM_function, "function" },
    };
    for (size_t i = 0; i < KZ_COUNTOF(names); i++) if (rt->atom[names[i].atom] == t) return names[i].s;
    return "object";
}

JSValue js_bool(bool b) { return JV_MKBOOL(b); }
JSValue js_number(double d) { return jv_number(d); }
JSValue js_int(int32_t i) { return JV_MKINT(i); }
double js_number_value(JSValue v) { return JV_IS_NUMBER(v) ? jv_num(v) : __builtin_nan(""); }
bool js_bool_value(JSValue v) { return v == JS_TRUE; }

JSValue js_string(JSContext *ctx, const char *utf8, size_t len) {
    if (len == (size_t)-1) len = strlen(utf8);
    return keep(ctx, kz_str_from_utf8(ctx, utf8, len));
}

JSValue js_string_utf16(JSContext *ctx, const uint16_t *s, size_t len) {
    if (len > KZ_STRING_MAX) return kz_throw_range(ctx, "Invalid string length");
    return keep(ctx, kz_str_from_utf16(ctx, s, (uint32_t)len));
}

JSValue js_new_object(JSContext *ctx) { return keep(ctx, kz_new_object(ctx)); }
JSValue js_new_object_proto(JSContext *ctx, JSValue proto) {
    if (!JV_IS_OBJECT(proto) && proto != JS_NULL) return kz_throw_type(ctx, "prototype must be an object or null");
    return keep(ctx, kz_new_object_proto(ctx, proto));
}
JSValue js_new_array(JSContext *ctx, uint32_t length) { return keep(ctx, kz_new_array(ctx, length)); }

JSValue js_new_symbol(JSContext *ctx, const char *description) {
    KZ_ROOT(ctx, d);
    d = JS_UNDEFINED;
    if (description) {
        d = kz_str_from_cstr(ctx, description);
        if (d == JS_EXCEPTION) return d;
    }
    JSSymbol *s = kz_gc_new(ctx, KZ_GC_SYMBOL, sizeof(JSSymbol));
    if (!s) return JS_EXCEPTION;
    s->description = d;
    s->hash = (uint32_t)((uintptr_t)s >> 4);
    return keep(ctx, JV_MKSYM(s));
}

JSValue js_to_string(JSContext *ctx, JSValue v) { return keep(ctx, kz_to_string(ctx, v)); }
JSValue js_to_number_value(JSContext *ctx, JSValue v) { return keep(ctx, kz_to_number_value(ctx, v)); }
int js_to_number(JSContext *ctx, JSValue v, double *out) { return kz_to_number(ctx, v, out); }
int js_to_int32(JSContext *ctx, JSValue v, int32_t *out) { return kz_to_int32(ctx, v, out); }
int js_to_uint32(JSContext *ctx, JSValue v, uint32_t *out) { return kz_to_uint32(ctx, v, out); }
int js_to_index(JSContext *ctx, JSValue v, uint64_t *out) { return kz_to_index(ctx, v, out); }
bool js_to_bool(JSContext *ctx, JSValue v) { (void)ctx; return kz_to_bool(v); }
JSValue js_to_object(JSContext *ctx, JSValue v) { return keep(ctx, kz_to_object(ctx, v)); }

char *js_to_utf8(JSContext *ctx, JSValue v, size_t *len) {
    KZ_ROOT(ctx, s);
    s = kz_to_string(ctx, v);
    if (s == JS_EXCEPTION) return NULL;
    extern char *kz_str_to_utf8_replace(JSContext *ctx, JSValue v, size_t *plen);
    return kz_str_to_utf8_replace(ctx, s, len);
}

void js_free_utf8(JSContext *ctx, char *s) { if (s) kz_free(ctx->rt, s); }

size_t js_string_length(JSValue str) { return JV_IS_STRING(str) ? kz_str_len(JV_STR(str)) : 0; }

size_t js_string_copy_utf16(JSValue str, uint16_t *out, size_t max) {
    if (!JV_IS_STRING(str)) return 0;
    JSString *s = JV_STR(str);
    uint32_t n = kz_str_len(s);
    if (s->h.flags & STR_ROPE) return n;    /* (callers flatten through js_to_string first) */
    for (uint32_t i = 0; i < n && i < max; i++) out[i] = (uint16_t)kz_str_at(s, i);
    return n;
}

bool js_strict_equals(JSContext *ctx, JSValue a, JSValue b) {
    int r = kz_strict_eq(ctx, a, b);
    if (r < 0) { kz_take_exception(ctx); return false; }
    return r;
}

bool js_same_value(JSContext *ctx, JSValue a, JSValue b) {
    int r = kz_same_value(ctx, a, b);
    if (r < 0) { kz_take_exception(ctx); return false; }
    return r;
}

/* ------------------------------------------------------------------ properties */

static JSValue name_key(JSContext *ctx, const char *name) {
    JSValue s = kz_str_from_cstr(ctx, name);
    if (s == JS_EXCEPTION) return s;
    KZ_ROOT(ctx, rs);
    rs = s;
    return kz_key_from_value(ctx, rs);
}

JSValue js_get(JSContext *ctx, JSValue obj, JSValue key) {
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, key);
    if (k == JS_EXCEPTION) return k;
    return keep(ctx, kz_get(ctx, obj, k, obj));
}

JSValue js_get_str(JSContext *ctx, JSValue obj, const char *name) {
    KZ_ROOT(ctx, k);
    k = name_key(ctx, name);
    if (k == JS_EXCEPTION) return k;
    return keep(ctx, kz_get(ctx, obj, k, obj));
}

JSValue js_get_index(JSContext *ctx, JSValue obj, uint32_t index) { return keep(ctx, kz_get_index(ctx, obj, index)); }

int js_set(JSContext *ctx, JSValue obj, JSValue key, JSValue value) {
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, key);
    if (k == JS_EXCEPTION) return -1;
    return kz_set(ctx, obj, k, value, obj, true) < 0 ? -1 : 0;
}

int js_set_str(JSContext *ctx, JSValue obj, const char *name, JSValue value) {
    KZ_ROOT(ctx, k);
    k = name_key(ctx, name);
    if (k == JS_EXCEPTION) return -1;
    return kz_set(ctx, obj, k, value, obj, true) < 0 ? -1 : 0;
}

int js_set_index(JSContext *ctx, JSValue obj, uint32_t index, JSValue value) {
    return kz_set_index(ctx, obj, index, value, true) < 0 ? -1 : 0;
}

int js_has(JSContext *ctx, JSValue obj, JSValue key) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "not an object"); return -1; }
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, key);
    if (k == JS_EXCEPTION) return -1;
    return kz_has_property(ctx, JV_OBJ(obj), k);
}

int js_delete(JSContext *ctx, JSValue obj, JSValue key) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "not an object"); return -1; }
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, key);
    if (k == JS_EXCEPTION) return -1;
    return kz_delete_property(ctx, JV_OBJ(obj), k, false);
}

int js_define(JSContext *ctx, JSValue obj, JSValue key, JSValue value, JSValue getter, JSValue setter, int flags) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "not an object"); return -1; }
    KZ_ROOT(ctx, k);
    k = kz_to_property_key(ctx, key);
    if (k == JS_EXCEPTION) return -1;
    if (kz_is_callable(getter) || kz_is_callable(setter))
        return kz_define_accessor(ctx, obj, k, getter, setter, (uint32_t)flags & (KZ_PROP_E | KZ_PROP_C));
    return kz_define_value(ctx, obj, k, value, (uint32_t)flags & KZ_PROP_WEC);
}

int js_define_str(JSContext *ctx, JSValue obj, const char *name, JSValue value, int flags) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "not an object"); return -1; }
    KZ_ROOT(ctx, k);
    k = name_key(ctx, name);
    if (k == JS_EXCEPTION) return -1;
    return kz_define_value(ctx, obj, k, value, (uint32_t)flags & KZ_PROP_WEC);
}

JSValue js_keys(JSContext *ctx, JSValue obj) {
    JSValue o = kz_to_object(ctx, obj);
    if (o == JS_EXCEPTION) return o;
    KZ_ROOT(ctx, ro);
    ro = o;
    KZ_ROOT_VEC(ctx, keys);
    if (kz_own_keys(ctx, JV_OBJ(ro), &keys, KZ_KEYS_STRINGS | KZ_KEYS_ENUM)) return JS_EXCEPTION;
    for (uint32_t i = 0; i < keys.len; i++) {
        JSValue s = kz_key_to_value(ctx, keys.v[i]);
        if (s == JS_EXCEPTION) return s;
        keys.v[i] = s;
    }
    return keep(ctx, kz_new_array_from(ctx, keys.len, keys.v));
}

JSValue js_get_prototype(JSContext *ctx, JSValue obj) {
    if (!JV_IS_OBJECT(obj)) return kz_throw_type(ctx, "not an object");
    JSValue p;
    if (kz_get_proto(ctx, JV_OBJ(obj), &p)) return JS_EXCEPTION;
    return keep(ctx, p);
}

int js_set_prototype(JSContext *ctx, JSValue obj, JSValue proto) {
    if (!JV_IS_OBJECT(obj)) { kz_throw_type(ctx, "not an object"); return -1; }
    return kz_set_proto(ctx, JV_OBJ(obj), proto, true) < 0 ? -1 : 0;
}

uint32_t js_array_length(JSContext *ctx, JSValue arr) {
    (void)ctx;
    return kz_is_class(arr, KZ_CLASS_ARRAY) ? JV_OBJ(arr)->u.array.length : 0;
}

/* ------------------------------------------------------------------ functions */

JSValue js_call(JSContext *ctx, JSValue func, JSValue this_val, int argc, const JSValue *argv) {
    return keep(ctx, kz_call(ctx, func, this_val, argc, (JSValue *)argv));
}

JSValue js_construct(JSContext *ctx, JSValue ctor, int argc, const JSValue *argv) {
    return keep(ctx, kz_construct(ctx, ctor, argc, (JSValue *)argv, ctor));
}

JSValue js_invoke(JSContext *ctx, JSValue obj, const char *method, int argc, const JSValue *argv) {
    KZ_ROOT(ctx, k);
    k = name_key(ctx, method);
    if (k == JS_EXCEPTION) return k;
    return keep(ctx, kz_invoke(ctx, obj, k, argc, (JSValue *)argv));
}

/* the bridge from the engine's native calling convention to the embedder's */
static JSValue host_trampoline(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    JSObject *f = ctx->rt->cur_native;
    JSNativeFunction *fn = (JSNativeFunction *)f->u.native.host_fn;
    int mark = js_scope_open(ctx);
    JSValue r = fn(ctx, this_val, argc, argv, f->u.native.host_data);
    KZ_ROOT(ctx, rr);
    rr = r;
    js_scope_close(ctx, mark);
    if (r == JS_EXCEPTION && !kz_has_exception(ctx)) return kz_throw_type(ctx, "native function failed without an exception");
    return rr;
}

JSValue js_new_function_with_slots(JSContext *ctx, JSNativeFunction *fn, const char *name, int length, void *data,
                                   int nslots, const JSValue *slots) {
    KZ_ROOT(ctx, n);
    n = kz_atom(ctx, name ? name : "");
    if (n == JS_EXCEPTION) return n;
    JSValue f = kz_new_native(ctx, host_trampoline, n, length, 0, nslots, slots);
    if (f == JS_EXCEPTION) return f;
    JV_OBJ(f)->u.native.host_fn = (void *)fn;
    JV_OBJ(f)->u.native.host_data = data;
    JV_OBJ(f)->h.flags |= OBJ_CONSTRUCTOR;
    return keep(ctx, f);
}

JSValue js_new_function(JSContext *ctx, JSNativeFunction *fn, const char *name, int length, void *data) {
    return js_new_function_with_slots(ctx, fn, name, length, data, 0, NULL);
}

JSValue js_function_slot(JSContext *ctx, int index) { return kz_native_data(ctx, index); }
JSValue js_new_target(JSContext *ctx) { return ctx->rt->new_target; }

/* ------------------------------------------------------------------ host classes */

static const JSClassDef *host_def(JSObject *o) { return o->u.host.def; }

static JSValue key_for_host(JSContext *ctx, JSValue key) {
    if (KZ_KEY_IS_INDEX(key)) return jv_uint32(KZ_KEY_INDEX(key));
    (void)ctx;
    return key;
}

static int host_get(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->get) return KZ_FALLBACK;
    int mark = js_scope_open(ctx);
    JSValue v = JS_UNDEFINED;
    int r = d->get(ctx, JV_MKOBJ(o), o->u.host.opaque, key_for_host(ctx, key), receiver, &v);
    KZ_ROOT(ctx, rv);
    rv = v;
    js_scope_close(ctx, mark);
    if (r < 0) return -1;
    if (r == 0) return KZ_FALLBACK;
    *out = rv;
    return 1;
}

static int host_set(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->set) return KZ_FALLBACK;
    int mark = js_scope_open(ctx);
    int r = d->set(ctx, JV_MKOBJ(o), o->u.host.opaque, key_for_host(ctx, key), val, receiver);
    js_scope_close(ctx, mark);
    if (r < 0) return -1;
    return r ? 1 : KZ_FALLBACK;
}

static int host_has(JSContext *ctx, JSObject *o, JSValue key) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->has) return KZ_FALLBACK;
    int mark = js_scope_open(ctx);
    int r = d->has(ctx, JV_MKOBJ(o), o->u.host.opaque, key_for_host(ctx, key));
    js_scope_close(ctx, mark);
    if (r < 0) return -1;
    return r ? 1 : KZ_FALLBACK;
}

static int host_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *pd) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->get) return KZ_FALLBACK;
    /* a hook-provided property reads as a plain data property */
    JSValue v;
    int r = host_get(ctx, o, key, JV_MKOBJ(o), &v);
    if (r != 1) return r;
    pd->value = v;
    pd->getter = pd->setter = JS_UNDEFINED;
    pd->flags = KZ_PROP_WEC | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
    return 1;
}

static int host_del(JSContext *ctx, JSObject *o, JSValue key) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->delete) return KZ_FALLBACK;
    int mark = js_scope_open(ctx);
    bool deleted = false;
    int r = d->delete(ctx, JV_MKOBJ(o), o->u.host.opaque, key_for_host(ctx, key), &deleted);
    js_scope_close(ctx, mark);
    if (r < 0) return -1;
    if (!r) return KZ_FALLBACK;
    return deleted ? 1 : 0;
}

static int host_own_keys(JSContext *ctx, JSObject *o, KzVec *keys) {
    const JSClassDef *d = host_def(o);
    if (!d || !d->own_keys) return 0;
    int mark = js_scope_open(ctx);
    JSValue arr = JS_UNDEFINED;
    int r = d->own_keys(ctx, JV_MKOBJ(o), o->u.host.opaque, &arr);
    KZ_ROOT(ctx, ra);
    ra = arr;
    js_scope_close(ctx, mark);
    if (r < 0) return -1;
    if (!JV_IS_OBJECT(ra)) return 0;
    KZ_ROOT_VEC(ctx, tmp);
    if (kz_create_list_from_array_like(ctx, ra, &tmp)) return -1;
    for (uint32_t i = 0; i < tmp.len; i++) {
        JSValue k = kz_to_property_key(ctx, tmp.v[i]);
        if (k == JS_EXCEPTION || kz_vec_push(ctx, keys, k)) return -1;
    }
    return 0;
}

static const KzExotic host_exotic = {
    .get_own = host_get_own, .has = host_has, .get = host_get, .set = host_set, .del = host_del, .own_keys = host_own_keys,
};

int js_class_register(JSRuntime *rt, const JSClassDef *def) {
    const JSClassDef **n = rt->sys_realloc(rt->sys_opaque, (void *)rt->host_classes, ((size_t)rt->nhost_classes + 1) * sizeof(*n));
    if (!n) return -1;
    n[rt->nhost_classes] = def;
    rt->host_classes = n;
    rt->nhost_classes++;
    kz_exotic[KZ_CLASS_HOST] = &host_exotic;
    return rt->nhost_classes;
}

JSValue js_new_host_object(JSContext *ctx, int class_id, JSValue proto, void *opaque) {
    JSRuntime *rt = ctx->rt;
    if (class_id < 1 || class_id > rt->nhost_classes) return kz_throw_type(ctx, "unknown class");
    const JSClassDef *def = rt->host_classes[class_id - 1];
    if (!JV_IS_OBJECT(proto) && proto != JS_NULL) proto = kz_intr(ctx, KZ_I_OBJECT_PROTO);
    JSValue o = kz_new_object_class(ctx, KZ_CLASS_HOST, proto);
    if (o == JS_EXCEPTION) return o;
    JSObject *ho = JV_OBJ(o);
    ho->u.host.opaque = opaque;
    ho->u.host.def = def;
    ho->u.host.class_id = class_id;
    if (!(def->get || def->set || def->has || def->delete || def->own_keys)) ho->h.flags &= ~OBJ_EXOTIC;
    if (def->call) ho->h.flags |= OBJ_CALLABLE;
    if (def->construct) ho->h.flags |= OBJ_CONSTRUCTOR | OBJ_CALLABLE;
    return keep(ctx, o);
}

void *js_host_opaque(JSValue obj, int class_id) {
    if (!kz_is_class(obj, KZ_CLASS_HOST)) return NULL;
    JSObject *o = JV_OBJ(obj);
    return o->u.host.class_id == class_id ? o->u.host.opaque : NULL;
}

/* ------------------------------------------------------------------ JSON and promises */

JSValue js_json_parse(JSContext *ctx, const char *utf8, size_t len) {
    if (len == (size_t)-1) len = strlen(utf8);
    KZ_ROOT(ctx, s);
    s = kz_str_from_utf8(ctx, utf8, len);
    if (s == JS_EXCEPTION) return s;
    return keep(ctx, kz_json_parse(ctx, s, JS_UNDEFINED));
}

JSValue js_json_stringify(JSContext *ctx, JSValue v, JSValue replacer, JSValue indent) {
    return keep(ctx, kz_json_stringify(ctx, v, replacer, indent));
}

JSValue js_new_promise(JSContext *ctx, JSValue *resolve, JSValue *reject) {
    KZ_ROOTS(ctx, r, 2);
    JSValue p = kz_promise_new(ctx, &r[0], &r[1]);
    if (p == JS_EXCEPTION) return p;
    KZ_ROOT(ctx, rp);
    rp = p;
    if (resolve) *resolve = keep(ctx, r[0]);
    if (reject) *reject = keep(ctx, r[1]);
    return keep(ctx, rp);
}

JSPromiseState js_promise_state(JSContext *ctx, JSValue promise) {
    (void)ctx;
    if (!kz_is_class(promise, KZ_CLASS_PROMISE)) return JS_PROMISE_PENDING;
    return (JSPromiseState)kz_promise_state(promise);
}

JSValue js_promise_result(JSContext *ctx, JSValue promise) {
    if (!kz_is_class(promise, KZ_CLASS_PROMISE)) return JS_UNDEFINED;
    return keep(ctx, JV_OBJ(promise)->u.promise.result);
}
