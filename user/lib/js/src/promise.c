/*
 * promise.c: Promise (ECMA-262 27.2) and its jobs.
 *
 * A reaction record holds a capability and a handler, which is either a JavaScript function or one of the engine's
 * internal reaction functions (await, async generators, async-from-sync iterators: registered in a small table and
 * stored by index, so a job's arguments stay plain values).  A capability whose resolve function is undefined is an
 * engine-made %Promise% resolved directly; that is unobservable, since nobody else holds its resolving functions.
 *
 * Unhandled rejections (HostPromiseRejectionTracker) are collected while jobs run and reported once the queue is
 * empty if they are still unhandled.
 */
#include "jsint.h"

struct KzPromiseReaction {
    KzPromiseReaction *next;
    JSValue cap_promise, cap_resolve, cap_reject;   /* the capability (all undefined: none) */
    JSValue on_ok, on_err;      /* handlers (undefined: none) */
    JSValue data;               /* for internal handlers */
    int16_t native_ok, native_err;   /* internal handler indexes + 1 (0: the JS handler) */
};

enum { PS_PENDING, PS_FULFILLED, PS_REJECTED };

/* ------------------------------------------------------------------ internal reaction functions */

static KzReactionFn *reaction_tab[32];
static int nreaction_tab;

static int reaction_index(KzReactionFn *fn) {
    if (!fn) return 0;
    for (int i = 0; i < nreaction_tab; i++)
        if (reaction_tab[i] == fn) return i + 1;
    if (nreaction_tab == (int)KZ_COUNTOF(reaction_tab)) return 0;
    reaction_tab[nreaction_tab++] = fn;
    return nreaction_tab;
}

/* ------------------------------------------------------------------ the GC side */

void kz_promise_trace(JSRuntime *rt, JSObject *o) {
    kz_mark_value(rt, o->u.promise.result);
    for (KzPromiseReaction *r = o->u.promise.reactions; r; r = r->next) {
        kz_mark_value(rt, r->cap_promise);
        kz_mark_value(rt, r->cap_resolve);
        kz_mark_value(rt, r->cap_reject);
        kz_mark_value(rt, r->on_ok);
        kz_mark_value(rt, r->on_err);
        kz_mark_value(rt, r->data);
    }
}

void kz_promise_free(JSRuntime *rt, JSObject *o) {
    for (KzPromiseReaction *r = o->u.promise.reactions; r;) {
        KzPromiseReaction *n = r->next;
        kz_free(rt, r);
        r = n;
    }
    o->u.promise.reactions = NULL;
}

int kz_promise_state(JSValue p) { return JV_OBJ(p)->u.promise.state; }

static void track_rejection(JSContext *ctx, JSValue promise, bool handle);

/* the host (or the engine) deals with this promise's outcome without a reaction */
void kz_promise_mark_handled(JSContext *ctx, JSValue promise) {
    JSObject *o = JV_OBJ(promise);
    if (o->u.promise.state == PS_REJECTED && !o->u.promise.handled) track_rejection(ctx, promise, true);
    o->u.promise.handled = true;
}

/* ------------------------------------------------------------------ rejection tracking */

static void track_rejection(JSContext *ctx, JSValue promise, bool handle) {
    JSRuntime *rt = ctx->rt;
    if (handle) {
        for (uint32_t i = 0; i < rt->unhandled.len; i++)
            if (rt->unhandled.v[i] == promise) {
                rt->unhandled.v[i] = rt->unhandled.v[--rt->unhandled.len];
                return;
            }
        return;
    }
    if (kz_vec_push(ctx, &rt->unhandled, promise)) kz_take_exception(ctx);   /* out of memory: just not reported */
}

void kz_report_unhandled(JSContext *ctx, JSValue err, bool rejection);

/* reports (and forgets) rejections still unhandled; called when the job queue is empty */
void kz_report_rejections(JSRuntime *rt) {
    while (rt->unhandled.len) {
        JSValue p = rt->unhandled.v[--rt->unhandled.len];
        JSObject *o = JV_OBJ(p);
        if (o->u.promise.handled) continue;
        JSContext *ctx = rt->contexts;
        if (!ctx) return;
        KZ_ROOT(ctx, rp);
        rp = p;
        kz_report_unhandled(ctx, JV_OBJ(rp)->u.promise.result, true);
    }
}

