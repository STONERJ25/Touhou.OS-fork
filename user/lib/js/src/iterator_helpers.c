/*
 * iterator_helpers.c: the Iterator constructor and the iterator helpers (ECMA-262 27.1.3 and 27.1.4, ES2025): Iterator,
 * Iterator.from, %WrapForValidIteratorPrototype%, %IteratorHelperPrototype%, and on Iterator.prototype the lazy methods
 * map, filter, take, drop and flatMap (each returns an iterator helper object) and the eager ones reduce, toArray,
 * forEach, some, every and find.
 *
 * The specification writes a helper as a generator closure.  Here a helper is a state machine: the object keeps the
 * underlying iterator record, the callback and a few counters in an array (u.any.a), its generator state in u.any.b
 * (suspended-start, suspended-yield, executing, completed) and its kind in u.any.c.  next() runs the closure's loop up
 * to its next Yield (or its end); return() is the closure resumed with a return completion.  Every abrupt completion
 * inside the closure closes the underlying iterator first (IfAbruptCloseIterator), which is what kz_iter_close_on_throw
 * does: the error in flight stays, whatever the iterator's return() does.
 */
#include "jsint.h"

enum { HK_MAP, HK_FILTER, HK_TAKE, HK_DROP, HK_FLATMAP, HK_WRAP };           /* u.any.c */
enum { HS_START, HS_YIELD, HS_RUNNING, HS_DONE };                              /* u.any.b */
enum { F_ITER, F_NEXT, F_FN, F_COUNT, F_INNER, F_INNER_NEXT, F_FLAG, F_NFIELDS };   /* the state array (u.any.a) */

/* ================================================================== iterator records */

/* GetIteratorDirect (7.4.2): the next method is read once, here */
static int get_iterator_direct(JSContext *ctx, JSValue obj, KzIter *it) {
    it->iter = obj;
    it->next = JS_UNDEFINED;
    JSValue next = kz_get(ctx, obj, ctx->rt->atom[KZ_ATOM_next], obj);
    if (next == JS_EXCEPTION) return -1;
    it->next = next;
    it->done = false;
    return 0;
}

/* GetIteratorFlattenable (7.4.3) */
static int get_iterator_flattenable(JSContext *ctx, JSValue obj, bool allow_strings, KzIter *it) {
    if (!JV_IS_OBJECT(obj) && !(allow_strings && JV_IS_STRING(obj))) {
        char buf[64];
        kz_format_value_for_msg(ctx, obj, buf, sizeof buf);
        kz_throw_type(ctx, "%s is not an object", buf);
        return -1;
    }
    KZ_ROOTS(ctx, r, 2);
    r[0] = obj;
    r[1] = kz_get_method(ctx, obj, ctx->rt->sym[KZ_SYM_iterator]);
    if (r[1] == JS_EXCEPTION) return -1;
    JSValue iter = obj;
    if (r[1] != JS_UNDEFINED) {
        iter = kz_call(ctx, r[1], obj, 0, NULL);
        if (iter == JS_EXCEPTION) return -1;
    }
    if (!JV_IS_OBJECT(iter)) { kz_throw_type(ctx, "Result of the Symbol.iterator method is not an object"); return -1; }
    r[0] = iter;
    return get_iterator_direct(ctx, iter, it);
}

/* IteratorStep (7.4.8) without reading the value: 1 not done, 0 done, -1 */
static int iter_step_only(JSContext *ctx, KzIter *it) {
    KZ_ROOT(ctx, res);
    res = kz_call(ctx, it->next, it->iter, 0, NULL);
    if (res == JS_EXCEPTION) { it->done = true; return -1; }
    if (!JV_IS_OBJECT(res)) { it->done = true; kz_throw_type(ctx, "Iterator result is not an object"); return -1; }
    JSValue done = kz_get(ctx, res, ctx->rt->atom[KZ_ATOM_done], res);
    if (done == JS_EXCEPTION) { it->done = true; return -1; }
    if (kz_to_bool(done)) { it->done = true; return 0; }
    return 1;
}

