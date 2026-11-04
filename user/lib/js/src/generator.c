/*
 * generator.c: generators (ECMA-262 27.5), async functions (27.7), async generators (27.6) and the
 * async-from-sync iterator (27.1.4).
 *
 * Each runs its body in a heap frame (kz_new_gen_frame).  The frame suspends by returning JS_EMPTY from kz_run with
 * rt->suspend_kind saying why: the initial yield after the parameters are bound, a yield (the value in
 * rt->suspend_value), a raw yield (yield*: an inner result object passed through as it is), or an await.  The
 * resumption protocol: after a yield, push the received value and the mode (RESUME_NEXT/THROW/RETURN; the code at
 * the yield sorts it out); after an await, push the value, or throw the reason at the suspension point.
 */
#include "jsint.h"
#include "bytecode.h"

enum { GS_START, GS_YIELD, GS_EXEC, GS_DONE, GS_AWAIT_RETURN };
enum { GK_SYNC, GK_ASYNC_FN, GK_ASYNC_GEN };
enum { RUN_START, RUN_VALUE, RUN_THROW, RUN_YIELD };

typedef struct KzGenReq {
    struct KzGenReq *next;
    JSValue value, promise;
    int mode;
} KzGenReq;

struct KzGenerator {
    JSFrame *frame;             /* NULL once finished */
    uint8_t state, kind;
    JSValue promise;            /* an async function's result promise */
    KzGenReq *queue, *tail;     /* an async generator's pending requests */
};

static KzGenerator *gen_of(JSValue v) { return JV_OBJ(v)->u.gen; }

void kz_generator_trace(JSRuntime *rt, JSObject *o) {
    KzGenerator *g = o->u.gen;
    if (!g) return;
    if (g->frame) kz_trace_frame(rt, g->frame);
    kz_mark_value(rt, g->promise);
    for (KzGenReq *q = g->queue; q; q = q->next) {
        kz_mark_value(rt, q->value);
        kz_mark_value(rt, q->promise);
    }
}

void kz_generator_free(JSRuntime *rt, JSObject *o) {
    KzGenerator *g = o->u.gen;
    if (!g) return;
    if (g->frame) kz_free_frame(rt, g->frame);
    for (KzGenReq *q = g->queue; q;) {
        KzGenReq *n = q->next;
        kz_free(rt, q);
        q = n;
    }
    kz_free(rt, g);
    o->u.gen = NULL;
}

static void finish(JSContext *ctx, KzGenerator *g) {
    if (g->frame) kz_free_frame(ctx->rt, g->frame);
    g->frame = NULL;
    g->state = GS_DONE;
}

/* a generator-ish object with its state and frame; NULL + exception */
static JSValue new_gen_object(JSContext *ctx, KzClassId cls, JSValue proto, JSObject *func, JSValue this_val, int argc,
                              JSValue *argv, int kind) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = JV_MKOBJ(func);
    r[1] = kz_new_object_class(ctx, cls, proto);
    if (r[1] == JS_EXCEPTION) return r[1];
    KzGenerator *g = kz_mallocz(ctx, sizeof(KzGenerator));
    if (!g) return JS_EXCEPTION;
    g->promise = JS_UNDEFINED;
    g->kind = (uint8_t)kind;
    g->state = GS_EXEC;
    JV_OBJ(r[1])->u.gen = g;
    JSFrame *f = kz_new_gen_frame(ctx, JV_OBJ(r[0]), this_val, argc, argv);
    if (!f) return JS_EXCEPTION;
    gen_of(r[1])->frame = f;     /* attached before anything else allocates */
    return r[1];
}

static JSValue gen_prototype(JSContext *ctx, JSObject *func, KzIntrinsic fallback) {
    JSValue p = kz_get(ctx, JV_MKOBJ(func), ctx->rt->atom[KZ_ATOM_prototype], JV_MKOBJ(func));
    if (p == JS_EXCEPTION || JV_IS_OBJECT(p)) return p;
    JSContext *realm = kz_function_realm(ctx, func);
    return kz_intr(realm ? realm : ctx, fallback);
}

/* ================================================================== generators */