/* ------------------------------------------------------------------ settling */

static JSValue promise_reaction_job(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);

static void trigger(JSContext *ctx, KzPromiseReaction *list, JSValue arg, bool rejected) {
    /* the list is newest first: reverse it, then queue one job per reaction */
    KzPromiseReaction *rev = NULL;
    while (list) {
        KzPromiseReaction *n = list->next;
        list->next = rev;
        rev = list;
        list = n;
    }
    KZ_ROOT(ctx, ra);
    ra = arg;
    while (rev) {
        KzPromiseReaction *r = rev;
        rev = r->next;
        int native = rejected ? r->native_err : r->native_ok;
        JSValue args[7] = { JV_MKINT((rejected ? 1 : 0) | (native << 1)), rejected ? r->on_err : r->on_ok, r->cap_promise,
                            r->cap_resolve, r->cap_reject, ra, r->data };
        if (kz_enqueue_job(ctx, promise_reaction_job, 7, args)) kz_take_exception(ctx);
        kz_free(ctx->rt, r);
    }
}

static void settle(JSContext *ctx, JSValue promise, JSValue v, int state) {
    JSObject *o = JV_OBJ(promise);
    if (o->u.promise.state != PS_PENDING) return;
    KzPromiseReaction *list = o->u.promise.reactions;
    o->u.promise.reactions = NULL;
    o->u.promise.result = v;
    o->u.promise.state = (uint8_t)state;
    KZ_ROOT(ctx, rp);
    rp = promise;
    trigger(ctx, list, v, state == PS_REJECTED);      /* (queues the jobs without allocating from the heap) */
    if (state == PS_REJECTED && !JV_OBJ(rp)->u.promise.handled) track_rejection(ctx, rp, false);
}

void kz_promise_reject_value(JSContext *ctx, JSValue promise, JSValue v) { settle(ctx, promise, v, PS_REJECTED); }

static JSValue resolve_thenable_job(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);

/* the body of a promise resolve function (after the already-resolved check) */
void kz_promise_resolve_value(JSContext *ctx, JSValue promise, JSValue res) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = promise;
    r[1] = res;
    if (res == promise) {
        r[2] = kz_new_error_msg(ctx, KZ_ERR_TYPE, "Chaining cycle detected for promise");
        if (r[2] == JS_EXCEPTION) r[2] = kz_take_exception(ctx);
        settle(ctx, r[0], r[2], PS_REJECTED);
        return;
    }
    if (!JV_IS_OBJECT(res)) { settle(ctx, r[0], res, PS_FULFILLED); return; }
    r[2] = kz_get(ctx, res, ctx->rt->atom[KZ_ATOM_then], res);
    if (r[2] == JS_EXCEPTION) {
        if (ctx->rt->exc_uncatchable) return;
        r[2] = kz_take_exception(ctx);
        settle(ctx, r[0], r[2], PS_REJECTED);
        return;
    }
    if (!kz_is_callable(r[2])) { settle(ctx, r[0], r[1], PS_FULFILLED); return; }
    JSValue args[3] = { r[0], r[1], r[2] };
    if (kz_enqueue_job(ctx, resolve_thenable_job, 3, args)) kz_take_exception(ctx);
}

/* ------------------------------------------------------------------ resolving functions */

/* data: [promise, the shared already-resolved cell] */
static JSValue resolve_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    JSVarCell *done = JV_PTR(kz_native_data(ctx, 1));
    if (done->value == JS_TRUE) return JS_UNDEFINED;
    done->value = JS_TRUE;
    JSValue p = kz_native_data(ctx, 0);
    if (magic) settle(ctx, p, argv[0], PS_REJECTED);
    else kz_promise_resolve_value(ctx, p, argv[0]);
    if (ctx->rt->exc_uncatchable && kz_has_exception(ctx)) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static int make_resolving_functions(JSContext *ctx, JSValue promise, JSValue *resolve, JSValue *reject) {
    KZ_ROOTS(ctx, r, 3);
    JSVarCell *cell = kz_new_cell(ctx, JS_FALSE);
    if (!cell) return -1;
    r[0] = JV_MKPTR(cell);
    JSValue data[2] = { promise, r[0] };
    r[1] = kz_new_native(ctx, resolve_fn, ctx->rt->atom[KZ_ATOM_empty], 1, 0, 2, data);
    if (r[1] == JS_EXCEPTION) return -1;
    r[2] = kz_new_native(ctx, resolve_fn, ctx->rt->atom[KZ_ATOM_empty], 1, 1, 2, data);
    if (r[2] == JS_EXCEPTION) return -1;
    *resolve = r[1];
    *reject = r[2];
    return 0;
}

