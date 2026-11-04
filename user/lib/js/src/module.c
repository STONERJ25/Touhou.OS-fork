/*
 * module.c: ECMAScript modules (ECMA-262 16.2): loading a graph of source text modules through the host, linking it,
 * evaluating it (with top-level await), module namespace exotic objects (10.4.6), import.meta and import().
 *
 * A module is a KzModule cell in the context's module map (ctx->modules, keyed by the canonical name the host gave it).
 * Its code is compiled by compile.c into a generator-like function whose description for this file is an array in the
 * bytecode (see "modules" in compile.c): the specifiers it requests, its import entries (request, name, frame slot) and
 * its export entries (local: a frame slot; indirect: a request and the name there; star: a request).
 *
 * Bindings that cross a module boundary live in variable cells (JSVarCell).  The linker makes the cell of every local
 * export, shares the exporter's cell with every importer (so a binding is live: importers see later assignments, and
 * the temporal dead zone of let/const/class is the cell's JS_UNINIT), and writes the cells into the registers of the
 * module's frame before the frame runs its prologue.  A namespace object is a view of such cells.
 *
 * The phases follow the specification:
 *   load      (HostLoadImportedModule): the host's load_module hook is asked for each distinct specifier of each module,
 *             depth first; the answer is parsed and compiled (ParseModule) unless the map already has that name.
 *   link      (Link): all modules not linked yet that the root reaches are linked together in three steps (frames and
 *             export cells; resolving imports and indirect exports, SyntaxError when one does not resolve or is
 *             ambiguous; running the prologues).  Failure undoes all three so that a retry starts clean.
 *   evaluate  (Evaluate, InnerModuleEvaluation, ExecuteAsyncModule, AsyncModuleExecutionFulfilled/Rejected): the
 *             depth-first evaluation with strongly connected components ("cycle roots"), the evaluation order of
 *             async modules and their parents, and the promise of the entry module.
 * Graph walks recurse; every recursion checks the C stack (kz_check_stack), so a chain of 100000 modules ends in a
 * RangeError, not a crash.
 */
#include "jsint.h"
#include <stdlib.h>

extern char *kz_str_to_utf8_replace(JSContext *ctx, JSValue v, size_t *plen);

enum { MS_NEW, MS_LINKING, MS_LINKED, MS_EVALUATING, MS_EVALUATING_ASYNC, MS_EVALUATED };
enum { MX_LOCAL, MX_INDIRECT, MX_STAR };            /* export entry kinds (compile.c) */

typedef struct KzReq { JSValue spec; KzModule *mod; } KzReq;                      /* a requested specifier and what it loaded */
typedef struct KzImp { int32_t req, slot; JSValue name; } KzImp;                  /* name: the exported name, "*" the namespace */
typedef struct KzExp { int32_t kind, a; JSValue name, b; } KzExp;                 /* MX_LOCAL: a = slot, b = 1 if it starts in its TDZ;
                                                                                     MX_INDIRECT: a = request, b = name there;
                                                                                     MX_STAR: a = request */

struct KzModule {
    JSGCHeader h;
    uint8_t status;
    bool has_tla;               /* top-level await in this module's own code */
    bool loading, loaded;       /* the load walk is inside it / has finished with it */
    bool has_error;             /* [[EvaluationError]] is set: error */
    bool async_eval;            /* [[AsyncEvaluation]]: true from the decision to run it asynchronously until it ends */
    JSContext *realm;
    JSValue name;               /* the canonical name: an atom, also the bytecode's filename */
    JSValue code;               /* the module function's bytecode */
    JSValue gen;                /* between linking and evaluation: the state of the module's frame */
    JSValue ns;                 /* the namespace object, once made */
    JSValue meta;               /* import.meta, once made */
    JSValue error;
    JSValue top;                /* [[TopLevelCapability]]: a promise, or undefined */
    int32_t dfs_index, dfs_ancestor, pending;
    uint32_t async_order;       /* position in the order in which modules became async (for sorting) */
    KzModule *cycle_root;
    KzModule **parents;         /* [[AsyncParentModules]] */
    uint32_t nparents, cap_parents;
    uint32_t nreqs, nimps, nexps;
    KzReq *reqs;
    KzImp *imps;
    KzExp *exps;
    uint32_t nslots;            /* the frame's locals, for cells[] */
    JSVarCell **cells;          /* the cell of each local export / import, by frame slot (NULL: none) */
    uint32_t nns, ns_mask;      /* the namespace: nns exports, sorted by name; a hash of their keys */
    JSValue *ns_keys;           /* property keys (atoms, or array indexes) */
    JSVarCell **ns_cells;
    uint32_t *ns_hash;          /* index + 1, open addressing */
};

/* ================================================================== the GC side */

void kz_module_trace(JSRuntime *rt, KzModule *m) {
    kz_mark_value(rt, m->name);
    kz_mark_value(rt, m->code);
    kz_mark_value(rt, m->gen);
    kz_mark_value(rt, m->ns);
    kz_mark_value(rt, m->meta);
    kz_mark_value(rt, m->error);
    kz_mark_value(rt, m->top);
    kz_mark_cell(rt, m->cycle_root);
    for (uint32_t i = 0; i < m->nparents; i++) kz_mark_cell(rt, m->parents[i]);
    for (uint32_t i = 0; i < m->nreqs; i++) {
        kz_mark_value(rt, m->reqs[i].spec);
        kz_mark_cell(rt, m->reqs[i].mod);
    }
    for (uint32_t i = 0; i < m->nimps; i++) kz_mark_value(rt, m->imps[i].name);
    for (uint32_t i = 0; i < m->nexps; i++) {
        kz_mark_value(rt, m->exps[i].name);
        kz_mark_value(rt, m->exps[i].b);
    }
    for (uint32_t i = 0; i < m->nslots; i++) kz_mark_cell(rt, m->cells[i]);
    for (uint32_t i = 0; i < m->nns; i++) {
        kz_mark_value(rt, m->ns_keys[i]);
        kz_mark_cell(rt, m->ns_cells[i]);
    }
}

