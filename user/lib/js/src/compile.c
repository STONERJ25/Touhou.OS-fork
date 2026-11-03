/*
 * compile.c: from the syntax tree to bytecode.
 *
 * Two passes over a parsed program:
 *   1. analysis: every identifier is resolved through the scope chain to its Binding (or to the global object).
 *      A binding used by an inner function is marked captured: it will live in a heap var cell, and each function
 *      between the use and the declaration lists it among its closure variables (flat closures: a closure copies
 *      the cell pointers it needs when it is created).  `this`, `new.target`, the home object and `arguments` get
 *      hidden bindings when arrow functions (or a direct eval) need them from an enclosing function.  References that
 *      pass a `with` statement, or a function with a sloppy direct eval (which may add vars at run time), are given
 *      a list of dynamic scope objects to consult first.
 *   2. code generation for a stack machine (opcodes.h): locals in frame slots, captured ones as cells, globals by
 *      name with inline caches.  try/finally uses completion records (value, type) on the operand stack and a jump
 *      table at the end of the finally block, so break/continue/return through finally need no code copies.
 *
 * Specification: ECMA-262 runtime semantics (13 to 16), the instantiation algorithms (FunctionDeclarationInstantiation,
 * GlobalDeclarationInstantiation, EvalDeclarationInstantiation) and Annex B.3.3 (block functions in sloppy code).
 */
#include "parser.h"
#include "bytecode.h"
#include <stdarg.h>

const KzOpInfo kz_op_info[OP_COUNT] = {
#define OP(name, size, pop, push) { size, pop, push, #name },
#include "opcodes.h"
#undef OP
};

/* ================================================================== analysis */

typedef struct Analyzer {
    Parser *p;
    JSContext *ctx;
    bool failed;
} Analyzer;

static bool is_lexical_kind_x(int k) { return k == BK_LET || k == BK_CONST || k == BK_CLASS; }

static bool an_fail(Analyzer *a) {
    a->failed = true;
    a->p->failed = true;
    return false;
}

static FuncInfo *non_arrow(FuncInfo *f) {
    while (f && (f->flags & (FN_ARROW | FN_DIRECT_EVAL))) f = f->parent;
    return f;
}

/* adds b to the closure variables of every function from `from` up to (not including) b's function */
static bool capture(Analyzer *a, Binding *b, FuncInfo *from) {
    FuncInfo *owner = b->scope->func;
    if (from == owner) return true;
    b->flags |= BF_CAPTURED;
    for (FuncInfo *f = from; f && f != owner; f = f->parent) {
        bool have = false;
        for (CapturedVar *c = f->captured; c; c = c->next)
            if (c->b == b) { have = true; break; }
        if (have) continue;
        CapturedVar *c = kz_arena_alloc(a->p, sizeof(CapturedVar));
        if (!c) return an_fail(a);
        c->b = b;
        c->index = f->ncaptured++;
        if (f->captured_tail) f->captured_tail->next = c; else f->captured = c;
        f->captured_tail = c;
    }
    return true;
}

static int captured_index(FuncInfo *f, Binding *b) {
    for (CapturedVar *c = f->captured; c; c = c->next)
        if (c->b == b) return c->index;
    return -1;
}

static Binding *hidden_binding(Analyzer *a, FuncInfo *g, JSValue name, Binding **slot) {
    if (*slot) return *slot;
    Binding *b = kz_declare_hidden(a->p, g->scope, name);
    if (!b) { an_fail(a); return NULL; }
    *slot = b;
    return b;
}

static bool sloppy_eval_func(FuncInfo *f) { return f && f->has_direct_eval && !(f->flags & FN_STRICT); }

/* scopes whose bindings are global-object or global-lexical names rather than frame slots */
static bool global_scope(Scope *s) { return s->kind == SC_SCRIPT; }

/* is b (declared in s) a name on the global object rather than a frame slot? */
static bool global_binding(Scope *s, Binding *b) {
    if (b->kind == BK_HIDDEN) return false;
    if (s->kind == SC_SCRIPT) return true;
    return s->kind == SC_EVAL && (s->func->flags & FN_GLOBAL_VARS) && (b->kind == BK_VAR || b->kind == BK_FUNC);
}

/* Resolve `name` seen in scope `start` from function `fi`.  Dynamic scopes met on the way are appended to `dyn`. */
static Binding *resolve(Analyzer *a, FuncInfo *fi, Scope *start, JSValue name, NodeList *dyn) {
    for (Scope *s = start; s; s = s->parent) {
        Binding *b = kz_scope_find(s, name);
        if (b && b->kind != BK_HIDDEN) {
            if (global_binding(s, b)) return NULL;
            if (!capture(a, b, fi)) return NULL;
            return b;
        }
        /* direct eval code: names its sloppy var declarations made dynamic live in the caller's var object */
        if (dyn && s->kind == SC_EVAL && s->eval_obj) {
            Binding *e = s->eval_obj;
            if (!capture(a, e, fi)) return NULL;
            Node *d = kz_arena_alloc(a->p, sizeof(Node));
            if (!d) { an_fail(a); return NULL; }
            d->binding = e;
            d->ival = WK_EVAL;
            if (kz_list_push(a->p, dyn, d)) { an_fail(a); return NULL; }
        }
        if (s->kind == SC_WITH && dyn) {
            Binding *w = s->with_obj;
            if (!capture(a, w, fi)) return NULL;
            Node *d = kz_arena_alloc(a->p, sizeof(Node));
            if (!d) { an_fail(a); return NULL; }
            d->binding = w;
            d->ival = WK_WITH;
            if (kz_list_push(a->p, dyn, d)) { an_fail(a); return NULL; }
        }
        /* leaving a function's var scope that a sloppy direct eval may extend */
        if (dyn && s->kind == SC_FUNC && s->func && s == s->func->body_scope &&
            sloppy_eval_func(s->func) && !(s->func->flags & (FN_SCRIPT | FN_SYNTH_CALLER))) {
            Binding *e = hidden_binding(a, s->func, a->ctx->rt->atom[KZ_ATOM_dyn_scope], &s->eval_obj);
            if (!e || !capture(a, e, fi)) return NULL;
            Node *d = kz_arena_alloc(a->p, sizeof(Node));
            if (!d) { an_fail(a); return NULL; }
            d->binding = e;
            d->ival = WK_EVAL;
            if (kz_list_push(a->p, dyn, d)) { an_fail(a); return NULL; }
        }
    }
    return NULL;
}

static void analyze(Analyzer *a, Node *n, FuncInfo *fi);
static void analyze_function(Analyzer *a, FuncInfo *fi);

static void analyze_list(Analyzer *a, NodeList *l, FuncInfo *fi) {
    for (int i = 0; i < l->n && !a->failed; i++) analyze(a, l->v[i], fi);
}

static Binding *this_binding_for(Analyzer *a, FuncInfo *fi) {
    FuncInfo *g = non_arrow(fi);
    if (!g) return NULL;
    if (g->flags & FN_SYNTH_CALLER) {
        /* eval code: the caller's binding if it had one; else the eval function's own this (the caller's) */
        if (g->this_b && !capture(a, g->this_b, fi)) return NULL;
        return g->this_b;
    }
    Binding *b = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_lex_this], &g->this_b);
    if (b && !capture(a, b, fi)) return NULL;
    return b;
}

static void analyze_ident(Analyzer *a, Node *n, FuncInfo *fi) {
    if (n->binding || (n->ival & 0x40000000)) return;
    n->ival |= 0x40000000;          /* resolved */
    n->binding = resolve(a, fi, n->scope ? n->scope : fi->scope, n->val, &n->list);
#ifdef KZ_DEBUG_RESOLVE
    {
        extern int printf(const char *, ...);
        char buf[64];
        kz_str_to_utf8_buf(JV_STR(n->val), buf, sizeof buf);
        int depth = 0;
        for (Scope *s = n->scope; s; s = s->parent) depth++;
        printf("resolve %s line %u scope %p (kind %d, depth %d, first %p) -> %p\n", buf, n->line, (void *)n->scope,
               n->scope ? n->scope->kind : -1, depth, n->scope ? (void *)n->scope->first : NULL, (void *)n->binding);
    }
#endif
}

static JSValue private_key(Analyzer *a, JSValue name) {
    /* private names are bound under "#name" so that they never collide with identifiers */
    KzStrBuf sb;
    kz_sb_init(&sb, a->ctx);
    kz_sb_putc(&sb, '#');
    kz_sb_put_value(&sb, name);
    JSValue s = kz_sb_finish(&sb);
    if (s == JS_EXCEPTION) { an_fail(a); return JS_EXCEPTION; }
    KZ_ROOT(a->ctx, rs);
    rs = s;
    JSValue at = kz_atom_from_str(a->ctx, rs);
    if (at == JS_EXCEPTION || kz_parser_keep(a->p, at) == JS_EXCEPTION) { an_fail(a); return JS_EXCEPTION; }
    return at;
}

static void analyze_private_ref(Analyzer *a, Node *n, FuncInfo *fi) {
    JSValue k = private_key(a, n->val);
    if (k == JS_EXCEPTION) return;
    n->val2 = k;
    for (Scope *s = n->scope; s; s = s->parent) {
        Binding *b = kz_scope_find(s, k);
        if (b) {
            if (!capture(a, b, fi)) return;
            n->binding = b;
            return;
        }
    }
    /* declared outside (eval in a class): resolved by name at run time; unsupported for now */
}

static void analyze(Analyzer *a, Node *n, FuncInfo *fi) {
    if (!n || a->failed) return;
    switch (n->type) {
    case N_IDENT:
        analyze_ident(a, n, fi);
        return;
    case N_THIS: {
        FuncInfo *g = non_arrow(fi);
        if (g && (g != fi || (g->flags & FN_DERIVED) || g->has_direct_eval)) n->binding = this_binding_for(a, fi);
        return;
    }
    case N_NEW_TARGET: {
        FuncInfo *g = non_arrow(fi);
        if (g && (g->flags & FN_SYNTH_CALLER)) {
            if (g->new_target_b && capture(a, g->new_target_b, fi)) n->binding = g->new_target_b;
            return;
        }
        if (g && g != fi) {
            Binding *b = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_new_target], &g->new_target_b);
            if (b && capture(a, b, fi)) n->binding = b;
        }
        return;
    }
    case N_SUPER: {
        /* super.x and super[x]: the home object and this; super(): the function, new.target and this */
        FuncInfo *g = non_arrow(fi);
        if (!g) return;
        if (g->flags & FN_SYNTH_CALLER) {
            /* eval code in a method: the eval function's home is the caller's; super() is not supported */
            n->binding = this_binding_for(a, fi);
            return;
        }
        if (g != fi) {
            Binding *h = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_home_object], &g->home_b);
            if (!h || !capture(a, h, fi)) return;
            Binding *nt = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_new_target], &g->new_target_b);
            if (!nt || !capture(a, nt, fi)) return;
            Binding *fb = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_class_fields], &g->fields_b);
            if (!fb || !capture(a, fb, fi)) return;
        }
        n->binding = this_binding_for(a, fi);
        if (g == fi && !(g->flags & FN_DERIVED) && !g->has_direct_eval) n->binding = NULL;
        return;
    }
    case N_FUNC: case N_ARROW: case N_FUNC_DECL:
        analyze_function(a, n->func);
        return;
    case N_CLASS: {
        /* private names become hidden bindings of the class scope */
        for (int i = 0; i < n->list.n; i++) {
            Node *el = n->list.v[i];
            if (!(el->flags & F_PRIVATE_KEY)) continue;
            JSValue k = private_key(a, el->a->val);
            if (k == JS_EXCEPTION) return;
            el->a->val2 = k;
            if (!kz_scope_find(n->scope, k)) {
                Binding *b = kz_declare_hidden(a->p, n->scope, k);
                if (!b) { an_fail(a); return; }
            }
        }
        analyze(a, n->a, fi);
        for (int i = 0; i < n->list.n && !a->failed; i++) {
            Node *el = n->list.v[i];
            if (el->flags & F_COMPUTED) analyze(a, el->a, fi);
            if (el->b) analyze(a, el->b, fi);
        }
        return;
    }
    case N_PRIVATE_MEMBER:
        analyze(a, n->a, fi);
        analyze_private_ref(a, n, fi);
        return;
    case N_PRIVATE_IN:
        analyze(a, n->b, fi);
        analyze_private_ref(a, n, fi);
        return;
    case N_MEMBER:
        analyze(a, n->a, fi);
        if (n->flags & F_COMPUTED) analyze(a, n->b, fi);
        return;
    case N_PROP: case N_PAT_PROP:
        if (n->flags & F_COMPUTED) analyze(a, n->a, fi);
        analyze(a, n->b, fi);
        if (n->type == N_PROP && n->op == P_SPREAD) analyze(a, n->a, fi);
        return;
    case N_CALL:
        analyze(a, n->a, fi);
        analyze_list(a, &n->list, fi);
        if (n->flags & F_DIRECT_EVAL) {
            /* a sloppy function whose body holds the eval gets an object for the vars the eval code declares */
            if (!(fi->flags & (FN_STRICT | FN_SCRIPT | FN_MODULE | FN_SYNTH_CALLER | FN_GLOBAL_VARS | FN_DIRECT_EVAL)) &&
                !fi->body_scope->eval_obj) {
                Binding *eb = kz_declare_hidden(a->p, fi->body_scope, a->ctx->rt->atom[KZ_ATOM_dyn_scope]);
                if (!eb) { an_fail(a); return; }
                fi->body_scope->eval_obj = eb;
            }
            /* everything visible becomes reachable by name: capture this, new.target, home, arguments */
            FuncInfo *g = non_arrow(fi);
            if (g && !(g->flags & (FN_SCRIPT | FN_MODULE | FN_SYNTH_CALLER))) {
                this_binding_for(a, fi);
                Binding *b = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_new_target], &g->new_target_b);
                if (b) capture(a, b, fi);
                if (g->flags & (FN_METHOD | FN_CTOR | FN_FIELD_INIT | FN_STATIC_BLOCK)) {
                    b = hidden_binding(a, g, a->ctx->rt->atom[KZ_ATOM_home_object], &g->home_b);
                    if (b) capture(a, b, fi);
                }
            }
        }
        return;
    case N_TEMPLATE_PART: case N_NUMBER: case N_STRING: case N_BIGINT: case N_REGEXP: case N_NULL: case N_TRUE:
    case N_FALSE: case N_EMPTY: case N_DEBUGGER: case N_IMPORT_META: case N_PRIVATE_NAME: case N_BREAK: case N_CONTINUE:
        return;
    case N_IMPORT: case N_EXPORT:
        if (n->a) analyze(a, n->a, fi);
        for (int i = 0; i < n->list.n; i++) if (n->list.v[i]->a) analyze(a, n->list.v[i]->a, fi);
        return;
    default:
        analyze(a, n->a, fi);
        analyze(a, n->b, fi);
        analyze(a, n->c, fi);
        analyze(a, n->d, fi);
        analyze_list(a, &n->list, fi);
        return;
    }
}

/* Annex B.3.3: give a sloppy block function a var binding in its function when that causes no conflict */
static void annexb(Analyzer *a, FuncInfo *fi) {
    for (int i = 0; i < fi->annexb.n; i++) {
        Node *fn = fi->annexb.v[i];
        JSValue name = fn->val;
        Scope *blk = fn->scope, *vs = fi->body_scope;
        bool ok = true;
        Binding *var = NULL;
        for (Scope *s = blk->parent; s && ok; s = s->parent) {
            Binding *b = kz_scope_find(s, name);
            if (s == vs || s == fi->scope) {
                if (b) {
                    if (b->kind == BK_VAR || b->kind == BK_FUNC || b->kind == BK_ARGUMENTS) var = b;
                    else ok = false;      /* a parameter or a lexical declaration */
                }
                if (s == fi->scope) break;
                if (s == vs && vs != fi->scope) {
                    Binding *pb = kz_scope_find(fi->scope, name);
                    if (pb) ok = false;    /* a parameter name */
                    break;
                }
                continue;
            }
            if (b && !(b->kind == BK_CATCH && (b->flags & BF_SIMPLE_CATCH))) ok = false;
        }
        if (!ok) continue;
        if (!var) {
            if (fi->flags & FN_SCRIPT) {
                var = kz_scope_find(vs, name);
                if (!var) {
                    Binding *nb = kz_arena_alloc(a->p, sizeof(Binding));
                    if (!nb) { an_fail(a); return; }
                    nb->name = name;
                    nb->kind = BK_VAR;
                    nb->scope = vs;
                    nb->slot = -1;
                    nb->export_index = -1;
                    if (vs->last) vs->last->next = nb; else vs->first = nb;
                    vs->last = nb;
                    var = nb;
                }
            } else {
                Binding *nb = kz_arena_alloc(a->p, sizeof(Binding));
                if (!nb) { an_fail(a); return; }
                nb->name = name;
                nb->kind = BK_VAR;
                nb->scope = vs;
                nb->slot = -1;
                nb->export_index = -1;
                if (vs->last) vs->last->next = nb; else vs->first = nb;
                vs->last = nb;
                var = nb;
            }
            var->flags |= BF_ANNEXB;
        }
        fn->binding = var;
    }
}

static void analyze_function(Analyzer *a, FuncInfo *fi) {
    if (a->failed) return;
    JSRuntime *rt = a->ctx->rt;
    /* the implicit arguments object */
    if (!(fi->flags & (FN_ARROW | FN_SCRIPT | FN_MODULE | FN_EVAL | FN_FIELD_INIT | FN_STATIC_BLOCK)) && fi->uses_arguments) {
        JSValue an = rt->atom[KZ_ATOM_arguments];
        Binding *pb = kz_scope_find(fi->scope, an);
        bool create = true;
        if (pb && pb->kind == BK_PARAM) create = false;
        Binding *bb = fi->body_scope != fi->scope ? kz_scope_find(fi->body_scope, an) : pb;
        if (create && (fi->flags & FN_SIMPLE) && bb && (bb->kind == BK_FUNC || is_lexical_kind_x(bb->kind))) create = false;
        if (create) {
            if (bb && bb->kind == BK_VAR && fi->body_scope == fi->scope) { bb->kind = BK_ARGUMENTS; fi->arguments_b = bb; }
            else if (pb && pb->kind == BK_VAR) { pb->kind = BK_ARGUMENTS; fi->arguments_b = pb; }
            else {
                Binding *b = kz_declare_hidden(a->p, fi->scope, an);
                if (!b) { an_fail(a); return; }
                b->kind = BK_ARGUMENTS;
                fi->arguments_b = b;
            }
            /* a mapped arguments object aliases the parameters: they live in cells it can point to */
            if (!(fi->flags & FN_STRICT) && (fi->flags & FN_SIMPLE))
                for (Binding *b = fi->scope->first; b; b = b->next)
                    if (b->kind == BK_PARAM) b->flags |= BF_CAPTURED;
        }
    }
    annexb(a, fi);
    if (fi->flags & FN_DERIVED) this_binding_for(a, fi);    /* derived constructors keep this in a TDZ binding */
    for (int i = 0; i < fi->params.n && !a->failed; i++) analyze(a, fi->params.v[i], fi);
    analyze_list(a, &fi->body, fi);
    /* a direct eval may use any binding of this function and its parents: keep them all in cells */
    if (fi->has_direct_eval) {
        for (FuncInfo *f = fi; f; f = f->parent)
            for (Scope *s = f->scopes; s; s = s->next_in_func)
                for (Binding *b = s->first; b; b = b->next) {
                    if (global_scope(s) && b->kind != BK_HIDDEN) continue;
                    b->flags |= BF_CAPTURED;
                    if (b->scope->func != fi && !capture(a, b, fi)) return;
                }
    }
}

