/*
 * vm.c: the bytecode interpreter.
 *
 * Calls between bytecode functions do not recurse in C: OP_call pushes the callee's frame onto the runtime's value
 * stack and the same loop goes on running it; OP_return pops it.  Natives, getters, proxies and the like are called
 * through kz_call, and when they call back into JavaScript a new kz_run starts (the C stack is checked against a
 * limit there, so deep recursion through natives ends in a RangeError, not a crash).  Generator and async frames
 * live in heap blocks and are run by kz_run as the entry frame; they suspend by returning JS_EMPTY with
 * rt->suspend_kind set.
 *
 * Exceptions: an operation that fails leaves the exception pending in the runtime and jumps to `exception`, which
 * looks the pc up in the frame's exception table (innermost range first), restores the operand stack depth recorded
 * there, pushes the exception and continues at the handler; without a handler the frame is popped and the search
 * continues in the caller.  An uncatchable error (an interrupt) skips every handler.
 *
 * Property access goes through monomorphic inline caches keyed by shape (and the shape's version for dictionary
 * shapes): own properties, and properties of the direct prototype.
 */
#include "jsint.h"
#include "bytecode.h"

#define INTERRUPT_EVERY 10000

/* ================================================================== the C stack */

int kz_check_stack(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    if ((uintptr_t)__builtin_frame_address(0) < rt->c_stack_limit) {
        if (!kz_has_exception(ctx) || ctx->rt->exception != ctx->rt->oom_error) kz_throw_range(ctx, "Maximum call stack size exceeded");
        return -1;
    }
    return 0;
}

void kz_poll_interrupt(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    rt->interrupt_count = INTERRUPT_EVERY;
    if (rt->interrupt && rt->interrupt(rt, rt->interrupt_opaque)) {
        kz_throw_error(ctx, KZ_ERR_ERROR, "interrupted");
        rt->exc_uncatchable = true;
    }
}

/* ================================================================== frames */

static JSValue stack_overflow(JSContext *ctx) { return kz_throw_range(ctx, "Maximum call stack size exceeded"); }

static void init_regs(JSFrame *f, JSFunctionBytecode *b, int argc, const JSValue *argv) {
    uint32_t n = f->nregs;
    JSValue *r = f->regs;
    for (uint32_t i = 0; i < n; i++) r[i] = JS_UNDEFINED;
    uint32_t np = b->nparams < (uint32_t)argc ? b->nparams : (uint32_t)argc;
    for (uint32_t i = 0; i < np; i++) r[i] = argv[i];
}

/* a frame on the value stack (NULL + RangeError when it does not fit) */
static JSFrame *push_frame(JSContext *ctx, JSObject *func, JSValue this_val, JSValue new_target, int argc,
                           JSValue *argv, uint32_t flags) {
    JSRuntime *rt = ctx->rt;
    JSFunctionBytecode *b = func->u.func.b;
    uint32_t nregs = (uint32_t)b->nlocals + b->stack_size;
    size_t need = sizeof(JSFrame) + (size_t)nregs * sizeof(JSValue);
    if ((size_t)(rt->vstack_end - rt->vstack_top) < need || rt->depth >= rt->max_depth) { stack_overflow(ctx); return NULL; }
    JSFrame *f = (JSFrame *)rt->vstack_top;
    rt->vstack_top += need;
    f->prev = rt->frame;
    f->b = b;
    f->func = func;
    f->pc = b->code;
    f->regs = (JSValue *)(f + 1);
    f->nregs = nregs;
    f->sp = f->regs + b->nlocals;
    f->this_val = this_val;
    f->new_target = new_target;
    f->argv = argv;
    f->argc = (uint32_t)argc;
    f->flags = flags;
    f->cells = func->u.func.cells;
    init_regs(f, b, argc, argv);
    rt->frame = f;
    rt->depth++;
    return f;
}

static void pop_frame(JSRuntime *rt, JSFrame *f) {
    rt->frame = f->prev;
    rt->vstack_top = (uint8_t *)f;
    rt->depth--;
}

/* A heap frame for generators and async functions: the arguments are copied in, since the caller's stack goes. */
static JSFrame *new_heap_frame(JSContext *ctx, JSObject *func, JSValue this_val, JSValue new_target, int argc, JSValue *argv) {
    JSFunctionBytecode *b = func->u.func.b;
    uint32_t nregs = (uint32_t)b->nlocals + b->stack_size;
    size_t size = sizeof(JSFrame) + ((size_t)nregs + (size_t)argc) * sizeof(JSValue);
    JSFrame *f = kz_malloc(ctx, size);
    if (!f) return NULL;
    memset(f, 0, sizeof(JSFrame));
    f->b = b;
    f->func = func;
    f->pc = b->code;
    f->regs = (JSValue *)(f + 1);
    f->nregs = nregs;
    f->sp = f->regs + b->nlocals;
    f->this_val = this_val;
    f->new_target = new_target;
    f->argv = f->regs + nregs;
    for (int i = 0; i < argc; i++) f->argv[i] = argv[i];
    f->argc = (uint32_t)argc;
    f->flags = FRAME_HEAP | FRAME_ENTRY;
    f->cells = func->u.func.cells;
    init_regs(f, b, argc, argv);
    return f;
}

void kz_free_frame(JSRuntime *rt, JSFrame *f) { kz_free(rt, f); }

static JSValue coerce_this(JSContext *ctx, JSFunctionBytecode *b, JSValue this_val);

/* the heap frame of a generator or async function call (this coerced as for a call) */
JSFrame *kz_new_gen_frame(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv) {
    KZ_ROOT(ctx, tv);
    tv = coerce_this(ctx, func->u.func.b, this_val);
    if (tv == JS_EXCEPTION) return NULL;
    return new_heap_frame(ctx, func, tv, JS_UNDEFINED, argc, argv);
}

void kz_trace_frame(JSRuntime *rt, JSFrame *f) {
    kz_mark_cell(rt, f->b);
    kz_mark_cell(rt, f->func);
    kz_mark_value(rt, f->this_val);
    kz_mark_value(rt, f->new_target);
    if (f->argv) for (uint32_t i = 0; i < f->argc; i++) kz_mark_value(rt, f->argv[i]);
    for (uint32_t i = 0; i < f->nregs; i++) kz_mark_value(rt, f->regs[i]);
}

/* the frame's position for messages and stack traces */
int kz_line_of_pc(JSFunctionBytecode *b, uint32_t pc, uint32_t *line, uint32_t *col) {
    *line = b->line;
    *col = b->col;
    if (!b->npos) return 0;
    uint32_t lo = 0, hi = b->npos;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (b->pos[mid].pc <= pc) lo = mid; else hi = mid;
    }
    if (b->pos[lo].pc > pc) return 0;
    *line = b->pos[lo].line;
    *col = b->pos[lo].col;
    return 1;
}

/* ================================================================== closures */

JSVarCell *kz_new_cell(JSContext *ctx, JSValue v) {
    KZ_ROOT(ctx, rv);
    rv = v;
    JSVarCell *c = kz_gc_new(ctx, KZ_GC_VARCELL, sizeof(JSVarCell));
    if (!c) return NULL;
    c->value = rv;
    return c;
}

static JSShape *function_shape(JSContext *ctx, JSFunctionBytecode *b) {
    return (b->h.flags & BC_CONSTRUCTOR) && kz_bc_kind(b) == KZ_FUNC_NORMAL ? ctx->shape_func_proto : ctx->shape_func;
}

/* A closure of b; its captured cells come from `parent`'s frame (locals holding cells, or the parent's own cells). */
JSValue kz_new_closure(JSContext *ctx, JSFunctionBytecode *b, JSFrame *parent, JSValue home) {
    KZ_ROOTS(ctx, r, 4);
    r[0] = JV_MKPTR(b);
    r[1] = home;
    JSVarCell **cells = NULL;
    if (b->ncvars) {
        cells = kz_mallocz(ctx, b->ncvars * sizeof(JSVarCell *));
        if (!cells) return JS_EXCEPTION;
        for (uint32_t i = 0; i < b->ncvars; i++) {
            KzClosureVar *cv = &b->cvars[i];
            JSVarCell *c;
            if (cv->from_local) {
                JSValue v = parent->regs[cv->index];
                c = JV_IS_INTERNAL(v) ? JV_PTR(v) : NULL;
                if (!c) {
                    /* (a slot the compiler expected to hold a cell does not: keep the closure safe) */
                    c = kz_new_cell(ctx, v);
                    if (!c) { kz_free(ctx->rt, cells); return JS_EXCEPTION; }
                    parent->regs[cv->index] = JV_MKPTR(c);
                }
            } else c = parent->cells ? parent->cells[cv->index] : NULL;
            cells[i] = c;
        }
    }
    int kind = kz_bc_kind(b);
    KzIntrinsic proto_id = KZ_I_FUNCTION_PROTO;
    if (b->h.flags & BC_GENERATOR) proto_id = (b->h.flags & BC_ASYNC) ? KZ_I_ASYNC_GENERATOR_FUNCTION_PROTO : KZ_I_GENERATOR_FUNCTION_PROTO;
    else if (b->h.flags & BC_ASYNC) proto_id = KZ_I_ASYNC_FUNCTION_PROTO;
    JSShape *shape;
    if (proto_id == KZ_I_FUNCTION_PROTO) shape = function_shape(ctx, b);
    else {
        /* generator functions have a prototype property too (not a constructor) */
        JSValue keys[3] = { ctx->rt->atom[KZ_ATOM_length], ctx->rt->atom[KZ_ATOM_name], ctx->rt->atom[KZ_ATOM_prototype] };
        int nk = (b->h.flags & BC_GENERATOR) ? 3 : 2;
        shape = kz_shape_build(ctx, kz_intr(ctx, proto_id), keys, nk, KZ_PROP_C);
        if (!shape) { kz_free(ctx->rt, cells); return JS_EXCEPTION; }
        r[3] = JV_MKPTR(shape);
    }
    JSObject *f = kz_obj_alloc(ctx, KZ_CLASS_FUNCTION, shape, 0);
    if (!f) { kz_free(ctx->rt, cells); return JS_EXCEPTION; }
    b = (JSFunctionBytecode *)JV_PTR(r[0]);
    f->u.func.b = b;
    f->u.func.cells = cells;
    f->u.func.home = r[1];
    f->u.func.fields = JS_UNDEFINED;
    f->u.func.realm = ctx;
    f->h.flags |= OBJ_CALLABLE;
    if ((b->h.flags & BC_CONSTRUCTOR) && !(b->h.flags & (BC_GENERATOR | BC_ASYNC))) f->h.flags |= OBJ_CONSTRUCTOR;
    if (kind == KZ_FUNC_CLASS_CTOR || kind == KZ_FUNC_DERIVED_CTOR) f->h.flags |= OBJ_CLASS_CTOR;
    /* slots: length, name (non-writable, configurable) [, prototype] */
    f->slots[0] = JV_MKINT(b->length);
    f->slots[1] = b->name;
    r[2] = JV_MKOBJ(f);
    if (shape->nprops >= 3) {
        /* the prototype object */
        JSValue proto;
        if (b->h.flags & BC_GENERATOR) {
            proto = kz_new_object_proto(ctx, kz_intr(ctx, (b->h.flags & BC_ASYNC) ? KZ_I_ASYNC_GENERATOR_PROTO : KZ_I_GENERATOR_PROTO));
            if (proto == JS_EXCEPTION) return proto;
        } else {
            proto = kz_new_object(ctx);
            if (proto == JS_EXCEPTION) return proto;
            KZ_ROOT(ctx, rp);
            rp = proto;
            if (kz_obj_add_prop(ctx, JV_OBJ(rp), ctx->rt->atom[KZ_ATOM_constructor], KZ_PROP_W | KZ_PROP_C, r[2])) return JS_EXCEPTION;
            proto = rp;
        }
        JV_OBJ(r[2])->slots[2] = proto;
    }
    return r[2];
}

/* ================================================================== calls */

JSContext *kz_function_realm(JSContext *ctx, JSObject *f) {
    for (int depth = 0; depth < 10000; depth++) {
        switch (kz_class(f)) {
        case KZ_CLASS_FUNCTION: return f->u.func.realm;
        case KZ_CLASS_NATIVE: return f->u.native.realm ? f->u.native.realm : ctx;
        case KZ_CLASS_BOUND:
            if (!JV_IS_OBJECT(f->u.bound.target)) return ctx;
            f = JV_OBJ(f->u.bound.target);
            continue;
        case KZ_CLASS_PROXY:
            if (f->u.proxy.handler == JS_NULL) { kz_throw_type(ctx, "Cannot perform operation on a revoked proxy"); return NULL; }
            f = JV_OBJ(f->u.proxy.target);
            continue;
        default:
            return ctx;
        }
    }
    return ctx;
}

JSValue kz_proxy_call(JSContext *ctx, JSObject *p, JSValue this_val, int argc, JSValue *argv);
JSValue kz_proxy_construct(JSContext *ctx, JSObject *p, int argc, JSValue *argv, JSValue new_target);
JSValue kz_generator_call(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv);
JSValue kz_async_call(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv);

static JSValue call_native(JSContext *ctx, JSObject *f, JSValue this_val, int argc, JSValue *argv, JSValue new_target) {
    JSRuntime *rt = ctx->rt;
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    if (rt->depth >= rt->max_depth) return stack_overflow(ctx);
    JSValue pad[16];
    JSValue *args = argv;
    /* every native may read argv[0..2] (and up to its declared nargs) without checking argc */
    int need = f->u.native.nargs < 3 ? 3 : f->u.native.nargs;
    JSValue *heap_args = NULL;
    if (argc < need) {
        if (need <= 16) args = pad;
        else {
            heap_args = kz_malloc(ctx, (size_t)need * sizeof(JSValue));
            if (!heap_args) return JS_EXCEPTION;
            args = heap_args;
        }
        for (int i = 0; i < argc; i++) args[i] = argv[i];
        for (int i = argc; i < need; i++) args[i] = JS_UNDEFINED;
    }
    JSObject *saved_native = rt->cur_native;
    JSValue saved_nt = rt->new_target;
    rt->cur_native = f;
    rt->new_target = new_target;
    rt->depth++;
    JSContext *realm = f->u.native.realm ? f->u.native.realm : ctx;
    JSValue r = f->u.native.fn(realm, this_val, argc, args, f->u.native.magic);
    rt->depth--;
    rt->cur_native = saved_native;
    rt->new_target = saved_nt;
    if (heap_args) kz_free(rt, heap_args);
    return r;
}