/* the error in flight stays; the iterator is closed (its failures are dropped) */
static JSValue close_and_fail(JSContext *ctx, KzIter *it) {
    kz_iter_close_on_throw(ctx, it);
    return JS_EXCEPTION;
}

/* ================================================================== the Iterator constructor and Iterator.from */

static JSValue iterator_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED || rt->new_target == JV_MKOBJ(rt->cur_native))
        return kz_throw_type(ctx, "Abstract class Iterator not directly constructable");
    return kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_ITERATOR_PROTO, KZ_CLASS_OBJECT);
}

/* the objects Iterator.from makes around iterators that do not inherit from Iterator.prototype: u.any.a = [iterator, next] */
static JSValue make_wrapper(JSContext *ctx, KzIter *it) {
    KZ_ROOTS(ctx, r, 2);
    JSValue pair[2] = { it->iter, it->next };
    r[0] = kz_new_array_from(ctx, 2, pair);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_new_object_class(ctx, KZ_CLASS_ITERATOR_HELPER, kz_intr(ctx, KZ_I_WRAP_FOR_VALID_ITERATOR_PROTO));
    if (r[1] == JS_EXCEPTION) return r[1];
    JSObject *w = JV_OBJ(r[1]);
    w->u.any.a = r[0];
    w->u.any.b = JV_MKINT(HS_START);
    w->u.any.c = JV_MKINT(HK_WRAP);
    return r[1];
}

static JSValue iterator_from(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT_ITER(ctx, it);
    if (get_iterator_flattenable(ctx, argv[0], true, &it)) return JS_EXCEPTION;
    int has = kz_ordinary_has_instance(ctx, kz_intr(ctx, KZ_I_ITERATOR), it.iter);
    if (has < 0) return JS_EXCEPTION;
    if (has) return it.iter;
    return make_wrapper(ctx, &it);
}

static JSObject *helper_of(JSValue v, bool wrapper) {
    if (!kz_is_class(v, KZ_CLASS_ITERATOR_HELPER)) return NULL;
    JSObject *o = JV_OBJ(v);
    return (JV_INT(o->u.any.c) == HK_WRAP) == wrapper ? o : NULL;
}

static JSValue wrap_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSObject *w = helper_of(this_val, true);
    if (!w) return kz_throw_type(ctx, "next method called on an incompatible receiver");
    JSValue *st = JV_OBJ(w->u.any.a)->elems;
    return kz_call(ctx, st[1], st[0], 0, NULL);
}

static JSValue wrap_return(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSObject *w = helper_of(this_val, true);
    if (!w) return kz_throw_type(ctx, "return method called on an incompatible receiver");
    KZ_ROOT(ctx, m);
    JSValue iter = JV_OBJ(w->u.any.a)->elems[0];
    m = kz_get_method(ctx, iter, ctx->rt->atom[KZ_ATOM_return]);
    if (m == JS_EXCEPTION) return m;
    if (m == JS_UNDEFINED) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    return kz_call(ctx, m, iter, 0, NULL);
}

/* SetterThatIgnoresPrototypeProperties (27.1.4.x): assigning to Iterator.prototype.constructor or @@toStringTag through an
   object that inherits from it makes an own property instead of failing on the accessor */
static JSValue ignoring_setter(JSContext *ctx, JSValue this_val, JSValue key, JSValue v) {
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Iterator.prototype setter called on a non-object");
    if (this_val == kz_intr(ctx, KZ_I_ITERATOR_PROTO)) return kz_throw_type(ctx, "Cannot assign to this property of Iterator.prototype");
    KZ_ROOT_DESC(ctx, d);
    int has = kz_get_own_property(ctx, JV_OBJ(this_val), key, &d);
    if (has < 0) return JS_EXCEPTION;
    if (!has) return kz_create_data_property(ctx, this_val, key, v) ? JS_EXCEPTION : JS_UNDEFINED;
    return kz_set(ctx, this_val, key, v, this_val, true) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue proto_constructor_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return kz_intr(ctx, KZ_I_ITERATOR);
}