/* forgets the namespace's tables (the object, if it was made, is left to the collector: it finds no exports) */
static void ns_reset(JSRuntime *rt, KzModule *m) {
    m->ns = JS_UNDEFINED;
    m->nns = 0;
    m->ns_mask = 0;
    if (m->ns_keys) kz_free(rt, m->ns_keys);
    if (m->ns_cells) kz_free(rt, m->ns_cells);
    if (m->ns_hash) kz_free(rt, m->ns_hash);
    m->ns_keys = NULL;
    m->ns_cells = NULL;
    m->ns_hash = NULL;
}

/* drops everything linking made (the frame, the cells, the namespace) */
static void module_unlink(JSRuntime *rt, KzModule *m) {
    m->status = MS_NEW;
    m->gen = JS_UNDEFINED;
    ns_reset(rt, m);
    m->nslots = 0;
    if (m->cells) kz_free(rt, m->cells);
    m->cells = NULL;
}

void kz_module_free(JSRuntime *rt, KzModule *m) {
    module_unlink(rt, m);
    if (m->parents) kz_free(rt, m->parents);
    if (m->reqs) kz_free(rt, m->reqs);
    if (m->imps) kz_free(rt, m->imps);
    if (m->exps) kz_free(rt, m->exps);
    m->parents = NULL;
    m->reqs = NULL;
    m->imps = NULL;
    m->exps = NULL;
    m->nparents = m->nreqs = m->nimps = m->nexps = 0;
}

/* ================================================================== making modules */

static void cstr(JSValue v, char *buf, size_t cap) {
    if (JV_IS_STRING(v)) kz_str_to_utf8_buf(JV_STR(v), buf, cap);
    else if (cap) buf[0] = 0;
}

/* an atom for a name the host gave (UTF-8) */
static JSValue name_atom(JSContext *ctx, const char *s) {
    KZ_ROOT(ctx, str);
    str = kz_str_from_utf8(ctx, s, strlen(s));
    if (str == JS_EXCEPTION) return str;
    return kz_atom_from_str(ctx, str);
}

static KzModule *find_module(JSContext *ctx, JSValue name) {
    for (uint32_t i = 0; i < ctx->modules.len; i++) {
        KzModule *m = JV_PTR(ctx->modules.v[i]);
        if (m->name == name) return m;
    }
    return NULL;
}

/* reads the description compile.c left in the bytecode into the module's tables */
static int module_decode(JSContext *ctx, KzModule *m, JSFunctionBytecode *b) {
    JSValue desc = b->module_desc;
    if (!kz_is_class(desc, KZ_CLASS_ARRAY)) { kz_throw_syntax(ctx, "module has no description"); return -1; }
    const JSValue *e = JV_OBJ(desc)->elems;
    uint32_t n = JV_OBJ(desc)->elem_len, i = 0;
#define NEXT() (i < n ? e[i++] : JV_MKINT(0))
    m->has_tla = NEXT() == JS_TRUE;
    uint32_t nreq = (uint32_t)JV_INT(NEXT());
    if (nreq) {
        m->reqs = kz_mallocz(ctx, nreq * sizeof(KzReq));
        if (!m->reqs) return -1;
        m->nreqs = nreq;
        for (uint32_t k = 0; k < nreq; k++) {
            m->reqs[k].spec = NEXT();
            m->reqs[k].mod = NULL;
        }
    }
    uint32_t nimp = (uint32_t)JV_INT(NEXT());
    if (nimp) {
        m->imps = kz_mallocz(ctx, nimp * sizeof(KzImp));
        if (!m->imps) return -1;
        m->nimps = nimp;
        for (uint32_t k = 0; k < nimp; k++) {
            m->imps[k].req = JV_INT(NEXT());
            m->imps[k].name = NEXT();
            m->imps[k].slot = JV_INT(NEXT());
        }
    }
    uint32_t nexp = (uint32_t)JV_INT(NEXT());
    if (nexp) {
        m->exps = kz_mallocz(ctx, nexp * sizeof(KzExp));
        if (!m->exps) return -1;
        m->nexps = nexp;
        for (uint32_t k = 0; k < nexp; k++) {
            m->exps[k].kind = JV_INT(NEXT());
            m->exps[k].name = NEXT();
            m->exps[k].a = JV_INT(NEXT());
            m->exps[k].b = NEXT();
        }
    }
#undef NEXT
    return 0;
}

/* a module for compiled code: in the map from now on (a module of the same name is replaced) */
static KzModule *module_create(JSContext *ctx, JSValue name, JSValue code) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = name;
    r[1] = code;
    KzModule *m = kz_gc_new(ctx, KZ_GC_MODULE, sizeof(KzModule));
    if (!m) return NULL;
    m->name = r[0];
    m->code = r[1];
    m->gen = m->ns = m->meta = m->error = m->top = JS_UNDEFINED;
    m->realm = ctx;
    r[2] = JV_MKPTR(m);
    for (uint32_t i = 0; i < ctx->modules.len; i++) {
        KzModule *old = JV_PTR(ctx->modules.v[i]);
        if (old->name == r[0]) { ctx->modules.v[i] = r[2]; goto registered; }
    }
    if (kz_vec_push(ctx, &ctx->modules, r[2])) return NULL;
registered:
    if (module_decode(ctx, m, JV_PTR(r[1]))) return NULL;
    return m;
}

/* ================================================================== loading */

/* HostLoadImportedModule: the module `spec` names for the module (or script) called `referrer` (undefined: none), loaded
   through the host's hook unless the map has its canonical name already.  NULL with the exception pending. */
