/*
 * js.h: Kosuzu, the TouhouOS JavaScript engine (an ECMAScript interpreter with a bytecode VM), the embedding API.
 *
 * Named after Kosuzu Motoori of the Suzunaan book rental, who can read any writing, youkai books included.
 * docs/JS.md describes the language support, the limits and the design; this header is the stable C interface.
 *
 * The model, in five sentences:
 *   - A JSRuntime owns the heap, the garbage collector and the job queue; it is used by one thread at a time.
 *   - A JSContext is a realm (a global object with its own Object, Array, ...); a runtime may hold several.
 *   - A JSValue is a 64-bit value (a number, a string, an object, ...).  The collector is precise and non-moving: a
 *     value stays alive while something reachable holds it.  Every value an API function returns is kept alive by
 *     the current *scope* until that scope closes (js_scope_open/js_scope_close; a native function called by the
 *     engine runs inside its own scope).  To keep a value longer (in a C structure, across scripts), js_pin() it.
 *   - Errors are values: a function that fails returns JS_EXCEPTION and leaves the exception pending in the runtime
 *     (js_take_exception() fetches it).  Nothing longjmps out of the engine, and no input (a script, a memory limit,
 *     a deep recursion) crashes it: they end in a SyntaxError, a RangeError or "out of memory".
 *   - Promise jobs (microtasks) run when the host calls js_run_jobs(); timers belong to the host (see js_pin).
 */
#ifndef JS_H
#define JS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t JSValue;
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct JSTracer JSTracer;

/* Constant values (their encoding is part of the ABI). */
#define JS_UNDEFINED ((JSValue)0xFFFA000000000000ULL)
#define JS_NULL      ((JSValue)0xFFFA000000000001ULL)
#define JS_FALSE     ((JSValue)0xFFFA000000000002ULL)
#define JS_TRUE      ((JSValue)0xFFFA000000000003ULL)
#define JS_EXCEPTION ((JSValue)0xFFFA000000000006ULL)   /* "an exception is pending": never a real value */

/* ------------------------------------------------------------------ runtime */

typedef struct JSRuntimeOptions {
    size_t memory_limit;        /* bytes of heap the runtime may use; 0 = no limit */
    size_t stack_size;          /* bytes for the VM's value stack (frames of JavaScript calls); 0 = 1 MiB */
    size_t c_stack_size;        /* bytes of C stack the engine may use below the caller of js_runtime_new; 0 = 512 KiB */
    int max_call_depth;         /* nested calls before a RangeError; 0 = 10000 */
    /* the allocator: all three or none (then malloc/realloc/free) */
    void *(*malloc_fn)(void *opaque, size_t n);
    void *(*realloc_fn)(void *opaque, void *p, size_t n);
    void (*free_fn)(void *opaque, void *p);
    void *malloc_opaque;
} JSRuntimeOptions;

JSRuntime *js_runtime_new(const JSRuntimeOptions *opts);        /* NULL when out of memory */
void js_runtime_free(JSRuntime *rt);                            /* frees every context and every value */
void js_runtime_set_memory_limit(JSRuntime *rt, size_t bytes);  /* 0 = none */
/* Called every few thousand backward jumps and calls; a nonzero return stops the running script with an error that
   no try/catch can catch ("interrupted").  Use it for a time limit or a Ctrl-C. */
void js_runtime_set_interrupt_handler(JSRuntime *rt, int (*fn)(JSRuntime *rt, void *opaque), void *opaque);
/* Marks the current C stack position as the engine's stack base again (call it when entering from another thread
   or from a much deeper point than js_runtime_new's caller). */
void js_runtime_set_stack_base(JSRuntime *rt);

/* Host services.  now_ms: the time (milliseconds since 1970-01-01 UTC) for Date.now() and new Date().
   tz_offset_min: minutes to add to UTC to get local time at the UTC instant `utc_ms` (e.g. +540 for JST, -300 for
   EST); the default is UTC.  Both may be NULL. */