JSValue kz_generator_call(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv) {
    JSFunctionBytecode *b = func->u.func.b;
    bool async = b->h.flags & BC_ASYNC;
    KZ_ROOTS(ctx, r, 3);
    r[0] = JV_MKOBJ(func);
    r[1] = gen_prototype(ctx, func, async ? KZ_I_ASYNC_GENERATOR_PROTO : KZ_I_GENERATOR_PROTO);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = new_gen_object(ctx, async ? KZ_CLASS_ASYNC_GENERATOR : KZ_CLASS_GENERATOR, r[1], JV_OBJ(r[0]), this_val, argc,
                          argv, async ? GK_ASYNC_GEN : GK_SYNC);
    if (r[2] == JS_EXCEPTION) return r[2];
    /* bind the parameters (their initializers run now); the body waits at the initial yield */
    JSValue res = kz_run(ctx, gen_of(r[2])->frame);
    KzGenerator *g = gen_of(r[2]);
    if (res != JS_EMPTY) {
        finish(ctx, g);
        if (res == JS_EXCEPTION) return res;
        return r[2];
    }
    g->state = GS_START;
    return r[2];
}

/* GeneratorResume / GeneratorResumeAbrupt */
static JSValue gen_resume(JSContext *ctx, JSValue gobj, JSValue v, int mode) {
    JSRuntime *rt = ctx->rt;
    KzGenerator *g = gen_of(gobj);
    if (g->state == GS_EXEC) return kz_throw_type(ctx, "Generator is already running");
    if (g->state == GS_START && mode != RESUME_NEXT) finish(ctx, g);
    if (g->state == GS_DONE) {
        if (mode == RESUME_THROW) return kz_throw(ctx, v);
        return kz_create_iter_result(ctx, mode == RESUME_RETURN ? v : JS_UNDEFINED, true);
    }
    KZ_ROOT(ctx, rg);
    rg = gobj;
    JSFrame *f = g->frame;
    if (g->state == GS_YIELD) {
        *f->sp++ = v;
        *f->sp++ = JV_MKINT(mode);
    }
    g->state = GS_EXEC;
    JSValue res = kz_run(ctx, f);
    g = gen_of(rg);
    if (res == JS_EMPTY) {
        g->state = GS_YIELD;
        JSValue y = rt->suspend_value;
        rt->suspend_value = JS_UNDEFINED;
        if (rt->suspend_kind == KZ_SUSPEND_YIELD_RAW) return y;
        return kz_create_iter_result(ctx, y, false);
    }
    finish(ctx, g);
    if (res == JS_EXCEPTION) return res;
    return kz_create_iter_result(ctx, res, true);
}

JSValue kz_generator_resume(JSContext *ctx, JSObject *gen, JSValue v, int mode, bool *done) {
    (void)done;
    return gen_resume(ctx, JV_MKOBJ(gen), v, mode);
}

/* next (RESUME_NEXT), return (RESUME_RETURN), throw (RESUME_THROW) */
static JSValue generator_method(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    static const char *const names[] = { "next", "throw", "return" };
    if (!kz_is_class(this_val, KZ_CLASS_GENERATOR) || !gen_of(this_val))
        return kz_throw_type(ctx, "Generator.prototype.%s called on an incompatible receiver", names[magic]);
    return gen_resume(ctx, this_val, argv[0], magic);
}

/* ================================================================== the async driver */

static JSValue await_ok(JSContext *ctx, JSValue gobj, JSValue v);
static JSValue await_err(JSContext *ctx, JSValue gobj, JSValue v);
static void complete_step(JSContext *ctx, JSValue gobj, JSValue v, bool is_throw, bool done);
static void drain_queue(JSContext *ctx, JSValue gobj);