static KzModule *load_one(JSContext *ctx, JSValue referrer, JSValue spec) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = referrer;
    r[1] = spec;
    char s_buf[96];
    cstr(spec, s_buf, sizeof s_buf);
    if (!rt->hooks.load_module) {
        kz_throw_error(ctx, KZ_ERR_ERROR, "Cannot load module '%s': the host has no module loader", s_buf);
        return NULL;
    }
    size_t n;
    char *s = kz_str_to_utf8_replace(ctx, r[1], &n);
    if (!s) return NULL;
    char *ref = JV_IS_STRING(r[0]) ? kz_str_to_utf8_replace(ctx, r[0], &n) : NULL;
    if (JV_IS_STRING(r[0]) && !ref) { kz_free(rt, s); return NULL; }
    size_t len = 0;
    char *resolved = NULL;
    const char *error = NULL;
    char *src = rt->hooks.load_module(ctx, s, ref ? ref : "", &len, &resolved, &error, rt->hooks.opaque);
    if (ref) kz_free(rt, ref);
    /* (src, resolved and error belong to the host: used before anything can call the hook again) */
    KzModule *m = NULL;
    if (!src) {
        kz_throw_error(ctx, KZ_ERR_ERROR, "Cannot load module '%s': %s", s_buf, error ? error : "not found");
    } else {
        r[2] = name_atom(ctx, resolved ? resolved : s);
        if (r[2] != JS_EXCEPTION) {
            m = find_module(ctx, r[2]);
            if (!m) {
                JSValue code = kz_compile_module(ctx, src, len, r[2]);
                if (code != JS_EXCEPTION) m = module_create(ctx, r[2], code);
            }
        }
    }
    kz_free(rt, s);
    return m;
}

/* loads everything `m` requests, and what that requests (the host is asked once per specifier of a module).  On failure
   the modules this walk completed are marked not loaded again, so the next attempt walks them again. */
static int load_walk(JSContext *ctx, KzModule *m, KzVec *done) {
    if (m->loaded || m->loading) return 0;
    if (kz_check_stack(ctx)) return -1;
    m->loading = true;
    for (uint32_t i = 0; i < m->nreqs; i++) {
        if (!m->reqs[i].mod) {
            KzModule *r = load_one(ctx, m->name, m->reqs[i].spec);
            if (!r) { m->loading = false; return -1; }
            m->reqs[i].mod = r;
        }
        if (load_walk(ctx, m->reqs[i].mod, done)) { m->loading = false; return -1; }
    }
    m->loading = false;
    m->loaded = true;
    if (kz_vec_push(ctx, done, JV_MKPTR(m))) { m->loaded = false; return -1; }
    return 0;
}

static int load_graph(JSContext *ctx, KzModule *m) {
    KZ_ROOT_VEC(ctx, done);
    if (load_walk(ctx, m, &done) == 0) return 0;
    for (uint32_t i = 0; i < done.len; i++) ((KzModule *)JV_PTR(done.v[i]))->loaded = false;
    return -1;
}

/* ================================================================== exports */

typedef struct KzBind { KzModule *mod; int32_t slot; } KzBind;   /* slot -1: the module's namespace */
enum { RES_ERROR = -1, RES_NULL = 0, RES_FOUND = 1, RES_AMBIGUOUS = 2 };

static bool vec_has(const KzVec *v, JSValue x) {
    for (uint32_t i = 0; i < v->len; i++) if (v->v[i] == x) return true;
    return false;
}

/* GetExportedNames (16.2.1.6.2): appends the names to `names`; `seen` is the exportStarSet */
static int exported_names(JSContext *ctx, KzModule *m, KzVec *seen, KzVec *names) {
    if (vec_has(seen, JV_MKPTR(m))) return 0;
    if (kz_check_stack(ctx)) return -1;
    if (kz_vec_push(ctx, seen, JV_MKPTR(m))) return -1;
    for (uint32_t i = 0; i < m->nexps; i++)
        if (m->exps[i].kind != MX_STAR && kz_vec_push(ctx, names, m->exps[i].name)) return -1;
    for (uint32_t i = 0; i < m->nexps; i++) {
        if (m->exps[i].kind != MX_STAR) continue;
        KZ_ROOT_VEC(ctx, star);
        if (exported_names(ctx, m->reqs[m->exps[i].a].mod, seen, &star)) return -1;
        for (uint32_t k = 0; k < star.len; k++) {
            if (star.v[k] == ctx->rt->atom[KZ_ATOM_default] || vec_has(names, star.v[k])) continue;
            if (kz_vec_push(ctx, names, star.v[k])) return -1;
        }
    }
    return 0;
}

/* ResolveExport (16.2.1.6.3); `rset` holds (module, name) pairs already visited */
static int resolve_export(JSContext *ctx, KzModule *m, JSValue name, KzVec *rset, KzBind *out) {
    if (kz_check_stack(ctx)) return RES_ERROR;
    for (uint32_t i = 0; i + 1 < rset->len; i += 2)
        if (rset->v[i] == JV_MKPTR(m) && rset->v[i + 1] == name) return RES_NULL;      /* a circular request */
    if (kz_vec_push(ctx, rset, JV_MKPTR(m)) || kz_vec_push(ctx, rset, name)) return RES_ERROR;
    for (uint32_t i = 0; i < m->nexps; i++) {
        const KzExp *e = &m->exps[i];
        if (e->kind == MX_STAR || e->name != name) continue;
        if (e->kind == MX_LOCAL) {
            out->mod = m;
            out->slot = e->a;
            return RES_FOUND;
        }
        KzModule *im = m->reqs[e->a].mod;
        if (e->b == ctx->rt->atom[KZ_ATOM_star]) {
            out->mod = im;
            out->slot = -1;
            return RES_FOUND;
        }
        return resolve_export(ctx, im, e->b, rset, out);
    }
    if (name == ctx->rt->atom[KZ_ATOM_default]) return RES_NULL;       /* export * never provides "default" */
    KzBind star = { NULL, 0 };
    bool have = false;
    for (uint32_t i = 0; i < m->nexps; i++) {
        if (m->exps[i].kind != MX_STAR) continue;
        KzBind r;
        int rc = resolve_export(ctx, m->reqs[m->exps[i].a].mod, name, rset, &r);
        if (rc == RES_ERROR || rc == RES_AMBIGUOUS) return rc;
        if (rc != RES_FOUND) continue;
        if (!have) {
            star = r;
            have = true;
        } else if (r.mod != star.mod || r.slot != star.slot) return RES_AMBIGUOUS;
    }
    if (!have) return RES_NULL;
    *out = star;
    return RES_FOUND;
}

