/*
 * jstool: the host-side driver of Kosuzu (user/lib/js) for tests/host/test_js.py.  It is built with the engine's
 * sources and only uses the public API (js.h), like any embedder.
 *
 *   jstool [options] [file.js ...]
 *     -e CODE          evaluate CODE (several allowed, in order with the files)
 *     --module         evaluate files (and -e code) as modules; an import is read from disk (a path relative to the importing
 *                      file, or absolute; import.meta.url is the module's name), a rejected evaluation is "Uncaught ..."
 *     --strict         evaluate scripts in strict mode
 *     --stress         collect garbage at every allocation (after the realm is set up)
 *     --mem MB         memory limit in megabytes
 *     --stack KB       value stack size
 *     --depth N        maximum call depth
 *     --time-limit MS  interrupt scripts that run longer
 *     --tz MINUTES     the local time zone offset Date sees (default 0, UTC)
 *     --tz-dst START END MINUTES   add MINUTES to the offset for the UTC instants START <= t < END (milliseconds):
 *                      one daylight saving period, enough to test skipped and repeated local times
 *     --tz-name NAME   the abbreviation Date.prototype.toString shows in parentheses (default: none)
 *     --now MS         a fixed Date.now() (default: the real clock)
 *     --console        install the `js` applet's console and inspector (js_prelude.h): console.log prints like Node's
 *     --stats          print memory statistics at the end
 *
 * Globals for scripts: print(...) (space separated, newline), console.log/error/info/warn, gc(), and
 * kz.memory() (heap bytes), kz.module(source[, name]) (evaluates a module from text, returns its promise).  An uncaught exception prints "Uncaught <report>" on stdout and exits with status 1.
 */
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../user/bin/kappabox/js_prelude.h"
#include "modload.h"

static JSRuntime *g_rt;
static int g_status;
static long g_time_limit_ms;
static struct timespec g_start;
static int g_tz;
static double g_now = -1;
static double g_dst_start, g_dst_end;
static int g_dst_delta;
static const char *g_tz_name;

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}

static JSValue print_values(JSContext *ctx, int argc, const JSValue *argv, FILE *out) {
    for (int i = 0; i < argc; i++) {
        size_t len;
        char *s = js_to_utf8(ctx, argv[i], &len);
        if (!s) return JS_EXCEPTION;
        if (i) fputc(' ', out);
        fwrite(s, 1, len, out);
        js_free_utf8(ctx, s);
    }
    fputc('\n', out);
    fflush(out);
    return JS_UNDEFINED;
}

static JSValue fn_print(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)data;
    return print_values(ctx, argc, argv, stdout);
}

static JSValue fn_gc(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    js_gc(js_context_runtime(ctx));
    return JS_UNDEFINED;
}

static JSValue fn_memory(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    JSMemoryUsage u;
    js_memory_usage(js_context_runtime(ctx), &u);
    return js_number((double)u.heap_bytes);
}

/* kz.module(source [, name]): evaluates a module from text (a syntax error is thrown; the result is its promise); the
   imports in it are relative to `name`, a path */
static JSValue fn_module(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)data;
    size_t len;
    char *src = js_to_utf8(ctx, argv[0], &len);
    if (!src) return JS_EXCEPTION;
    char *name = argc > 1 && !js_is_undefined(argv[1]) ? js_to_utf8(ctx, argv[1], NULL) : NULL;
    JSValue r = js_eval(ctx, src, len, name ? name : "<kz.module>", JS_EVAL_MODULE);
    js_free_utf8(ctx, src);
    js_free_utf8(ctx, name);
    return r;
}

static void import_meta(JSContext *ctx, JSValue meta, const char *name, void *opaque) {
    (void)opaque;
    js_define_str(ctx, meta, "url", js_string(ctx, name, (size_t)-1), JS_PROP_DEFAULT);
}

static void report_error(JSContext *ctx, JSValue err, bool rejection, void *opaque) {
    (void)opaque;
    char *s = js_error_report(ctx, err);
    printf("%s %s\n", rejection ? "Unhandled rejection:" : "Uncaught (in job)", s ? s : "?");
    js_free_utf8(ctx, s);
    g_status = 1;
}

static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (g_time_limit_ms <= 0) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g_start.tv_sec) * 1000 + (now.tv_nsec - g_start.tv_nsec) / 1000000;
    return ms > g_time_limit_ms;
}

static double now_ms(void *opaque) {
    (void)opaque;
    if (g_now >= 0) return g_now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000 + (double)(ts.tv_nsec / 1000000);
}