/* runs the frame from a resumption until it awaits, yields or finishes; -1 only for an uncatchable error */
static int drive(JSContext *ctx, JSValue gobj, int how, JSValue v, int mode) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = gobj;
    r[1] = v;
    for (;;) {
        KzGenerator *g = gen_of(r[0]);
        JSFrame *f = g->frame;
        switch (how) {
        case RUN_VALUE: *f->sp++ = r[1]; break;
        case RUN_THROW: kz_throw(ctx, r[1]); f->flags |= FRAME_RESUME_THROW; break;
        case RUN_YIELD: *f->sp++ = r[1]; *f->sp++ = JV_MKINT(mode); break;
        default: break;
        }
        g->state = GS_EXEC;
        JSValue res = kz_run(ctx, f);
        g = gen_of(r[0]);
        if (res == JS_EMPTY) {
            int kind = rt->suspend_kind;
            r[1] = rt->suspend_value;
            rt->suspend_value = JS_UNDEFINED;
            if (kind == KZ_SUSPEND_AWAIT) {
                /* Await: PromiseResolve(%Promise%, v) then resume on settlement */
                r[2] = kz_promise_resolve(ctx, kz_intr(ctx, KZ_I_PROMISE), r[1]);
                if (r[2] == JS_EXCEPTION) {
                    if (rt->exc_uncatchable) return -1;
                    r[1] = kz_take_exception(ctx);
                    how = RUN_THROW;
                    continue;
                }
                if (kz_promise_then_internal(ctx, r[2], await_ok, await_err, r[0], JS_UNDEFINED)) {
                    if (rt->exc_uncatchable) return -1;
                    r[1] = kz_take_exception(ctx);
                    how = RUN_THROW;
                    continue;
                }
                return 0;
            }
            /* a yield in an async generator (the value was awaited already): AsyncGeneratorYield */
            complete_step(ctx, r[0], r[1], false, false);
            g = gen_of(r[0]);
            if (g->queue) {
                /* another request is waiting: go on without suspending */
                r[1] = g->queue->value;
                mode = g->queue->mode;
                how = RUN_YIELD;
                continue;
            }
            g->state = GS_YIELD;
            return 0;
        }
        finish(ctx, g);
        if (res == JS_EXCEPTION) {
            if (rt->exc_uncatchable) return -1;
            r[1] = kz_take_exception(ctx);
            if (g->kind == GK_ASYNC_FN) kz_promise_reject_value(ctx, g->promise, r[1]);
            else {
                complete_step(ctx, r[0], r[1], true, true);
                drain_queue(ctx, r[0]);
            }
        } else {
            r[1] = res;
            if (g->kind == GK_ASYNC_FN) kz_promise_resolve_value(ctx, g->promise, r[1]);
            else {
                complete_step(ctx, r[0], r[1], false, true);
                drain_queue(ctx, r[0]);
            }
        }
        return 0;
    }
}

static JSValue await_ok(JSContext *ctx, JSValue gobj, JSValue v) {
    return drive(ctx, gobj, RUN_VALUE, v, 0) ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue await_err(JSContext *ctx, JSValue gobj, JSValue v) {
    return drive(ctx, gobj, RUN_THROW, v, 0) ? JS_EXCEPTION : JS_UNDEFINED;
}

/* ================================================================== async functions */

JSValue kz_async_call(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv) {
    JSContext *realm = kz_function_realm(ctx, func);
    if (!realm) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 3);
    r[0] = JV_MKOBJ(func);
    r[1] = kz_new_promise_intrinsic(realm);
    if (r[1] == JS_EXCEPTION) return r[1];
    r[2] = new_gen_object(ctx, KZ_CLASS_ASYNC_FUNCTION, JS_NULL, JV_OBJ(r[0]), this_val, argc, argv, GK_ASYNC_FN);
    if (r[2] == JS_EXCEPTION) return r[2];
    gen_of(r[2])->promise = r[1];
    if (drive(ctx, r[2], RUN_START, JS_UNDEFINED, 0)) return JS_EXCEPTION;
    return r[1];
}

/* ================================================================== async generators */

static int enqueue(JSContext *ctx, JSValue gobj, JSValue v, int mode, JSValue promise) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = gobj; r[1] = v; r[2] = promise;
    KzGenReq *q = kz_malloc(ctx, sizeof(KzGenReq));
    if (!q) return -1;
    q->next = NULL;
    q->value = r[1];
    q->promise = r[2];
    q->mode = mode;
    KzGenerator *g = gen_of(r[0]);
    if (g->tail) g->tail->next = q; else g->queue = q;
    g->tail = q;
    return 0;
}

