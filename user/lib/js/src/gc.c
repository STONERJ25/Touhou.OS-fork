/*
 * gc.c: the heap allocator and the garbage collector.
 *
 * Allocation.  Small blocks (up to 1 KiB with their 8-byte header) come from 64 KiB pages split into cells of one size
 * class each; free cells of a class form one free list.  Bigger blocks go to the system allocator with a 32-byte
 * prefix that links the garbage-collected ones into a list for the sweep.  Garbage-collected cells and plain buffers
 * (kz_malloc: slot arrays, string data, bytecode) share the pages; a buffer's header says KZ_GC_RAW and the collector
 * skips it, its owner frees it.  Every byte taken from the system counts against the runtime's memory limit; when an
 * allocation would pass it the collector runs first, and if that does not help the allocation fails with the
 * preallocated "out of memory" RangeError (throwing it allocates nothing).
 *
 * Collection is precise, non-moving mark and sweep:
 *   1. The roots are marked: C root frames (KZ_ROOTS), pinned values, every context's globals and intrinsics, the
 *      call frames, the job queue, permanent atoms and symbols.
 *   2. Marked cells are pushed on an explicit mark stack and traced until it is empty, so a deep object graph never
 *      recurses on the C stack.  If the mark stack cannot grow, the overflow is remembered and the heap is rescanned
 *      for marked cells whose children may be unmarked, until nothing is left (slow but bounded memory).
 *   3. Weak things: WeakMap/WeakSet entries are ephemerons (an entry's value is marked once its key is), iterated to a
 *      fixpoint; then WeakRefs to unmarked targets are cleared and FinalizationRegistry cells with unmarked targets
 *      queue their cleanup jobs.  The atom table, the shape transition lists and the root-shape table are weak too.
 *   4. The sweep finalises every unmarked cell (freeing its buffers) and rebuilds the free lists; pages that became
 *      empty go back to the system.
 * "Marked" means header.mark == rt->epoch, and the epoch alternates between 1 and 2, so marks never need clearing and
 * liveness questions asked during the sweep (is this shape's parent alive?) have stable answers.
 */
#include "jsint.h"

#define KZ_PAGE_SIZE (64 * 1024)

struct KzPage {
    KzPage *next;
    uint32_t cls, cell_size, ncells, nfree;
    uint8_t *cells;
};

struct KzLarge {
    KzLarge *next, *prev;
    size_t size;            /* bytes requested by the system allocator (prefix included) */
    uint64_t gc;            /* 1: a garbage-collected cell (listed), 0: a raw buffer */
};
#define LARGE_HDR sizeof(KzLarge)
#define RAW_LARGE 0x8000    /* RAW header flag: the buffer has a KzLarge prefix */

static const uint16_t class_size[KZ_NUM_CLASSES] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024
};
static uint8_t size_to_class[KZ_MAX_SMALL / 16 + 1];
static bool tables_ready;
static uint8_t g_epoch_unused;

static void init_tables(void) {
    if (tables_ready) return;
    int c = 0;
    for (int i = 0; i <= KZ_MAX_SMALL / 16; i++) {
        while (class_size[c] < i * 16) c++;
        size_to_class[i] = (uint8_t)c;
    }
    tables_ready = true;
    (void)g_epoch_unused;
}

/* the runtime's mark epoch lives in a spare field: the low bits of gc_count */
#define EPOCH(rt) ((uint8_t)(((rt)->gc_count & 1) + 1))
static inline bool is_marked(JSRuntime *rt, const void *cell) { return ((const JSGCHeader *)cell)->mark == EPOCH(rt); }

/* ------------------------------------------------------------------ the system side */

static void *sys_alloc(JSRuntime *rt, size_t n) { return rt->sys_malloc(rt->sys_opaque, n); }
static void sys_release(JSRuntime *rt, void *p) { rt->sys_free(rt->sys_opaque, p); }

/* Can `n` more bytes be taken from the system?  Collects first when the limit is near. */
static bool reserve(JSRuntime *rt, size_t n) {
    if (!rt->mem_limit || rt->mem_used + n <= rt->mem_limit) return true;
    if (!rt->gc_running && !rt->gc_disabled) kz_gc_collect(rt);
    return rt->mem_used + n <= rt->mem_limit;
}

static void account(JSRuntime *rt, size_t n) {
    rt->mem_used += n;
    if (rt->mem_used > rt->mem_peak) rt->mem_peak = rt->mem_used;
}

static KzPage *new_page(JSRuntime *rt, int cls) {
    if (!reserve(rt, KZ_PAGE_SIZE)) return NULL;
    KzPage *p = sys_alloc(rt, KZ_PAGE_SIZE);
    if (!p) return NULL;
    account(rt, KZ_PAGE_SIZE);
    p->cls = (uint32_t)cls;
    p->cell_size = class_size[cls];
    size_t hdr = (sizeof(KzPage) + 15) & ~(size_t)15;
    p->cells = (uint8_t *)p + hdr;
    p->ncells = (uint32_t)((KZ_PAGE_SIZE - hdr) / p->cell_size);
    p->nfree = p->ncells;
    /* thread the cells onto the class free list, in address order */
    void **link = &rt->free_cells[cls];
    void *rest = *link;
    for (uint32_t i = 0; i < p->ncells; i++) {
        uint8_t *c = p->cells + (size_t)i * p->cell_size;
        ((JSGCHeader *)c)->gctype = KZ_GC_FREE;
        ((JSGCHeader *)c)->mark = 0;
        *link = c;
        link = (void **)(c + 8);
    }
    *link = rest;
    p->next = rt->pages[cls];
    rt->pages[cls] = p;
    return p;
}