static JSValue new_promise_proto(JSContext *ctx, JSValue proto) {
    JSValue p = kz_new_object_class(ctx, KZ_CLASS_PROMISE, proto);
    if (p == JS_EXCEPTION) return p;
    JSObject *o = JV_OBJ(p);
    o->u.promise.reactions = NULL;
    o->u.promise.result = JS_UNDEFINED;
    o->u.promise.state = PS_PENDING;
    o->u.promise.handled = false;
    return p;
}

JSValue kz_new_promise_intrinsic(JSContext *ctx) { return new_promise_proto(ctx, kz_intr(ctx, KZ_I_PROMISE_PROTO)); }

JSValue kz_promise_new(JSContext *ctx, JSValue *resolve, JSValue *reject) {
    KZ_ROOT(ctx, p);
    p = kz_new_promise_intrinsic(ctx);
    if (p == JS_EXCEPTION) return p;
    if (resolve && make_resolving_functions(ctx, p, resolve, reject)) return JS_EXCEPTION;
    return p;
}

/* ------------------------------------------------------------------ capabilities */

typedef struct Cap { JSValue promise, resolve, reject; } Cap;   /* callers root the three values */

static JSValue cap_executor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSVarCell *res = JV_PTR(kz_native_data(ctx, 0)), *rej = JV_PTR(kz_native_data(ctx, 1));
    if (res->value != JS_UNDEFINED) return kz_throw_type(ctx, "Promise executor has already been invoked with non-undefined arguments");
    if (rej->value != JS_UNDEFINED) return kz_throw_type(ctx, "Promise executor has already been invoked with non-undefined arguments");
    res->value = argv[0];
    rej->value = argv[1];
    return JS_UNDEFINED;
}

static bool is_intrinsic_promise_ctor(JSContext *ctx, JSValue c) {
    if (!JV_IS_OBJECT(c)) return false;
    JSContext *realm = kz_function_realm(ctx, JV_OBJ(c));
    return realm && c == kz_intr(realm, KZ_I_PROMISE);
}

/* NewPromiseCapability(C); with functions false an intrinsic C gives an internal capability (no functions) */
static int new_capability(JSContext *ctx, JSValue c, Cap *cap, bool functions) {
    cap->promise = cap->resolve = cap->reject = JS_UNDEFINED;
    if (is_intrinsic_promise_ctor(ctx, c)) {
        JSContext *realm = kz_function_realm(ctx, JV_OBJ(c));
        cap->promise = new_promise_proto(ctx, kz_intr(realm, KZ_I_PROMISE_PROTO));
        if (cap->promise == JS_EXCEPTION) return -1;
        if (functions && make_resolving_functions(ctx, cap->promise, &cap->resolve, &cap->reject)) return -1;
        return 0;
    }
    if (!kz_is_constructor(c)) { kz_throw_type(ctx, "Promise resolver is not a constructor"); return -1; }
    KZ_ROOTS(ctx, r, 3);
    JSVarCell *a = kz_new_cell(ctx, JS_UNDEFINED);
    if (!a) return -1;
    r[0] = JV_MKPTR(a);
    JSVarCell *b = kz_new_cell(ctx, JS_UNDEFINED);
    if (!b) return -1;
    r[1] = JV_MKPTR(b);
    JSValue data[2] = { r[0], r[1] };
    r[2] = kz_new_native(ctx, cap_executor, ctx->rt->atom[KZ_ATOM_empty], 2, 0, 2, data);
    if (r[2] == JS_EXCEPTION) return -1;
    JSValue p = kz_construct(ctx, c, 1, &r[2], c);
    if (p == JS_EXCEPTION) return -1;
    cap->promise = p;
    a = JV_PTR(r[0]);
    b = JV_PTR(r[1]);
    if (!kz_is_callable(a->value)) { kz_throw_type(ctx, "Promise resolve function is not callable"); return -1; }
    if (!kz_is_callable(b->value)) { kz_throw_type(ctx, "Promise reject function is not callable"); return -1; }
    cap->resolve = a->value;
    cap->reject = b->value;
    return 0;
}