static JSValue call_bound(JSContext *ctx, JSObject *f, JSValue this_val, int argc, JSValue *argv, JSValue new_target, bool construct) {
    uint32_t n = f->u.bound.argc + (uint32_t)argc;
    if (n > 65535) return kz_throw_range(ctx, "Too many arguments");
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    JSValue small[16];
    JSValue *args = n <= 16 ? small : kz_malloc(ctx, n * sizeof(JSValue));
    if (!args) return JS_EXCEPTION;
    for (uint32_t i = 0; i < f->u.bound.argc; i++) args[i] = f->u.bound.args[i];
    for (int i = 0; i < argc; i++) args[f->u.bound.argc + (uint32_t)i] = argv[i];
    /* the copies are rooted by the bound function and the caller: but a callee may drop the caller's values; root */
    KzRootFrame rf = { ctx->rt->roots, ctx->rt, args, n, NULL };
    ctx->rt->roots = &rf;
    JSValue r;
    if (construct) {
        JSValue nt = new_target == JV_MKOBJ(f) ? f->u.bound.target : new_target;
        r = kz_construct(ctx, f->u.bound.target, (int)n, args, nt);
    } else r = kz_call(ctx, f->u.bound.target, f->u.bound.this_val, (int)n, args);
    ctx->rt->roots = rf.prev;
    (void)this_val;
    if (args != small) kz_free(ctx->rt, args);
    return r;
}

static JSValue call_host(JSContext *ctx, JSObject *f, JSValue this_val, int argc, JSValue *argv, JSValue new_target, bool construct);

static JSValue coerce_this(JSContext *ctx, JSFunctionBytecode *b, JSValue this_val) {
    if (b->h.flags & BC_STRICT) return this_val;
    int kind = kz_bc_kind(b);
    if (kind == KZ_FUNC_ARROW) return this_val;
    if (JV_IS_OBJECT(this_val)) return this_val;
    if (JV_IS_NULLISH(this_val)) return b->realm->global;
    return kz_to_object(ctx, this_val);
}

JSValue kz_call(JSContext *ctx, JSValue fv, JSValue this_val, int argc, JSValue *argv) {
    if (!JV_IS_OBJECT(fv)) return kz_throw_not_function(ctx, fv);
    JSObject *f = JV_OBJ(fv);
    switch (kz_class(f)) {
    case KZ_CLASS_FUNCTION: {
        JSFunctionBytecode *b = f->u.func.b;
        if (f->h.flags & OBJ_CLASS_CTOR) {
            KZ_ROOT(ctx, n);
            n = b->name;
            char buf[80] = "";
            if (JV_IS_STRING(n)) kz_str_to_utf8_buf(JV_STR(n), buf, sizeof buf);
            return kz_throw_type(ctx, "Class constructor %s cannot be invoked without 'new'", buf);
        }
        if (kz_check_stack(ctx)) return JS_EXCEPTION;
        if (b->h.flags & BC_GENERATOR) return kz_generator_call(ctx, f, this_val, argc, argv);
        if (b->h.flags & BC_ASYNC) return kz_async_call(ctx, f, this_val, argc, argv);
        KZ_ROOT(ctx, tv);
        tv = coerce_this(ctx, b, this_val);
        if (tv == JS_EXCEPTION) return tv;
        JSFrame *fr = push_frame(ctx, f, tv, JS_UNDEFINED, argc, argv, FRAME_ENTRY);
        if (!fr) return JS_EXCEPTION;
        return kz_run(ctx, fr);
    }
    case KZ_CLASS_NATIVE:
        return call_native(ctx, f, this_val, argc, argv, JS_UNDEFINED);
    case KZ_CLASS_BOUND:
        return call_bound(ctx, f, this_val, argc, argv, JS_UNDEFINED, false);
    case KZ_CLASS_PROXY:
        if (!(f->h.flags & OBJ_CALLABLE)) break;
        if (kz_check_stack(ctx)) return JS_EXCEPTION;
        return kz_proxy_call(ctx, f, this_val, argc, argv);
    case KZ_CLASS_HOST:
        if (f->u.host.def && f->u.host.def->call) return call_host(ctx, f, this_val, argc, argv, JS_UNDEFINED, false);
        break;
    default:
        break;
    }
    return kz_throw_not_function(ctx, fv);
}

/* runs a bytecode function (eval code) with an explicit this and new.target */
JSValue kz_call_with_new_target(JSContext *ctx, JSValue fv, JSValue this_val, JSValue new_target);
JSValue kz_call_with_new_target(JSContext *ctx, JSValue fv, JSValue this_val, JSValue new_target) {
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    JSFrame *fr = push_frame(ctx, JV_OBJ(fv), this_val, new_target, 0, NULL, FRAME_ENTRY);
    if (!fr) return JS_EXCEPTION;
    return kz_run(ctx, fr);
}

/* [[Construct]] for a bytecode constructor: the this object (base) or nothing yet (derived) */
static JSValue construct_this(JSContext *ctx, JSObject *f, JSValue new_target) {
    if (kz_bc_kind(f->u.func.b) == KZ_FUNC_DERIVED_CTOR) return JS_UNINIT;
    return kz_ordinary_create_from_ctor(ctx, new_target, KZ_I_OBJECT_PROTO, KZ_CLASS_OBJECT);
}

JSValue kz_construct(JSContext *ctx, JSValue fv, int argc, JSValue *argv, JSValue new_target) {
    if (!kz_is_constructor(fv)) {
        char buf[64];
        kz_format_value_for_msg(ctx, fv, buf, sizeof buf);
        return kz_throw_type(ctx, "%s is not a constructor", buf);
    }
    JSObject *f = JV_OBJ(fv);
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    switch (kz_class(f)) {
    case KZ_CLASS_FUNCTION: {
        KZ_ROOT(ctx, th);
        th = construct_this(ctx, f, new_target);
        if (th == JS_EXCEPTION) return th;
        JSFrame *fr = push_frame(ctx, f, th, new_target, argc, argv, FRAME_ENTRY | FRAME_CONSTRUCT);
        if (!fr) return JS_EXCEPTION;
        return kz_run(ctx, fr);
    }
    case KZ_CLASS_NATIVE:
        return call_native(ctx, f, new_target, argc, argv, new_target);
    case KZ_CLASS_BOUND:
        return call_bound(ctx, f, JS_UNDEFINED, argc, argv, new_target, true);
    case KZ_CLASS_PROXY:
        return kz_proxy_construct(ctx, f, argc, argv, new_target);
    case KZ_CLASS_HOST:
        return call_host(ctx, f, JS_UNDEFINED, argc, argv, new_target, true);
    default:
        return kz_throw_type(ctx, "not a constructor");
    }
}

static JSValue call_host(JSContext *ctx, JSObject *f, JSValue this_val, int argc, JSValue *argv, JSValue new_target, bool construct) {
    const JSClassDef *def = f->u.host.def;
    extern int js__scope_open_internal(JSContext *ctx);
    extern void js__scope_close_internal(JSContext *ctx, int mark);
    int mark = js__scope_open_internal(ctx);
    JSValue r;
    if (construct) r = def->construct ? def->construct(ctx, JV_MKOBJ(f), f->u.host.opaque, argc, argv, new_target) : kz_throw_type(ctx, "not a constructor");
    else r = def->call(ctx, JV_MKOBJ(f), f->u.host.opaque, this_val, argc, argv);
    KZ_ROOT(ctx, rr);
    rr = r;
    js__scope_close_internal(ctx, mark);
    return rr;
}

/* ================================================================== property inline caches */

static inline bool ic_hit(const KzIC *ic, const JSObject *o) {
    return o->shape == ic->shape && o->shape->version == ic->version;
}

/* get with cache update: obj is any value */
static JSValue ic_get(JSContext *ctx, JSValue obj, JSValue key, KzIC *ic) {
    if (!JV_IS_OBJECT(obj)) return kz_get(ctx, obj, key, obj);
    JSObject *o = JV_OBJ(obj), *start = o;
    for (int depth = 0; depth < 3; depth++) {
        if (o->h.flags & OBJ_EXOTIC) break;
        if (kz_class(o) == KZ_CLASS_ARRAY && key == ctx->rt->atom[KZ_ATOM_length]) return depth == 0 ? jv_uint32(o->u.array.length) : kz_get(ctx, obj, key, obj);
        int i = kz_shape_find(o->shape, key);
        if (i >= 0) {
            uint32_t a = o->shape->props[i].attrs;
            if (depth <= 1 && ic) {
                ic->shape = start->shape;
                ic->version = start->shape->version;
                ic->slot = (uint32_t)i;
                ic->flags = (a & KZ_PROP_ACCESSOR) ? IC_ACCESSOR : 0;
                if (depth == 0) ic->holder = NULL, ic->holder_shape = NULL;
                else { ic->holder = o; ic->holder_shape = o->shape; ic->holder_version = o->shape->version; }
            }
            if (a & KZ_PROP_ACCESSOR) {
                JSValue g = ((JSAccessor *)JV_PTR(o->slots[i]))->getter;
                if (!JV_IS_OBJECT(g)) return JS_UNDEFINED;
                return kz_call(ctx, g, obj, 0, NULL);
            }
            return o->slots[i];
        }
        JSValue p = o->shape->proto;
        if (!JV_IS_OBJECT(p)) return JS_UNDEFINED;
        o = JV_OBJ(p);
    }
    return kz_get(ctx, obj, key, obj);
}

static inline JSValue ic_cached_get(JSContext *ctx, JSObject *o, JSValue obj, KzIC *ic, bool *hit) {
    JSObject *h = o;
    if (ic->holder) {
        h = ic->holder;
        if (h->shape != ic->holder_shape || h->shape->version != ic->holder_version) { *hit = false; return JS_UNDEFINED; }
    }
    *hit = true;
    JSValue v = h->slots[ic->slot];
    if (ic->flags & IC_ACCESSOR) {
        JSValue g = ((JSAccessor *)JV_PTR(v))->getter;
        if (!JV_IS_OBJECT(g)) return JS_UNDEFINED;
        return kz_call(ctx, g, obj, 0, NULL);
    }
    return v;
}

