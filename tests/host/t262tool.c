/*
 * t262tool: runs test262 tests (https://github.com/tc39/test262, INTERPRETING.md) against Kosuzu, each run in a fresh
 * runtime and realm.  It is driven by tests/host/test262.py, which reads the tests' metadata and writes a manifest;
 * this program only builds the source, runs it and judges the outcome.
 *
 *   t262tool [--time-limit MS] [--mem MB] HARNESS_DIR MANIFEST
 *
 * The manifest has one run per line, tab separated:
 *   <id> <test file> <mode> <includes> <flags> <negative phase> <negative type>
 * mode: sloppy | strict | raw | module; includes and flags are comma separated lists; negative phase is empty,
 * "parse", "resolution" or "runtime".
 *
 * Output, one line per event, flushed at once so that a crash names the test that caused it:
 *   B <id>                          the run starts
 *   R <id> PASS|FAIL|TIMEOUT <msg>  the run ended
 */
#include <js.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "modload.h"

static const char *g_harness;
static long g_limit_ms = 10000;
static size_t g_mem = (size_t)1024 << 20;
static struct timespec g_start;

static char g_print[8192];
static size_t g_print_len;
static char g_job_error[512];

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 15, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap + 1); }
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g_start.tv_sec) * 1000 + (now.tv_nsec - g_start.tv_nsec) / 1000000;
    return ms > g_limit_ms;
}

static double now_ms(void *opaque) {
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000 + (double)(ts.tv_nsec / 1000000);
}

static int tz_offset(void *opaque, double utc_ms) { (void)opaque; (void)utc_ms; return 0; }

static void report_error(JSContext *ctx, JSValue err, bool rejection, void *opaque) {
    (void)opaque; (void)rejection;
    char *s = js_error_report(ctx, err);
    if (s && !g_job_error[0]) strncpy(g_job_error, s, sizeof g_job_error - 1);
    js_free_utf8(ctx, s);
}

/* ------------------------------------------------------------------ the host environment ($262, print) */

static void install_262(JSContext *ctx);

static JSValue fn_print(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)data;
    if (argc < 1) return JS_UNDEFINED;
    size_t len;
    char *s = js_to_utf8(ctx, argv[0], &len);
    if (!s) return JS_EXCEPTION;
    if (g_print_len + len + 2 < sizeof g_print) {
        memcpy(g_print + g_print_len, s, len);
        g_print_len += len;
        g_print[g_print_len++] = '\n';
        g_print[g_print_len] = 0;
    }
    js_free_utf8(ctx, s);
    return JS_UNDEFINED;
}

static JSValue fn_gc(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    js_gc(js_context_runtime(ctx));
    return JS_UNDEFINED;
}

static JSValue fn_detach(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    if (js_array_buffer_detach(ctx, argv[0])) return JS_EXCEPTION;
    return JS_NULL;
}

static JSValue fn_eval_script(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    size_t len;
    char *src = js_to_utf8(ctx, argv[0], &len);
    if (!src) return JS_EXCEPTION;
    JSValue r = js_eval(ctx, src, len, "<evalScript>", JS_EVAL_SCRIPT);
    js_free_utf8(ctx, src);
    return r;
}

static JSValue fn_create_realm(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    JSContext *n = js_context_new(js_context_runtime(ctx));
    if (!n) return js_throw_error(ctx, JS_ERROR, "cannot create a realm");
    install_262(n);
    return js_get_str(n, js_global_object(n), "$262");
}

static void sleep_ms(void *opaque, double ms) {
    (void)opaque;
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms - (double)(long)(ms / 1000) * 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* $262.agent: the harness (atomicsHelper.js) loads against it, but there is only one agent: start() cannot work, and
   the tests that call it are skipped by tests/host/test262.py ("needs agents") */
static JSValue fn_agent_sleep(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    double ms;
    if (js_to_number(ctx, argv[0], &ms)) return JS_EXCEPTION;
    if (ms > 0) sleep_ms(NULL, ms);
    return JS_UNDEFINED;
}

static JSValue fn_agent_now(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)data;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return js_number((double)ts.tv_sec * 1000 + (double)ts.tv_nsec / 1e6);
}

static JSValue fn_agent_null(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)data;
    return JS_NULL;
}

static JSValue fn_agent_unsupported(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    return js_throw_error(ctx, JS_ERROR, "$262.agent: this host has no second agent");
}

