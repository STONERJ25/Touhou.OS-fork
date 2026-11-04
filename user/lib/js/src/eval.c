/*
 * eval.c: turning source text into running code: scripts (ECMA-262 16.1 ScriptEvaluation), indirect eval (global
 * code with configurable vars), direct eval (19.2.1.1 PerformEval with the caller's environment), and the text of
 * the Function constructor (20.2.1.1.1 CreateDynamicFunction).
 *
 * Direct eval: the compiler made every binding visible at a direct eval call site live in a var cell and recorded
 * the site's names in a descriptor (KzVarName: name, kind, where the cell is).  Here the descriptor becomes a chain
 * of synthetic scopes standing for the caller, the eval code is parsed and compiled inside it (its references to
 * the caller's names become closure variables), and the closure's cells are taken from the caller's frame.
 * Sloppy eval code's var declarations go to the caller's existing var binding of that name, or else into the
 * caller's "eval var object" (created on demand; the caller's own name lookups consult it), or onto the global
 * object when the caller is global code.
 */
#include "parser.h"
#include "bytecode.h"

static KzSource *make_source(JSContext *ctx, const char *src, size_t len) {
    KzSource *s = kz_gc_new(ctx, KZ_GC_SOURCE, sizeof(KzSource) + len + 1);
    if (!s) return NULL;
    s->len = (uint32_t)len;
    memcpy(s->text, src, len);
    s->text[len] = 0;
    return s;
}

static Scope *synth_scope(Parser *p, int kind, Scope *parent, FuncInfo *fi) {
    Scope *s = kz_arena_alloc(p, sizeof(Scope));
    if (!s) return NULL;
    s->kind = (uint8_t)kind;
    s->parent = parent;
    s->func = fi;
    s->next_in_func = fi->scopes;
    fi->scopes = s;
    return s;
}

static Binding *synth_binding(Parser *p, Scope *s, JSValue name, int kind, int slot) {
    Binding *b = kz_arena_alloc(p, sizeof(Binding));
    if (!b) return NULL;
    b->name = name;
    b->kind = (uint8_t)kind;
    b->scope = s;
    b->slot = (int16_t)slot;
    b->flags = BF_CAPTURED;
    b->export_index = -1;
    /* (prepended: the entries are made outermost first and kz_scope_find takes the first match, so of two bindings of one name the
       innermost wins) */
    b->next = s->first;
    s->first = b;
    if (!s->last) s->last = b;
    return b;
}