/* set with cache update for existing writable own data properties */
static int ic_put(JSContext *ctx, JSValue obj, JSValue key, JSValue val, KzIC *ic, bool strict) {
    if (JV_IS_OBJECT(obj)) {
        JSObject *o = JV_OBJ(obj);
        if (!(o->h.flags & OBJ_EXOTIC)) {
            int i = kz_shape_find(o->shape, key);
            if (i >= 0 && (o->shape->props[i].attrs & (KZ_PROP_W | KZ_PROP_ACCESSOR)) == KZ_PROP_W) {
                o->slots[i] = val;
                ic->shape = o->shape;
                ic->version = o->shape->version;
                ic->slot = (uint32_t)i;
                ic->holder = NULL;
                ic->flags = 0;
                return 0;
            }
        }
    }
    return kz_set(ctx, obj, key, val, obj, strict) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ globals */

static JSValue name_str(JSContext *ctx, JSValue atom, char *buf, size_t cap) {
    if (JV_IS_STRING(atom)) kz_str_to_utf8_buf(JV_STR(atom), buf, cap);
    else buf[0] = 0;
    (void)ctx;
    return atom;
}

/* the global lexical binding cell for name, or NULL */
static JSVarCell *global_lex_cell(JSContext *ctx, JSValue name) {
    JSObject *lex = JV_OBJ(ctx->global_lex);
    int i = kz_shape_find(lex->shape, name);
    if (i < 0) return NULL;
    return (JSVarCell *)JV_PTR(lex->slots[i]);
}

#define IC_LEXCELL 2

static JSValue get_global(JSContext *ctx, JSValue name, KzIC *ic, bool for_typeof) {
    JSVarCell *c = global_lex_cell(ctx, name);
    if (c) {
        if (c->value == JS_UNINIT) {
            char buf[80];
            name_str(ctx, name, buf, sizeof buf);
            return kz_throw_reference(ctx, "Cannot access '%s' before initialization", buf);
        }
        ic->holder = (JSObject *)c;
        ic->shape = NULL;
        ic->flags = IC_LEXCELL;
        return c->value;
    }
    JSValue g = ctx->global;
    JSObject *go = JV_OBJ(g);
    int i = kz_shape_find(go->shape, name);
    if (i >= 0 && !(go->h.flags & OBJ_EXOTIC)) {
        uint32_t a = go->shape->props[i].attrs;
        ic->shape = go->shape;
        ic->version = go->shape->version;
        ic->slot = (uint32_t)i;
        ic->holder = NULL;
        ic->flags = (a & KZ_PROP_ACCESSOR) ? IC_ACCESSOR : 0;
        ic->holder_version = ctx->global_epoch;
        if (a & KZ_PROP_ACCESSOR) {
            JSValue gt = ((JSAccessor *)JV_PTR(go->slots[i]))->getter;
            if (!JV_IS_OBJECT(gt)) return JS_UNDEFINED;
            return kz_call(ctx, gt, g, 0, NULL);
        }
        return go->slots[i];
    }
    int has = kz_has_property(ctx, go, name);
    if (has < 0) return JS_EXCEPTION;
    if (!has) {
        if (for_typeof) return JS_UNDEFINED;
        char buf[80];
        name_str(ctx, name, buf, sizeof buf);
        return kz_throw_reference(ctx, "%s is not defined", buf);
    }
    return kz_get(ctx, g, name, g);
}

static inline bool global_ic_hit(JSContext *ctx, KzIC *ic) {
    if (ic->flags & IC_LEXCELL) return true;
    JSObject *go = JV_OBJ(ctx->global);
    return ic->shape && go->shape == ic->shape && go->shape->version == ic->version && ic->holder_version == ctx->global_epoch;
}

static int put_global(JSContext *ctx, JSValue name, JSValue val, KzIC *ic, bool strict, bool init) {
    JSVarCell *c = global_lex_cell(ctx, name);
    if (c) {
        if (!init && c->value == JS_UNINIT) {
            char buf[80];
            name_str(ctx, name, buf, sizeof buf);
            kz_throw_reference(ctx, "Cannot access '%s' before initialization", buf);
            return -1;
        }
        if (!init) {
            /* const? the lex object records constness in the property's writable bit */
            JSObject *lex = JV_OBJ(ctx->global_lex);
            int i = kz_shape_find(lex->shape, name);
            if (i >= 0 && !(lex->shape->props[i].attrs & KZ_PROP_W)) { kz_throw_type(ctx, "Assignment to constant variable."); return -1; }
            ic->holder = (JSObject *)c;
            ic->flags = IC_LEXCELL;
            ic->shape = NULL;
        }
        c->value = val;
        return 0;
    }
    JSObject *go = JV_OBJ(ctx->global);
    int i = kz_shape_find(go->shape, name);
    if (i >= 0 && !(go->h.flags & OBJ_EXOTIC) && (go->shape->props[i].attrs & (KZ_PROP_W | KZ_PROP_ACCESSOR)) == KZ_PROP_W) {
        go->slots[i] = val;
        ic->shape = go->shape;
        ic->version = go->shape->version;
        ic->slot = (uint32_t)i;
        ic->holder = NULL;
        ic->flags = 0;
        ic->holder_version = ctx->global_epoch;
        return 0;
    }
    if (strict && !init) {
        int has = kz_has_property(ctx, go, name);
        if (has < 0) return -1;
        if (!has) {
            char buf[80];
            name_str(ctx, name, buf, sizeof buf);
            kz_throw_reference(ctx, "%s is not defined", buf);
            return -1;
        }
    }
    return kz_set(ctx, ctx->global, name, val, ctx->global, strict) < 0 ? -1 : 0;
}

/* GlobalDeclarationInstantiation (and EvalDeclarationInstantiation for global eval code):
   list = [-1, eval?, kind, name, ...] */
static int global_decls(JSContext *ctx, JSValue list) {
    JSObject *l = JV_OBJ(list);
    JSObject *go = JV_OBJ(ctx->global);
    JSObject *lex = JV_OBJ(ctx->global_lex);
    uint32_t n = l->elem_len;
    bool eval_code = l->elems[1] == JS_TRUE;
    /* checks first: a lexical name may not exist as a var or lexical yet, nor be a non-configurable global */
    for (uint32_t i = 2; i + 1 < n; i += 2) {
        int kind = JV_INT(l->elems[i]);
        JSValue name = l->elems[i + 1];
        char buf[80];
        if (kind == GD_LET || kind == GD_CONST || kind == GD_CLASS) {
            if (kz_shape_find(lex->shape, name) >= 0) goto redeclared;
            KZ_ROOT_DESC(ctx, d);
            int h = kz_get_own_property(ctx, go, name, &d);
            if (h < 0) return -1;
            if (h && !(d.flags & KZ_PROP_C)) goto redeclared;
        } else {
            if (kz_shape_find(lex->shape, name) >= 0) goto redeclared;
        }
        continue;
    redeclared:
        name_str(ctx, name, buf, sizeof buf);
        kz_throw_syntax(ctx, "Identifier '%s' has already been declared", buf);
        return -1;
    }
    /* functions must be definable, vars creatable */
    for (uint32_t i = 2; i + 1 < n; i += 2) {
        int kind = JV_INT(l->elems[i]);
        JSValue name = l->elems[i + 1];
        if (kind == GD_FUNC) {
            KZ_ROOT_DESC(ctx, d);
            int h = kz_get_own_property(ctx, go, name, &d);
            if (h < 0) return -1;
            if (!h || (d.flags & KZ_PROP_C)) {
                if (!h && !(go->h.flags & OBJ_EXTENSIBLE)) goto cannot;
                if (kz_define_value(ctx, ctx->global, name, JS_UNDEFINED, eval_code ? KZ_PROP_WEC : (KZ_PROP_W | KZ_PROP_E))) return -1;
            } else if ((d.flags & PD_ACCESSOR) || !(d.flags & KZ_PROP_W) || !(d.flags & KZ_PROP_E)) goto cannot;
        } else if (kind == GD_VAR) {
            int h = kz_has_property(ctx, go, name);
            KZ_ROOT_DESC(ctx, d);
            int own = kz_get_own_property(ctx, go, name, &d);
            if (own < 0 || h < 0) return -1;
            if (!own) {
                if (!(go->h.flags & OBJ_EXTENSIBLE)) goto cannot;
                if (kz_define_value(ctx, ctx->global, name, JS_UNDEFINED, eval_code ? KZ_PROP_WEC : (KZ_PROP_W | KZ_PROP_E))) return -1;
            }
        }
        continue;
    cannot: {
            char buf[80];
            name_str(ctx, name, buf, sizeof buf);
            kz_throw_type(ctx, "Cannot declare global '%s'", buf);
            return -1;
        }
    }
    /* lexical bindings: cells in the TDZ */
    for (uint32_t i = 2; i + 1 < n; i += 2) {
        int kind = JV_INT(l->elems[i]);
        if (kind != GD_LET && kind != GD_CONST && kind != GD_CLASS) continue;
        KZ_ROOTS(ctx, r, 2);
        r[0] = l->elems[i + 1];
        JSVarCell *c = kz_new_cell(ctx, JS_UNINIT);
        if (!c) return -1;
        r[1] = JV_MKPTR(c);                 /* (adding the property may collect: the cell is in no object yet) */
        if (kz_obj_add_prop(ctx, JV_OBJ(ctx->global_lex), r[0], kind == GD_CONST ? KZ_PROP_E : (KZ_PROP_W | KZ_PROP_E), r[1])) return -1;
        ctx->global_epoch++;
    }
    return 0;
}

/* ================================================================== helpers for complex instructions */

static JSValue throw_tdz(JSContext *ctx, JSFunctionBytecode *b, uint32_t slot, bool cell) {
    /* the name from the debug table (eval descriptors) when there is one */
    for (uint32_t i = 0; i < b->nvarnames; i++)
        if (b->varnames[i].slot == slot && b->varnames[i].where != VN_CLOSURE) {
            char buf[80];
            name_str(ctx, b->varnames[i].name, buf, sizeof buf);
            return kz_throw_reference(ctx, "Cannot access '%s' before initialization", buf);
        }
    (void)cell;
    return kz_throw_reference(ctx, "Cannot access a variable before initialization");
}

/* Symbol.unscopables check for with: 1 blocked, 0 not, -1 */
static int unscopable(JSContext *ctx, JSValue obj, JSValue name) {
    JSValue u = kz_get(ctx, obj, ctx->rt->sym[KZ_SYM_unscopables], obj);
    if (u == JS_EXCEPTION) return -1;
    if (!JV_IS_OBJECT(u)) return 0;
    KZ_ROOT(ctx, ru);
    ru = u;
    JSValue v = kz_get(ctx, ru, name, ru);
    if (v == JS_EXCEPTION) return -1;
    return kz_to_bool(v);
}

/* does the dynamic scope object have `name`? */
static int with_has(JSContext *ctx, JSValue obj, JSValue name, int kind) {
    if (!JV_IS_OBJECT(obj)) return 0;
    int h = kz_has_property(ctx, JV_OBJ(obj), name);
    if (h <= 0) return h;
    if (kind == WK_WITH) {
        int u = unscopable(ctx, obj, name);
        if (u < 0) return -1;
        if (u) return 0;
    }
    return 1;
}

static JSValue template_object(JSContext *ctx, JSFunctionBytecode *b, uint32_t ci) {
    JSValue c = b->consts[ci];
    JSObject *pair = JV_OBJ(c);
    if (pair->elem_len == 2 && JV_IS_OBJECT(pair->elems[0]) && kz_class(JV_OBJ(pair->elems[0])) == KZ_CLASS_ARRAY &&
        pair->u.array.length == 2 && JV_IS_OBJECT(pair->elems[1])) {
        KZ_ROOTS(ctx, r, 2);
        r[0] = pair->elems[0];
        r[1] = pair->elems[1];
        /* cooked and raw arrays: freeze both, attach raw (non-enumerable, non-writable, non-configurable) */
        if (kz_define_value(ctx, r[0], ctx->rt->atom[KZ_ATOM_raw], r[1], 0)) return JS_EXCEPTION;
        if (kz_set_integrity(ctx, JV_OBJ(r[1]), true)) return JS_EXCEPTION;
        if (kz_set_integrity(ctx, JV_OBJ(r[0]), true)) return JS_EXCEPTION;
        b->consts[ci] = r[0];
        return r[0];
    }
    return c;
}

static int define_method(JSContext *ctx, JSValue obj, JSValue key, JSValue fn, int kind) {
    JSObject *f = JV_OBJ(fn);
    if (kz_class(f) == KZ_CLASS_FUNCTION) f->u.func.home = obj;
    uint32_t enumf = (kind & DM_ENUM) ? KZ_PROP_E : 0;
    int k = kind & 3;
    if (k == DM_METHOD) return kz_define_value(ctx, obj, key, fn, KZ_PROP_W | KZ_PROP_C | enumf);
    /* getters/setters: name gets the prefix */
    if (kz_set_function_name(ctx, fn, key, k == DM_GETTER ? "get" : "set")) return -1;
    KZ_ROOT_DESC(ctx, d);
    d.flags = PD_HAS_E | PD_HAS_C | KZ_PROP_C | enumf | (k == DM_GETTER ? PD_HAS_GET : PD_HAS_SET);
    if (k == DM_GETTER) d.getter = fn; else d.setter = fn;
    return kz_define_own_property(ctx, JV_OBJ(obj), key, &d, true) < 0 ? -1 : 0;
}

/* class field records: [kind, key, value, ...] in the constructor's fields array */
static int add_field_record(JSContext *ctx, JSValue ctor, JSValue key, JSValue val, int fr) {
    JSObject *c = JV_OBJ(ctor);
    if (kz_class(c) != KZ_CLASS_FUNCTION) return 0;
    if (c->u.func.fields == JS_UNDEFINED) {
        JSValue a = kz_new_array(ctx, 0);
        if (a == JS_EXCEPTION) return -1;
        c = JV_OBJ(ctor);
        c->u.func.fields = a;
    }
    JSObject *a = JV_OBJ(c->u.func.fields);
    if (kz_array_push(ctx, a, JV_MKINT(fr)) || kz_array_push(ctx, a, key) || kz_array_push(ctx, a, val)) return -1;
    return 0;
}

int kz_private_get(JSContext *ctx, JSValue obj, JSValue name, JSValue *out);
int kz_private_set(JSContext *ctx, JSValue obj, JSValue name, JSValue val);
int kz_private_define(JSContext *ctx, JSValue obj, JSValue name, JSValue val, int kind);
int kz_private_has(JSValue obj, JSValue name);

/* InitializeInstanceElements: private methods first, then fields in order */
static int run_fields(JSContext *ctx, JSValue func, JSValue this_val) {
    if (!JV_IS_OBJECT(func) || kz_class(JV_OBJ(func)) != KZ_CLASS_FUNCTION) return 0;
    KZ_ROOTS(ctx, r, 4);
    r[0] = JV_OBJ(func)->u.func.fields;
    if (r[0] == JS_UNDEFINED) return 0;
    r[1] = this_val;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = JV_OBJ(r[0])->elem_len;
        for (uint32_t i = 0; i + 2 < n + 1 && i + 2 <= n - 1 + 1; i += 3) {
            JSObject *a = JV_OBJ(r[0]);
            if (i + 2 >= a->elem_len) break;
            int fr = JV_INT(a->elems[i]);
            r[2] = a->elems[i + 1];
            r[3] = a->elems[i + 2];
            if ((pass == 0) != (fr != FR_FIELD)) continue;
            if (fr != FR_FIELD) {
                if (kz_private_define(ctx, r[1], r[2], r[3], fr)) return -1;
                continue;
            }
            JSValue v = JS_UNDEFINED;
            if (r[3] != JS_UNDEFINED) {
                v = kz_call(ctx, r[3], r[1], 0, NULL);
                if (v == JS_EXCEPTION) return -1;
            }
            if (JV_IS_SYMBOL(r[2]) && (JV_SYM(r[2])->h.flags & SYM_PRIVATE)) {
                if (kz_private_define(ctx, r[1], r[2], v, FR_FIELD)) return -1;
            } else {
                KZ_ROOT(ctx, rv);
                rv = v;
                if (kz_create_data_property(ctx, r[1], r[2], rv)) return -1;
            }
        }
    }
    return 0;
}

/* ClassDefinitionEvaluation's steps for the constructor and prototype objects */
static JSValue class_create(JSContext *ctx, JSFrame *f, JSFunctionBytecode *cb, JSValue heritage, bool has_heritage, JSValue *proto_out) {
    KZ_ROOTS(ctx, r, 4);
    JSValue proto_parent = kz_intr(ctx, KZ_I_OBJECT_PROTO), ctor_parent = kz_intr(ctx, KZ_I_FUNCTION_PROTO);
    r[0] = heritage;
    if (has_heritage) {
        if (heritage == JS_NULL) {
            proto_parent = JS_NULL;
        } else {
            if (!kz_is_constructor(heritage)) return kz_throw_type(ctx, "Class extends value is not a constructor or null");
            JSObject *h = JV_OBJ(heritage);
            if (kz_class(h) == KZ_CLASS_FUNCTION && (kz_bc_kind(h->u.func.b) == KZ_FUNC_GETTER)) { }
            JSValue pp = kz_get(ctx, heritage, ctx->rt->atom[KZ_ATOM_prototype], heritage);
            if (pp == JS_EXCEPTION) return pp;
            if (!JV_IS_OBJECT(pp) && pp != JS_NULL) return kz_throw_type(ctx, "Class extends value does not have valid prototype property");
            proto_parent = pp;
            ctor_parent = heritage;
        }
    }
    r[1] = proto_parent;
    r[2] = ctor_parent;
    r[3] = kz_new_object_proto(ctx, r[1]);
    if (r[3] == JS_EXCEPTION) return JS_EXCEPTION;
    JSValue ctor = kz_new_closure(ctx, cb, f, r[3]);
    if (ctor == JS_EXCEPTION) return ctor;
    KZ_ROOT(ctx, rc);
    rc = ctor;
    JSObject *c = JV_OBJ(rc);
    c->h.flags |= OBJ_CONSTRUCTOR | OBJ_CLASS_CTOR;
    /* class constructors: the prototype property is not writable */
    if (kz_obj_set_proto_raw(ctx, c, r[2])) return JS_EXCEPTION;
    if (c->shape->nprops >= 3 && kz_shape_find(c->shape, ctx->rt->atom[KZ_ATOM_prototype]) >= 0) {
        KzPropDesc d = { r[3], JS_UNDEFINED, JS_UNDEFINED, PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C };
        if (kz_define_own_property(ctx, c, ctx->rt->atom[KZ_ATOM_prototype], &d, true) < 0) return JS_EXCEPTION;
    } else if (kz_define_value(ctx, rc, ctx->rt->atom[KZ_ATOM_prototype], r[3], 0)) return JS_EXCEPTION;
    if (kz_define_value(ctx, r[3], ctx->rt->atom[KZ_ATOM_constructor], rc, KZ_PROP_W | KZ_PROP_C)) return JS_EXCEPTION;
    *proto_out = r[3];
    return rc;
}

static JSValue special_object(JSContext *ctx, JSFrame *f, int kind);
JSValue kz_eval_direct(JSContext *ctx, JSFrame *f, JSValue arg, uint16_t first, uint16_t count);