/* ================================================================== code generation state */

typedef struct IntVec { int *v; int n, cap; } IntVec;

static bool iv_push(Parser *p, IntVec *iv, int x) {
    if (iv->n == iv->cap) {
        int nc = iv->cap ? iv->cap * 2 : 8;
        int *nv = kz_arena_alloc(p, (size_t)nc * sizeof(int));
        if (!nv) return false;
        if (iv->n) memcpy(nv, iv->v, (size_t)iv->n * sizeof(int));
        iv->v = nv;
        iv->cap = nc;
    }
    iv->v[iv->n++] = x;
    return true;
}

enum { CB_LOOP = 1, CB_LABEL, CB_SWITCH, CB_FINALLY, CB_FOROF, CB_FORIN, CB_ITERDESTR };

typedef struct Cont { int kind; struct CB *target; bool is_continue; } Cont;  /* kind 0 jump, 1 return */

typedef struct CB {
    struct CB *prev;
    int kind;
    JSValue labels[8];
    int nlabels;
    bool breakable, continuable;
    bool async_iter;
    int depth;          /* stack depth below the block's own items */
    int items;          /* stack items the block keeps while it runs */
    IntVec breaks, continues;
    IntVec fin_gotos;   /* CB_FINALLY: jumps into the finally block */
    Cont *conts;
    int nconts, conts_cap;
} CB;

typedef struct FS {
    Parser *p;
    JSContext *ctx;
    FuncInfo *fi;
    struct FS *parent;
    uint8_t *code;
    uint32_t len, cap;
    KzVec *consts;
    int depth, max_depth;
    int nslots;
    CB *cb;
    KzExcEntry *exc;
    uint32_t nexc, exc_cap;
    KzPosEntry *pos;
    uint32_t npos, pos_cap;
    uint32_t nic;
    KzVarName *vn;
    uint32_t nvn, vn_cap;
    uint32_t last_line, last_col;
    int completion;     /* script/eval: slot of the completion value, or -1 */
    int ret_temp;
    bool global_code;   /* script (or sloppy eval in global code): top-level names are global properties */
    bool eval_code;
    bool failed;
    uint32_t *chash;    /* constant dedup: value hash -> index + 1 */
    uint32_t chash_size;
} FS;

static bool fs_oom(FS *fs) { fs->failed = true; fs->p->failed = true; return false; }

static bool grow_code(FS *fs, uint32_t extra) {
    if (fs->len + extra <= fs->cap) return true;
    uint32_t nc = fs->cap ? fs->cap * 2 : 256;
    while (nc < fs->len + extra) nc *= 2;
    uint8_t *c = kz_realloc(fs->ctx, fs->code, nc);
    if (!c) return fs_oom(fs);
    fs->code = c;
    fs->cap = nc;
    return true;
}

static void emit_byte(FS *fs, uint8_t b) { if (grow_code(fs, 1)) fs->code[fs->len++] = b; }
static void emit_u16(FS *fs, uint16_t v) { if (grow_code(fs, 2)) { fs->code[fs->len++] = (uint8_t)v; fs->code[fs->len++] = (uint8_t)(v >> 8); } }
static void emit_u32(FS *fs, uint32_t v) {
    if (!grow_code(fs, 4)) return;
    for (int i = 0; i < 4; i++) fs->code[fs->len++] = (uint8_t)(v >> (8 * i));
}
static void put_u32_at(FS *fs, uint32_t at, uint32_t v) { for (int i = 0; i < 4; i++) fs->code[at + i] = (uint8_t)(v >> (8 * i)); }

static void adjust(FS *fs, int delta) {
    fs->depth += delta;
    if (fs->depth > fs->max_depth) fs->max_depth = fs->depth;
}

static int emit_op(FS *fs, int op) {
    int at = (int)fs->len;
    emit_byte(fs, (uint8_t)op);
    const KzOpInfo *oi = &kz_op_info[op];
    if (oi->pop >= 0) adjust(fs, oi->push - oi->pop);
    return at;
}

static void emit_op_u8(FS *fs, int op, uint8_t v) { emit_op(fs, op); emit_byte(fs, v); }
static void emit_op_u16(FS *fs, int op, uint16_t v) { emit_op(fs, op); emit_u16(fs, v); }
static void emit_op_u32(FS *fs, int op, uint32_t v) { emit_op(fs, op); emit_u32(fs, v); }

static int emit_jump(FS *fs, int op) {
    int at = emit_op(fs, op);
    emit_u32(fs, 0);
    return at;
}

static void patch_to(FS *fs, int at, int target) {
    if (fs->failed) return;
    put_u32_at(fs, (uint32_t)at + 1, (uint32_t)(target - at));
}

static void patch_here(FS *fs, int at) { patch_to(fs, at, (int)fs->len); }

static void emit_jump_back(FS *fs, int op, int target) {
    int at = emit_op(fs, op);
    emit_u32(fs, (uint32_t)(target - at));
}

static void set_pos(FS *fs, const Node *n) {
    if (!n || !n->line) return;
    if (n->line == fs->last_line && n->col == fs->last_col) return;
    if (fs->npos == fs->pos_cap) {
        uint32_t nc = fs->pos_cap ? fs->pos_cap * 2 : 32;
        KzPosEntry *np = kz_realloc(fs->ctx, fs->pos, nc * sizeof(KzPosEntry));
        if (!np) { fs_oom(fs); return; }
        fs->pos = np;
        fs->pos_cap = nc;
    }
    if (fs->npos && fs->pos[fs->npos - 1].pc == fs->len) fs->npos--;
    fs->pos[fs->npos].pc = fs->len;
    fs->pos[fs->npos].line = n->line;
    fs->pos[fs->npos].col = n->col;
    fs->npos++;
    fs->last_line = n->line;
    fs->last_col = n->col;
}

static uint32_t value_hash(JSValue v) { return (uint32_t)(((uint64_t)v * 0x9E3779B97F4A7C15ULL) >> 32); }

/* a constant-pool index for v (deduplicated by identity) */
static uint32_t add_const(FS *fs, JSValue v) {
    if (fs->chash_size && (fs->consts->len + 1) * 2 > fs->chash_size) {
        /* rehash */
        uint32_t ns = fs->chash_size * 2;
        uint32_t *nh = kz_arena_alloc(fs->p, ns * sizeof(uint32_t));
        if (!nh) { fs_oom(fs); return 0; }
        for (uint32_t i = 0; i < fs->consts->len; i++) {
            uint32_t j = value_hash(fs->consts->v[i]) & (ns - 1);
            while (nh[j]) j = (j + 1) & (ns - 1);
            nh[j] = i + 1;
        }
        fs->chash = nh;
        fs->chash_size = ns;
    }
    if (!fs->chash_size) {
        fs->chash_size = 64;
        fs->chash = kz_arena_alloc(fs->p, 64 * sizeof(uint32_t));
        if (!fs->chash) { fs_oom(fs); return 0; }
    }
    bool dedup = !JV_IS_INTERNAL(v);
    uint32_t m = fs->chash_size - 1, j = value_hash(v) & m;
    if (dedup)
        for (; fs->chash[j]; j = (j + 1) & m)
            if (fs->consts->v[fs->chash[j] - 1] == v) return fs->chash[j] - 1;
    if (kz_vec_push(fs->ctx, fs->consts, v)) { fs_oom(fs); return 0; }
    uint32_t idx = fs->consts->len - 1;
    j = value_hash(v) & m;
    while (fs->chash[j]) j = (j + 1) & m;
    fs->chash[j] = idx + 1;
    return idx;
}

static uint32_t new_ic(FS *fs) { return fs->nic++; }

static void emit_atom_ic(FS *fs, int op, JSValue atom) {
    emit_op(fs, op);
    emit_u32(fs, add_const(fs, atom));
    emit_u32(fs, new_ic(fs));
}

static void emit_atom(FS *fs, int op, JSValue atom) {
    emit_op(fs, op);
    emit_u32(fs, add_const(fs, atom));
}

static int new_slot(FS *fs) {
    if (fs->nslots >= 65000) { kz_throw_range(fs->ctx, "too many local variables"); fs_oom(fs); return 0; }
    return fs->nslots++;
}

static void add_exc(FS *fs, uint32_t start, uint32_t end, uint32_t handler, int depth) {
    if (start >= end) return;
    if (fs->nexc == fs->exc_cap) {
        uint32_t nc = fs->exc_cap ? fs->exc_cap * 2 : 8;
        KzExcEntry *ne = kz_realloc(fs->ctx, fs->exc, nc * sizeof(KzExcEntry));
        if (!ne) { fs_oom(fs); return; }
        fs->exc = ne;
        fs->exc_cap = nc;
    }
    fs->exc[fs->nexc].start = start;
    fs->exc[fs->nexc].end = end;
    fs->exc[fs->nexc].handler = handler;
    fs->exc[fs->nexc].depth = (uint32_t)depth;
    fs->nexc++;
}

static void push_number(FS *fs, double d) {
    if (d >= -128 && d <= 127 && d == (int)d && !(d == 0 && __builtin_signbit(d))) {
        emit_op_u8(fs, OP_push_i8, (uint8_t)(int8_t)(int)d);
        return;
    }
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == (int32_t)d && !(d == 0 && __builtin_signbit(d))) {
        emit_op_u32(fs, OP_push_i32, (uint32_t)(int32_t)d);
        return;
    }
    emit_op_u32(fs, OP_push_const, add_const(fs, jv_from_raw_double(d)));
}

static void push_string(FS *fs, JSValue s) { emit_op_u32(fs, OP_push_const, add_const(fs, s)); }

/* ================================================================== bindings: load and store */

static bool in_this_fs(FS *fs, Binding *b) { return b->scope->func == fs->fi; }

/* does a use at `use` of b need a TDZ check? */
static bool needs_tdz(FS *fs, Binding *b, const Node *use) {
    if (!is_lexical_kind_x(b->kind) && !(b->flags & BF_TDZ) && b->kind != BK_IMPORT) {
        if (b->kind == BK_PARAM && !(fs->fi->flags & FN_SIMPLE) && in_this_fs(fs, b)) return true;
        return false;
    }
    if (b->kind == BK_IMPORT) return true;
    if (!in_this_fs(fs, b)) return true;
    if (b->scope->kind == SC_SWITCH) return true;
    if (!use || use->pos < b->decl_end || b->decl_end == 0) return true;
    return false;
}

static void emit_load_binding(FS *fs, Binding *b, bool check) {
    if (in_this_fs(fs, b)) {
        if (b->flags & BF_CAPTURED) emit_op_u16(fs, check ? OP_get_lcell_check : OP_get_lcell, (uint16_t)b->slot);
        else emit_op_u16(fs, check ? OP_get_loc_check : OP_get_loc, (uint16_t)b->slot);
    } else {
        int idx = captured_index(fs->fi, b);
        emit_op_u16(fs, check ? OP_get_cvar_check : OP_get_cvar, (uint16_t)(idx < 0 ? 0 : idx));
    }
}

/* stores the top of the stack into b: keep = leave the value on the stack; init = the declaration itself */
static void emit_store_binding(FS *fs, Binding *b, bool keep, bool check, bool init) {
    if (in_this_fs(fs, b)) {
        if (b->flags & BF_CAPTURED) {
            if (init) {
                if (keep) emit_op(fs, OP_dup);
                emit_op_u16(fs, OP_init_lcell, (uint16_t)b->slot);
            } else emit_op_u16(fs, keep ? OP_set_lcell : (check ? OP_put_lcell_check : OP_put_lcell), (uint16_t)b->slot);
            if (keep && check && !init) { /* set_lcell does no check: done via put + get below */ }
        } else {
            if (keep && !(check && !init)) emit_op_u16(fs, OP_set_loc, (uint16_t)b->slot);
            else if (keep) { emit_op(fs, OP_dup); emit_op_u16(fs, OP_put_loc_check, (uint16_t)b->slot); }
            else emit_op_u16(fs, (check && !init) ? OP_put_loc_check : OP_put_loc, (uint16_t)b->slot);
        }
    } else {
        int idx = captured_index(fs->fi, b);
        if (idx < 0) idx = 0;
        if (init) {
            if (keep) emit_op(fs, OP_dup);
            emit_op_u16(fs, OP_init_cvar, (uint16_t)idx);
        } else if (keep && check) { emit_op(fs, OP_dup); emit_op_u16(fs, OP_put_cvar_check, (uint16_t)idx); }
        else emit_op_u16(fs, keep ? OP_set_cvar : (check ? OP_put_cvar_check : OP_put_cvar), (uint16_t)idx);
    }
}

/* captured lets need the checked store when not initialising; set_lcell has no check, so use dup + put_check */
static void emit_assign_binding(FS *fs, Binding *b, bool keep, const Node *use) {
    bool check = needs_tdz(fs, b, use);
    if (b->kind == BK_CONST || b->kind == BK_IMPORT || (b->kind == BK_CALLEE && (fs->fi->flags & FN_STRICT))) {
        /* TDZ first (a ReferenceError beats the TypeError), then the error */
        if (check) {
            emit_load_binding(fs, b, true);
            emit_op(fs, OP_drop);
        }
        emit_atom(fs, OP_throw_const, b->name);
        if (!keep) emit_op(fs, OP_drop);
        return;
    }
    if (b->kind == BK_CALLEE) {             /* sloppy: silently ignored */
        if (!keep) emit_op(fs, OP_drop);
        return;
    }
    if (check && (b->flags & BF_CAPTURED) && in_this_fs(fs, b) && keep) {
        emit_op(fs, OP_dup);
        emit_op_u16(fs, OP_put_lcell_check, (uint16_t)b->slot);
        return;
    }
    emit_store_binding(fs, b, keep, check, false);
}

/* the dynamic scope objects (with, eval vars) to consult before the static binding */
static void emit_dyn_prefix(FS *fs, Node *ref, int op, IntVec *found) {
    for (int i = 0; i < ref->list.n; i++) {
        Node *d = ref->list.v[i];
        emit_load_binding(fs, d->binding, false);
        int at = emit_op(fs, op);
        emit_u32(fs, add_const(fs, ref->val));
        emit_u32(fs, 0);
        emit_byte(fs, (uint8_t)d->ival);
        iv_push(fs->p, found, at);
    }
}

static void patch_dyn(FS *fs, IntVec *found, int offset_pos) {
    for (int i = 0; i < found->n; i++) put_u32_at(fs, (uint32_t)found->v[i] + 5, (uint32_t)((int)fs->len - found->v[i]));
    (void)offset_pos;
}

static void gen_ident_load(FS *fs, Node *n, bool for_typeof) {
    IntVec found = { 0 };
    int base = fs->depth;
    emit_dyn_prefix(fs, n, for_typeof ? OP_with_typeof : OP_with_get, &found);
    Binding *b = n->binding;
    if (!b) emit_atom_ic(fs, for_typeof ? OP_get_global_typeof : OP_get_global, n->val);
    else emit_load_binding(fs, b, needs_tdz(fs, b, n));
    patch_dyn(fs, &found, 0);
    fs->depth = base + 1;
    if (fs->depth > fs->max_depth) fs->max_depth = fs->depth;
}

/* value on the stack -> stored into the identifier's binding */
static void gen_ident_store(FS *fs, Node *n, bool keep) {
    IntVec found = { 0 };
    int base = fs->depth;
    for (int i = 0; i < n->list.n; i++) {
        Node *d = n->list.v[i];
        emit_load_binding(fs, d->binding, false);
        int at = emit_op(fs, OP_with_put);
        emit_u32(fs, add_const(fs, n->val));
        emit_u32(fs, 0);
        emit_byte(fs, (uint8_t)d->ival);
        iv_push(fs->p, &found, at);
    }
    Binding *b = n->binding;
    if (!b) {
        emit_atom_ic(fs, OP_set_global, n->val);
    } else emit_assign_binding(fs, b, true, n);
    patch_dyn(fs, &found, 0);
    fs->depth = base;
    if (!keep) emit_op(fs, OP_drop);
}

/* initialises a declared binding (var/let/const/param/catch) from the stack */
static void gen_binding_init(FS *fs, Node *id, int decl) {
    Binding *b = id->binding;
    if (!b) {
        /* global code: var and function names are global properties; let/const/class are global lexicals */
        if (decl == V_LET || decl == V_CONST || decl == BK_CLASS) emit_atom_ic(fs, OP_put_global_init, id->val);
        else emit_atom_ic(fs, OP_put_global, id->val);
        return;
    }
    if (decl == V_VAR && id->list.n) {
        /* a var in a function with a sloppy eval: the binding may be dynamic; store by the usual path */
        gen_ident_store(fs, id, false);
        return;
    }
    if (decl == V_VAR) emit_store_binding(fs, b, false, false, false);
    else emit_store_binding(fs, b, false, false, true);
}

/* ================================================================== forward declarations */

static void gen_expr(FS *fs, Node *n);
static void gen_expr_named(FS *fs, Node *n, JSValue name);
static void gen_stmt(FS *fs, Node *n);
static void gen_stmts(FS *fs, NodeList *l);
static JSFunctionBytecode *gen_function(FS *parent, FuncInfo *fi, JSValue name_hint);
static void gen_destructure(FS *fs, Node *pat, int decl);
static void gen_class(FS *fs, Node *n, JSValue name_hint);
static void gen_return_value(FS *fs);
static void enter_scope(FS *fs, Scope *s);

static bool is_anon_fn(Node *n) {
    if (!n) return false;
    if (n->type == N_ARROW) return true;
    if (n->type == N_FUNC) return !n->func->name;
    if (n->type == N_CLASS) return n->val == JS_UNDEFINED;
    return false;
}