/* ================================================================== cells */

/* the cell of a frame slot of m, made when it does not exist yet (and put into the frame's register) */
static JSVarCell *module_cell(JSContext *ctx, KzModule *m, uint32_t slot, JSValue init) {
    if (slot >= m->nslots) { kz_throw_syntax(ctx, "invalid module binding"); return NULL; }
    if (m->cells[slot]) return m->cells[slot];
    JSVarCell *c = kz_new_cell(ctx, init);
    if (!c) return NULL;
    m->cells[slot] = c;
    JSValue *regs = kz_module_gen_regs(m->gen);
    if (regs) regs[slot] = JV_MKPTR(c);
    return c;
}

/* ================================================================== namespace objects (10.4.6) */

static uint32_t key_hash(JSValue k) {
    uint64_t x = (uint64_t)k;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static int ns_find(const KzModule *m, JSValue key) {
    if (!m->nns) return -1;
    uint32_t h = key_hash(key) & m->ns_mask;
    for (;;) {
        uint32_t e = m->ns_hash[h];
        if (!e) return -1;
        if (m->ns_keys[e - 1] == key) return (int)(e - 1);
        h = (h + 1) & m->ns_mask;
    }
}

static int ns_tdz(JSContext *ctx, JSValue key) {
    KZ_ROOT(ctx, name);
    name = kz_key_debug_name(ctx, key);
    if (name == JS_EXCEPTION) return -1;
    char buf[96];
    cstr(name, buf, sizeof buf);
    kz_throw_reference(ctx, "Cannot access '%s' before initialization", buf);
    return -1;
}

static int ns_get_own(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d) {
    if (JV_IS_SYMBOL(key)) return KZ_FALLBACK;
    KzModule *m = o->u.ns.module;
    int i = ns_find(m, key);
    if (i < 0) return 0;
    JSValue v = m->ns_cells[i]->value;
    if (v == JS_UNINIT) return ns_tdz(ctx, key);
    d->value = v;
    d->getter = d->setter = JS_UNDEFINED;
    d->flags = KZ_PROP_W | KZ_PROP_E | PD_HAS_VALUE | PD_HAS_W | PD_HAS_E | PD_HAS_C;
    return 1;
}

static int ns_define_own(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d) {
    if (JV_IS_SYMBOL(key)) return KZ_FALLBACK;
    KZ_ROOT_DESC(ctx, cur);
    int has = ns_get_own(ctx, o, key, &cur);
    if (has <= 0) return has;
    if ((d->flags & PD_HAS_C) && (d->flags & KZ_PROP_C)) return 0;
    if ((d->flags & PD_HAS_E) && !(d->flags & KZ_PROP_E)) return 0;
    if (d->flags & (PD_HAS_GET | PD_HAS_SET)) return 0;
    if ((d->flags & PD_HAS_W) && !(d->flags & KZ_PROP_W)) return 0;
    if (d->flags & PD_HAS_VALUE) return kz_same_value(ctx, d->value, cur.value);
    return 1;
}

static int ns_has(JSContext *ctx, JSObject *o, JSValue key) {
    (void)ctx;
    if (JV_IS_SYMBOL(key)) return KZ_FALLBACK;
    return ns_find(o->u.ns.module, key) >= 0;
}

static int ns_get(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out) {
    (void)receiver;
    if (JV_IS_SYMBOL(key)) return KZ_FALLBACK;
    KzModule *m = o->u.ns.module;
    int i = ns_find(m, key);
    if (i < 0) {
        *out = JS_UNDEFINED;
        return 1;
    }
    JSValue v = m->ns_cells[i]->value;
    if (v == JS_UNINIT) return ns_tdz(ctx, key);
    *out = v;
    return 1;
}

static int ns_set(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver) {
    (void)ctx; (void)o; (void)key; (void)val; (void)receiver;
    return 0;                                   /* [[Set]] is always false */
}

static int ns_del(JSContext *ctx, JSObject *o, JSValue key) {
    (void)ctx;
    if (JV_IS_SYMBOL(key)) return KZ_FALLBACK;
    return ns_find(o->u.ns.module, key) >= 0 ? 0 : 1;
}

static int ns_own_keys(JSContext *ctx, JSObject *o, KzVec *keys) {
    KzModule *m = o->u.ns.module;
    for (uint32_t i = 0; i < m->nns; i++)
        if (kz_vec_push(ctx, keys, m->ns_keys[i])) return -1;
    return 0;                                   /* the symbols (@@toStringTag) follow */
}

static const KzExotic ns_exotic = {
    .get_own = ns_get_own, .define_own = ns_define_own, .has = ns_has, .get = ns_get, .set = ns_set, .del = ns_del,
    .own_keys = ns_own_keys,
};

typedef struct NsEnt { JSValue name; KzModule *mod; int32_t slot; } NsEnt;

static int ns_ent_cmp(const void *a, const void *b) {
    return kz_str_cmp(JV_STR(((const NsEnt *)a)->name), JV_STR(((const NsEnt *)b)->name));
}

/* GetModuleNamespace (16.2.1.10): the module must be linked (its cells exist) */
static JSValue get_namespace(JSContext *ctx, KzModule *m) {
    JSRuntime *rt = ctx->rt;
    if (m->ns != JS_UNDEFINED) return m->ns;
    if (kz_check_stack(ctx)) return JS_EXCEPTION;
    KZ_ROOT_VEC(ctx, seen);
    KZ_ROOT_VEC(ctx, names);
    if (exported_names(ctx, m, &seen, &names)) return JS_EXCEPTION;
    /* the names that resolve to one binding (an ambiguous or missing name is left out) */
    NsEnt *ents = NULL;
    uint32_t nents = 0;
    for (uint32_t i = 0; i < names.len; i++) {
        KZ_ROOT_VEC(ctx, rset);
        KzBind b;
        int rc = resolve_export(ctx, m, names.v[i], &rset, &b);
        if (rc == RES_ERROR) { if (ents) kz_free(rt, ents); return JS_EXCEPTION; }
        if (rc != RES_FOUND) continue;
        if (!ents) {
            ents = kz_malloc(ctx, names.len * sizeof(NsEnt));
            if (!ents) return JS_EXCEPTION;
        }
        ents[nents].name = names.v[i];
        ents[nents].mod = b.mod;
        ents[nents].slot = b.slot;
        nents++;
    }
    if (nents > 1) qsort(ents, nents, sizeof(NsEnt), ns_ent_cmp);
    /* the object first, registered in the module, so that a namespace that refers to itself finds it */
    KZ_ROOTS(ctx, r, 2);
    JSValue result = JS_EXCEPTION;
    r[0] = kz_new_object_class(ctx, KZ_CLASS_MODULE_NS, JS_NULL);
    if (r[0] == JS_EXCEPTION) goto done;
    JV_OBJ(r[0])->u.ns.module = m;
    r[1] = kz_atom(ctx, "Module");
    if (r[1] == JS_EXCEPTION || kz_define_value(ctx, r[0], rt->sym[KZ_SYM_toStringTag], r[1], 0)) goto done;
    JV_OBJ(r[0])->h.flags &= ~OBJ_EXTENSIBLE;
    m->ns = r[0];
    if (nents) {
        uint32_t hsize = 4;
        while (hsize < nents * 2) hsize <<= 1;
        m->ns_keys = kz_mallocz(ctx, nents * sizeof(JSValue));
        if (m->ns_keys) m->ns_cells = kz_mallocz(ctx, nents * sizeof(JSVarCell *));
        if (m->ns_cells) m->ns_hash = kz_mallocz(ctx, hsize * sizeof(uint32_t));
        if (!m->ns_hash) goto fail;
        m->ns_mask = hsize - 1;
        m->nns = nents;
        for (uint32_t i = 0; i < nents; i++) {
            JSValue k = kz_key_from_value(ctx, ents[i].name);
            if (k == JS_EXCEPTION) goto fail;
            m->ns_keys[i] = k;
            uint32_t h = key_hash(k) & m->ns_mask;
            while (m->ns_hash[h]) h = (h + 1) & m->ns_mask;
            m->ns_hash[h] = i + 1;
            if (ents[i].slot >= 0) {
                JSVarCell *c = ents[i].mod->cells ? ents[i].mod->cells[ents[i].slot] : NULL;
                if (!c) { kz_throw_syntax(ctx, "module is not linked"); goto fail; }
                m->ns_cells[i] = c;
            } else {
                /* export * as ns from "x": a binding holding the namespace of x */
                JSValue nsv = get_namespace(ctx, ents[i].mod);
                if (nsv == JS_EXCEPTION) goto fail;
                JSVarCell *c = kz_new_cell(ctx, nsv);
                if (!c) goto fail;
                m->ns_cells[i] = c;
            }
        }
    }
    result = r[0];
    goto done;
fail:
    ns_reset(rt, m);                    /* (the module is left without a namespace) */
done:
    if (ents) kz_free(rt, ents);
    return result;
}

/* ================================================================== linking */

static void unlink_all(JSContext *ctx, KzVec *order) {
    for (uint32_t i = 0; i < order->len; i++) module_unlink(ctx->rt, JV_PTR(order->v[i]));
}

/* the walk: modules still new, in reverse post order... collected in `pre` (to undo) and `post` (dependencies first) */
static int link_collect(JSContext *ctx, KzModule *m, KzVec *pre, KzVec *post) {
    if (m->status != MS_NEW) return 0;
    if (kz_check_stack(ctx)) return -1;
    m->status = MS_LINKING;
    if (kz_vec_push(ctx, pre, JV_MKPTR(m))) return -1;
    for (uint32_t i = 0; i < m->nreqs; i++) {
        if (!m->reqs[i].mod) { kz_throw_error(ctx, KZ_ERR_ERROR, "module graph is not loaded"); return -1; }
        if (link_collect(ctx, m->reqs[i].mod, pre, post)) return -1;
    }
    return kz_vec_push(ctx, post, JV_MKPTR(m));
}

/* step 1: the module's frame, and the cells of its local exports */
static int link_instantiate(JSContext *ctx, KzModule *m) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = JV_MKPTR(m);
    JSFunctionBytecode *b = JV_PTR(m->code);
    r[1] = kz_new_closure(ctx, b, NULL, JS_UNDEFINED);
    if (r[1] == JS_EXCEPTION) return -1;
    r[2] = kz_module_gen_new(ctx, JV_OBJ(r[1]));
    if (r[2] == JS_EXCEPTION) return -1;
    m->gen = r[2];
    b = JV_PTR(m->code);
    if (b->nlocals) {
        m->cells = kz_mallocz(ctx, b->nlocals * sizeof(JSVarCell *));
        if (!m->cells) return -1;
        m->nslots = b->nlocals;
    }
    for (uint32_t i = 0; i < m->nexps; i++) {
        const KzExp *e = &m->exps[i];
        if (e->kind != MX_LOCAL) continue;
        if (!module_cell(ctx, m, (uint32_t)e->a, JV_INT(e->b) == 1 ? JS_UNINIT : JS_UNDEFINED)) return -1;
    }
    return 0;
}