/* A cell of `total` bytes (header included), uninitialised apart from being taken off its free list. */
static void *cell_alloc(JSRuntime *rt, size_t total, bool gc) {
    if (total <= KZ_MAX_SMALL) {
        int cls = size_to_class[(total + 15) >> 4];
        void *c = rt->free_cells[cls];
        if (!c) {
            if (!new_page(rt, cls)) return NULL;
            c = rt->free_cells[cls];
        }
        rt->free_cells[cls] = *(void **)((uint8_t *)c + 8);
        rt->gc_alloc_bytes += class_size[cls];
        return c;
    }
    if (total > ((size_t)1 << 40) || !reserve(rt, total + LARGE_HDR)) return NULL;
    KzLarge *l = sys_alloc(rt, total + LARGE_HDR);
    if (!l) return NULL;
    account(rt, total + LARGE_HDR);
    rt->gc_alloc_bytes += total;
    l->size = total + LARGE_HDR;
    l->gc = gc;
    l->prev = NULL;
    l->next = NULL;
    if (gc) {
        l->next = rt->large;
        if (rt->large) rt->large->prev = l;
        rt->large = l;
    }
    return (uint8_t *)l + LARGE_HDR;
}

static void maybe_collect(JSRuntime *rt) {
    if (rt->gc_running || rt->gc_disabled) return;
    if (rt->gc_stress && ++rt->gc_stress_count >= rt->gc_stress_every) { rt->gc_stress_count = 0; kz_gc_collect(rt); }
    else if (rt->gc_alloc_bytes >= rt->gc_threshold) kz_gc_collect(rt);
}

void kz_gc_maybe(JSContext *ctx) { maybe_collect(ctx->rt); }

JSValue kz_throw_oom(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    rt->exception = rt->oom_error;
    rt->exc_uncatchable = false;
    return JS_EXCEPTION;
}

void *kz_gc_new(JSContext *ctx, KzGCType type, size_t size) {
    JSRuntime *rt = ctx->rt;
    maybe_collect(rt);
    void *c = cell_alloc(rt, size, true);
    if (!c) { kz_throw_oom(ctx); return NULL; }
    memset(c, 0, size);
    ((JSGCHeader *)c)->gctype = (uint8_t)type;
    return c;
}

/* ------------------------------------------------------------------ raw buffers */

static void *raw_alloc(JSRuntime *rt, size_t n, bool collect) {
    if (n > ((size_t)1 << 40)) return NULL;
    if (collect) maybe_collect(rt);
    size_t total = n + 8;
    uint8_t *c = cell_alloc(rt, total, false);
    if (!c) return NULL;
    JSGCHeader *h = (JSGCHeader *)c;
    h->gctype = KZ_GC_RAW;
    h->mark = 0;
    h->flags = total > KZ_MAX_SMALL ? RAW_LARGE : 0;
    h->aux = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
    return c + 8;
}

size_t kz_alloc_size(const void *p) {
    const JSGCHeader *h = (const JSGCHeader *)((const uint8_t *)p - 8);
    if (h->flags & RAW_LARGE) {
        const KzLarge *l = (const KzLarge *)((const uint8_t *)h - LARGE_HDR);
        return l->size - LARGE_HDR - 8;
    }
    int cls = size_to_class[((size_t)h->aux + 8 + 15) >> 4];
    return (size_t)class_size[cls] - 8;
}

void *kz_rt_malloc(JSRuntime *rt, size_t n) { return raw_alloc(rt, n, true); }

void *kz_malloc(JSContext *ctx, size_t n) {
    void *p = raw_alloc(ctx->rt, n, true);
    if (!p) kz_throw_oom(ctx);
    return p;
}

void *kz_mallocz(JSContext *ctx, size_t n) {
    void *p = kz_malloc(ctx, n);
    if (p) memset(p, 0, n);
    return p;
}

void kz_free(JSRuntime *rt, void *p) {
    if (!p) return;
    JSGCHeader *h = (JSGCHeader *)((uint8_t *)p - 8);
    if (h->flags & RAW_LARGE) {
        KzLarge *l = (KzLarge *)((uint8_t *)h - LARGE_HDR);
        rt->mem_used -= l->size;
        sys_release(rt, l);
        return;
    }
#ifdef KZ_POISON
    memset(p, 0xDD, kz_alloc_size(p));      /* (a debug build: see sweep) */
#endif
    h->gctype = KZ_GC_FREE;
    h->mark = 0;
    if (rt->gc_running) return;             /* the sweep rebuilds the free lists */
    int cls = size_to_class[((size_t)h->aux + 8 + 15) >> 4];
    *(void **)((uint8_t *)h + 8) = rt->free_cells[cls];
    rt->free_cells[cls] = h;
}