/* ================================================================== functions */

static void gen_closure(FS *fs, Node *fn, JSValue name_hint) {
    JSFunctionBytecode *b = gen_function(fs, fn->func, name_hint);
    if (!b) { fs_oom(fs); emit_op(fs, OP_push_undefined); return; }
    emit_op_u32(fs, OP_closure, add_const(fs, JV_MKPTR(b)));
}

/* a function expression, with its own name scope when named */
static void gen_func_expr(FS *fs, Node *n, JSValue name_hint) {
    if (n->type == N_FUNC && n->func->name && (n->func->flags & FN_EXPR_NAMED)) {
        /* the callee scope: a binding of the function to itself, a cell if the body uses it */
        Scope *cs = n->func->scope->parent;
        Binding *cb = cs && cs->kind == SC_CALLEE ? cs->first : NULL;
        if (cb && (cb->flags & BF_CAPTURED)) {
            if (cb->slot < 0) cb->slot = (int16_t)new_slot(fs);
            emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)cb->slot);
            gen_closure(fs, n, 0);
            emit_op(fs, OP_dup);
            emit_op_u16(fs, OP_init_lcell, (uint16_t)cb->slot);
            return;
        }
    }
    gen_closure(fs, n, name_hint);
}

static void gen_expr_named(FS *fs, Node *n, JSValue name) {
    if (name && is_anon_fn(n)) {
        if (n->type == N_CLASS) { gen_class(fs, n, name); return; }
        gen_func_expr(fs, n, name);
        return;
    }
    gen_expr(fs, n);
}

/* ================================================================== references */

enum { REF_VAR, REF_FIELD, REF_ELEM, REF_SUPER_FIELD, REF_SUPER_ELEM, REF_PRIVATE, REF_BAD };

typedef struct Ref { int kind; Node *node; } Ref;

static void gen_super_base(FS *fs, Node *super_node) {
    /* pushes the receiver (this) and super's base object (home.[[Prototype]]) */
    if (super_node->binding) emit_load_binding(fs, super_node->binding, (fs->fi->flags & FN_DERIVED) || !in_this_fs(fs, super_node->binding));
    else emit_op(fs, OP_push_this);
    FuncInfo *g = non_arrow(fs->fi);
    if (g && g != fs->fi && g->home_b) emit_load_binding(fs, g->home_b, false);
    else emit_op(fs, OP_push_home);
    emit_op(fs, OP_get_super);
}

/* evaluates the parts of a reference that come before the value: object, key */
static Ref gen_ref(FS *fs, Node *t) {
    Ref r = { REF_BAD, t };
    while (t->type == N_PAREN) t = t->a;
    r.node = t;
    switch (t->type) {
    case N_IDENT:
        r.kind = REF_VAR;
        return r;
    case N_MEMBER:
        if (t->a->type == N_SUPER) {
            gen_super_base(fs, t->a);
            if (t->flags & F_COMPUTED) {
                gen_expr(fs, t->b);
                emit_op(fs, OP_to_propkey);
                r.kind = REF_SUPER_ELEM;
            } else r.kind = REF_SUPER_FIELD;
            return r;
        }
        gen_expr(fs, t->a);
        if (t->flags & F_COMPUTED) {
            gen_expr(fs, t->b);
            r.kind = REF_ELEM;
        } else r.kind = REF_FIELD;
        return r;
    case N_PRIVATE_MEMBER:
        gen_expr(fs, t->a);
        if (t->binding) emit_load_binding(fs, t->binding, false);
        else emit_op(fs, OP_push_undefined);
        r.kind = REF_PRIVATE;
        return r;
    default:
        /* f() = x in sloppy code: evaluate the call, then a ReferenceError */
        gen_expr(fs, t);
        emit_op(fs, OP_drop);
        r.kind = REF_BAD;
        return r;
    }
}

static int ref_parts(const Ref *r) {
    switch (r->kind) {
    case REF_FIELD: return 1;
    case REF_ELEM: case REF_SUPER_FIELD: case REF_PRIVATE: return 2;
    case REF_SUPER_ELEM: return 3;
    default: return 0;
    }
}

/* duplicates the reference parts and loads the current value: parts -> parts value */
static void gen_ref_load_keep(FS *fs, Ref *r) {
    Node *t = r->node;
    switch (r->kind) {
    case REF_VAR: gen_ident_load(fs, t, false); break;
    case REF_FIELD: emit_op(fs, OP_dup); emit_atom_ic(fs, OP_get_field, t->val); break;
    case REF_ELEM: emit_op(fs, OP_dup2); emit_op(fs, OP_get_elem); break;
    case REF_SUPER_FIELD: emit_op(fs, OP_dup2); emit_atom(fs, OP_get_field_recv, t->val); break;
    case REF_PRIVATE: emit_op(fs, OP_dup2); emit_op(fs, OP_get_private); break;
    case REF_SUPER_ELEM: {
        /* receiver obj key: copy all three */
        int s1 = new_slot(fs), s2 = new_slot(fs), s3 = new_slot(fs);
        emit_op_u16(fs, OP_put_loc, (uint16_t)s3);
        emit_op_u16(fs, OP_put_loc, (uint16_t)s2);
        emit_op_u16(fs, OP_put_loc, (uint16_t)s1);
        for (int k = 0; k < 2; k++) {
            emit_op_u16(fs, OP_get_loc, (uint16_t)s1);
            emit_op_u16(fs, OP_get_loc, (uint16_t)s2);
            emit_op_u16(fs, OP_get_loc, (uint16_t)s3);
        }
        emit_op(fs, OP_get_elem_recv);
        break;
    }
    default: emit_op(fs, OP_push_undefined); break;
    }
}

/* parts value -> (value if keep) */
static void gen_ref_store(FS *fs, Ref *r, bool keep) {
    Node *t = r->node;
    switch (r->kind) {
    case REF_VAR: gen_ident_store(fs, t, keep); return;
    case REF_FIELD:
        if (keep) emit_op(fs, OP_insert2);
        emit_atom_ic(fs, OP_put_field, t->val);
        return;
    case REF_ELEM:
        if (keep) emit_op(fs, OP_insert3);
        emit_op(fs, OP_put_elem);
        return;
    case REF_SUPER_FIELD:
        if (keep) emit_op(fs, OP_insert3);
        emit_atom(fs, OP_put_field_recv, t->val);
        return;
    case REF_PRIVATE:
        if (keep) emit_op(fs, OP_insert3);
        emit_op(fs, OP_put_private);
        return;
    case REF_SUPER_ELEM:
        if (keep) emit_op(fs, OP_insert4);
        emit_op(fs, OP_put_elem_recv);
        return;
    default:
        emit_op_u32(fs, OP_throw_ref, add_const(fs, kz_str_from_cstr(fs->ctx, "Invalid left-hand side in assignment")));
        if (!keep) emit_op(fs, OP_drop);
        return;
    }
}

static JSValue ref_name(Node *t) {
    while (t->type == N_PAREN) t = t->a;
    return t->type == N_IDENT ? t->val : 0;
}

/* ================================================================== expressions */

static int binop_for_token(int tok) {
    switch (tok) {
    case T_PLUS: case T_PLUS_ASSIGN: return OP_add;
    case T_MINUS: case T_MINUS_ASSIGN: return OP_sub;
    case T_STAR: case T_STAR_ASSIGN: return OP_mul;
    case T_SLASH: case T_SLASH_ASSIGN: return OP_div;
    case T_PERCENT: case T_PERCENT_ASSIGN: return OP_mod;
    case T_STARSTAR: case T_STARSTAR_ASSIGN: return OP_pow;
    case T_AMP: case T_AMP_ASSIGN: return OP_band;
    case T_PIPE: case T_PIPE_ASSIGN: return OP_bor;
    case T_CARET: case T_CARET_ASSIGN: return OP_bxor;
    case T_SHL: case T_SHL_ASSIGN: return OP_shl;
    case T_SAR: case T_SAR_ASSIGN: return OP_sar;
    case T_SHR: case T_SHR_ASSIGN: return OP_shr;
    case T_LT: return OP_lt;
    case T_LE: return OP_le;
    case T_GT: return OP_gt;
    case T_GE: return OP_ge;
    case T_EQ: return OP_eq;
    case T_NE: return OP_ne;
    case T_SEQ: return OP_seq;
    case T_SNE: return OP_sne;
    case OP_KW + KW_instanceof: return OP_instanceof;
    case OP_KW + KW_in: return OP_in;
    }
    return OP_nop;
}

static void gen_assign(FS *fs, Node *n, bool keep) {
    Node *t = n->a;
    int op = n->op;
    if (op == T_ASSIGN && (t->type == N_OBJ_PATTERN || t->type == N_ARR_PATTERN)) {
        gen_expr(fs, n->b);
        if (keep) emit_op(fs, OP_dup);
        gen_destructure(fs, t, 0);
        return;
    }
    Ref r = gen_ref(fs, t);
    set_pos(fs, n);
    if (op == T_ASSIGN) {
        gen_expr_named(fs, n->b, ref_name(t));
        gen_ref_store(fs, &r, keep);
        return;
    }
    if (op == T_ANDAND_ASSIGN || op == T_OROR_ASSIGN || op == T_QQ_ASSIGN) {
        /* short-circuit: the store happens only when the right side runs */
        int parts = ref_parts(&r);
        gen_ref_load_keep(fs, &r);              /* parts value */
        int skip_depth = fs->depth;
        int j;
        if (op == T_QQ_ASSIGN) j = emit_jump(fs, OP_if_not_nullish);
        else {
            emit_op(fs, OP_dup);
            j = emit_jump(fs, op == T_ANDAND_ASSIGN ? OP_if_false : OP_if_true);
        }
        emit_op(fs, OP_drop);                   /* parts */
        gen_expr_named(fs, n->b, ref_name(t));
        gen_ref_store(fs, &r, true);            /* value */
        int done = emit_jump(fs, OP_goto);
        patch_here(fs, j);
        fs->depth = skip_depth;                 /* parts value: drop the parts under the value */
        for (int i = 0; i < parts; i++) emit_op(fs, OP_nip);
        patch_here(fs, done);
        if (!keep) emit_op(fs, OP_drop);
        return;
    }
    gen_ref_load_keep(fs, &r);
    gen_expr(fs, n->b);
    emit_op(fs, binop_for_token(op));
    gen_ref_store(fs, &r, keep);
}

static void gen_update(FS *fs, Node *n, bool keep) {
    Ref r = gen_ref(fs, n->a);
    set_pos(fs, n);
    int op = n->op == T_INC ? OP_inc : OP_dec;
    gen_ref_load_keep(fs, &r);
    if (n->flags & F_PREFIX || !keep) {
        emit_op(fs, op);
        gen_ref_store(fs, &r, keep);
        return;
    }
    /* postfix with a used value: the old value (as a number) stays under the reference parts */
    emit_op(fs, OP_to_numeric);
    int parts = ref_parts(&r);
    switch (parts) {
    case 0: emit_op(fs, OP_dup); break;
    case 1: emit_op(fs, OP_insert2); break;
    case 2: emit_op(fs, OP_insert3); break;
    default: emit_op(fs, OP_insert4); break;
    }
    emit_op(fs, op);
    gen_ref_store(fs, &r, false);
}

/* arguments: returns the count, or -1 when a spread made them into an array on the stack */
static int gen_args(FS *fs, NodeList *args) {
    bool spread = false;
    for (int i = 0; i < args->n; i++) if (args->v[i]->type == N_SPREAD) spread = true;
    if (!spread) {
        for (int i = 0; i < args->n; i++) gen_expr(fs, args->v[i]);
        return args->n;
    }
    emit_op_u16(fs, OP_array_from, 0);
    adjust(fs, 1);
    for (int i = 0; i < args->n; i++) {
        Node *a = args->v[i];
        if (a->type == N_SPREAD) { gen_expr(fs, a->a); emit_op(fs, OP_array_append); }
        else { gen_expr(fs, a); emit_op(fs, OP_array_push); }
    }
    return -1;
}

static void emit_call(FS *fs, int argc) {
    if (argc < 0) { emit_op(fs, OP_call_spread); return; }
    emit_op_u16(fs, OP_call, (uint16_t)argc);
    adjust(fs, -(argc + 2) + 1);
}

/* the eval scope descriptor for a direct eval at this point: every visible binding, innermost first */
static void gen_eval_site(FS *fs, Node *call, uint16_t *first, uint16_t *count) {
    *first = (uint16_t)fs->nvn;
    FuncInfo *fi = fs->fi;
    for (Scope *s = call->scope; s; s = s->parent) {
        for (Binding *b = s->first; b; b = b->next) {
            if (global_scope(s) && b->kind != BK_HIDDEN) continue;
            int vk;
            switch (b->kind) {
            case BK_LET: case BK_CATCH: vk = VK_LET; break;
            case BK_CONST: vk = s->kind == SC_CLASS ? VK_CLASS_INNER : VK_CONST; break;
            case BK_CLASS: vk = VK_LET; break;
            case BK_CALLEE: vk = VK_CALLEE; break;
            case BK_FUNC: vk = VK_FUNC; break;
            case BK_PARAM: vk = VK_PARAM; break;
            case BK_ARGUMENTS: vk = VK_ARGUMENTS; break;
            case BK_HIDDEN:
                if (b == b->scope->func->this_b) vk = VK_THIS;
                else if (b == b->scope->func->new_target_b) vk = VK_NEW_TARGET;
                else if (b == b->scope->func->home_b) vk = VK_HOME;
                else if (s->kind == SC_WITH && b == s->with_obj) vk = VK_WITH;
                else if (b == s->eval_obj) vk = VK_EVAL_OBJ;
                else if (JV_IS_STRING(b->name) && kz_str_len(JV_STR(b->name)) > 0 && kz_str_at(kz_str_flat(fs->ctx, JV_STR(b->name)), 0) == '#') vk = VK_PRIVATE;
                else continue;
                break;
            default: vk = VK_VAR; break;
            }
            if (!(b->flags & BF_CAPTURED) && in_this_fs(fs, b)) continue;     /* (cannot happen with eval) */
            if (fs->nvn == fs->vn_cap) {
                uint32_t nc = fs->vn_cap ? fs->vn_cap * 2 : 16;
                KzVarName *nv = kz_realloc(fs->ctx, fs->vn, nc * sizeof(KzVarName));
                if (!nv) { fs_oom(fs); return; }
                fs->vn = nv;
                fs->vn_cap = nc;
            }
            KzVarName *e = &fs->vn[fs->nvn++];
            e->name = b->name;
            e->kind = (uint8_t)vk;
            if (in_this_fs(fs, b)) { e->where = VN_LOCAL_CELL; e->slot = (uint16_t)b->slot; }
            else { e->where = VN_CLOSURE; e->slot = (uint16_t)captured_index(fi, b); }
            e->scope_start = e->scope_end = 0;
        }
        /* the function boundary of a sloppy function: eval's vars go to the innermost var scope's object */
        if ((s->kind == SC_FUNC || s->kind == SC_EVAL) && s->func && s == s->func->body_scope && sloppy_eval_func(s->func) &&
            !(s->func->flags & FN_SCRIPT) && !s->eval_obj) { }
    }
    *count = (uint16_t)(fs->nvn - *first);
}

/* stores the top of the stack (super()'s result) into the derived constructor's this binding; a ReferenceError if
   it was already initialised */
static void gen_init_this(FS *fs, Binding *tb) {
    if (in_this_fs(fs, tb)) emit_op_u16(fs, (tb->flags & BF_CAPTURED) ? OP_init_this_lcell : OP_init_this_loc, (uint16_t)tb->slot);
    else emit_op_u16(fs, OP_init_this_cvar, (uint16_t)captured_index(fs->fi, tb));
}

/* super(args): [the active function, new.target, args] -> the new this, then field initialisers */
static void gen_super_call(FS *fs, Node *n, Node *callee) {
    FuncInfo *g = non_arrow(fs->fi);
    if (g == fs->fi) emit_op(fs, OP_push_func);
    else emit_load_binding(fs, g->fields_b, false);
    if (g == fs->fi) emit_op(fs, OP_push_new_target);
    else emit_load_binding(fs, g->new_target_b, false);
    set_pos(fs, n);
    int argc = gen_args(fs, &n->list);
    if (argc < 0) emit_op(fs, OP_super_call_spread);
    else { emit_op_u16(fs, OP_super_call, (uint16_t)argc); adjust(fs, -(argc + 2) + 1); }
    Binding *tb = callee->binding ? callee->binding : g->this_b;
    emit_op(fs, OP_dup);
    gen_init_this(fs, tb);
    if (g->has_instance_elements) {
        /* this -> this func this -> (run_fields) -> this */
        if (g == fs->fi) emit_op(fs, OP_push_func);
        else emit_load_binding(fs, g->fields_b, false);
        emit_op(fs, OP_dup_under);           /* this this func */
        emit_op(fs, OP_swap);                /* this func this */
        emit_op(fs, OP_run_fields);
    }
}