static int cap_settle(JSContext *ctx, Cap *cap, JSValue v, bool reject) {
    if (cap->resolve == JS_UNDEFINED) {
        if (reject) settle(ctx, cap->promise, v, PS_REJECTED);
        else kz_promise_resolve_value(ctx, cap->promise, v);
        return kz_has_exception(ctx) && ctx->rt->exc_uncatchable ? -1 : 0;
    }
    JSValue args[1] = { v };
    return kz_call(ctx, reject ? cap->reject : cap->resolve, JS_UNDEFINED, 1, args) == JS_EXCEPTION ? -1 : 0;
}

/* IfAbruptRejectPromise: rejects the capability with the pending exception and returns its promise */
static JSValue reject_with_pending(JSContext *ctx, Cap *cap) {
    if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
    KZ_ROOT(ctx, e);
    e = kz_take_exception(ctx);
    if (cap_settle(ctx, cap, e, true)) return JS_EXCEPTION;
    return cap->promise;
}

/* ------------------------------------------------------------------ then */

static int add_reaction(JSContext *ctx, JSValue promise, JSValue on_ok, JSValue on_err, int native_ok, int native_err,
                        JSValue data, Cap *cap) {
    JSObject *o = JV_OBJ(promise);
    JSValue okh = kz_is_callable(on_ok) ? on_ok : JS_UNDEFINED, errh = kz_is_callable(on_err) ? on_err : JS_UNDEFINED;
    if (o->u.promise.state == PS_PENDING) {
        KzPromiseReaction *r = kz_malloc(ctx, sizeof(KzPromiseReaction));
        if (!r) return -1;
        o = JV_OBJ(promise);
        r->cap_promise = cap ? cap->promise : JS_UNDEFINED;
        r->cap_resolve = cap ? cap->resolve : JS_UNDEFINED;
        r->cap_reject = cap ? cap->reject : JS_UNDEFINED;
        r->on_ok = okh;
        r->on_err = errh;
        r->data = data;
        r->native_ok = (int16_t)native_ok;
        r->native_err = (int16_t)native_err;
        r->next = o->u.promise.reactions;
        o->u.promise.reactions = r;
    } else {
        bool rejected = o->u.promise.state == PS_REJECTED;
        if (rejected && !o->u.promise.handled) track_rejection(ctx, promise, true);
        int native = rejected ? native_err : native_ok;
        JSValue args[7] = { JV_MKINT((rejected ? 1 : 0) | (native << 1)), rejected ? errh : okh,
                            cap ? cap->promise : JS_UNDEFINED, cap ? cap->resolve : JS_UNDEFINED,
                            cap ? cap->reject : JS_UNDEFINED, o->u.promise.result, data };
        if (kz_enqueue_job(ctx, promise_reaction_job, 7, args)) return -1;
    }
    JV_OBJ(promise)->u.promise.handled = true;
    return 0;
}

/* an engine-internal then: ok/err (NULL: pass the value or reason through) get data and the value; their result
   settles `result` (an engine-made promise, or undefined for none) */
int kz_promise_then_internal(JSContext *ctx, JSValue promise, KzReactionFn *ok, KzReactionFn *err, JSValue data,
                             JSValue result) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = promise;
    r[1] = data;
    r[2] = result;
    Cap cap = { result, JS_UNDEFINED, JS_UNDEFINED };
    return add_reaction(ctx, r[0], JS_UNDEFINED, JS_UNDEFINED, reaction_index(ok), reaction_index(err), r[1],
                        result == JS_UNDEFINED ? NULL : &cap);
}