static void install_262(JSContext *ctx) {
    JSValue g = js_global_object(ctx);
    JSValue t = js_new_object(ctx);
    JSValue agent = js_new_object(ctx);
    js_define_str(ctx, agent, "start", js_new_function(ctx, fn_agent_unsupported, "start", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "broadcast", js_new_function(ctx, fn_agent_unsupported, "broadcast", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "receiveBroadcast", js_new_function(ctx, fn_agent_unsupported, "receiveBroadcast", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "report", js_new_function(ctx, fn_agent_unsupported, "report", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "leaving", js_new_function(ctx, fn_agent_unsupported, "leaving", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "getReport", js_new_function(ctx, fn_agent_null, "getReport", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "sleep", js_new_function(ctx, fn_agent_sleep, "sleep", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, agent, "monotonicNow", js_new_function(ctx, fn_agent_now, "monotonicNow", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "agent", agent, JS_PROP_HIDDEN);
    js_define_str(ctx, g, "print", js_new_function(ctx, fn_print, "print", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "gc", js_new_function(ctx, fn_gc, "gc", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "detachArrayBuffer", js_new_function(ctx, fn_detach, "detachArrayBuffer", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "evalScript", js_new_function(ctx, fn_eval_script, "evalScript", 1, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "createRealm", js_new_function(ctx, fn_create_realm, "createRealm", 0, NULL), JS_PROP_HIDDEN);
    js_define_str(ctx, t, "global", g, JS_PROP_HIDDEN);
    js_define_str(ctx, g, "$262", t, JS_PROP_HIDDEN);
}

/* ------------------------------------------------------------------ one run */

typedef struct Run {
    char *id, *file, *mode, *includes, *flags, *neg_phase, *neg_type;
} Run;

static bool has_item(const char *csv, const char *item) {
    size_t n = strlen(item);
    for (const char *p = csv; *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, item, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* the constructor's name of an exception value, and the first line of its report */
static void describe_exception(JSContext *ctx, JSValue e, char *name, size_t ncap, char *msg, size_t mcap) {
    name[0] = 0;
    if (js_is_object(e)) {
        JSValue c = js_get_str(ctx, e, "constructor");
        if (c != JS_EXCEPTION && js_is_object(c)) {
            JSValue n = js_get_str(ctx, c, "name");
            if (n != JS_EXCEPTION && js_is_string(n)) {
                char *s = js_to_utf8(ctx, n, NULL);
                if (s) { strncpy(name, s, ncap - 1); name[ncap - 1] = 0; js_free_utf8(ctx, s); }
            }
        }
        if (js_has_exception(ctx)) js_take_exception(ctx);
    } else strncpy(name, "(not an object)", ncap - 1);
    char *r = js_error_report(ctx, e);
    size_t i = 0;
    if (r) {
        for (; r[i] && r[i] != '\n' && i + 1 < mcap; i++) msg[i] = r[i];
        js_free_utf8(ctx, r);
    }
    msg[i] = 0;
}

/* evaluates one harness file; false (with msg) when it fails */
static bool eval_harness(JSContext *ctx, const char *name, char *msg, size_t mcap) {
    char path[1024], n2[64];
    snprintf(path, sizeof path, "%s/%s", g_harness, name);
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { snprintf(msg, mcap, "missing harness file %s", name); return false; }
    JSValue r = js_eval(ctx, src, len, name, JS_EVAL_SCRIPT);
    free(src);
    if (r == JS_EXCEPTION) {
        char buf[300];
        describe_exception(ctx, js_take_exception(ctx), n2, sizeof n2, buf, sizeof buf);
        snprintf(msg, mcap, "harness %s: %s", name, buf);
        return false;
    }
    return true;
}

static bool interrupted(const char *msg) { return strstr(msg, "interrupted") != NULL; }

static const char *run_one(const Run *r, char *msg, size_t mcap) {
    msg[0] = 0;
    g_print_len = 0;
    g_print[0] = 0;
    g_job_error[0] = 0;
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    JSRuntimeOptions o = { 0 };
    o.memory_limit = g_mem;
    JSRuntime *rt = js_runtime_new(&o);
    if (!rt) { snprintf(msg, mcap, "cannot create a runtime"); return "FAIL"; }
    JSHostHooks hooks = { 0 };
    hooks.report_error = report_error;
    hooks.now_ms = now_ms;
    hooks.tz_offset_min = tz_offset;
    hooks.sleep_ms = sleep_ms;
    hooks.load_module = modload_hook;
    js_runtime_set_host_hooks(rt, &hooks);
    js_runtime_set_interrupt_handler(rt, interrupt, NULL);
    const char *status = "FAIL";
    char *text = NULL, *src = NULL;
    JSContext *ctx = js_context_new(rt);
    if (!ctx) { snprintf(msg, mcap, "cannot create a context"); goto done; }
    bool raw = !strcmp(r->mode, "raw"), strict = !strcmp(r->mode, "strict"), module = !strcmp(r->mode, "module");
    bool async = has_item(r->flags, "async");
    install_262(ctx);
    if (!raw) {
        if (!eval_harness(ctx, "assert.js", msg, mcap) || !eval_harness(ctx, "sta.js", msg, mcap)) goto done;
        if (async && !eval_harness(ctx, "doneprintHandle.js", msg, mcap)) goto done;
        char *inc = strdup(r->includes);
        for (char *p = strtok(inc, ","); p; p = strtok(NULL, ","))
            if (!eval_harness(ctx, p, msg, mcap)) { free(inc); goto done; }
        free(inc);
    }
    size_t tlen;
    text = read_file(r->file, &tlen);
    if (!text) { snprintf(msg, mcap, "cannot read %s", r->file); goto done; }
    const char *prefix = strict ? "\"use strict\";\n" : "";
    size_t plen = strlen(prefix);
    src = malloc(plen + tlen + 1);
    memcpy(src, prefix, plen);
    memcpy(src + plen, text, tlen + 1);
    bool neg = r->neg_phase[0] != 0;
    bool neg_early = neg && (!strcmp(r->neg_phase, "parse") || !strcmp(r->neg_phase, "resolution"));
    char name[96], emsg[300];
    if (module) {
        /* a module: js_eval loads what it imports (the fixtures next to the test), links and runs it, and gives a
           promise; a syntax error in the test itself is thrown, anything else (in a dependency too) rejects it */
        char file[1024];
        snprintf(file, sizeof file, "%s", r->file);
        modload_normalize(file);
        JSValue p = js_eval(ctx, src, plen + tlen, file, JS_EVAL_MODULE);
        if (p == JS_EXCEPTION) {
            describe_exception(ctx, js_take_exception(ctx), name, sizeof name, emsg, sizeof emsg);
            if (interrupted(emsg)) { status = "TIMEOUT"; snprintf(msg, mcap, "while parsing"); goto done; }
            if (neg_early && !strcmp(name, r->neg_type)) { status = "PASS"; goto done; }
            if (neg_early) snprintf(msg, mcap, "expected %s at %s, got %s: %s", r->neg_type, r->neg_phase, name, emsg);
            else snprintf(msg, mcap, "unexpected early error %s: %s", name, emsg);
            goto done;
        }
        js_promise_set_handled(ctx, p);
        js_run_jobs(rt, 0);
        JSPromiseState st = js_promise_state(ctx, p);
        if (st == JS_PROMISE_REJECTED) {
            describe_exception(ctx, js_promise_result(ctx, p), name, sizeof name, emsg, sizeof emsg);
            if (interrupted(emsg)) { status = "TIMEOUT"; snprintf(msg, mcap, "timeout"); goto done; }
            if (neg && !strcmp(name, r->neg_type)) { status = "PASS"; goto done; }
            if (neg) snprintf(msg, mcap, "expected %s at %s, got %s: %s", r->neg_type, r->neg_phase, name, emsg);
            else snprintf(msg, mcap, "%s", emsg);
            goto done;
        }
        if (neg) { snprintf(msg, mcap, "expected %s at %s, but nothing was thrown", r->neg_type, r->neg_phase); goto done; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - g_start.tv_sec) * 1000 + (now.tv_nsec - g_start.tv_nsec) / 1000000 > g_limit_ms) {
            status = "TIMEOUT";
            snprintf(msg, mcap, "timeout");
            goto done;
        }
        if (!async) {
            if (st == JS_PROMISE_PENDING) snprintf(msg, mcap, "the module never finished (an unsettled top-level await)");
            else status = "PASS";
            goto done;
        }
        if (strstr(g_print, "Test262:AsyncTestFailure")) {
            char *q = strstr(g_print, "Test262:AsyncTestFailure");
            size_t i = 0;
            for (; q[i] && q[i] != '\n' && i + 1 < mcap; i++) msg[i] = q[i];
            msg[i] = 0;
        } else if (strstr(g_print, "Test262:AsyncTestComplete")) status = "PASS";
        else snprintf(msg, mcap, "the async test never completed%s%s", g_job_error[0] ? ": " : "", g_job_error);
        goto done;
    }
    JSValue f = js_eval(ctx, src, plen + tlen, r->file, JS_EVAL_COMPILE);
    if (f == JS_EXCEPTION) {
        describe_exception(ctx, js_take_exception(ctx), name, sizeof name, emsg, sizeof emsg);
        if (interrupted(emsg)) { status = "TIMEOUT"; snprintf(msg, mcap, "while parsing"); goto done; }
        if (neg_early && !strcmp(name, r->neg_type)) { status = "PASS"; goto done; }
        if (neg_early) snprintf(msg, mcap, "expected %s at %s, got %s: %s", r->neg_type, r->neg_phase, name, emsg);
        else snprintf(msg, mcap, "unexpected early error %s: %s", name, emsg);
        goto done;
    }
    if (neg_early) { snprintf(msg, mcap, "expected %s at %s, but the source was accepted", r->neg_type, r->neg_phase); goto done; }
    JSValue v = js_call(ctx, f, js_global_object(ctx), 0, NULL);       /* a script's `this` is the global object, strict or not */
    if (v == JS_EXCEPTION) {
        describe_exception(ctx, js_take_exception(ctx), name, sizeof name, emsg, sizeof emsg);
        if (interrupted(emsg)) { status = "TIMEOUT"; snprintf(msg, mcap, "timeout"); goto done; }
        if (neg && !strcmp(name, r->neg_type)) { status = "PASS"; goto done; }
        if (neg) snprintf(msg, mcap, "expected %s at runtime, got %s: %s", r->neg_type, name, emsg);
        else snprintf(msg, mcap, "%s", emsg);
        goto done;
    }
    if (neg) { snprintf(msg, mcap, "expected %s at runtime, but nothing was thrown", r->neg_type); goto done; }
    js_run_jobs(rt, 0);
    if (async) {
        if (strstr(g_print, "Test262:AsyncTestFailure")) {
            char *p = strstr(g_print, "Test262:AsyncTestFailure");
            size_t i = 0;
            for (; p[i] && p[i] != '\n' && i + 1 < mcap; i++) msg[i] = p[i];
            msg[i] = 0;
        } else if (strstr(g_print, "Test262:AsyncTestComplete")) status = "PASS";
        else if (interrupted(g_job_error)) { status = "TIMEOUT"; snprintf(msg, mcap, "timeout in a job"); }
        else snprintf(msg, mcap, "the async test never completed%s%s", g_job_error[0] ? ": " : "", g_job_error);
        goto done;
    }
    if (interrupted(g_job_error)) { status = "TIMEOUT"; snprintf(msg, mcap, "timeout in a job"); goto done; }
    status = "PASS";
done:
    free(text);
    free(src);
    js_runtime_free(rt);
    return status;
}

/* ------------------------------------------------------------------ main */

static void clean(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i++) {
        if (!strcmp(argv[i], "--time-limit") && i + 1 < argc) g_limit_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) g_mem = (size_t)atol(argv[++i]) << 20;
        else { fprintf(stderr, "t262tool: unknown option %s\n", argv[i]); return 2; }
    }
    if (argc - i != 2) { fprintf(stderr, "usage: t262tool [--time-limit MS] [--mem MB] HARNESS_DIR MANIFEST\n"); return 2; }
    g_harness = argv[i];
    size_t mlen;
    char *manifest = read_file(argv[i + 1], &mlen);
    if (!manifest) { fprintf(stderr, "t262tool: cannot read %s\n", argv[i + 1]); return 2; }
    for (char *line = manifest; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *f[7];
        char *p = line;
        for (int k = 0; k < 7; k++) {
            f[k] = p;
            char *t = k < 6 ? strchr(p, '\t') : NULL;
            if (t) { *t = 0; p = t + 1; } else p = p + strlen(p);
        }
        if (f[0][0]) {
            Run r = { f[0], f[1], f[2], f[3], f[4], f[5], f[6] };
            char msg[400];
            printf("B\t%s\n", r.id);
            fflush(stdout);
            const char *st = run_one(&r, msg, sizeof msg);
            clean(msg);
            printf("R\t%s\t%s\t%s\n", r.id, st, msg);
            fflush(stdout);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(manifest);
    return 0;
}