static void gen_call(FS *fs, Node *n) {
    Node *callee = n->a;
    while (callee->type == N_PAREN && callee->a->type != N_IDENT) callee = callee->a;
    if (callee->type == N_SUPER) {
        gen_super_call(fs, n, callee);
        return;
    }
    if (callee->type == N_MEMBER && callee->a->type == N_SUPER) {
        gen_super_base(fs, callee->a);           /* receiver base */
        if (!(callee->flags & F_COMPUTED)) {
            emit_op(fs, OP_dup_under);           /* r r b */
            emit_atom(fs, OP_get_field_recv, callee->val);   /* r f */
            emit_op(fs, OP_swap);                /* f r */
        } else {
            int sb = new_slot(fs), sk = new_slot(fs);
            gen_expr(fs, callee->b);
            emit_op(fs, OP_to_propkey);          /* r b k */
            emit_op_u16(fs, OP_put_loc, (uint16_t)sk);
            emit_op_u16(fs, OP_put_loc, (uint16_t)sb);
            emit_op(fs, OP_dup);                 /* r r */
            emit_op_u16(fs, OP_get_loc, (uint16_t)sb);
            emit_op_u16(fs, OP_get_loc, (uint16_t)sk);
            emit_op(fs, OP_get_elem_recv);       /* r f */
            emit_op(fs, OP_swap);                /* f r */
        }
        set_pos(fs, n);
        emit_call(fs, gen_args(fs, &n->list));
        return;
    }
    if (callee->type == N_MEMBER) {
        gen_expr(fs, callee->a);
        set_pos(fs, callee);
        if (callee->flags & F_COMPUTED) { gen_expr(fs, callee->b); emit_op(fs, OP_get_elem2); }
        else emit_atom_ic(fs, OP_get_field2, callee->val);
        set_pos(fs, n);
        emit_call(fs, gen_args(fs, &n->list));
        return;
    }
    if (callee->type == N_PRIVATE_MEMBER) {
        gen_expr(fs, callee->a);
        emit_op(fs, OP_dup);
        if (callee->binding) emit_load_binding(fs, callee->binding, false); else emit_op(fs, OP_push_undefined);
        emit_op(fs, OP_get_private);         /* obj f */
        emit_op(fs, OP_swap);                /* f obj */
        set_pos(fs, n);
        emit_call(fs, gen_args(fs, &n->list));
        return;
    }
    Node *id = callee->type == N_PAREN ? callee->a : callee;
    if (id->type == N_IDENT && id->list.n) {
        /* inside with: a function found on the with object is called with it as this */
        IntVec found = { 0 };
        int base = fs->depth;
        emit_dyn_prefix(fs, id, OP_with_call, &found);
        if (!id->binding) emit_atom_ic(fs, OP_get_global, id->val);
        else emit_load_binding(fs, id->binding, needs_tdz(fs, id->binding, id));
        emit_op(fs, OP_push_undefined);
        patch_dyn(fs, &found, 0);
        fs->depth = base + 2;
        if (fs->depth > fs->max_depth) fs->max_depth = fs->depth;
    } else {
        gen_expr(fs, callee);
        emit_op(fs, OP_push_undefined);
    }
    set_pos(fs, n);
    if (n->flags & F_DIRECT_EVAL) {
        int argc = gen_args(fs, &n->list);
        if (argc < 0) { emit_op(fs, OP_call_spread); return; }
        uint16_t first = 0, count = 0;
        gen_eval_site(fs, n, &first, &count);
        emit_op(fs, OP_eval);
        emit_u16(fs, (uint16_t)argc);
        emit_u16(fs, first);
        emit_u16(fs, count);
        adjust(fs, -(argc + 2) + 1);
        return;
    }
    emit_call(fs, gen_args(fs, &n->list));
}

/* optional chains: every ?. jumps to `nullish` (through a trampoline that drops what is above the chain's base) */
typedef struct OptCtx { IntVec jumps; IntVec drops; } OptCtx;

static void gen_chain(FS *fs, Node *n, OptCtx *oc, int base);

static void opt_check(FS *fs, OptCtx *oc, int base) {
    int at = emit_jump(fs, OP_if_nullish);
    iv_push(fs->p, &oc->jumps, at);
    iv_push(fs->p, &oc->drops, fs->depth - base);
}

/* generates a chain element; for calls inside chains the [func this] pair is built here */
static void gen_chain_callee(FS *fs, Node *callee, OptCtx *oc, int base) {
    /* pushes func this */
    if (callee->type == N_MEMBER && callee->a->type != N_SUPER) {
        gen_chain(fs, callee->a, oc, base);
        if (callee->flags & F_OPTIONAL) opt_check(fs, oc, base);
        if (callee->flags & F_COMPUTED) { gen_expr(fs, callee->b); emit_op(fs, OP_get_elem2); }
        else emit_atom_ic(fs, OP_get_field2, callee->val);
        return;
    }
    if (callee->type == N_PRIVATE_MEMBER) {
        gen_chain(fs, callee->a, oc, base);
        if (callee->flags & F_OPTIONAL) opt_check(fs, oc, base);
        emit_op(fs, OP_dup);
        if (callee->binding) emit_load_binding(fs, callee->binding, false); else emit_op(fs, OP_push_undefined);
        emit_op(fs, OP_get_private);
        emit_op(fs, OP_swap);
        return;
    }
    gen_chain(fs, callee, oc, base);
    emit_op(fs, OP_push_undefined);
}

static void gen_chain(FS *fs, Node *n, OptCtx *oc, int base) {
    switch (n->type) {
    case N_MEMBER:
        if (n->a->type == N_SUPER) { gen_expr(fs, n); return; }
        gen_chain(fs, n->a, oc, base);
        if (n->flags & F_OPTIONAL) opt_check(fs, oc, base);
        set_pos(fs, n);
        if (n->flags & F_COMPUTED) { gen_expr(fs, n->b); emit_op(fs, OP_get_elem); }
        else emit_atom_ic(fs, OP_get_field, n->val);
        return;
    case N_PRIVATE_MEMBER:
        gen_chain(fs, n->a, oc, base);
        if (n->flags & F_OPTIONAL) opt_check(fs, oc, base);
        if (n->binding) emit_load_binding(fs, n->binding, false); else emit_op(fs, OP_push_undefined);
        emit_op(fs, OP_get_private);
        return;
    case N_CALL:
        if (n->a->type == N_SUPER || (n->flags & F_DIRECT_EVAL)) { gen_call(fs, n); return; }
        gen_chain_callee(fs, n->a, oc, base);
        if (n->flags & F_OPTIONAL) {
            /* callee?.(): test the function (below this) */
            emit_op(fs, OP_swap);
            opt_check(fs, oc, base);
            emit_op(fs, OP_swap);
        }
        set_pos(fs, n);
        emit_call(fs, gen_args(fs, &n->list));
        return;
    default:
        gen_expr(fs, n);
    }
}

static void gen_opt_chain(FS *fs, Node *n) {
    OptCtx oc = { { 0 }, { 0 } };
    int base = fs->depth;
    gen_chain(fs, n->a, &oc, base);
    int done = emit_jump(fs, OP_goto);
    /* trampolines: drop what each check left above the base, then undefined */
    IntVec to_end = { 0 };
    for (int i = 0; i < oc.jumps.n; i++) {
        patch_here(fs, oc.jumps.v[i]);
        fs->depth = base + oc.drops.v[i];
        for (int k = 0; k < oc.drops.v[i]; k++) emit_op(fs, OP_drop);
        emit_op(fs, OP_push_undefined);
        iv_push(fs->p, &to_end, emit_jump(fs, OP_goto));
    }
    patch_here(fs, done);
    for (int i = 0; i < to_end.n; i++) patch_here(fs, to_end.v[i]);
    fs->depth = base + 1;
}

static void gen_template(FS *fs, Node *n) {
    bool first = true;
    for (int i = 0; i < n->list.n; i++) {
        Node *e = n->list.v[i];
        if (e->type == N_TEMPLATE_PART) {
            if (kz_str_len(JV_STR(e->val)) == 0 && !(first && i == n->list.n - 1)) continue;
            push_string(fs, e->val);
        } else {
            gen_expr(fs, e);
            emit_op(fs, OP_to_string);
        }
        if (!first) emit_op(fs, OP_add);
        first = false;
    }
    if (first) push_string(fs, fs->ctx->rt->atom[KZ_ATOM_empty]);
}

static void gen_tagged(FS *fs, Node *n) {
    Node *tag = n->a, *tpl = n->b;
    /* the template object: an internal [cooked..., raw...] pair the VM turns into the frozen array once per site */
    int nparts = 0;
    for (int i = 0; i < tpl->list.n; i++) if (tpl->list.v[i]->type == N_TEMPLATE_PART) nparts++;
    KZ_ROOTS(fs->ctx, r, 3);
    r[0] = kz_new_array(fs->ctx, 0);
    r[1] = kz_new_array(fs->ctx, 0);
    if (r[0] == JS_EXCEPTION || r[1] == JS_EXCEPTION) { fs_oom(fs); return; }
    for (int i = 0; i < tpl->list.n; i++) {
        Node *e = tpl->list.v[i];
        if (e->type != N_TEMPLATE_PART) continue;
        if (kz_array_push(fs->ctx, JV_OBJ(r[0]), e->val) || kz_array_push(fs->ctx, JV_OBJ(r[1]), e->val2)) { fs_oom(fs); return; }
    }
    r[2] = kz_new_array_from(fs->ctx, 2, r);
    if (r[2] == JS_EXCEPTION) { fs_oom(fs); return; }
    uint32_t ci = add_const(fs, r[2]);
    (void)nparts;
    if (tag->type == N_MEMBER && tag->a->type != N_SUPER) {
        gen_expr(fs, tag->a);
        if (tag->flags & F_COMPUTED) { gen_expr(fs, tag->b); emit_op(fs, OP_get_elem2); }
        else emit_atom_ic(fs, OP_get_field2, tag->val);
    } else {
        gen_expr(fs, tag);
        emit_op(fs, OP_push_undefined);
    }
    emit_op_u32(fs, OP_template_object, ci);
    int argc = 1;
    for (int i = 0; i < tpl->list.n; i++) {
        Node *e = tpl->list.v[i];
        if (e->type == N_TEMPLATE_PART) continue;
        gen_expr(fs, e);
        argc++;
    }
    set_pos(fs, n);
    emit_call(fs, argc);
}

static JSValue literal_key(FS *fs, Node *k) {
    /* the property key of a non-computed literal key: an atom, or an index (via a number) */
    if (k->type == N_STRING) {
        KZ_ROOT(fs->ctx, s);
        s = k->val;
        JSValue key = kz_key_from_value(fs->ctx, s);
        if (key == JS_EXCEPTION) { fs_oom(fs); return JS_UNDEFINED; }
        return key;
    }
    if (k->type == N_NUMBER) {
        JSValue key = kz_key_from_value(fs->ctx, jv_from_raw_double(k->num));
        if (key == JS_EXCEPTION) { fs_oom(fs); return JS_UNDEFINED; }
        if (kz_parser_keep(fs->p, key) == JS_EXCEPTION) { fs_oom(fs); return JS_UNDEFINED; }
        return key;
    }
    if (k->type == N_BIGINT) {
        JSValue b = kz_bigint_literal(fs->ctx, (const char *)kz_str8(JV_STR(k->val)), kz_str_len(JV_STR(k->val)), k->ival);
        if (b == JS_EXCEPTION) { fs_oom(fs); return JS_UNDEFINED; }
        KZ_ROOT(fs->ctx, rb);
        rb = b;
        JSValue key = kz_key_from_value(fs->ctx, rb);
        if (key == JS_EXCEPTION || kz_parser_keep(fs->p, key) == JS_EXCEPTION) { fs_oom(fs); return JS_UNDEFINED; }
        return key;
    }
    return k->val;
}

/* pushes a key for a define_elem-style op (index keys as numbers) and reports whether it is an atom */
static bool key_is_atom(JSValue key) { return JV_IS_STRING(key) || JV_IS_SYMBOL(key); }

static void push_key(FS *fs, JSValue key) {
    if (KZ_KEY_IS_INDEX(key)) push_number(fs, (double)KZ_KEY_INDEX(key));
    else push_string(fs, key);
}

static JSValue key_name(FS *fs, JSValue key) {
    /* the function name a literal key gives */
    if (KZ_KEY_IS_INDEX(key)) {
        JSValue s = kz_int_to_string(fs->ctx, KZ_KEY_INDEX(key));
        if (s == JS_EXCEPTION || kz_parser_keep(fs->p, s) == JS_EXCEPTION) { fs_oom(fs); return 0; }
        KZ_ROOT(fs->ctx, rs);
        rs = s;
        JSValue a = kz_atom_from_str(fs->ctx, rs);
        if (a == JS_EXCEPTION || kz_parser_keep(fs->p, a) == JS_EXCEPTION) { fs_oom(fs); return 0; }
        return a;
    }
    return JV_IS_STRING(key) ? key : 0;
}

static void gen_object(FS *fs, Node *n) {
    emit_op(fs, OP_object);
    for (int i = 0; i < n->list.n; i++) {
        Node *pr = n->list.v[i];
        set_pos(fs, pr);
        switch (pr->op) {
        case P_SPREAD:
            gen_expr(fs, pr->a);
            emit_op_u8(fs, OP_copy_data_props, 0);
            adjust(fs, -1);
            break;
        case P_PROTO:
            gen_expr(fs, pr->b);
            emit_op(fs, OP_set_proto);
            break;
        case P_INIT: case P_SHORTHAND:
            if (pr->flags & F_COMPUTED) {
                gen_expr(fs, pr->a);
                emit_op(fs, OP_to_propkey);
                if (is_anon_fn(pr->b)) {
                    gen_expr(fs, pr->b);
                    emit_op_u8(fs, OP_set_name_computed, 0);
                } else gen_expr(fs, pr->b);
                emit_op(fs, OP_define_elem);
            } else {
                JSValue key = literal_key(fs, pr->a);
                if (key_is_atom(key)) {
                    gen_expr_named(fs, pr->b, key_name(fs, key));
                    emit_atom(fs, OP_define_field, key);
                } else {
                    push_key(fs, key);
                    gen_expr_named(fs, pr->b, key_name(fs, key));
                    emit_op(fs, OP_define_elem);
                }
            }
            break;
        case P_METHOD: case P_GET: case P_SET: {
            int kind = (pr->op == P_GET ? DM_GETTER : pr->op == P_SET ? DM_SETTER : DM_METHOD) | DM_ENUM;
            if (pr->flags & F_COMPUTED) {
                gen_expr(fs, pr->a);
                emit_op(fs, OP_to_propkey);
                gen_closure(fs, pr->b, 0);
                emit_op_u8(fs, OP_define_method_elem, (uint8_t)kind);
            } else {
                JSValue key = literal_key(fs, pr->a);
                if (key_is_atom(key)) {
                    gen_closure(fs, pr->b, key_name(fs, key));
                    emit_op(fs, OP_define_method);
                    emit_u32(fs, add_const(fs, key));
                    emit_byte(fs, (uint8_t)kind);
                } else {
                    push_key(fs, key);
                    gen_closure(fs, pr->b, key_name(fs, key));
                    emit_op_u8(fs, OP_define_method_elem, (uint8_t)kind);
                }
            }
            break;
        }
        }
    }
}

static void gen_array(FS *fs, Node *n) {
    bool simple = n->list.n < 1024;
    for (int i = 0; i < n->list.n && simple; i++)
        if (!n->list.v[i] || n->list.v[i]->type == N_SPREAD) simple = false;
    if (simple) {
        for (int i = 0; i < n->list.n; i++) gen_expr(fs, n->list.v[i]);
        emit_op_u16(fs, OP_array_from, (uint16_t)n->list.n);
        adjust(fs, -n->list.n + 1);
        return;
    }
    emit_op_u16(fs, OP_array_from, 0);
    adjust(fs, 1);
    for (int i = 0; i < n->list.n; i++) {
        Node *e = n->list.v[i];
        if (!e) emit_op(fs, OP_array_hole);
        else if (e->type == N_SPREAD) { gen_expr(fs, e->a); emit_op(fs, OP_array_append); }
        else { gen_expr(fs, e); emit_op(fs, OP_array_push); }
    }
}

static void gen_logical(FS *fs, Node *n) {
    gen_expr(fs, n->a);
    int j;
    if (n->op == T_QQ) {
        j = emit_jump(fs, OP_if_not_nullish);
        emit_op(fs, OP_drop);
        gen_expr(fs, n->b);
        patch_here(fs, j);
        return;
    }
    emit_op(fs, OP_dup);
    j = emit_jump(fs, n->op == T_ANDAND ? OP_if_false : OP_if_true);
    emit_op(fs, OP_drop);
    gen_expr(fs, n->b);
    patch_here(fs, j);
}

static void gen_delete(FS *fs, Node *n) {
    Node *t = n->a;
    while (t->type == N_PAREN) t = t->a;
    switch (t->type) {
    case N_IDENT: {
        IntVec found = { 0 };
        int base = fs->depth;
        emit_dyn_prefix(fs, t, OP_with_delete, &found);
        if (!t->binding) emit_atom(fs, OP_delete_global, t->val);
        else emit_op(fs, OP_push_false);        /* declared bindings cannot be deleted */
        patch_dyn(fs, &found, 0);
        fs->depth = base + 1;
        return;
    }
    case N_MEMBER:
        if (t->a->type == N_SUPER) {
            gen_super_base(fs, t->a);
            emit_op(fs, OP_drop);
            emit_op(fs, OP_drop);
            emit_op_u32(fs, OP_throw_ref, add_const(fs, kz_str_from_cstr(fs->ctx, "Unsupported reference to 'super'")));
            emit_op(fs, OP_push_false);
            return;
        }
        gen_expr(fs, t->a);
        if (t->flags & F_COMPUTED) { gen_expr(fs, t->b); emit_op(fs, OP_delete_elem); }
        else emit_atom(fs, OP_delete_field, t->val);
        return;
    case N_OPT_CHAIN: {
        /* delete a?.b: true when the chain short-circuits */
        OptCtx oc = { { 0 }, { 0 } };
        int base = fs->depth;
        Node *m = t->a;
        if (m->type == N_MEMBER) {
            gen_chain(fs, m->a, &oc, base);
            if (m->flags & F_OPTIONAL) opt_check(fs, &oc, base);
            if (m->flags & F_COMPUTED) { gen_expr(fs, m->b); emit_op(fs, OP_delete_elem); }
            else emit_atom(fs, OP_delete_field, m->val);
        } else {
            gen_chain(fs, m, &oc, base);
            emit_op(fs, OP_drop);
            emit_op(fs, OP_push_true);
        }
        int done = emit_jump(fs, OP_goto);
        IntVec to_end = { 0 };
        for (int i = 0; i < oc.jumps.n; i++) {
            patch_here(fs, oc.jumps.v[i]);
            fs->depth = base + oc.drops.v[i];
            for (int k = 0; k < oc.drops.v[i]; k++) emit_op(fs, OP_drop);
            emit_op(fs, OP_push_true);
            iv_push(fs->p, &to_end, emit_jump(fs, OP_goto));
        }
        patch_here(fs, done);
        for (int i = 0; i < to_end.n; i++) patch_here(fs, to_end.v[i]);
        fs->depth = base + 1;
        return;
    }
    default:
        gen_expr(fs, t);
        emit_op(fs, OP_drop);
        emit_op(fs, OP_push_true);
    }
}