static JSValue proto_constructor_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    return ignoring_setter(ctx, this_val, ctx->rt->atom[KZ_ATOM_constructor], argv[0]);
}

static JSValue proto_tag_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return kz_atom(ctx, "Iterator");
}

static JSValue proto_tag_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    return ignoring_setter(ctx, this_val, ctx->rt->sym[KZ_SYM_toStringTag], argv[0]);
}

/* ================================================================== the lazy helpers */

/* the limit of take and drop (ToNumber, then the range checks); false with the error thrown */
static bool check_limit(JSContext *ctx, JSValue limit, double *out) {
    double d;
    if (kz_to_number(ctx, limit, &d)) return false;
    if (d != d) { kz_throw_range(ctx, "limit must be a number that is not NaN"); return false; }
    if (d != KZ_INFINITY && d > 9007199254740991.0) { kz_throw_range(ctx, "limit is too large"); return false; }
    double n = d == KZ_INFINITY || d == -KZ_INFINITY ? d : kz_math_trunc(d);
    if (n < 0) { kz_throw_range(ctx, "limit must not be negative"); return false; }
    *out = n == 0 ? 0 : n;
    return true;
}

/* map, filter, take, drop, flatMap: validate the arguments (closing the receiver when they are bad), read next, make the helper */
static JSValue iterator_lazy(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Iterator helper called on a non-object");
    KZ_ROOT_ITER(ctx, it);
    it.iter = this_val;
    KZ_ROOT(ctx, count);
    count = JV_MKINT(0);
    JSValue fn = JS_UNDEFINED;
    if (magic == HK_TAKE || magic == HK_DROP) {
        double limit;
        if (!check_limit(ctx, argv[0], &limit)) return close_and_fail(ctx, &it);
        count = jv_number(limit);
    } else {
        fn = argv[0];
        if (!kz_is_callable(fn)) {
            kz_throw_type(ctx, "%s is not a function", "the argument");
            return close_and_fail(ctx, &it);
        }
    }
    if (get_iterator_direct(ctx, this_val, &it)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 2);
    JSValue vals[F_NFIELDS] = { it.iter, it.next, fn, count, JS_UNDEFINED, JS_UNDEFINED, JV_MKINT(0) };
    r[0] = kz_new_array_from(ctx, F_NFIELDS, vals);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_new_object_class(ctx, KZ_CLASS_ITERATOR_HELPER, kz_intr(ctx, KZ_I_ITERATOR_HELPER_PROTO));
    if (r[1] == JS_EXCEPTION) return r[1];
    JSObject *h = JV_OBJ(r[1]);
    h->u.any.a = r[0];
    h->u.any.b = JV_MKINT(HS_START);
    h->u.any.c = JV_MKINT(magic);
    return r[1];
}

static void load_iter(JSValue *st, KzIter *it) {
    it->iter = st[F_ITER];
    it->next = st[F_NEXT];
    it->done = false;
}

/* runs the closure from its current place to its next Yield: 1 with *out the yielded value, 0 when it ends, -1 with an
   exception (the underlying iterator is closed already where the specification says so) */