/* Builds the scope chain of a direct eval's caller from its descriptor (entries innermost first). */
static FuncInfo *build_caller(Parser *p, const KzVarName *vn, uint16_t count, bool caller_strict) {
    FuncInfo *fi = kz_arena_alloc(p, sizeof(FuncInfo));
    if (!fi) return NULL;
    fi->flags = FN_SYNTH_CALLER | (caller_strict ? FN_STRICT : 0);
    /* scopes are made outermost first so each one's parent exists; the descriptor is innermost first */
    Scope *cur = NULL;
    Scope **chain = kz_arena_alloc(p, sizeof(Scope *) * ((size_t)count + 1));
    if (!chain) return NULL;
    int nchain = 0;
    cur = synth_scope(p, SC_BLOCK, NULL, fi);
    if (!cur) return NULL;
    chain[nchain++] = cur;
    for (int i = (int)count - 1; i >= 0; i--) {
        const KzVarName *e = &vn[i];
        if (e->kind == VK_FIELD_INIT) { fi->flags |= FN_FIELD_INIT; continue; }       /* (a marker: no binding) */
        if (e->kind == VK_EVAL_OBJ) {
            /* the object a function's sloppy eval vars live in: a scope of its own, so that names are looked up in it after the
               bindings of the function (the entries after this one, made into the scopes inside it) and before those outside */
            Scope *m = synth_scope(p, SC_BLOCK, cur, fi);
            if (!m) return NULL;
            Binding *mb = synth_binding(p, m, e->name, BK_HIDDEN, i);
            if (!mb) return NULL;
            if (e->outer) mb->flags |= BF_OUTER;
            if (e->paramenv) mb->flags |= BF_PARAMENV;
            m->eval_obj = mb;
            cur = synth_scope(p, SC_BLOCK, m, fi);
            if (!cur) return NULL;
            continue;
        }
        if (e->kind == VK_WITH) {
            Scope *w = synth_scope(p, SC_WITH, cur, fi);
            if (!w) return NULL;
            Binding *wb = synth_binding(p, w, JV_MKINT(0), BK_HIDDEN, i);
            if (!wb) return NULL;
            w->with_obj = wb;
            cur = synth_scope(p, SC_BLOCK, w, fi);
            if (!cur) return NULL;
            continue;
        }
        int bk;
        switch (e->kind) {
        case VK_LET: bk = BK_LET; break;
        case VK_CONST: case VK_CLASS_INNER: bk = BK_CONST; break;
        case VK_CALLEE: bk = BK_CALLEE; break;
        case VK_ARGUMENTS: bk = BK_ARGUMENTS; break;
        case VK_PARAM: bk = BK_PARAM; break;
        case VK_FUNC: bk = BK_FUNC; break;
        case VK_THIS: case VK_NEW_TARGET: case VK_HOME: case VK_EVAL_OBJ: case VK_PRIVATE: case VK_FUNCOBJ: bk = BK_HIDDEN; break;
        default: bk = BK_VAR; break;
        }
        /* an inner name shadows an outer one: the first (innermost) entry wins, so later scopes are outer */
        Binding *b = synth_binding(p, cur, e->name, bk, i);
        if (!b) return NULL;
        if (e->outer) b->flags |= BF_OUTER;
        if (e->paramenv) b->flags |= BF_PARAMENV;
        if (bk == BK_LET || bk == BK_CONST) b->flags |= BF_TDZ;
        /* (this loop runs from the outermost entry inwards: the innermost this, new.target and home object win) */
        if (e->kind == VK_THIS) fi->this_b = b;
        if (e->kind == VK_NEW_TARGET) fi->new_target_b = b;
        if (e->kind == VK_HOME) fi->home_b = b;
        if (e->kind == VK_FUNCOBJ) {
            /* the caller is a derived constructor (or an arrow in one): super() may be called, `this` can be
               uninitialised, and the fields are run after the call (run_fields does nothing when there are none) */
            fi->fields_b = b;
            fi->has_instance_elements = true;
            fi->flags |= FN_DERIVED;
        }
    }
    (void)chain;
    fi->scope = fi->body_scope = cur;
    p->scope = cur;
    p->fn = fi;
    return fi;
}

/* the binding the innermost descriptor entry of `name` stands for (NULL: none) */
static const KzVarName *find_entry(const KzVarName *vn, uint16_t count, JSValue name, int *index) {
    for (uint16_t i = 0; i < count; i++)
        if (vn[i].name == name && vn[i].kind != VK_WITH && vn[i].kind != VK_EVAL_OBJ && !vn[i].outer) { *index = i; return &vn[i]; }
    return NULL;
}

/* compile a parsed program; NULL with the exception pending */
static JSFunctionBytecode *compile(Parser *p, Node *prog) {
    JSFunctionBytecode *b = kz_compile_program(p, prog);
    if (!b && !kz_has_exception(p->ctx)) kz_throw_syntax(p->ctx, "compilation failed");
    return b;
}

JSValue kz_eval_script(JSContext *ctx, const char *src, size_t len, const char *filename, int flags, JSFrame *caller, int site) {
    (void)caller;
    (void)site;
    JSRuntime *rt = ctx->rt;
    KZ_ROOT_VEC(ctx, values);
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_atom(ctx, filename ? filename : "<input>");
    if (r[0] == JS_EXCEPTION) return JS_EXCEPTION;
    KzSource *s = make_source(ctx, src, len);
    if (!s) return JS_EXCEPTION;
    r[1] = JV_MKPTR(s);
    Parser p;
    if (kz_parser_init(&p, ctx, s->text, len, r[0], &values)) { kz_parser_free(&p); return JS_EXCEPTION; }
    p.source = r[1];
    bool indirect = flags & 0x100;
    p.strict = flags & JS_EVAL_STRICT;
    p.module = flags & JS_EVAL_MODULE;
    if (indirect) p.eval_kind = 3;
    Node *prog = kz_parse_program(&p);
    JSFunctionBytecode *b = NULL;
    if (prog) {
        if (indirect && !(prog->func->flags & FN_STRICT)) prog->func->flags |= FN_GLOBAL_VARS;
        b = compile(&p, prog);
    }
    kz_parser_free(&p);
    if (!b) return JS_EXCEPTION;
    r[2] = JV_MKPTR(b);
    JSValue fn = kz_new_closure(ctx, b, NULL, JS_UNDEFINED);
    if (fn == JS_EXCEPTION) return fn;
    if (flags & JS_EVAL_COMPILE) return fn;
    KZ_ROOT(ctx, rf);
    rf = fn;
    (void)rt;
    return kz_call(ctx, rf, ctx->global, 0, NULL);
}