typedef struct JSHostHooks {
    double (*now_ms)(void *opaque);
    int (*tz_offset_min)(void *opaque, double utc_ms);
    /* An exception that nobody caught in a job (a promise reaction, a FinalizationRegistry callback) or a promise
       rejected without a handler: handed here (may be NULL: then they are dropped). */
    void (*report_error)(JSContext *ctx, JSValue error, bool unhandled_rejection, void *opaque);
    /* Module loading: return the module's source (malloc'd; the engine frees it with free_fn/free) and its canonical
       name in *resolved (malloc'd too), or NULL with a message in *error (static or malloc'd: set *error_owned). */
    char *(*load_module)(JSContext *ctx, const char *specifier, const char *referrer, size_t *len, char **resolved,
                         const char **error, void *opaque);
    void *opaque;
} JSHostHooks;
void js_runtime_set_host_hooks(JSRuntime *rt, const JSHostHooks *hooks);

typedef struct JSMemoryUsage {
    size_t heap_bytes;          /* bytes taken from the allocator now */
    size_t peak_bytes;
    size_t limit;
    size_t gc_count;
    size_t objects, strings;    /* live cells after the last collection */
} JSMemoryUsage;
void js_memory_usage(JSRuntime *rt, JSMemoryUsage *out);
void js_gc(JSRuntime *rt);                                     /* a full collection now */
void js_runtime_set_gc_stress(JSRuntime *rt, bool on);         /* collect at every allocation (for tests) */
void js_runtime_set_gc_stress_every(JSRuntime *rt, unsigned every);   /* at every n-th allocation; 0: off */

/* ------------------------------------------------------------------ contexts (realms) */

JSContext *js_context_new(JSRuntime *rt);
void js_context_free(JSContext *ctx);
JSRuntime *js_context_runtime(JSContext *ctx);
void js_context_set_opaque(JSContext *ctx, void *opaque);
void *js_context_opaque(JSContext *ctx);
JSValue js_global_object(JSContext *ctx);

/* ------------------------------------------------------------------ scopes and pinned values */

/* Values returned by API calls stay alive until the scope that was open when they were made closes.  Scopes nest; a
   context starts with one open scope that lives until js_context_free. */
int js_scope_open(JSContext *ctx);                      /* returns a mark for js_scope_close */
void js_scope_close(JSContext *ctx, int mark);
JSValue js_scope_keep(JSContext *ctx, JSValue v);       /* re-register v in the (now current) scope; returns v */
/* Pinned values live until unpinned (counted: pin twice, unpin twice). */
void js_pin(JSContext *ctx, JSValue v);
void js_unpin(JSContext *ctx, JSValue v);

/* ------------------------------------------------------------------ evaluating code */

#define JS_EVAL_SCRIPT   0      /* a classic script: var and function declarations become global properties */
#define JS_EVAL_MODULE   1      /* an ES module (strict, import/export); returns a promise for its evaluation */
#define JS_EVAL_STRICT   2      /* a script in strict mode */
#define JS_EVAL_COMPILE  4      /* only compile: returns a function that runs the script when called */

JSValue js_eval(JSContext *ctx, const char *utf8, size_t len, const char *filename, int flags);
/* Runs every pending promise job (and FinalizationRegistry cleanup) until the queue is empty or `max` jobs have run
   (max <= 0: no limit).  Returns the number of jobs run. */
int js_run_jobs(JSRuntime *rt, int max);
bool js_has_jobs(JSRuntime *rt);

/* ------------------------------------------------------------------ exceptions */