/* PromiseReactionJob: args [kind, handler, cap promise, cap resolve, cap reject, argument, data] */
static JSValue promise_reaction_job(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    int kind = JV_INT(argv[0]);
    bool rejected = kind & 1;
    int native = kind >> 1;
    JSValue handler = argv[1], arg = argv[5];
    KZ_ROOT(ctx, res);
    bool threw = false;
    if (native) {
        res = reaction_tab[native - 1](ctx, argv[6], arg);
        if (res == JS_EXCEPTION) {
            if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
            res = kz_take_exception(ctx);
            threw = true;
        }
    } else if (handler == JS_UNDEFINED) {
        res = arg;
        threw = rejected;
    } else {
        JSValue args[1] = { arg };
        res = kz_call(ctx, handler, JS_UNDEFINED, 1, args);
        if (res == JS_EXCEPTION) {
            if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
            res = kz_take_exception(ctx);
            threw = true;
        }
    }
    if (argv[2] == JS_UNDEFINED) {
        if (threw) return kz_throw(ctx, res);    /* (an internal handler failed: report it) */
        return JS_UNDEFINED;
    }
    Cap cap = { argv[2], argv[3], argv[4] };
    if (cap_settle(ctx, &cap, res, threw)) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* NewPromiseResolveThenableJob: args [promise, thenable, then] */
static JSValue resolve_thenable_job(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    if (make_resolving_functions(ctx, argv[0], &r[0], &r[1])) return JS_EXCEPTION;
    JSValue args[2] = { r[0], r[1] };
    JSValue res = kz_call(ctx, argv[2], argv[1], 2, args);
    if (res == JS_EXCEPTION) {
        if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
        JSValue e = kz_take_exception(ctx);
        JSValue a[1] = { e };
        return kz_call(ctx, r[1], JS_UNDEFINED, 1, a) == JS_EXCEPTION ? JS_EXCEPTION : JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* PromiseResolve(C, x) */
JSValue kz_promise_resolve(JSContext *ctx, JSValue c, JSValue x) {
    KZ_ROOTS(ctx, r, 4);
    r[0] = c;
    r[1] = x;
    if (kz_is_class(x, KZ_CLASS_PROMISE)) {
        r[2] = kz_get(ctx, x, ctx->rt->atom[KZ_ATOM_constructor], x);
        if (r[2] == JS_EXCEPTION) return r[2];
        if (r[2] == c) return x;
    }
    Cap cap;
    KzRootFrame rf = { ctx->rt->roots, ctx->rt, &cap.promise, 3, NULL };
    ctx->rt->roots = &rf;
    int rc = new_capability(ctx, r[0], &cap, false);
    JSValue out = JS_EXCEPTION;
    if (!rc && !cap_settle(ctx, &cap, r[1], false)) out = cap.promise;
    ctx->rt->roots = rf.prev;
    return out;
}

/* ================================================================== the constructor and statics */

static JSValue promise_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return kz_throw_type(ctx, "Promise constructor cannot be invoked without 'new'");
    if (!kz_is_callable(argv[0])) return kz_throw_type(ctx, "Promise resolver is not a function");
    KZ_ROOTS(ctx, r, 4);
    if (kz_get_proto_from_ctor(ctx, rt->new_target, KZ_I_PROMISE_PROTO, &r[0])) return JS_EXCEPTION;
    r[1] = new_promise_proto(ctx, r[0]);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (make_resolving_functions(ctx, r[1], &r[2], &r[3])) return JS_EXCEPTION;
    JSValue args[2] = { r[2], r[3] };
    JSValue res = kz_call(ctx, argv[0], JS_UNDEFINED, 2, args);
    if (res == JS_EXCEPTION) {
        if (rt->exc_uncatchable) return JS_EXCEPTION;
        JSValue e = kz_take_exception(ctx);
        JSValue a[1] = { e };
        if (kz_call(ctx, r[3], JS_UNDEFINED, 1, a) == JS_EXCEPTION) return JS_EXCEPTION;
    }
    return r[1];
}

#define ROOT_CAP(ctx, cap)                                                                             \
    Cap cap = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };                                            \
    KzRootFrame cap##_rf __attribute__((cleanup(kz_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, &cap.promise, 3, NULL }; \
    (ctx)->rt->roots = &cap##_rf

/* resolve (0), reject (1) */
static JSValue promise_resolve_static(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "PromiseResolve called on non-object");
    if (magic == 0) return kz_promise_resolve(ctx, this_val, argv[0]);
    ROOT_CAP(ctx, cap);
    if (new_capability(ctx, this_val, &cap, false)) return JS_EXCEPTION;
    if (cap_settle(ctx, &cap, argv[0], true)) return JS_EXCEPTION;
    return cap.promise;
}

