/*
 * jstool: the host-side driver of Kosuzu (user/lib/js) for tests/host/test_js.py.  It is built with the engine's
 * sources and only uses the public API (js.h), like any embedder.
 *
 *   jstool [options] [file.js ...]
 *     -e CODE          evaluate CODE (several allowed, in order with the files)
 *     --module         evaluate files as modules
 *     --strict         evaluate scripts in strict mode
 *     --stress         collect garbage at every allocation (after the realm is set up)
 *     --mem MB         memory limit in megabytes
 *     --stack KB       value stack size
 *     --depth N        maximum call depth
 *     --time-limit MS  interrupt scripts that run longer
 *     --tz MINUTES     the local time zone offset Date sees (default 0, UTC)
 *     --now MS         a fixed Date.now() (default: the real clock)
 *     --stats          print memory statistics at the end
 *
 * Globals for scripts: print(...) (space separated, newline), console.log/error/info/warn, gc(), and
 * kz.memory() (heap bytes).  An uncaught exception prints "Uncaught <report>" on stdout and exits with status 1.
 */
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static JSRuntime *g_rt;
static int g_status;
static long g_time_limit_ms;
static struct timespec g_start;
static int g_tz;
static double g_now = -1;

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
    (void)opaque; (void)utc_ms;
    return g_tz;
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
    js_run_jobs(js_context_runtime(ctx), 0);
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
    g_rt = js_runtime_new(&opts);
    if (!g_rt) { fprintf(stderr, "jstool: cannot create a runtime\n"); return 2; }
    JSHostHooks hooks = { 0 };
    hooks.report_error = report_error;
    hooks.now_ms = now_ms;
    hooks.tz_offset_min = tz_offset;
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
    js_define_str(ctx, g, "kz", kz, JS_PROP_HIDDEN);
    for (int i = 1; i < argc && !g_status; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--mem") || !strcmp(a, "--stack") || !strcmp(a, "--depth")) { i++; continue; }
        if (!strcmp(a, "--stress")) { stress = true; js_runtime_set_gc_stress(g_rt, true); continue; }
        if (!strcmp(a, "--stress-every") && i + 1 < argc) { stress = true; js_runtime_set_gc_stress_every(g_rt, (unsigned)atoi(argv[++i])); continue; }
        if (!strcmp(a, "--stats")) { stats = true; continue; }
        if (!strcmp(a, "--module")) { module = true; continue; }
        if (!strcmp(a, "--strict")) { strict = true; continue; }
        if (!strcmp(a, "--time-limit") && i + 1 < argc) { g_time_limit_ms = atol(argv[++i]); continue; }
        if (!strcmp(a, "--tz") && i + 1 < argc) { g_tz = atoi(argv[++i]); continue; }
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
        if (!run(ctx, src, len, a, flags)) g_status = 1;
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