static int copy_data_props(JSContext *ctx, JSValue target, JSValue source, JSValue *excl, int nexcl) {
    if (JV_IS_NULLISH(source)) return 0;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_to_object(ctx, source);
    if (r[0] == JS_EXCEPTION) return -1;
    KZ_ROOT_VEC(ctx, keys);
    JSObject *src = JV_OBJ(r[0]);
    /* fast path: an ordinary source object without accessors */
    if (kz_own_keys(ctx, src, &keys, KZ_KEYS_STRINGS | KZ_KEYS_SYMBOLS)) return -1;
    for (uint32_t i = 0; i < keys.len; i++) {
        JSValue k = keys.v[i];
        bool skip = false;
        for (int e = 0; e < nexcl; e++) {
            /* exclusion keys arrive as property keys (to_propkey) or literal keys */
            JSValue ek = excl[e];
            if (ek == k) { skip = true; break; }
            if (!KZ_KEY_IS_INDEX(ek) && JV_IS_NUMBER(ek)) {
                JSValue kk = kz_key_from_value(ctx, ek);
                if (kk == k) { skip = true; break; }
            }
        }
        if (skip) continue;
        KZ_ROOT_DESC(ctx, d);
        int h = kz_get_own_property(ctx, src, k, &d);
        if (h < 0) return -1;
        if (!h || !(d.flags & KZ_PROP_E)) continue;
        r[1] = kz_get(ctx, r[0], k, r[0]);
        if (r[1] == JS_EXCEPTION) return -1;
        if (kz_create_data_property(ctx, target, k, r[1])) return -1;
    }
    return 0;
}

/* ================================================================== for-in */

/* EnumerateObjectProperties: the keys are collected up front (own then inherited, no duplicates, only enumerable
   string keys), and each is checked again when its turn comes (deleted keys are skipped). */
static JSValue for_in_start(JSContext *ctx, JSValue v) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = JS_UNDEFINED;
    if (!JV_IS_NULLISH(v)) {
        r[0] = kz_to_object(ctx, v);
        if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    }
    r[1] = kz_new_array(ctx, 0);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    if (r[0] != JS_UNDEFINED) {
        KZ_ROOT_VEC(ctx, seen);
        KZ_ROOT_VEC(ctx, keys);
        JSValue o = r[0];
        for (int depth = 0; JV_IS_OBJECT(o) && depth < 100000; depth++) {
            r[2] = o;
            keys.len = 0;
            if (kz_own_keys(ctx, JV_OBJ(o), &keys, KZ_KEYS_STRINGS)) return JS_EXCEPTION;
            for (uint32_t i = 0; i < keys.len; i++) {
                JSValue k = keys.v[i];
                bool dup = false;
                for (uint32_t j = 0; j < seen.len; j++) if (seen.v[j] == k) { dup = true; break; }
                if (dup) continue;
                if (kz_vec_push(ctx, &seen, k)) return JS_EXCEPTION;
                KZ_ROOT_DESC(ctx, d);
                int h = kz_get_own_property(ctx, JV_OBJ(o), k, &d);
                if (h < 0) return JS_EXCEPTION;
                if (!h || !(d.flags & KZ_PROP_E)) continue;
                if (kz_array_push(ctx, JV_OBJ(r[1]), k)) return JS_EXCEPTION;
            }
            JSValue p;
            if (kz_get_proto(ctx, JV_OBJ(o), &p)) return JS_EXCEPTION;
            o = p;
        }
    }
    JSObject *it = kz_obj_alloc(ctx, KZ_CLASS_FOR_IN_ITERATOR, ctx->rt->null_root ? ctx->rt->null_root : kz_root_shape(ctx, JS_NULL), 0);
    if (!it) return JS_EXCEPTION;
    it->u.forin.obj = r[0];
    it->u.forin.keys = r[1];
    it->u.forin.pos = 0;
    return JV_MKOBJ(it);
}

/* the next key (as a string) or JS_EMPTY */
static JSValue for_in_next(JSContext *ctx, JSObject *it) {
    for (;;) {
        JSObject *keys = JV_OBJ(it->u.forin.keys);
        if (it->u.forin.pos >= keys->elem_len) return JS_EMPTY;
        JSValue k = keys->elems[it->u.forin.pos++];
        /* still there? */
        if (JV_IS_OBJECT(it->u.forin.obj)) {
            int h = kz_has_property(ctx, JV_OBJ(it->u.forin.obj), k);
            if (h < 0) return JS_EXCEPTION;
            if (!h) continue;
        }
        return kz_key_to_value(ctx, k);
    }
}

/* ================================================================== the interpreter */

#define DISPATCH() goto *dispatch[*pc]
#define SAVE() do { f->pc = pc; f->sp = sp; } while (0)
#define THROW() goto exception
#define CHECK(v) do { if ((v) == JS_EXCEPTION) THROW(); } while (0)
#define CHECK_I(r) do { if ((r) < 0) THROW(); } while (0)
#define LOAD_FRAME() do { b = f->b; regs = f->regs; consts = b->consts; } while (0)

/* helpers for kz_run: anything that needs a scoped root lives in a normal function, because the dispatch loop's
   computed gotos cannot run cleanups (clang rejects them; gcc would skip them) */
static int vm_get_iterator(JSContext *ctx, JSValue *sp, bool async) {
    KZ_ROOT_ITER(ctx, it);
    if (kz_get_iterator(ctx, sp[-1], &it, async)) return -1;
    sp[-1] = it.iter;
    sp[0] = it.next;
    return 0;
}

/* the internal argument array of a spread call: made dense, holes as undefined (nothing else can see it) */
static int vm_dense_args(JSContext *ctx, JSValue arrv, uint32_t *n) {
    JSObject *arr = JV_OBJ(arrv);
    uint32_t len = arr->u.array.length;
    if (arr->elem_len < len) {
        KZ_ROOT(ctx, ra);
        ra = arrv;
        if (kz_elems_reserve(ctx, JV_OBJ(ra), len)) return -1;
        arr = JV_OBJ(ra);
        for (uint32_t i = arr->elem_len; i < len; i++) arr->elems[i] = JS_UNDEFINED;
        arr->elem_len = len;
    }
    for (uint32_t i = 0; i < len; i++)
        if (arr->elems[i] == JS_HOLE) arr->elems[i] = JS_UNDEFINED;
    *n = len;
    return 0;
}

static int vm_append_iterable(JSContext *ctx, JSValue arr, JSValue iterable) {
    KZ_ROOT_VEC(ctx, vals);
    if (kz_iterate_to_vec(ctx, iterable, &vals)) return -1;
    for (uint32_t i = 0; i < vals.len; i++)
        if (kz_array_push(ctx, JV_OBJ(arr), vals.v[i])) return -1;
    return 0;
}

JSValue kz_run(JSContext *ctx, JSFrame *f) {
    static void *const dispatch[256] = {
#define OP(name, size, pop, push) [OP_##name] = &&L_##name,
#include "opcodes.h"
#undef OP
    };
    JSRuntime *rt = ctx->rt;
    JSFunctionBytecode *b;
    JSValue *regs, *sp, *consts;
    const uint8_t *pc;
    JSValue ret;
    JSFrame *entry = f;
    if (!(f->flags & FRAME_ENTRY)) f->flags |= FRAME_ENTRY;
    if (f->flags & FRAME_HEAP) {
        /* resuming (or starting) a generator/async frame: link it in */
        f->prev = rt->frame;
        rt->frame = f;
        rt->depth++;
    }
    LOAD_FRAME();
    pc = f->pc;
    sp = f->sp;
    if (f->flags & FRAME_RESUME_THROW) {
        f->flags &= ~FRAME_RESUME_THROW;
        THROW();
    }
    if (kz_check_stack(ctx)) THROW();
    DISPATCH();

    /* ------------------------------------------------------------ constants */
L_nop: pc++; DISPATCH();
L_push_undefined: *sp++ = JS_UNDEFINED; pc++; DISPATCH();
L_push_null: *sp++ = JS_NULL; pc++; DISPATCH();
L_push_true: *sp++ = JS_TRUE; pc++; DISPATCH();
L_push_false: *sp++ = JS_FALSE; pc++; DISPATCH();
L_push_i8: *sp++ = JV_MKINT((int8_t)pc[1]); pc += 2; DISPATCH();
L_push_i32: *sp++ = JV_MKINT(rdi32(pc + 1)); pc += 5; DISPATCH();
L_push_const: *sp++ = consts[rd32(pc + 1)]; pc += 5; DISPATCH();
L_push_hole: *sp++ = JS_HOLE; pc++; DISPATCH();
L_push_this: *sp++ = f->this_val; pc++; DISPATCH();
L_push_new_target: *sp++ = f->new_target; pc++; DISPATCH();
L_push_func: *sp++ = JV_MKOBJ(f->func); pc++; DISPATCH();
L_push_home: *sp++ = f->func->u.func.home; pc++; DISPATCH();
L_dup: sp[0] = sp[-1]; sp++; pc++; DISPATCH();
L_dup2: sp[0] = sp[-2]; sp[1] = sp[-1]; sp += 2; pc++; DISPATCH();
L_dup_under: sp[0] = sp[-1]; sp[-1] = sp[-2]; sp++; pc++; DISPATCH();
L_drop: sp--; pc++; DISPATCH();
L_nip: sp[-2] = sp[-1]; sp--; pc++; DISPATCH();
L_swap: { JSValue t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; pc++; DISPATCH(); }
L_rot3l: { JSValue a = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = a; pc++; DISPATCH(); }
L_rot3r: { JSValue c = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = c; pc++; DISPATCH(); }
L_insert2: { JSValue bb = sp[-1]; sp[0] = bb; sp[-1] = sp[-2]; sp[-2] = bb; sp++; pc++; DISPATCH(); }
L_insert3: { JSValue c = sp[-1]; sp[0] = c; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = c; sp++; pc++; DISPATCH(); }
L_insert4: { JSValue d = sp[-1]; sp[0] = d; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = sp[-4]; sp[-4] = d; sp++; pc++; DISPATCH(); }
L_swap2: { JSValue a = sp[-4], bb = sp[-3]; sp[-4] = sp[-2]; sp[-3] = sp[-1]; sp[-2] = a; sp[-1] = bb; pc++; DISPATCH(); }
L_rot4l: { JSValue a = sp[-4]; sp[-4] = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = a; pc++; DISPATCH(); }

    /* ------------------------------------------------------------ locals */
L_get_loc: *sp++ = regs[rd16(pc + 1)]; pc += 3; DISPATCH();
L_put_loc: regs[rd16(pc + 1)] = *--sp; pc += 3; DISPATCH();
L_set_loc: regs[rd16(pc + 1)] = sp[-1]; pc += 3; DISPATCH();
L_get_loc_check: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        if (kz_unlikely(regs[s] == JS_UNINIT)) { SAVE(); throw_tdz(ctx, b, s, false); THROW(); }
        *sp++ = regs[s];
        DISPATCH();
    }
L_put_loc_check: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        if (kz_unlikely(regs[s] == JS_UNINIT)) { SAVE(); throw_tdz(ctx, b, s, false); THROW(); }
        regs[s] = *--sp;
        DISPATCH();
    }
L_set_loc_uninit: regs[rd16(pc + 1)] = JS_UNINIT; pc += 3; DISPATCH();
L_box_loc: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        SAVE();
        JSVarCell *c = kz_new_cell(ctx, regs[s]);
        if (!c) THROW();
        regs[s] = JV_MKPTR(c);
        DISPATCH();
    }
L_get_lcell: *sp++ = ((JSVarCell *)JV_PTR(regs[rd16(pc + 1)]))->value; pc += 3; DISPATCH();
L_put_lcell: ((JSVarCell *)JV_PTR(regs[rd16(pc + 1)]))->value = *--sp; pc += 3; DISPATCH();
L_set_lcell: ((JSVarCell *)JV_PTR(regs[rd16(pc + 1)]))->value = sp[-1]; pc += 3; DISPATCH();
L_init_lcell: ((JSVarCell *)JV_PTR(regs[rd16(pc + 1)]))->value = *--sp; pc += 3; DISPATCH();
L_get_lcell_check: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        JSValue v = ((JSVarCell *)JV_PTR(regs[s]))->value;
        if (kz_unlikely(v == JS_UNINIT)) { SAVE(); throw_tdz(ctx, b, s, true); THROW(); }
        *sp++ = v;
        DISPATCH();
    }
L_put_lcell_check: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        JSVarCell *c = JV_PTR(regs[s]);
        if (kz_unlikely(c->value == JS_UNINIT)) { SAVE(); throw_tdz(ctx, b, s, true); THROW(); }
        c->value = *--sp;
        DISPATCH();
    }
L_new_lcell: L_new_lcell_undef: L_copy_lcell: {
        uint8_t op = *pc;
        uint16_t s = rd16(pc + 1);
        pc += 3;
        SAVE();
        JSValue init = op == OP_new_lcell ? JS_UNINIT : op == OP_new_lcell_undef ? JS_UNDEFINED :
                       (JV_IS_INTERNAL(regs[s]) ? ((JSVarCell *)JV_PTR(regs[s]))->value : regs[s]);
        JSVarCell *c = kz_new_cell(ctx, init);
        if (!c) THROW();
        regs[s] = JV_MKPTR(c);
        DISPATCH();
    }
L_get_cvar: *sp++ = f->cells[rd16(pc + 1)]->value; pc += 3; DISPATCH();
L_put_cvar: f->cells[rd16(pc + 1)]->value = *--sp; pc += 3; DISPATCH();
L_set_cvar: f->cells[rd16(pc + 1)]->value = sp[-1]; pc += 3; DISPATCH();
L_init_cvar: f->cells[rd16(pc + 1)]->value = *--sp; pc += 3; DISPATCH();
L_get_cvar_raw: *sp++ = JV_MKPTR(f->cells[rd16(pc + 1)]); pc += 3; DISPATCH();
L_get_cvar_check: {
        JSValue v = f->cells[rd16(pc + 1)]->value;
        pc += 3;
        if (kz_unlikely(v == JS_UNINIT)) { SAVE(); kz_throw_reference(ctx, "Cannot access a variable before initialization"); THROW(); }
        *sp++ = v;
        DISPATCH();
    }
L_put_cvar_check: {
        JSVarCell *c = f->cells[rd16(pc + 1)];
        pc += 3;
        if (kz_unlikely(c->value == JS_UNINIT)) { SAVE(); kz_throw_reference(ctx, "Cannot access a variable before initialization"); THROW(); }
        c->value = *--sp;
        DISPATCH();
    }