/* AsyncGeneratorCompleteStep */
static void complete_step(JSContext *ctx, JSValue gobj, JSValue v, bool is_throw, bool done) {
    KzGenerator *g = gen_of(gobj);
    KzGenReq *q = g->queue;
    if (!q) return;
    g->queue = q->next;
    if (!g->queue) g->tail = NULL;
    KZ_ROOTS(ctx, r, 3);
    r[0] = gobj;
    r[1] = q->promise;
    r[2] = v;
    kz_free(ctx->rt, q);
    if (is_throw) { kz_promise_reject_value(ctx, r[1], r[2]); return; }
    JSValue it = kz_create_iter_result(ctx, r[2], done);
    if (it == JS_EXCEPTION) {
        JSValue e = kz_take_exception(ctx);
        kz_promise_reject_value(ctx, r[1], e);
        return;
    }
    kz_promise_resolve_value(ctx, r[1], it);
}

static JSValue await_return_ok(JSContext *ctx, JSValue gobj, JSValue v) {
    gen_of(gobj)->state = GS_DONE;
    complete_step(ctx, gobj, v, false, true);
    drain_queue(ctx, gobj);
    return JS_UNDEFINED;
}

static JSValue await_return_err(JSContext *ctx, JSValue gobj, JSValue v) {
    gen_of(gobj)->state = GS_DONE;
    complete_step(ctx, gobj, v, true, true);
    drain_queue(ctx, gobj);
    return JS_UNDEFINED;
}

/* AsyncGeneratorAwaitReturn: the head of the queue is a return request */
static void await_return(JSContext *ctx, JSValue gobj) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = gobj;
    KzGenerator *g = gen_of(r[0]);
    if (!g->queue) return;
    r[1] = kz_promise_resolve(ctx, kz_intr(ctx, KZ_I_PROMISE), g->queue->value);
    if (r[1] == JS_EXCEPTION || kz_promise_then_internal(ctx, r[1], await_return_ok, await_return_err, r[0], JS_UNDEFINED)) {
        if (ctx->rt->exc_uncatchable) return;
        r[1] = kz_take_exception(ctx);
        gen_of(r[0])->state = GS_DONE;
        complete_step(ctx, r[0], r[1], true, true);
        drain_queue(ctx, r[0]);
    }
}

/* AsyncGeneratorDrainQueue */
static void drain_queue(JSContext *ctx, JSValue gobj) {
    KZ_ROOT(ctx, rg);
    rg = gobj;
    for (;;) {
        KzGenerator *g = gen_of(rg);
        KzGenReq *q = g->queue;
        if (!q) return;
        if (q->mode == RESUME_RETURN) {
            g->state = GS_AWAIT_RETURN;
            await_return(ctx, rg);
            return;
        }
        complete_step(ctx, rg, q->mode == RESUME_THROW ? q->value : JS_UNDEFINED, q->mode == RESUME_THROW, true);
    }
}