static void gen_expr(FS *fs, Node *n) {
    if (fs->failed) return;
    if (kz_check_stack(fs->ctx)) { kz_take_exception(fs->ctx); kz_throw_range(fs->ctx, "Program too deeply nested"); fs_oom(fs); return; }
    JSRuntime *rt = fs->ctx->rt;
    switch (n->type) {
    case N_NUMBER: push_number(fs, n->num); return;
    case N_STRING: push_string(fs, n->val); return;
    case N_BIGINT: {
        JSValue b = kz_bigint_literal(fs->ctx, (const char *)kz_str8(JV_STR(n->val)), kz_str_len(JV_STR(n->val)), n->ival);
        if (b == JS_EXCEPTION) { fs_oom(fs); return; }
        emit_op_u32(fs, OP_push_const, add_const(fs, b));
        return;
    }
    case N_TEMPLATE: gen_template(fs, n); return;
    case N_TAGGED: gen_tagged(fs, n); return;
    case N_REGEXP:
        set_pos(fs, n);
        push_string(fs, n->val);
        push_string(fs, n->val2);
        emit_op(fs, OP_regexp);
        return;
    case N_NULL: emit_op(fs, OP_push_null); return;
    case N_TRUE: emit_op(fs, OP_push_true); return;
    case N_FALSE: emit_op(fs, OP_push_false); return;
    case N_THIS:
        if (n->binding) {
            FuncInfo *g = non_arrow(fs->fi);
            bool check = g && (g->flags & FN_DERIVED);
            emit_load_binding(fs, n->binding, check);
        } else emit_op(fs, OP_push_this);
        return;
    case N_NEW_TARGET:
        if (n->binding) emit_load_binding(fs, n->binding, false);
        else emit_op(fs, OP_push_new_target);
        return;
    case N_IMPORT_META: emit_op(fs, OP_push_import_meta); return;
    case N_IDENT: set_pos(fs, n); gen_ident_load(fs, n, false); return;
    case N_PAREN: gen_expr(fs, n->a); return;
    case N_ARRAY: gen_array(fs, n); return;
    case N_OBJECT: gen_object(fs, n); return;
    case N_FUNC: case N_ARROW: gen_func_expr(fs, n, 0); return;
    case N_CLASS: gen_class(fs, n, 0); return;
    case N_UNARY:
        if (n->op == OP_KW + KW_delete) { set_pos(fs, n); gen_delete(fs, n); return; }
        if (n->op == OP_KW + KW_typeof) {
            Node *t = n->a;
            while (t->type == N_PAREN) t = t->a;
            if (t->type == N_IDENT) gen_ident_load(fs, t, true);
            else gen_expr(fs, n->a);
            emit_op(fs, OP_typeof);
            return;
        }
        gen_expr(fs, n->a);
        set_pos(fs, n);
        switch (n->op) {
        case T_MINUS: emit_op(fs, OP_neg); break;
        case T_PLUS: emit_op(fs, OP_plus); break;
        case T_BANG: emit_op(fs, OP_not); break;
        case T_TILDE: emit_op(fs, OP_bnot); break;
        case OP_KW + KW_void: emit_op(fs, OP_drop); emit_op(fs, OP_push_undefined); break;
        }
        return;
    case N_UPDATE: gen_update(fs, n, true); return;
    case N_BINARY:
        gen_expr(fs, n->a);
        gen_expr(fs, n->b);
        set_pos(fs, n);
        emit_op(fs, binop_for_token(n->op));
        return;
    case N_PRIVATE_IN:
        if (n->binding) emit_load_binding(fs, n->binding, false); else emit_op(fs, OP_push_undefined);
        gen_expr(fs, n->b);
        set_pos(fs, n);
        emit_op(fs, OP_private_in);
        return;
    case N_LOGICAL: gen_logical(fs, n); return;
    case N_ASSIGN: gen_assign(fs, n, true); return;
    case N_COND: {
        gen_expr(fs, n->a);
        int jf = emit_jump(fs, OP_if_false);
        gen_expr(fs, n->b);
        int je = emit_jump(fs, OP_goto);
        adjust(fs, -1);
        patch_here(fs, jf);
        gen_expr(fs, n->c);
        patch_here(fs, je);
        return;
    }
    case N_CALL: gen_call(fs, n); return;
    case N_NEW: {
        gen_expr(fs, n->a);
        set_pos(fs, n);
        int argc = gen_args(fs, &n->list);
        if (argc < 0) emit_op(fs, OP_new_spread);
        else { emit_op_u16(fs, OP_new, (uint16_t)argc); adjust(fs, -(argc + 1) + 1); }
        return;
    }
    case N_MEMBER:
        if (n->a->type == N_SUPER) {
            gen_super_base(fs, n->a);
            set_pos(fs, n);
            if (n->flags & F_COMPUTED) { gen_expr(fs, n->b); emit_op(fs, OP_to_propkey); emit_op(fs, OP_get_elem_recv); }
            else emit_atom(fs, OP_get_field_recv, n->val);
            return;
        }
        gen_expr(fs, n->a);
        set_pos(fs, n);
        if (n->flags & F_COMPUTED) {
            gen_expr(fs, n->b);
            emit_op(fs, OP_get_elem);
        } else if (n->val == rt->atom[KZ_ATOM_length]) emit_op(fs, OP_get_length);
        else emit_atom_ic(fs, OP_get_field, n->val);
        return;
    case N_PRIVATE_MEMBER:
        gen_expr(fs, n->a);
        if (n->binding) emit_load_binding(fs, n->binding, false); else emit_op(fs, OP_push_undefined);
        set_pos(fs, n);
        emit_op(fs, OP_get_private);
        return;
    case N_OPT_CHAIN: gen_opt_chain(fs, n); return;
    case N_SEQ:
        for (int i = 0; i < n->list.n; i++) {
            gen_expr(fs, n->list.v[i]);
            if (i + 1 < n->list.n) emit_op(fs, OP_drop);
        }
        return;
    case N_SPREAD: gen_expr(fs, n->a); return;
    case N_YIELD: {
        FuncInfo *fi = fs->fi;
        set_pos(fs, n);
        if (n->flags & F_DELEGATE) {
            gen_expr(fs, n->a);
            int base = fs->depth - 1;
            if (fi->flags & FN_ASYNC) {
                /* async delegation (ECMA-262 15.5.5, async case), one step per loop:
                     iter next received mode --async_yield_star--> iter next r   (r: what the method returned)
                   then r is awaited and inspected; a return() without a return method jumps out with received */
                int smode = new_slot(fs);
                emit_op(fs, OP_get_async_iterator);      /* iter next */
                emit_op(fs, OP_push_undefined);
                emit_op_u8(fs, OP_push_i8, RESUME_NEXT); /* iter next received mode */
                int loop = (int)fs->len;
                emit_op(fs, OP_dup);
                emit_op_u16(fs, OP_put_loc, (uint16_t)smode);
                int at = emit_op(fs, OP_async_yield_star);
                emit_u32(fs, 0);                          /* iter next r */
                emit_op(fs, OP_await);
                emit_op(fs, OP_check_object);
                emit_op(fs, OP_dup);
                emit_atom_ic(fs, OP_get_field, rt->atom[KZ_ATOM_done]);
                int jdone = emit_jump(fs, OP_if_true);   /* iter next r */
                emit_atom_ic(fs, OP_get_field, rt->atom[KZ_ATOM_value]);
                emit_op(fs, OP_yield);                   /* iter next received mode */
                emit_jump_back(fs, OP_goto, loop);
                /* done: the value is the result of yield*, or (after a return()) the generator's return value */
                patch_here(fs, jdone);
                fs->depth = base + 3;
                emit_atom_ic(fs, OP_get_field, rt->atom[KZ_ATOM_value]);   /* iter next value */
                emit_op(fs, OP_nip);
                emit_op(fs, OP_nip);                     /* value */
                emit_op_u16(fs, OP_get_loc, (uint16_t)smode);
                emit_op_u8(fs, OP_push_i8, RESUME_RETURN);
                emit_op(fs, OP_seq);
                int jnormal = emit_jump(fs, OP_if_false);
                gen_return_value(fs);
                patch_here(fs, jnormal);
                fs->depth = base + 1;
                int jend = emit_jump(fs, OP_goto);
                /* return() and the iterator has no return method: return received (awaited) */
                put_u32_at(fs, (uint32_t)at + 1, (uint32_t)((int)fs->len - at));
                fs->depth = base + 3;                    /* iter next received */
                emit_op(fs, OP_nip);
                emit_op(fs, OP_nip);
                emit_op(fs, OP_await);
                gen_return_value(fs);
                patch_here(fs, jend);
                fs->depth = base + 1;
                return;
            }
            emit_op(fs, OP_get_iterator);
            emit_op(fs, OP_push_undefined);
            emit_op_u8(fs, OP_push_i8, RESUME_NEXT);
            int at = emit_op(fs, OP_yield_star);
            emit_u32(fs, 0);
            emit_u32(fs, 0);
            /* the op never falls through: first the return path (a return completion here), then the done path */
            put_u32_at(fs, (uint32_t)at + 5, (uint32_t)((int)fs->len - at));
            gen_return_value(fs);
            put_u32_at(fs, (uint32_t)at + 1, (uint32_t)((int)fs->len - at));
            fs->depth = base + 1;
            return;
        }
        if (n->a) gen_expr(fs, n->a); else emit_op(fs, OP_push_undefined);
        if (fi->flags & FN_ASYNC) emit_op(fs, OP_await);
        emit_op(fs, OP_yield);
        int at = emit_jump(fs, OP_resume_dispatch);
        int jn = emit_jump(fs, OP_goto);
        patch_here(fs, at);
        if (fi->flags & FN_ASYNC) emit_op(fs, OP_await);    /* return(v) in an async generator awaits v */
        gen_return_value(fs);
        fs->depth++;
        patch_here(fs, jn);
        return;
    }
    case N_AWAIT:
        gen_expr(fs, n->a);
        set_pos(fs, n);
        emit_op(fs, OP_await);
        return;
    case N_IMPORT_CALL:
        gen_expr(fs, n->a);
        if (n->b) gen_expr(fs, n->b); else emit_op(fs, OP_push_undefined);
        set_pos(fs, n);
        emit_op(fs, OP_import_call);
        return;
    default:
        kz_throw_syntax(fs->ctx, "unsupported expression (node %d)", n->type);
        fs_oom(fs);
        return;
    }
}

/* evaluates n for its effects only */
static void gen_expr_discard(FS *fs, Node *n) {
    if (n->type == N_ASSIGN) { gen_assign(fs, n, false); return; }
    if (n->type == N_UPDATE) { gen_update(fs, n, false); return; }
    gen_expr(fs, n);
    emit_op(fs, OP_drop);
}

/* ================================================================== destructuring */

/* value on the stack -> consumed, stored into the pattern's targets.  decl: 0 assignment, V_VAR/V_LET/V_CONST,
   BK_PARAM or BK_CATCH-style initialisation */
static void gen_target_store(FS *fs, Node *t, int decl, bool have_ref, Ref *ref) {
    while (t->type == N_PAREN) t = t->a;
    if (t->type == N_OBJ_PATTERN || t->type == N_ARR_PATTERN) { gen_destructure(fs, t, decl); return; }
    if (decl) {
        if (t->type == N_IDENT) { gen_binding_init(fs, t, decl); return; }
    }
    if (have_ref) { gen_ref_store(fs, ref, false); return; }
    if (t->type == N_IDENT) { gen_ident_store(fs, t, false); return; }
    Ref r = gen_ref(fs, t);
    /* value parts -> parts value */
    int parts = ref_parts(&r);
    if (parts == 1) emit_op(fs, OP_swap);
    else if (parts == 2) emit_op(fs, OP_rot3l);
    else if (parts == 3) emit_op(fs, OP_rot4l);
    gen_ref_store(fs, &r, false);
}

/* For assignment patterns whose target is a member expression, the reference is evaluated before the value is
   fetched (ECMA-262 13.15.5.6). */
static void gen_destructure(FS *fs, Node *pat, int decl) {
    JSRuntime *rt = fs->ctx->rt;
    if (pat->type == N_IDENT) { gen_target_store(fs, pat, decl, false, NULL); return; }
    if (pat->type == N_ASSIGN_PAT) {
        int j = emit_jump(fs, OP_if_not_undefined);
        emit_op(fs, OP_drop);
        gen_expr_named(fs, pat->b, ref_name(pat->a));
        patch_here(fs, j);
        gen_destructure(fs, pat->a, decl);
        return;
    }
    if (pat->type == N_OBJ_PATTERN) {
        /* the source waits in a slot; computed keys too (the rest element excludes every key used) */
        emit_op(fs, OP_check_coercible);
        int ssrc = new_slot(fs);
        emit_op_u16(fs, OP_put_loc, (uint16_t)ssrc);
        int nprops = pat->list.n;
        int *keyslot = kz_arena_alloc(fs->p, (size_t)(nprops + 1) * sizeof(int));
        if (!keyslot) { fs_oom(fs); return; }
        for (int i = 0; i < nprops; i++) {
            Node *pp = pat->list.v[i];
            keyslot[i] = -1;
            if (pp->type == N_REST) {
                emit_op(fs, OP_object);
                emit_op_u16(fs, OP_get_loc, (uint16_t)ssrc);
                int nexcl = 0;
                for (int k = 0; k < i && nexcl < 255; k++, nexcl++) {
                    Node *q = pat->list.v[k];
                    if (keyslot[k] >= 0) emit_op_u16(fs, OP_get_loc, (uint16_t)keyslot[k]);
                    else push_key(fs, literal_key(fs, q->a));
                }
                emit_op_u8(fs, OP_copy_data_props, (uint8_t)nexcl);
                adjust(fs, -1 - nexcl);
                gen_target_store(fs, pp->a, decl, false, NULL);
                break;
            }
            Node *t = pp->b, *target = t, *def = NULL;
            if (t->type == N_ASSIGN_PAT) { target = t->a; def = t->b; }
            while (target->type == N_PAREN) target = target->a;
            bool member_target = !decl && (target->type == N_MEMBER || target->type == N_PRIVATE_MEMBER);
            JSValue key = 0;
            if (pp->flags & F_COMPUTED) {
                gen_expr(fs, pp->a);
                emit_op(fs, OP_to_propkey);
                keyslot[i] = new_slot(fs);
                emit_op_u16(fs, OP_put_loc, (uint16_t)keyslot[i]);
            } else key = literal_key(fs, pp->a);
            Ref r = { REF_VAR, target };
            if (member_target) r = gen_ref(fs, target);     /* the target before the value */
            emit_op_u16(fs, OP_get_loc, (uint16_t)ssrc);
            if (keyslot[i] >= 0) { emit_op_u16(fs, OP_get_loc, (uint16_t)keyslot[i]); emit_op(fs, OP_get_elem); }
            else if (key_is_atom(key)) emit_atom_ic(fs, OP_get_field, key);
            else { push_key(fs, key); emit_op(fs, OP_get_elem); }
            if (def) {
                int j = emit_jump(fs, OP_if_not_undefined);
                emit_op(fs, OP_drop);
                gen_expr_named(fs, def, ref_name(target));
                patch_here(fs, j);
            }
            if (member_target) gen_ref_store(fs, &r, false);
            else gen_target_store(fs, target, decl, false, NULL);
        }
        return;
    }
    if (pat->type == N_ARR_PATTERN) {
        emit_op(fs, OP_get_iterator);
        emit_op(fs, OP_push_done);                      /* iter next done */
        int base = fs->depth - 3;
        uint32_t start = fs->len;
        CB cb = { 0 };
        cb.kind = CB_ITERDESTR;
        cb.prev = fs->cb;
        cb.depth = base;
        cb.items = 3;
        fs->cb = &cb;
        for (int i = 0; i < pat->list.n; i++) {
            Node *e = pat->list.v[i];
            if (!e) {
                emit_op(fs, OP_iter_step);
                emit_op(fs, OP_drop);
                continue;
            }
            if (e->type == N_REST) {
                Node *target = e->a;
                while (target->type == N_PAREN) target = target->a;
                if (!decl && (target->type == N_MEMBER || target->type == N_PRIVATE_MEMBER)) {
                    Ref r = gen_ref(fs, target);
                    int parts = ref_parts(&r);
                    /* iter next done parts -> keep parts above: compute rest, then store */
                    int slots[3];
                    for (int k = parts - 1; k >= 0; k--) { slots[k] = new_slot(fs); emit_op_u16(fs, OP_put_loc, (uint16_t)slots[k]); }
                    emit_op(fs, OP_iter_rest);
                    int sv = new_slot(fs);
                    emit_op_u16(fs, OP_put_loc, (uint16_t)sv);
                    for (int k = 0; k < parts; k++) emit_op_u16(fs, OP_get_loc, (uint16_t)slots[k]);
                    emit_op_u16(fs, OP_get_loc, (uint16_t)sv);
                    gen_ref_store(fs, &r, false);
                } else {
                    emit_op(fs, OP_iter_rest);
                    gen_target_store(fs, target, decl, false, NULL);
                }
                continue;
            }
            Node *target = e, *def = NULL;
            if (e->type == N_ASSIGN_PAT) { target = e->a; def = e->b; }
            while (target->type == N_PAREN) target = target->a;
            if (!decl && (target->type == N_MEMBER || target->type == N_PRIVATE_MEMBER)) {
                Ref r = gen_ref(fs, target);
                int parts = ref_parts(&r);
                int slots[3];
                for (int k = parts - 1; k >= 0; k--) { slots[k] = new_slot(fs); emit_op_u16(fs, OP_put_loc, (uint16_t)slots[k]); }
                emit_op(fs, OP_iter_step);
                if (def) {
                    int j = emit_jump(fs, OP_if_not_undefined);
                    emit_op(fs, OP_drop);
                    gen_expr(fs, def);
                    patch_here(fs, j);
                }
                int sv = new_slot(fs);
                emit_op_u16(fs, OP_put_loc, (uint16_t)sv);
                for (int k = 0; k < parts; k++) emit_op_u16(fs, OP_get_loc, (uint16_t)slots[k]);
                emit_op_u16(fs, OP_get_loc, (uint16_t)sv);
                gen_ref_store(fs, &r, false);
                continue;
            }
            emit_op(fs, OP_iter_step);
            if (def) {
                int j = emit_jump(fs, OP_if_not_undefined);
                emit_op(fs, OP_drop);
                gen_expr_named(fs, def, ref_name(target));
                patch_here(fs, j);
            }
            gen_target_store(fs, target, decl, false, NULL);
        }
        fs->cb = cb.prev;
        uint32_t end = fs->len;
        emit_op(fs, OP_iter_finish);
        int jok = emit_jump(fs, OP_goto);
        /* an exception while destructuring closes the iterator (unless it is done) */
        uint32_t handler = fs->len;
        fs->depth = base + 3 + 1;
        emit_op(fs, OP_iter_finish_throw);
        add_exc(fs, start, end, handler, base + 3);
        patch_here(fs, jok);
        fs->depth = base;
        (void)rt;
        return;
    }
    gen_target_store(fs, pat, decl, false, NULL);
}

/* ================================================================== statements: control blocks */

static CB *push_cb(FS *fs, CB *cb, int kind) {
    memset(cb, 0, sizeof *cb);
    cb->kind = kind;
    cb->prev = fs->cb;
    cb->depth = fs->depth;
    fs->cb = cb;
    return cb;
}

static void pop_cb(FS *fs) { fs->cb = fs->cb->prev; }

