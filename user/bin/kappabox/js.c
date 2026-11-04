/*
 * js: run JavaScript (Kosuzu, user/lib/js) from a file, from the command line, from standard input, or interactively.
 *
 *   js [-s] [-m MB] [-t SECONDS] FILE [ARG...]       run a script
 *   js [...] -e CODE [ARG...]    -p CODE [ARG...]    run CODE (-p prints the value of the last expression)
 *   js [...] -                                       run the program read from standard input
 *   js [-i]                                          an interactive session (when standard input is a terminal)
 *   js --module ...                                  run as an ES module (also any FILE.mjs): import, export, import(),
 *                                                    top-level await; imports are files, relative to the importing one
 *
 * What a script sees beyond the language: console (log, error, table-less Node-style printing; js_prelude.h), the global
 * print, process (argv, env, exit, exitCode, stdout.write, stderr.write, cwd, platform), timers (setTimeout, setInterval,
 * setImmediate, queueMicrotask), and four functions that read and write the outside world: readFile, writeFile, readLine
 * and readStdin, plus sleep.  Everything runs in one realm; after the program ends the event loop runs until no timer
 * is left, then process.exitCode (or 0) is the exit status.
 *
 * Exit status: 0 normally, 1 after an uncaught error (also an unhandled promise rejection), 2 for a usage error or a file
 * that cannot be read, 13 when a module is left waiting for a top-level await nothing can settle, 124 when the time limit
 * (-t) stopped the program, else the code given to process.exit().
 * Limits: -m sets the memory limit (a script that exceeds it gets a catchable RangeError "out of memory"), -t a time limit
 * for the whole run (for each input in the interactive session).  Ctrl-C stops a running script.
 */
#include "kappabox.h"
#include <js.h>
#include "js_prelude.h"

#define JS_DEFAULT_MEM_MB 64

extern char **environ;

/* ------------------------------------------------------------------ state */

typedef struct Timer {
    struct Timer *next;
    int id;
    double due;                     /* monotonic milliseconds */
    double interval;                /* >= 0 for setInterval (milliseconds), < 0 for a one-shot timer */
    JSValue fn, args;               /* pinned: the callback and an array of its arguments */
} Timer;

static JSRuntime *g_rt;
static JSContext *g_ctx;
static JSValue g_inspect = JS_UNDEFINED;        /* the prelude's inspect(), pinned */
static Timer *g_timers;
static int g_next_timer = 1;
static int g_status;                            /* exit status so far (an uncaught error makes it 1) */
static bool g_exiting;
static volatile int g_sigint;                    /* (set by the SIGINT handler: one int is written atomically) */
static double g_limit_ms;                       /* the time limit in milliseconds, 0 = none */
static double g_deadline;                       /* monotonic milliseconds at which the run is stopped, 0 = none */
static bool g_timed_out;
static bool g_repl;
static JSValue g_main = JS_UNDEFINED;           /* the promise of the main module (pinned) until the program has ended */

/* ------------------------------------------------------------------ time */

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double real_ms(void *opaque) {
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
}

static char g_tzname[16];

static int tz_offset_min(void *opaque, double utc_ms) {
    (void)opaque;
    long long secs = (long long)(utc_ms / 1000.0);
    if ((double)secs * 1000.0 > utc_ms) secs--;                 /* floor, also before 1970 */
    time_t t = (time_t)secs;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    snprintf(g_tzname, sizeof g_tzname, "%s", tm.tm_zone ? tm.tm_zone : "");
    return (int)(tm.tm_gmtoff / 60);
}

static const char *tz_name(void *opaque, double utc_ms) {
    tz_offset_min(opaque, utc_ms);
    return g_tzname[0] ? g_tzname : NULL;
}

static void sleep_ms(void *opaque, double ms) {
    (void)opaque;
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000.0), (long)((ms - (double)(time_t)(ms / 1000.0) * 1000.0) * 1e6) };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ output (buffered unless it is a terminal) */

static char g_obuf[8192];
static size_t g_olen;