L_throw_const: {
        JSValue name = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        char buf[80];
        name_str(ctx, name, buf, sizeof buf);
        kz_throw_type(ctx, "Assignment to constant variable '%s'.", buf);
        THROW();
    }
L_throw_ref: L_throw_syntax: L_throw_type: {
        uint8_t op = *pc;
        JSValue msg = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        char buf[160];
        name_str(ctx, msg, buf, sizeof buf);
        kz_throw_error(ctx, op == OP_throw_ref ? KZ_ERR_REFERENCE : op == OP_throw_syntax ? KZ_ERR_SYNTAX : KZ_ERR_TYPE, "%s", buf);
        THROW();
    }

    /* ------------------------------------------------------------ globals */
L_get_global: L_get_global_typeof: {
        uint8_t op = *pc;
        JSValue name = consts[rd32(pc + 1)];
        KzIC *ic = &b->ic[rd32(pc + 5)];
        pc += 9;
        if (global_ic_hit(ctx, ic)) {
            if (ic->flags & IC_LEXCELL) {
                JSValue v = ((JSVarCell *)ic->holder)->value;
                if (v != JS_UNINIT) { *sp++ = v; DISPATCH(); }
            } else if (!(ic->flags & IC_ACCESSOR)) { *sp++ = JV_OBJ(ctx->global)->slots[ic->slot]; DISPATCH(); }
        }
        SAVE();
        JSValue v = get_global(ctx, name, ic, op == OP_get_global_typeof);
        CHECK(v);
        *sp++ = v;
        DISPATCH();
    }
L_put_global: L_set_global: L_put_global_init: {
        uint8_t op = *pc;
        JSValue name = consts[rd32(pc + 1)];
        KzIC *ic = &b->ic[rd32(pc + 5)];
        pc += 9;
        JSValue v = sp[-1];
        if (op != OP_put_global_init && global_ic_hit(ctx, ic) && !(ic->flags & IC_ACCESSOR)) {
            if (ic->flags & IC_LEXCELL) {
                JSVarCell *c = (JSVarCell *)ic->holder;
                if (c->value != JS_UNINIT) { c->value = v; if (op == OP_put_global || op == OP_put_global_init) sp--; DISPATCH(); }
            } else {
                JV_OBJ(ctx->global)->slots[ic->slot] = v;
                if (op != OP_set_global) sp--;
                DISPATCH();
            }
        }
        SAVE();
        CHECK_I(put_global(ctx, name, v, ic, (b->h.flags & BC_STRICT) != 0, op == OP_put_global_init));
        if (op != OP_set_global) sp--;
        DISPATCH();
    }
L_delete_global: {
        JSValue name = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        if (global_lex_cell(ctx, name)) { *sp++ = JS_FALSE; DISPATCH(); }
        int r = kz_delete_property(ctx, JV_OBJ(ctx->global), name, false);
        CHECK_I(r);
        *sp++ = JV_MKBOOL(r);
        DISPATCH();
    }
L_global_decls: {
        JSValue list = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        CHECK_I(global_decls(ctx, list));
        DISPATCH();
    }
L_eval_decl: {
        JSValue name = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        JSVarCell *cell = JV_PTR(sp[-2]);
        if (!JV_IS_OBJECT(cell->value)) {
            JSValue x = kz_new_object_proto(ctx, JS_NULL);
            CHECK(x);
            cell = JV_PTR(sp[-2]);
            cell->value = x;
        }
        JSValue x = cell->value;
        if (sp[-1] == JS_HOLE) {
            int h = kz_has_property(ctx, JV_OBJ(x), name);
            CHECK_I(h);
            if (!h) CHECK_I(kz_define_value(ctx, x, name, JS_UNDEFINED, KZ_PROP_WEC));
        } else CHECK_I(kz_define_value(ctx, x, name, sp[-1], KZ_PROP_WEC));
        sp -= 2;
        DISPATCH();
    }

    /* ------------------------------------------------------------ dynamic scopes */
L_with_get: L_with_put: L_with_call: L_with_typeof: L_with_delete: {
        uint8_t op = *pc;
        JSValue name = consts[rd32(pc + 1)];
        int32_t off = rdi32(pc + 5);
        int kind = pc[9];
        const uint8_t *target = pc + off;
        pc += 10;
        SAVE();
        JSValue obj = sp[-1];
        int h = with_has(ctx, obj, name, kind);
        CHECK_I(h);
        if (!h) { sp--; DISPATCH(); }
        switch (op) {
        case OP_with_get: case OP_with_typeof: {
            JSValue v = kz_get(ctx, obj, name, obj);
            CHECK(v);
            sp[-1] = v;
            break;
        }
        case OP_with_call: {
            JSValue v = kz_get(ctx, obj, name, obj);
            CHECK(v);
            sp[0] = kind == WK_WITH ? obj : JS_UNDEFINED;
            sp[-1] = v;
            sp++;
            break;
        }
        case OP_with_put: {
            int r = kz_set(ctx, obj, name, sp[-2], obj, (b->h.flags & BC_STRICT) != 0);
            CHECK_I(r);
            sp--;
            break;
        }
        case OP_with_delete: {
            int r = kz_delete_property(ctx, JV_OBJ(obj), name, (b->h.flags & BC_STRICT) != 0);
            CHECK_I(r);
            sp[-1] = JV_MKBOOL(r);
            break;
        }
        }
        pc = target;
        DISPATCH();
    }
L_with_obj: {
        pc++;
        SAVE();
        JSValue o = kz_to_object(ctx, sp[-1]);
        CHECK(o);
        sp[-1] = o;
        DISPATCH();
    }
L_eval_var_obj: {
        uint16_t s = rd16(pc + 1);
        pc += 3;
        JSValue v = regs[s];
        if (JV_IS_INTERNAL(v)) v = ((JSVarCell *)JV_PTR(v))->value;
        *sp++ = v;
        DISPATCH();
    }

    /* ------------------------------------------------------------ properties */
L_get_field: {
        JSValue key = consts[rd32(pc + 1)];
        KzIC *ic = &b->ic[rd32(pc + 5)];
        pc += 9;
        JSValue obj = sp[-1];
        if (JV_IS_OBJECT(obj)) {
            JSObject *o = JV_OBJ(obj);
            if (ic_hit(ic, o) && !(ic->flags & IC_ACCESSOR)) {
                if (!ic->holder) { sp[-1] = o->slots[ic->slot]; DISPATCH(); }
                JSObject *h = ic->holder;
                if (h->shape == ic->holder_shape && h->shape->version == ic->holder_version) { sp[-1] = h->slots[ic->slot]; DISPATCH(); }
            }
        }
        SAVE();
        JSValue v;
        if (JV_IS_OBJECT(obj) && ic_hit(ic, JV_OBJ(obj))) {
            bool hit;
            v = ic_cached_get(ctx, JV_OBJ(obj), obj, ic, &hit);
            if (!hit) v = ic_get(ctx, obj, key, ic);
        } else v = ic_get(ctx, obj, key, ic);
        CHECK(v);
        sp[-1] = v;
        DISPATCH();
    }
L_get_field2: {
        JSValue key = consts[rd32(pc + 1)];
        KzIC *ic = &b->ic[rd32(pc + 5)];
        pc += 9;
        JSValue obj = sp[-1];
        if (JV_IS_OBJECT(obj)) {
            JSObject *o = JV_OBJ(obj);
            if (ic_hit(ic, o) && !(ic->flags & IC_ACCESSOR)) {
                if (!ic->holder) { sp[-1] = o->slots[ic->slot]; sp[0] = obj; sp++; DISPATCH(); }
                JSObject *h = ic->holder;
                if (h->shape == ic->holder_shape && h->shape->version == ic->holder_version) { sp[-1] = h->slots[ic->slot]; sp[0] = obj; sp++; DISPATCH(); }
            }
        }
        SAVE();
        JSValue v = ic_get(ctx, obj, key, ic);
        CHECK(v);
        sp[0] = obj;
        sp[-1] = v;
        sp++;
        DISPATCH();
    }
L_put_field: {
        JSValue key = consts[rd32(pc + 1)];
        KzIC *ic = &b->ic[rd32(pc + 5)];
        pc += 9;
        JSValue obj = sp[-2], val = sp[-1];
        if (JV_IS_OBJECT(obj)) {
            JSObject *o = JV_OBJ(obj);
            if (ic_hit(ic, o) && !ic->holder && !ic->flags) { o->slots[ic->slot] = val; sp -= 2; DISPATCH(); }
        }
        SAVE();
        CHECK_I(ic_put(ctx, obj, key, val, ic, (b->h.flags & BC_STRICT) != 0));
        sp -= 2;
        DISPATCH();
    }
L_get_elem: {
        pc++;
        JSValue obj = sp[-2], key = sp[-1];
        if (JV_IS_OBJECT(obj) && JV_IS_INT(key)) {
            JSObject *o = JV_OBJ(obj);
            uint32_t i = (uint32_t)JV_INT(key);
            if (i < o->elem_len && o->elems[i] != JS_HOLE && !(o->h.flags & OBJ_EXOTIC)) { sp[-2] = o->elems[i]; sp--; DISPATCH(); }
        }
        SAVE();
        if (JV_IS_NULLISH(obj)) {
            JSValue k = kz_to_property_key(ctx, key);
            CHECK(k);
            JSValue v = kz_get(ctx, obj, k, obj);
            CHECK(v);
            sp[-2] = v;
            sp--;
            DISPATCH();
        }
        JSValue k = kz_to_property_key(ctx, key);
        CHECK(k);
        sp[-1] = k;
        JSValue v = kz_get(ctx, obj, k, obj);
        CHECK(v);
        sp[-2] = v;
        sp--;
        DISPATCH();
    }
L_get_elem2: {
        pc++;
        JSValue obj = sp[-2], key = sp[-1];
        SAVE();
        JSValue k = kz_to_property_key(ctx, key);
        CHECK(k);
        sp[-1] = k;
        JSValue v = kz_get(ctx, obj, k, obj);
        CHECK(v);
        sp[-2] = v;
        sp[-1] = obj;
        DISPATCH();
    }
L_put_elem: {
        pc++;
        JSValue obj = sp[-3], key = sp[-2], val = sp[-1];
        if (JV_IS_OBJECT(obj) && JV_IS_INT(key)) {
            JSObject *o = JV_OBJ(obj);
            uint32_t i = (uint32_t)JV_INT(key);
            if (!(o->h.flags & (OBJ_EXOTIC | OBJ_ELEMS_FROZEN))) {
                if (i < o->elem_len && o->elems[i] != JS_HOLE) { o->elems[i] = val; sp -= 3; DISPATCH(); }
                /* appending to a fast array whose prototypes have no indexed properties */
                if (kz_class(o) == KZ_CLASS_ARRAY && i == o->elem_len && i == o->u.array.length && i < o->elem_cap &&
                    (o->h.flags & (OBJ_EXTENSIBLE | OBJ_LEN_RO)) == OBJ_EXTENSIBLE && !(o->shape->h.flags & SHAPE_HAS_INDEX)) {
                    JSValue p = o->shape->proto;
                    bool clean = true;
                    while (JV_IS_OBJECT(p)) {
                        JSObject *po = JV_OBJ(p);
                        if (po->elem_len || (po->shape->h.flags & SHAPE_HAS_INDEX) || (po->h.flags & OBJ_EXOTIC)) { clean = false; break; }
                        p = po->shape->proto;
                    }
                    if (clean) {
                        o->elems[i] = val;
                        o->elem_len = i + 1;
                        o->u.array.length = i + 1;
                        sp -= 3;
                        DISPATCH();
                    }
                }
            }
        }
        SAVE();
        JSValue k = kz_to_property_key(ctx, key);
        CHECK(k);
        sp[-2] = k;
        CHECK_I(kz_set(ctx, obj, k, sp[-1], obj, (b->h.flags & BC_STRICT) != 0));
        sp -= 3;
        DISPATCH();
    }
L_get_length: {
        pc++;
        JSValue obj = sp[-1];
        if (JV_IS_OBJECT(obj) && kz_class(JV_OBJ(obj)) == KZ_CLASS_ARRAY) { sp[-1] = jv_uint32(JV_OBJ(obj)->u.array.length); DISPATCH(); }
        if (JV_IS_STRING(obj)) { sp[-1] = JV_MKINT((int32_t)kz_str_len(JV_STR(obj))); DISPATCH(); }
        SAVE();
        JSValue v = kz_get(ctx, obj, rt->atom[KZ_ATOM_length], obj);
        CHECK(v);
        sp[-1] = v;
        DISPATCH();
    }
L_get_field_recv: {
        JSValue key = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        JSValue base = sp[-1], recv = sp[-2];
        if (!JV_IS_OBJECT(base)) {
            if (JV_IS_NULLISH(base)) { kz_throw_type(ctx, "Cannot read properties of super"); THROW(); }
        }
        JSValue v = kz_get(ctx, base, key, recv);
        CHECK(v);
        sp[-2] = v;
        sp--;
        DISPATCH();
    }
L_get_elem_recv: {
        pc++;
        SAVE();
        JSValue k = kz_to_property_key(ctx, sp[-1]);
        CHECK(k);
        sp[-1] = k;
        if (JV_IS_NULLISH(sp[-2])) { kz_throw_type(ctx, "Cannot read properties of super"); THROW(); }
        JSValue v = kz_get(ctx, sp[-2], k, sp[-3]);
        CHECK(v);
        sp[-3] = v;
        sp -= 2;
        DISPATCH();
    }
L_put_field_recv: {
        JSValue key = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        if (!JV_IS_OBJECT(sp[-2])) { kz_throw_type(ctx, "Cannot set properties of super"); THROW(); }
        CHECK_I(kz_set(ctx, sp[-2], key, sp[-1], sp[-3], true));
        sp -= 3;
        DISPATCH();
    }
L_put_elem_recv: {
        pc++;
        SAVE();
        JSValue k = kz_to_property_key(ctx, sp[-2]);
        CHECK(k);
        sp[-2] = k;
        if (!JV_IS_OBJECT(sp[-3])) { kz_throw_type(ctx, "Cannot set properties of super"); THROW(); }
        CHECK_I(kz_set(ctx, sp[-3], k, sp[-1], sp[-4], true));
        sp -= 4;
        DISPATCH();
    }
L_get_super: {
        pc++;
        SAVE();
        JSValue home = sp[-1];
        if (!JV_IS_OBJECT(home)) { sp[-1] = JS_UNDEFINED; DISPATCH(); }
        JSValue p;
        CHECK_I(kz_get_proto(ctx, JV_OBJ(home), &p));
        sp[-1] = p;
        DISPATCH();
    }