static void drop_to(FS *fs, int depth) {
    while (fs->depth > depth) emit_op(fs, OP_drop);
}

/* the cleanup an exit through block cb needs (its own stack items) */
static void exit_block_items(FS *fs, CB *cb) {
    drop_to(fs, cb->depth + cb->items);
    if (cb->kind == CB_FOROF) {
        if (cb->async_iter) {
            /* for await: return() is awaited */
            emit_op(fs, OP_iter_close);
        } else emit_op(fs, OP_iter_close);
    } else if (cb->kind == CB_FORIN) emit_op(fs, OP_drop);
    else if (cb->kind == CB_ITERDESTR) emit_op(fs, OP_iter_finish);
    fs->depth = cb->depth;
}

static int add_cont(FS *fs, CB *fin, int kind, CB *target, bool is_continue) {
    if (fin->nconts == fin->conts_cap) {
        int nc = fin->conts_cap ? fin->conts_cap * 2 : 4;
        Cont *n = kz_arena_alloc(fs->p, (size_t)nc * sizeof(Cont));
        if (!n) { fs_oom(fs); return 0; }
        if (fin->nconts) memcpy(n, fin->conts, (size_t)fin->nconts * sizeof(Cont));
        fin->conts = n;
        fin->conts_cap = nc;
    }
    for (int i = 0; i < fin->nconts; i++)
        if (fin->conts[i].kind == kind && fin->conts[i].target == target && fin->conts[i].is_continue == is_continue) return i;
    fin->conts[fin->nconts].kind = kind;
    fin->conts[fin->nconts].target = target;
    fin->conts[fin->nconts].is_continue = is_continue;
    return fin->nconts++;
}

/* jumps to target's break (or continue) point, cleaning up and passing through finally blocks on the way */
static void gen_jump_to(FS *fs, CB *target, bool is_continue) {
    for (CB *cb = fs->cb; cb != target; cb = cb->prev) {
        if (cb->kind == CB_FINALLY) {
            drop_to(fs, cb->depth);
            int k = add_cont(fs, cb, 0, target, is_continue);
            emit_op(fs, OP_push_undefined);
            emit_op_u32(fs, OP_push_i32, (uint32_t)(k + 2));
            iv_push(fs->p, &cb->fin_gotos, emit_jump(fs, OP_goto));
            fs->depth = cb->depth;
            return;
        }
        exit_block_items(fs, cb);
    }
    if (is_continue) {
        drop_to(fs, target->depth + target->items);
        iv_push(fs->p, &target->continues, emit_jump(fs, OP_goto));
    } else {
        exit_block_items(fs, target);
        iv_push(fs->p, &target->breaks, emit_jump(fs, OP_goto));
    }
}

/* the value on top is returned, through every enclosing finally */
static void gen_return_value(FS *fs) {
    bool cleanup = false;
    for (CB *cb = fs->cb; cb; cb = cb->prev)
        if (cb->kind == CB_FINALLY || cb->kind == CB_FOROF || cb->kind == CB_ITERDESTR) { cleanup = true; break; }
    FuncInfo *fi = fs->fi;
    if (!cleanup) {
        if (fi->flags & FN_DERIVED) {
            /* value this -> what a derived constructor returns (checked) */
            if (fi->this_b) emit_load_binding(fs, fi->this_b, false); else emit_op(fs, OP_push_this);
            emit_op(fs, OP_check_ctor_return);
        }
        emit_op(fs, OP_return);
        return;
    }
    if (fs->ret_temp < 0) fs->ret_temp = new_slot(fs);
    emit_op_u16(fs, OP_put_loc, (uint16_t)fs->ret_temp);
    for (CB *cb = fs->cb; cb; cb = cb->prev) {
        if (cb->kind == CB_FINALLY) {
            drop_to(fs, cb->depth);
            int k = add_cont(fs, cb, 1, NULL, false);
            emit_op_u16(fs, OP_get_loc, (uint16_t)fs->ret_temp);
            emit_op_u32(fs, OP_push_i32, (uint32_t)(k + 2));
            iv_push(fs->p, &cb->fin_gotos, emit_jump(fs, OP_goto));
            fs->depth = cb->depth;
            return;
        }
        exit_block_items(fs, cb);
    }
    emit_op_u16(fs, OP_get_loc, (uint16_t)fs->ret_temp);
    if (fi->flags & FN_DERIVED) {
        if (fi->this_b) emit_load_binding(fs, fi->this_b, false); else emit_op(fs, OP_push_this);
        emit_op(fs, OP_check_ctor_return);
    }
    emit_op(fs, OP_return);
}

static CB *find_label(FS *fs, JSValue label, bool is_continue) {
    for (CB *cb = fs->cb; cb; cb = cb->prev) {
        if (label) {
            for (int i = 0; i < cb->nlabels; i++)
                if (cb->labels[i] == label) {
                    if (is_continue && !cb->continuable) continue;
                    return cb;
                }
        } else if (is_continue ? cb->continuable : cb->breakable) return cb;
    }
    return NULL;
}

/* labels collected by consecutive labelled statements, handed to the next breakable statement */
static JSValue g_pending_labels_unused;

/* ================================================================== statements */

static void set_completion_undefined(FS *fs) {
    if (fs->completion >= 0) {
        emit_op(fs, OP_push_undefined);
        emit_op_u16(fs, OP_put_loc, (uint16_t)fs->completion);
    }
}

static void gen_var_decl(FS *fs, Node *n) {
    for (int i = 0; i < n->list.n; i++) {
        Node *d = n->list.v[i];
        set_pos(fs, d);
        if (!d->b) {
            /* let x; initialises to undefined, var x; does nothing */
            if (n->op != V_VAR && d->a->type == N_IDENT) {
                emit_op(fs, OP_push_undefined);
                gen_binding_init(fs, d->a, n->op);
            }
            continue;
        }
        if (d->a->type == N_IDENT) {
            gen_expr_named(fs, d->b, d->a->val);
            gen_binding_init(fs, d->a, n->op);
        } else {
            gen_expr(fs, d->b);
            gen_destructure(fs, d->a, n->op);
        }
    }
}

/* hoisting at scope entry: cells for captured bindings, TDZ markers, function declarations */
static void enter_scope(FS *fs, Scope *s) {
    if (!s || fs->failed) return;
    if (global_scope(s)) return;
    for (Binding *b = s->first; b; b = b->next) {
        if (b->slot < 0) continue;
        if (b->kind == BK_PARAM || b->kind == BK_ARGUMENTS || b->kind == BK_IMPORT) continue;
        if (b->kind == BK_HIDDEN && (b == fs->fi->this_b || b == fs->fi->new_target_b || b == fs->fi->home_b || b == fs->fi->fields_b)) continue;
        bool lexical = is_lexical_kind_x(b->kind);
        if (b->flags & BF_CAPTURED) emit_op_u16(fs, lexical ? OP_new_lcell : OP_new_lcell_undef, (uint16_t)b->slot);
        else if (lexical) emit_op_u16(fs, OP_set_loc_uninit, (uint16_t)b->slot);
    }
    /* function declarations of this scope (later ones win) */
    for (Binding *b = s->first; b; b = b->next) {
        if (b->kind != BK_FUNC || !b->decl || b->decl->type != N_FUNC_DECL || b->slot < 0) continue;
        /* the last declaration with this name */
        Node *decl = b->decl;
        gen_closure(fs, decl, 0);
        emit_store_binding(fs, b, false, false, true);
    }
}

static void gen_block(FS *fs, Node *n) {
    enter_scope(fs, n->scope);
    gen_stmts(fs, &n->list);
}

static void gen_if(FS *fs, Node *n) {
    set_completion_undefined(fs);
    gen_expr(fs, n->a);
    int jf = emit_jump(fs, OP_if_false);
    gen_stmt(fs, n->b);
    if (n->c) {
        int je = emit_jump(fs, OP_goto);
        patch_here(fs, jf);
        gen_stmt(fs, n->c);
        patch_here(fs, je);
    } else patch_here(fs, jf);
}

/* copies every captured per-iteration binding of a for(let ...) head into fresh cells */
static void per_iteration_copy(FS *fs, Scope *s) {
    for (Binding *b = s->first; b; b = b->next)
        if ((b->flags & BF_CAPTURED) && b->slot >= 0 && is_lexical_kind_x(b->kind)) emit_op_u16(fs, OP_copy_lcell, (uint16_t)b->slot);
}


static void gen_loop_common_end(FS *fs, CB *cb, int continue_target, int break_target) {
    for (int i = 0; i < cb->continues.n; i++) patch_to(fs, cb->continues.v[i], continue_target);
    for (int i = 0; i < cb->breaks.n; i++) patch_to(fs, cb->breaks.v[i], break_target);
}

static JSValue *g_label_buf;
static int g_label_n;

static void gen_for(FS *fs, Node *n, JSValue *labels, int nlabels) {
    set_completion_undefined(fs);
    enter_scope(fs, n->scope);
    if (n->a) {
        if (n->a->type == N_VAR) gen_var_decl(fs, n->a);
        else gen_expr_discard(fs, n->a);
    }
    bool per_iter = n->a && n->a->type == N_VAR && n->a->op != V_VAR;
    if (per_iter) per_iteration_copy(fs, n->scope);
    CB cb;
    push_cb(fs, &cb, CB_LOOP);
    cb.breakable = cb.continuable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int top = (int)fs->len;
    int jexit = -1;
    if (n->b) {
        gen_expr(fs, n->b);
        jexit = emit_jump(fs, OP_if_false);
    }
    gen_stmt(fs, n->d);
    int cont = (int)fs->len;
    if (per_iter) per_iteration_copy(fs, n->scope);
    if (n->c) gen_expr_discard(fs, n->c);
    emit_jump_back(fs, OP_goto, top);
    if (jexit >= 0) patch_here(fs, jexit);
    pop_cb(fs);
    gen_loop_common_end(fs, &cb, cont, (int)fs->len);
}

static void gen_while(FS *fs, Node *n, JSValue *labels, int nlabels) {
    set_completion_undefined(fs);
    CB cb;
    push_cb(fs, &cb, CB_LOOP);
    cb.breakable = cb.continuable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int top = (int)fs->len;
    gen_expr(fs, n->a);
    int jexit = emit_jump(fs, OP_if_false);
    gen_stmt(fs, n->d);
    emit_jump_back(fs, OP_goto, top);
    patch_here(fs, jexit);
    pop_cb(fs);
    gen_loop_common_end(fs, &cb, top, (int)fs->len);
}

static void gen_do_while(FS *fs, Node *n, JSValue *labels, int nlabels) {
    set_completion_undefined(fs);
    CB cb;
    push_cb(fs, &cb, CB_LOOP);
    cb.breakable = cb.continuable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int top = (int)fs->len;
    gen_stmt(fs, n->d);
    int cont = (int)fs->len;
    gen_expr(fs, n->a);
    emit_jump_back(fs, OP_if_true, top);
    pop_cb(fs);
    gen_loop_common_end(fs, &cb, cont, (int)fs->len);
}

/* the target of a for-in/of iteration: a declaration, or an assignment target */
static void gen_for_target(FS *fs, Node *left, Scope *s) {
    if (left->type == N_VAR) {
        Node *d = left->list.v[0];
        if (left->op != V_VAR) {
            /* a fresh binding per iteration */
            for (Binding *b = s->first; b; b = b->next)
                if ((b->flags & BF_CAPTURED) && b->slot >= 0 && is_lexical_kind_x(b->kind)) emit_op_u16(fs, OP_new_lcell, (uint16_t)b->slot);
        }
        if (d->a->type == N_IDENT) gen_binding_init(fs, d->a, left->op);
        else gen_destructure(fs, d->a, left->op);
        return;
    }
    gen_target_store(fs, left, 0, false, NULL);
}

static void gen_for_in(FS *fs, Node *n, JSValue *labels, int nlabels) {
    set_completion_undefined(fs);
    enter_scope(fs, n->scope);
    Node *left = n->a;
    /* for (var x = init in o): Annex B initialiser */
    if (left->type == N_VAR && left->list.v[0]->b) {
        Node *d = left->list.v[0];
        gen_expr_named(fs, d->b, d->a->val);
        gen_binding_init(fs, d->a, V_VAR);
    }
    gen_expr(fs, n->b);
    emit_op(fs, OP_for_in_start);
    CB cb;
    push_cb(fs, &cb, CB_FORIN);
    cb.depth = fs->depth - 1;
    cb.items = 1;
    cb.breakable = cb.continuable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int top = (int)fs->len;
    int jdone = emit_jump(fs, OP_for_in_next);   /* iter key */
    gen_for_target(fs, left, n->scope);
    gen_stmt(fs, n->d);
    emit_jump_back(fs, OP_goto, top);
    patch_here(fs, jdone);
    pop_cb(fs);
    for (int i = 0; i < cb.continues.n; i++) patch_to(fs, cb.continues.v[i], top);
    fs->depth = cb.depth + 1;
    emit_op(fs, OP_drop);
    /* breaks already dropped the iterator */
    for (int i = 0; i < cb.breaks.n; i++) patch_to(fs, cb.breaks.v[i], (int)fs->len);
}

static void gen_for_of(FS *fs, Node *n, JSValue *labels, int nlabels) {
    JSRuntime *rt = fs->ctx->rt;
    set_completion_undefined(fs);
    enter_scope(fs, n->scope);
    bool async = n->flags & F_AWAIT;
    gen_expr(fs, n->b);
    emit_op(fs, async ? OP_for_await_start : OP_for_of_start);
    CB cb;
    push_cb(fs, &cb, CB_FOROF);
    cb.depth = fs->depth - 2;
    cb.items = 2;
    cb.async_iter = async;
    cb.breakable = cb.continuable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int top = (int)fs->len;
    int jdone;
    if (async) {
        emit_op(fs, OP_iter_call_next);         /* iter next result */
        emit_op(fs, OP_await);
        jdone = emit_jump(fs, OP_iter_result);  /* iter next value */
    } else jdone = emit_jump(fs, OP_for_of_next);   /* iter next value */
    uint32_t body_start = fs->len;
    gen_for_target(fs, n->a, n->scope);
    gen_stmt(fs, n->d);
    uint32_t body_end = fs->len;
    emit_jump_back(fs, OP_goto, top);
    /* exceptions in the body close the iterator */
    uint32_t handler = fs->len;
    fs->depth = cb.depth + 3;
    if (async) {
        /* iter next exc: call return and await it, then rethrow */
        emit_op(fs, OP_iter_close_throw);
    } else emit_op(fs, OP_iter_close_throw);
    add_exc(fs, body_start, body_end, handler, cb.depth + 2);
    fs->depth = cb.depth + 2;
    patch_here(fs, jdone);
    if (async) fs->depth = cb.depth + 2;
    pop_cb(fs);
    /* done: the iterator is finished; drop its record */
    for (int i = 0; i < cb.continues.n; i++) patch_to(fs, cb.continues.v[i], top);
    emit_op(fs, OP_drop);
    emit_op(fs, OP_drop);
    for (int i = 0; i < cb.breaks.n; i++) patch_to(fs, cb.breaks.v[i], (int)fs->len);
    fs->depth = cb.depth;
    (void)rt;
}

static void gen_switch(FS *fs, Node *n, JSValue *labels, int nlabels) {
    set_completion_undefined(fs);
    gen_expr(fs, n->a);
    int disc = new_slot(fs);
    emit_op_u16(fs, OP_put_loc, (uint16_t)disc);
    enter_scope(fs, n->scope);
    CB cb;
    push_cb(fs, &cb, CB_SWITCH);
    cb.breakable = true;
    for (int i = 0; i < nlabels && i < 8; i++) cb.labels[cb.nlabels++] = labels[i];
    int ncase = n->list.n;
    int *jumps = kz_arena_alloc(fs->p, (size_t)(ncase + 1) * sizeof(int));
    if (!jumps) { fs_oom(fs); return; }
    int default_idx = -1;
    for (int i = 0; i < ncase; i++) {
        Node *c = n->list.v[i];
        if (!c->a) { default_idx = i; continue; }
        emit_op_u16(fs, OP_get_loc, (uint16_t)disc);
        gen_expr(fs, c->a);
        emit_op(fs, OP_seq);
        jumps[i] = emit_jump(fs, OP_if_true);
    }
    int jdefault = emit_jump(fs, OP_goto);
    for (int i = 0; i < ncase; i++) {
        Node *c = n->list.v[i];
        if (c->a) patch_here(fs, jumps[i]);
        else patch_here(fs, jdefault);
        gen_stmts(fs, &c->list);
    }
    if (default_idx < 0) patch_here(fs, jdefault);
    pop_cb(fs);
    for (int i = 0; i < cb.breaks.n; i++) patch_to(fs, cb.breaks.v[i], (int)fs->len);
}

static void gen_try(FS *fs, Node *n) {
    set_completion_undefined(fs);
    int base = fs->depth;
    CB fin;
    bool has_finally = n->d != NULL;
    if (has_finally) {
        push_cb(fs, &fin, CB_FINALLY);
        fin.depth = base;
    }
    uint32_t try_start = fs->len;
    gen_block(fs, n->a);
    uint32_t try_end = fs->len;
    int jover = -1;
    if (has_finally) {
        emit_op(fs, OP_push_undefined);
        emit_op_u8(fs, OP_push_i8, COMPLETION_NORMAL);
        iv_push(fs->p, &fin.fin_gotos, emit_jump(fs, OP_goto));
        fs->depth = base;
    } else jover = emit_jump(fs, OP_goto);
    uint32_t catch_end = try_end;
    if (n->c) {
        uint32_t handler = fs->len;
        add_exc(fs, try_start, try_end, handler, base);
        fs->depth = base + 1;
        enter_scope(fs, n->scope);
        if (n->b) {
            if (n->b->type == N_IDENT) {
                Binding *b = n->b->binding;
                if (!b) b = kz_scope_find(n->scope, n->b->val);
                if (b) emit_store_binding(fs, b, false, false, true);
                else emit_op(fs, OP_drop);
            } else gen_destructure(fs, n->b, V_LET);
        } else emit_op(fs, OP_drop);
        gen_stmts(fs, &n->c->list);
        catch_end = fs->len;
        if (has_finally) {
            emit_op(fs, OP_push_undefined);
            emit_op_u8(fs, OP_push_i8, COMPLETION_NORMAL);
            iv_push(fs->p, &fin.fin_gotos, emit_jump(fs, OP_goto));
            fs->depth = base;
        }
    }
    if (!has_finally) {
        patch_here(fs, jover);
        return;
    }
    /* the throw path into finally: exc -> exc THROW */
    uint32_t handler = fs->len;
    add_exc(fs, try_start, catch_end, handler, base);
    fs->depth = base + 1;
    emit_op_u8(fs, OP_push_i8, COMPLETION_THROW);
    pop_cb(fs);
    /* the finally block */
    int fin_entry = (int)fs->len;
    for (int i = 0; i < fin.fin_gotos.n; i++) patch_to(fs, fin.fin_gotos.v[i], fin_entry);
    fs->depth = base + 2;
    /* while the finally block runs, the completion record sits on the stack: blocks inside see a deeper base */
    gen_block(fs, n->d);
    int at = emit_op(fs, OP_finally_end);
    emit_u16(fs, (uint16_t)fin.nconts);
    uint32_t table = fs->len;
    for (int i = 0; i < fin.nconts; i++) emit_u32(fs, 0);
    fs->depth = base;
    int jend = emit_jump(fs, OP_goto);
    /* the continuations: the value is on the stack */
    for (int i = 0; i < fin.nconts; i++) {
        put_u32_at(fs, table + 4 * (uint32_t)i, (uint32_t)((int)fs->len - at));
        fs->depth = base + 1;
        Cont *c = &fin.conts[i];
        if (c->kind == 1) gen_return_value(fs);
        else {
            emit_op(fs, OP_drop);
            gen_jump_to(fs, c->target, c->is_continue);
        }
    }
    patch_here(fs, jend);
    fs->depth = base;
}