static int helper_run(JSContext *ctx, JSObject *h, JSValue *out) {
    int kind = JV_INT(h->u.any.c);
    JSValue *st = JV_OBJ(h->u.any.a)->elems;
    KZ_ROOT_ITER(ctx, it);
    load_iter(st, &it);
    KZ_ROOTS(ctx, r, 3);
    switch (kind) {
    case HK_MAP: {
        int s = kz_iter_step(ctx, &it, &r[0]);
        if (s <= 0) return s;
        JSValue args[2] = { r[0], st[F_COUNT] };
        r[1] = kz_call(ctx, st[F_FN], JS_UNDEFINED, 2, args);
        if (r[1] == JS_EXCEPTION) return close_and_fail(ctx, &it), -1;
        st[F_COUNT] = jv_number(jv_num(st[F_COUNT]) + 1);
        *out = r[1];
        return 1;
    }
    case HK_FILTER:
        for (;;) {
            int s = kz_iter_step(ctx, &it, &r[0]);
            if (s <= 0) return s;
            JSValue args[2] = { r[0], st[F_COUNT] };
            r[1] = kz_call(ctx, st[F_FN], JS_UNDEFINED, 2, args);
            if (r[1] == JS_EXCEPTION) return close_and_fail(ctx, &it), -1;
            st[F_COUNT] = jv_number(jv_num(st[F_COUNT]) + 1);
            if (kz_to_bool(r[1])) { *out = r[0]; return 1; }
        }
    case HK_TAKE: {
        double remaining = jv_num(st[F_COUNT]);
        if (remaining == 0) return kz_iter_close(ctx, &it, JS_UNDEFINED) == JS_EXCEPTION ? -1 : 0;
        if (remaining != KZ_INFINITY) st[F_COUNT] = jv_number(remaining - 1);
        int s = kz_iter_step(ctx, &it, &r[0]);
        if (s <= 0) return s;
        *out = r[0];
        return 1;
    }
    case HK_DROP: {
        double remaining = jv_num(st[F_COUNT]);
        while (remaining > 0) {
            if (remaining != KZ_INFINITY) remaining -= 1;
            st[F_COUNT] = jv_number(remaining);
            int s = iter_step_only(ctx, &it);
            if (s <= 0) return s;
        }
        int s = kz_iter_step(ctx, &it, &r[0]);
        if (s <= 0) return s;
        *out = r[0];
        return 1;
    }
    default: {                                  /* HK_FLATMAP */
        for (;;) {
            if (st[F_INNER] != JS_UNDEFINED) {
                KZ_ROOT_ITER(ctx, inner);
                inner.iter = st[F_INNER];
                inner.next = st[F_INNER_NEXT];
                int s = kz_iter_step(ctx, &inner, &r[0]);
                if (s < 0) return close_and_fail(ctx, &it), -1;
                if (s == 0) {
                    st[F_INNER] = st[F_INNER_NEXT] = JS_UNDEFINED;
                    st[F_COUNT] = jv_number(jv_num(st[F_COUNT]) + 1);
                    continue;
                }
                *out = r[0];
                return 1;
            }
            int s = kz_iter_step(ctx, &it, &r[0]);
            if (s <= 0) return s;
            JSValue args[2] = { r[0], st[F_COUNT] };
            r[1] = kz_call(ctx, st[F_FN], JS_UNDEFINED, 2, args);
            if (r[1] == JS_EXCEPTION) return close_and_fail(ctx, &it), -1;
            KZ_ROOT_ITER(ctx, inner);
            if (get_iterator_flattenable(ctx, r[1], false, &inner)) return close_and_fail(ctx, &it), -1;
            st[F_INNER] = inner.iter;
            st[F_INNER_NEXT] = inner.next;
        }
    }
    }
}

static JSValue helper_next(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSObject *h = helper_of(this_val, false);
    if (!h) return kz_throw_type(ctx, "Iterator Helper.prototype.next called on an incompatible receiver");
    int state = JV_INT(h->u.any.b);
    if (state == HS_RUNNING) return kz_throw_type(ctx, "Generator is already running");
    if (state == HS_DONE) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    h->u.any.b = JV_MKINT(HS_RUNNING);
    KZ_ROOT(ctx, out);
    int rc = helper_run(ctx, h, &out);
    h->u.any.b = JV_MKINT(rc == 1 ? HS_YIELD : HS_DONE);
    if (rc < 0) return JS_EXCEPTION;
    return kz_create_iter_result(ctx, rc == 1 ? out : JS_UNDEFINED, rc == 0);
}