/* AsyncGenerator.prototype.next / throw / return (magic: the mode) */
static JSValue async_generator_method(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_promise_intrinsic(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!kz_is_class(this_val, KZ_CLASS_ASYNC_GENERATOR) || !gen_of(this_val)) {
        r[1] = kz_new_error_msg(ctx, KZ_ERR_TYPE, "AsyncGenerator method called on an incompatible receiver");
        if (r[1] == JS_EXCEPTION) return r[1];
        kz_promise_reject_value(ctx, r[0], r[1]);
        return r[0];
    }
    KzGenerator *g = gen_of(this_val);
    int state = g->state;
    JSValue v = argv[0];
    if (magic == RESUME_NEXT) {
        if (state == GS_DONE) {
            r[1] = kz_create_iter_result(ctx, JS_UNDEFINED, true);
            if (r[1] == JS_EXCEPTION) return r[1];
            kz_promise_resolve_value(ctx, r[0], r[1]);
            return r[0];
        }
        if (enqueue(ctx, this_val, v, RESUME_NEXT, r[0])) return JS_EXCEPTION;
        if (state == GS_START || state == GS_YIELD)
            if (drive(ctx, this_val, state == GS_START ? RUN_START : RUN_YIELD, v, RESUME_NEXT)) return JS_EXCEPTION;
        return r[0];
    }
    if (magic == RESUME_RETURN) {
        if (enqueue(ctx, this_val, v, RESUME_RETURN, r[0])) return JS_EXCEPTION;
        if (state == GS_START || state == GS_DONE) {
            g = gen_of(this_val);
            if (state == GS_START) finish(ctx, g);
            g->state = GS_AWAIT_RETURN;
            await_return(ctx, this_val);
        } else if (state == GS_YIELD) {
            if (drive(ctx, this_val, RUN_YIELD, v, RESUME_RETURN)) return JS_EXCEPTION;
        }
        return r[0];
    }
    /* throw */
    if (state == GS_START) { finish(ctx, g); state = GS_DONE; }
    if (state == GS_DONE) {
        kz_promise_reject_value(ctx, r[0], v);
        return r[0];
    }
    if (enqueue(ctx, this_val, v, RESUME_THROW, r[0])) return JS_EXCEPTION;
    if (state == GS_YIELD && drive(ctx, this_val, RUN_YIELD, v, RESUME_THROW)) return JS_EXCEPTION;
    return r[0];
}

/* ================================================================== module bodies
 *
 * A module's function (compile.c) is a generator, so that it can stop after its prologue (which makes the closures of
 * the module's function declarations) and before its first statement; it is also async when the module has a top-level
 * await.  The linker (module.c) does not call it: it makes the state object and the frame with kz_module_gen_new, puts
 * the cells of the bindings that cross a module boundary into the registers, and only then runs the prologue
 * (kz_module_gen_start, which stops at the initial yield).  Evaluating the module is resuming it, once: a synchronous
 * body runs to its end (kz_module_gen_run); an async one is driven like an async function (AsyncBlockStart), settling
 * the promise it is given (kz_module_gen_run_async), so it awaits exactly as an async function does. */

JSValue kz_module_gen_new(JSContext *ctx, JSObject *func) {
    bool async = func->u.func.b->h.flags & BC_ASYNC;
    if (async) return new_gen_object(ctx, KZ_CLASS_ASYNC_FUNCTION, JS_NULL, func, JS_UNDEFINED, 0, NULL, GK_ASYNC_FN);
    return new_gen_object(ctx, KZ_CLASS_GENERATOR, kz_intr(ctx, KZ_I_GENERATOR_PROTO), func, JS_UNDEFINED, 0, NULL, GK_SYNC);
}

/* the registers of the module's frame (NULL once it has finished) */
JSValue *kz_module_gen_regs(JSValue gobj) {
    KzGenerator *g = gen_of(gobj);
    return g && g->frame ? g->frame->regs : NULL;
}

/* runs the prologue up to the initial yield: -1 with the exception pending (out of memory) */
int kz_module_gen_start(JSContext *ctx, JSValue gobj) {
    KZ_ROOT(ctx, r);
    r = gobj;
    JSValue res = kz_run(ctx, gen_of(r)->frame);
    KzGenerator *g = gen_of(r);
    if (res != JS_EMPTY) {
        finish(ctx, g);
        return res == JS_EXCEPTION ? -1 : 0;
    }
    g->state = GS_START;
    return 0;
}

/* runs a synchronous module's body to its end: -1 with the exception pending, else 0 */
int kz_module_gen_run(JSContext *ctx, JSValue gobj) {
    return gen_resume(ctx, gobj, JS_UNDEFINED, RESUME_NEXT) == JS_EXCEPTION ? -1 : 0;
}

/* starts an async module's body; `promise` (an engine-made %Promise%) is settled when the body ends: -1 only for an
   uncatchable error (the interrupt handler said stop) */
int kz_module_gen_run_async(JSContext *ctx, JSValue gobj, JSValue promise) {
    KZ_ROOT(ctx, r);
    r = gobj;
    gen_of(r)->promise = promise;
    return drive(ctx, r, RUN_START, JS_UNDEFINED, 0) ? -1 : 0;
}