static int tz_offset(void *opaque, double utc_ms) {
    (void)opaque;
    return g_tz + (g_dst_delta && utc_ms >= g_dst_start && utc_ms < g_dst_end ? g_dst_delta : 0);
}

static const char *tz_name(void *opaque, double utc_ms) {
    (void)opaque; (void)utc_ms;
    return g_tz_name;
}

static void sleep_ms(void *opaque, double ms) {
    (void)opaque;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms - (double)(long)(ms / 1000) * 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* --console: the `js` applet's console and inspector (user/bin/kappabox/js_prelude.h, JavaScript), with the same host natives */
static JSValue host_write(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    int32_t fd = 1;
    if (js_to_int32(ctx, argv[0], &fd)) return JS_EXCEPTION;
    size_t len;
    char *s = js_to_utf8(ctx, argv[1], &len);
    if (!s) return JS_EXCEPTION;
    FILE *out = fd == 2 ? stderr : stdout;
    fwrite(s, 1, len, out);
    fflush(out);
    js_free_utf8(ctx, s);
    return JS_UNDEFINED;
}

static JSValue host_now(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)data;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return js_number((double)ts.tv_sec * 1000 + (double)ts.tv_nsec / 1e6);
}

static JSValue host_promise_state(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    if (!js_is_promise(ctx, argv[0])) return JS_UNDEFINED;
    JSPromiseState st = js_promise_state(ctx, argv[0]);
    JSValue pair = js_new_array(ctx, 2);
    js_set_index(ctx, pair, 0, js_int((int32_t)st));
    js_set_index(ctx, pair, 1, st == JS_PROMISE_PENDING ? JS_UNDEFINED : js_promise_result(ctx, argv[0]));
    return pair;
}