static JSValue promise_with_resolvers(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    ROOT_CAP(ctx, cap);
    if (new_capability(ctx, this_val, &cap, true)) return JS_EXCEPTION;
    KZ_ROOT(ctx, o);
    o = kz_new_object(ctx);
    if (o == JS_EXCEPTION) return o;
    JSRuntime *rt = ctx->rt;
    if (kz_create_data_property(ctx, o, rt->atom[KZ_ATOM_promise], cap.promise) ||
        kz_create_data_property(ctx, o, rt->atom[KZ_ATOM_resolve], cap.resolve) ||
        kz_create_data_property(ctx, o, rt->atom[KZ_ATOM_reject], cap.reject))
        return JS_EXCEPTION;
    return o;
}

/* Promise.try (ECMA-262 2025, 27.2.4.8): calls the function at once with the rest of the arguments and settles a new promise
   (made by `this`, as the other statics do) with its result or the value it threw */
static JSValue promise_try(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Promise.try called on non-object");
    ROOT_CAP(ctx, cap);
    if (new_capability(ctx, this_val, &cap, false)) return JS_EXCEPTION;
    KZ_ROOT(ctx, rv);
    bool threw = false;
    JSValue res = kz_call(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, JS_UNDEFINED, argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL);
    if (res == JS_EXCEPTION) {
        if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
        rv = kz_take_exception(ctx);
        threw = true;
    } else rv = res;
    if (cap_settle(ctx, &cap, rv, threw)) return JS_EXCEPTION;
    return cap.promise;
}

/*
 * The element functions of Promise.all (kind 0), allSettled (1 fulfilled, 2 rejected) and any (3).
 * data: [values array, remaining cell, capability promise, resolve, reject, index, already-called cell]
 */