static void out_flush(void) {
    size_t off = 0;
    while (off < g_olen) {
        ssize_t w = write(1, g_obuf + off, g_olen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    g_olen = 0;
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void put_out(int fd, const char *p, size_t n) {
    if (fd == 2) {
        out_flush();
        write_all(2, p, n);
        return;
    }
    if (n >= sizeof g_obuf) {
        out_flush();
        write_all(1, p, n);
        return;
    }
    if (g_olen + n > sizeof g_obuf) out_flush();
    memcpy(g_obuf + g_olen, p, n);
    g_olen += n;
    if (isatty(1)) out_flush();
}

static void put_str(int fd, const char *s) { put_out(fd, s, strlen(s)); }

/* ------------------------------------------------------------------ standard input */

static char g_ibuf[4096];
static size_t g_ipos, g_ilen;
static bool g_ieof;

static int in_fill(void) {
    if (g_ieof) return 0;
    out_flush();                                    /* a prompt written without a newline must be seen before we wait */
    for (;;) {
        ssize_t r = read(0, g_ibuf, sizeof g_ibuf);
        if (r > 0) { g_ipos = 0; g_ilen = (size_t)r; return 1; }
        if (r < 0 && errno == EINTR) { if (g_sigint) return 0; continue; }
        g_ieof = true;
        return 0;
    }
}

/* the rest of standard input (malloc'd, NUL terminated) */
static char *read_all_stdin(size_t *len) {
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    for (;;) {
        if (g_ipos >= g_ilen && !in_fill()) break;
        size_t chunk = g_ilen - g_ipos;
        if (n + chunk + 1 > cap) {
            while (n + chunk + 1 > cap) cap *= 2;
            char *nb = realloc(b, cap);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
        memcpy(b + n, g_ibuf + g_ipos, chunk);
        n += chunk;
        g_ipos = g_ilen;
    }
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* one line without its line terminator (malloc'd), NULL at the end of the input */
static char *read_line_stdin(size_t *len) {
    size_t cap = 128, n = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    bool any = false;
    for (;;) {
        if (g_ipos >= g_ilen && !in_fill()) break;
        char c = g_ibuf[g_ipos++];
        any = true;
        if (c == '\n') break;
        if (n + 2 > cap) {
            cap *= 2;
            char *nb = realloc(b, cap);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
        b[n++] = c;
    }
    if (!any) { free(b); return NULL; }
    if (n && b[n - 1] == '\r') n--;
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* ------------------------------------------------------------------ native functions */

static JSValue host_write(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    int32_t fd = 1;
    if (js_to_int32(ctx, argv[0], &fd)) return JS_EXCEPTION;
    size_t len;
    char *s = js_to_utf8(ctx, argv[1], &len);
    if (!s) return JS_EXCEPTION;
    put_out(fd == 2 ? 2 : 1, s, len);
    js_free_utf8(ctx, s);
    return JS_UNDEFINED;
}

static JSValue host_now(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)data;
    return js_number(mono_ms());
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

static JSValue fn_exit(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)data;
    int32_t code = g_status;
    if (argc > 0 && argv[0] != JS_UNDEFINED) {
        if (js_to_int32(ctx, argv[0], &code)) return JS_EXCEPTION;
    } else {
        JSValue p = js_get_str(ctx, js_global_object(ctx), "process");
        JSValue ec = js_is_object(p) ? js_get_str(ctx, p, "exitCode") : JS_UNDEFINED;
        if (js_is_number(ec)) js_to_int32(ctx, ec, &code);
    }
    out_flush();
    exit(code & 0xFF);
}

static JSValue fn_read_file(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    char *path = js_to_utf8(ctx, argv[0], NULL);
    if (!path) return JS_EXCEPTION;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        JSValue e = js_throw_error(ctx, JS_ERROR, "readFile: %s: %s", path, strerror(errno));
        js_free_utf8(ctx, path);
        return e;
    }
    size_t len;
    char *text = kb_read_all(fd, &len);
    close(fd);
    if (!text) {
        JSValue e = js_throw_error(ctx, JS_ERROR, "readFile: %s: %s", path, strerror(errno));
        js_free_utf8(ctx, path);
        return e;
    }
    js_free_utf8(ctx, path);
    JSValue s = js_string(ctx, text, len);
    free(text);
    return s;
}

static JSValue fn_write_file(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    char *path = js_to_utf8(ctx, argv[0], NULL);
    if (!path) return JS_EXCEPTION;
    size_t len;
    char *text = js_to_utf8(ctx, argv[1], &len);
    if (!text) { js_free_utf8(ctx, path); return JS_EXCEPTION; }
    bool append = js_to_bool(ctx, argv[2]);
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    JSValue r = JS_UNDEFINED;
    if (fd < 0) r = js_throw_error(ctx, JS_ERROR, "writeFile: %s: %s", path, strerror(errno));
    else {
        write_all(fd, text, len);
        if (close(fd) < 0) r = js_throw_error(ctx, JS_ERROR, "writeFile: %s: %s", path, strerror(errno));
    }
    js_free_utf8(ctx, text);
    js_free_utf8(ctx, path);
    return r;
}

static JSValue fn_read_line(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    size_t len;
    char *line = read_line_stdin(&len);
    if (!line) return JS_NULL;
    JSValue s = js_string(ctx, line, len);
    free(line);
    return s;
}

static JSValue fn_read_stdin(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    size_t len;
    char *all = read_all_stdin(&len);
    if (!all) return js_throw_error(ctx, JS_ERROR, "readStdin: out of memory");
    JSValue s = js_string(ctx, all, len);
    free(all);
    return s;
}

static JSValue fn_sleep(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    double ms;
    if (js_to_number(ctx, argv[0], &ms)) return JS_EXCEPTION;
    double until = mono_ms() + (ms > 0 ? ms : 0);
    for (;;) {                                      /* in slices, so that Ctrl-C and the time limit are noticed */
        double left = until - mono_ms();
        if (left <= 0) break;
        sleep_ms(NULL, left < 20 ? left : 20);
        if (g_sigint) return js_throw_error(ctx, JS_ERROR, "interrupted");
        if (g_deadline > 0 && mono_ms() > g_deadline) { g_timed_out = true; return js_throw_error(ctx, JS_ERROR, "interrupted"); }
    }
    return JS_UNDEFINED;
}

static JSValue fn_cwd(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)argv; (void)data;
    char buf[1024];
    if (!getcwd(buf, sizeof buf)) return js_throw_error(ctx, JS_ERROR, "cwd: %s", strerror(errno));
    return js_string(ctx, buf, (size_t)-1);
}

/* ---- modules: the loader the engine asks for every import (JSHostHooks.load_module)
 *
 * A module's name is an absolute, normalized path; a specifier is an absolute path, or one that starts with ./ or ../ and
 * is taken from the directory of the module that imports (for code that is not a module, from the directory of its file,
 * or the current one).  The source and the name the hook returns stay here until the next call; the engine copies them. */

static char *g_mod_src, *g_mod_name;

/* collapses "." and ".." segments and repeated slashes of an absolute path, in place */
static void path_normalize(char *p) {
    if (p[0] != '/') return;
    char *w = p + 1;
    const char *r = p + 1;
    while (*r) {
        while (*r == '/') r++;
        if (!*r) break;
        const char *s = r;
        while (*r && *r != '/') r++;
        size_t l = (size_t)(r - s);
        if (l == 1 && s[0] == '.') continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (w > p + 1) {
                while (w > p + 1 && w[-1] != '/') w--;
                if (w > p + 1) w--;
            }
            continue;
        }
        if (w > p + 1) *w++ = '/';
        memmove(w, s, l);
        w += l;
    }
    *w = 0;
}

/* the absolute, normalized form of a path (relative ones are taken from the current directory) */
static void abs_path(const char *file, char *out, size_t cap) {
    if (file[0] == '/') snprintf(out, cap, "%s", file);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
        snprintf(out, cap, "%s/%s", cwd, file);
    }
    path_normalize(out);
}

static char *module_load(JSContext *ctx, const char *spec, const char *referrer, size_t *len, char **resolved,
                         const char **error, void *opaque) {
    (void)ctx; (void)opaque;
    free(g_mod_src);
    free(g_mod_name);
    g_mod_src = g_mod_name = NULL;
    char path[1024];
    if (spec[0] == '/') snprintf(path, sizeof path, "%s", spec);
    else if (!strncmp(spec, "./", 2) || !strncmp(spec, "../", 3) || !strcmp(spec, ".") || !strcmp(spec, "..")) {
        char base[1024];
        if (strchr(referrer, '/')) {
            abs_path(referrer, base, sizeof base);
            char *slash = strrchr(base, '/');
            if (slash == base) base[1] = 0; else *slash = 0;
        } else if (!getcwd(base, sizeof base)) {
            *error = "cannot find the current directory";
            return NULL;
        }
        if (snprintf(path, sizeof path, "%s/%s", base, spec) >= (int)sizeof path) {
            *error = "the path is too long";
            return NULL;
        }
        path_normalize(path);
    } else {
        *error = "only relative and absolute paths can be imported";
        return NULL;
    }
    path_normalize(path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *error = strerror(errno);
        return NULL;
    }
    g_mod_src = kb_read_all(fd, len);
    close(fd);
    if (!g_mod_src) {
        *error = strerror(errno);
        return NULL;
    }
    g_mod_name = strdup(path);
    *resolved = g_mod_name;
    return g_mod_src;
}

/* import.meta: url, filename and dirname of the module */
static void module_meta(JSContext *ctx, JSValue meta, const char *name, void *opaque) {
    (void)opaque;
    char url[1100], dir[1024];
    snprintf(url, sizeof url, "file://%.1000s", name);
    snprintf(dir, sizeof dir, "%s", name);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = 0; else if (slash) *slash = 0;
    js_define_str(ctx, meta, "url", js_string(ctx, url, (size_t)-1), JS_PROP_DEFAULT);
    js_define_str(ctx, meta, "filename", js_string(ctx, name, (size_t)-1), JS_PROP_DEFAULT);
    js_define_str(ctx, meta, "dirname", js_string(ctx, dir, (size_t)-1), JS_PROP_DEFAULT);
}

/* ---- timers: setTimeout (kind 0), setInterval (1), setImmediate (2) */

static void timer_free(Timer *t) {
    js_unpin(g_ctx, t->fn);
    js_unpin(g_ctx, t->args);
    free(t);
}

static JSValue fn_set_timer(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val;
    int kind = (int)(intptr_t)data;
    if (!js_is_function(ctx, argv[0])) return js_throw_error(ctx, JS_TYPE_ERROR, "The \"callback\" argument must be of type function");
    double ms = 0;
    int first_extra = kind == 2 ? 1 : 2;
    if (kind != 2 && argc > 1 && js_to_number(ctx, argv[1], &ms)) return JS_EXCEPTION;
    if (!(ms >= 1)) ms = kind == 2 ? 0 : 1;          /* (like Node: a delay below 1 ms, or NaN, is 1 ms) */
    if (ms > 2147483647.0) ms = 1;
    Timer *t = calloc(1, sizeof *t);
    if (!t) return js_throw_error(ctx, JS_ERROR, "out of memory");
    JSValue args = js_new_array(ctx, 0);
    int n = 0;
    for (int i = first_extra; i < argc; i++) js_set_index(ctx, args, (uint32_t)n++, argv[i]);
    t->id = g_next_timer++;
    t->due = mono_ms() + (kind == 2 ? 0 : ms);
    t->interval = kind == 1 ? ms : -1;
    t->fn = argv[0];
    t->args = args;
    js_pin(ctx, t->fn);
    js_pin(ctx, t->args);
    Timer **pp = &g_timers;
    while (*pp && (*pp)->due <= t->due) pp = &(*pp)->next;      /* sorted by due time, equal times in the order made */
    t->next = *pp;
    *pp = t;
    return js_int(t->id);
}

static JSValue fn_clear_timer(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    (void)this_val; (void)argc; (void)data;
    int32_t id = 0;
    if (!js_is_number(argv[0]) || js_to_int32(ctx, argv[0], &id)) return JS_UNDEFINED;
    for (Timer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            Timer *t = *pp;
            *pp = t->next;
            timer_free(t);
            break;
        }
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------ reporting errors */

static void report_exception(JSValue e, const char *prefix) {
    char *s = js_error_report(g_ctx, e);
    out_flush();
    fprintf(stderr, "%s%s\n", prefix, s ? s : "(an error that cannot be shown)");
    js_free_utf8(g_ctx, s);
}

static bool is_interrupt(JSValue e) {
    if (!js_is_error(g_ctx, e)) return false;
    JSValue m = js_get_str(g_ctx, e, "message");
    if (!js_is_string(m)) return false;
    char *s = js_to_utf8(g_ctx, m, NULL);
    bool r = s && !strcmp(s, "interrupted");
    js_free_utf8(g_ctx, s);
    return r;
}

/* Ctrl-C or the time limit: says so and returns true (a script then ends, the interactive session goes on) */
static bool stop_requested(void) {
    if (g_sigint) {
        out_flush();
        fprintf(stderr, "js: interrupted\n");
        g_status = g_repl ? g_status : 130;
        g_exiting = !g_repl;
        return true;
    }
    if (g_timed_out || (g_deadline > 0 && mono_ms() > g_deadline)) {
        out_flush();
        fprintf(stderr, "js: time limit of %g seconds exceeded\n", g_limit_ms / 1000.0);
        g_status = g_repl ? g_status : 124;
        g_exiting = !g_repl;
        g_timed_out = false;
        g_deadline = 0;
        return true;
    }
    return false;
}

/* an exception is pending: shows it and sets the exit status (the time limit and Ctrl-C have their own messages) */
static void handle_pending_exception(void) {
    JSValue e = js_take_exception(g_ctx);
    if ((g_timed_out || g_sigint) && is_interrupt(e) && stop_requested()) return;
    report_exception(e, g_repl ? "Uncaught " : "");
    if (!g_repl) { g_status = 1; g_exiting = true; }
}

/* the main module's promise has been rejected: an error nobody caught, which ends the run like an uncaught exception */
static bool main_module_failed(void) {
    if (g_main == JS_UNDEFINED || js_promise_state(g_ctx, g_main) != JS_PROMISE_REJECTED) return false;
    JSValue e = js_promise_result(g_ctx, g_main);
    js_unpin(g_ctx, g_main);
    g_main = JS_UNDEFINED;
    if ((g_timed_out || g_sigint) && is_interrupt(e) && stop_requested()) return true;
    report_exception(e, g_repl ? "Uncaught " : "");
    if (!g_repl) { g_status = 1; g_exiting = true; }
    return true;
}

static void report_job_error(JSContext *ctx, JSValue err, bool rejection, void *opaque) {
    (void)ctx; (void)opaque;
    report_exception(err, rejection ? "Uncaught (in promise) " : "Uncaught ");
    if (!g_repl) { g_status = 1; g_exiting = true; }
}

static int interrupt_cb(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (g_sigint) return 1;
    if (g_deadline > 0 && mono_ms() > g_deadline) { g_timed_out = true; return 1; }
    return 0;
}

static void on_sigint(int sig) { (void)sig; g_sigint = 1; }

/* ------------------------------------------------------------------ the event loop */

/* runs due timers and promise jobs until nothing is left; stops early after process.exit, an uncaught error, the time
   limit or Ctrl-C */
static void run_event_loop(void) {
    while (!g_exiting) {
        js_run_jobs(g_rt, 0);
        if (js_has_exception(g_ctx)) { handle_pending_exception(); break; }
        if (main_module_failed()) break;
        if (g_exiting || !g_timers) break;
        Timer *t = g_timers;
        double now = mono_ms();
        if (t->due > now) {
            double wait = t->due - now;
            if (g_deadline > 0 && g_deadline - now < wait) wait = g_deadline - now;
            sleep_ms(NULL, wait < 50 ? wait : 50);
            if (stop_requested()) break;
            continue;
        }
        g_timers = t->next;
        int mark = js_scope_open(g_ctx);
        JSValue fn = t->fn;
        uint32_t n = js_array_length(g_ctx, t->args);
        JSValue av[16];
        if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) av[i] = js_get_index(g_ctx, t->args, i);
        bool again = t->interval >= 0;
        if (again) {                                    /* an interval is queued again before it runs, so it can clear itself */
            t->due = mono_ms() + t->interval;
            Timer **pp = &g_timers;
            while (*pp && (*pp)->due <= t->due) pp = &(*pp)->next;
            t->next = *pp;
            *pp = t;
        }
        JSValue r = js_call(g_ctx, fn, JS_UNDEFINED, (int)n, av);
        if (!again) timer_free(t);
        if (r == JS_EXCEPTION) handle_pending_exception();
        js_scope_close(g_ctx, mark);
    }
}

/* ------------------------------------------------------------------ setting up the realm */

static void define_fn(JSValue obj, const char *name, JSNativeFunction *fn, int length, intptr_t data) {
    js_define_str(g_ctx, obj, name, js_new_function(g_ctx, fn, name, length, (void *)data), JS_PROP_HIDDEN);
}

static bool install_prelude(void) {
    JSValue host = js_new_object(g_ctx);
    define_fn(host, "write", host_write, 2, 0);
    define_fn(host, "now", host_now, 0, 0);
    define_fn(host, "promiseState", host_promise_state, 1, 0);
    JSValue fn = js_eval(g_ctx, kb_js_prelude, strlen(kb_js_prelude), "<console>", JS_EVAL_SCRIPT);
    JSValue r = fn == JS_EXCEPTION ? fn : js_call(g_ctx, fn, JS_UNDEFINED, 1, &host);
    if (r == JS_EXCEPTION) {
        report_exception(js_take_exception(g_ctx), "js: cannot start the console: ");
        return false;
    }
    g_inspect = js_get_str(g_ctx, r, "inspect");
    js_pin(g_ctx, g_inspect);
    return true;
}

static void install_globals(int nargs, char **args, bool have_script, const char *script) {
    JSValue g = js_global_object(g_ctx);
    define_fn(g, "readFile", fn_read_file, 1, 0);
    define_fn(g, "writeFile", fn_write_file, 2, 0);
    define_fn(g, "readLine", fn_read_line, 0, 0);
    define_fn(g, "readStdin", fn_read_stdin, 0, 0);
    define_fn(g, "sleep", fn_sleep, 1, 0);
    define_fn(g, "setTimeout", fn_set_timer, 2, 0);
    define_fn(g, "setInterval", fn_set_timer, 2, 1);
    define_fn(g, "setImmediate", fn_set_timer, 1, 2);
    define_fn(g, "clearTimeout", fn_clear_timer, 1, 0);
    define_fn(g, "clearInterval", fn_clear_timer, 1, 0);
    define_fn(g, "clearImmediate", fn_clear_timer, 1, 0);
    JSValue proc = js_new_object(g_ctx);
    JSValue argv = js_new_array(g_ctx, 0);
    uint32_t n = 0;
    js_set_index(g_ctx, argv, n++, js_string(g_ctx, "js", (size_t)-1));
    if (have_script) js_set_index(g_ctx, argv, n++, js_string(g_ctx, script, (size_t)-1));
    for (int i = 0; i < nargs; i++) js_set_index(g_ctx, argv, n++, js_string(g_ctx, args[i], (size_t)-1));
    js_define_str(g_ctx, proc, "argv", argv, JS_PROP_DEFAULT);
    JSValue env = js_new_object(g_ctx);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char name[256];
        size_t nl = (size_t)(eq - *e);
        if (nl >= sizeof name) continue;
        memcpy(name, *e, nl);
        name[nl] = 0;
        js_define_str(g_ctx, env, name, js_string(g_ctx, eq + 1, (size_t)-1), JS_PROP_DEFAULT);
    }
    js_define_str(g_ctx, proc, "env", env, JS_PROP_DEFAULT);
    js_define_str(g_ctx, proc, "platform", js_string(g_ctx, "touhou", (size_t)-1), JS_PROP_DEFAULT);
    js_define_str(g_ctx, proc, "version", js_string(g_ctx, "kosuzu", (size_t)-1), JS_PROP_DEFAULT);
    js_define_str(g_ctx, proc, "exitCode", JS_UNDEFINED, JS_PROP_DEFAULT);
    define_fn(proc, "exit", fn_exit, 1, 0);
    define_fn(proc, "cwd", fn_cwd, 0, 0);
    JSValue so = js_new_object(g_ctx), se = js_new_object(g_ctx);
    js_define_str(g_ctx, so, "isTTY", js_bool(isatty(1)), JS_PROP_DEFAULT);
    js_define_str(g_ctx, se, "isTTY", js_bool(isatty(2)), JS_PROP_DEFAULT);
    js_define_str(g_ctx, proc, "stdout", so, JS_PROP_DEFAULT);
    js_define_str(g_ctx, proc, "stderr", se, JS_PROP_DEFAULT);
    js_define_str(g_ctx, g, "process", proc, JS_PROP_HIDDEN);
    /* the pieces that are simplest in JavaScript itself */
    static const char glue[] =
        "(function (proc, hostWrite) {"
        "  proc.stdout.write = function (s) { hostWrite(1, String(s)); return true; };"
        "  proc.stderr.write = function (s) { hostWrite(2, String(s)); return true; };"
        "  proc.nextTick = function (f) { var a = Array.prototype.slice.call(arguments, 1); Promise.resolve().then(function () { f.apply(undefined, a); }); };"
        "  Object.defineProperty(globalThis, \"queueMicrotask\", { value: function queueMicrotask(f) {"
        "    if (typeof f !== \"function\") throw new TypeError(\"The \\\"callback\\\" argument must be of type function\");"
        "    Promise.resolve().then(f); }, writable: true, configurable: true });"
        "})";
    JSValue gf = js_eval(g_ctx, glue, sizeof glue - 1, "<glue>", JS_EVAL_SCRIPT);
    if (gf != JS_EXCEPTION) {
        JSValue hw = js_new_function(g_ctx, host_write, "write", 2, NULL);
        JSValue av[2] = { proc, hw };
        js_call(g_ctx, gf, JS_UNDEFINED, 2, av);
    }
}

/* ------------------------------------------------------------------ running programs */

static void arm_deadline(void) { g_deadline = g_limit_ms > 0 ? mono_ms() + g_limit_ms : 0; g_timed_out = false; }

/* evaluates source; the completion value is returned when want_value (the caller keeps it alive with the scope) */
static JSValue eval_source(const char *src, size_t len, const char *name, int flags) {
    return js_eval(g_ctx, src, len, name, flags);
}

static void print_value(JSValue v, bool raw_strings) {
    JSValue s = raw_strings && js_is_string(v) ? v : js_call(g_ctx, g_inspect, JS_UNDEFINED, 1, &v);
    if (s == JS_EXCEPTION) { js_take_exception(g_ctx); put_str(1, "(a value that cannot be shown)\n"); return; }
    size_t len;
    char *text = js_to_utf8(g_ctx, s, &len);
    if (text) {
        put_out(1, text, len);
        put_out(1, "\n", 1);
        js_free_utf8(g_ctx, text);
    }
}

static int finish(void) {
    if (!g_exiting) run_event_loop();
    if (!g_exiting && g_main != JS_UNDEFINED && js_promise_state(g_ctx, g_main) == JS_PROMISE_PENDING) {
        /* no job and no timer is left that could settle the main module's promise */
        if (g_timed_out || g_sigint) stop_requested();
        else {
            out_flush();
            fprintf(stderr, "js: the module never finished (an unsettled top-level await)\n");
            g_status = 13;
        }
    }
    out_flush();
    if (!g_status && !g_exiting) {
        JSValue p = js_get_str(g_ctx, js_global_object(g_ctx), "process");
        JSValue ec = js_is_object(p) ? js_get_str(g_ctx, p, "exitCode") : JS_UNDEFINED;
        int32_t code;
        if (js_is_number(ec) && js_to_int32(g_ctx, ec, &code) == 0) g_status = code & 0xFF;
    }
    return g_status;
}

/* runs a whole program: script text with a name */
static int run_program(const char *src, size_t len, const char *name, int flags, bool print_result) {
    arm_deadline();
    int mark = js_scope_open(g_ctx);
    JSValue r = eval_source(src, len, name, flags);
    if (r == JS_EXCEPTION) handle_pending_exception();
    else if (flags & JS_EVAL_MODULE) {
        /* the promise for the module's evaluation: the event loop reports its rejection (once), and a promise that stays
           pending when nothing is left to run is an unsettled top-level await */
        g_main = r;
        js_pin(g_ctx, r);
        js_promise_set_handled(g_ctx, r);
    } else if (print_result) print_value(r, true);
    js_scope_close(g_ctx, mark);
    return finish();
}

/* ------------------------------------------------------------------ the interactive session */

#define MAX_LINE 4096
#define MAX_HIST 500

static char *g_hist[MAX_HIST];
static int g_nhist;
static struct termios g_orig_term;
static bool g_raw;

static void raw_on(void) {
    if (g_raw) return;
    tcgetattr(0, &g_orig_term);
    struct termios t = g_orig_term;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~ICRNL;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    g_raw = true;
}

static void raw_off(void) {
    if (!g_raw) return;
    tcsetattr(0, TCSANOW, &g_orig_term);
    g_raw = false;
}

static int read_byte(void) {
    unsigned char c;
    for (;;) {
        ssize_t r = read(0, &c, 1);
        if (r == 1) return c;
        if (r == 0) return -1;
        if (errno != EINTR) return -1;
    }
}

static void hist_add(const char *line) {
    if (!*line || (g_nhist && !strcmp(g_hist[g_nhist - 1], line))) return;
    if (g_nhist == MAX_HIST) { free(g_hist[0]); memmove(g_hist, g_hist + 1, (MAX_HIST - 1) * sizeof *g_hist); g_nhist--; }
    g_hist[g_nhist++] = strdup(line);
}

static char *hist_path(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    return kb_join(home, ".js_history");
}

static void hist_load(void) {
    char *p = hist_path();
    FILE *f = p ? fopen(p, "r") : NULL;
    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            hist_add(line);
        }
        fclose(f);
    }
    free(p);
}