static int link_error(JSContext *ctx, int rc, JSValue spec, JSValue name) {
    char sb[96], nb[96];
    cstr(spec, sb, sizeof sb);
    cstr(name, nb, sizeof nb);
    if (rc == RES_AMBIGUOUS)
        kz_throw_syntax(ctx, "The requested module '%s' contains conflicting star exports for name '%s'", sb, nb);
    else
        kz_throw_syntax(ctx, "The requested module '%s' does not provide an export named '%s'", sb, nb);
    return -1;
}

/* step 2 (InitializeEnvironment): indirect exports must resolve; imports get the exporters' cells */
static int link_wire(JSContext *ctx, KzModule *m) {
    JSRuntime *rt = ctx->rt;
    for (uint32_t i = 0; i < m->nexps; i++) {
        const KzExp *e = &m->exps[i];
        if (e->kind != MX_INDIRECT) continue;
        KZ_ROOT_VEC(ctx, rset);
        KzBind b;
        int rc = resolve_export(ctx, m, e->name, &rset, &b);
        if (rc == RES_ERROR) return -1;
        if (rc != RES_FOUND) return link_error(ctx, rc, m->reqs[e->a].spec, e->b);
    }
    for (uint32_t i = 0; i < m->nimps; i++) {
        const KzImp *in = &m->imps[i];
        KzModule *t = m->reqs[in->req].mod;
        if (in->name == rt->atom[KZ_ATOM_star]) {
            KZ_ROOT(ctx, nsv);
            nsv = get_namespace(ctx, t);
            if (nsv == JS_EXCEPTION) return -1;
            JSVarCell *c = module_cell(ctx, m, (uint32_t)in->slot, JS_UNDEFINED);
            if (!c) return -1;
            c->value = nsv;
            continue;
        }
        KZ_ROOT_VEC(ctx, rset);
        KzBind b;
        int rc = resolve_export(ctx, t, in->name, &rset, &b);
        if (rc == RES_ERROR) return -1;
        if (rc != RES_FOUND) return link_error(ctx, rc, m->reqs[in->req].spec, in->name);
        JSVarCell *c;
        if (b.slot >= 0) {
            c = b.mod->cells ? b.mod->cells[b.slot] : NULL;
            if (!c) { kz_throw_syntax(ctx, "module is not linked"); return -1; }
        } else {
            KZ_ROOT(ctx, nsv);
            nsv = get_namespace(ctx, b.mod);
            if (nsv == JS_EXCEPTION) return -1;
            c = kz_new_cell(ctx, nsv);
            if (!c) return -1;
        }
        if ((uint32_t)in->slot >= m->nslots) { kz_throw_syntax(ctx, "invalid module binding"); return -1; }
        m->cells[in->slot] = c;
        JSValue *regs = kz_module_gen_regs(m->gen);
        if (regs) regs[in->slot] = JV_MKPTR(c);
    }
    return 0;
}