static JSValue element_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    JSRuntime *rt = ctx->rt;
    JSVarCell *called = JV_PTR(kz_native_data(ctx, 6));
    if (called->value == JS_TRUE) return JS_UNDEFINED;
    called->value = JS_TRUE;
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_native_data(ctx, 0);
    JSVarCell *remaining = JV_PTR(kz_native_data(ctx, 1));
    Cap cap = { kz_native_data(ctx, 2), kz_native_data(ctx, 3), kz_native_data(ctx, 4) };
    r[1] = cap.promise; r[2] = cap.resolve; r[3] = cap.reject;
    uint32_t index = (uint32_t)JV_INT(kz_native_data(ctx, 5));
    JSValue v = argv[0];
    if (magic == 1 || magic == 2) {
        KZ_ROOT(ctx, o);
        o = kz_new_object(ctx);
        if (o == JS_EXCEPTION) return o;
        JSValue status = kz_atom(ctx, magic == 1 ? "fulfilled" : "rejected");
        if (status == JS_EXCEPTION || kz_create_data_property(ctx, o, rt->atom[KZ_ATOM_status], status)) return JS_EXCEPTION;
        if (kz_create_data_property(ctx, o, rt->atom[magic == 1 ? KZ_ATOM_value : KZ_ATOM_reason], v)) return JS_EXCEPTION;
        v = o;
        if (kz_create_data_property(ctx, r[0], KZ_KEY_FROM_INDEX(index), v)) return JS_EXCEPTION;
    } else if (kz_create_data_property(ctx, r[0], KZ_KEY_FROM_INDEX(index), v)) return JS_EXCEPTION;
    remaining = JV_PTR(kz_native_data(ctx, 1));
    remaining->value = JV_MKINT(JV_INT(remaining->value) - 1);
    if (JV_INT(remaining->value) == 0) {
        Cap c2 = { r[1], r[2], r[3] };
        if (magic == 3) {
            KZ_ROOT(ctx, e);
            e = kz_new_error_msg(ctx, KZ_ERR_AGGREGATE, "All promises were rejected");
            if (e == JS_EXCEPTION) return e;
            if (kz_define_value_atom(ctx, e, KZ_ATOM_errors, r[0], KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
            return cap_settle(ctx, &c2, e, true) ? JS_EXCEPTION : JS_UNDEFINED;
        }
        return cap_settle(ctx, &c2, r[0], false) ? JS_EXCEPTION : JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* Promise.all (0), allSettled (1), any (2), race (3) */
static JSValue promise_combinator(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    JSRuntime *rt = ctx->rt;
    ROOT_CAP(ctx, cap);
    if (new_capability(ctx, this_val, &cap, true)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 8);
    r[0] = kz_get(ctx, this_val, rt->atom[KZ_ATOM_resolve], this_val);
    if (r[0] == JS_EXCEPTION) return reject_with_pending(ctx, &cap);
    if (!kz_is_callable(r[0])) {
        kz_throw_type(ctx, "Promise resolve is not a function");
        return reject_with_pending(ctx, &cap);
    }
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, argv[0], &it, false)) return reject_with_pending(ctx, &cap);
    /* the values (or errors) list and the remaining count */
    r[1] = kz_new_array(ctx, 0);
    if (r[1] == JS_EXCEPTION) return reject_with_pending(ctx, &cap);
    JSVarCell *remaining = kz_new_cell(ctx, JV_MKINT(1));
    if (!remaining) return reject_with_pending(ctx, &cap);
    r[2] = JV_MKPTR(remaining);
    for (uint32_t index = 0;; index++) {
        int s = kz_iter_step(ctx, &it, &r[3]);
        if (s < 0) return reject_with_pending(ctx, &cap);   /* (the iterator is done: no close) */
        if (!s) {
            if (magic == 3) return cap.promise;
            remaining = JV_PTR(r[2]);
            remaining->value = JV_MKINT(JV_INT(remaining->value) - 1);
            if (JV_INT(remaining->value) == 0) {
                if (magic == 2) {
                    r[4] = kz_new_error_msg(ctx, KZ_ERR_AGGREGATE, "All promises were rejected");
                    if (r[4] == JS_EXCEPTION) return reject_with_pending(ctx, &cap);
                    if (kz_define_value_atom(ctx, r[4], KZ_ATOM_errors, r[1], KZ_PROP_W | KZ_PROP_C)) return reject_with_pending(ctx, &cap);
                    if (cap_settle(ctx, &cap, r[4], true)) return JS_EXCEPTION;
                } else if (cap_settle(ctx, &cap, r[1], false)) return JS_EXCEPTION;
            }
            return cap.promise;
        }
        if (magic != 3 && kz_create_data_property(ctx, r[1], KZ_KEY_FROM_INDEX(index), JS_UNDEFINED)) goto close;
        {
            JSValue a[1] = { r[3] };
            r[4] = kz_call(ctx, r[0], this_val, 1, a);
        }
        if (r[4] == JS_EXCEPTION) goto close;
        if (magic == 3) {
            JSValue a[2] = { cap.resolve, cap.reject };
            if (kz_invoke(ctx, r[4], rt->atom[KZ_ATOM_then], 2, a) == JS_EXCEPTION) goto close;
            continue;
        }
        /* the element function(s) */
        JSVarCell *called = kz_new_cell(ctx, JS_FALSE);
        if (!called) goto close;
        r[5] = JV_MKPTR(called);
        JSValue data[7] = { r[1], r[2], cap.promise, cap.resolve, cap.reject, JV_MKINT((int32_t)index), r[5] };
        static const int kinds[3][2] = { { 0, -1 }, { 1, 2 }, { -1, 3 } };
        r[6] = JS_UNDEFINED;
        r[7] = JS_UNDEFINED;
        if (kinds[magic][0] >= 0) {
            r[6] = kz_new_native(ctx, element_fn, rt->atom[KZ_ATOM_empty], 1, kinds[magic][0], 7, data);
            if (r[6] == JS_EXCEPTION) goto close;
        }
        if (kinds[magic][1] >= 0) {
            r[7] = kz_new_native(ctx, element_fn, rt->atom[KZ_ATOM_empty], 1, kinds[magic][1], 7, data);
            if (r[7] == JS_EXCEPTION) goto close;
        }
        remaining = JV_PTR(r[2]);
        remaining->value = JV_MKINT(JV_INT(remaining->value) + 1);
        JSValue a[2] = { magic == 2 ? cap.resolve : r[6], magic == 0 ? cap.reject : r[7] };
        if (kz_invoke(ctx, r[4], rt->atom[KZ_ATOM_then], 2, a) == JS_EXCEPTION) goto close;
    }
close:
    if (rt->exc_uncatchable) return JS_EXCEPTION;
    kz_iter_close_on_throw(ctx, &it);
    return reject_with_pending(ctx, &cap);
}

/* ================================================================== Promise.prototype */

static JSValue promise_then(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!kz_is_class(this_val, KZ_CLASS_PROMISE)) return kz_throw_type(ctx, "Promise.prototype.then called on an incompatible receiver");
    KZ_ROOT(ctx, c);
    c = kz_species_constructor(ctx, this_val, kz_intr(ctx, KZ_I_PROMISE));
    if (c == JS_EXCEPTION) return c;
    ROOT_CAP(ctx, cap);
    if (new_capability(ctx, c, &cap, false)) return JS_EXCEPTION;
    if (add_reaction(ctx, this_val, argv[0], argv[1], 0, 0, JS_UNDEFINED, &cap)) return JS_EXCEPTION;
    return cap.promise;
}