/* ================================================================== the async-from-sync iterator */

/* u.any: a = the sync iterator, b = its next method */
JSValue kz_async_from_sync_iterator(JSContext *ctx, KzIter *sync) {
    JSValue o = kz_new_object_class(ctx, KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR, kz_intr(ctx, KZ_I_ASYNC_FROM_SYNC_ITERATOR_PROTO));
    if (o == JS_EXCEPTION) return o;
    JV_OBJ(o)->u.any.a = sync->iter;
    JV_OBJ(o)->u.any.b = sync->next;
    return o;
}

static JSValue unwrap_done(JSContext *ctx, JSValue data, JSValue v) { (void)data; return kz_create_iter_result(ctx, v, true); }
static JSValue unwrap_more(JSContext *ctx, JSValue data, JSValue v) { (void)data; return kz_create_iter_result(ctx, v, false); }

static JSValue close_and_rethrow(JSContext *ctx, JSValue iter, JSValue err) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = iter;
    r[1] = err;
    KZ_ROOT_ITER(ctx, it);
    it.iter = r[0];
    kz_throw(ctx, r[1]);
    kz_iter_close_on_throw(ctx, &it);
    return JS_EXCEPTION;
}

/* AsyncFromSyncIteratorContinuation */
static JSValue afs_continue(JSContext *ctx, JSValue result, JSValue promise, JSValue iter, bool close_on_reject) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 4);
    r[0] = result;
    r[1] = promise;
    r[2] = kz_get(ctx, result, rt->atom[KZ_ATOM_done], result);
    if (r[2] == JS_EXCEPTION) goto reject;
    bool done = kz_to_bool(r[2]);
    r[2] = kz_get(ctx, r[0], rt->atom[KZ_ATOM_value], r[0]);
    if (r[2] == JS_EXCEPTION) goto reject;
    r[3] = kz_promise_resolve(ctx, kz_intr(ctx, KZ_I_PROMISE), r[2]);
    if (r[3] == JS_EXCEPTION) {
        if (!done && close_on_reject && !rt->exc_uncatchable) {
            KZ_ROOT_ITER(ctx, it);
            it.iter = iter;
            kz_iter_close_on_throw(ctx, &it);
        }
        goto reject;
    }
    if (kz_promise_then_internal(ctx, r[3], done ? unwrap_done : unwrap_more,
                                 done || !close_on_reject ? NULL : close_and_rethrow, iter, r[1]))
        goto reject;
    return r[1];
reject:
    if (rt->exc_uncatchable) return JS_EXCEPTION;
    r[2] = kz_take_exception(ctx);
    kz_promise_reject_value(ctx, r[1], r[2]);
    return r[1];
}

/* next (0), return (1), throw (2) */
static JSValue afs_method(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 4);
    r[0] = kz_new_promise_intrinsic(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    if (!kz_is_class(this_val, KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR)) return kz_throw_type(ctx, "incompatible receiver");
    r[1] = JV_OBJ(this_val)->u.any.a;      /* the sync iterator */
    if (magic == 0) {
        JSValue nx = JV_OBJ(this_val)->u.any.b;
        r[2] = kz_call(ctx, nx, r[1], argc > 0 ? 1 : 0, argv);
        if (r[2] == JS_EXCEPTION) goto reject;
        if (!JV_IS_OBJECT(r[2])) { kz_throw_type(ctx, "Iterator result is not an object"); goto reject; }
        return afs_continue(ctx, r[2], r[0], r[1], true);
    }
    r[3] = kz_get_method(ctx, r[1], rt->atom[magic == 1 ? KZ_ATOM_return : KZ_ATOM_throw]);
    if (r[3] == JS_EXCEPTION) goto reject;
    if (r[3] == JS_UNDEFINED) {
        if (magic == 1) {
            r[2] = kz_create_iter_result(ctx, argv[0], true);
            if (r[2] == JS_EXCEPTION) goto reject;
            kz_promise_resolve_value(ctx, r[0], r[2]);
            return r[0];
        }
        /* no throw method: close the sync iterator, then reject with a TypeError */
        KZ_ROOT_ITER(ctx, it);
        it.iter = r[1];
        if (kz_iter_close(ctx, &it, JS_UNDEFINED) == JS_EXCEPTION) goto reject;
        kz_throw_type(ctx, "The iterator does not provide a 'throw' method");
        goto reject;
    }
    r[2] = kz_call(ctx, r[3], r[1], argc > 0 ? 1 : 0, argv);
    if (r[2] == JS_EXCEPTION) goto reject;
    if (!JV_IS_OBJECT(r[2])) { kz_throw_type(ctx, "Iterator result is not an object"); goto reject; }
    return afs_continue(ctx, r[2], r[0], r[1], magic == 2);
reject:
    if (rt->exc_uncatchable) return JS_EXCEPTION;
    r[2] = kz_take_exception(ctx);
    kz_promise_reject_value(ctx, r[0], r[2]);
    return r[0];
}