/* Link (16.2.1.5.1) for a loaded graph */
static int link_graph(JSContext *ctx, KzModule *root) {
    KZ_ROOT_VEC(ctx, pre);
    KZ_ROOT_VEC(ctx, post);
    if (link_collect(ctx, root, &pre, &post)) { unlink_all(ctx, &pre); return -1; }
    for (uint32_t i = 0; i < post.len; i++)
        if (link_instantiate(ctx, JV_PTR(post.v[i]))) { unlink_all(ctx, &pre); return -1; }
    for (uint32_t i = 0; i < post.len; i++)
        if (link_wire(ctx, JV_PTR(post.v[i]))) { unlink_all(ctx, &pre); return -1; }
    for (uint32_t i = 0; i < post.len; i++) {
        KzModule *m = JV_PTR(post.v[i]);
        if (kz_module_gen_start(ctx, m->gen)) { unlink_all(ctx, &pre); return -1; }
    }
    for (uint32_t i = 0; i < pre.len; i++) ((KzModule *)JV_PTR(pre.v[i]))->status = MS_LINKED;
    return 0;
}

/* ================================================================== evaluation */

static JSValue async_ok(JSContext *ctx, JSValue data, JSValue arg);
static JSValue async_err(JSContext *ctx, JSValue data, JSValue arg);

/* ExecuteModule for a module without top-level await: the body runs now */
static int execute_sync(JSContext *ctx, KzModule *m) {
    KZ_ROOT(ctx, g);
    g = m->gen;
    m->gen = JS_UNDEFINED;
    return kz_module_gen_run(ctx, g);
}

/* ExecuteAsyncModule (16.2.1.5.3.2) */
static int execute_async(JSContext *ctx, KzModule *m) {
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_new_promise_intrinsic(ctx);
    if (r[0] == JS_EXCEPTION) return -1;
    if (kz_promise_then_internal(ctx, r[0], async_ok, async_err, JV_MKPTR(m), JS_UNDEFINED)) return -1;
    r[1] = m->gen;
    m->gen = JS_UNDEFINED;
    return kz_module_gen_run_async(ctx, r[1], r[0]);
}

/* InnerModuleEvaluation (16.2.1.5.3.1): 0, or -1 with the module's error pending */
static int inner_eval(JSContext *ctx, KzModule *m, KzVec *stack, int32_t *index) {
    if (m->status == MS_EVALUATING_ASYNC || m->status == MS_EVALUATED) {
        if (!m->has_error) return 0;
        kz_throw(ctx, m->error);
        return -1;
    }
    if (m->status == MS_EVALUATING) return 0;
    if (m->status != MS_LINKED) { kz_throw_error(ctx, KZ_ERR_ERROR, "module is not linked"); return -1; }
    if (kz_check_stack(ctx)) return -1;
    m->status = MS_EVALUATING;
    m->dfs_index = m->dfs_ancestor = *index;
    m->pending = 0;
    (*index)++;
    if (kz_vec_push(ctx, stack, JV_MKPTR(m))) return -1;
    for (uint32_t i = 0; i < m->nreqs; i++) {
        KzModule *rm = m->reqs[i].mod;
        if (inner_eval(ctx, rm, stack, index)) return -1;
        if (rm->status == MS_EVALUATING) {
            if (rm->dfs_ancestor < m->dfs_ancestor) m->dfs_ancestor = rm->dfs_ancestor;
        } else {
            rm = rm->cycle_root;
            if (rm->has_error) {
                kz_throw(ctx, rm->error);
                return -1;
            }
        }
        if (rm->async_eval) {
            m->pending++;
            if (rm->nparents == rm->cap_parents) {
                uint32_t nc = rm->cap_parents ? rm->cap_parents * 2 : 4;
                KzModule **np = kz_realloc(ctx, rm->parents, nc * sizeof(KzModule *));
                if (!np) return -1;
                rm->parents = np;
                rm->cap_parents = nc;
            }
            rm->parents[rm->nparents++] = m;
        }
    }
    if (m->pending > 0 || m->has_tla) {
        m->async_eval = true;
        m->async_order = ++ctx->mod_async_seq;
        if (m->pending == 0 && execute_async(ctx, m)) return -1;
    } else if (execute_sync(ctx, m)) return -1;
    if (m->dfs_ancestor == m->dfs_index) {
        for (;;) {
            KzModule *rm = JV_PTR(stack->v[--stack->len]);
            rm->status = rm->async_eval ? MS_EVALUATING_ASYNC : MS_EVALUATED;
            rm->cycle_root = m;
            if (rm == m) break;
        }
    }
    return 0;
}