static void hist_save(void) {
    char *p = hist_path();
    FILE *f = p ? fopen(p, "w") : NULL;
    if (f) {
        int start = g_nhist > MAX_HIST ? g_nhist - MAX_HIST : 0;
        for (int i = start; i < g_nhist; i++) fprintf(f, "%s\n", g_hist[i]);
        fclose(f);
    }
    free(p);
}

static int utf8_cols(const char *s, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static void refresh(const char *prompt, const char *buf, size_t len, size_t pos) {
    int plen = utf8_cols(prompt, strlen(prompt));
    int cols = kb_term_cols();
    int avail = cols - plen - 1;
    if (avail < 8) avail = 8;
    size_t start = 0;
    /* (bytes, not columns: a line with wide text scrolls a little early, which is harmless) */
    if ((int)pos >= avail) start = pos - (size_t)avail + 1;
    while (start < pos && ((unsigned char)buf[start] & 0xC0) == 0x80) start++;
    size_t shown = len - start;
    if ((int)shown > avail) shown = (size_t)avail;
    char out[MAX_LINE + 256];
    int n = snprintf(out, sizeof out, "\r%s", prompt);
    memcpy(out + n, buf + start, shown);
    n += (int)shown;
    n += snprintf(out + n, sizeof out - (size_t)n, "\x1b[K\r");
    int col = plen + utf8_cols(buf + start, pos - start);
    if (col > 0) n += snprintf(out + n, sizeof out - (size_t)n, "\x1b[%dC", col);
    write_all(1, out, (size_t)n);
}

/* Reads one line with editing (NULL at the end of the input; "" after Ctrl-C).  Not a terminal: plain getline. */
static char *edit_line(const char *prompt) {
    if (!isatty(0)) return read_line_stdin(NULL);        /* (no prompts when the input is not a person) */
    out_flush();
    char buf[MAX_LINE];
    size_t len = 0, pos = 0;
    static char kill_buf[MAX_LINE];
    buf[0] = 0;
    int hist_idx = g_nhist;
    char *saved = NULL;
    raw_on();
    write_all(1, "\r", 1);
    write_all(1, prompt, strlen(prompt));
    for (;;) {
        int c = read_byte();
        if (c < 0) { raw_off(); free(saved); return NULL; }
        if (c == '\r' || c == '\n') {
            refresh(prompt, buf, len, len);
            write_all(1, "\r\n", 2);
            raw_off();
            free(saved);
            return strdup(buf);
        } else if (c == 3) {                                    /* Ctrl-C: abandon the line */
            write_all(1, "^C\r\n", 4);
            raw_off();
            free(saved);
            return strdup("");
        } else if (c == 4) {                                    /* Ctrl-D: end of input on an empty line, else delete */
            if (len == 0) { write_all(1, "\r\n", 2); raw_off(); free(saved); return NULL; }
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
        } else if (c == 127 || c == 8) {                        /* backspace (a whole UTF-8 character) */
            if (pos > 0) {
                size_t p = pos - 1;
                while (p > 0 && ((unsigned char)buf[p] & 0xC0) == 0x80) p--;
                memmove(buf + p, buf + pos, len - pos + 1);
                len -= pos - p;
                pos = p;
            }
        } else if (c == 1) pos = 0;                             /* Ctrl-A */
        else if (c == 5) pos = len;                             /* Ctrl-E */
        else if (c == 2) { if (pos) { pos--; while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos--; } }      /* Ctrl-B */
        else if (c == 6) { if (pos < len) { pos++; while (pos < len && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos++; } }   /* Ctrl-F */
        else if (c == 11) {                                     /* Ctrl-K: kill to the end */
            snprintf(kill_buf, sizeof kill_buf, "%s", buf + pos);
            buf[pos] = 0;
            len = pos;
        } else if (c == 21) {                                   /* Ctrl-U: kill to the start */
            snprintf(kill_buf, sizeof kill_buf, "%.*s", (int)pos, buf);
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
        } else if (c == 23) {                                   /* Ctrl-W: kill the word before */
            size_t p = pos;
            while (p > 0 && buf[p - 1] == ' ') p--;
            while (p > 0 && buf[p - 1] != ' ') p--;
            snprintf(kill_buf, sizeof kill_buf, "%.*s", (int)(pos - p), buf + p);
            memmove(buf + p, buf + pos, len - pos + 1);
            len -= pos - p;
            pos = p;
        } else if (c == 25) {                                   /* Ctrl-Y: yank */
            size_t kl = strlen(kill_buf);
            if (len + kl < MAX_LINE - 1) {
                memmove(buf + pos + kl, buf + pos, len - pos + 1);
                memcpy(buf + pos, kill_buf, kl);
                pos += kl;
                len += kl;
            }
        } else if (c == 12) {                                   /* Ctrl-L: clear the screen */
            write_all(1, "\x1b[2J\x1b[H", 7);
        } else if (c == 9) {                                    /* Tab: two spaces (there is no completion) */
            if (len + 2 < MAX_LINE - 1) {
                memmove(buf + pos + 2, buf + pos, len - pos + 1);
                buf[pos] = buf[pos + 1] = ' ';
                pos += 2;
                len += 2;
            }
        } else if (c == 27) {                                   /* escape sequences */
            int c1 = read_byte();
            if (c1 == '[' || c1 == 'O') {
                int params[4] = { 0 }, np = 0, fin;
                for (;;) {
                    fin = read_byte();
                    if (fin >= '0' && fin <= '9') { params[np] = params[np] * 10 + (fin - '0'); continue; }
                    if (fin == ';') { if (np < 3) np++; continue; }
                    break;
                }
                switch (fin) {
                case 'A':
                    if (hist_idx > 0) {
                        if (hist_idx == g_nhist) { free(saved); saved = strdup(buf); }
                        hist_idx--;
                        snprintf(buf, sizeof buf, "%s", g_hist[hist_idx]);
                        len = pos = strlen(buf);
                    }
                    break;
                case 'B':
                    if (hist_idx < g_nhist) {
                        hist_idx++;
                        snprintf(buf, sizeof buf, "%s", hist_idx == g_nhist ? (saved ? saved : "") : g_hist[hist_idx]);
                        len = pos = strlen(buf);
                    }
                    break;
                case 'C': if (pos < len) { pos++; while (pos < len && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos++; } break;
                case 'D': if (pos > 0) { pos--; while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos--; } break;
                case 'H': pos = 0; break;
                case 'F': pos = len; break;
                case '~':
                    if (params[0] == 3 && pos < len) { size_t e = pos + 1; while (e < len && ((unsigned char)buf[e] & 0xC0) == 0x80) e++; memmove(buf + pos, buf + e, len - e + 1); len -= e - pos; }
                    else if (params[0] == 1 || params[0] == 7) pos = 0;
                    else if (params[0] == 4 || params[0] == 8) pos = len;
                    break;
                default: break;
                }
            }
        } else if (c >= 32 && len < MAX_LINE - 2) {             /* a printable byte (and the bytes of UTF-8 characters) */
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char)c;
            len++;
        }
        refresh(prompt, buf, len, pos);
    }
}

/* a SyntaxError that only says the input ended too soon: the user is in the middle of a statement */
static bool needs_more_input(JSValue e) {
    if (!js_is_error(g_ctx, e)) return false;
    JSValue m = js_get_str(g_ctx, e, "message");
    char *s = js_is_string(m) ? js_to_utf8(g_ctx, m, NULL) : NULL;
    bool r = s && (strstr(s, "end of input") || strstr(s, "Unterminated") || strstr(s, "unterminated") || strstr(s, "Unexpected end"));
    js_free_utf8(g_ctx, s);
    JSValue n = js_get_str(g_ctx, e, "name");
    char *ns = js_is_string(n) ? js_to_utf8(g_ctx, n, NULL) : NULL;
    bool syntax = ns && !strcmp(ns, "SyntaxError");
    js_free_utf8(g_ctx, ns);
    return r && syntax;
}

static void repl_help(void) {
    put_str(1,
        ".help           this text\n"
        ".exit           leave (also Ctrl-D)\n"
        ".clear          forget the statement being typed\n"
        ".load FILE      run the file in this session\n"
        "_               the value of the last expression\n"
        "Up, Down        earlier lines     Ctrl-A, Ctrl-E, Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y, Ctrl-L   edit the line\n"
        "Ctrl-C          abandon the line, or stop a running program\n");
}

static void repl_run(const char *src, size_t len) {
    arm_deadline();
    g_sigint = 0;
    int mark = js_scope_open(g_ctx);
    JSValue r = js_eval(g_ctx, src, len, "<repl>", JS_EVAL_SCRIPT);
    if (r == JS_EXCEPTION) handle_pending_exception();
    else {
        print_value(r, false);
        if (r != JS_UNDEFINED) js_set_str(g_ctx, js_global_object(g_ctx), "_", r);
    }
    run_event_loop();                           /* timers and promise jobs that the line started */
    js_scope_close(g_ctx, mark);
    g_sigint = 0;
    g_exiting = false;
}

static int run_repl(void) {
    g_repl = true;
    bool tty = isatty(0);
    if (tty) {
        hist_load();
        put_str(1, "Kosuzu JavaScript. Type .help for help, Ctrl-D to leave.\n");
    }
    char *pending = NULL;
    size_t plen = 0;
    for (;;) {
        char *line = edit_line(pending ? "... " : "> ");
        if (!line) break;
        if (!pending && line[0] == '.') {
            if (!strcmp(line, ".exit") || !strcmp(line, ".quit")) { free(line); break; }
            if (!strcmp(line, ".help")) { repl_help(); free(line); continue; }
            if (!strcmp(line, ".clear")) { free(line); continue; }
            if (!strncmp(line, ".load ", 6)) {
                int fd = open(line + 6, O_RDONLY);
                size_t n;
                char *text = fd >= 0 ? kb_read_all(fd, &n) : NULL;
                if (fd >= 0) close(fd);
                if (!text) fprintf(stderr, "js: cannot read %s: %s\n", line + 6, strerror(errno));
                else { repl_run(text, n); free(text); }
                free(line);
                continue;
            }
        }
        size_t ll = strlen(line);
        if (tty) hist_add(line);
        char *joined = realloc(pending, plen + ll + 2);
        if (!joined) { free(line); break; }
        pending = joined;
        if (plen) pending[plen++] = '\n';
        memcpy(pending + plen, line, ll + 1);
        plen += ll;
        free(line);
        if (plen == 0 || pending[0] == 0) { free(pending); pending = NULL; plen = 0; continue; }
        /* compile first: a statement that is not finished asks for another line, anything else runs */
        int mark = js_scope_open(g_ctx);
        JSValue f = js_eval(g_ctx, pending, plen, "<repl>", JS_EVAL_SCRIPT | JS_EVAL_COMPILE);
        bool more = false;
        if (f == JS_EXCEPTION) {
            JSValue e = js_take_exception(g_ctx);
            if (needs_more_input(e)) more = true;
            else { report_exception(e, "Uncaught "); free(pending); pending = NULL; plen = 0; }
        }
        js_scope_close(g_ctx, mark);
        if (more) continue;
        if (f != JS_EXCEPTION) {
            repl_run(pending, plen);
            free(pending);
            pending = NULL;
            plen = 0;
        }
    }
    free(pending);
    if (tty) hist_save();
    out_flush();
    return g_status;
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *f) {
    fprintf(f,
        "usage: js [-s] [-m MB] [-t SECONDS] FILE [ARG...]\n"
        "       js [-s] [-m MB] [-t SECONDS] -e CODE [ARG...]\n"
        "       js [-s] [-m MB] [-t SECONDS] -p CODE [ARG...]\n"
        "       js [-s] [-m MB] [-t SECONDS] [-i] [-]\n"
        "Run a JavaScript program, or start an interactive session.\n"
        "  -e CODE      run CODE            -p CODE   run CODE and print the value of its last expression\n"
        "  -i           interactive session (also after a program)\n"
        "  -s           strict mode         -         read the program from standard input\n"
        "  --module     run the program as an ES module (a file whose name ends in .mjs is one already)\n"
        "  -m MB        memory limit in megabytes (default %d, 0 = none)\n"
        "  -t SECONDS   time limit (fractions allowed); the program is stopped when it runs longer\n"
        "  -h, --help   this text           --version\n", JS_DEFAULT_MEM_MB);
}

APPLET(js) {
    long mem_mb = JS_DEFAULT_MEM_MB;
    bool strict = false, interactive = false, print_result = false, from_stdin = false, module = false;
    const char *code = NULL, *file = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        if (!strcmp(a, "--version")) { puts("js (Kosuzu, ECMAScript 2024 with BigInt, RegExp, typed arrays, Proxy)"); return 0; }
        if (!strcmp(a, "-s")) { strict = true; continue; }
        if (!strcmp(a, "--module")) { module = true; continue; }
        if (!strcmp(a, "-i")) { interactive = true; continue; }
        if ((!strcmp(a, "-e") || !strcmp(a, "-p")) && i + 1 < argc) { code = argv[++i]; print_result = a[1] == 'p'; continue; }
        if (!strcmp(a, "-m") && i + 1 < argc) {
            char *end;
            mem_mb = strtol(argv[++i], &end, 10);
            if (*end || mem_mb < 0 || mem_mb > 4096) { kb_error("bad memory limit '%s'", argv[i]); return 2; }
            continue;
        }
        if (!strcmp(a, "-t") && i + 1 < argc) {
            char *end;
            double s = strtod(argv[++i], &end);
            if (*end || !(s >= 0) || s > 86400 * 30) { kb_error("bad time limit '%s'", argv[i]); return 2; }
            g_limit_ms = s * 1000.0;
            continue;
        }
        kb_error("unknown option '%s'", a);
        usage(stderr);
        return 2;
    }
    if (!code && i < argc) {
        if (!strcmp(argv[i], "-")) from_stdin = true;
        else file = argv[i];
        i++;
    }
    if (!code && !file && !from_stdin && !interactive && !isatty(0)) from_stdin = true;      /* js < program.js */
    char **args = argv + i;
    int nargs = argc - i;

    JSRuntimeOptions opts = { 0 };
    opts.memory_limit = (size_t)mem_mb << 20;
    g_rt = js_runtime_new(&opts);
    if (!g_rt) { kb_error("cannot create a runtime (out of memory)"); return 2; }
    JSHostHooks hooks = { 0 };
    hooks.now_ms = real_ms;
    hooks.tz_offset_min = tz_offset_min;
    hooks.tz_name = tz_name;
    hooks.report_error = report_job_error;
    hooks.sleep_ms = sleep_ms;
    hooks.load_module = module_load;
    hooks.import_meta = module_meta;
    js_runtime_set_host_hooks(g_rt, &hooks);
    js_runtime_set_interrupt_handler(g_rt, interrupt_cb, NULL);
    g_ctx = js_context_new(g_rt);
    if (!g_ctx) { kb_error("cannot create a context (out of memory)"); return 2; }
    js_context_set_opaque(g_ctx, NULL);
    signal(SIGINT, on_sigint);

    if (!install_prelude()) return 2;
    install_globals(nargs, args, file != NULL, file);
    int flags = strict ? JS_EVAL_STRICT : JS_EVAL_SCRIPT;
    char mname[1100];

    int status;
    if (code) {
        if (module && print_result) { kb_error("-p cannot be used with --module (a module has no value)"); return 2; }
        if (module) { abs_path("[eval]", mname, sizeof mname); flags = JS_EVAL_MODULE; }
        status = run_program(code, strlen(code), module ? mname : "[eval]", flags, print_result);
        if (interactive && !g_exiting) status = run_repl();
    } else if (file) {
        int fd = open(file, O_RDONLY);
        if (fd < 0) { kb_error("cannot open %s: %s", file, strerror(errno)); return 2; }
        size_t len;
        char *src = kb_read_all(fd, &len);
        close(fd);
        if (!src) { kb_error("cannot read %s: %s", file, strerror(errno)); return 2; }
        size_t fl = strlen(file);
        if (module || (fl > 4 && !strcmp(file + fl - 4, ".mjs"))) {
            abs_path(file, mname, sizeof mname);            /* (what an import of this file resolves to) */
            flags = JS_EVAL_MODULE;
        }
        status = run_program(src, len, flags == JS_EVAL_MODULE ? mname : file, flags, false);
        free(src);
        if (interactive && !g_exiting) status = run_repl();
    } else if (from_stdin) {
        size_t len;
        char *src = read_all_stdin(&len);
        if (!src) { kb_error("cannot read standard input"); return 2; }
        if (module) { abs_path("[stdin]", mname, sizeof mname); flags = JS_EVAL_MODULE; }
        status = run_program(src, len, module ? mname : "[stdin]", flags, false);
        free(src);
    } else {
        status = run_repl();
    }
    out_flush();
    raw_off();
    /* the runtime is not freed: the process ends here, and tearing a big heap down object by object only costs time */
    return status;
}