/* ================================================================== setup */

static const KzFuncDef generator_proto_funcs[] = {
    KZ_FN_MAGIC("next", generator_method, 1, RESUME_NEXT),
    KZ_FN_MAGIC("return", generator_method, 1, RESUME_RETURN),
    KZ_FN_MAGIC("throw", generator_method, 1, RESUME_THROW),
};

static const KzFuncDef async_generator_proto_funcs[] = {
    KZ_FN_MAGIC("next", async_generator_method, 1, RESUME_NEXT),
    KZ_FN_MAGIC("return", async_generator_method, 1, RESUME_RETURN),
    KZ_FN_MAGIC("throw", async_generator_method, 1, RESUME_THROW),
};

static const KzFuncDef afs_proto_funcs[] = {
    KZ_FN_MAGIC("next", afs_method, 1, 0),
    KZ_FN_MAGIC("return", afs_method, 1, 1),
    KZ_FN_MAGIC("throw", afs_method, 1, 2),
};

static int link_kind(JSContext *ctx, KzIntrinsic fn_proto, KzIntrinsic proto_id, KzIntrinsic parent, const KzFuncDef *funcs,
                     int n, const char *tag) {
    KZ_ROOT(ctx, p);
    p = kz_new_object_proto(ctx, kz_intr(ctx, parent));
    if (p == JS_EXCEPTION) return -1;
    ctx->intrinsic[proto_id] = p;
    if (kz_define_functions(ctx, p, funcs, n)) return -1;
    JSValue t = kz_atom(ctx, tag);
    if (t == JS_EXCEPTION || kz_define_value(ctx, p, ctx->rt->sym[KZ_SYM_toStringTag], t, KZ_PROP_C)) return -1;
    /* %GeneratorFunction.prototype%.prototype and back */
    if (kz_define_value_atom(ctx, kz_intr(ctx, fn_proto), KZ_ATOM_prototype, p, KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, p, KZ_ATOM_constructor, kz_intr(ctx, fn_proto), KZ_PROP_C)) return -1;
    return 0;
}

int kz_init_generators(JSContext *ctx) {
    if (link_kind(ctx, KZ_I_GENERATOR_FUNCTION_PROTO, KZ_I_GENERATOR_PROTO, KZ_I_ITERATOR_PROTO, generator_proto_funcs,
                  KZ_COUNTOF(generator_proto_funcs), "Generator"))
        return -1;
    if (link_kind(ctx, KZ_I_ASYNC_GENERATOR_FUNCTION_PROTO, KZ_I_ASYNC_GENERATOR_PROTO, KZ_I_ASYNC_ITERATOR_PROTO,
                  async_generator_proto_funcs, KZ_COUNTOF(async_generator_proto_funcs), "AsyncGenerator"))
        return -1;
    KZ_ROOT(ctx, p);
    p = kz_new_object_proto(ctx, kz_intr(ctx, KZ_I_ASYNC_ITERATOR_PROTO));
    if (p == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ASYNC_FROM_SYNC_ITERATOR_PROTO] = p;
    if (kz_define_functions(ctx, p, afs_proto_funcs, KZ_COUNTOF(afs_proto_funcs))) return -1;
    return 0;
}