/* GatherAvailableAncestors (16.2.1.5.3.4) */
static int gather_ancestors(JSContext *ctx, KzModule *m, KzVec *exec) {
    if (kz_check_stack(ctx)) return -1;
    for (uint32_t i = 0; i < m->nparents; i++) {
        KzModule *p = m->parents[i];
        if (vec_has(exec, JV_MKPTR(p)) || p->cycle_root->has_error) continue;
        if (--p->pending != 0) continue;
        if (kz_vec_push(ctx, exec, JV_MKPTR(p))) return -1;
        if (!p->has_tla && gather_ancestors(ctx, p, exec)) return -1;
    }
    return 0;
}

static void finish_ok(JSContext *ctx, KzModule *m) {
    m->async_eval = false;
    m->status = MS_EVALUATED;
    if (m->top != JS_UNDEFINED) kz_promise_resolve_value(ctx, m->top, JS_UNDEFINED);
}

/* AsyncModuleExecutionRejected (16.2.1.5.3.6) */
static int async_rejected(JSContext *ctx, KzModule *m, JSValue error) {
    if (m->status == MS_EVALUATED) return 0;
    if (kz_check_stack(ctx)) return -1;
    m->has_error = true;
    m->error = error;
    m->async_eval = false;
    m->status = MS_EVALUATED;
    /* the module's own promise first, then its parents' (leaf to root) */
    if (m->top != JS_UNDEFINED) kz_promise_reject_value(ctx, m->top, error);
    for (uint32_t i = 0; i < m->nparents; i++)
        if (async_rejected(ctx, m->parents[i], error)) return -1;
    return 0;
}

static int async_order_cmp(const void *a, const void *b) {
    uint32_t x = (*(KzModule *const *)a)->async_order, y = (*(KzModule *const *)b)->async_order;
    return x < y ? -1 : x > y;
}

/* AsyncModuleExecutionFulfilled (16.2.1.5.3.5) */
static int async_fulfilled(JSContext *ctx, KzModule *m) {
    if (m->status == MS_EVALUATED) return 0;
    finish_ok(ctx, m);
    KZ_ROOT_VEC(ctx, exec);
    if (gather_ancestors(ctx, m, &exec)) return -1;
    /* in the order in which they became async */
    if (exec.len > 1) {
        KzModule **tmp = kz_malloc(ctx, exec.len * sizeof(KzModule *));
        if (!tmp) return -1;
        for (uint32_t i = 0; i < exec.len; i++) tmp[i] = JV_PTR(exec.v[i]);
        qsort(tmp, exec.len, sizeof(KzModule *), async_order_cmp);
        for (uint32_t i = 0; i < exec.len; i++) exec.v[i] = JV_MKPTR(tmp[i]);
        kz_free(ctx->rt, tmp);
    }
    for (uint32_t i = 0; i < exec.len; i++) {
        KzModule *p = JV_PTR(exec.v[i]);
        if (p->status == MS_EVALUATED) continue;
        if (p->has_tla) {
            if (execute_async(ctx, p)) return -1;
        } else if (execute_sync(ctx, p)) {
            if (ctx->rt->exc_uncatchable) return -1;
            KZ_ROOT(ctx, err);
            err = kz_take_exception(ctx);
            if (async_rejected(ctx, p, err)) return -1;
        } else finish_ok(ctx, p);
    }
    return 0;
}