static JSValue promise_catch(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSValue a[2] = { JS_UNDEFINED, argv[0] };
    return kz_invoke(ctx, this_val, ctx->rt->atom[KZ_ATOM_then], 2, a);
}

/* finally's pieces: magic 0 thenFinally / 1 catchFinally (data [onFinally, C]); 2 value thunk / 3 thrower (data [v]) */
static JSValue finally_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc;
    if (magic == 2) return kz_native_data(ctx, 0);
    if (magic == 3) return kz_throw(ctx, kz_native_data(ctx, 0));
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_native_data(ctx, 0);
    r[1] = kz_native_data(ctx, 1);
    r[3] = argv[0];
    r[2] = kz_call(ctx, r[0], JS_UNDEFINED, 0, NULL);
    if (r[2] == JS_EXCEPTION) return r[2];
    r[2] = kz_promise_resolve(ctx, r[1], r[2]);
    if (r[2] == JS_EXCEPTION) return r[2];
    JSValue data[1] = { r[3] };
    r[0] = kz_new_native(ctx, finally_fn, ctx->rt->atom[KZ_ATOM_empty], 0, magic == 0 ? 2 : 3, 1, data);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSValue a[1] = { r[0] };
    return kz_invoke(ctx, r[2], ctx->rt->atom[KZ_ATOM_then], 1, a);
}

static JSValue promise_finally(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Promise.prototype.finally called on a non-object");
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_species_constructor(ctx, this_val, kz_intr(ctx, KZ_I_PROMISE));
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!kz_is_callable(argv[0])) {
        JSValue a[2] = { argv[0], argv[0] };
        return kz_invoke(ctx, this_val, ctx->rt->atom[KZ_ATOM_then], 2, a);
    }
    JSValue data[2] = { argv[0], r[0] };
    r[1] = kz_new_native(ctx, finally_fn, ctx->rt->atom[KZ_ATOM_empty], 1, 0, 2, data);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = kz_new_native(ctx, finally_fn, ctx->rt->atom[KZ_ATOM_empty], 1, 1, 2, data);
    if (r[2] == JS_EXCEPTION) return r[2];
    JSValue a[2] = { r[1], r[2] };
    return kz_invoke(ctx, this_val, ctx->rt->atom[KZ_ATOM_then], 2, a);
}

static const KzFuncDef promise_funcs[] = {
    KZ_FN_MAGIC("all", promise_combinator, 1, 0),
    KZ_FN_MAGIC("allSettled", promise_combinator, 1, 1),
    KZ_FN_MAGIC("any", promise_combinator, 1, 2),
    KZ_FN_MAGIC("race", promise_combinator, 1, 3),
    KZ_FN_MAGIC("reject", promise_resolve_static, 1, 1),
    KZ_FN_MAGIC("resolve", promise_resolve_static, 1, 0),
    KZ_FN("try", promise_try, 1),
    KZ_FN("withResolvers", promise_with_resolvers, 0),
    KZ_GETTER("[Symbol.species]", kz_native_return_this),
};

static const KzFuncDef promise_proto_funcs[] = {
    KZ_FN("catch", promise_catch, 1),
    KZ_FN("finally", promise_finally, 1),
};

int kz_init_promise(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_PROMISE_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, promise_ctor, "Promise", 1, 0, KZ_I_PROMISE_PROTO, KZ_I_PROMISE);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], promise_funcs, KZ_COUNTOF(promise_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], promise_proto_funcs, KZ_COUNTOF(promise_proto_funcs))) return -1;
    r[2] = kz_new_native_c(ctx, promise_then, "then", 2, 0);
    if (r[2] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_PROMISE_THEN] = r[2];
    if (kz_define_value_atom(ctx, r[0], KZ_ATOM_then, r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    JSValue tag = kz_atom(ctx, "Promise");
    if (tag == JS_EXCEPTION || kz_define_value(ctx, r[0], ctx->rt->sym[KZ_SYM_toStringTag], tag, KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Promise, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