L_delete_field: {
        JSValue key = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        JSValue o = kz_to_object(ctx, sp[-1]);
        CHECK(o);
        sp[-1] = o;
        int r = kz_delete_property(ctx, JV_OBJ(o), key, (b->h.flags & BC_STRICT) != 0);
        CHECK_I(r);
        sp[-1] = JV_MKBOOL(r);
        DISPATCH();
    }
L_delete_elem: {
        pc++;
        SAVE();
        JSValue o = kz_to_object(ctx, sp[-2]);
        CHECK(o);
        sp[-2] = o;
        JSValue k = kz_to_property_key(ctx, sp[-1]);
        CHECK(k);
        sp[-1] = k;
        int r = kz_delete_property(ctx, JV_OBJ(o), k, (b->h.flags & BC_STRICT) != 0);
        CHECK_I(r);
        sp[-2] = JV_MKBOOL(r);
        sp--;
        DISPATCH();
    }
L_define_field: {
        JSValue key = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        CHECK_I(kz_create_data_property(ctx, sp[-2], key, sp[-1]));
        sp--;
        DISPATCH();
    }
L_define_elem: {
        pc++;
        SAVE();
        JSValue k = kz_to_property_key(ctx, sp[-2]);
        CHECK(k);
        sp[-2] = k;
        CHECK_I(kz_create_data_property(ctx, sp[-3], k, sp[-1]));
        sp -= 2;
        DISPATCH();
    }
L_define_method: {
        JSValue key = consts[rd32(pc + 1)];
        int kind = pc[5];
        pc += 6;
        SAVE();
        CHECK_I(define_method(ctx, sp[-2], key, sp[-1], kind));
        sp--;
        DISPATCH();
    }
L_define_method_elem: {
        int kind = pc[1];
        pc += 2;
        SAVE();
        if (kind & DM_FIELD) {
            /* a class element record: ctor key value -> ctor */
            JSValue k = sp[-2];
            if (!(kind & DM_PRIVATE)) {
                k = kz_to_property_key(ctx, k);
                CHECK(k);
                sp[-2] = k;
            }
            CHECK_I(add_field_record(ctx, sp[-3], k, sp[-1], kind & 3));
            sp -= 2;
            DISPATCH();
        }
        JSValue k = kz_to_property_key(ctx, sp[-2]);
        CHECK(k);
        sp[-2] = k;
        if ((kind & 3) == DM_METHOD) CHECK_I(kz_set_function_name(ctx, sp[-1], k, NULL));
        CHECK_I(define_method(ctx, sp[-3], k, sp[-1], kind));
        sp -= 2;
        DISPATCH();
    }
L_set_proto: {
        pc++;
        SAVE();
        JSValue pv = sp[-1];
        if (JV_IS_OBJECT(pv) || pv == JS_NULL) CHECK_I(kz_set_proto(ctx, JV_OBJ(sp[-2]), pv, true));
        sp--;
        DISPATCH();
    }
L_copy_data_props: {
        int n = pc[1];
        pc += 2;
        SAVE();
        JSValue *excl = sp - n;
        CHECK_I(copy_data_props(ctx, excl[-2], excl[-1], excl, n));
        sp -= n + 1;
        DISPATCH();
    }
L_get_private: {
        pc++;
        SAVE();
        JSValue v;
        CHECK_I(kz_private_get(ctx, sp[-2], sp[-1], &v));
        sp[-2] = v;
        sp--;
        DISPATCH();
    }
L_put_private: {
        pc++;
        SAVE();
        CHECK_I(kz_private_set(ctx, sp[-3], sp[-2], sp[-1]));
        sp -= 3;
        DISPATCH();
    }
L_define_private: {
        pc++;
        SAVE();
        CHECK_I(kz_private_define(ctx, sp[-3], sp[-2], sp[-1], FR_FIELD));
        sp -= 2;
        DISPATCH();
    }
L_define_private_method: {
        int kind = pc[1];
        pc += 2;
        SAVE();
        int fr = kind == DM_GETTER ? FR_PRIVATE_GETTER : kind == DM_SETTER ? FR_PRIVATE_SETTER : FR_PRIVATE_METHOD;
        CHECK_I(kz_private_define(ctx, sp[-3], sp[-2], sp[-1], fr));
        sp -= 2;
        DISPATCH();
    }
L_private_in: {
        pc++;
        SAVE();
        if (!JV_IS_OBJECT(sp[-1])) { kz_throw_type(ctx, "Cannot use 'in' operator to search for a private field in a non-object"); THROW(); }
        sp[-2] = JV_MKBOOL(kz_private_has(sp[-1], sp[-2]));
        sp--;
        DISPATCH();
    }

    /* ------------------------------------------------------------ arithmetic */
L_add: {
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb)) {
            int64_t r = (int64_t)JV_INT(a) + JV_INT(bb);
            sp[-2] = jv_int64(r);
            sp--;
            DISPATCH();
        }
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) { sp[-2] = jv_from_raw_double(jv_num(a) + jv_num(bb)); sp--; DISPATCH(); }
        SAVE();
        JSValue r = kz_add(ctx, a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_sub: {
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb)) { sp[-2] = jv_int64((int64_t)JV_INT(a) - JV_INT(bb)); sp--; DISPATCH(); }
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) { sp[-2] = jv_from_raw_double(jv_num(a) - jv_num(bb)); sp--; DISPATCH(); }
        SAVE();
        JSValue r = kz_binary_arith(ctx, KZ_BIN_SUB, a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_mul: {
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb)) {
            int64_t r = (int64_t)JV_INT(a) * JV_INT(bb);
            if (r != 0 || (JV_INT(a) >= 0 && JV_INT(bb) >= 0)) { sp[-2] = jv_int64(r); sp--; DISPATCH(); }
            sp[-2] = jv_from_dbl(-0.0); sp--; DISPATCH();
        }
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) { sp[-2] = jv_from_raw_double(jv_num(a) * jv_num(bb)); sp--; DISPATCH(); }
        SAVE();
        JSValue r = kz_binary_arith(ctx, KZ_BIN_MUL, a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_div: {
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) { sp[-2] = jv_from_raw_double(jv_num(a) / jv_num(bb)); sp--; DISPATCH(); }
        SAVE();
        JSValue r = kz_binary_arith(ctx, KZ_BIN_DIV, a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_mod: {
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb) && JV_INT(a) >= 0 && JV_INT(bb) > 0) { sp[-2] = JV_MKINT(JV_INT(a) % JV_INT(bb)); sp--; DISPATCH(); }
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) { sp[-2] = jv_from_raw_double(kz_math_fmod(jv_num(a), jv_num(bb))); sp--; DISPATCH(); }
        SAVE();
        JSValue r = kz_binary_arith(ctx, KZ_BIN_MOD, a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_pow: L_band: L_bor: L_bxor: L_shl: L_sar: L_shr: {
        uint8_t op = *pc;
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb)) {
            int32_t x = JV_INT(a), y = JV_INT(bb);
            switch (op) {
            case OP_band: sp[-2] = JV_MKINT(x & y); sp--; DISPATCH();
            case OP_bor: sp[-2] = JV_MKINT(x | y); sp--; DISPATCH();
            case OP_bxor: sp[-2] = JV_MKINT(x ^ y); sp--; DISPATCH();
            case OP_shl: sp[-2] = JV_MKINT((int32_t)((uint32_t)x << (y & 31))); sp--; DISPATCH();
            case OP_sar: sp[-2] = JV_MKINT(x >> (y & 31)); sp--; DISPATCH();
            case OP_shr: sp[-2] = jv_uint32((uint32_t)x >> (y & 31)); sp--; DISPATCH();
            default: break;
            }
        }
        SAVE();
        static const int bin[] = { [OP_pow] = KZ_BIN_POW, [OP_band] = KZ_BIN_AND, [OP_bor] = KZ_BIN_OR, [OP_bxor] = KZ_BIN_XOR,
                                   [OP_shl] = KZ_BIN_SHL, [OP_sar] = KZ_BIN_SAR, [OP_shr] = KZ_BIN_SHR };
        JSValue r;
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb) && op == OP_pow) r = jv_from_raw_double(kz_math_pow(jv_num(a), jv_num(bb)));
        else r = kz_binary_arith(ctx, bin[op], a, bb);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_lt: L_le: L_gt: L_ge: {
        uint8_t op = *pc;
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        if (JV_IS_INT(a) && JV_IS_INT(bb)) {
            int32_t x = JV_INT(a), y = JV_INT(bb);
            bool r = op == OP_lt ? x < y : op == OP_le ? x <= y : op == OP_gt ? x > y : x >= y;
            sp[-2] = JV_MKBOOL(r);
            sp--;
            DISPATCH();
        }
        if (JV_IS_NUMBER(a) && JV_IS_NUMBER(bb)) {
            double x = jv_num(a), y = jv_num(bb);
            bool r = op == OP_lt ? x < y : op == OP_le ? x <= y : op == OP_gt ? x > y : x >= y;
            sp[-2] = JV_MKBOOL(r);
            sp--;
            DISPATCH();
        }
        SAVE();
        /* ECMA-262 13.10.1: < and >= ask IsLessThan(a, b), > and <= ask IsLessThan(b, a) with the right side
           converted first; an undefined answer (NaN) makes all four false */
        int r = (op == OP_lt || op == OP_ge) ? kz_less_than(ctx, a, bb, true) : kz_less_than(ctx, bb, a, false);
        CHECK_I(r);
        bool res = (op == OP_lt || op == OP_gt) ? r == 1 : r == 0;
        sp[-2] = JV_MKBOOL(res);
        sp--;
        DISPATCH();
    }
L_eq: L_ne: {
        uint8_t op = *pc;
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        int r;
        if (JV_IS_INT(a) && JV_IS_INT(bb)) r = a == bb;
        else { SAVE(); r = kz_loose_eq(ctx, a, bb); CHECK_I(r); }
        sp[-2] = JV_MKBOOL(op == OP_eq ? r : !r);
        sp--;
        DISPATCH();
    }
L_seq: L_sne: {
        uint8_t op = *pc;
        pc++;
        JSValue a = sp[-2], bb = sp[-1];
        int r;
        if (a == bb && !JV_IS_DOUBLE(a)) r = 1;
        else { SAVE(); r = kz_strict_eq(ctx, a, bb); CHECK_I(r); }
        sp[-2] = JV_MKBOOL(op == OP_seq ? r : !r);
        sp--;
        DISPATCH();
    }
L_instanceof: {
        pc++;
        SAVE();
        int r = kz_instance_of(ctx, sp[-2], sp[-1]);
        CHECK_I(r);
        sp[-2] = JV_MKBOOL(r);
        sp--;
        DISPATCH();
    }
L_in: {
        pc++;
        SAVE();
        if (!JV_IS_OBJECT(sp[-1])) { kz_throw_type(ctx, "Cannot use 'in' operator to search for a key in a non-object"); THROW(); }
        JSValue k = kz_to_property_key(ctx, sp[-2]);
        CHECK(k);
        sp[-2] = k;
        int r = kz_has_property(ctx, JV_OBJ(sp[-1]), k);
        CHECK_I(r);
        sp[-2] = JV_MKBOOL(r);
        sp--;
        DISPATCH();
    }
L_neg: {
        pc++;
        JSValue a = sp[-1];
        if (JV_IS_INT(a) && JV_INT(a) != 0 && JV_INT(a) != INT32_MIN) { sp[-1] = JV_MKINT(-JV_INT(a)); DISPATCH(); }
        if (JV_IS_NUMBER(a)) { sp[-1] = jv_from_raw_double(-jv_num(a)); DISPATCH(); }
        SAVE();
        JSValue n = kz_to_numeric(ctx, a);
        CHECK(n);
        if (JV_IS_BIGINT(n)) { n = kz_bigint_neg(ctx, n); CHECK(n); sp[-1] = n; }
        else sp[-1] = jv_from_raw_double(-jv_num(n));
        DISPATCH();
    }
L_plus: {
        pc++;
        if (JV_IS_NUMBER(sp[-1])) DISPATCH();
        SAVE();
        JSValue n = kz_to_number_value(ctx, sp[-1]);
        CHECK(n);
        sp[-1] = n;
        DISPATCH();
    }
L_to_numeric: {
        pc++;
        if (JV_IS_NUMBER(sp[-1])) DISPATCH();
        SAVE();
        JSValue n = kz_to_numeric(ctx, sp[-1]);
        CHECK(n);
        sp[-1] = n;
        DISPATCH();
    }
L_not: sp[-1] = JV_MKBOOL(!kz_to_bool(sp[-1])); pc++; DISPATCH();
L_bnot: {
        pc++;
        if (JV_IS_INT(sp[-1])) { sp[-1] = JV_MKINT(~JV_INT(sp[-1])); DISPATCH(); }
        SAVE();
        JSValue n = kz_to_numeric(ctx, sp[-1]);
        CHECK(n);
        if (JV_IS_BIGINT(n)) { n = kz_bigint_not(ctx, n); CHECK(n); sp[-1] = n; }
        else sp[-1] = JV_MKINT(~kz_double_to_int32(jv_num(n)));
        DISPATCH();
    }
L_typeof: sp[-1] = kz_typeof(ctx, sp[-1]); pc++; DISPATCH();
L_inc: L_dec: {
        uint8_t op = *pc;
        pc++;
        JSValue a = sp[-1];
        if (JV_IS_INT(a)) {
            int32_t x = JV_INT(a);
            if (op == OP_inc ? x != INT32_MAX : x != INT32_MIN) { sp[-1] = JV_MKINT(op == OP_inc ? x + 1 : x - 1); DISPATCH(); }
        }
        if (JV_IS_NUMBER(a)) { sp[-1] = jv_from_raw_double(jv_num(a) + (op == OP_inc ? 1 : -1)); DISPATCH(); }
        SAVE();
        JSValue n = kz_to_numeric(ctx, a);
        CHECK(n);
        sp[-1] = n;
        if (JV_IS_BIGINT(n)) {
            JSValue one = kz_bigint_from_int64(ctx, 1);
            CHECK(one);
            JSValue r = kz_bigint_binary(ctx, op == OP_inc ? KZ_BIN_ADD : KZ_BIN_SUB, sp[-1], one);
            CHECK(r);
            sp[-1] = r;
        } else sp[-1] = jv_from_raw_double(jv_num(n) + (op == OP_inc ? 1 : -1));
        DISPATCH();
    }
L_to_propkey: {
        pc++;
        JSValue k = sp[-1];
        if (JV_IS_INT(k) || JV_IS_STRING(k) || JV_IS_SYMBOL(k)) DISPATCH();
        SAVE();
        JSValue r = kz_to_property_key(ctx, k);
        CHECK(r);
        /* keep it as a value: index keys become numbers again */
        sp[-1] = KZ_KEY_IS_INDEX(r) ? jv_uint32(KZ_KEY_INDEX(r)) : r;
        DISPATCH();
    }
