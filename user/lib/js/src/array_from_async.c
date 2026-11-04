/*
 * array_from_async.c: Array.fromAsync (ECMA-262 2026, 23.1.2.2).
 *
 * The specification writes it as an async closure: an async function body with the awaits at fixed places.  Here the body
 * is a state machine over a state array, resumed by promise reactions, so that every Await takes the promise ticks the
 * specification's does (PromiseResolve, then a reaction job).  The phases are the places after an await:
 *   PH_NEXT_RESULT   the iterator's next() promise settled
 *   PH_MAPPED        the mapper's result settled (iterator case)
 *   PH_CLOSE         the return() of AsyncIteratorClose settled (the error that caused the close is then rethrown)
 *   PH_LIKE_VALUE    an array-like's element settled
 *   PH_LIKE_MAPPED   the mapper's result settled (array-like case)
 * Any abrupt completion rejects the promise Array.fromAsync returned.
 */
#include "jsint.h"

enum { S_PROMISE, S_C, S_ITEMS, S_MAPFN, S_THIS, S_ITER, S_NEXT, S_A, S_K, S_LIKE, S_LEN, S_PHASE, S_ERR, S_NFIELDS };
enum { PH_START, PH_CALL_NEXT, PH_NEXT_RESULT, PH_MAPPED, PH_CLOSE, PH_LIKE_LOOP, PH_LIKE_VALUE, PH_LIKE_MAPPED };

JSValue kz_async_from_sync_iterator(JSContext *ctx, KzIter *sync);

static void run(JSContext *ctx, JSValue stv, JSValue input, bool is_throw);