static JSValue helper_return(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    JSObject *h = helper_of(this_val, false);
    if (!h) return kz_throw_type(ctx, "Iterator Helper.prototype.return called on an incompatible receiver");
    int state = JV_INT(h->u.any.b);
    if (state == HS_RUNNING) return kz_throw_type(ctx, "Generator is already running");
    if (state == HS_DONE) return kz_create_iter_result(ctx, JS_UNDEFINED, true);
    JSValue *st = JV_OBJ(h->u.any.a)->elems;
    h->u.any.b = JV_MKINT(HS_DONE);                 /* (whatever happens next, it does not run again) */
    KZ_ROOT_ITER(ctx, it);
    load_iter(st, &it);
    if (state == HS_YIELD && JV_INT(h->u.any.c) == HK_FLATMAP && st[F_INNER] != JS_UNDEFINED) {
        /* resumed with a return completion inside flatMap's inner loop: the inner iterator is closed first; if that fails the
           outer one is closed with that error */
        KZ_ROOT_ITER(ctx, inner);
        inner.iter = st[F_INNER];
        inner.next = st[F_INNER_NEXT];
        if (kz_iter_close(ctx, &inner, JS_UNDEFINED) == JS_EXCEPTION) return close_and_fail(ctx, &it);
    }
    if (kz_iter_close(ctx, &it, JS_UNDEFINED) == JS_EXCEPTION) return JS_EXCEPTION;
    return kz_create_iter_result(ctx, JS_UNDEFINED, true);
}

/* ================================================================== the eager methods */

enum { EK_FOREACH, EK_SOME, EK_EVERY, EK_FIND };

static JSValue iterator_eager(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Iterator helper called on a non-object");
    KZ_ROOT_ITER(ctx, it);
    it.iter = this_val;
    JSValue fn = argv[0];
    if (!kz_is_callable(fn)) {
        kz_throw_type(ctx, "%s is not a function", "the argument");
        return close_and_fail(ctx, &it);
    }
    if (get_iterator_direct(ctx, this_val, &it)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 2);
    r[1] = fn;
    double counter = 0;
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[0]);
        if (s < 0) return JS_EXCEPTION;
        if (s == 0) return magic == EK_FOREACH ? JS_UNDEFINED : magic == EK_EVERY ? JS_TRUE : magic == EK_SOME ? JS_FALSE : JS_UNDEFINED;
        JSValue args[2] = { r[0], jv_number(counter) };
        JSValue res = kz_call(ctx, fn, JS_UNDEFINED, 2, args);
        if (res == JS_EXCEPTION) return close_and_fail(ctx, &it);
        counter += 1;
        switch (magic) {
        case EK_SOME:
            if (kz_to_bool(res)) return kz_iter_close(ctx, &it, JS_TRUE);
            break;
        case EK_EVERY:
            if (!kz_to_bool(res)) return kz_iter_close(ctx, &it, JS_FALSE);
            break;
        case EK_FIND:
            if (kz_to_bool(res)) return kz_iter_close(ctx, &it, r[0]);
            break;
        default: break;
        }
    }
}

static JSValue iterator_reduce(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Iterator.prototype.reduce called on a non-object");
    KZ_ROOT_ITER(ctx, it);
    it.iter = this_val;
    JSValue fn = argv[0];
    if (!kz_is_callable(fn)) {
        kz_throw_type(ctx, "%s is not a function", "the reducer");
        return close_and_fail(ctx, &it);
    }
    if (get_iterator_direct(ctx, this_val, &it)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 3);                            /* the accumulator, the value, the result */
    double counter;
    if (argc >= 2) {
        r[0] = argv[1];
        counter = 0;
    } else {
        int s = kz_iter_step(ctx, &it, &r[0]);
        if (s < 0) return JS_EXCEPTION;
        if (s == 0) return kz_throw_type(ctx, "Reduce of empty iterator with no initial value");
        counter = 1;
    }
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[1]);
        if (s < 0) return JS_EXCEPTION;
        if (s == 0) return r[0];
        JSValue args[3] = { r[0], r[1], jv_number(counter) };
        r[2] = kz_call(ctx, fn, JS_UNDEFINED, 3, args);
        if (r[2] == JS_EXCEPTION) return close_and_fail(ctx, &it);
        r[0] = r[2];
        counter += 1;
    }
}