static void gen_with(FS *fs, Node *n) {
    set_completion_undefined(fs);
    gen_expr(fs, n->a);
    emit_op(fs, OP_with_obj);
    enter_scope(fs, n->scope);
    Binding *w = n->scope->with_obj;
    if (w->flags & BF_CAPTURED) emit_op_u16(fs, OP_init_lcell, (uint16_t)w->slot);
    else emit_op_u16(fs, OP_put_loc, (uint16_t)w->slot);
    gen_stmt(fs, n->b);
}

static void gen_labelled(FS *fs, Node *n) {
    /* collect consecutive labels */
    JSValue labels[8];
    int nl = 0;
    Node *s = n;
    while (s->type == N_LABELED) {
        if (nl < 8) labels[nl++] = s->val;
        s = s->a;
    }
    switch (s->type) {
    case N_FOR: gen_for(fs, s, labels, nl); return;
    case N_WHILE: gen_while(fs, s, labels, nl); return;
    case N_DO_WHILE: gen_do_while(fs, s, labels, nl); return;
    case N_FOR_IN: gen_for_in(fs, s, labels, nl); return;
    case N_FOR_OF: gen_for_of(fs, s, labels, nl); return;
    case N_SWITCH: gen_switch(fs, s, labels, nl); return;
    default: break;
    }
    CB cb;
    push_cb(fs, &cb, CB_LABEL);
    for (int i = 0; i < nl; i++) cb.labels[cb.nlabels++] = labels[i];
    gen_stmt(fs, s);
    pop_cb(fs);
    for (int i = 0; i < cb.breaks.n; i++) patch_to(fs, cb.breaks.v[i], (int)fs->len);
}

static void gen_class_decl(FS *fs, Node *n) {
    Node *cls = n->a;
    gen_class(fs, cls, 0);
    /* the outer binding */
    Binding *b = NULL;
    for (Scope *s = n->scope; s; s = s->parent) {
        b = kz_scope_find(s, cls->val);
        if (b) break;
    }
    if (b && !global_scope(b->scope)) emit_store_binding(fs, b, false, false, true);
    else emit_atom_ic(fs, OP_put_global_init, cls->val);
}

static void gen_stmt(FS *fs, Node *n) {
    if (!n || fs->failed) return;
    if (kz_check_stack(fs->ctx)) { kz_take_exception(fs->ctx); kz_throw_range(fs->ctx, "Program too deeply nested"); fs_oom(fs); return; }
    set_pos(fs, n);
    int depth = fs->depth;
    switch (n->type) {
    case N_VAR: gen_var_decl(fs, n); break;
    case N_FUNC_DECL:
        /* Annex B: copy a block function into its var binding when the declaration is evaluated */
        if (n->binding) {
            Binding *blk = NULL;
            for (Scope *s = n->scope; s; s = s->parent) { blk = kz_scope_find(s, n->val); if (blk) break; }
            if (blk && blk != n->binding) {
                emit_load_binding(fs, blk, false);
                if (global_scope(n->binding->scope)) emit_atom_ic(fs, OP_put_global, n->val);
                else emit_store_binding(fs, n->binding, false, false, false);
            }
        }
        break;
    case N_CLASS_DECL: gen_class_decl(fs, n); break;
    case N_EXPR_STMT:
        if (fs->completion >= 0) {
            gen_expr(fs, n->a);
            emit_op_u16(fs, OP_put_loc, (uint16_t)fs->completion);
        } else gen_expr_discard(fs, n->a);
        break;
    case N_BLOCK: gen_block(fs, n); break;
    case N_EMPTY: case N_DEBUGGER: break;
    case N_IF: gen_if(fs, n); break;
    case N_FOR: gen_for(fs, n, NULL, 0); break;
    case N_FOR_IN: gen_for_in(fs, n, NULL, 0); break;
    case N_FOR_OF: gen_for_of(fs, n, NULL, 0); break;
    case N_WHILE: gen_while(fs, n, NULL, 0); break;
    case N_DO_WHILE: gen_do_while(fs, n, NULL, 0); break;
    case N_CONTINUE: case N_BREAK: {
        bool is_continue = n->type == N_CONTINUE;
        CB *target = find_label(fs, n->val, is_continue);
        if (!target) { kz_throw_syntax(fs->ctx, "Illegal %s statement", is_continue ? "continue" : "break"); fs_oom(fs); return; }
        gen_jump_to(fs, target, is_continue);
        fs->depth = depth;
        break;
    }
    case N_RETURN:
        if (n->a) gen_expr_named(fs, n->a, (fs->fi->flags & FN_FIELD_INIT) ? fs->fi->field_name : 0);
        else emit_op(fs, OP_push_undefined);
        if ((fs->fi->flags & FN_ASYNC) && (fs->fi->flags & FN_GENERATOR)) emit_op(fs, OP_await);
        gen_return_value(fs);
        fs->depth = depth;
        break;
    case N_WITH: gen_with(fs, n); break;
    case N_SWITCH: gen_switch(fs, n, NULL, 0); break;
    case N_LABELED: gen_labelled(fs, n); break;
    case N_THROW:
        gen_expr(fs, n->a);
        set_pos(fs, n);
        emit_op(fs, OP_throw);
        fs->depth = depth;
        break;
    case N_TRY: gen_try(fs, n); break;
    case N_IMPORT: break;
    case N_EXPORT:
        if (n->op == EX_DECL || n->op == EX_DEFAULT_DECL) {
            if (n->a->type == N_VAR) gen_var_decl(fs, n->a);
            else if (n->a->type == N_CLASS_DECL) gen_class_decl(fs, n->a);
        } else if (n->op == EX_DEFAULT_EXPR) {
            gen_expr_named(fs, n->a, fs->ctx->rt->atom[KZ_ATOM_default]);
            Binding *b = kz_scope_find(fs->fi->scope, fs->ctx->rt->atom[KZ_ATOM_star_default_star]);
            if (b) emit_store_binding(fs, b, false, false, true); else emit_op(fs, OP_drop);
        }
        break;
    default:
        kz_throw_syntax(fs->ctx, "unsupported statement (node %d)", n->type);
        fs_oom(fs);
        return;
    }
    if (!fs->failed && fs->depth != depth) {
        /* a statement must leave the stack as it found it */
        while (fs->depth > depth) emit_op(fs, OP_drop);
        fs->depth = depth;
    }
}

static void gen_stmts(FS *fs, NodeList *l) {
    for (int i = 0; i < l->n && !fs->failed; i++) gen_stmt(fs, l->v[i]);
}

/* ================================================================== classes */

static FuncInfo *synth_ctor(FS *fs, Node *cls, bool derived) {
    /* the implicit constructor: constructor() {} or constructor(...args) { super(...args); } */
    FuncInfo *fi = kz_arena_alloc(fs->p, sizeof(FuncInfo));
    if (!fi) { fs_oom(fs); return NULL; }
    fi->flags = FN_CTOR | FN_STRICT | FN_SIMPLE | (derived ? FN_DERIVED : 0);
    fi->parent = fs->fi;
    fi->class_node = cls;
    Scope *s = kz_arena_alloc(fs->p, sizeof(Scope));
    if (!s) { fs_oom(fs); return NULL; }
    s->kind = SC_FUNC;
    s->parent = cls->scope;
    s->func = fi;
    fi->scope = fi->body_scope = s;
    fi->scopes = s;
    fi->src_start = cls->pos;
    fi->src_end = cls->end;
    fi->line = cls->line;
    fi->col = cls->col;
    /* the hidden this binding (derived: TDZ until super() returns) */
    if (derived) {
        Binding *tb = kz_declare_hidden(fs->p, s, fs->ctx->rt->atom[KZ_ATOM_lex_this]);
        if (!tb) { fs_oom(fs); return NULL; }
        fi->this_b = tb;
    }
    fi->ival_synth = derived ? 2 : 1;
    return fi;
}

static bool class_has_instance_elements(Node *cls) {
    for (int i = 0; i < cls->list.n; i++) {
        Node *el = cls->list.v[i];
        if (el->flags & F_STATIC) continue;
        if (el->op == CE_FIELD || (el->flags & F_PRIVATE_KEY)) return true;
    }
    return false;
}

/* closure for a class element function with its home object: pushes the function */
static void gen_class_fn(FS *fs, Node *fn, JSValue name, int home_slot) {
    emit_op_u16(fs, OP_get_loc, (uint16_t)home_slot);
    gen_closure(fs, fn, name);
    emit_op(fs, OP_set_home);                   /* home fn */
    emit_op(fs, OP_nip);                        /* fn */
}

/* ClassDefinitionEvaluation (ECMA-262 15.7.14) */
static void gen_class(FS *fs, Node *n, JSValue name_hint) {
    JSRuntime *rt = fs->ctx->rt;
    JSValue name = n->val != JS_UNDEFINED ? n->val : name_hint;
    Scope *cs = n->scope;
    /* the class scope: the inner name (TDZ) and the private names */
    for (Binding *b = cs->first; b; b = b->next) {
        if (b->slot < 0) b->slot = (int16_t)new_slot(fs);
        if (b->kind == BK_CONST) {
            if (b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell, (uint16_t)b->slot);
            else emit_op_u16(fs, OP_set_loc_uninit, (uint16_t)b->slot);
        } else if (b->kind == BK_HIDDEN) {
            if (b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)b->slot);
            emit_atom(fs, OP_private_name, b->name);
            emit_store_binding(fs, b, false, false, true);
        }
    }
    Node *ctor_el = NULL;
    for (int i = 0; i < n->list.n; i++)
        if (n->list.v[i]->op == CE_METHOD && n->list.v[i]->ival == 1) ctor_el = n->list.v[i];
    bool derived = n->a != NULL;
    FuncInfo *ctor_fi = ctor_el ? ctor_el->b->func : synth_ctor(fs, n, derived);
    if (!ctor_fi) return;
    ctor_fi->has_instance_elements = class_has_instance_elements(n);
    if (n->a) gen_expr(fs, n->a);
    else emit_op(fs, OP_push_undefined);
    JSFunctionBytecode *cb = gen_function(fs, ctor_fi, name ? name : rt->atom[KZ_ATOM_empty]);
    if (!cb) { fs_oom(fs); return; }
    set_pos(fs, n);
    emit_op(fs, OP_class_create);
    emit_u32(fs, add_const(fs, JV_MKPTR(cb)));
    emit_byte(fs, (uint8_t)(derived ? 1 : 0));
    int sc = new_slot(fs), sp = new_slot(fs);
    emit_op_u16(fs, OP_put_loc, (uint16_t)sp);
    emit_op_u16(fs, OP_put_loc, (uint16_t)sc);
    IntVec statics = { 0 };
    for (int i = 0; i < n->list.n; i++) {
        Node *el = n->list.v[i];
        if (el == ctor_el) continue;
        bool is_static = el->flags & F_STATIC;
        int target = is_static ? sc : sp;
        set_pos(fs, el);
        if (el->op == CE_STATIC_BLOCK || (el->op == CE_FIELD && is_static)) {
            iv_push(fs->p, &statics, i);
            if (el->op == CE_FIELD && (el->flags & F_COMPUTED)) {
                /* computed keys are evaluated now, in element order */
                gen_expr(fs, el->a);
                emit_op(fs, OP_to_propkey);
                el->ival = new_slot(fs) + 2;
                emit_op_u16(fs, OP_put_loc, (uint16_t)(el->ival - 2));
            }
            continue;
        }
        JSValue key = 0;
        if (!(el->flags & (F_COMPUTED | F_PRIVATE_KEY))) key = literal_key(fs, el->a);
        if (el->op == CE_FIELD) {
            /* an instance field: a record (key, initialiser) on the constructor */
            emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
            if (el->flags & F_PRIVATE_KEY) emit_load_binding(fs, kz_scope_find(cs, el->a->val2), false);
            else if (el->flags & F_COMPUTED) { gen_expr(fs, el->a); emit_op(fs, OP_to_propkey); }
            else push_key(fs, key);
            if (el->b) {
                el->b->func->field_name = (el->flags & F_COMPUTED) ? 0 : (el->flags & F_PRIVATE_KEY) ? el->a->val2 : key_name(fs, key);
                gen_class_fn(fs, el->b, 0, sp);
            } else emit_op(fs, OP_push_undefined);
            emit_op_u8(fs, OP_define_method_elem, (uint8_t)(DM_FIELD | ((el->flags & F_PRIVATE_KEY) ? DM_PRIVATE : 0) | FR_FIELD));
            emit_op(fs, OP_drop);
            continue;
        }
        int kind = el->op == CE_GET ? DM_GETTER : el->op == CE_SET ? DM_SETTER : DM_METHOD;
        if (el->flags & F_PRIVATE_KEY) {
            Binding *pb = kz_scope_find(cs, el->a->val2);
            if (is_static) {
                emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
                emit_load_binding(fs, pb, false);
                gen_class_fn(fs, el->b, el->a->val2, sc);
                emit_op_u8(fs, OP_define_private_method, (uint8_t)kind);
                emit_op(fs, OP_drop);
            } else {
                /* instance private methods are installed on each new instance: a record on the constructor */
                int fr = kind == DM_GETTER ? FR_PRIVATE_GETTER : kind == DM_SETTER ? FR_PRIVATE_SETTER : FR_PRIVATE_METHOD;
                emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
                emit_load_binding(fs, pb, false);
                gen_class_fn(fs, el->b, el->a->val2, sp);
                emit_op_u8(fs, OP_define_method_elem, (uint8_t)(DM_FIELD | DM_PRIVATE | fr));
                emit_op(fs, OP_drop);
            }
            continue;
        }
        emit_op_u16(fs, OP_get_loc, (uint16_t)target);
        if (el->flags & F_COMPUTED) {
            gen_expr(fs, el->a);
            emit_op(fs, OP_to_propkey);
            gen_closure(fs, el->b, 0);
            emit_op_u8(fs, OP_define_method_elem, (uint8_t)kind);
        } else if (key_is_atom(key)) {
            gen_closure(fs, el->b, key_name(fs, key));
            emit_op(fs, OP_define_method);
            emit_u32(fs, add_const(fs, key));
            emit_byte(fs, (uint8_t)kind);
        } else {
            push_key(fs, key);
            gen_closure(fs, el->b, key_name(fs, key));
            emit_op_u8(fs, OP_define_method_elem, (uint8_t)kind);
        }
        emit_op(fs, OP_drop);
    }
    /* the inner binding gets the class; static elements see it */
    Binding *inner = n->val != JS_UNDEFINED ? kz_scope_find(cs, n->val) : NULL;
    if (inner && inner->kind == BK_CONST) {
        emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
        emit_store_binding(fs, inner, false, false, true);
    }
    for (int i = 0; i < statics.n; i++) {
        Node *el = n->list.v[statics.v[i]];
        set_pos(fs, el);
        if (el->op == CE_STATIC_BLOCK) {
            gen_class_fn(fs, el->b, 0, sc);         /* fn */
            emit_op_u16(fs, OP_get_loc, (uint16_t)sc);  /* fn this */
            emit_op_u16(fs, OP_call, 0);
            adjust(fs, -2 + 1);
            emit_op(fs, OP_drop);
            continue;
        }
        /* a static field: DefineField on the constructor, the initialiser called with this = the constructor */
        emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
        JSValue key = 0;
        if (el->flags & F_PRIVATE_KEY) emit_load_binding(fs, kz_scope_find(cs, el->a->val2), false);
        else if (el->flags & F_COMPUTED) emit_op_u16(fs, OP_get_loc, (uint16_t)(el->ival - 2));
        else { key = literal_key(fs, el->a); push_key(fs, key); }
        if (el->b) {
            el->b->func->field_name = (el->flags & F_COMPUTED) ? 0 : (el->flags & F_PRIVATE_KEY) ? el->a->val2 : key_name(fs, key);
            gen_class_fn(fs, el->b, 0, sc);
            emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
            emit_op_u16(fs, OP_call, 0);
            adjust(fs, -2 + 1);
        } else emit_op(fs, OP_push_undefined);
        if (el->flags & F_PRIVATE_KEY) emit_op(fs, OP_define_private);
        else emit_op(fs, OP_define_elem);
        emit_op(fs, OP_drop);
    }
    emit_op_u16(fs, OP_get_loc, (uint16_t)sc);
}

/* ================================================================== functions */

static void gen_param_inits(FS *fs, FuncInfo *fi) {
    /* non-simple parameter lists: arguments sit in slots 0..n-1; bind them pattern by pattern */
    for (int i = 0; i < fi->params.n; i++) {
        Node *prm = fi->params.v[i];
        set_pos(fs, prm);
        if (prm->type == N_REST) {
            emit_op_u16(fs, OP_rest, (uint16_t)i);
            gen_destructure(fs, prm->a, BK_PARAM + 100);
            continue;
        }
        emit_op_u16(fs, OP_get_loc, (uint16_t)i);
        gen_destructure(fs, prm, BK_PARAM + 100);
    }
}