static bool install_console(JSContext *ctx) {
    JSValue host = js_new_object(ctx);
    js_define_str(ctx, host, "write", js_new_function(ctx, host_write, "write", 2, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, host, "now", js_new_function(ctx, host_now, "now", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, host, "promiseState", js_new_function(ctx, host_promise_state, "promiseState", 1, NULL), JS_PROP_HIDDEN);
    JSValue fn = js_eval(ctx, kb_js_prelude, strlen(kb_js_prelude), "<prelude>", JS_EVAL_SCRIPT);
    JSValue r = fn == JS_EXCEPTION ? fn : js_call(ctx, fn, JS_UNDEFINED, 1, &host);
    if (r == JS_EXCEPTION) {
        char *s = js_error_report(ctx, js_take_exception(ctx));
        printf("Uncaught in the prelude: %s\n", s ? s : "?");
        js_free_utf8(ctx, s);
        return false;
    }
    JSValue g = js_global_object(ctx);
    js_define_str(ctx, g, "__inspect", js_get_str(ctx, r, "inspect"), JS_PROP_HIDDEN);
    js_define_str(ctx, g, "__format", js_get_str(ctx, r, "format"), JS_PROP_HIDDEN);
    return true;
}

static bool run(JSContext *ctx, const char *src, size_t len, const char *name, int flags) {
    int mark = js_scope_open(ctx);
    JSValue r = js_eval(ctx, src, len, name, flags);
    bool ok = true;
    if (r == JS_EXCEPTION) {
        JSValue e = js_take_exception(ctx);
        char *s = js_error_report(ctx, e);
        printf("Uncaught %s\n", s ? s : "?");
        js_free_utf8(ctx, s);
        ok = false;
    }
    bool module = r != JS_EXCEPTION && (flags & JS_EVAL_MODULE);
    if (module) js_promise_set_handled(ctx, r);          /* its outcome is reported below, not as an unhandled rejection */
    js_run_jobs(js_context_runtime(ctx), 0);
    if (module) {
        JSPromiseState st = js_promise_state(ctx, r);
        if (st == JS_PROMISE_REJECTED) {
            char *s = js_error_report(ctx, js_promise_result(ctx, r));
            printf("Uncaught %s\n", s ? s : "?");
            js_free_utf8(ctx, s);
            ok = false;
        } else if (st == JS_PROMISE_PENDING) {
            printf("Uncaught Error: the module never finished (an unsettled top-level await)\n");
            ok = false;
        }
    }
    js_scope_close(ctx, mark);
    return ok;
}

int main(int argc, char **argv) {
    JSRuntimeOptions opts = { 0 };
    bool stress = false, stats = false, module = false, strict = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mem") && i + 1 < argc) opts.memory_limit = (size_t)atol(argv[++i]) << 20;
        else if (!strcmp(argv[i], "--stack") && i + 1 < argc) opts.stack_size = (size_t)atol(argv[++i]) << 10;
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) opts.max_call_depth = atoi(argv[++i]);
    }
    /* the time zone options act on the runtime hooks, so they are read before any script runs */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tz") && i + 1 < argc) g_tz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tz-dst") && i + 3 < argc) { g_dst_start = atof(argv[++i]); g_dst_end = atof(argv[++i]); g_dst_delta = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--tz-name") && i + 1 < argc) g_tz_name = argv[++i];
    }
    g_rt = js_runtime_new(&opts);
    if (!g_rt) { fprintf(stderr, "jstool: cannot create a runtime\n"); return 2; }
    JSHostHooks hooks = { 0 };
    hooks.report_error = report_error;
    hooks.now_ms = now_ms;
    hooks.tz_offset_min = tz_offset;
    hooks.tz_name = tz_name;
    hooks.sleep_ms = sleep_ms;
    hooks.load_module = modload_hook;
    hooks.import_meta = import_meta;
    js_runtime_set_host_hooks(g_rt, &hooks);
    js_runtime_set_interrupt_handler(g_rt, interrupt, NULL);
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    JSContext *ctx = js_context_new(g_rt);
    if (!ctx) { fprintf(stderr, "jstool: cannot create a context\n"); return 2; }
    JSValue g = js_global_object(ctx);
    js_define_str(ctx, g, "print", js_new_function(ctx, fn_print, "print", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, g, "gc", js_new_function(ctx, fn_gc, "gc", 0, NULL), JS_PROP_HIDDEN);
    JSValue console = js_new_object(ctx);
    js_define_str(ctx, console, "log", js_new_function(ctx, fn_print, "log", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, console, "info", js_new_function(ctx, fn_print, "info", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, console, "warn", js_new_function(ctx, fn_print, "warn", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, console, "error", js_new_function(ctx, fn_print, "error", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, g, "console", console, JS_PROP_HIDDEN);
    JSValue kz = js_new_object(ctx);
    js_define_str(ctx, kz, "memory", js_new_function(ctx, fn_memory, "memory", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, kz, "module", js_new_function(ctx, fn_module, "module", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, g, "kz", kz, JS_PROP_HIDDEN);
    for (int i = 1; i < argc && !g_status; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--mem") || !strcmp(a, "--stack") || !strcmp(a, "--depth") || !strcmp(a, "--tz") || !strcmp(a, "--tz-name")) { i++; continue; }
        if (!strcmp(a, "--tz-dst")) { i += 3; continue; }
        if (!strcmp(a, "--stress")) { stress = true; js_runtime_set_gc_stress(g_rt, true); continue; }
        if (!strcmp(a, "--stress-every") && i + 1 < argc) { stress = true; js_runtime_set_gc_stress_every(g_rt, (unsigned)atoi(argv[++i])); continue; }
        if (!strcmp(a, "--stats")) { stats = true; continue; }
        if (!strcmp(a, "--console")) { if (!install_console(ctx)) g_status = 1; continue; }
        if (!strcmp(a, "--module")) { module = true; continue; }
        if (!strcmp(a, "--strict")) { strict = true; continue; }
        if (!strcmp(a, "--time-limit") && i + 1 < argc) { g_time_limit_ms = atol(argv[++i]); continue; }
        if (!strcmp(a, "--now") && i + 1 < argc) { g_now = atof(argv[++i]); continue; }
        int flags = module ? JS_EVAL_MODULE : strict ? JS_EVAL_STRICT : JS_EVAL_SCRIPT;
        if (!strcmp(a, "-e") && i + 1 < argc) {
            const char *code = argv[++i];
            if (!run(ctx, code, strlen(code), "<cmdline>", flags)) g_status = 1;
            continue;
        }
        size_t len;
        char *src = read_file(a, &len);
        if (!src) { fprintf(stderr, "jstool: cannot read %s\n", a); g_status = 2; break; }
        char name[4096];
        snprintf(name, sizeof name, "%s", a);
        if (module) modload_normalize(name);               /* (what an import of this file from another module resolves to) */
        if (!run(ctx, src, len, name, flags)) g_status = 1;
        free(src);
    }
    (void)stress;
    if (stats) {
        JSMemoryUsage u;
        js_gc(g_rt);
        js_memory_usage(g_rt, &u);
        printf("[stats] heap %zu peak %zu collections %zu objects %zu strings %zu\n", u.heap_bytes, u.peak_bytes,
               u.gc_count, u.objects, u.strings);
    }
    js_context_free(ctx);
    js_runtime_free(g_rt);
    return g_status;
}