static JSValue iterator_to_array(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Iterator.prototype.toArray called on a non-object");
    KZ_ROOT_ITER(ctx, it);
    if (get_iterator_direct(ctx, this_val, &it)) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_array(ctx, 0);
    if (r[0] == JS_EXCEPTION) return r[0];
    for (;;) {
        int s = kz_iter_step(ctx, &it, &r[1]);
        if (s < 0) return JS_EXCEPTION;
        if (s == 0) return r[0];
        if (kz_array_push(ctx, JV_OBJ(r[0]), r[1])) return JS_EXCEPTION;
    }
}

/* ================================================================== setup */

static const KzFuncDef iterator_static_funcs[] = {
    KZ_FN("from", iterator_from, 1),
};

static const KzFuncDef iterator_proto_helper_funcs[] = {
    KZ_FN_MAGIC("map", iterator_lazy, 1, HK_MAP),
    KZ_FN_MAGIC("filter", iterator_lazy, 1, HK_FILTER),
    KZ_FN_MAGIC("take", iterator_lazy, 1, HK_TAKE),
    KZ_FN_MAGIC("drop", iterator_lazy, 1, HK_DROP),
    KZ_FN_MAGIC("flatMap", iterator_lazy, 1, HK_FLATMAP),
    KZ_FN("reduce", iterator_reduce, 1),
    KZ_FN("toArray", iterator_to_array, 0),
    KZ_FN_MAGIC("forEach", iterator_eager, 1, EK_FOREACH),
    KZ_FN_MAGIC("some", iterator_eager, 1, EK_SOME),
    KZ_FN_MAGIC("every", iterator_eager, 1, EK_EVERY),
    KZ_FN_MAGIC("find", iterator_eager, 1, EK_FIND),
    KZ_ACCESSOR("constructor", proto_constructor_get, proto_constructor_set),
    KZ_ACCESSOR("[Symbol.toStringTag]", proto_tag_get, proto_tag_set),
};

static const KzFuncDef helper_proto_funcs[] = {
    KZ_FN("next", helper_next, 0),
    KZ_FN("return", helper_return, 0),
};

static const KzFuncDef wrap_proto_funcs[] = {
    KZ_FN("next", wrap_next, 0),
    KZ_FN("return", wrap_return, 0),
};

int kz_init_iterator_helpers(JSContext *ctx) {
    KZ_ROOTS(ctx, r, 3);
    /* the constructor, linked to %Iterator.prototype% (whose `constructor` is an accessor, defined next) */
    r[0] = kz_make_ctor(ctx, iterator_ctor, "Iterator", 0, 0, KZ_I_ITERATOR_PROTO, KZ_I_ITERATOR);
    if (r[0] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[0], iterator_static_funcs, KZ_COUNTOF(iterator_static_funcs))) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Iterator, r[0], KZ_PROP_W | KZ_PROP_C)) return -1;
    JSValue proto = kz_intr(ctx, KZ_I_ITERATOR_PROTO);
    if (kz_delete_property(ctx, JV_OBJ(proto), ctx->rt->atom[KZ_ATOM_constructor], false) <= 0) return -1;
    if (kz_define_functions(ctx, proto, iterator_proto_helper_funcs, KZ_COUNTOF(iterator_proto_helper_funcs))) return -1;
    /* %IteratorHelperPrototype% */
    r[1] = kz_new_object_proto(ctx, proto);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_ITERATOR_HELPER_PROTO] = r[1];
    if (kz_define_functions(ctx, r[1], helper_proto_funcs, KZ_COUNTOF(helper_proto_funcs))) return -1;
    r[2] = kz_atom(ctx, "Iterator Helper");
    if (r[2] == JS_EXCEPTION || kz_define_value(ctx, r[1], ctx->rt->sym[KZ_SYM_toStringTag], r[2], KZ_PROP_C)) return -1;
    /* %WrapForValidIteratorPrototype% */
    r[1] = kz_new_object_proto(ctx, proto);
    if (r[1] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_WRAP_FOR_VALID_ITERATOR_PROTO] = r[1];
    if (kz_define_functions(ctx, r[1], wrap_proto_funcs, KZ_COUNTOF(wrap_proto_funcs))) return -1;
    return 0;
}