static JSValue resumed_ok(JSContext *ctx, JSValue data, JSValue v) {
    run(ctx, data, v, false);
    return ctx->rt->exc_uncatchable && kz_has_exception(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue resumed_err(JSContext *ctx, JSValue data, JSValue v) {
    run(ctx, data, v, true);
    return ctx->rt->exc_uncatchable && kz_has_exception(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
}

/* Await(value): the state machine continues in run() when the promise settles; false with an exception pending when the
   awaiting itself failed (it is then thrown at the await point) */
static bool await_value(JSContext *ctx, JSValue stv, JSValue value) {
    KZ_ROOT(ctx, p);
    p = kz_promise_resolve(ctx, kz_intr(ctx, KZ_I_PROMISE), value);
    if (p == JS_EXCEPTION) return false;
    return kz_promise_then_internal(ctx, p, resumed_ok, resumed_err, stv, JS_UNDEFINED) == 0;
}

static JSValue *fields(JSValue stv) { return JV_OBJ(stv)->elems; }

/* the key for index k (an array index, or a string for the 2^53 range) */
static JSValue index_key(JSContext *ctx, double k) {
    if (k < 4294967295.0) return KZ_KEY_FROM_INDEX((uint32_t)k);
    return kz_to_property_key(ctx, jv_number(k));
}

/* CreateDataPropertyOrThrow(A, ToString(k), v) */
static int define_element(JSContext *ctx, JSValue a, double k, JSValue v) {
    KZ_ROOT(ctx, key);
    key = index_key(ctx, k);
    if (key == JS_EXCEPTION) return -1;
    return kz_create_data_property(ctx, a, key, v);
}

/* Set(A, "length", n, true) */
static int set_length(JSContext *ctx, JSValue a, double n) {
    return kz_set_prop(ctx, a, ctx->rt->atom[KZ_ATOM_length], jv_number(n), true) < 0 ? -1 : 0;
}

static void settle_ok(JSContext *ctx, JSValue stv) {
    JSValue *st = fields(stv);
    kz_promise_resolve_value(ctx, st[S_PROMISE], st[S_A]);
}

/* rejects the result promise with the exception in flight (an uncatchable one - an interrupt - stays pending instead) */
static void settle_throw(JSContext *ctx, JSValue stv) {
    if (ctx->rt->exc_uncatchable) return;
    KZ_ROOT(ctx, e);
    e = kz_take_exception(ctx);
    kz_promise_reject_value(ctx, fields(stv)[S_PROMISE], e);
}

/* AsyncIteratorClose(iteratorRecord, throw completion): the exception in flight is the completion.  Calls return() and
   waits for what it returns; the original error is then thrown whatever return() did.  Returns true when it is waiting
   (the machine resumes in PH_CLOSE), false when the promise was rejected at once. */
static bool close_async_iterator(JSContext *ctx, JSValue stv) {
    JSRuntime *rt = ctx->rt;
    if (rt->exc_uncatchable) return false;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_take_exception(ctx);
    JSValue *st = fields(stv);
    st[S_ERR] = r[0];
    r[1] = kz_get_method(ctx, st[S_ITER], rt->atom[KZ_ATOM_return]);
    if (r[1] != JS_EXCEPTION && r[1] != JS_UNDEFINED) {
        r[2] = kz_call(ctx, r[1], st[S_ITER], 0, NULL);
        if (r[2] != JS_EXCEPTION) {
            fields(stv)[S_PHASE] = JV_MKINT(PH_CLOSE);
            if (await_value(ctx, stv, r[2])) return true;
        }
    }
    if (rt->exc_uncatchable) return false;
    if (kz_has_exception(ctx)) kz_take_exception(ctx);       /* (an error of return() itself is dropped: the original wins) */
    kz_throw(ctx, r[0]);
    settle_throw(ctx, stv);
    return false;
}

static void run(JSContext *ctx, JSValue stv, JSValue input, bool is_throw) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 5);
    r[0] = stv;
    r[1] = input;
    for (;;) {
        JSValue *st = fields(r[0]);
        int phase = JV_INT(st[S_PHASE]);
        /* a rejection of what was awaited is thrown at the await point */
        if (is_throw) {
            is_throw = false;
            if (phase == PH_CLOSE) {                         /* return() failed or rejected: the original error is rethrown */
                kz_throw(ctx, st[S_ERR]);
                settle_throw(ctx, r[0]);
                return;
            }
            kz_throw(ctx, r[1]);
            if (phase == PH_MAPPED) { close_async_iterator(ctx, r[0]); return; }
            settle_throw(ctx, r[0]);
            return;
        }
        switch (phase) {
        case PH_START: {
            JSValue mapfn = st[S_MAPFN], items = st[S_ITEMS];
            if (mapfn != JS_UNDEFINED && !kz_is_callable(mapfn)) { kz_throw_type(ctx, "Array.fromAsync: the map function is not callable"); goto fail; }
            r[2] = kz_get_method(ctx, items, rt->sym[KZ_SYM_asyncIterator]);
            if (r[2] == JS_EXCEPTION) goto fail;
            bool sync = false;
            if (r[2] == JS_UNDEFINED) {
                r[2] = kz_get_method(ctx, items, rt->sym[KZ_SYM_iterator]);
                if (r[2] == JS_EXCEPTION) goto fail;
                sync = true;
            }
            if (r[2] != JS_UNDEFINED) {
                KZ_ROOT_ITER(ctx, it);
                if (kz_get_iterator_from_method(ctx, items, r[2], &it)) goto fail;
                if (sync) {
                    JSValue ai = kz_async_from_sync_iterator(ctx, &it);
                    if (ai == JS_EXCEPTION) goto fail;
                    it.iter = ai;
                    it.next = kz_get(ctx, ai, rt->atom[KZ_ATOM_next], ai);
                    if (it.next == JS_EXCEPTION) goto fail;
                }
                st = fields(r[0]);
                st[S_ITER] = it.iter;
                st[S_NEXT] = it.next;
                JSValue c = st[S_C];
                r[3] = kz_is_constructor(c) ? kz_construct(ctx, c, 0, NULL, c) : kz_new_array(ctx, 0);
                if (r[3] == JS_EXCEPTION) goto fail;
                fields(r[0])[S_A] = r[3];
                fields(r[0])[S_K] = JV_MKINT(0);
                fields(r[0])[S_PHASE] = JV_MKINT(PH_CALL_NEXT);
                continue;
            }
            /* neither: an array-like */
            r[3] = kz_to_object(ctx, items);
            if (r[3] == JS_EXCEPTION) goto fail;
            uint64_t len;
            if (kz_length_of(ctx, r[3], &len)) goto fail;
            JSValue c = fields(r[0])[S_C];
            if (kz_is_constructor(c)) {
                JSValue lenv = jv_number((double)len);
                r[4] = kz_construct(ctx, c, 1, &lenv, c);
            } else r[4] = kz_array_create(ctx, len, kz_intr(ctx, KZ_I_ARRAY_PROTO));
            if (r[4] == JS_EXCEPTION) goto fail;
            st = fields(r[0]);
            st[S_LIKE] = r[3];
            st[S_LEN] = jv_number((double)len);
            st[S_A] = r[4];
            st[S_K] = JV_MKINT(0);
            st[S_PHASE] = JV_MKINT(PH_LIKE_LOOP);
            continue;
        }
        case PH_CALL_NEXT: {
            double k = jv_num(st[S_K]);
            if (k >= 9007199254740991.0) {
                kz_throw_type(ctx, "Array.fromAsync: too many elements");
                close_async_iterator(ctx, r[0]);
                return;
            }
            r[2] = kz_call(ctx, st[S_NEXT], st[S_ITER], 0, NULL);
            if (r[2] == JS_EXCEPTION) goto fail;
            st[S_PHASE] = JV_MKINT(PH_NEXT_RESULT);
            if (!await_value(ctx, r[0], r[2])) { r[1] = kz_take_exception(ctx); is_throw = true; }
            else return;
            continue;
        }
        case PH_NEXT_RESULT: {
            JSValue res = r[1];
            if (!JV_IS_OBJECT(res)) { kz_throw_type(ctx, "Iterator result is not an object"); goto fail; }
            r[2] = kz_get(ctx, res, rt->atom[KZ_ATOM_done], res);
            if (r[2] == JS_EXCEPTION) goto fail;
            st = fields(r[0]);
            double k = jv_num(st[S_K]);
            if (kz_to_bool(r[2])) {
                if (set_length(ctx, st[S_A], k)) goto fail;
                settle_ok(ctx, r[0]);
                return;
            }
            r[2] = kz_get(ctx, res, rt->atom[KZ_ATOM_value], res);
            if (r[2] == JS_EXCEPTION) goto fail;
            st = fields(r[0]);
            if (st[S_MAPFN] != JS_UNDEFINED) {
                JSValue args[2] = { r[2], jv_number(k) };
                r[3] = kz_call(ctx, st[S_MAPFN], st[S_THIS], 2, args);
                if (r[3] == JS_EXCEPTION) { close_async_iterator(ctx, r[0]); return; }
                fields(r[0])[S_PHASE] = JV_MKINT(PH_MAPPED);
                if (!await_value(ctx, r[0], r[3])) {
                    close_async_iterator(ctx, r[0]);
                    return;
                }
                return;
            }
            r[1] = r[2];                    /* (no mapper: the value itself is defined, as in the step after the mapper's await) */
            st = fields(r[0]);
            if (define_element(ctx, st[S_A], k, r[1])) { close_async_iterator(ctx, r[0]); return; }
            fields(r[0])[S_K] = jv_number(k + 1);
            fields(r[0])[S_PHASE] = JV_MKINT(PH_CALL_NEXT);
            continue;
        }
        case PH_MAPPED: {
            double k = jv_num(st[S_K]);
            if (define_element(ctx, st[S_A], k, r[1])) { close_async_iterator(ctx, r[0]); return; }
            fields(r[0])[S_K] = jv_number(k + 1);
            fields(r[0])[S_PHASE] = JV_MKINT(PH_CALL_NEXT);
            continue;
        }
        case PH_CLOSE:                      /* return() settled normally: the error that started the close is thrown */
            kz_throw(ctx, st[S_ERR]);
            settle_throw(ctx, r[0]);
            return;
        case PH_LIKE_LOOP: {
            double k = jv_num(st[S_K]), len = jv_num(st[S_LEN]);
            if (k >= len) {
                if (set_length(ctx, st[S_A], len)) goto fail;
                settle_ok(ctx, r[0]);
                return;
            }
            r[2] = kz_get(ctx, st[S_LIKE], index_key(ctx, k), st[S_LIKE]);
            if (r[2] == JS_EXCEPTION) goto fail;
            fields(r[0])[S_PHASE] = JV_MKINT(PH_LIKE_VALUE);
            if (!await_value(ctx, r[0], r[2])) { r[1] = kz_take_exception(ctx); is_throw = true; continue; }
            return;
        }
        case PH_LIKE_VALUE: {
            double k = jv_num(st[S_K]);
            if (st[S_MAPFN] != JS_UNDEFINED) {
                JSValue args[2] = { r[1], jv_number(k) };
                r[2] = kz_call(ctx, st[S_MAPFN], st[S_THIS], 2, args);
                if (r[2] == JS_EXCEPTION) goto fail;
                fields(r[0])[S_PHASE] = JV_MKINT(PH_LIKE_MAPPED);
                if (!await_value(ctx, r[0], r[2])) { r[1] = kz_take_exception(ctx); is_throw = true; continue; }
                return;
            }
            if (define_element(ctx, st[S_A], k, r[1])) goto fail;
            fields(r[0])[S_K] = jv_number(k + 1);
            fields(r[0])[S_PHASE] = JV_MKINT(PH_LIKE_LOOP);
            continue;
        }
        default: {                                          /* PH_LIKE_MAPPED */
            double k = jv_num(st[S_K]);
            if (define_element(ctx, st[S_A], k, r[1])) goto fail;
            fields(r[0])[S_K] = jv_number(k + 1);
            fields(r[0])[S_PHASE] = JV_MKINT(PH_LIKE_LOOP);
            continue;
        }
        }
    fail:
        settle_throw(ctx, r[0]);
        return;
    }
}

JSValue kz_array_from_async(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_promise_intrinsic(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    JSValue vals[S_NFIELDS] = { r[0], this_val, argv[0], argv[1], argv[2], JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                                JV_MKINT(0), JS_UNDEFINED, JV_MKINT(0), JV_MKINT(PH_START), JS_UNDEFINED };
    r[1] = kz_new_array_from(ctx, S_NFIELDS, vals);
    if (r[1] == JS_EXCEPTION) return r[1];
    run(ctx, r[1], JS_UNDEFINED, false);
    if (ctx->rt->exc_uncatchable && kz_has_exception(ctx)) return JS_EXCEPTION;
    return r[0];
}