static void assign_slots(FS *fs, FuncInfo *fi) {
    int n = 0;
    bool simple = fi->flags & FN_SIMPLE;
    if (simple) {
        for (int i = 0; i < fi->params.n; i++) {
            Node *prm = fi->params.v[i];
            Binding *b = kz_scope_find(fi->scope, prm->val);
            if (b) b->slot = (int16_t)i;       /* duplicates: the last one wins */
        }
    }
    n = fi->params.n;
    for (Scope *s = fi->scopes; s; s = s->next_in_func) {
        if (global_scope(s)) {
            for (Binding *b = s->first; b; b = b->next) if (b->kind == BK_HIDDEN && b->slot < 0) b->slot = (int16_t)n++;
            continue;
        }
        for (Binding *b = s->first; b; b = b->next) {
            if (b->slot >= 0) continue;
            if (b->kind == BK_IMPORT || global_binding(s, b)) continue;
            b->slot = (int16_t)n++;
        }
    }
    fs->nslots = n;
}

static uint16_t bc_kind(FuncInfo *fi) {
    if (fi->flags & FN_ARROW) return KZ_FUNC_ARROW;
    if (fi->flags & FN_DERIVED) return KZ_FUNC_DERIVED_CTOR;
    if (fi->flags & FN_CTOR) return KZ_FUNC_CLASS_CTOR;
    if (fi->flags & FN_GETTER) return KZ_FUNC_GETTER;
    if (fi->flags & FN_SETTER) return KZ_FUNC_SETTER;
    if (fi->flags & FN_FIELD_INIT) return KZ_FUNC_FIELD_INIT;
    if (fi->flags & FN_STATIC_BLOCK) return KZ_FUNC_STATIC_INIT;
    if (fi->flags & FN_METHOD) return KZ_FUNC_METHOD;
    if (fi->flags & FN_SCRIPT) return KZ_FUNC_SCRIPT;
    if (fi->flags & FN_EVAL) return KZ_FUNC_EVAL;
    if (fi->flags & FN_MODULE) return KZ_FUNC_MODULE;
    return KZ_FUNC_NORMAL;
}

/* The prologue: FunctionDeclarationInstantiation in instructions. */
static void gen_prologue(FS *fs, FuncInfo *fi) {
    JSRuntime *rt = fs->ctx->rt;
    bool simple = fi->flags & FN_SIMPLE;
    /* parameters that inner functions capture move into cells */
    if (simple)
        for (int i = 0; i < fi->params.n; i++) {
            Binding *b = kz_scope_find(fi->scope, fi->params.v[i]->val);
            if (b && (b->flags & BF_CAPTURED) && b->slot == i) emit_op_u16(fs, OP_box_loc, (uint16_t)i);
        }
    /* hidden bindings */
    if (fi->this_b) {
        if (fi->flags & FN_DERIVED) {
            if (fi->this_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell, (uint16_t)fi->this_b->slot);
            else emit_op_u16(fs, OP_set_loc_uninit, (uint16_t)fi->this_b->slot);
        } else {
            if (fi->this_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)fi->this_b->slot);
            emit_op(fs, OP_push_this);
            emit_store_binding(fs, fi->this_b, false, false, true);
        }
    }
    if (fi->new_target_b) {
        if (fi->new_target_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)fi->new_target_b->slot);
        emit_op(fs, OP_push_new_target);
        emit_store_binding(fs, fi->new_target_b, false, false, true);
    }
    if (fi->home_b) {
        if (fi->home_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)fi->home_b->slot);
        emit_op(fs, OP_push_home);
        emit_store_binding(fs, fi->home_b, false, false, true);
    }
    if (fi->fields_b) {
        if (fi->fields_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)fi->fields_b->slot);
        emit_op(fs, OP_push_func);
        emit_store_binding(fs, fi->fields_b, false, false, true);
    }
    /* the arguments object */
    if (fi->arguments_b) {
        bool mapped = !(fi->flags & FN_STRICT) && simple;
        if (fi->arguments_b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)fi->arguments_b->slot);
        emit_op_u8(fs, OP_special_object, mapped ? SO_MAPPED_ARGUMENTS : SO_ARGUMENTS);
        emit_store_binding(fs, fi->arguments_b, false, false, true);
    }
    /* base class constructors initialise fields before the body */
    if ((fi->flags & FN_CTOR) && !(fi->flags & FN_DERIVED) && fi->has_instance_elements) {
        emit_op(fs, OP_push_func);
        emit_op(fs, OP_push_this);
        emit_op(fs, OP_run_fields);
        adjust(fs, -2);
    }
    if (!simple) {
        /* parameters are in their TDZ until bound */
        for (Binding *b = fi->scope->first; b; b = b->next) {
            if (b->kind != BK_PARAM) continue;
            if (b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell, (uint16_t)b->slot);
            else emit_op_u16(fs, OP_set_loc_uninit, (uint16_t)b->slot);
        }
        gen_param_inits(fs, fi);
    }
    /* vars and functions of the body */
    Scope *vs = fi->body_scope;
    if (vs && vs != fi->scope) {
        /* a separate var scope: vars named like parameters start with the parameter's value */
        for (Binding *b = vs->first; b; b = b->next) {
            if (b->kind != BK_VAR && b->kind != BK_FUNC) continue;
            Binding *pb = kz_scope_find(fi->scope, b->name);
            if (b->flags & BF_CAPTURED) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)b->slot);
            if (pb && b->kind == BK_VAR) {
                emit_load_binding(fs, pb, false);
                emit_store_binding(fs, b, false, false, true);
            }
        }
        /* lexical bindings and functions */
        for (Binding *b = vs->first; b; b = b->next) {
            if (b->kind == BK_VAR || b->kind == BK_FUNC) continue;
            if (b->kind == BK_HIDDEN && b == vs->eval_obj) continue;
            bool lexical = is_lexical_kind_x(b->kind);
            if (b->flags & BF_CAPTURED) emit_op_u16(fs, lexical ? OP_new_lcell : OP_new_lcell_undef, (uint16_t)b->slot);
            else if (lexical) emit_op_u16(fs, OP_set_loc_uninit, (uint16_t)b->slot);
        }
        for (Binding *b = vs->first; b; b = b->next) {
            if (b->kind != BK_FUNC || !b->decl || b->decl->type != N_FUNC_DECL) continue;
            gen_closure(fs, b->decl, 0);
            emit_store_binding(fs, b, false, false, true);
        }
    } else enter_scope(fs, fi->scope);
    if (vs && vs->eval_obj && (vs->eval_obj->flags & BF_CAPTURED)) emit_op_u16(fs, OP_new_lcell_undef, (uint16_t)vs->eval_obj->slot);
    if (fi->flags & FN_GENERATOR) emit_op(fs, OP_initial_yield);
    (void)rt;
}

static JSFunctionBytecode *finish_bytecode(FS *fs, FuncInfo *fi, JSValue name) {
    JSContext *ctx = fs->ctx;
    if (fs->failed) return NULL;
    KZ_ROOT(ctx, rb);
    JSFunctionBytecode *b = kz_gc_new(ctx, KZ_GC_BYTECODE, sizeof(JSFunctionBytecode));
    if (!b) return NULL;
    rb = JV_MKPTR(b);
    b->h.aux = bc_kind(fi);
    uint16_t fl = 0;
    if (fi->flags & FN_STRICT) fl |= BC_STRICT;
    if (fi->flags & FN_GENERATOR) fl |= BC_GENERATOR;
    if (fi->flags & FN_ASYNC) fl |= BC_ASYNC;
    if (fi->arguments_b) fl |= BC_ARGUMENTS;
    if (fi->has_direct_eval) fl |= BC_HAS_EVAL;
    if (fi->flags & FN_SIMPLE) fl |= BC_SIMPLE_PARAMS;
    if (fi->uses_super_prop || fi->home_b) fl |= BC_NEEDS_HOME;
    if (!(fi->flags & (FN_ARROW | FN_METHOD | FN_GETTER | FN_SETTER | FN_GENERATOR | FN_ASYNC | FN_SCRIPT | FN_EVAL | FN_MODULE |
                       FN_FIELD_INIT | FN_STATIC_BLOCK)))
        fl |= BC_CONSTRUCTOR;
    if (fi->flags & FN_CTOR) fl |= BC_CONSTRUCTOR;
    if (fi->params.n && fi->params.v[fi->params.n - 1]->type == N_REST) fl |= BC_REST;
    b->h.flags = fl;
    /* hand over the buffers (they become the bytecode's, freed with it) */
    b->code = fs->code;
    b->code_len = fs->len;
    fs->code = NULL;
    b->nconsts = fs->consts->len;
    if (b->nconsts) {
        b->consts = kz_malloc(ctx, b->nconsts * sizeof(JSValue));
        if (!b->consts) { b->nconsts = 0; return NULL; }
        memcpy(b->consts, fs->consts->v, b->nconsts * sizeof(JSValue));
    }
    b->ncvars = (uint16_t)fi->ncaptured;
    if (fi->ncaptured) {
        b->cvars = kz_malloc(ctx, (size_t)fi->ncaptured * sizeof(KzClosureVar));
        if (!b->cvars) { b->ncvars = 0; return NULL; }
        for (CapturedVar *c = fi->captured; c; c = c->next) {
            KzClosureVar *cv = &b->cvars[c->index];
            FuncInfo *parent = fi->parent;
            if (c->b->scope->func == parent) { cv->from_local = 1; cv->index = (uint16_t)c->b->slot; }
            else { cv->from_local = 0; cv->index = (uint16_t)(parent ? captured_index(parent, c->b) : 0); }
            cv->pad = 0;
        }
    }
    b->length = (uint16_t)fi->nparams_len;
    b->nparams = (uint16_t)fi->params.n;
    b->nlocals = (uint16_t)fs->nslots;
    b->stack_size = (uint16_t)(fs->max_depth + 2);
    b->exc = fs->exc;
    b->nexc = fs->nexc;
    fs->exc = NULL;
    b->pos = fs->pos;
    b->npos = fs->npos;
    fs->pos = NULL;
    b->nic = fs->nic;
    if (fs->nic) {
        b->ic = kz_mallocz(ctx, fs->nic * sizeof(KzIC));
        if (!b->ic) { b->nic = 0; return NULL; }
    }
    b->varnames = fs->vn;
    b->nvarnames = (uint16_t)fs->nvn;
    fs->vn = NULL;
    b->name = name ? name : ctx->rt->atom[KZ_ATOM_empty];
    b->filename = fs->p->filename;
    b->src_start = fi->src_start;
    b->src_end = fi->src_end;
    b->line = fi->line;
    b->col = fi->col;
    b->realm = ctx;
    b->source = fs->p->source ? fs->p->source : JS_UNDEFINED;
    return b;
}

static JSFunctionBytecode *gen_function(FS *parent, FuncInfo *fi, JSValue name_hint) {
    JSContext *ctx = parent ? parent->ctx : NULL;
    Parser *p = parent->p;
    ctx = p->ctx;
    KZ_ROOT_VEC(ctx, consts);
    FS fs;
    memset(&fs, 0, sizeof fs);
    fs.p = p;
    fs.ctx = ctx;
    fs.fi = fi;
    fs.parent = parent;
    fs.consts = &consts;
    fs.completion = -1;
    fs.ret_temp = -1;
    assign_slots(&fs, fi);
    gen_prologue(&fs, fi);
    if (fi->ival_synth == 2) {
        /* the default derived constructor: super(...arguments), without the iterator protocol */
        emit_op(&fs, OP_push_func);
        emit_op(&fs, OP_push_new_target);
        emit_op_u16(&fs, OP_rest, 0);
        emit_op(&fs, OP_super_call_spread);
        emit_op(&fs, OP_dup);
        gen_init_this(&fs, fi->this_b);
        if (fi->has_instance_elements) {
            emit_op(&fs, OP_push_func);
            emit_op(&fs, OP_dup_under);
            emit_op(&fs, OP_swap);
            emit_op(&fs, OP_run_fields);
        }
        emit_op(&fs, OP_drop);
    }
    gen_stmts(&fs, &fi->body);
    /* falling off the end */
    if (!fs.failed) {
        if (fi->flags & FN_EXPR_BODY) { }
        emit_op(&fs, OP_push_undefined);
        if (fi->flags & FN_DERIVED) {
            if (fi->this_b) emit_load_binding(&fs, fi->this_b, false); else emit_op(&fs, OP_push_this);
            emit_op(&fs, OP_check_ctor_return);
        }
        emit_op(&fs, OP_return);
    }
    JSValue name = fi->name ? fi->name : name_hint;
    if ((fi->flags & FN_GETTER) || (fi->flags & FN_SETTER)) name = name_hint;
    JSFunctionBytecode *b = finish_bytecode(&fs, fi, name);
    if (fs.code) kz_free(ctx->rt, fs.code);
    if (fs.exc) kz_free(ctx->rt, fs.exc);
    if (fs.pos) kz_free(ctx->rt, fs.pos);
    if (fs.vn) kz_free(ctx->rt, fs.vn);
    if (!b) { parent->failed = true; p->failed = true; }
    return b;
}

/* ================================================================== programs */

/* a script's (or global eval code's) declarations for OP_global_decls: [-1, eval?, kind, name, ...] */
static JSValue global_decl_list(FS *fs, FuncInfo *fi, bool eval_code) {
    JSContext *ctx = fs->ctx;
    KZ_ROOT(ctx, arr);
    arr = kz_new_array(ctx, 0);
    if (arr == JS_EXCEPTION) return arr;
    if (kz_array_push(ctx, JV_OBJ(arr), JV_MKINT(-1)) || kz_array_push(ctx, JV_OBJ(arr), JV_MKBOOL(eval_code))) return JS_EXCEPTION;
    Scope *s = fi->scope;
    for (Binding *b = s->first; b; b = b->next) {
        int k;
        switch (b->kind) {
        case BK_VAR: k = GD_VAR; break;
        case BK_FUNC: k = GD_FUNC; break;
        case BK_LET: k = GD_LET; break;
        case BK_CONST: k = GD_CONST; break;
        case BK_CLASS: k = GD_CLASS; break;
        default: continue;
        }
        if (eval_code && k != GD_VAR && k != GD_FUNC) continue;     /* eval's lexical names stay local */
        if (kz_array_push(ctx, JV_OBJ(arr), JV_MKINT(k)) || kz_array_push(ctx, JV_OBJ(arr), b->name)) return JS_EXCEPTION;
    }
    return arr;
}

/* sloppy direct eval code: its var and function names that belong to the caller, bound before the body */
static void gen_eval_decls(FS *fs, FuncInfo *fi) {
    Scope *es = fi->scope;
    for (int i = 0; i < fi->eval_vars.n; i++) {
        Node *n = fi->eval_vars.v[i];
        if (n->a) gen_closure(fs, n->a, 0);
        else emit_op(fs, OP_push_hole);
        if (n->binding) {
            /* the caller already has a var of that name: functions overwrite it, vars change nothing */
            if (n->a) emit_assign_binding(fs, n->binding, false, NULL);
            else emit_op(fs, OP_drop);
            continue;
        }
        int xi = es->eval_obj ? captured_index(fi, es->eval_obj) : -1;
        if (xi < 0) { emit_op(fs, OP_drop); continue; }
        emit_op_u16(fs, OP_get_cvar_raw, (uint16_t)xi);
        emit_op(fs, OP_swap);
        emit_atom(fs, OP_eval_decl, n->val);
    }
}

JSFunctionBytecode *kz_compile_program(Parser *p, Node *prog) {
    JSContext *ctx = p->ctx;
    FuncInfo *fi = prog->func;
    Analyzer a = { p, ctx, false };
    analyze_function(&a, fi);
    /* direct eval: resolve the names moved out of the eval scope (to the caller's bindings or its var object) */
    for (int i = 0; i < fi->eval_vars.n && !a.failed; i++) {
        Node *n = fi->eval_vars.v[i];
        n->binding = resolve(&a, fi, fi->scope, n->val, NULL);
        if (fi->scope->eval_obj) capture(&a, fi->scope->eval_obj, fi);
    }
    if (a.failed || p->failed) return NULL;
    KZ_ROOT_VEC(ctx, consts);
    FS fs;
    memset(&fs, 0, sizeof fs);
    fs.p = p;
    fs.ctx = ctx;
    fs.fi = fi;
    fs.consts = &consts;
    fs.ret_temp = -1;
    fs.completion = -1;
    fs.global_code = (fi->flags & FN_SCRIPT) != 0;
    fs.eval_code = (fi->flags & FN_EVAL) != 0;
    assign_slots(&fs, fi);
    if (fi->flags & (FN_SCRIPT | FN_EVAL)) {
        fs.completion = new_slot(&fs);
    }
    if (fi->flags & (FN_SCRIPT | FN_GLOBAL_VARS)) {
        JSValue list = global_decl_list(&fs, fi, !(fi->flags & FN_SCRIPT));
        if (list == JS_EXCEPTION) return NULL;
        emit_op_u32(&fs, OP_global_decls, add_const(&fs, list));
        /* hidden bindings (this for arrows) */
        if (fi->this_b) {
            if (fi->this_b->flags & BF_CAPTURED) emit_op_u16(&fs, OP_new_lcell_undef, (uint16_t)fi->this_b->slot);
            emit_op(&fs, OP_push_this);
            emit_store_binding(&fs, fi->this_b, false, false, true);
        }
        /* eval code's lexical bindings are frame slots like a block's */
        if (!(fi->flags & FN_SCRIPT)) enter_scope(&fs, fi->scope);
        /* function declarations become global properties */
        for (Binding *b = fi->scope->first; b; b = b->next) {
            if (b->kind != BK_FUNC || !b->decl || b->decl->type != N_FUNC_DECL) continue;
            gen_closure(&fs, b->decl, 0);
            emit_atom_ic(&fs, OP_put_global, b->name);
        }
    } else {
        gen_prologue(&fs, fi);
        if (fi->eval_vars.n) gen_eval_decls(&fs, fi);
    }
    gen_stmts(&fs, &prog->list);
    if (fs.failed) {
        if (fs.code) kz_free(ctx->rt, fs.code);
        if (fs.exc) kz_free(ctx->rt, fs.exc);
        if (fs.pos) kz_free(ctx->rt, fs.pos);
        if (fs.vn) kz_free(ctx->rt, fs.vn);
        return NULL;
    }
    if (fs.completion >= 0) emit_op_u16(&fs, OP_get_loc, (uint16_t)fs.completion);
    else emit_op(&fs, OP_push_undefined);
    emit_op(&fs, OP_return);
    JSFunctionBytecode *b = finish_bytecode(&fs, fi, ctx->rt->atom[KZ_ATOM_empty]);
    if (fs.code) kz_free(ctx->rt, fs.code);
    if (fs.exc) kz_free(ctx->rt, fs.exc);
    if (fs.pos) kz_free(ctx->rt, fs.pos);
    if (fs.vn) kz_free(ctx->rt, fs.vn);
    (void)g_pending_labels_unused;
    (void)g_label_buf;
    (void)g_label_n;
    return b;
}