/* ------------------------------------------------------------------ modules */

/* ParseModule (16.2.1.6.1): parses and compiles module source text.  Returns the module function's bytecode as a cell
   pointer value (the caller roots it at once; nothing allocates between the return and that), or JS_EXCEPTION with the
   SyntaxError pending.  `name` is the module's canonical name (an atom): it becomes the bytecode's filename. */
JSValue kz_compile_module(JSContext *ctx, const char *src, size_t len, JSValue name) {
    KZ_ROOT_VEC(ctx, values);
    KZ_ROOTS(ctx, r, 3);
    r[0] = name;
    KzSource *s = make_source(ctx, src, len);
    if (!s) return JS_EXCEPTION;
    r[1] = JV_MKPTR(s);
    Parser p;
    if (kz_parser_init(&p, ctx, s->text, len, r[0], &values)) { kz_parser_free(&p); return JS_EXCEPTION; }
    p.source = r[1];
    p.strict = true;
    p.module = true;
    Node *prog = kz_parse_program(&p);
    JSFunctionBytecode *b = prog ? compile(&p, prog) : NULL;
    kz_parser_free(&p);
    if (!b) return JS_EXCEPTION;
    return JV_MKPTR(b);
}

/* ------------------------------------------------------------------ direct eval */

JSValue kz_eval_direct(JSContext *ctx, JSFrame *f, JSValue arg, uint16_t first, uint16_t count) {
    if (!JV_IS_STRING(arg)) return arg;
    JSRuntime *rt = ctx->rt;
    JSFunctionBytecode *cb = f->b;
    bool caller_strict = cb->h.flags & BC_STRICT;
    int caller_kind = kz_bc_kind(cb);
    bool global_caller = caller_kind == KZ_FUNC_SCRIPT || (caller_kind == KZ_FUNC_EVAL && count == 0);
    KZ_ROOT_VEC(ctx, values);
    KZ_ROOTS(ctx, r, 4);
    r[0] = arg;
    size_t len;
    char *src = kz_str_to_utf8(ctx, r[0], &len);
    if (!src) return JS_EXCEPTION;
    KzSource *s = make_source(ctx, src, len);
    kz_free(rt, src);
    if (!s) return JS_EXCEPTION;
    r[1] = JV_MKPTR(s);
    r[2] = cb->filename;
    Parser p;
    if (kz_parser_init(&p, ctx, s->text, len, r[2], &values)) { kz_parser_free(&p); return JS_EXCEPTION; }
    p.source = r[1];
    p.strict = caller_strict;
    p.eval_kind = caller_strict ? 2 : 1;
    const KzVarName *vn = cb->varnames ? cb->varnames + first : NULL;
    FuncInfo *callerfi = NULL;
    if (!global_caller || count > 0) {
        callerfi = build_caller(&p, vn, count, caller_strict);
        if (!callerfi) { kz_parser_free(&p); return JS_EXCEPTION; }
    }
    if (!global_caller) {
        /* what the eval code may use: new.target in functions, super in methods */
        p.eval_in_function = true;
        p.eval_new_target_ok = caller_kind != KZ_FUNC_ARROW || callerfi->new_target_b;
        p.eval_super_prop_ok = callerfi->home_b != NULL;
        p.eval_super_call_ok = callerfi->fields_b != NULL && callerfi->this_b != NULL;     /* a derived constructor, or an arrow in one */
        p.eval_in_class_field = caller_kind == KZ_FUNC_FIELD_INIT || (callerfi->flags & FN_FIELD_INIT);
    }
    /* the private names of the classes around the call site (their descriptor entries are named "#name"): the eval code may
       refer to them, and finds the names' cells in the synthesized scopes like any other binding */
    int nprivate = 0;
    for (uint16_t i = 0; i < count; i++) if (vn[i].kind == VK_PRIVATE) nprivate++;
    if (nprivate) {
        JSValue *names = kz_arena_alloc(&p, (size_t)nprivate * sizeof(JSValue));
        if (!names) { kz_parser_free(&p); return JS_EXCEPTION; }
        int k = 0;
        for (uint16_t i = 0; i < count; i++) {
            if (vn[i].kind != VK_PRIVATE) continue;
            KZ_ROOT(ctx, bare);
            bare = kz_str_sub(ctx, vn[i].name, 1, kz_str_len(JV_STR(vn[i].name)));
            if (bare == JS_EXCEPTION) { kz_parser_free(&p); return JS_EXCEPTION; }
            JSValue at = kz_atom_from_str(ctx, bare);
            if (at == JS_EXCEPTION || kz_parser_keep(&p, at) == JS_EXCEPTION) { kz_parser_free(&p); return JS_EXCEPTION; }
            names[k++] = at;
        }
        p.outer_private = names;
        p.nouter_private = k;
    }

    Node *prog = kz_parse_program(&p);
    JSFunctionBytecode *b = NULL;
    if (prog) {
        FuncInfo *efi = prog->func;
        efi->flags |= FN_DIRECT_EVAL;
        bool strict = efi->flags & FN_STRICT;
        Scope *es = efi->scope;
        if (global_caller && !strict) efi->flags |= FN_GLOBAL_VARS;
        else if (!strict && callerfi) {
            /* sloppy eval in a function: var names go to the caller's binding of that name, or its var object */
            Binding **link = &es->first;
            Binding *last = NULL;
            int eval_obj = -1;
            /* (the var object of the function the eval code runs in; one of an enclosing function is not the place for its vars) */
            for (uint16_t i = 0; i < count; i++) if (vn[i].kind == VK_EVAL_OBJ && !vn[i].outer) { eval_obj = i; break; }
            bool failed = false;
            while (*link) {
                Binding *bb = *link;
                if (bb->kind != BK_VAR && bb->kind != BK_FUNC) { last = bb; link = &bb->next; continue; }
                int idx;
                const KzVarName *e = find_entry(vn, count, bb->name, &idx);
                /* A var of eval code in a parameter expression conflicts with the parameters and `arguments`, which live in an
                   environment between the eval code's and the var object's (the entries before the object in the descriptor). */
                bool in_params = e && e->paramenv && (e->kind == VK_PARAM || e->kind == VK_ARGUMENTS) && (eval_obj < 0 || idx < eval_obj);
                if (e && (e->kind == VK_LET || e->kind == VK_CONST || e->kind == VK_CLASS_INNER || in_params)) {
                    char buf[80];
                    kz_str_to_utf8_buf(JV_STR(bb->name), buf, sizeof buf);
                    kz_throw_syntax(ctx, "Identifier '%s' has already been declared", buf);
                    failed = true;
                    break;
                }
                if (eval_obj < 0) {
                    /* a parameter expression: there is no var object to put the name in, so it stays a variable of the eval code
                       itself (visible there only; the specification keeps it for the rest of the parameter list) */
                    last = bb;
                    link = &bb->next;
                    continue;
                }
                /* drop it from the eval scope: references resolve outward, to the caller's binding or the
                   var object (through the eval scope's eval_obj); the prologue binds the name there */
                *link = bb->next;
                if (es->last == bb) es->last = last;
                bb->next = NULL;
                Node *vn_node = kz_arena_alloc(&p, sizeof(Node));
                if (!vn_node) { failed = true; break; }
                vn_node->type = N_IDENT;
                vn_node->val = bb->name;
                vn_node->val2 = JS_UNDEFINED;
                vn_node->scope = es;
                vn_node->a = bb->kind == BK_FUNC && bb->decl && bb->decl->type == N_FUNC_DECL ? bb->decl : NULL;
                if (kz_list_push(&p, &efi->eval_vars, vn_node)) { failed = true; break; }
                (void)e;
            }
            if (failed) { kz_parser_free(&p); return JS_EXCEPTION; }
            if (eval_obj >= 0) {
                for (Scope *sc = callerfi->scopes; sc; sc = sc->next_in_func)
                    for (Binding *bb = sc->first; bb; bb = bb->next)
                        if (bb->slot == eval_obj && bb->kind == BK_HIDDEN) es->eval_obj = bb;
            }
        }
        if (!p.failed) b = compile(&p, prog);
    }
    /* the hoisting of sloppy eval vars that went to the caller's var object is done by the code itself:
       see the eval prologue in kz_compile_program (OP_eval_decls) */
    kz_parser_free(&p);
    if (!b) return JS_EXCEPTION;
    r[3] = JV_MKPTR(b);
    /* the closure: its cells come from the caller's frame through the descriptor */
    JSVarCell **cells = NULL;
    if (b->ncvars) {
        cells = kz_mallocz(ctx, b->ncvars * sizeof(JSVarCell *));
        if (!cells) return JS_EXCEPTION;
        for (uint32_t i = 0; i < b->ncvars; i++) {
            uint16_t di = b->cvars[i].index;
            if (!vn || di >= count) continue;
            const KzVarName *e = &vn[di];
            JSValue v;
            if (e->where == VN_LOCAL_CELL) v = f->regs[e->slot];
            else v = f->cells ? JV_MKPTR(f->cells[e->slot]) : JS_UNDEFINED;
            cells[i] = JV_IS_INTERNAL(v) ? JV_PTR(v) : NULL;
        }
    }
    JSShape *shape = ctx->shape_func;
    JSObject *fo = kz_obj_alloc(ctx, KZ_CLASS_FUNCTION, shape, 0);
    if (!fo) { kz_free(rt, cells); return JS_EXCEPTION; }
    b = JV_PTR(r[3]);
    fo->h.flags |= OBJ_CALLABLE;
    fo->u.func.b = b;
    fo->u.func.cells = cells;
    fo->u.func.home = f->func ? f->func->u.func.home : JS_UNDEFINED;
    fo->u.func.realm = ctx;
    fo->slots[0] = JV_MKINT(0);
    fo->slots[1] = rt->atom[KZ_ATOM_empty];
    KZ_ROOT(ctx, rf);
    rf = JV_MKOBJ(fo);
    /* run it with the caller's this and new.target */
    extern JSValue kz_call_with_new_target(JSContext *ctx, JSValue f, JSValue this_val, JSValue new_target);
    return kz_call_with_new_target(ctx, rf, f->this_val, f->new_target);
}