L_to_string: {
        pc++;
        if (JV_IS_STRING(sp[-1])) DISPATCH();
        SAVE();
        JSValue s = kz_to_string(ctx, sp[-1]);
        CHECK(s);
        sp[-1] = s;
        DISPATCH();
    }
L_is_undefined: sp[-1] = JV_MKBOOL(sp[-1] == JS_UNDEFINED); pc++; DISPATCH();
L_is_nullish: sp[-1] = JV_MKBOOL(JV_IS_NULLISH(sp[-1])); pc++; DISPATCH();

    /* ------------------------------------------------------------ control */
L_goto: {
        int32_t off = rdi32(pc + 1);
        if (off <= 0 && --rt->interrupt_count <= 0) {      /* (0: a jump to itself, as in for (;;) {}) */
            SAVE();
            kz_poll_interrupt(ctx);
            if (kz_has_exception(ctx)) THROW();
            kz_gc_maybe(ctx);
        }
        pc += off;
        DISPATCH();
    }
L_if_true: L_if_false: {
        uint8_t op = *pc;
        int32_t off = rdi32(pc + 1);
        JSValue v = *--sp;
        bool t = v == JS_TRUE ? true : v == JS_FALSE ? false : kz_to_bool(v);
        if (t == (op == OP_if_true)) {
            if (off <= 0 && --rt->interrupt_count <= 0) {
                SAVE();
                kz_poll_interrupt(ctx);
                if (kz_has_exception(ctx)) THROW();
            }
            pc += off;
        } else pc += 5;
        DISPATCH();
    }
L_if_nullish: L_if_not_nullish: {
        uint8_t op = *pc;
        bool n = JV_IS_NULLISH(sp[-1]);
        if (n == (op == OP_if_nullish)) pc += rdi32(pc + 1); else pc += 5;
        DISPATCH();
    }
L_if_undefined: L_if_not_undefined: {
        uint8_t op = *pc;
        bool u = sp[-1] == JS_UNDEFINED;
        if (u == (op == OP_if_undefined)) pc += rdi32(pc + 1); else pc += 5;
        DISPATCH();
    }
L_return: ret = *--sp; goto do_return;
L_return_undef: ret = JS_UNDEFINED; goto do_return;
L_throw: {
        pc++;
        SAVE();
        kz_throw(ctx, *--sp);
        THROW();
    }
L_finally_end: {
        const uint8_t *at = pc;
        uint16_t n = rd16(pc + 1);
        JSValue type = sp[-1], val = sp[-2];
        int t = JV_IS_INT(type) ? JV_INT(type) : 0;
        if (t == COMPLETION_NORMAL) { sp -= 2; pc += 3 + 4 * (size_t)n; DISPATCH(); }
        if (t == COMPLETION_THROW) { sp -= 2; pc += 3 + 4 * (size_t)n; SAVE(); kz_throw(ctx, val); THROW(); }
        int k = t - 2;
        sp--;                               /* the value stays for the continuation */
        pc = at + rdi32(at + 3 + 4 * (size_t)k);
        DISPATCH();
    }
L_check_ctor_return: {
        pc++;
        JSValue v = sp[-2], th = sp[-1];
        sp--;
        if (JV_IS_OBJECT(v)) { sp[-1] = v; DISPATCH(); }
        SAVE();
        if (v != JS_UNDEFINED) { kz_throw_type(ctx, "Derived constructors may only return object or undefined"); THROW(); }
        if (th == JS_UNINIT) { kz_throw_reference(ctx, "Must call super constructor in derived class before accessing 'this' or returning from derived constructor"); THROW(); }
        sp[-1] = th;
        DISPATCH();
    }
L_init_this_loc: L_init_this_lcell: L_init_this_cvar: {
        uint8_t op = *pc;
        uint16_t s = rd16(pc + 1);
        pc += 3;
        JSValue v = *--sp;
        JSValue *slot;
        if (op == OP_init_this_loc) slot = &regs[s];
        else if (op == OP_init_this_lcell) slot = &((JSVarCell *)JV_PTR(regs[s]))->value;
        else slot = &f->cells[s]->value;
        if (*slot != JS_UNINIT) { SAVE(); kz_throw_reference(ctx, "Super constructor may only be called once"); THROW(); }
        *slot = v;
        DISPATCH();
    }
L_debugger: pc++; DISPATCH();

    /* ------------------------------------------------------------ iteration */
L_for_in_start: {
        pc++;
        SAVE();
        JSValue it = for_in_start(ctx, sp[-1]);
        CHECK(it);
        sp[-1] = it;
        DISPATCH();
    }
L_for_in_next: {
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        SAVE();
        JSValue k = for_in_next(ctx, JV_OBJ(sp[-1]));
        CHECK(k);
        if (k == JS_EMPTY) { pc = target; DISPATCH(); }
        *sp++ = k;
        DISPATCH();
    }
L_for_of_start: L_get_iterator: L_for_await_start: L_get_async_iterator: {
        uint8_t op = *pc;
        pc++;
        SAVE();
        CHECK_I(vm_get_iterator(ctx, sp, op == OP_for_await_start || op == OP_get_async_iterator));
        sp++;
        DISPATCH();
    }
L_for_of_next: {
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        SAVE();
        KzIter it = { sp[-2], sp[-1], false };   /* (the operand stack keeps both alive) */
        JSValue v;
        int r = kz_iter_step(ctx, &it, &v);
        CHECK_I(r);
        if (!r) { pc = target; DISPATCH(); }
        *sp++ = v;
        DISPATCH();
    }
L_iter_call_next: {
        pc++;
        SAVE();
        JSValue r = kz_call(ctx, sp[-1], sp[-2], 0, NULL);
        CHECK(r);
        *sp++ = r;
        DISPATCH();
    }
L_iter_result: {
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        SAVE();
        JSValue res = sp[-1];
        if (!JV_IS_OBJECT(res)) { kz_throw_type(ctx, "Iterator result is not an object"); THROW(); }
        JSValue d = kz_get(ctx, res, rt->atom[KZ_ATOM_done], res);
        CHECK(d);
        if (kz_to_bool(d)) { sp--; pc = target; DISPATCH(); }
        JSValue v = kz_get(ctx, sp[-1], rt->atom[KZ_ATOM_value], sp[-1]);
        CHECK(v);
        sp[-1] = v;
        DISPATCH();
    }
L_iter_close: {
        pc++;
        SAVE();
        KzIter it = { sp[-2], sp[-1], false };
        JSValue r = kz_iter_close(ctx, &it, JS_UNDEFINED);
        CHECK(r);
        sp -= 2;
        DISPATCH();
    }
L_iter_close_throw: {
        pc++;
        SAVE();
        KzIter it = { sp[-3], sp[-2], false };   /* (popped slots stay scanned: frames mark all registers) */
        kz_throw(ctx, sp[-1]);
        sp -= 3;
        kz_iter_close_on_throw(ctx, &it);
        THROW();
    }
L_iter_close_async: {
        /* for await, leaving the loop: iter next -> the result of return() for the code to await; no return method:
           jump, nothing awaited */
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        SAVE();
        JSValue r = kz_iter_close_async_begin(ctx, sp[-2]);
        CHECK(r);
        if (r == JS_EMPTY) { sp -= 2; pc = target; DISPATCH(); }
        sp--;
        sp[-1] = r;
        DISPATCH();
    }
L_iter_close_check: {
        pc++;
        SAVE();
        if (!JV_IS_OBJECT(sp[-1])) { kz_throw_type(ctx, "Iterator result is not an object"); THROW(); }
        sp--;
        DISPATCH();
    }