void *kz_rt_realloc(JSRuntime *rt, void *p, size_t n) {
    if (!p) return kz_rt_malloc(rt, n);
    size_t have = kz_alloc_size(p);
    JSGCHeader *h = (JSGCHeader *)((uint8_t *)p - 8);
    if (n <= have && !(h->flags & RAW_LARGE) && n + 8 > (have + 8) / 2) {
        h->aux = (uint32_t)have;            /* still names this cell's size class */
        return p;
    }
    size_t old = (h->flags & RAW_LARGE) ? have : h->aux;
    void *q = kz_rt_malloc(rt, n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    kz_free(rt, p);
    return q;
}

void *kz_realloc(JSContext *ctx, void *p, size_t n) {
    void *q = kz_rt_realloc(ctx->rt, p, n);
    if (!q) kz_throw_oom(ctx);
    return q;
}

/* ------------------------------------------------------------------ roots */

void kz_root_pop(KzRootFrame *f) { f->rt->roots = f->prev; }

void kz_vec_root_pop(KzRootFrame *f) {
    f->rt->roots = f->prev;
    if (f->vec->v) kz_free(f->rt, f->vec->v);
    f->vec->v = NULL;
    f->vec->len = f->vec->cap = 0;
}

int kz_vec_push(JSContext *ctx, KzVec *v, JSValue x) {
    if (v->len == v->cap) {
        KZ_ROOT(ctx, rx);               /* x is not in the vector yet: keep it alive while the buffer grows */
        rx = x;
        uint32_t nc = v->cap ? v->cap * 2 : 8;
        JSValue *nv = kz_realloc(ctx, v->v, (size_t)nc * sizeof(JSValue));
        if (!nv) return -1;
        v->v = nv;
        v->cap = nc;
        x = rx;
    }
    v->v[v->len++] = x;
    return 0;
}

static uint32_t pin_hash(JSValue v) { return (uint32_t)((v * 0x9E3779B97F4A7C15ULL) >> 32); }

static bool pins_grow(JSRuntime *rt) {
    uint32_t nc = rt->pin_cap ? rt->pin_cap * 2 : 64;
    KzPinEntry *n = sys_alloc(rt, nc * sizeof(KzPinEntry));
    if (!n) return false;
    for (uint32_t i = 0; i < nc; i++) { n[i].v = JS_EMPTY; n[i].count = 0; }
    for (uint32_t i = 0; i < rt->pin_cap; i++) {
        if (rt->pins[i].v == JS_EMPTY || !rt->pins[i].count) continue;
        uint32_t j = pin_hash(rt->pins[i].v) & (nc - 1);
        while (n[j].v != JS_EMPTY) j = (j + 1) & (nc - 1);
        n[j] = rt->pins[i];
    }
    if (rt->pins) sys_release(rt, rt->pins);
    rt->pins = n;
    rt->pin_cap = nc;
    return true;
}

void kz_pin(JSRuntime *rt, JSValue v) {
    if (!JV_IS_HEAP(v)) return;
    if ((rt->pin_count + 1) * 2 > rt->pin_cap && !pins_grow(rt)) return;
    uint32_t m = rt->pin_cap - 1, j = pin_hash(v) & m, tomb = UINT32_MAX;
    while (rt->pins[j].v != JS_EMPTY) {
        if (rt->pins[j].v == v && rt->pins[j].count) { rt->pins[j].count++; return; }
        if (!rt->pins[j].count && tomb == UINT32_MAX) tomb = j;
        j = (j + 1) & m;
    }
    if (tomb != UINT32_MAX) j = tomb;
    rt->pins[j].v = v;
    rt->pins[j].count = 1;
    rt->pin_count++;
}

void kz_unpin(JSRuntime *rt, JSValue v) {
    if (!JV_IS_HEAP(v) || !rt->pin_cap) return;
    uint32_t m = rt->pin_cap - 1, j = pin_hash(v) & m;
    while (rt->pins[j].v != JS_EMPTY) {
        if (rt->pins[j].v == v && rt->pins[j].count) {
            if (--rt->pins[j].count == 0) rt->pin_count--;      /* stays as a tombstone (count 0) */
            return;
        }
        j = (j + 1) & m;
    }
}

/* ------------------------------------------------------------------ marking */

static void mark_push(JSRuntime *rt, void *cell) {
    if (rt->mark_sp == rt->mark_cap) {
        size_t nc = rt->mark_cap ? rt->mark_cap * 2 : 4096;
        JSValue *n = rt->sys_realloc(rt->sys_opaque, rt->mark_stack, nc * sizeof(JSValue));
        if (!n) { rt->mark_overflow = true; return; }
        rt->mark_stack = n;
        rt->mark_cap = nc;
    }
    rt->mark_stack[rt->mark_sp++] = (JSValue)(uintptr_t)cell;
}

void kz_mark_cell(JSRuntime *rt, void *cell) {
    if (!cell) return;
    JSGCHeader *h = cell;
    uint8_t e = EPOCH(rt);
    if (h->mark == e) return;
    h->mark = e;
    switch (h->gctype) {
    case KZ_GC_SOURCE: case KZ_GC_BIGINT: case KZ_GC_RAW: case KZ_GC_FREE:
        return;                                     /* nothing inside */
    case KZ_GC_STRING:
        if (!(h->flags & STR_ROPE)) return;
        break;
    default:
        break;
    }
    mark_push(rt, cell);
}

void kz_mark_value(JSRuntime *rt, JSValue v) {
    if (JV_IS_HEAP(v)) kz_mark_cell(rt, JV_PTR(v));
}

static void mark_values(JSRuntime *rt, const JSValue *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (JV_IS_HEAP(v[i])) kz_mark_cell(rt, JV_PTR(v[i]));
}

/* weak objects met while marking, processed after the main marking */
static JSObject **g_unused_weak;
typedef struct WeakList { JSObject **v; size_t n, cap; bool failed; } WeakList;
static WeakList *cur_weak(JSRuntime *rt) { return (WeakList *)(void *)rt->weak_list; }

static void weak_add(JSRuntime *rt, JSObject *o) {
    WeakList *w = cur_weak(rt);
    if (!w) return;
    if (w->n == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 64;
        JSObject **n = rt->sys_realloc(rt->sys_opaque, w->v, nc * sizeof(JSObject *));
        if (!n) { w->failed = true; return; }
        w->v = n;
        w->cap = nc;
    }
    w->v[w->n++] = o;
    (void)g_unused_weak;
}

struct JSTracer { JSRuntime *rt; };

void js_trace_value(JSTracer *trc, JSValue v) { kz_mark_value(trc->rt, v); }

static void trace_object(JSRuntime *rt, JSObject *o) {
    if (o->shape) {
        kz_mark_cell(rt, o->shape);
        mark_values(rt, o->slots, o->slots ? o->shape->nprops : 0);
    }
    if (o->elems) mark_values(rt, o->elems, o->elem_len);
    switch (kz_class(o)) {
    case KZ_CLASS_FUNCTION:
        kz_mark_cell(rt, o->u.func.b);
        if (o->u.func.cells && o->u.func.b)
            for (uint32_t i = 0; i < o->u.func.b->ncvars; i++) kz_mark_cell(rt, o->u.func.cells[i]);
        kz_mark_value(rt, o->u.func.home);
        kz_mark_value(rt, o->u.func.fields);
        break;
    case KZ_CLASS_NATIVE:
        if (o->u.native.data) mark_values(rt, o->u.native.data, o->u.native.ndata);
        break;
    case KZ_CLASS_BOUND:
        kz_mark_value(rt, o->u.bound.target);
        kz_mark_value(rt, o->u.bound.this_val);
        if (o->u.bound.args) mark_values(rt, o->u.bound.args, o->u.bound.argc);
        break;
    case KZ_CLASS_BOOLEAN: case KZ_CLASS_NUMBER: case KZ_CLASS_STRING: case KZ_CLASS_SYMBOL: case KZ_CLASS_BIGINT:
        kz_mark_value(rt, o->u.prim);
        break;
    case KZ_CLASS_REGEXP:
        kz_mark_value(rt, o->u.regexp.source);
        kz_mark_value(rt, o->u.regexp.flags);
        break;
    case KZ_CLASS_MAP: case KZ_CLASS_SET:
        kz_map_trace(rt, o);
        break;
    case KZ_CLASS_WEAKMAP: case KZ_CLASS_WEAKSET: case KZ_CLASS_WEAKREF:
        weak_add(rt, o);
        if (cur_weak(rt) && cur_weak(rt)->failed) {           /* no room to remember it: treat it as strong */
            if (kz_class(o) == KZ_CLASS_WEAKREF) kz_mark_value(rt, o->u.weakref.target);
            else kz_map_trace(rt, o);
        }
        break;
    case KZ_CLASS_FINREG:
        kz_finreg_trace(rt, o);
        weak_add(rt, o);
        break;
    case KZ_CLASS_PROMISE:
        kz_promise_trace(rt, o);
        break;
    case KZ_CLASS_PROXY:
        kz_mark_value(rt, o->u.proxy.target);
        kz_mark_value(rt, o->u.proxy.handler);
        break;
    case KZ_CLASS_UINT8C_ARRAY: case KZ_CLASS_INT8_ARRAY: case KZ_CLASS_UINT8_ARRAY: case KZ_CLASS_INT16_ARRAY:
    case KZ_CLASS_UINT16_ARRAY: case KZ_CLASS_INT32_ARRAY: case KZ_CLASS_UINT32_ARRAY: case KZ_CLASS_FLOAT32_ARRAY:
    case KZ_CLASS_FLOAT64_ARRAY: case KZ_CLASS_BIGINT64_ARRAY: case KZ_CLASS_BIGUINT64_ARRAY: case KZ_CLASS_DATAVIEW:
        kz_mark_value(rt, o->u.ta.buffer);
        break;
    case KZ_CLASS_GENERATOR: case KZ_CLASS_ASYNC_GENERATOR: case KZ_CLASS_ASYNC_FUNCTION:
        kz_generator_trace(rt, o);
        break;
    case KZ_CLASS_ARRAY_ITERATOR: case KZ_CLASS_STRING_ITERATOR: case KZ_CLASS_MAP_ITERATOR:
    case KZ_CLASS_SET_ITERATOR: case KZ_CLASS_REGEXP_STRING_ITERATOR: case KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR:
    case KZ_CLASS_ITERATOR_HELPER:
        kz_mark_value(rt, o->u.any.a);
        kz_mark_value(rt, o->u.any.b);
        kz_mark_value(rt, o->u.any.c);
        break;
    case KZ_CLASS_FOR_IN_ITERATOR:
        kz_mark_value(rt, o->u.forin.obj);
        kz_mark_value(rt, o->u.forin.keys);
        kz_mark_cell(rt, o->u.forin.shape);
        break;
    case KZ_CLASS_MAPPED_ARGUMENTS:
        if (o->u.margs.cells)
            for (uint32_t i = 0; i < o->u.margs.nmapped; i++) kz_mark_cell(rt, o->u.margs.cells[i]);
        break;
    case KZ_CLASS_MODULE_NS:
        kz_mark_cell(rt, o->u.ns.module);
        break;
    case KZ_CLASS_HOST:
        if (o->u.host.def && o->u.host.def->trace && o->u.host.opaque) {
            JSTracer t = { rt };
            o->u.host.def->trace(&t, o->u.host.opaque);
        }
        break;
    default:
        break;
    }
}

static void trace_cell(JSRuntime *rt, void *cell) {
    JSGCHeader *h = cell;
    switch (h->gctype) {
    case KZ_GC_STRING: {
        JSString *s = cell;
        if (h->flags & STR_ROPE) {
            kz_mark_cell(rt, s->u.rope.left);
            kz_mark_cell(rt, s->u.rope.right);
        }
        break;
    }
    case KZ_GC_SYMBOL:
        kz_mark_value(rt, ((JSSymbol *)cell)->description);
        break;
    case KZ_GC_OBJECT:
        trace_object(rt, cell);
        break;
    case KZ_GC_SHAPE: {
        JSShape *s = cell;
        kz_mark_value(rt, s->proto);
        kz_mark_cell(rt, s->parent);
        if (s->props)
            for (uint32_t i = 0; i < s->nprops; i++) kz_mark_value(rt, s->props[i].key);
        break;
    }
    case KZ_GC_BYTECODE: {
        JSFunctionBytecode *b = cell;
        if (b->consts) mark_values(rt, b->consts, b->nconsts);
        kz_mark_value(rt, b->name);
        kz_mark_value(rt, b->filename);
        kz_mark_value(rt, b->source);
        kz_mark_value(rt, b->module_desc);
        if (b->ic)
            for (uint32_t i = 0; i < b->nic; i++) {
                kz_mark_cell(rt, b->ic[i].shape);
                kz_mark_cell(rt, b->ic[i].holder);
                kz_mark_cell(rt, b->ic[i].holder_shape);
            }
        if (b->varnames)
            for (uint32_t i = 0; i < b->nvarnames; i++) kz_mark_value(rt, b->varnames[i].name);
        break;
    }
    case KZ_GC_VARCELL:
        kz_mark_value(rt, ((JSVarCell *)cell)->value);
        break;
    case KZ_GC_ACCESSOR:
        kz_mark_value(rt, ((JSAccessor *)cell)->getter);
        kz_mark_value(rt, ((JSAccessor *)cell)->setter);
        break;
    case KZ_GC_MODULE:
        kz_module_trace(rt, cell);
        break;
    default:
        break;
    }
}

static void drain(JSRuntime *rt) {
    while (rt->mark_sp) {
        void *cell = (void *)(uintptr_t)rt->mark_stack[--rt->mark_sp];
        trace_cell(rt, cell);
    }
}

/* After a mark-stack overflow: trace every marked cell again until no overflow remains. */
static void recover_overflow(JSRuntime *rt) {
    while (rt->mark_overflow) {
        rt->mark_overflow = false;
        for (int c = 0; c < KZ_NUM_CLASSES; c++)
            for (KzPage *p = rt->pages[c]; p; p = p->next)
                for (uint32_t i = 0; i < p->ncells; i++) {
                    JSGCHeader *h = (JSGCHeader *)(p->cells + (size_t)i * p->cell_size);
                    if (h->gctype > KZ_GC_RAW && h->mark == EPOCH(rt)) { trace_cell(rt, h); drain(rt); }
                }
        for (KzLarge *l = rt->large; l; l = l->next) {
            JSGCHeader *h = (JSGCHeader *)((uint8_t *)l + LARGE_HDR);
            if (h->mark == EPOCH(rt)) { trace_cell(rt, h); drain(rt); }
        }
    }
}

static void mark_context(JSRuntime *rt, JSContext *ctx) {
    kz_mark_value(rt, ctx->global);
    kz_mark_value(rt, ctx->global_lex);
    mark_values(rt, ctx->intrinsic, KZ_I_COUNT);
    kz_mark_cell(rt, ctx->shape_object);
    kz_mark_cell(rt, ctx->shape_array);
    kz_mark_cell(rt, ctx->shape_func_proto);
    kz_mark_cell(rt, ctx->shape_func);
    kz_mark_cell(rt, ctx->shape_error);
    kz_mark_cell(rt, ctx->shape_iter_result);
    kz_mark_cell(rt, ctx->shape_arguments);
    kz_mark_value(rt, ctx->template_cache);
    if (ctx->api_scope.v) mark_values(rt, ctx->api_scope.v, ctx->api_scope.len);
    if (ctx->modules.v) mark_values(rt, ctx->modules.v, ctx->modules.len);
}

static void mark_roots(JSRuntime *rt) {
    for (KzRootFrame *f = rt->roots; f; f = f->prev) {
        if (f->vec) { if (f->vec->v) mark_values(rt, f->vec->v, f->vec->len); }
        else mark_values(rt, f->vals, f->n);
    }
    for (uint32_t i = 0; i < rt->pin_cap; i++)
        if (rt->pins[i].count) kz_mark_value(rt, rt->pins[i].v);
    for (JSContext *c = rt->contexts; c; c = c->next) mark_context(rt, c);
    mark_values(rt, rt->atom, KZ_ATOM_COUNT);
    mark_values(rt, rt->char_str, 256);
    mark_values(rt, rt->sym, KZ_SYM_COUNT);
    kz_mark_value(rt, rt->symbol_registry);
    kz_mark_cell(rt, rt->null_root);
    kz_mark_value(rt, rt->oom_error);
    kz_mark_value(rt, rt->exception);
    kz_mark_value(rt, rt->new_target);
    kz_mark_cell(rt, rt->cur_native);
    kz_mark_value(rt, rt->suspend_value);
    if (rt->kept_alive.v) mark_values(rt, rt->kept_alive.v, rt->kept_alive.len);
    if (rt->unhandled.v) mark_values(rt, rt->unhandled.v, rt->unhandled.len);
    for (uint32_t i = 0; i < rt->finreg_npending; i++) kz_mark_cell(rt, rt->finreg_pending[i]);
    for (JSFrame *f = rt->frame; f; f = f->prev) kz_trace_frame(rt, f);
    for (KzJob *j = rt->job_head; j; j = j->next) mark_values(rt, j->args, j->argc);
}

/* ------------------------------------------------------------------ weak tables */

void kz_shapes_sweep(JSRuntime *rt);     /* object.c */

/* ------------------------------------------------------------------ sweeping */

static void free_object(JSRuntime *rt, JSObject *o) {
    if (o->slots) kz_free(rt, o->slots);
    if (o->elems) kz_free(rt, o->elems);
    switch (kz_class(o)) {
    case KZ_CLASS_FUNCTION:
        if (o->u.func.cells) kz_free(rt, o->u.func.cells);
        break;
    case KZ_CLASS_NATIVE:
        if (o->u.native.data) kz_free(rt, o->u.native.data);
        break;
    case KZ_CLASS_BOUND:
        if (o->u.bound.args) kz_free(rt, o->u.bound.args);
        break;
    case KZ_CLASS_REGEXP:
        if (o->u.regexp.code) kz_regexp_free(rt, o->u.regexp.code);
        break;
    case KZ_CLASS_MAP: case KZ_CLASS_SET: case KZ_CLASS_WEAKMAP: case KZ_CLASS_WEAKSET:
        kz_map_free(rt, o);
        break;
    case KZ_CLASS_MAP_ITERATOR: case KZ_CLASS_SET_ITERATOR:
        kz_map_iter_free(rt, o);
        break;
    case KZ_CLASS_FINREG:
        kz_finreg_free(rt, o);
        break;
    case KZ_CLASS_PROMISE:
        kz_promise_free(rt, o);
        break;
    case KZ_CLASS_ARRAYBUFFER: case KZ_CLASS_SHAREDARRAYBUFFER:
        if (o->u.ab.data && !o->u.ab.external) kz_free(rt, o->u.ab.data);
        break;
    case KZ_CLASS_GENERATOR: case KZ_CLASS_ASYNC_GENERATOR: case KZ_CLASS_ASYNC_FUNCTION:
        kz_generator_free(rt, o);
        break;
    case KZ_CLASS_MAPPED_ARGUMENTS:
        if (o->u.margs.cells) kz_free(rt, o->u.margs.cells);
        break;
    case KZ_CLASS_HOST:
        if (o->u.host.def && o->u.host.def->finalize) {
            rt->in_finalizer = true;
            o->u.host.def->finalize(rt, o->u.host.opaque);
            rt->in_finalizer = false;
        }
        break;
    default:
        break;
    }
}

static void remove_transition(JSRuntime *rt, JSShape *child) {
    JSShape *p = child->parent;
    if (!p || !is_marked(rt, p) || !p->trans) return;
    for (uint32_t i = 0; i < p->ntrans; i++)
        if (p->trans[i].child == child) {
            p->trans[i] = p->trans[--p->ntrans];
            return;
        }
}

static void finalize(JSRuntime *rt, JSGCHeader *h) {
    switch (h->gctype) {
    case KZ_GC_STRING: {
        JSString *s = (JSString *)h;
        if (h->flags & STR_EXT) kz_free(rt, s->u.ext);
        break;
    }
    case KZ_GC_OBJECT:
        free_object(rt, (JSObject *)h);
        break;
    case KZ_GC_SHAPE: {
        JSShape *s = (JSShape *)h;
        remove_transition(rt, s);
        if (s->props) kz_free(rt, s->props);
        if (s->hash) kz_free(rt, s->hash);
        if (s->trans) kz_free(rt, s->trans);
        break;
    }
    case KZ_GC_BYTECODE: {
        JSFunctionBytecode *b = (JSFunctionBytecode *)h;
        kz_free(rt, b->code);
        kz_free(rt, b->consts);
        kz_free(rt, b->cvars);
        kz_free(rt, b->exc);
        kz_free(rt, b->pos);
        kz_free(rt, b->ic);
        kz_free(rt, b->varnames);
        break;
    }
    case KZ_GC_MODULE:
        kz_module_free(rt, (KzModule *)h);
        break;
    default:
        break;
    }
}

static size_t g_live_obj, g_live_str;

static void sweep(JSRuntime *rt) {
    uint8_t e = EPOCH(rt);
    g_live_obj = g_live_str = 0;
    /* phase 1: finalise the dead (their buffers are freed into the pages, marked free, not listed) */
    for (int c = 0; c < KZ_NUM_CLASSES; c++)
        for (KzPage *p = rt->pages[c]; p; p = p->next)
            for (uint32_t i = 0; i < p->ncells; i++) {
                JSGCHeader *h = (JSGCHeader *)(p->cells + (size_t)i * p->cell_size);
                if (h->gctype <= KZ_GC_RAW) continue;
                if (h->mark == e) {
                    if (h->gctype == KZ_GC_OBJECT) g_live_obj++;
                    else if (h->gctype == KZ_GC_STRING) g_live_str++;
                    continue;
                }
                if (h->gctype == KZ_GC_STRING && (h->flags & STR_PERM)) continue;
                finalize(rt, h);
                h->gctype = KZ_GC_FREE;
                h->mark = 0;
#ifdef KZ_POISON
                /* a debug build (-DKZ_POISON): a dead cell is filled with 0xDD, so that a value the collector should have
                   kept shows up at once as a wild pointer instead of working until the memory is reused */
                memset((uint8_t *)h + sizeof(JSGCHeader), 0xDD, p->cell_size - sizeof(JSGCHeader));
#endif
            }
    KzLarge *l = rt->large;
    while (l) {
        KzLarge *next = l->next;
        JSGCHeader *h = (JSGCHeader *)((uint8_t *)l + LARGE_HDR);
        if (h->mark != e) {
            finalize(rt, h);
            if (l->prev) l->prev->next = l->next; else rt->large = l->next;
            if (l->next) l->next->prev = l->prev;
            rt->mem_used -= l->size;
            sys_release(rt, l);
        } else if (h->gctype == KZ_GC_OBJECT) g_live_obj++;
        else if (h->gctype == KZ_GC_STRING) g_live_str++;
        l = next;
    }
    /* phase 2: rebuild the free lists; give empty pages back (keeping one per class) */
    for (int c = 0; c < KZ_NUM_CLASSES; c++) {
        void *list = NULL;
        KzPage **pp = &rt->pages[c];
        bool kept_empty = false;
        while (*pp) {
            KzPage *p = *pp;
            uint32_t nfree = 0;
            for (uint32_t i = 0; i < p->ncells; i++)
                if (((JSGCHeader *)(p->cells + (size_t)i * p->cell_size))->gctype == KZ_GC_FREE) nfree++;
            if (nfree == p->ncells && kept_empty) {
                *pp = p->next;
                rt->mem_used -= KZ_PAGE_SIZE;
                sys_release(rt, p);
                continue;
            }
            if (nfree == p->ncells) kept_empty = true;
            /* push in reverse so allocation walks a page upwards */
            for (uint32_t i = p->ncells; i-- > 0;) {
                uint8_t *cell = p->cells + (size_t)i * p->cell_size;
                if (((JSGCHeader *)cell)->gctype == KZ_GC_FREE) {
                    *(void **)(cell + 8) = list;
                    list = cell;
                }
            }
            p->nfree = nfree;
            pp = &p->next;
        }
        rt->free_cells[c] = list;
    }
    rt->live_objects = g_live_obj;
    rt->live_strings = g_live_str;
}

void kz_gc_collect(JSRuntime *rt) {
    if (rt->gc_running || rt->gc_disabled) return;
    rt->gc_running = true;
    rt->gc_count++;                          /* flips the epoch: everything is unmarked now */
    WeakList wl = { NULL, 0, 0, false };
    rt->weak_list = (JSObject *)(void *)&wl;
    rt->mark_overflow = false;
    mark_roots(rt);
    drain(rt);
    recover_overflow(rt);
    kz_weak_process(rt);                     /* ephemerons, weak refs, finalization registries */
    drain(rt);
    recover_overflow(rt);
    rt->weak_list = NULL;
    if (wl.v) rt->sys_free(rt->sys_opaque, wl.v);
    kz_atoms_sweep(rt);
    kz_shapes_sweep(rt);
    sweep(rt);
    rt->gc_alloc_bytes = 0;
    size_t live = rt->mem_used;
    rt->gc_threshold = live < (2u << 20) ? (2u << 20) : live;
    if (rt->mem_limit && rt->gc_threshold > rt->mem_limit / 2) rt->gc_threshold = rt->mem_limit / 2;
    if (rt->mark_cap > 65536) {              /* do not keep a huge mark stack around */
        rt->sys_free(rt->sys_opaque, rt->mark_stack);
        rt->mark_stack = NULL;
        rt->mark_cap = 0;
    }
    rt->gc_running = false;
}

/* The weak list as seen by kz_weak_process (map.c) */
size_t kz_weak_count(JSRuntime *rt);
JSObject *kz_weak_at(JSRuntime *rt, size_t i);
void kz_weak_drain(JSRuntime *rt);
bool kz_is_marked(JSRuntime *rt, const void *cell);

size_t kz_weak_count(JSRuntime *rt) { return cur_weak(rt) ? cur_weak(rt)->n : 0; }
JSObject *kz_weak_at(JSRuntime *rt, size_t i) { return cur_weak(rt)->v[i]; }
void kz_weak_drain(JSRuntime *rt) { drain(rt); recover_overflow(rt); }
bool kz_is_marked(JSRuntime *rt, const void *cell) { return is_marked(rt, cell); }

/* ------------------------------------------------------------------ setup and teardown */

bool kz_rt_init_alloc(JSRuntime *rt) {
    init_tables();
    rt->gc_threshold = 2u << 20;
    return true;
}

void kz_rt_free_all(JSRuntime *rt) {
    /* Finalise everything (host finalizers see their objects go), then hand every page back.  The epoch moves on
       first, so nothing counts as marked and no finaliser looks at another dead cell. */
    rt->gc_running = true;
    rt->gc_count++;
    for (int c = 0; c < KZ_NUM_CLASSES; c++)
        for (KzPage *p = rt->pages[c]; p; p = p->next)
            for (uint32_t i = 0; i < p->ncells; i++) {
                JSGCHeader *h = (JSGCHeader *)(p->cells + (size_t)i * p->cell_size);
                if (h->gctype == KZ_GC_OBJECT && kz_class((JSObject *)h) == KZ_CLASS_HOST) finalize(rt, h);
            }
    for (KzLarge *l = rt->large; l; l = l->next) {
        JSGCHeader *h = (JSGCHeader *)((uint8_t *)l + LARGE_HDR);
        if (h->gctype == KZ_GC_OBJECT && kz_class((JSObject *)h) == KZ_CLASS_HOST) finalize(rt, h);
    }
    /* large raw buffers are not listed: free them through their owners */
    for (int c = 0; c < KZ_NUM_CLASSES; c++)
        for (KzPage *p = rt->pages[c]; p; p = p->next)
            for (uint32_t i = 0; i < p->ncells; i++) {
                JSGCHeader *h = (JSGCHeader *)(p->cells + (size_t)i * p->cell_size);
                if (h->gctype > KZ_GC_RAW && !(h->gctype == KZ_GC_OBJECT && kz_class((JSObject *)h) == KZ_CLASS_HOST))
                    finalize(rt, h);
            }
    for (KzLarge *l = rt->large; l; l = l->next) {
        JSGCHeader *h = (JSGCHeader *)((uint8_t *)l + LARGE_HDR);
        if (!(h->gctype == KZ_GC_OBJECT && kz_class((JSObject *)h) == KZ_CLASS_HOST)) finalize(rt, h);
    }
    kz_regexp_cache_free(rt);       /* (before the pages go: compiled patterns are raw buffers in them) */
    for (int c = 0; c < KZ_NUM_CLASSES; c++) {
        KzPage *p = rt->pages[c];
        while (p) { KzPage *n = p->next; sys_release(rt, p); p = n; }
        rt->pages[c] = NULL;
        rt->free_cells[c] = NULL;
    }
    KzLarge *l = rt->large;
    while (l) { KzLarge *n = l->next; sys_release(rt, l); l = n; }
    rt->large = NULL;
    if (rt->mark_stack) rt->sys_free(rt->sys_opaque, rt->mark_stack);
    if (rt->pins) sys_release(rt, rt->pins);
    if (rt->finreg_pending) sys_release(rt, rt->finreg_pending);
    rt->finreg_pending = NULL;
    rt->finreg_npending = rt->finreg_cap = 0;
    rt->mark_stack = NULL;
    rt->pins = NULL;
}