/* ------------------------------------------------------------------ the Function constructor */

JSValue kz_compile_function_text(JSContext *ctx, const char *prefix, JSValue params, JSValue body, int kind) {
    (void)kind;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    kz_sb_putc(&sb, '(');
    kz_sb_puts(&sb, prefix);
    kz_sb_puts(&sb, " anonymous(");
    kz_sb_put_value(&sb, params);
    kz_sb_puts(&sb, "\n) {\n");
    kz_sb_put_value(&sb, body);
    kz_sb_puts(&sb, "\n})");
    KZ_ROOT(ctx, text);
    text = kz_sb_finish(&sb);
    if (text == JS_EXCEPTION) return text;
    size_t len;
    char *src = kz_str_to_utf8(ctx, text, &len);
    if (!src) return JS_EXCEPTION;
    KZ_ROOT_VEC(ctx, values);
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_atom(ctx, "anonymous");
    if (r[0] == JS_EXCEPTION) { kz_free(ctx->rt, src); return JS_EXCEPTION; }
    KzSource *s = make_source(ctx, src, len);
    kz_free(ctx->rt, src);
    if (!s) return JS_EXCEPTION;
    r[1] = JV_MKPTR(s);
    Parser p;
    if (kz_parser_init(&p, ctx, s->text, len, r[0], &values)) { kz_parser_free(&p); return JS_EXCEPTION; }
    p.source = r[1];
    Node *prog = kz_parse_program(&p);
    JSFunctionBytecode *b = NULL;
    if (prog) {
        /* exactly one function expression spanning the text: anything else means the parameters or the body
           escaped their place */
        Node *st = prog->list.n == 1 ? prog->list.v[0] : NULL;
        Node *e = st && st->type == N_EXPR_STMT ? st->a : NULL;
        while (e && e->type == N_PAREN) e = e->a;
        if (!e || e->type != N_FUNC || e->func->src_start != 1 || e->func->src_end + 1 != len) {
            kz_throw_syntax(ctx, "Invalid function body or parameters");
            kz_parser_free(&p);
            return JS_EXCEPTION;
        }
        prog->func->flags |= FN_GLOBAL_VARS;
        b = compile(&p, prog);
    }
    kz_parser_free(&p);
    if (!b) return JS_EXCEPTION;
    r[2] = JV_MKPTR(b);
    JSValue fn = kz_new_closure(ctx, b, NULL, JS_UNDEFINED);
    if (fn == JS_EXCEPTION) return fn;
    KZ_ROOT(ctx, rf);
    rf = fn;
    /* the program's completion value is the function */
    return kz_call(ctx, rf, ctx->global, 0, NULL);
}