L_iter_close_throw_async: {
        /* for await, an exception in the body: iter next exc -> exc result (to await), or jump with just exc */
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        SAVE();
        JSValue r = kz_iter_close_async_on_throw(ctx, sp[-3]);
        CHECK(r);
        sp[-3] = sp[-1];
        if (r == JS_EMPTY) { sp -= 2; pc = target; DISPATCH(); }
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_push_done: *sp++ = JS_FALSE; pc++; DISPATCH();
L_iter_step: {
        pc++;
        SAVE();
        if (sp[-1] == JS_TRUE) { *sp++ = JS_UNDEFINED; DISPATCH(); }
        KzIter it = { sp[-3], sp[-2], false };
        JSValue v;
        int r = kz_iter_step(ctx, &it, &v);
        if (r < 0) { sp[-1] = JS_TRUE; THROW(); }
        if (!r) { sp[-1] = JS_TRUE; *sp++ = JS_UNDEFINED; DISPATCH(); }
        *sp++ = v;
        DISPATCH();
    }
L_iter_rest: {
        pc++;
        SAVE();
        JSValue arr = kz_new_array(ctx, 0);
        CHECK(arr);
        *sp++ = arr;
        if (sp[-2] == JS_TRUE) DISPATCH();
        KzIter it = { sp[-4], sp[-3], false };
        for (;;) {
            JSValue v;
            int r = kz_iter_step(ctx, &it, &v);
            if (r < 0) { sp[-2] = JS_TRUE; THROW(); }
            if (!r) break;
            if (kz_array_push(ctx, JV_OBJ(sp[-1]), v)) THROW();   /* (it roots v while growing) */
        }
        sp[-2] = JS_TRUE;
        DISPATCH();
    }
L_iter_finish: {
        pc++;
        SAVE();
        if (sp[-1] != JS_TRUE) {
            KzIter it = { sp[-3], sp[-2], false };
            JSValue r = kz_iter_close(ctx, &it, JS_UNDEFINED);
            CHECK(r);
        }
        sp -= 3;
        DISPATCH();
    }
L_iter_finish_throw: {
        pc++;
        SAVE();
        kz_throw(ctx, sp[-1]);
        if (sp[-2] != JS_TRUE) {
            KzIter it = { sp[-4], sp[-3], false };
            sp -= 4;
            kz_iter_close_on_throw(ctx, &it);
        } else sp -= 4;
        THROW();
    }

    /* ------------------------------------------------------------ calls */
L_call: {
        uint16_t argc = rd16(pc + 1);
        pc += 3;
        JSValue *args = sp - argc;
        JSValue fv = args[-2], this_val = args[-1];
        if (JV_IS_OBJECT(fv)) {
            JSObject *fo = JV_OBJ(fv);
            if (kz_class(fo) == KZ_CLASS_FUNCTION && !(fo->h.flags & OBJ_CLASS_CTOR) &&
                !(fo->u.func.b->h.flags & (BC_GENERATOR | BC_ASYNC))) {
                JSFunctionBytecode *nb = fo->u.func.b;
                if (!(nb->h.flags & BC_STRICT) && !JV_IS_OBJECT(this_val) && kz_bc_kind(nb) != KZ_FUNC_ARROW) {
                    SAVE();
                    JSValue t = coerce_this(ctx, nb, this_val);
                    CHECK(t);
                    args[-1] = this_val = t;
                }
                f->pc = pc;
                f->sp = args - 2;
                JSFrame *nf = push_frame(ctx, fo, this_val, JS_UNDEFINED, argc, args, 0);
                if (!nf) { f->sp = sp; THROW(); }
                f = nf;
                LOAD_FRAME();
                pc = f->pc;
                sp = f->sp;
                if (--rt->interrupt_count <= 0) {
                    SAVE();
                    kz_poll_interrupt(ctx);
                    if (kz_has_exception(ctx)) THROW();
                    kz_gc_maybe(ctx);
                }
                DISPATCH();
            }
        }
        SAVE();
        JSValue r = kz_call(ctx, fv, this_val, argc, args);
        CHECK(r);
        sp = args - 2;
        *sp++ = r;
        DISPATCH();
    }
L_new: {
        uint16_t argc = rd16(pc + 1);
        pc += 3;
        JSValue *args = sp - argc;
        JSValue fv = args[-1];
        SAVE();
        if (JV_IS_OBJECT(fv) && kz_class(JV_OBJ(fv)) == KZ_CLASS_FUNCTION && (JV_OBJ(fv)->h.flags & OBJ_CONSTRUCTOR)) {
            JSObject *fo = JV_OBJ(fv);
            JSValue th = construct_this(ctx, fo, fv);
            CHECK(th);
            /* keep this somewhere scanned: the callee frame holds it from now on */
            f->pc = pc;
            f->sp = args - 1;
            JSFrame *nf = push_frame(ctx, fo, th, fv, argc, args, FRAME_CONSTRUCT);   /* (no heap allocation) */
            if (!nf) { f->sp = sp; THROW(); }
            f = nf;
            LOAD_FRAME();
            pc = f->pc;
            sp = f->sp;
            DISPATCH();
        }
        JSValue r = kz_construct(ctx, fv, argc, args, fv);
        CHECK(r);
        sp = args - 1;
        *sp++ = r;
        DISPATCH();
    }
L_call_spread: L_new_spread: {
        uint8_t op = *pc;
        pc++;
        SAVE();
        uint32_t nargs;
        CHECK_I(vm_dense_args(ctx, sp[-1], &nargs));
        JSValue *argv_ = JV_OBJ(sp[-1])->elems;
        JSValue r;
        if (op == OP_call_spread) {
            r = kz_call(ctx, sp[-3], sp[-2], (int)nargs, argv_);
            CHECK(r);
            sp -= 3;
        } else {
            r = kz_construct(ctx, sp[-2], (int)nargs, argv_, sp[-2]);
            CHECK(r);
            sp -= 2;
        }
        *sp++ = r;
        DISPATCH();
    }
L_eval: {
        uint16_t argc = rd16(pc + 1), first = rd16(pc + 3), count = rd16(pc + 5);
        pc += 7;
        JSValue *args = sp - argc;
        JSValue fv = args[-2];
        SAVE();
        JSValue r;
        if (fv == kz_intr(ctx, KZ_I_EVAL)) {
            r = kz_eval_direct(ctx, f, argc ? args[0] : JS_UNDEFINED, first, count);
        } else r = kz_call(ctx, fv, args[-1], argc, args);
        CHECK(r);
        sp = args - 2;
        *sp++ = r;
        DISPATCH();
    }
L_super_call: L_super_call_spread: {
        uint8_t op = *pc;
        uint16_t argc = 0;
        if (op == OP_super_call) { argc = rd16(pc + 1); pc += 3; } else pc++;
        SAVE();
        JSValue *args;
        JSValue *base;
        if (op == OP_super_call) { args = sp - argc; base = args - 2; }
        else {
            uint32_t nargs;
            CHECK_I(vm_dense_args(ctx, sp[-1], &nargs));
            if (nargs > 65535) { kz_throw_range(ctx, "Too many arguments"); THROW(); }
            args = JV_OBJ(sp[-1])->elems;
            argc = (uint16_t)nargs;
            base = sp - 3;
        }
        JSValue func = base[0], nt = base[1];
        JSValue parent;
        CHECK_I(kz_get_proto(ctx, JV_OBJ(func), &parent));
        if (!kz_is_constructor(parent)) { kz_throw_type(ctx, "Super constructor is not a constructor"); THROW(); }
        JSValue r = kz_construct(ctx, parent, argc, args, nt);
        CHECK(r);
        sp = base;
        *sp++ = r;
        DISPATCH();
    }
L_import_call: {
        pc++;
        SAVE();
        JSValue r = kz_dynamic_import(ctx, sp[-2], sp[-1], b->filename);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }

    /* ------------------------------------------------------------ functions and classes */
L_closure: {
        JSValue bc = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        JSValue fn = kz_new_closure(ctx, JV_PTR(bc), f, JS_UNDEFINED);
        CHECK(fn);
        *sp++ = fn;
        DISPATCH();
    }
L_set_name: {
        JSValue name = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        CHECK_I(kz_set_function_name(ctx, sp[-1], name, NULL));
        DISPATCH();
    }
L_set_name_computed: {
        int prefix = pc[1];
        pc += 2;
        SAVE();
        JSValue k = sp[-2];
        JSValue key = kz_key_from_value(ctx, k);
        CHECK(key);
        CHECK_I(kz_set_function_name(ctx, sp[-1], key, prefix == 1 ? "get" : prefix == 2 ? "set" : NULL));
        DISPATCH();
    }
L_set_home: {
        pc++;
        JSValue fn = sp[-1];
        if (JV_IS_OBJECT(fn) && kz_class(JV_OBJ(fn)) == KZ_CLASS_FUNCTION) JV_OBJ(fn)->u.func.home = sp[-2];
        DISPATCH();
    }
L_class_create: {
        JSValue bc = consts[rd32(pc + 1)];
        bool has = pc[5];
        pc += 6;
        SAVE();
        JSValue proto;
        JSValue ctor = class_create(ctx, f, JV_PTR(bc), sp[-1], has, &proto);
        CHECK(ctor);
        sp[-1] = ctor;
        *sp++ = proto;
        DISPATCH();
    }
L_private_name: {
        JSValue desc = consts[rd32(pc + 1)];
        pc += 5;
        SAVE();
        JSSymbol *s = kz_gc_new(ctx, KZ_GC_SYMBOL, sizeof(JSSymbol));
        if (!s) THROW();
        s->description = desc;
        s->h.flags = SYM_PRIVATE;
        s->hash = (uint32_t)(uintptr_t)s;
        *sp++ = JV_MKSYM(s);
        DISPATCH();
    }
L_set_fields: pc++; sp--; DISPATCH();
L_run_fields: {
        pc++;
        SAVE();
        CHECK_I(run_fields(ctx, sp[-2], sp[-1]));
        sp -= 2;
        DISPATCH();
    }
L_set_class_name: pc += 5; DISPATCH();

    /* ------------------------------------------------------------ generators and async functions */
L_initial_yield:
        pc++;
        SAVE();
        rt->suspend_kind = KZ_SUSPEND_INITIAL;
        rt->suspend_value = JS_UNDEFINED;
        ret = JS_EMPTY;
        goto suspend;
L_yield:
        pc++;
        sp--;
        SAVE();
        rt->suspend_kind = KZ_SUSPEND_YIELD;
        rt->suspend_value = *sp;
        ret = JS_EMPTY;
        goto suspend;
L_resume_dispatch: {
        int32_t off = rdi32(pc + 1);
        const uint8_t *target = pc + off;
        pc += 5;
        int mode = JV_INT(sp[-1]);
        sp--;
        if (mode == RESUME_NEXT) DISPATCH();
        if (mode == RESUME_THROW) { SAVE(); kz_throw(ctx, *--sp); THROW(); }
        pc = target;
        DISPATCH();
    }
L_yield_star: {
        const uint8_t *at = pc;
        int32_t done_off = rdi32(pc + 1), ret_off = rdi32(pc + 5);
        SAVE();
        /* iter next received mode */
        int mode = JV_INT(sp[-1]);
        JSValue iter = sp[-4], received = sp[-2];
        JSValue res;
        if (mode == RESUME_NEXT) res = kz_call(ctx, sp[-3], iter, 1, &sp[-2]);
        else {
            JSValue m = kz_get_method(ctx, iter, rt->atom[mode == RESUME_THROW ? KZ_ATOM_throw : KZ_ATOM_return]);
            if (m == JS_EXCEPTION) { pc = at + 9; THROW(); }
            if (m == JS_UNDEFINED) {
                if (mode == RESUME_RETURN) {
                    /* complete the generator with received */
                    sp[-4] = received;
                    sp -= 3;
                    pc = at + ret_off;
                    DISPATCH();
                }
                /* no throw method: close the iterator, then a TypeError */
                KzIter it = { iter, sp[-3], false };
                JSValue c = kz_iter_close(ctx, &it, JS_UNDEFINED);
                pc = at + 9;
                if (c == JS_EXCEPTION) THROW();
                kz_throw_type(ctx, "The iterator does not provide a 'throw' method");
                THROW();
            }
            JSValue rm = m;
            KzRootFrame rmf = { rt->roots, rt, &rm, 1, NULL };
            rt->roots = &rmf;
            res = kz_call(ctx, rm, iter, 1, &sp[-2]);
            rt->roots = rmf.prev;
        }
        if (res == JS_EXCEPTION) { pc = at + 9; THROW(); }
        if (!JV_IS_OBJECT(res)) { pc = at + 9; kz_throw_type(ctx, "Iterator result is not an object"); THROW(); }
        sp[-2] = res;                       /* keep it rooted */
        JSValue d = kz_get(ctx, res, rt->atom[KZ_ATOM_done], res);
        if (d == JS_EXCEPTION) { pc = at + 9; THROW(); }
        if (kz_to_bool(d)) {
            JSValue v = kz_get(ctx, sp[-2], rt->atom[KZ_ATOM_value], sp[-2]);
            if (v == JS_EXCEPTION) { pc = at + 9; THROW(); }
            sp[-4] = v;
            sp -= 3;
            pc = at + (mode == RESUME_RETURN ? ret_off : done_off);
            DISPATCH();
        }
        /* yield the inner result object as it is; resumption pushes received mode and re-runs this */
        sp -= 2;
        pc = at;
        f->pc = pc;
        f->sp = sp;
        rt->suspend_kind = KZ_SUSPEND_YIELD_RAW;
        rt->suspend_value = res;
        ret = JS_EMPTY;
        goto suspend;
    }
L_async_yield_star: {
        const uint8_t *at = pc;
        int32_t ret_off = rdi32(pc + 1);
        pc += 5;
        SAVE();
        int mode = JV_INT(sp[-1]);
        JSValue iter = sp[-4];
        JSValue res;
        if (mode == RESUME_NEXT) res = kz_call(ctx, sp[-3], iter, 1, &sp[-2]);
        else {
            JSValue m = kz_get_method(ctx, iter, rt->atom[mode == RESUME_THROW ? KZ_ATOM_throw : KZ_ATOM_return]);
            CHECK(m);
            if (m == JS_UNDEFINED) {
                if (mode == RESUME_RETURN) {
                    /* iter next received */
                    sp--;
                    pc = at + ret_off;
                    DISPATCH();
                }
                KzIter it = { iter, sp[-3], false };
                JSValue c = kz_iter_close(ctx, &it, JS_UNDEFINED);
                CHECK(c);
                kz_throw_type(ctx, "The iterator does not provide a 'throw' method");
                THROW();
            }
            JSValue rm = m;
            KzRootFrame rmf = { rt->roots, rt, &rm, 1, NULL };
            rt->roots = &rmf;
            res = kz_call(ctx, rm, iter, 1, &sp[-2]);
            rt->roots = rmf.prev;
        }
        CHECK(res);
        sp[-2] = res;
        sp--;                               /* iter next res */
        DISPATCH();
    }
L_await:
        pc++;
        sp--;
        SAVE();
        rt->suspend_kind = KZ_SUSPEND_AWAIT;
        rt->suspend_value = *sp;
        ret = JS_EMPTY;
        goto suspend;

    /* ------------------------------------------------------------ literals */
L_object: {
        pc++;
        SAVE();
        JSValue o = kz_new_object(ctx);
        CHECK(o);
        *sp++ = o;
        DISPATCH();
    }
L_array_from: {
        uint16_t n = rd16(pc + 1);
        pc += 3;
        SAVE();
        JSValue a = kz_new_array_from(ctx, n, sp - n);
        CHECK(a);
        sp -= n;
        *sp++ = a;
        DISPATCH();
    }
L_array_append: {
        pc++;
        SAVE();
        JSValue src = sp[-1];
        JSObject *s = JV_IS_OBJECT(src) ? JV_OBJ(src) : NULL;
        /* fast path: a plain array with the original iterator */
        if (s && kz_array_is_fast(s) && !(s->h.flags & OBJ_EXOTIC)) {
            JSValue m = kz_get(ctx, src, rt->sym[KZ_SYM_iterator], src);
            CHECK(m);
            if (m == kz_intr(ctx, KZ_I_ARRAY_PROTO_VALUES)) {
                extern bool kz_array_iterator_pristine(JSContext *ctx);
                if (kz_array_iterator_pristine(ctx)) {
                    for (uint32_t i = 0; i < JV_OBJ(sp[-1])->elem_len; i++) {
                        JSValue v = JV_OBJ(sp[-1])->elems[i];
                        if (v == JS_HOLE) v = JS_UNDEFINED;
                        CHECK_I(kz_array_push(ctx, JV_OBJ(sp[-2]), v));
                    }
                    sp--;
                    DISPATCH();
                }
            }
        }
        CHECK_I(vm_append_iterable(ctx, sp[-2], sp[-1]));
        sp--;
        DISPATCH();
    }
L_array_push: {
        pc++;
        SAVE();
        CHECK_I(kz_array_push(ctx, JV_OBJ(sp[-2]), sp[-1]));
        sp--;
        DISPATCH();
    }
L_array_hole: {
        pc++;
        JSObject *a = JV_OBJ(sp[-1]);
        a->u.array.length++;
        DISPATCH();
    }
L_template_object: {
        uint32_t ci = rd32(pc + 1);
        pc += 5;
        SAVE();
        JSValue t = template_object(ctx, b, ci);
        CHECK(t);
        *sp++ = t;
        DISPATCH();
    }
L_regexp: {
        pc++;
        SAVE();
        JSValue r = kz_regexp_create(ctx, sp[-2], sp[-1]);
        CHECK(r);
        sp[-2] = r;
        sp--;
        DISPATCH();
    }
L_special_object: {
        int kind = pc[1];
        pc += 2;
        SAVE();
        JSValue o = special_object(ctx, f, kind);
        CHECK(o);
        *sp++ = o;
        DISPATCH();
    }
L_rest: {
        uint16_t first = rd16(pc + 1);
        pc += 3;
        SAVE();
        uint32_t n = f->argc > first ? f->argc - first : 0;
        JSValue a = kz_new_array_from(ctx, n, f->argv + first);
        CHECK(a);
        *sp++ = a;
        DISPATCH();
    }
L_check_coercible: {
        pc++;
        if (JV_IS_NULLISH(sp[-1])) {
            SAVE();
            kz_throw_type(ctx, "Cannot destructure '%s' as it is %s.", sp[-1] == JS_NULL ? "null" : "undefined", sp[-1] == JS_NULL ? "null" : "undefined");
            THROW();
        }
        DISPATCH();
    }
L_check_object: {
        pc++;
        if (!JV_IS_OBJECT(sp[-1])) { SAVE(); kz_throw_type(ctx, "Iterator result is not an object"); THROW(); }
        DISPATCH();
    }
L_check_this: pc++; DISPATCH();
L_bind_this: pc++; DISPATCH();
L_push_import_meta: {
        pc++;
        SAVE();
        JSValue m = kz_import_meta(ctx, b);
        CHECK(m);
        *sp++ = m;
        DISPATCH();
    }
L_throw_iterator_result: pc++; DISPATCH();
L_line: pc++; DISPATCH();

    /* ------------------------------------------------------------ return, exceptions, suspension */
do_return: {
        JSFrame *done = f;
        if ((done->flags & FRAME_CONSTRUCT) && !JV_IS_OBJECT(ret) && kz_bc_kind(done->b) != KZ_FUNC_DERIVED_CTOR) ret = done->this_val;
        if (done->flags & FRAME_ENTRY) {
            if (done->flags & FRAME_HEAP) { rt->frame = done->prev; rt->depth--; }
            else pop_frame(rt, done);
            rt->suspend_kind = KZ_SUSPEND_NONE;
            return ret;
        }
        pop_frame(rt, done);
        f = done->prev;
        LOAD_FRAME();
        pc = f->pc;
        sp = f->sp;
        *sp++ = ret;
        DISPATCH();
    }
suspend: {
        /* only heap frames (the entry frame) suspend */
        rt->frame = f->prev;
        rt->depth--;
        return ret;
    }
exception: {
        for (;;) {
            if (!rt->exc_uncatchable) {
                uint32_t off = (uint32_t)(pc - b->code);
                if (off) off--;
                KzExcEntry *h = NULL;
                for (uint32_t i = 0; i < b->nexc; i++)
                    if (off >= b->exc[i].start && off < b->exc[i].end) { h = &b->exc[i]; break; }
                if (h) {
                    sp = regs + b->nlocals + h->depth;
                    *sp++ = kz_take_exception(ctx);
                    pc = b->code + h->handler;
                    DISPATCH();
                }
            }
            JSFrame *done = f;
            if (done->flags & FRAME_ENTRY) {
                if (done->flags & FRAME_HEAP) { rt->frame = done->prev; rt->depth--; }
                else pop_frame(rt, done);
                rt->suspend_kind = KZ_SUSPEND_NONE;
                return JS_EXCEPTION;
            }
            pop_frame(rt, done);
            f = done->prev;
            LOAD_FRAME();
            pc = f->pc;
            sp = f->sp;
        }
    }
    (void)entry;
}

/* ================================================================== special objects */

extern JSValue kz_new_arguments(JSContext *ctx, JSFrame *f, bool mapped);

static JSValue special_object(JSContext *ctx, JSFrame *f, int kind) {
    switch (kind) {
    case SO_ARGUMENTS: return kz_new_arguments(ctx, f, false);
    case SO_MAPPED_ARGUMENTS: return kz_new_arguments(ctx, f, true);
    default: return JS_UNDEFINED;
    }
}