bool js_has_exception(JSContext *ctx);
JSValue js_take_exception(JSContext *ctx);              /* clears it; JS_UNDEFINED when none */
JSValue js_throw(JSContext *ctx, JSValue error);        /* makes `error` pending; returns JS_EXCEPTION */
typedef enum JSErrorKind {
    JS_ERROR, JS_EVAL_ERROR, JS_RANGE_ERROR, JS_REFERENCE_ERROR, JS_SYNTAX_ERROR, JS_TYPE_ERROR, JS_URI_ERROR,
    JS_AGGREGATE_ERROR
} JSErrorKind;
JSValue js_new_error(JSContext *ctx, JSErrorKind kind, const char *message);
JSValue js_throw_error(JSContext *ctx, JSErrorKind kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* "TypeError: x is not a function\n    at f (file.js:3:5)\n..." for any value (malloc'd UTF-8; js_free_utf8). */
char *js_error_report(JSContext *ctx, JSValue error);

/* ------------------------------------------------------------------ values */

bool js_is_undefined(JSValue v);
bool js_is_null(JSValue v);
bool js_is_bool(JSValue v);
bool js_is_number(JSValue v);
bool js_is_string(JSValue v);
bool js_is_symbol(JSValue v);
bool js_is_bigint(JSValue v);
bool js_is_object(JSValue v);
bool js_is_function(JSContext *ctx, JSValue v);
bool js_is_constructor(JSContext *ctx, JSValue v);
bool js_is_array(JSContext *ctx, JSValue v);           /* Array.isArray */
bool js_is_error(JSContext *ctx, JSValue v);
bool js_is_promise(JSContext *ctx, JSValue v);
const char *js_typeof(JSContext *ctx, JSValue v);      /* "undefined", "object", ... */

JSValue js_bool(bool b);
JSValue js_number(double d);
JSValue js_int(int32_t i);
double js_number_value(JSValue v);                       /* v must be a number */
bool js_bool_value(JSValue v);                           /* v must be a boolean */
JSValue js_string(JSContext *ctx, const char *utf8, size_t len);    /* len == (size_t)-1: up to the NUL */
JSValue js_string_utf16(JSContext *ctx, const uint16_t *s, size_t len);
JSValue js_new_object(JSContext *ctx);
JSValue js_new_object_proto(JSContext *ctx, JSValue proto);         /* proto: an object or JS_NULL */
JSValue js_new_array(JSContext *ctx, uint32_t length);
JSValue js_new_symbol(JSContext *ctx, const char *description);

/* Conversions run the language's own rules (valueOf, toString, Symbol.toPrimitive): they may call JavaScript and
   fail (JS_EXCEPTION, or -1 for the int ones). */
JSValue js_to_string(JSContext *ctx, JSValue v);
JSValue js_to_number_value(JSContext *ctx, JSValue v);
int js_to_number(JSContext *ctx, JSValue v, double *out);
int js_to_int32(JSContext *ctx, JSValue v, int32_t *out);
int js_to_uint32(JSContext *ctx, JSValue v, uint32_t *out);
int js_to_index(JSContext *ctx, JSValue v, uint64_t *out);
bool js_to_bool(JSContext *ctx, JSValue v);
JSValue js_to_object(JSContext *ctx, JSValue v);
/* A string's UTF-8 text (unpaired surrogates become U+FFFD), malloc'd and NUL terminated; NULL on error. */
char *js_to_utf8(JSContext *ctx, JSValue v, size_t *len);
void js_free_utf8(JSContext *ctx, char *s);
size_t js_string_length(JSValue str);                   /* in UTF-16 code units */
size_t js_string_copy_utf16(JSValue str, uint16_t *out, size_t max);  /* copies up to max units; returns the length */
bool js_strict_equals(JSContext *ctx, JSValue a, JSValue b);
bool js_same_value(JSContext *ctx, JSValue a, JSValue b);

/* ------------------------------------------------------------------ properties */

/* Keys: js_get/js_set take any value as the key (ToPropertyKey); the _str forms take a UTF-8 name, the _index forms
   an array index. */
JSValue js_get(JSContext *ctx, JSValue obj, JSValue key);
JSValue js_get_str(JSContext *ctx, JSValue obj, const char *name);
JSValue js_get_index(JSContext *ctx, JSValue obj, uint32_t index);
int js_set(JSContext *ctx, JSValue obj, JSValue key, JSValue value);          /* 0 ok, -1 exception */
int js_set_str(JSContext *ctx, JSValue obj, const char *name, JSValue value);
int js_set_index(JSContext *ctx, JSValue obj, uint32_t index, JSValue value);
int js_has(JSContext *ctx, JSValue obj, JSValue key);                         /* 1, 0, -1 */
int js_delete(JSContext *ctx, JSValue obj, JSValue key);                      /* 1 deleted, 0 refused, -1 */

#define JS_PROP_WRITABLE      (1 << 0)
#define JS_PROP_ENUMERABLE    (1 << 1)
#define JS_PROP_CONFIGURABLE  (1 << 2)
#define JS_PROP_DEFAULT       (JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE)
#define JS_PROP_HIDDEN        (JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)       /* like built-in methods */
/* Defines a data property (getter == setter == JS_UNDEFINED) or an accessor (either one a function). */
int js_define(JSContext *ctx, JSValue obj, JSValue key, JSValue value, JSValue getter, JSValue setter, int flags);
int js_define_str(JSContext *ctx, JSValue obj, const char *name, JSValue value, int flags);
/* Own enumerable string keys in order (Object.keys) as an array. */
JSValue js_keys(JSContext *ctx, JSValue obj);
JSValue js_get_prototype(JSContext *ctx, JSValue obj);
int js_set_prototype(JSContext *ctx, JSValue obj, JSValue proto);
uint32_t js_array_length(JSContext *ctx, JSValue arr);  /* 0 for non-arrays */

/* ------------------------------------------------------------------ functions */

JSValue js_call(JSContext *ctx, JSValue func, JSValue this_val, int argc, const JSValue *argv);
JSValue js_construct(JSContext *ctx, JSValue ctor, int argc, const JSValue *argv);
JSValue js_invoke(JSContext *ctx, JSValue obj, const char *method, int argc, const JSValue *argv);

/* A native function.  argv holds at least `length` values (missing arguments are undefined); `this_val` is the this
   value; `data` is the pointer given at creation.  Return a value or JS_EXCEPTION (after js_throw*). */
typedef JSValue JSNativeFunction(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data);
JSValue js_new_function(JSContext *ctx, JSNativeFunction *fn, const char *name, int length, void *data);
/* The same with JavaScript values carried by the function (traced by the collector): js_function_slot() reads them
   inside the native while it runs. */
JSValue js_new_function_with_slots(JSContext *ctx, JSNativeFunction *fn, const char *name, int length, void *data,
                                   int nslots, const JSValue *slots);
JSValue js_function_slot(JSContext *ctx, int index);
/* Inside a native called with `new`: the new.target, else JS_UNDEFINED. */
JSValue js_new_target(JSContext *ctx);

/* ------------------------------------------------------------------ host objects (what a DOM binds through) */

typedef struct JSClassDef JSClassDef;
/* A property hook returns 1 when it handled the request, 0 to fall back to the object's ordinary properties, -1 on
   an exception.  `key` is a string, a symbol or a number (an array index). */
struct JSClassDef {
    const char *name;                                   /* the class name (Object.prototype.toString's tag) */
    void (*finalize)(JSRuntime *rt, void *opaque);      /* the object was collected */
    void (*trace)(JSTracer *trc, void *opaque);         /* mark the JSValues the opaque data holds: js_trace_value */
    int (*get)(JSContext *ctx, JSValue obj, void *opaque, JSValue key, JSValue receiver, JSValue *out);
    int (*set)(JSContext *ctx, JSValue obj, void *opaque, JSValue key, JSValue value, JSValue receiver);
    int (*has)(JSContext *ctx, JSValue obj, void *opaque, JSValue key);         /* 1 has (handled), 0 fall back */
    int (*delete)(JSContext *ctx, JSValue obj, void *opaque, JSValue key, bool *deleted);
    /* Extra own keys listed before the ordinary ones (indices first): fill *keys with a new array. */
    int (*own_keys)(JSContext *ctx, JSValue obj, void *opaque, JSValue *keys);
    /* Callable host objects (both optional). */
    JSValue (*call)(JSContext *ctx, JSValue obj, void *opaque, JSValue this_val, int argc, const JSValue *argv);
    JSValue (*construct)(JSContext *ctx, JSValue obj, void *opaque, int argc, const JSValue *argv, JSValue new_target);
};
int js_class_register(JSRuntime *rt, const JSClassDef *def);           /* returns the class id (> 0) or -1 */
JSValue js_new_host_object(JSContext *ctx, int class_id, JSValue proto, void *opaque);
void *js_host_opaque(JSValue obj, int class_id);                       /* NULL if obj is not of that class */
void js_trace_value(JSTracer *trc, JSValue v);

/* ------------------------------------------------------------------ JSON, promises, modules */

JSValue js_json_parse(JSContext *ctx, const char *utf8, size_t len);
JSValue js_json_stringify(JSContext *ctx, JSValue v, JSValue replacer, JSValue indent);
/* A new pending promise with its resolving functions. */
JSValue js_new_promise(JSContext *ctx, JSValue *resolve, JSValue *reject);
typedef enum { JS_PROMISE_PENDING, JS_PROMISE_FULFILLED, JS_PROMISE_REJECTED } JSPromiseState;
JSPromiseState js_promise_state(JSContext *ctx, JSValue promise);
JSValue js_promise_result(JSContext *ctx, JSValue promise);

#ifdef __cplusplus
}
#endif
#endif