static JSValue async_ok(JSContext *ctx, JSValue data, JSValue arg) {
    (void)arg;
    return async_fulfilled(ctx, JV_PTR(data)) ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue async_err(JSContext *ctx, JSValue data, JSValue arg) {
    return async_rejected(ctx, JV_PTR(data), arg) ? JS_EXCEPTION : JS_UNDEFINED;
}

/* Evaluate (16.2.1.5.3): the promise for the module's evaluation (and its dependencies'); JS_EXCEPTION only for an
   uncatchable error or out of memory */
static JSValue module_evaluate(JSContext *ctx, KzModule *m) {
    if ((m->status == MS_EVALUATING_ASYNC || m->status == MS_EVALUATED) && m->cycle_root) m = m->cycle_root;
    if (m->top != JS_UNDEFINED) return m->top;
    KZ_ROOTS(ctx, r, 2);
    r[0] = JV_MKPTR(m);
    r[1] = kz_new_promise_intrinsic(ctx);
    if (r[1] == JS_EXCEPTION) return r[1];
    m->top = r[1];
    if (m->status == MS_EVALUATED && m->has_error) {      /* it failed as part of another graph */
        kz_promise_reject_value(ctx, r[1], m->error);
        return r[1];
    }
    KZ_ROOT_VEC(ctx, stack);
    int32_t index = 0;
    if (inner_eval(ctx, m, &stack, &index)) {
        if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
        KZ_ROOT(ctx, err);
        err = kz_take_exception(ctx);
        /* every module still on the stack ends with the error (the cycle root of such a module is the one evaluated) */
        for (uint32_t i = 0; i < stack.len; i++) {
            KzModule *sm = JV_PTR(stack.v[i]);
            sm->status = MS_EVALUATED;
            sm->has_error = true;
            sm->error = err;
            sm->async_eval = false;
            sm->cycle_root = m;
        }
        if (!m->has_error) {                              /* (it was not on the stack: it failed before it started) */
            m->has_error = true;
            m->error = err;
            m->status = MS_EVALUATED;
            m->cycle_root = m;
        }
        kz_promise_reject_value(ctx, m->top, err);
        return m->top;
    }
    if (!m->async_eval) kz_promise_resolve_value(ctx, m->top, JS_UNDEFINED);
    return m->top;
}

/* ================================================================== entry points */

/* a promise rejected with the exception that is pending (JS_EXCEPTION for an uncatchable one) */
static JSValue rejected_with_pending(JSContext *ctx) {
    if (ctx->rt->exc_uncatchable) return JS_EXCEPTION;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_take_exception(ctx);
    r[1] = kz_new_promise_intrinsic(ctx);
    if (r[1] == JS_EXCEPTION) return r[1];
    kz_promise_reject_value(ctx, r[1], r[0]);
    return r[1];
}

/* js_eval(JS_EVAL_MODULE): compile (a syntax error is thrown), load what it imports, link and evaluate; the result is
   the promise for its evaluation, rejected by anything that goes wrong after the compile */
JSValue kz_module_evaluate_source(JSContext *ctx, const char *src, size_t len, const char *filename) {
    KZ_ROOTS(ctx, r, 3);
    r[0] = name_atom(ctx, filename ? filename : "<module>");
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    r[1] = kz_compile_module(ctx, src, len, r[0]);
    if (r[1] == JS_EXCEPTION) return JS_EXCEPTION;
    KzModule *m = module_create(ctx, r[0], r[1]);
    if (!m) return JS_EXCEPTION;
    r[2] = JV_MKPTR(m);
    if (load_graph(ctx, m) || link_graph(ctx, m)) return rejected_with_pending(ctx);
    return module_evaluate(ctx, m);
}

/* the reaction that turns the evaluation of a dynamically imported module into its namespace */
static JSValue dynamic_ok(JSContext *ctx, JSValue data, JSValue arg) {
    (void)arg;
    return get_namespace(ctx, JV_PTR(data));
}

/* the part of import() that runs after the current job: load, link, evaluate; args [promise, specifier, referrer] */
static JSValue dynamic_import_job(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = argv[0];
    KzModule *m = load_one(ctx, argv[2], argv[1]);
    if (m && load_graph(ctx, m) == 0 && link_graph(ctx, m) == 0) {
        r[1] = JV_MKPTR(m);
        r[2] = module_evaluate(ctx, m);
        if (r[2] == JS_EXCEPTION) return JS_EXCEPTION;
        if (kz_promise_then_internal(ctx, r[2], dynamic_ok, NULL, r[1], r[0]) == 0) return JS_UNDEFINED;
    }
    if (rt->exc_uncatchable) return JS_EXCEPTION;
    r[1] = kz_take_exception(ctx);
    kz_promise_reject_value(ctx, r[0], r[1]);
    return JS_UNDEFINED;
}

/* import(specifier, options) (13.3.10.1): a promise; the work happens in a job, so the code that called it finishes first */
JSValue kz_dynamic_import(JSContext *ctx, JSValue specifier, JSValue options, JSValue referrer) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 5);
    r[0] = kz_new_promise_intrinsic(ctx);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_to_string(ctx, specifier);
    if (r[1] == JS_EXCEPTION) goto abrupt;
    if (options != JS_UNDEFINED) {
        if (!JV_IS_OBJECT(options)) {
            kz_throw_type(ctx, "The second argument of import() must be an object");
            goto abrupt;
        }
        r[2] = kz_get(ctx, options, rt->atom[KZ_ATOM_kw_with], options);
        if (r[2] == JS_EXCEPTION) goto abrupt;
        if (r[2] != JS_UNDEFINED) {
            if (!JV_IS_OBJECT(r[2])) {
                kz_throw_type(ctx, "The 'with' option of import() must be an object");
                goto abrupt;
            }
            KZ_ROOT_VEC(ctx, keys);
            if (kz_own_keys(ctx, JV_OBJ(r[2]), &keys, KZ_KEYS_STRINGS | KZ_KEYS_ENUM)) goto abrupt;
            for (uint32_t i = 0; i < keys.len; i++) {
                r[3] = kz_get(ctx, r[2], keys.v[i], r[2]);
                if (r[3] == JS_EXCEPTION) goto abrupt;
                if (!JV_IS_STRING(r[3])) {
                    kz_throw_type(ctx, "Import attribute values must be strings");
                    goto abrupt;
                }
                /* no attribute is supported by this host */
                kz_throw_type(ctx, "Import attributes are not supported");
                goto abrupt;
            }
        }
    }
    {
        JSValue args[3] = { r[0], r[1], referrer };
        if (kz_enqueue_job(ctx, dynamic_import_job, 3, args)) goto abrupt;
    }
    return r[0];
abrupt:
    if (rt->exc_uncatchable) return JS_EXCEPTION;
    r[4] = kz_take_exception(ctx);
    kz_promise_reject_value(ctx, r[0], r[4]);
    return r[0];
}

/* import.meta: one object per module, made on first use (HostGetImportMetaProperties fills it) */
JSValue kz_import_meta(JSContext *ctx, JSFunctionBytecode *b) {
    JSRuntime *rt = ctx->rt;
    KzModule *m = find_module(ctx, b->filename);
    if (!m) return kz_throw_syntax(ctx, "Cannot use 'import.meta' outside a module");
    if (m->meta != JS_UNDEFINED) return m->meta;
    KZ_ROOTS(ctx, r, 2);
    r[0] = JV_MKPTR(m);
    r[1] = kz_new_object_proto(ctx, JS_NULL);
    if (r[1] == JS_EXCEPTION) return r[1];
    m->meta = r[1];
    if (rt->hooks.import_meta) {
        size_t n;
        char *name = kz_str_to_utf8_replace(ctx, m->name, &n);
        if (!name) return JS_EXCEPTION;
        int mark = js_scope_open(ctx);
        rt->hooks.import_meta(ctx, r[1], name, rt->hooks.opaque);
        js_scope_close(ctx, mark);
        kz_free(rt, name);
        if (kz_has_exception(ctx)) return JS_EXCEPTION;
    }
    return m->meta;
}

int kz_init_modules(JSContext *ctx) {
    (void)ctx;
    kz_exotic[KZ_CLASS_MODULE_NS] = &ns_exotic;
    return 0;
}
