/*
 * parser.c: the syntactic grammar (ECMA-262 13 to 16) as recursive descent, building the tree of parser.h, with the
 * early errors of the specification checked as the constructs are recognised, and Annex B's web-compatibility rules
 * for sloppy-mode code (function declarations in blocks and in if statements, HTML-like comments, legacy octals).
 *
 * Arrow functions: at '(' (or 'async (') the parser scans ahead over balanced tokens to see whether '=>' follows the
 * matching ')'.  If it does, the list is parsed as formal parameters inside the arrow's own scope; otherwise as a
 * parenthesised expression.  Destructuring assignment uses the cover grammar instead: an object or array literal
 * followed by '=' is converted into a pattern (to_pattern), which also validates `{ a = 1 }` initialisers.
 *
 * Every declaration goes into its scope while parsing; identifier references remember their scope and the compiler
 * resolves them afterwards.
 */
#include "parser.h"
#include <stdarg.h>

#define MAX_DEPTH 1500

struct ArenaBlock { struct ArenaBlock *next; size_t size; };
struct Arena { struct ArenaBlock *head; char *cur; size_t left; JSRuntime *rt; };

void *kz_arena_alloc(Parser *p, size_t n) {
    Arena *a = p->arena;
    n = (n + 7) & ~(size_t)7;
    if (n > a->left) {
        size_t bs = n > 32000 ? n : 32000;
        struct ArenaBlock *b = kz_rt_malloc(a->rt, sizeof(struct ArenaBlock) + bs);
        if (!b) { kz_throw_oom(p->ctx); p->failed = true; return NULL; }
        b->next = a->head;
        b->size = bs;
        a->head = b;
        a->cur = (char *)(b + 1);
        a->left = bs;
    }
    void *r = a->cur;
    a->cur += n;
    a->left -= n;
    memset(r, 0, n);
    return r;
}

JSValue kz_parser_keep(Parser *p, JSValue v) {
    if (!p) return v;
    if (!JV_IS_HEAP(v)) return v;
    if (kz_vec_push(p->ctx, p->values, v)) { p->failed = true; return JS_EXCEPTION; }
    return v;
}

int kz_list_push(Parser *p, NodeList *l, Node *n) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 4;
        Node **nv = kz_arena_alloc(p, (size_t)nc * sizeof(Node *));
        if (!nv) return -1;
        if (l->n) memcpy(nv, l->v, (size_t)l->n * sizeof(Node *));
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = n;
    return 0;
}

int kz_parser_init(Parser *p, JSContext *ctx, const char *src, size_t len, JSValue filename, KzVec *values) {
    memset(p, 0, sizeof *p);
    p->ctx = ctx;
    p->values = values;
    p->filename = filename;
    p->arena = kz_rt_malloc(ctx->rt, sizeof(Arena));
    if (!p->arena) { kz_throw_oom(ctx); return -1; }
    memset(p->arena, 0, sizeof(Arena));
    p->arena->rt = ctx->rt;
    if (len > 0x7FFFFFF0) { kz_throw_range(ctx, "source too long"); return -1; }
    kz_lex_init(&p->lx, ctx, (const uint8_t *)src, (uint32_t)len);
    p->lx.p = p;
    return 0;
}

void kz_parser_free(Parser *p) {
    if (!p->arena) return;
    struct ArenaBlock *b = p->arena->head;
    while (b) { struct ArenaBlock *n = b->next; kz_free(p->ctx->rt, b); b = n; }
    kz_free(p->ctx->rt, p->arena);
    p->arena = NULL;
    if (p->private_refs) kz_free(p->ctx->rt, p->private_refs);
}

/* ------------------------------------------------------------------ errors and tokens */

#define TOK (p->lx.tok)

static void verror_at(Parser *p, uint32_t line, uint32_t col, const char *fmt, va_list ap) {
    if (p->failed) return;
    extern int kz_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
    char msg[256];
    kz_vsnprintf(msg, sizeof msg, fmt, ap);
    p->failed = true;
    p->err_line = line;
    p->err_col = col;
    kz_throw_syntax_at(p->ctx, p->filename, line, col, "%s", msg);
}

static void error_at(Parser *p, uint32_t line, uint32_t col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(p, line, col, fmt, ap);
    va_end(ap);
}

static void error_tok(Parser *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(p, TOK.line, TOK.col, fmt, ap);
    va_end(ap);
}

static void error_node(Parser *p, Node *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(p, n->line, n->col, fmt, ap);
    va_end(ap);
}

static const char *const punct_text[T_COUNT] = {
    [T_LBRACE] = "{", [T_RBRACE] = "}", [T_LPAREN] = "(", [T_RPAREN] = ")", [T_LBRACKET] = "[", [T_RBRACKET] = "]",
    [T_DOT] = ".", [T_ELLIPSIS] = "...", [T_SEMI] = ";", [T_COMMA] = ",", [T_LT] = "<", [T_GT] = ">", [T_LE] = "<=",
    [T_GE] = ">=", [T_EQ] = "==", [T_NE] = "!=", [T_SEQ] = "===", [T_SNE] = "!==", [T_PLUS] = "+", [T_MINUS] = "-",
    [T_STAR] = "*", [T_SLASH] = "/", [T_PERCENT] = "%", [T_STARSTAR] = "**", [T_INC] = "++", [T_DEC] = "--",
    [T_SHL] = "<<", [T_SAR] = ">>", [T_SHR] = ">>>", [T_AMP] = "&", [T_PIPE] = "|", [T_CARET] = "^", [T_BANG] = "!",
    [T_TILDE] = "~", [T_ANDAND] = "&&", [T_OROR] = "||", [T_QQ] = "??", [T_QUESTION] = "?", [T_QDOT] = "?.",
    [T_COLON] = ":", [T_ASSIGN] = "=", [T_PLUS_ASSIGN] = "+=", [T_MINUS_ASSIGN] = "-=", [T_STAR_ASSIGN] = "*=",
    [T_SLASH_ASSIGN] = "/=", [T_PERCENT_ASSIGN] = "%=", [T_STARSTAR_ASSIGN] = "**=", [T_SHL_ASSIGN] = "<<=",
    [T_SAR_ASSIGN] = ">>=", [T_SHR_ASSIGN] = ">>>=", [T_AMP_ASSIGN] = "&=", [T_PIPE_ASSIGN] = "|=",
    [T_CARET_ASSIGN] = "^=", [T_ANDAND_ASSIGN] = "&&=", [T_OROR_ASSIGN] = "||=", [T_QQ_ASSIGN] = "?" "?=",
    [T_ARROW] = "=>", [T_AT] = "@",
};

static void name_text(JSValue atom, char *buf, size_t cap) { kz_str_to_utf8_buf(JV_STR(atom), buf, cap); }

static void unexpected(Parser *p) {
    if (p->failed) return;
    char buf[80];
    switch (TOK.type) {
    case T_EOF: error_tok(p, "Unexpected end of input"); return;
    case T_NAME:
        name_text(TOK.val, buf, sizeof buf);
        if (TOK.kw && TOK.kw < KW_FIRST_CONTEXTUAL && !TOK.escaped) error_tok(p, "Unexpected token '%s'", buf);
        else error_tok(p, "Unexpected identifier '%s'", buf);
        return;
    case T_PRIVATE: error_tok(p, "Unexpected private name"); return;
    case T_NUMBER: case T_BIGINT: error_tok(p, "Unexpected number"); return;
    case T_STRING: error_tok(p, "Unexpected string"); return;
    case T_TEMPLATE: error_tok(p, "Unexpected template string"); return;
    case T_REGEXP: error_tok(p, "Unexpected regular expression"); return;
    default:
        error_tok(p, "Unexpected token '%s'", punct_text[TOK.type] ? punct_text[TOK.type] : "?");
    }
}

static int next(Parser *p) {
    if (kz_lex_next(&p->lx)) { p->failed = true; return -1; }
    return 0;
}

static inline bool at(Parser *p, int type) { return TOK.type == type; }
static inline bool at_kw(Parser *p, int kw) { return TOK.type == T_NAME && TOK.kw == kw && !TOK.escaped; }

static int expect(Parser *p, int type) {
    if (TOK.type != type) { unexpected(p); return -1; }
    return next(p);
}

static int semicolon(Parser *p) {
    if (at(p, T_SEMI)) return next(p);
    if (at(p, T_RBRACE) || at(p, T_EOF) || TOK.nl_before) return 0;
    unexpected(p);
    return -1;
}

static Node *new_node(Parser *p, int type) {
    Node *n = kz_arena_alloc(p, sizeof(Node));
    if (!n) return NULL;
    n->type = (uint16_t)type;
    n->pos = TOK.pos;
    n->end = TOK.end;
    n->line = TOK.line;
    n->col = TOK.col;
    n->val = JS_UNDEFINED;
    n->val2 = JS_UNDEFINED;
    n->scope = p->scope;
    return n;
}

static Node *new_node_at(Parser *p, int type, const Node *start) {
    Node *n = new_node(p, type);
    if (!n) return NULL;
    n->pos = start->pos;
    n->line = start->line;
    n->col = start->col;
    return n;
}

static inline void finish(Parser *p, Node *n) { n->end = p->lx.prev_end; }

static bool depth_enter(Parser *p) {
    if (++p->depth > MAX_DEPTH || kz_check_stack(p->ctx)) {
        if (!p->failed) {
            if (kz_has_exception(p->ctx)) kz_take_exception(p->ctx);
            error_tok(p, "Program too deeply nested");
        }
        p->depth--;
        return false;
    }
    return true;
}
#define DEPTH_ENTER() do { if (!depth_enter(p)) return NULL; } while (0)
#define DEPTH_LEAVE() (p->depth--)

/* ------------------------------------------------------------------ scopes */

static Scope *push_scope(Parser *p, int kind) {
    Scope *s = kz_arena_alloc(p, sizeof(Scope));
    if (!s) return NULL;
    s->kind = (uint8_t)kind;
    s->parent = p->scope;
    s->func = p->fn;
    s->start = TOK.pos;
    if (p->fn) {
        s->next_in_func = p->fn->scopes;
        p->fn->scopes = s;
    }
    p->scope = s;
    return s;
}

static void pop_scope(Parser *p) {
    p->scope->end = p->lx.prev_end;
    p->scope = p->scope->parent;
}

Binding *kz_scope_find(Scope *s, JSValue name) {
    for (Binding *b = s->first; b; b = b->next)
        if (b->name == name) return b;
    return NULL;
}

static Binding *add_binding(Parser *p, Scope *s, JSValue name, int kind) {
    Binding *b = kz_arena_alloc(p, sizeof(Binding));
    if (!b) return NULL;
    b->name = name;
    b->kind = (uint8_t)kind;
    b->scope = s;
    b->slot = -1;
    b->export_index = -1;
    b->decl_pos = TOK.pos;
    b->decl_end = TOK.end;
    if (s->last) s->last->next = b; else s->first = b;
    s->last = b;
    return b;
}

Binding *kz_declare_hidden(Parser *p, Scope *s, JSValue name) {
    Binding *b = add_binding(p, s, name, BK_HIDDEN);
    return b;
}

static bool is_lexical_kind(int k) { return k == BK_LET || k == BK_CONST || k == BK_CLASS; }

static bool is_var_scope(const Scope *s) {
    return s->kind == SC_FUNC || s->kind == SC_SCRIPT || s->kind == SC_MODULE || s->kind == SC_EVAL;
}

static Scope *var_scope(Parser *p) {
    Scope *s = p->scope;
    while (!is_var_scope(s)) s = s->parent;
    return s;
}

static bool in_var_names(const Scope *s, JSValue name) {
    for (int i = 0; i < s->var_names.n; i++)
        if (s->var_names.v[i]->val == name) return true;
    return false;
}

static void redeclared(Parser *p, JSValue name, uint32_t line, uint32_t col) {
    char buf[80];
    name_text(name, buf, sizeof buf);
    error_at(p, line, col, "Identifier '%s' has already been declared", buf);
}

/* var x (and function declarations at function top level, which behave like var for conflicts) */
static Binding *declare_var(Parser *p, JSValue name, int kind, uint32_t line, uint32_t col) {
    Scope *vs = var_scope(p);
    for (Scope *s = p->scope;; s = s->parent) {
        Binding *b = kz_scope_find(s, name);
        if (b) {
            bool lexical = is_lexical_kind(b->kind) || (b->kind == BK_FUNC && (!is_var_scope(s) || s->kind == SC_MODULE)) ||
                           (b->kind == BK_CATCH && !(b->flags & BF_SIMPLE_CATCH)) || b->kind == BK_IMPORT;
            if (lexical) { redeclared(p, name, line, col); return NULL; }
        }
        if (s == vs) {
            if (b && (b->kind == BK_VAR || b->kind == BK_FUNC || b->kind == BK_PARAM || b->kind == BK_ARGUMENTS)) {
                if (kind == BK_FUNC && b->kind == BK_VAR) b->kind = BK_FUNC;
                return b;
            }
            if (b && b->kind == BK_CATCH) {
                /* simple catch parameter: the var is a separate binding in the function scope */
            }
            /* a parameter of the enclosing params scope with the same name: the body var starts with its value */
            Binding *nb = add_binding(p, s, name, kind);
            if (nb) { nb->decl_pos = TOK.pos; }
            return nb;
        }
        Node *n = kz_arena_alloc(p, sizeof(Node));
        if (!n) return NULL;
        n->type = N_IDENT;
        n->val = name;
        if (kz_list_push(p, &s->var_names, n)) return NULL;
    }
}

static Binding *declare_lexical(Parser *p, Scope *s, JSValue name, int kind, uint32_t line, uint32_t col) {
    if (name == p->ctx->rt->atom[KZ_ATOM_let] && kind != BK_FUNC && kind != BK_CATCH && kind != BK_PARAM) {
        error_at(p, line, col, "let is disallowed as a lexically bound name");
        return NULL;
    }
    Binding *b = kz_scope_find(s, name);
    if (b) {
        bool sloppy_dup_fn = kind == BK_FUNC && b->kind == BK_FUNC && !p->strict && !is_var_scope(s);
        if (!sloppy_dup_fn) { redeclared(p, name, line, col); return NULL; }
        return b;
    }
    if (in_var_names(s, name)) { redeclared(p, name, line, col); return NULL; }
    /* the body's lexical names may not repeat a parameter (non-simple lists keep parameters a scope up) */
    if (s->kind == SC_FUNC && s->parent && s->parent->kind == SC_PARAMS && s->parent->func == s->func &&
        kz_scope_find(s->parent, name)) {
        redeclared(p, name, line, col);
        return NULL;
    }
    b = add_binding(p, s, name, kind);
    return b;
}

/* ------------------------------------------------------------------ identifiers */

static bool is_strict_reserved(int kw) { return kw >= KW_FIRST_STRICT && kw < KW_FIRST_CONTEXTUAL; }

/* checks that the current T_NAME token may be used as an identifier (reference or binding) here */
static bool check_identifier(Parser *p, bool binding) {
    int kw = TOK.kw;
    char buf[80];
    if (kw == KW_NONE) return true;
    if (kw >= KW_FIRST_CONTEXTUAL) {
        if (binding && p->strict && (kw == KW_eval || kw == KW_arguments)) {
            name_text(TOK.val, buf, sizeof buf);
            error_tok(p, "Unexpected eval or arguments in strict mode");
            return false;
        }
        return true;
    }
    if (kw == KW_yield) {
        if (p->strict || (p->fn->flags & FN_GENERATOR)) { error_tok(p, "Unexpected strict mode reserved word 'yield'"); return false; }
        return true;
    }
    if (kw == KW_await) {
        if (p->module || (p->fn->flags & (FN_ASYNC | FN_STATIC_BLOCK))) { error_tok(p, "Unexpected reserved word 'await'"); return false; }
        if (p->in_class_field && binding) { error_tok(p, "Unexpected reserved word 'await'"); return false; }
        return true;
    }
    if (is_strict_reserved(kw)) {
        if (p->strict) {
            name_text(TOK.val, buf, sizeof buf);
            error_tok(p, "Unexpected strict mode reserved word '%s'", buf);
            return false;
        }
        return true;
    }
    name_text(TOK.val, buf, sizeof buf);
    error_tok(p, TOK.escaped ? "Keyword must not contain escaped characters" : "Unexpected token '%s'", buf);
    return false;
}

static void mark_uses(Parser *p, JSValue name) {
    JSRuntime *rt = p->ctx->rt;
    if (name == rt->atom[KZ_ATOM_arguments]) {
        FuncInfo *f = p->fn;
        while (f && (f->flags & FN_ARROW)) f = f->parent;
        if (f) f->uses_arguments = true;
        if (p->in_class_field && (!f || (f->flags & (FN_FIELD_INIT | FN_EVAL)))) error_tok(p, "'arguments' is not allowed in class field initializer or static initialization block");
    }
}

static Node *ident_ref(Parser *p) {
    if (!check_identifier(p, false)) return NULL;
    Node *n = new_node(p, N_IDENT);
    if (!n) return NULL;
    n->val = TOK.val;
    mark_uses(p, TOK.val);
    if (p->failed) return NULL;
    if (next(p)) return NULL;
    return n;
}

/* ------------------------------------------------------------------ forward declarations */

static Node *parse_statement(Parser *p, bool labelled_fn_ok);
static Node *parse_statement_list_item(Parser *p);
static Node *parse_expression(Parser *p, bool in);
static Node *parse_assignment(Parser *p, bool in);
static Node *parse_unary(Parser *p);
static Node *parse_lhs(Parser *p, bool allow_call);
static Node *parse_primary(Parser *p);
static Node *parse_function(Parser *p, int kind_flags, bool is_decl, bool is_export_default);
static Node *parse_class(Parser *p, bool is_decl, bool name_optional);
static Node *parse_binding_target(Parser *p, int decl_kind);
static Node *parse_template(Parser *p, bool tagged);
static Node *parse_arrow_from_params(Parser *p, uint32_t flags, Node *start_tok_node, Node *single_param);
static int parse_function_body(Parser *p, FuncInfo *fi);
static Node *parse_block(Parser *p, int scope_kind);
static int parse_directives_and_body(Parser *p, NodeList *body, FuncInfo *fi, int end_token);

/* ------------------------------------------------------------------ lookahead */

typedef struct LexState { Lexer lx; } LexState;
static void save_lex(Parser *p, LexState *s) { s->lx = p->lx; }
static void restore_lex(Parser *p, LexState *s) { p->lx = s->lx; }

/* At '(' : does '=>' follow the matching ')' on the same line?  A token scan over balanced brackets; regexps and
   templates are told apart from division and braces by the previous token. */
static bool arrow_ahead(Parser *p) {
    LexState st;
    save_lex(p, &st);
    bool failed = p->failed;
    int depth = 0;
    int stack[256];         /* bracket kinds: 1 (, 2 [, 3 {, 4 template ${ */
    int sp = 0;
    bool result = false;
    int prev = T_LPAREN;
    for (;;) {
        int t = TOK.type;
        if (t == T_EOF) break;
        if (t == T_LPAREN || t == T_LBRACKET || t == T_LBRACE) {
            if (sp == 256) break;
            stack[sp++] = t == T_LPAREN ? 1 : t == T_LBRACKET ? 2 : 3;
            depth++;
        } else if (t == T_TEMPLATE && !TOK.tail) {
            if (sp == 256) break;
            stack[sp++] = 4;
        } else if (t == T_RPAREN || t == T_RBRACKET || t == T_RBRACE) {
            if (!sp) break;
            int k = stack[--sp];
            if (k == 4 && t == T_RBRACE) {
                if (kz_lex_template_continue(&p->lx)) break;
                if (!TOK.tail) stack[sp++] = 4;
                prev = T_TEMPLATE;
                if (kz_lex_next(&p->lx)) break;
                continue;
            }
            if ((k == 1 && t != T_RPAREN) || (k == 2 && t != T_RBRACKET) || (k == 3 && t != T_RBRACE)) break;
            if (sp == 0) {
                if (kz_lex_next(&p->lx)) break;
                result = TOK.type == T_ARROW && !TOK.nl_before;
                break;
            }
        } else if (t == T_SLASH || t == T_SLASH_ASSIGN) {
            bool division = prev == T_RPAREN || prev == T_RBRACKET || prev == T_RBRACE || prev == T_NAME ||
                            prev == T_NUMBER || prev == T_STRING || prev == T_TEMPLATE || prev == T_BIGINT ||
                            prev == T_REGEXP || prev == T_PRIVATE;
            if (!division && kz_lex_regexp(&p->lx)) break;
        }
        prev = TOK.type;
        if (kz_lex_next(&p->lx)) break;
    }
    (void)depth;
    restore_lex(p, &st);
    if (!failed && p->failed) {
        p->failed = false;
        kz_take_exception(p->ctx);
    }
    return result;
}

/* the token after the current one */
static int peek_type(Parser *p, bool *nl, int *kw) {
    LexState st;
    save_lex(p, &st);
    bool failed = p->failed;
    int t = T_EOF;
    if (!kz_lex_next(&p->lx)) {
        t = TOK.type;
        if (nl) *nl = TOK.nl_before;
        if (kw) *kw = TOK.type == T_NAME && !TOK.escaped ? TOK.kw : KW_NONE;
    }
    restore_lex(p, &st);
    if (!failed && p->failed) { p->failed = false; kz_take_exception(p->ctx); }
    return t;
}

/* ------------------------------------------------------------------ patterns */

static bool is_valid_simple_target(Parser *p, Node *n) {
    while (n->type == N_PAREN) n = n->a;
    if (n->type == N_IDENT) {
        if (p->strict && (n->val == p->ctx->rt->atom[KZ_ATOM_eval] || n->val == p->ctx->rt->atom[KZ_ATOM_arguments])) return false;
        return true;
    }
    if (n->type == N_MEMBER || n->type == N_PRIVATE_MEMBER) return !(n->flags & F_OPTIONAL);
    return false;
}

static Node *strip_parens(Node *n) {
    while (n && n->type == N_PAREN) n = n->a;
    return n;
}

/* converts an expression to an assignment pattern (ECMA-262 13.15.5); binding = false here (identifiers are references) */
static Node *to_pattern(Parser *p, Node *e) {
    if (!e) return NULL;
    switch (e->type) {
    case N_IDENT:
        if (p->strict && (e->val == p->ctx->rt->atom[KZ_ATOM_eval] || e->val == p->ctx->rt->atom[KZ_ATOM_arguments])) {
            error_node(p, e, "Unexpected eval or arguments in strict mode");
            return NULL;
        }
        return e;
    case N_MEMBER: case N_PRIVATE_MEMBER:
        if (e->flags & F_OPTIONAL) break;
        return e;
    case N_PAREN: {
        Node *inner = strip_parens(e);
        if (inner->type == N_IDENT || inner->type == N_MEMBER || inner->type == N_PRIVATE_MEMBER) return to_pattern(p, inner);
        break;
    }
    case N_ASSIGN:
        if (e->op != T_ASSIGN) break;
        {
            Node *t = to_pattern(p, e->a);
            if (!t) return NULL;
            Node *n = new_node_at(p, N_ASSIGN_PAT, e);
            if (!n) return NULL;
            n->a = t;
            n->b = e->b;
            return n;
        }
    case N_ARRAY: {
        if (e->flags & F_PARENS) break;
        Node *n = new_node_at(p, N_ARR_PATTERN, e);
        if (!n) return NULL;
        for (int i = 0; i < e->list.n; i++) {
            Node *el = e->list.v[i], *t = NULL;
            if (el) {
                if (el->type == N_SPREAD) {
                    if (i != e->list.n - 1 || (e->ival & 1)) { error_node(p, el, "Rest element must be last element"); return NULL; }
                    Node *target = to_pattern(p, el->a);
                    if (!target) return NULL;
                    if (target->type == N_ASSIGN_PAT) { error_node(p, el, "Rest element may not have a default initializer"); return NULL; }
                    t = new_node_at(p, N_REST, el);
                    if (!t) return NULL;
                    t->a = target;
                } else {
                    t = to_pattern(p, el);
                    if (!t) return NULL;
                }
            }
            if (kz_list_push(p, &n->list, t)) return NULL;
        }
        return n;
    }
    case N_OBJECT: {
        if (e->flags & F_PARENS) break;
        Node *n = new_node_at(p, N_OBJ_PATTERN, e);
        if (!n) return NULL;
        for (int i = 0; i < e->list.n; i++) {
            Node *pr = e->list.v[i], *t;
            if (pr->op == P_SPREAD) {
                if (i != e->list.n - 1) { error_node(p, pr, "Rest element must be last element"); return NULL; }
                Node *target = to_pattern(p, pr->a);
                if (!target) return NULL;
                if (target->type != N_IDENT && target->type != N_MEMBER && target->type != N_PRIVATE_MEMBER) {
                    error_node(p, pr, "`...` must be followed by an assignable reference in assignment contexts");
                    return NULL;
                }
                t = new_node_at(p, N_REST, pr);
                if (!t) return NULL;
                t->a = target;
            } else if (pr->op == P_INIT || pr->op == P_SHORTHAND || pr->op == P_PROTO) {
                t = new_node_at(p, N_PAT_PROP, pr);
                if (!t) return NULL;
                t->a = pr->a;
                t->flags = pr->flags & F_COMPUTED;
                t->b = to_pattern(p, pr->b);
                if (!t->b) return NULL;
            } else {
                error_node(p, pr, "Invalid destructuring assignment target");
                return NULL;
            }
            if (kz_list_push(p, &n->list, t)) return NULL;
        }
        return n;
    }
    default:
        break;
    }
    error_node(p, e, "Invalid destructuring assignment target");
    return NULL;
}

/* does the expression tree still hold a `{ a = 1 }` initialiser that no pattern conversion consumed? */
static Node *find_cover_init(Node *e, int depth) {
    if (!e || depth > 200) return NULL;
    if (e->type == N_OBJECT) {
        for (int i = 0; i < e->list.n; i++) {
            Node *pr = e->list.v[i];
            if (pr->flags & F_SHORTHAND_INIT) return pr;
            Node *f = find_cover_init(pr->b, depth + 1);
            if (f) return f;
        }
    } else if (e->type == N_ARRAY) {
        for (int i = 0; i < e->list.n; i++) {
            Node *f = find_cover_init(e->list.v[i], depth + 1);
            if (f) return f;
        }
    } else if (e->type == N_SPREAD || e->type == N_PAREN) return find_cover_init(e->a, depth + 1);
    return NULL;
}

/* ------------------------------------------------------------------ templates */

static Node *parse_template(Parser *p, bool tagged) {
    Node *n = new_node(p, N_TEMPLATE);
    if (!n) return NULL;
    for (;;) {
        if (!at(p, T_TEMPLATE)) { unexpected(p); return NULL; }
        if (!tagged && TOK.val == JS_UNDEFINED) { error_tok(p, "Invalid escape sequence in template"); return NULL; }
        Node *part = new_node(p, N_TEMPLATE_PART);
        if (!part) return NULL;
        part->val = TOK.val;
        part->val2 = TOK.raw;
        if (kz_list_push(p, &n->list, part)) return NULL;
        bool tail = TOK.tail;
        if (next(p)) return NULL;
        if (tail) break;
        Node *e = parse_expression(p, true);
        if (!e) return NULL;
        if (kz_list_push(p, &n->list, e)) return NULL;
        if (!at(p, T_RBRACE)) { unexpected(p); return NULL; }
        if (kz_lex_template_continue(&p->lx)) { p->failed = true; return NULL; }
    }
    finish(p, n);
    return n;
}

/* ------------------------------------------------------------------ literals */

static Node *parse_property_key(Parser *p, Node *prop, bool allow_private) {
    /* fills prop->a with the key (N_STRING/N_NUMBER/N_BIGINT/computed expression/N_PRIVATE_NAME) */
    if (at(p, T_LBRACKET)) {
        if (next(p)) return NULL;
        Node *k = parse_assignment(p, true);
        if (!k) return NULL;
        if (expect(p, T_RBRACKET)) return NULL;
        prop->flags |= F_COMPUTED;
        prop->a = k;
        return k;
    }
    Node *k;
    switch (TOK.type) {
    case T_NAME:
        k = new_node(p, N_STRING);
        if (!k) return NULL;
        k->val = TOK.val;
        break;
    case T_STRING:
        if (TOK.legacy_octal && p->strict) { error_tok(p, "Octal escape sequences are not allowed in strict mode"); return NULL; }
        k = new_node(p, N_STRING);
        if (!k) return NULL;
        k->val = TOK.val;
        k->ival = 1;    /* a string literal key (for "constructor" and "__proto__" checks) */
        break;
    case T_NUMBER:
        if (TOK.legacy_octal && p->strict) { error_tok(p, "Octal literals are not allowed in strict mode"); return NULL; }
        k = new_node(p, N_NUMBER);
        if (!k) return NULL;
        k->num = TOK.num;
        break;
    case T_BIGINT:
        k = new_node(p, N_BIGINT);
        if (!k) return NULL;
        k->val = TOK.raw;
        k->ival = (int32_t)TOK.num;
        break;
    case T_PRIVATE:
        if (!allow_private) { unexpected(p); return NULL; }
        k = new_node(p, N_PRIVATE_NAME);
        if (!k) return NULL;
        k->val = TOK.val;
        prop->flags |= F_PRIVATE_KEY;
        break;
    default:
        unexpected(p);
        return NULL;
    }
    if (next(p)) return NULL;
    prop->a = k;
    return k;
}

/* method definitions in object literals and classes: the function node */
static Node *parse_method(Parser *p, uint32_t flags) {
    return parse_function(p, (int)(flags | FN_METHOD), false, false);
}

static Node *parse_object_literal(Parser *p) {
    Node *obj = new_node(p, N_OBJECT);
    if (!obj) return NULL;
    if (next(p)) return NULL;
    bool has_proto = false;
    JSRuntime *rt = p->ctx->rt;
    while (!at(p, T_RBRACE)) {
        Node *pr = new_node(p, N_PROP);
        if (!pr) return NULL;
        if (at(p, T_ELLIPSIS)) {
            if (next(p)) return NULL;
            pr->op = P_SPREAD;
            pr->a = parse_assignment(p, true);
            if (!pr->a) return NULL;
            goto add;
        }
        uint32_t fflags = 0;
        bool is_async = false, is_gen = false;
        int accessor = 0;
        if (at_kw(p, KW_async)) {
            int k;
            bool nl;
            int t = peek_type(p, &nl, &k);
            if (t != T_COMMA && t != T_COLON && t != T_LPAREN && t != T_RBRACE && t != T_ASSIGN && !nl) {
                is_async = true;
                if (next(p)) return NULL;
            }
        }
        if (at(p, T_STAR)) { is_gen = true; if (next(p)) return NULL; }
        if (!is_async && !is_gen && (at_kw(p, KW_get) || at_kw(p, KW_set))) {
            int t = peek_type(p, NULL, NULL);
            if (t != T_COMMA && t != T_COLON && t != T_LPAREN && t != T_RBRACE && t != T_ASSIGN) {
                accessor = at_kw(p, KW_get) ? 1 : 2;
                if (next(p)) return NULL;
            }
        }
        Token keytok = TOK;
        if (!parse_property_key(p, pr, false)) return NULL;
        if (is_async || is_gen || accessor || at(p, T_LPAREN)) {
            fflags = (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GENERATOR : 0) | (accessor == 1 ? FN_GETTER : accessor == 2 ? FN_SETTER : 0);
            pr->op = accessor == 1 ? P_GET : accessor == 2 ? P_SET : P_METHOD;
            pr->b = parse_method(p, fflags);
            if (!pr->b) return NULL;
            pr->b->func->src_start = pr->pos;       /* Function.prototype.toString: the whole method definition, name included */
            goto add;
        }
        if (at(p, T_COLON)) {
            if (next(p)) return NULL;
            pr->op = P_INIT;
            pr->b = parse_assignment(p, true);
            if (!pr->b) return NULL;
            if (!(pr->flags & F_COMPUTED) && pr->a->type == N_STRING && pr->a->val == rt->atom[KZ_ATOM_proto]) {
                if (has_proto) pr->ival = 1;     /* duplicate: an error unless this becomes a pattern */
                has_proto = true;
                pr->op = P_PROTO;
            }
            goto add;
        }
        /* shorthand: an identifier reference */
        if (keytok.type != T_NAME || (pr->flags & F_COMPUTED)) { unexpected(p); return NULL; }
        {
            Lexer saved = p->lx;
            p->lx.tok = keytok;
            bool ok = check_identifier(p, false);
            p->lx = saved;
            if (!ok) return NULL;
            Node *id = kz_arena_alloc(p, sizeof(Node));
            if (!id) return NULL;
            id->type = N_IDENT;
            id->val = keytok.val;
            id->pos = keytok.pos;
            id->end = keytok.end;
            id->line = keytok.line;
            id->col = keytok.col;
            id->scope = p->scope;
            id->val2 = JS_UNDEFINED;
            mark_uses(p, keytok.val);
            pr->op = P_SHORTHAND;
            pr->b = id;
            if (at(p, T_ASSIGN)) {
                /* CoverInitializedName: only valid when this literal becomes a pattern */
                if (next(p)) return NULL;
                Node *def = parse_assignment(p, true);
                if (!def) return NULL;
                Node *as = new_node_at(p, N_ASSIGN, id);
                if (!as) return NULL;
                as->op = T_ASSIGN;
                as->a = id;
                as->b = def;
                pr->b = as;
                pr->flags |= F_SHORTHAND_INIT;
            }
        }
    add:
        finish(p, pr);
        if (kz_list_push(p, &obj->list, pr)) return NULL;
        if (at(p, T_RBRACE)) break;
        if (expect(p, T_COMMA)) return NULL;
    }
    if (next(p)) return NULL;
    finish(p, obj);
    return obj;
}

static Node *parse_array_literal(Parser *p) {
    Node *arr = new_node(p, N_ARRAY);
    if (!arr) return NULL;
    if (next(p)) return NULL;
    while (!at(p, T_RBRACKET)) {
        if (at(p, T_COMMA)) {
            if (kz_list_push(p, &arr->list, NULL)) return NULL;
            if (next(p)) return NULL;
            continue;
        }
        Node *el;
        if (at(p, T_ELLIPSIS)) {
            el = new_node(p, N_SPREAD);
            if (!el) return NULL;
            if (next(p)) return NULL;
            el->a = parse_assignment(p, true);
            if (!el->a) return NULL;
            finish(p, el);
        } else {
            el = parse_assignment(p, true);
            if (!el) return NULL;
        }
        if (kz_list_push(p, &arr->list, el)) return NULL;
        if (at(p, T_RBRACKET)) break;
        if (expect(p, T_COMMA)) return NULL;
        if (at(p, T_RBRACKET) && el->type == N_SPREAD) arr->ival |= 1;   /* [...a,] : not a valid pattern */
    }
    if (next(p)) return NULL;
    finish(p, arr);
    return arr;
}

/* ------------------------------------------------------------------ primary expressions */

static Node *parse_paren_or_arrow(Parser *p) {
    if (arrow_ahead(p)) return parse_arrow_from_params(p, 0, NULL, NULL);
    Node *start = new_node(p, N_PAREN);
    if (!start) return NULL;
    if (next(p)) return NULL;
    Node *e = parse_expression(p, true);
    if (!e) return NULL;
    if (expect(p, T_RPAREN)) return NULL;
    start->a = e;
    e->flags |= F_PARENS;
    finish(p, start);
    return start;
}

static Node *parse_primary(Parser *p) {
    Node *n;
    JSRuntime *rt = p->ctx->rt;
    switch (TOK.type) {
    case T_NAME:
        if (!TOK.escaped) {
            switch (TOK.kw) {
            case KW_this:
                n = new_node(p, N_THIS);
                if (!n) return NULL;
                {
                    FuncInfo *f = p->fn;
                    while (f && (f->flags & FN_ARROW)) f = f->parent;
                    if (f) f->uses_this = true;
                }
                if (next(p)) return NULL;
                return n;
            case KW_null: n = new_node(p, N_NULL); if (!n || next(p)) return NULL; return n;
            case KW_true: n = new_node(p, N_TRUE); if (!n || next(p)) return NULL; return n;
            case KW_false: n = new_node(p, N_FALSE); if (!n || next(p)) return NULL; return n;
            case KW_function: return parse_function(p, 0, false, false);
            case KW_class: return parse_class(p, false, true);
            case KW_async: {
                bool nl;
                int k;
                int t = peek_type(p, &nl, &k);
                if (t == T_NAME && k == KW_function && !nl) {
                    if (next(p)) return NULL;
                    return parse_function(p, FN_ASYNC, false, false);
                }
                break;
            }
            case KW_new: {
                Node *start = new_node(p, N_NEW);
                if (!start) return NULL;
                if (next(p)) return NULL;
                if (at(p, T_DOT)) {
                    if (next(p)) return NULL;
                    if (!at_kw(p, KW_target)) { unexpected(p); return NULL; }
                    FuncInfo *f = p->fn;
                    while (f && (f->flags & FN_ARROW)) f = f->parent;
                    bool ok = (f && !(f->flags & (FN_SCRIPT | FN_MODULE | FN_EVAL))) || (f && (f->flags & FN_EVAL) && p->eval_new_target_ok) || p->in_class_field;
                    if (!ok) { error_tok(p, "new.target expression is not allowed here"); return NULL; }
                    if (f) f->uses_new_target = true;
                    start->type = N_NEW_TARGET;
                    if (next(p)) return NULL;
                    return start;
                }
                Node *callee;
                if (at_kw(p, KW_new)) callee = parse_primary(p);          /* new new X()() */
                else callee = parse_lhs(p, false);
                if (!callee) return NULL;
                if (callee->type == N_OPT_CHAIN) { error_node(p, callee, "Invalid optional chain from new expression"); return NULL; }
                /* import(x) is a call, not a MemberExpression: neither `new import(x)` nor `new import(x).y` */
                for (Node *base = callee; base; base = base->a) {
                    if (base->type == N_IMPORT_CALL) { error_node(p, callee, "Cannot use new with import"); return NULL; }
                    if (base->type != N_MEMBER && base->type != N_PRIVATE_MEMBER && base->type != N_TAGGED) break;
                }
                start->a = callee;
                if (at(p, T_LPAREN)) {
                    if (next(p)) return NULL;
                    while (!at(p, T_RPAREN)) {
                        Node *arg;
                        if (at(p, T_ELLIPSIS)) {
                            arg = new_node(p, N_SPREAD);
                            if (!arg || next(p)) return NULL;
                            arg->a = parse_assignment(p, true);
                            if (!arg->a) return NULL;
                        } else arg = parse_assignment(p, true);
                        if (!arg || kz_list_push(p, &start->list, arg)) return NULL;
                        if (at(p, T_RPAREN)) break;
                        if (expect(p, T_COMMA)) return NULL;
                    }
                    if (next(p)) return NULL;
                }
                finish(p, start);
                return start;
            }
            case KW_super: {
                n = new_node(p, N_SUPER);
                if (!n || next(p)) return NULL;
                FuncInfo *f = p->fn;
                while (f && (f->flags & FN_ARROW)) f = f->parent;
                if (at(p, T_LPAREN)) {
                    bool ok = (f && (f->flags & FN_DERIVED)) || (f && (f->flags & FN_EVAL) && p->eval_super_call_ok);
                    if (!ok) { error_node(p, n, "'super' keyword unexpected here"); return NULL; }
                    f->uses_super_call = true;
                    f->uses_this = true;
                } else if (at(p, T_DOT) || at(p, T_LBRACKET)) {
                    bool ok = (f && (f->flags & (FN_METHOD | FN_FIELD_INIT | FN_STATIC_BLOCK | FN_CTOR))) ||
                              (f && (f->flags & FN_EVAL) && p->eval_super_prop_ok) || p->in_class_field;
                    if (!ok) { error_node(p, n, "'super' keyword unexpected here"); return NULL; }
                    if (f) { f->uses_super_prop = true; f->uses_this = true; }
                } else { error_node(p, n, "'super' keyword unexpected here"); return NULL; }
                return n;
            }
            case KW_import: {
                n = new_node(p, N_IMPORT_CALL);
                if (!n || next(p)) return NULL;
                if (at(p, T_DOT)) {
                    if (next(p)) return NULL;
                    if (!at_kw(p, KW_meta)) { unexpected(p); return NULL; }
                    if (!p->module) { error_tok(p, "Cannot use 'import.meta' outside a module"); return NULL; }
                    n->type = N_IMPORT_META;
                    if (next(p)) return NULL;
                    return n;
                }
                if (expect(p, T_LPAREN)) return NULL;
                n->a = parse_assignment(p, true);
                if (!n->a) return NULL;
                if (at(p, T_COMMA)) {        /* import(x, options): the options are ignored */
                    if (next(p)) return NULL;
                    if (!at(p, T_RPAREN)) {
                        Node *opt = parse_assignment(p, true);
                        if (!opt) return NULL;
                        n->b = opt;
                        if (at(p, T_COMMA) && next(p)) return NULL;
                    }
                }
                if (expect(p, T_RPAREN)) return NULL;
                finish(p, n);
                return n;
            }
            default:
                break;
            }
        }
        /* an identifier, maybe an arrow's single parameter, or `async x =>` */
        if (at_kw(p, KW_async)) {
            bool nl;
            int k;
            int t = peek_type(p, &nl, &k);
            if (!nl && t == T_NAME) {
                /* async x => ... */
                LexState st;
                save_lex(p, &st);
                if (next(p)) return NULL;
                Token pt = TOK;
                bool nl2;
                if (peek_type(p, &nl2, NULL) == T_ARROW && !nl2) {
                    (void)pt;
                    return parse_arrow_from_params(p, FN_ASYNC, NULL, (Node *)1);
                }
                restore_lex(p, &st);
            } else if (!nl && t == T_LPAREN) {
                LexState st;
                save_lex(p, &st);
                if (next(p)) return NULL;
                if (arrow_ahead(p)) return parse_arrow_from_params(p, FN_ASYNC, NULL, NULL);
                restore_lex(p, &st);
            }
        }
        {
            bool nl;
            if (peek_type(p, &nl, NULL) == T_ARROW && !nl) return parse_arrow_from_params(p, 0, NULL, (Node *)1);
        }
        return ident_ref(p);
    case T_NUMBER:
        if (TOK.legacy_octal && p->strict) { error_tok(p, "Octal literals are not allowed in strict mode"); return NULL; }
        n = new_node(p, N_NUMBER);
        if (!n) return NULL;
        n->num = TOK.num;
        if (next(p)) return NULL;
        return n;
    case T_BIGINT:
        n = new_node(p, N_BIGINT);
        if (!n) return NULL;
        n->val = TOK.raw;
        n->ival = (int32_t)TOK.num;
        if (next(p)) return NULL;
        return n;
    case T_STRING:
        if (TOK.legacy_octal && p->strict) { error_tok(p, "Octal escape sequences are not allowed in strict mode"); return NULL; }
        n = new_node(p, N_STRING);
        if (!n) return NULL;
        n->val = TOK.val;
        if (next(p)) return NULL;
        return n;
    case T_TEMPLATE:
        return parse_template(p, false);
    case T_SLASH: case T_SLASH_ASSIGN: {
        if (kz_lex_regexp(&p->lx)) { p->failed = true; return NULL; }
        n = new_node(p, N_REGEXP);
        if (!n) return NULL;
        n->val = TOK.val;
        n->val2 = TOK.raw;
        char err[512];
        JSString *body = kz_str_flat(p->ctx, JV_STR(TOK.val)), *fl = kz_str_flat(p->ctx, JV_STR(TOK.raw));
        if (!body || !fl) { p->failed = true; return NULL; }
        int rc = kz_regexp_syntax_check(p->ctx, body, fl, err, sizeof err);
        if (rc < 0) { p->failed = true; return NULL; }
        if (rc > 0) { error_tok(p, "%s", err); return NULL; }
        if (next(p)) return NULL;
        return n;
    }
    case T_LBRACKET:
        return parse_array_literal(p);
    case T_LBRACE:
        return parse_object_literal(p);
    case T_LPAREN:
        return parse_paren_or_arrow(p);
    case T_PRIVATE: {
        /* #x in obj */
        n = new_node(p, N_PRIVATE_IN);
        if (!n) return NULL;
        n->val = TOK.val;
        if (next(p)) return NULL;
        if (!at_kw(p, KW_in)) { unexpected(p); return NULL; }
        if (p->nprivate_refs == p->private_refs_cap) {
            int nc = p->private_refs_cap ? p->private_refs_cap * 2 : 16;
            struct PrivateRef *nr = kz_realloc(p->ctx, p->private_refs, (size_t)nc * sizeof(struct PrivateRef));
            if (!nr) { p->failed = true; return NULL; }
            p->private_refs = nr;
            p->private_refs_cap = nc;
        }
        p->private_refs[p->nprivate_refs].name = n->val;
        p->private_refs[p->nprivate_refs].line = n->line;
        p->private_refs[p->nprivate_refs].col = n->col;
        p->private_refs[p->nprivate_refs].class_depth = p->class_depth;
        p->nprivate_refs++;
        (void)rt;
        return n;
    }
    default:
        unexpected(p);
        return NULL;
    }
}

/* ------------------------------------------------------------------ member, call, optional chains */

static int note_private_ref(Parser *p, JSValue name, uint32_t line, uint32_t col) {
    if (p->nprivate_refs == p->private_refs_cap) {
        int nc = p->private_refs_cap ? p->private_refs_cap * 2 : 16;
        struct PrivateRef *nr = kz_realloc(p->ctx, p->private_refs, (size_t)nc * sizeof(struct PrivateRef));
        if (!nr) { p->failed = true; return -1; }
        p->private_refs = nr;
        p->private_refs_cap = nc;
    }
    p->private_refs[p->nprivate_refs].name = name;
    p->private_refs[p->nprivate_refs].line = line;
    p->private_refs[p->nprivate_refs].col = col;
    p->private_refs[p->nprivate_refs].class_depth = p->class_depth;
    p->nprivate_refs++;
    return 0;
}

static int parse_arguments(Parser *p, NodeList *args) {
    if (expect(p, T_LPAREN)) return -1;
    while (!at(p, T_RPAREN)) {
        Node *arg;
        if (at(p, T_ELLIPSIS)) {
            arg = new_node(p, N_SPREAD);
            if (!arg || next(p)) return -1;
            arg->a = parse_assignment(p, true);
            if (!arg->a) return -1;
            finish(p, arg);
        } else {
            arg = parse_assignment(p, true);
            if (!arg) return -1;
        }
        if (kz_list_push(p, args, arg)) return -1;
        if (at(p, T_RPAREN)) break;
        if (expect(p, T_COMMA)) return -1;
    }
    return next(p);
}

static Node *parse_lhs(Parser *p, bool allow_call) {
    DEPTH_ENTER();
    Node *e = parse_primary(p);
    if (!e) { DEPTH_LEAVE(); return NULL; }
    bool in_chain = false;
    JSRuntime *rt = p->ctx->rt;
    for (;;) {
        if (at(p, T_DOT) || at(p, T_QDOT)) {
            bool opt = at(p, T_QDOT);
            if (opt && !allow_call) break;          /* new a?.b is an error reported by the caller */
            if (next(p)) goto fail;
            if (opt) {
                in_chain = true;
                if (at(p, T_LPAREN)) {
                    Node *c = new_node_at(p, N_CALL, e);
                    if (!c) goto fail;
                    c->a = e;
                    c->flags |= F_OPTIONAL;
                    if (parse_arguments(p, &c->list)) goto fail;
                    finish(p, c);
                    e = c;
                    continue;
                }
                if (at(p, T_LBRACKET)) {
                    if (next(p)) goto fail;
                    Node *m = new_node_at(p, N_MEMBER, e);
                    if (!m) goto fail;
                    m->a = e;
                    m->b = parse_expression(p, true);
                    if (!m->b || expect(p, T_RBRACKET)) goto fail;
                    m->flags |= F_COMPUTED | F_OPTIONAL;
                    finish(p, m);
                    e = m;
                    continue;
                }
                if (at(p, T_TEMPLATE)) { error_tok(p, "Invalid tagged template on optional chain"); goto fail; }
            }
            if (at(p, T_PRIVATE)) {
                if (e->type == N_SUPER) { error_tok(p, "Unexpected private field"); goto fail; }
                Node *m = new_node_at(p, N_PRIVATE_MEMBER, e);
                if (!m) goto fail;
                m->a = e;
                m->val = TOK.val;
                if (opt) m->flags |= F_OPTIONAL;
                if (note_private_ref(p, TOK.val, TOK.line, TOK.col)) goto fail;
                if (next(p)) goto fail;
                finish(p, m);
                e = m;
                continue;
            }
            if (!at(p, T_NAME)) { unexpected(p); goto fail; }
            Node *m = new_node_at(p, N_MEMBER, e);
            if (!m) goto fail;
            m->a = e;
            m->val = TOK.val;
            if (opt) m->flags |= F_OPTIONAL;
            if (next(p)) goto fail;
            finish(p, m);
            e = m;
            continue;
        }
        if (at(p, T_LBRACKET)) {
            if (next(p)) goto fail;
            Node *m = new_node_at(p, N_MEMBER, e);
            if (!m) goto fail;
            m->a = e;
            m->b = parse_expression(p, true);
            if (!m->b || expect(p, T_RBRACKET)) goto fail;
            m->flags |= F_COMPUTED;
            finish(p, m);
            e = m;
            continue;
        }
        if (at(p, T_TEMPLATE)) {
            if (in_chain) { error_tok(p, "Invalid tagged template on optional chain"); goto fail; }
            Node *t = new_node_at(p, N_TAGGED, e);
            if (!t) goto fail;
            t->a = e;
            t->b = parse_template(p, true);
            if (!t->b) goto fail;
            finish(p, t);
            e = t;
            continue;
        }
        if (at(p, T_LPAREN) && allow_call) {
            Node *c = new_node_at(p, N_CALL, e);
            if (!c) goto fail;
            c->a = e;
            Node *callee = e;
            if (parse_arguments(p, &c->list)) goto fail;
            if (strip_parens(callee)->type == N_IDENT && strip_parens(callee)->val == rt->atom[KZ_ATOM_eval]) {
                /* a direct eval: everything visible here must stay reachable by name */
                c->flags |= F_DIRECT_EVAL;
                for (Scope *s = p->scope; s; s = s->parent) s->has_eval = true;
                if (p->in_params && p->fn) p->fn->eval_in_params = true;
                for (FuncInfo *f = p->fn; f; f = f->parent) {
                    f->has_direct_eval = true;
                    if (!(f->flags & FN_ARROW)) { f->uses_arguments = true; f->uses_this = true; }
                }
            }
            finish(p, c);
            e = c;
            continue;
        }
        break;
    }
    if (in_chain) {
        Node *oc = new_node_at(p, N_OPT_CHAIN, e);
        if (!oc) goto fail;
        oc->a = e;
        finish(p, oc);
        e = oc;
    }
    if (e->type == N_SUPER) { error_node(p, e, "'super' keyword unexpected here"); goto fail; }
    DEPTH_LEAVE();
    return e;
fail:
    DEPTH_LEAVE();
    return NULL;
}

/* ------------------------------------------------------------------ unary, update, binary */

static Node *parse_update(Parser *p) {
    if (at(p, T_INC) || at(p, T_DEC)) {
        Node *n = new_node(p, N_UPDATE);
        if (!n) return NULL;
        n->op = TOK.type;
        n->flags |= F_PREFIX;
        if (next(p)) return NULL;
        n->a = parse_unary(p);
        if (!n->a) return NULL;
        Node *t = strip_parens(n->a);
        if (!is_valid_simple_target(p, n->a) && !(t->type == N_CALL && !p->strict)) {
            error_node(p, n->a, "Invalid left-hand side expression in prefix operation");
            return NULL;
        }
        finish(p, n);
        return n;
    }
    Node *e = parse_lhs(p, true);
    if (!e) return NULL;
    if ((at(p, T_INC) || at(p, T_DEC)) && !TOK.nl_before) {
        Node *t = strip_parens(e);
        if (!is_valid_simple_target(p, e) && !(t->type == N_CALL && !p->strict)) {
            error_node(p, e, "Invalid left-hand side expression in postfix operation");
            return NULL;
        }
        Node *n = new_node_at(p, N_UPDATE, e);
        if (!n) return NULL;
        n->op = TOK.type;
        n->a = e;
        if (next(p)) return NULL;
        finish(p, n);
        return n;
    }
    return e;
}

static bool in_async_context(Parser *p) {
    if (p->fn->flags & FN_ASYNC) return true;
    if ((p->fn->flags & FN_MODULE) && p->module) return true;
    return false;
}

static Node *parse_unary(Parser *p) {
    DEPTH_ENTER();
    Node *n = NULL;
    int t = TOK.type;
    if (t == T_PLUS || t == T_MINUS || t == T_BANG || t == T_TILDE ||
        (t == T_NAME && !TOK.escaped && (TOK.kw == KW_typeof || TOK.kw == KW_void || TOK.kw == KW_delete))) {
        n = new_node(p, N_UNARY);
        if (!n) goto out;
        n->op = t == T_NAME ? OP_KW + TOK.kw : t;
        if (next(p)) { n = NULL; goto out; }
        n->a = parse_unary(p);
        if (!n->a) { n = NULL; goto out; }
        if (n->op == OP_KW + KW_delete) {
            Node *x = strip_parens(n->a);
            if (p->strict && x->type == N_IDENT) { error_node(p, n, "Delete of an unqualified identifier in strict mode."); n = NULL; goto out; }
            if (x->type == N_PRIVATE_MEMBER) { error_node(p, n, "Private fields can not be deleted"); n = NULL; goto out; }
            if (x->type == N_OPT_CHAIN && x->a->type == N_PRIVATE_MEMBER) { error_node(p, n, "Private fields can not be deleted"); n = NULL; goto out; }
        }
        finish(p, n);
        if (at(p, T_STARSTAR)) { error_tok(p, "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence"); n = NULL; goto out; }
        goto out;
    }
    if (at_kw(p, KW_await) && in_async_context(p)) {
        if (p->in_params) { error_tok(p, "Illegal await-expression in formal parameters of async function"); goto out; }
        n = new_node(p, N_AWAIT);
        if (!n) goto out;
        if (p->lx.p && p->fn) p->fn->contains_await = true;
        if (next(p)) { n = NULL; goto out; }
        n->a = parse_unary(p);
        if (!n->a) { n = NULL; goto out; }
        finish(p, n);
        if (at(p, T_STARSTAR)) { error_tok(p, "Unary operator used immediately before exponentiation expression"); n = NULL; goto out; }
        goto out;
    }
    n = parse_update(p);
out:
    DEPTH_LEAVE();
    return n;
}

static int binary_prec(int t, int kw, bool in) {
    switch (t) {
    case T_QQ: return 1;
    case T_OROR: return 2;
    case T_ANDAND: return 3;
    case T_PIPE: return 4;
    case T_CARET: return 5;
    case T_AMP: return 6;
    case T_EQ: case T_NE: case T_SEQ: case T_SNE: return 7;
    case T_LT: case T_GT: case T_LE: case T_GE: return 8;
    case T_SHL: case T_SAR: case T_SHR: return 9;
    case T_PLUS: case T_MINUS: return 10;
    case T_STAR: case T_SLASH: case T_PERCENT: return 11;
    case T_STARSTAR: return 12;
    case T_NAME:
        if (kw == KW_instanceof) return 8;
        if (kw == KW_in && in) return 8;
        return 0;
    default: return 0;
    }
}

static Node *parse_binary(Parser *p, int min_prec, bool in) {
    DEPTH_ENTER();
    Node *left;
    if (at(p, T_PRIVATE)) {
        /* #x in obj: only directly as the left operand of in */
        left = parse_primary(p);
        if (!left) { DEPTH_LEAVE(); return NULL; }
        if (!in) { unexpected(p); DEPTH_LEAVE(); return NULL; }
        if (next(p)) { DEPTH_LEAVE(); return NULL; }        /* the in */
        left->b = parse_binary(p, 9, in);
        if (!left->b) { DEPTH_LEAVE(); return NULL; }
        finish(p, left);
    } else left = parse_unary(p);
    if (!left) { DEPTH_LEAVE(); return NULL; }
    for (;;) {
        int t = TOK.type, kw = (t == T_NAME && !TOK.escaped) ? TOK.kw : KW_NONE;
        int prec = binary_prec(t, kw, in);
        if (!prec || prec < min_prec) break;
        /* ** is right associative; ?? does not mix with || and && without parentheses */
        Node *n = new_node_at(p, (t == T_ANDAND || t == T_OROR || t == T_QQ) ? N_LOGICAL : N_BINARY, left);
        if (!n) goto fail;
        n->op = t == T_NAME ? OP_KW + kw : t;
        if (next(p)) goto fail;
        Node *right = parse_binary(p, t == T_STARSTAR ? prec : prec + 1, in);
        if (!right) goto fail;
        if (t == T_QQ) {
            if ((right->type == N_LOGICAL && (right->op == T_ANDAND || right->op == T_OROR) && !(right->flags & F_PARENS)) ||
                (left->type == N_LOGICAL && (left->op == T_ANDAND || left->op == T_OROR) && !(left->flags & F_PARENS))) {
                error_node(p, n, "Unexpected token '?" "?'");
                goto fail;
            }
        }
        if ((t == T_ANDAND || t == T_OROR) && ((left->type == N_LOGICAL && left->op == T_QQ && !(left->flags & F_PARENS)))) {
            error_node(p, n, "Unexpected token");
            goto fail;
        }
        n->a = left;
        n->b = right;
        finish(p, n);
        left = n;
    }
    DEPTH_LEAVE();
    return left;
fail:
    DEPTH_LEAVE();
    return NULL;
}

static Node *parse_conditional(Parser *p, bool in) {
    Node *test = parse_binary(p, 1, in);
    if (!test) return NULL;
    if (!at(p, T_QUESTION)) return test;
    Node *n = new_node_at(p, N_COND, test);
    if (!n) return NULL;
    if (next(p)) return NULL;
    n->a = test;
    n->b = parse_assignment(p, true);
    if (!n->b) return NULL;
    if (expect(p, T_COLON)) return NULL;
    n->c = parse_assignment(p, in);
    if (!n->c) return NULL;
    finish(p, n);
    return n;
}

static bool is_assign_op(int t) {
    return t == T_ASSIGN || (t >= T_PLUS_ASSIGN && t <= T_QQ_ASSIGN);
}

static Node *parse_yield(Parser *p, bool in) {
    if (p->in_params) { error_tok(p, "Yield expression not allowed in formal parameter"); return NULL; }
    Node *n = new_node(p, N_YIELD);
    if (!n) return NULL;
    p->fn->has_yield = true;
    if (next(p)) return NULL;
    if (!TOK.nl_before) {
        if (at(p, T_STAR)) {
            n->flags |= F_DELEGATE;
            if (next(p)) return NULL;
            n->a = parse_assignment(p, in);
            if (!n->a) return NULL;
        } else if (!at(p, T_RPAREN) && !at(p, T_RBRACKET) && !at(p, T_RBRACE) && !at(p, T_COMMA) && !at(p, T_SEMI) &&
                   !at(p, T_COLON) && !at(p, T_EOF) && !(at_kw(p, KW_in)) && !at(p, T_QUESTION)) {
            n->a = parse_assignment(p, in);
            if (!n->a) return NULL;
        }
    }
    finish(p, n);
    return n;
}

static Node *parse_assignment(Parser *p, bool in) {
    DEPTH_ENTER();
    Node *r = NULL;
    if (at_kw(p, KW_yield) && (p->fn->flags & FN_GENERATOR)) {
        if (p->in_class_field) { error_tok(p, "Yield expression not allowed in formal parameter"); goto out; }
        r = parse_yield(p, in);
        goto out;
    }
    Node *lhs = parse_conditional(p, in);
    if (!lhs) goto out;
    if (lhs->type == N_ARROW || (lhs->type == N_FUNC && 0)) { r = lhs; goto out; }
    if (is_assign_op(TOK.type)) {
        int op = TOK.type;
        Node *target;
        Node *inner = strip_parens(lhs);
        if (op == T_ASSIGN && (lhs->type == N_OBJECT || lhs->type == N_ARRAY)) {
            target = to_pattern(p, lhs);
            if (!target) goto out;
        } else {
            if (!is_valid_simple_target(p, lhs)) {
                if (inner->type == N_CALL && !p->strict && op != T_ANDAND_ASSIGN && op != T_OROR_ASSIGN && op != T_QQ_ASSIGN) target = lhs;
                else {
                    if (inner->type == N_IDENT) error_node(p, lhs, "Unexpected eval or arguments in strict mode");
                    else error_node(p, lhs, "Invalid left-hand side in assignment");
                    goto out;
                }
            } else target = inner;
        }
        Node *n = new_node_at(p, N_ASSIGN, lhs);
        if (!n) goto out;
        n->op = op;
        if (next(p)) goto out;
        n->a = target;
        n->b = parse_assignment(p, in);
        if (!n->b) goto out;
        finish(p, n);
        r = n;
        goto out;
    }
    {
        Node *ci = find_cover_init(lhs, 0);
        if (ci) { error_node(p, ci, "Invalid shorthand property initializer"); goto out; }
        /* duplicate __proto__ in a literal that stays an expression */
        if (lhs->type == N_OBJECT)
            for (int i = 0; i < lhs->list.n; i++)
                if (lhs->list.v[i]->op == P_PROTO && lhs->list.v[i]->ival) {
                    error_node(p, lhs->list.v[i], "Duplicate __proto__ fields are not allowed in object literals");
                    goto out;
                }
    }
    r = lhs;
out:
    DEPTH_LEAVE();
    return r;
}

static Node *parse_expression(Parser *p, bool in) {
    Node *e = parse_assignment(p, in);
    if (!e || !at(p, T_COMMA)) return e;
    Node *seq = new_node_at(p, N_SEQ, e);
    if (!seq || kz_list_push(p, &seq->list, e)) return NULL;
    while (at(p, T_COMMA)) {
        if (next(p)) return NULL;
        Node *x = parse_assignment(p, in);
        if (!x || kz_list_push(p, &seq->list, x)) return NULL;
    }
    finish(p, seq);
    return seq;
}

/* ------------------------------------------------------------------ binding patterns (declarations, parameters) */

static int declare_name(Parser *p, Node *id, int kind) {
    Binding *b;
    if (kind == BK_VAR) b = declare_var(p, id->val, BK_VAR, id->line, id->col);
    else if (kind == BK_PARAM) {
        /* duplicates are checked by the function parser (they are allowed in sloppy simple lists) */
        b = kz_scope_find(p->scope, id->val);
        if (!b) b = add_binding(p, p->scope, id->val, BK_PARAM);
        else b->flags |= 0;
    } else if (kind == BK_CATCH) b = add_binding(p, p->scope, id->val, BK_CATCH);
    else b = declare_lexical(p, p->scope, id->val, kind, id->line, id->col);
    if (!b) return -1;
    /* (a `var f` after `function f() {}` redeclares the binding but must not take the function declaration away from it) */
    if (kind != BK_PARAM && !(kind == BK_VAR && b->kind == BK_FUNC)) { b->decl_pos = id->pos; b->decl_end = id->end; b->decl = id; }
    id->binding = NULL;
    return 0;
}

static Node *parse_binding_identifier(Parser *p, int kind) {
    if (!at(p, T_NAME)) { unexpected(p); return NULL; }
    if (!check_identifier(p, true)) return NULL;
    if ((kind == BK_LET || kind == BK_CONST || kind == BK_CLASS) && TOK.val == p->ctx->rt->atom[KZ_ATOM_let]) {
        error_tok(p, "let is disallowed as a lexically bound name");
        return NULL;
    }
    Node *id = new_node(p, N_IDENT);
    if (!id) return NULL;
    id->val = TOK.val;
    if (p->fn && TOK.val == p->ctx->rt->atom[KZ_ATOM_arguments] && kind != BK_VAR) p->fn->arguments_shadowed = true;
    if (next(p)) return NULL;
    if (kind && declare_name(p, id, kind)) return NULL;
    return id;
}

static Node *parse_binding_element(Parser *p, int kind) {
    Node *t = parse_binding_target(p, kind);
    if (!t) return NULL;
    if (at(p, T_ASSIGN)) {
        Node *n = new_node_at(p, N_ASSIGN_PAT, t);
        if (!n || next(p)) return NULL;
        n->a = t;
        n->b = parse_assignment(p, true);
        if (!n->b) return NULL;
        finish(p, n);
        return n;
    }
    return t;
}

static Node *parse_binding_target(Parser *p, int kind) {
    DEPTH_ENTER();
    Node *r = NULL;
    if (at(p, T_LBRACKET)) {
        Node *n = new_node(p, N_ARR_PATTERN);
        if (!n || next(p)) goto out;
        while (!at(p, T_RBRACKET)) {
            if (at(p, T_COMMA)) {
                if (kz_list_push(p, &n->list, NULL) || next(p)) goto out;
                continue;
            }
            Node *el;
            if (at(p, T_ELLIPSIS)) {
                el = new_node(p, N_REST);
                if (!el || next(p)) goto out;
                el->a = parse_binding_target(p, kind);
                if (!el->a) goto out;
                if (kz_list_push(p, &n->list, el)) goto out;
                if (!at(p, T_RBRACKET)) { error_tok(p, "Rest element must be last element"); goto out; }
                break;
            }
            el = parse_binding_element(p, kind);
            if (!el || kz_list_push(p, &n->list, el)) goto out;
            if (at(p, T_RBRACKET)) break;
            if (expect(p, T_COMMA)) goto out;
        }
        if (next(p)) goto out;
        finish(p, n);
        r = n;
        goto out;
    }
    if (at(p, T_LBRACE)) {
        Node *n = new_node(p, N_OBJ_PATTERN);
        if (!n || next(p)) goto out;
        while (!at(p, T_RBRACE)) {
            if (at(p, T_ELLIPSIS)) {
                Node *el = new_node(p, N_REST);
                if (!el || next(p)) goto out;
                el->a = parse_binding_identifier(p, kind);
                if (!el->a || kz_list_push(p, &n->list, el)) goto out;
                if (!at(p, T_RBRACE)) { error_tok(p, "Rest element must be last element"); goto out; }
                break;
            }
            Node *pp = new_node(p, N_PAT_PROP);
            if (!pp) goto out;
            Token keytok = TOK;
            if (!parse_property_key(p, pp, false)) goto out;
            if (at(p, T_COLON)) {
                if (next(p)) goto out;
                pp->b = parse_binding_element(p, kind);
                if (!pp->b) goto out;
            } else {
                /* shorthand: the key is the binding name */
                if (keytok.type != T_NAME || (pp->flags & F_COMPUTED)) { unexpected(p); goto out; }
                Lexer saved = p->lx;
                p->lx.tok = keytok;
                bool ok = check_identifier(p, true);
                p->lx = saved;
                if (!ok) goto out;
                Node *id = kz_arena_alloc(p, sizeof(Node));
                if (!id) goto out;
                id->type = N_IDENT;
                id->val = keytok.val;
                id->pos = keytok.pos;
                id->end = keytok.end;
                id->line = keytok.line;
                id->col = keytok.col;
                id->scope = p->scope;
                id->val2 = JS_UNDEFINED;
                if ((kind == BK_LET || kind == BK_CONST) && keytok.val == p->ctx->rt->atom[KZ_ATOM_let]) {
                    error_at(p, keytok.line, keytok.col, "let is disallowed as a lexically bound name");
                    goto out;
                }
                if (kind && declare_name(p, id, kind)) goto out;
                pp->b = id;
                if (at(p, T_ASSIGN)) {
                    Node *as = new_node_at(p, N_ASSIGN_PAT, id);
                    if (!as || next(p)) goto out;
                    as->a = id;
                    as->b = parse_assignment(p, true);
                    if (!as->b) goto out;
                    finish(p, as);
                    pp->b = as;
                }
            }
            finish(p, pp);
            if (kz_list_push(p, &n->list, pp)) goto out;
            if (at(p, T_RBRACE)) break;
            if (expect(p, T_COMMA)) goto out;
        }
        if (next(p)) goto out;
        finish(p, n);
        r = n;
        goto out;
    }
    r = parse_binding_identifier(p, kind);
out:
    DEPTH_LEAVE();
    return r;
}

/* ------------------------------------------------------------------ functions */

static FuncInfo *new_func(Parser *p, uint32_t flags) {
    FuncInfo *fi = kz_arena_alloc(p, sizeof(FuncInfo));
    if (!fi) return NULL;
    fi->flags = flags;
    fi->name = 0;
    fi->parent = p->fn;
    fi->src_start = TOK.pos;
    fi->line = TOK.line;
    fi->col = TOK.col;
    if (p->fn) {
        fi->next_child = p->fn->first_child;
        p->fn->first_child = fi;
    }
    return fi;
}

/* collects the identifiers a parameter pattern binds */
static int bound_names(Parser *p, Node *n, NodeList *out) {
    if (!n) return 0;
    switch (n->type) {
    case N_IDENT: return kz_list_push(p, out, n);
    case N_ASSIGN_PAT: return bound_names(p, n->a, out);
    case N_REST: return bound_names(p, n->a, out);
    case N_ARR_PATTERN:
        for (int i = 0; i < n->list.n; i++) if (bound_names(p, n->list.v[i], out)) return -1;
        return 0;
    case N_OBJ_PATTERN:
        for (int i = 0; i < n->list.n; i++) {
            Node *pp = n->list.v[i];
            if (bound_names(p, pp->type == N_REST ? pp->a : pp->b, out)) return -1;
        }
        return 0;
    default: return 0;
    }
}

/* parameter-list checks that depend on strictness (known only after the body's directives) */
static bool check_params(Parser *p, FuncInfo *fi) {
    NodeList names = { 0 };
    for (int i = 0; i < fi->params.n; i++)
        if (bound_names(p, fi->params.v[i], &names)) return false;
    bool strict = fi->flags & FN_STRICT;
    bool simple = fi->flags & FN_SIMPLE;
    bool no_dups = strict || !simple || (fi->flags & (FN_ARROW | FN_METHOD | FN_CTOR));
    JSRuntime *rt = p->ctx->rt;
    for (int i = 0; i < names.n; i++) {
        Node *a = names.v[i];
        if (strict) {
            if (a->val == rt->atom[KZ_ATOM_eval] || a->val == rt->atom[KZ_ATOM_arguments]) {
                error_node(p, a, "Unexpected eval or arguments in strict mode");
                return false;
            }
            int kw = kz_keyword_of(a->val, rt);
            if (kw && is_strict_reserved(kw)) { error_node(p, a, "Unexpected strict mode reserved word"); return false; }
            if (kw == KW_yield) { error_node(p, a, "Unexpected strict mode reserved word 'yield'"); return false; }
        }
        if (no_dups)
            for (int j = 0; j < i; j++)
                if (names.v[j]->val == a->val) { error_node(p, a, "Duplicate parameter name not allowed in this context"); return false; }
    }
    return true;
}

static int parse_params_inner(Parser *p, FuncInfo *fi);
static int parse_params(Parser *p, FuncInfo *fi) {
    bool saved = p->in_params;
    p->in_params = true;
    int r = parse_params_inner(p, fi);
    p->in_params = saved;
    return r;
}

static int parse_params_inner(Parser *p, FuncInfo *fi) {
    if (expect(p, T_LPAREN)) return -1;
    bool simple = true;
    int len = -1;
    while (!at(p, T_RPAREN)) {
        Node *prm;
        if (at(p, T_ELLIPSIS)) {
            prm = new_node(p, N_REST);
            if (!prm || next(p)) return -1;
            prm->a = parse_binding_target(p, BK_PARAM);
            if (!prm->a) return -1;
            if (at(p, T_ASSIGN)) { error_tok(p, "Rest parameter may not have a default initializer"); return -1; }
            simple = false;
            if (len < 0) len = fi->params.n;
            if (kz_list_push(p, &fi->params, prm)) return -1;
            if (!at(p, T_RPAREN)) { error_tok(p, "Rest parameter must be last formal parameter"); return -1; }
            break;
        }
        prm = parse_binding_element(p, BK_PARAM);
        if (!prm) return -1;
        if (prm->type != N_IDENT) simple = false;
        if (prm->type == N_ASSIGN_PAT && len < 0) len = fi->params.n;
        if (kz_list_push(p, &fi->params, prm)) return -1;
        if (at(p, T_RPAREN)) break;
        if (expect(p, T_COMMA)) return -1;
    }
    if (next(p)) return -1;
    fi->nparams_len = len < 0 ? fi->params.n : len;
    if (simple) fi->flags |= FN_SIMPLE;
    if (fi->flags & FN_GETTER) {
        if (fi->params.n) { error_tok(p, "Getter must not have any formal parameters."); return -1; }
    }
    if (fi->flags & FN_SETTER) {
        if (fi->params.n != 1 || fi->params.v[0]->type == N_REST) { error_tok(p, "Setter must have exactly one formal parameter."); return -1; }
    }
    return 0;
}

/* The function's statements from '{' to '}' (or an arrow's expression). */
static int parse_function_body(Parser *p, FuncInfo *fi) {
    if (!(fi->flags & FN_SIMPLE)) {
        /* parameter expressions get their own scope; the body's vars live in a scope of their own */
        fi->scope->kind = SC_PARAMS;
        Scope *bs = push_scope(p, SC_FUNC);
        if (!bs) return -1;
        fi->body_scope = bs;
    } else fi->body_scope = fi->scope;
    if (expect(p, T_LBRACE)) return -1;
    if (parse_directives_and_body(p, &fi->body, fi, T_RBRACE)) return -1;
    fi->src_end = TOK.end;
    if (next(p)) return -1;
    if (!(fi->flags & FN_SIMPLE)) pop_scope(p);
    return 0;
}

/* Parses the parameter list and body of a function whose first token (function, the name, or the '(') is current.
   kind_flags: FN_ASYNC / FN_GENERATOR / FN_METHOD / FN_GETTER / FN_SETTER / FN_CTOR / FN_DERIVED ... */
static Node *parse_function(Parser *p, int kind_flags, bool is_decl, bool is_export_default) {
    JSRuntime *rt = p->ctx->rt;
    Node *n = new_node(p, is_decl ? N_FUNC_DECL : N_FUNC);
    if (!n) return NULL;
    uint32_t flags = (uint32_t)kind_flags;
    uint32_t start = TOK.pos, line = TOK.line, col = TOK.col;
    JSValue name = 0;
    Node *name_node = NULL;
    bool method = flags & FN_METHOD;
    if (!method) {
        /* the caller consumed `async`: the function's text (Function.prototype.toString, the Function constructors) starts at it */
        if (flags & FN_ASYNC) start = p->lx.prev_pos;
        if (expect(p, T_NAME)) return NULL;          /* 'function' */
        if (at(p, T_STAR)) { flags |= FN_GENERATOR; if (next(p)) return NULL; }
        if (at(p, T_NAME)) {
            /* the name: yield/await rules use the enclosing context for declarations, the function's own for
               expressions */
            uint32_t saved = p->fn->flags;
            if (!is_decl) p->fn->flags = (p->fn->flags & ~(FN_GENERATOR | FN_ASYNC)) | (flags & (FN_GENERATOR | FN_ASYNC));
            bool ok = check_identifier(p, true);
            p->fn->flags = saved;
            if (!ok) return NULL;
            name_node = new_node(p, N_IDENT);
            if (!name_node) return NULL;
            name_node->val = TOK.val;
            name = TOK.val;
            if (next(p)) return NULL;
        } else if (is_decl && !is_export_default) { unexpected(p); return NULL; }
    }
    if (is_decl && name_node) {
        /* declarations bind in the enclosing scope */
        Binding *b;
        /* (at the top level of a module, function declarations are lexical: duplicates and `var f; function f(){}` are errors) */
        bool block_level = !is_var_scope(p->scope) || p->scope->kind == SC_MODULE;
        if (block_level) {
            /* Annex B.3.2.4 lets a sloppy block hold two declarations of one name only when both are plain functions: an async
               function or a generator on either side is a redeclaration */
            Binding *prev = kz_scope_find(p->scope, name);
            if (prev && prev->kind == BK_FUNC && prev->decl && prev->decl->type == N_FUNC_DECL && prev->decl->func &&
                ((prev->decl->func->flags | flags) & (FN_ASYNC | FN_GENERATOR))) {
                redeclared(p, name, name_node->line, name_node->col);
                return NULL;
            }
            b = declare_lexical(p, p->scope, name, BK_FUNC, name_node->line, name_node->col);
        }
        else b = declare_var(p, name, BK_FUNC, name_node->line, name_node->col);
        if (!b) return NULL;
        b->decl = n;
        if (block_level && !p->strict && !(flags & (FN_GENERATOR | FN_ASYNC)) && kz_list_push(p, &p->fn->annexb, n)) return NULL;
    }
    Scope *callee_scope = NULL;
    if (!is_decl && name_node && !method) {
        callee_scope = push_scope(p, SC_CALLEE);
        if (!callee_scope) return NULL;
        Binding *cb = add_binding(p, callee_scope, name, BK_CALLEE);
        if (!cb) return NULL;
        flags |= FN_EXPR_NAMED;
    }
    FuncInfo *fi = new_func(p, flags | (p->strict ? FN_STRICT : 0) | (is_decl ? FN_DECL : 0));
    if (!fi) return NULL;
    fi->name = name;
    fi->src_start = start;
    fi->line = line;
    fi->col = col;
    fi->node = n;
    n->func = fi;
    FuncInfo *saved_fn = p->fn;
    bool saved_strict = p->strict, saved_in_fn = p->in_function, saved_field = p->in_class_field;
    bool saved_params = p->in_params;
    int saved_labels = p->labels_n, saved_loop = p->loop_depth, saved_switch = p->switch_depth;
    int saved_pending = p->label_pending;
    p->fn = fi;
    p->in_function = true;
    p->in_class_field = false;
    p->in_params = false;
    p->label_pending = 0;
    p->labels_n = 0;
    p->loop_depth = 0;
    p->switch_depth = 0;
    fi->scope = push_scope(p, SC_FUNC);
    if (!fi->scope) return NULL;
    if (parse_params(p, fi)) return NULL;
    if (parse_function_body(p, fi)) return NULL;
    pop_scope(p);
    if (callee_scope) pop_scope(p);
    /* names checked under the final strictness */
    if ((fi->flags & FN_STRICT) && name && (name == rt->atom[KZ_ATOM_eval] || name == rt->atom[KZ_ATOM_arguments])) {
        error_node(p, name_node, "Unexpected eval or arguments in strict mode");
        return NULL;
    }
    if ((fi->flags & FN_STRICT) && name) {
        int kw = kz_keyword_of(name, rt);
        if (kw && (is_strict_reserved(kw) || kw == KW_yield)) { error_node(p, name_node, "Unexpected strict mode reserved word"); return NULL; }
    }
    if (!check_params(p, fi)) return NULL;
    p->fn = saved_fn;
    p->strict = saved_strict;
    p->in_function = saved_in_fn;
    p->in_class_field = saved_field;
    p->in_params = saved_params;
    p->label_pending = saved_pending;
    p->labels_n = saved_labels;
    p->loop_depth = saved_loop;
    p->switch_depth = saved_switch;
    finish(p, n);
    n->val = name ? name : JS_UNDEFINED;
    return n;
}

/* Arrow functions: called at the '(' of the parameters (single_param == NULL) or at the single identifier
   (single_param != NULL).  flags may include FN_ASYNC (the 'async' is already consumed). */
static Node *parse_arrow_from_params(Parser *p, uint32_t flags, Node *unused, Node *single_param) {
    (void)unused;
    Node *n = new_node(p, N_ARROW);
    if (!n) return NULL;
    FuncInfo *fi = new_func(p, flags | FN_ARROW | (p->strict ? FN_STRICT : 0));
    if (!fi) return NULL;
    if (flags & FN_ASYNC) fi->src_start = p->lx.prev_pos;      /* (`async` is consumed) */
    fi->node = n;
    n->func = fi;
    FuncInfo *saved_fn = p->fn;
    bool saved_strict = p->strict, saved_params = p->in_params, saved_in_fn = p->in_function;
    int saved_labels = p->labels_n, saved_loop = p->loop_depth, saved_switch = p->switch_depth;
    int saved_pending = p->label_pending;
    p->fn = fi;
    p->in_function = true;                  /* (a `return` in an arrow's block body is legal even in a script's top level) */
    p->in_params = false;
    p->label_pending = 0;
    fi->scope = push_scope(p, SC_FUNC);
    if (!fi->scope) return NULL;
    if (single_param) {
        Node *id = parse_binding_identifier(p, BK_PARAM);
        if (!id || kz_list_push(p, &fi->params, id)) return NULL;
        fi->flags |= FN_SIMPLE;
        fi->nparams_len = 1;
    } else if (parse_params(p, fi)) return NULL;
    if (!at(p, T_ARROW) || TOK.nl_before) { unexpected(p); return NULL; }
    if (next(p)) return NULL;
    p->labels_n = 0;
    p->loop_depth = 0;
    p->switch_depth = 0;
    if (at(p, T_LBRACE)) {
        if (parse_function_body(p, fi)) return NULL;
    } else {
        fi->body_scope = fi->scope;
        if (!(fi->flags & FN_SIMPLE)) {
            fi->scope->kind = SC_PARAMS;
            Scope *bs = push_scope(p, SC_FUNC);
            if (!bs) return NULL;
            fi->body_scope = bs;
        }
        fi->flags |= FN_EXPR_BODY;
        Node *ret = new_node(p, N_RETURN);
        if (!ret) return NULL;
        ret->a = parse_assignment(p, true);
        if (!ret->a) return NULL;
        finish(p, ret);
        if (kz_list_push(p, &fi->body, ret)) return NULL;
        fi->src_end = p->lx.prev_end;
        if (!(fi->flags & FN_SIMPLE)) pop_scope(p);
    }
    pop_scope(p);
    if (!check_params(p, fi)) return NULL;
    p->fn = saved_fn;
    p->strict = saved_strict;
    p->in_function = saved_in_fn;
    p->in_params = saved_params;
    p->label_pending = saved_pending;
    p->labels_n = saved_labels;
    p->loop_depth = saved_loop;
    p->switch_depth = saved_switch;
    finish(p, n);
    return n;
}

/* ------------------------------------------------------------------ directives and statement lists */

static int parse_directives_and_body(Parser *p, NodeList *body, FuncInfo *fi, int end_token) {
    bool prologue = true;
    while (!at(p, end_token)) {
        if (at(p, T_EOF)) { unexpected(p); return -1; }
        if (prologue) {
            if (at(p, T_STRING)) {
                Token t = TOK;
                bool nl;
                int nt = peek_type(p, &nl, NULL);
                bool ends = nt == T_SEMI || nt == T_RBRACE || nt == T_EOF || nl;
                if (ends && (nt == T_SEMI || nt == T_RBRACE || nt == T_EOF || nl)) {
                    /* a directive: is it exactly "use strict" (no escapes)? */
                    bool use_strict = t.end - t.pos == 12 && !memcmp(p->lx.src + t.pos + 1, "use strict", 10);
                    if (use_strict) {
                        if (fi && !(fi->flags & FN_SIMPLE) && !(fi->flags & (FN_SCRIPT | FN_MODULE | FN_EVAL))) {
                            error_tok(p, "Illegal 'use strict' directive in function with non-simple parameter list");
                            return -1;
                        }
                        p->strict = true;
                        if (fi) fi->flags |= FN_STRICT;
                    } else if (t.legacy_octal) {
                        /* an octal escape in an earlier directive becomes an error once strict */
                    }
                } else prologue = false;
            } else prologue = false;
        }
        Node *s = parse_statement_list_item(p);
        if (!s) return -1;
        if (prologue && s->type == N_EXPR_STMT && s->a->type == N_STRING && p->strict) {
            /* recheck earlier directives' octal escapes */
        }
        if (kz_list_push(p, body, s)) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ classes */

static Node *parse_class_element(Parser *p, Node *cls, bool derived, bool *has_ctor) {
    JSRuntime *rt = p->ctx->rt;
    Node *el = new_node(p, N_CLASS_ELEM);
    if (!el) return NULL;
    bool is_static = false, is_async = false, is_gen = false;
    int accessor = 0;
    if (at_kw(p, KW_static)) {
        int t = peek_type(p, NULL, NULL);
        if (t != T_LPAREN && t != T_ASSIGN && t != T_SEMI && t != T_RBRACE) {
            is_static = true;
            if (next(p)) return NULL;
            if (at(p, T_LBRACE)) {
                /* static initialisation block */
                el->op = CE_STATIC_BLOCK;
                el->flags |= F_STATIC;
                Node *fn = new_node(p, N_FUNC);
                if (!fn) return NULL;
                FuncInfo *fi = new_func(p, FN_STATIC_BLOCK | FN_METHOD | FN_STRICT | FN_SIMPLE);
                if (!fi) return NULL;
                fi->node = fn;
                fi->class_node = cls;
                fn->func = fi;
                FuncInfo *saved_fn = p->fn;
                bool saved_field = p->in_class_field;
                int saved_labels = p->labels_n, saved_loop = p->loop_depth, saved_switch = p->switch_depth;
                p->fn = fi;
                p->in_class_field = true;
                p->labels_n = p->loop_depth = p->switch_depth = 0;
                fi->scope = push_scope(p, SC_FUNC);
                if (!fi->scope) return NULL;
                fi->body_scope = fi->scope;
                if (next(p)) return NULL;
                while (!at(p, T_RBRACE)) {
                    Node *s = parse_statement_list_item(p);
                    if (!s || kz_list_push(p, &fi->body, s)) return NULL;
                }
                fi->src_end = TOK.end;
                if (next(p)) return NULL;
                pop_scope(p);
                p->fn = saved_fn;
                p->in_class_field = saved_field;
                p->labels_n = saved_labels;
                p->loop_depth = saved_loop;
                p->switch_depth = saved_switch;
                el->b = fn;
                finish(p, el);
                return el;
            }
        }
    }
    if (is_static) el->flags |= F_STATIC;
    uint32_t elem_start = TOK.pos;      /* the text of a method starts here: its async, *, get or set, not the static before it */
    if (at_kw(p, KW_async)) {
        bool nl;
        int t = peek_type(p, &nl, NULL);
        if (t != T_LPAREN && t != T_ASSIGN && t != T_SEMI && t != T_RBRACE && !nl) {
            is_async = true;
            if (next(p)) return NULL;
        }
    }
    if (at(p, T_STAR)) { is_gen = true; if (next(p)) return NULL; }
    if (!is_async && !is_gen && (at_kw(p, KW_get) || at_kw(p, KW_set))) {
        int t = peek_type(p, NULL, NULL);
        if (t != T_LPAREN && t != T_ASSIGN && t != T_SEMI && t != T_RBRACE) {
            accessor = at_kw(p, KW_get) ? 1 : 2;
            if (next(p)) return NULL;
        }
    }
    Token keytok = TOK;
    if (!parse_property_key(p, el, true)) return NULL;
    Node *key = el->a;
    bool is_private = el->flags & F_PRIVATE_KEY;
    if (is_private && key->val == rt->atom[KZ_ATOM_constructor]) { }
    if (is_private) {
        char buf[80];
        name_text(key->val, buf, sizeof buf);
        if (!strcmp(buf, "constructor")) { error_node(p, key, "Classes may not have a private field named '#constructor'"); return NULL; }
    }
    bool named_ctor = !is_private && !(el->flags & F_COMPUTED) && key->type == N_STRING && key->val == rt->atom[KZ_ATOM_constructor];
    bool named_proto = !is_private && !(el->flags & F_COMPUTED) && key->type == N_STRING && key->val == rt->atom[KZ_ATOM_prototype];
    (void)keytok;
    if (at(p, T_LPAREN) || is_async || is_gen || accessor) {
        uint32_t ff = FN_METHOD | (is_async ? FN_ASYNC : 0) | (is_gen ? FN_GENERATOR : 0) |
                      (accessor == 1 ? FN_GETTER : accessor == 2 ? FN_SETTER : 0);
        if (!is_static && named_ctor) {
            if (accessor || is_gen || is_async) { error_node(p, key, "Class constructor may not be a%s", accessor ? "n accessor" : is_gen ? " generator" : "n async method"); return NULL; }
            if (*has_ctor) { error_node(p, key, "A class may only have one constructor"); return NULL; }
            *has_ctor = true;
            ff |= FN_CTOR | (derived ? FN_DERIVED : 0);
            ff &= ~FN_METHOD;
            el->op = CE_METHOD;
            el->ival = 1;    /* the constructor */
        } else el->op = accessor == 1 ? CE_GET : accessor == 2 ? CE_SET : CE_METHOD;
        if (is_static && named_proto) { error_node(p, key, "Classes may not have a static property named 'prototype'"); return NULL; }
        uint32_t saved_strict_flag = p->strict;
        p->strict = true;
        Node *fn = parse_function(p, (int)(ff | FN_METHOD), false, false);
        p->strict = saved_strict_flag;
        if (!fn) return NULL;
        fn->func->src_start = elem_start;
        if (ff & FN_CTOR) fn->func->flags = (fn->func->flags & ~FN_METHOD) | FN_CTOR | (derived ? FN_DERIVED : 0);
        fn->func->class_node = cls;
        el->b = fn;
        finish(p, el);
        return el;
    }
    /* a field */
    if (named_ctor || (key->type == N_STRING && key->ival == 1 && key->val == rt->atom[KZ_ATOM_constructor] && !(el->flags & F_COMPUTED))) {
        error_node(p, key, "Classes may not have a field named 'constructor'");
        return NULL;
    }
    if (is_static && named_proto) { error_node(p, key, "Classes may not have a static property named 'prototype'"); return NULL; }
    el->op = CE_FIELD;
    if (at(p, T_ASSIGN)) {
        if (next(p)) return NULL;
        /* the initialiser is a function of its own (this = the instance, no arguments) */
        Node *fn = new_node(p, N_FUNC);
        if (!fn) return NULL;
        FuncInfo *fi = new_func(p, FN_FIELD_INIT | FN_METHOD | FN_STRICT | FN_SIMPLE | FN_EXPR_BODY);
        if (!fi) return NULL;
        fi->node = fn;
        fi->class_node = cls;
        fn->func = fi;
        FuncInfo *saved_fn = p->fn;
        bool saved_field = p->in_class_field;
        p->fn = fi;
        p->in_class_field = true;
        fi->scope = push_scope(p, SC_FUNC);
        if (!fi->scope) return NULL;
        fi->body_scope = fi->scope;
        Node *ret = new_node(p, N_RETURN);
        if (!ret) return NULL;
        ret->a = parse_assignment(p, true);
        if (!ret->a) return NULL;
        if (kz_list_push(p, &fi->body, ret)) return NULL;
        fi->src_end = p->lx.prev_end;
        pop_scope(p);
        p->fn = saved_fn;
        p->in_class_field = saved_field;
        el->b = fn;
    }
    if (semicolon(p)) return NULL;
    finish(p, el);
    return el;
}

static Node *parse_class(Parser *p, bool is_decl, bool name_optional) {
    JSRuntime *rt = p->ctx->rt;
    Node *n = new_node(p, N_CLASS);
    if (!n) return NULL;
    if (next(p)) return NULL;           /* 'class' */
    bool saved_strict = p->strict;
    p->strict = true;
    JSValue name = 0;
    Node *name_node = NULL;
    if (at(p, T_NAME) && !at_kw(p, KW_extends)) {
        if (!check_identifier(p, true)) return NULL;
        if (TOK.val == rt->atom[KZ_ATOM_let]) { error_tok(p, "let is disallowed as a lexically bound name"); return NULL; }
        name_node = new_node(p, N_IDENT);
        if (!name_node) return NULL;
        name_node->val = TOK.val;
        name = TOK.val;
        if (next(p)) return NULL;
    } else if (is_decl && !name_optional) { unexpected(p); return NULL; }
    if (is_decl && name) {
        p->strict = saved_strict;
        Binding *b = declare_lexical(p, p->scope, name, BK_CLASS, name_node->line, name_node->col);
        p->strict = true;
        if (!b) return NULL;
        b->decl = n;
    }
    n->val = name ? name : JS_UNDEFINED;
    n->scope = push_scope(p, SC_CLASS);
    if (!n->scope) return NULL;
    if (name) {
        Binding *cb = add_binding(p, n->scope, name, BK_CONST);
        if (!cb) return NULL;
        cb->flags |= BF_TDZ;
    }
    bool derived = false;
    if (at_kw(p, KW_extends)) {
        if (next(p)) return NULL;
        n->a = parse_lhs(p, true);
        if (!n->a) return NULL;
        derived = true;
    }
    if (expect(p, T_LBRACE)) return NULL;
    p->class_depth++;
    int refs_start = p->nprivate_refs;
    NodeList privates = { 0 };
    bool has_ctor = false;
    while (!at(p, T_RBRACE)) {
        if (at(p, T_SEMI)) { if (next(p)) return NULL; continue; }
        Node *el = parse_class_element(p, n, derived, &has_ctor);
        if (!el) return NULL;
        if (el->flags & F_PRIVATE_KEY) {
            /* duplicates: only a getter and a setter (both static or both not) may share a name */
            for (int i = 0; i < privates.n; i++) {
                Node *o = privates.v[i];
                if (o->a->val != el->a->val) continue;
                bool pair = ((o->op == CE_GET && el->op == CE_SET) || (o->op == CE_SET && el->op == CE_GET)) &&
                            ((o->flags & F_STATIC) == (el->flags & F_STATIC)) && !o->ival;
                if (!pair) { error_node(p, el->a, "Identifier '#%s' has already been declared", "?"); return NULL; }
                o->ival = 2;    /* paired */
            }
            if (kz_list_push(p, &privates, el)) return NULL;
        }
        if (kz_list_push(p, &n->list, el)) return NULL;
    }
    if (next(p)) return NULL;
    /* private references made inside this class must name one of its privates (or an outer class's) */
    int w = refs_start;
    for (int i = refs_start; i < p->nprivate_refs; i++) {
        struct PrivateRef *r = &p->private_refs[i];
        bool found = false;
        for (int j = 0; j < privates.n; j++) if (privates.v[j]->a->val == r->name) { found = true; break; }
        if (found) continue;
        if (p->class_depth > 1) { p->private_refs[w++] = *r; continue; }      /* an outer class may declare it */
        bool outer = false;
        for (int j = 0; j < p->nouter_private; j++) if (p->outer_private[j] == r->name) outer = true;
        if (!outer) {
            char buf[80];
            name_text(r->name, buf, sizeof buf);
            error_at(p, r->line, r->col, "Private field '#%s' must be declared in an enclosing class", buf);
            return NULL;
        }
    }
    p->nprivate_refs = w;
    p->class_depth--;
    pop_scope(p);
    p->strict = saved_strict;
    finish(p, n);
    /* the text of a class is the whole class, also when its constructor is written out (Function.prototype.toString) */
    for (int i = 0; i < n->list.n; i++) {
        Node *el = n->list.v[i];
        if (el->op == CE_METHOD && el->ival == 1 && !(el->flags & F_STATIC) && el->b && el->b->func) {
            el->b->func->src_start = n->pos;
            el->b->func->src_end = n->end;
        }
    }
    if (is_decl) {
        Node *d = new_node_at(p, N_CLASS_DECL, n);
        if (!d) return NULL;
        d->a = n;
        d->val = n->val;
        finish(p, d);
        return d;
    }
    return n;
}

/* ------------------------------------------------------------------ statements */

static Node *parse_block(Parser *p, int scope_kind) {
    Node *n = new_node(p, N_BLOCK);
    if (!n) return NULL;
    if (expect(p, T_LBRACE)) return NULL;
    n->scope = push_scope(p, scope_kind);
    if (!n->scope) return NULL;
    while (!at(p, T_RBRACE)) {
        Node *s = parse_statement_list_item(p);
        if (!s || kz_list_push(p, &n->list, s)) return NULL;
    }
    if (next(p)) return NULL;
    pop_scope(p);
    finish(p, n);
    return n;
}

static Node *parse_var_decl(Parser *p, int kind, bool in, bool for_head) {
    Node *n = new_node(p, N_VAR);
    if (!n) return NULL;
    n->op = kind;
    int bk = kind == V_VAR ? BK_VAR : kind == V_LET ? BK_LET : BK_CONST;
    if (next(p)) return NULL;       /* var / let / const */
    for (;;) {
        Node *d = new_node(p, N_DECLARATOR);
        if (!d) return NULL;
        d->a = parse_binding_target(p, bk);
        if (!d->a) return NULL;
        if (at(p, T_ASSIGN)) {
            if (next(p)) return NULL;
            d->b = parse_assignment(p, in);
            if (!d->b) return NULL;
        } else if (!for_head) {
            if (kind == V_CONST) { error_tok(p, "Missing initializer in const declaration"); return NULL; }
            if (d->a->type != N_IDENT) { error_tok(p, "Missing initializer in destructuring declaration"); return NULL; }
        }
        finish(p, d);
        if (kz_list_push(p, &n->list, d)) return NULL;
        if (!at(p, T_COMMA)) break;
        if (next(p)) return NULL;
    }
    finish(p, n);
    return n;
}

static bool let_starts_declaration(Parser *p) {
    /* in a statement list, `let` followed by an identifier, [ or { starts a declaration (even across a line break);
       `let in x`, `let instanceof y` and `let = 1` use let as an identifier */
    bool nl;
    int kw;
    int t = peek_type(p, &nl, &kw);
    if (t == T_LBRACKET || t == T_LBRACE) return true;
    if (t == T_NAME) return kw != KW_in && kw != KW_instanceof;
    return false;
}

static int push_label(Parser *p, JSValue label, bool loop) {
    if (p->labels_n == 64) { error_tok(p, "too many nested labels"); return -1; }
    p->labels[p->labels_n] = label;
    p->label_is_loop[p->labels_n] = loop;
    p->labels_n++;
    return 0;
}

static Node *parse_loop_body(Parser *p) {
    p->loop_depth++;
    Node *b = parse_statement(p, false);
    p->loop_depth--;
    return b;
}

static Node *parse_for(Parser *p) {
    Node *n = new_node(p, N_FOR);
    if (!n || next(p)) return NULL;
    bool is_await = false;
    if (at_kw(p, KW_await)) {
        if (!in_async_context(p)) { unexpected(p); return NULL; }
        is_await = true;
        if (p->fn) p->fn->contains_await = true;        /* (a module with a top-level for await is an async module) */
        if (next(p)) return NULL;
    }
    if (expect(p, T_LPAREN)) return NULL;
    n->scope = push_scope(p, SC_FOR);
    if (!n->scope) return NULL;
    Node *init = NULL;
    bool decl = false;
    int decl_kind = 0;
    if (at(p, T_SEMI)) {
        if (is_await) { unexpected(p); return NULL; }
    } else if (at_kw(p, KW_var)) {
        init = parse_var_decl(p, V_VAR, false, true);
        decl = true;
        decl_kind = V_VAR;
    } else if (at_kw(p, KW_const) || (at_kw(p, KW_let) && let_starts_declaration(p))) {
        int k = at_kw(p, KW_const) ? V_CONST : V_LET;
        init = parse_var_decl(p, k, false, true);
        decl = true;
        decl_kind = k;
    } else {
        bool starts_let = at_kw(p, KW_let);
        bool starts_async = at_kw(p, KW_async) && !TOK.escaped;
        LexState st;
        save_lex(p, &st);
        init = parse_expression(p, false);
        if (init && at_kw(p, KW_of) && starts_let) { error_node(p, init, "The left-hand side of a for-of loop may not be 'let'."); return NULL; }
        if (init && at_kw(p, KW_of) && starts_async && init->type == N_IDENT && !is_await) {
            error_node(p, init, "The left-hand side of a for-of loop may not be 'async'.");
            return NULL;
        }
    }
    if (!init && p->failed) return NULL;
    if ((at_kw(p, KW_of) || at_kw(p, KW_in)) && init) {
        bool of = at_kw(p, KW_of);
        if (!of && is_await) { unexpected(p); return NULL; }
        n->type = of ? N_FOR_OF : N_FOR_IN;
        if (is_await) n->flags |= F_AWAIT;
        if (decl) {
            if (init->list.n != 1) { error_node(p, init, "Invalid left-hand side in for-%s loop: Must have a single binding.", of ? "of" : "in"); return NULL; }
            Node *d = init->list.v[0];
            if (d->b) {
                /* for (var x = 1 in o) is allowed in sloppy mode for a simple var (Annex B.3.5) */
                if (of || p->strict || decl_kind != V_VAR || d->a->type != N_IDENT) {
                    error_node(p, d, "for-%s loop variable declaration may not have an initializer.", of ? "of" : "in");
                    return NULL;
                }
            }
        } else {
            Node *t;
            Node *inner = strip_parens(init);
            if (init->type == N_OBJECT || init->type == N_ARRAY) t = to_pattern(p, init);
            else if (is_valid_simple_target(p, init)) t = inner;
            else if (inner->type == N_CALL && !p->strict) t = init;
            else { error_node(p, init, "Invalid left-hand side in for-%s loop", of ? "of" : "in"); return NULL; }
            if (!t) return NULL;
            init = t;
        }
        if (next(p)) return NULL;
        n->a = init;
        n->b = of ? parse_assignment(p, true) : parse_expression(p, true);
        if (!n->b) return NULL;
        if (expect(p, T_RPAREN)) return NULL;
        /* the head, the right-hand side included, runs with the loop's lexical bindings in their temporal dead zone:
           `for (let x of x)` is a ReferenceError.  A use before the end of the head keeps its TDZ check. */
        if (decl && decl_kind != V_VAR)
            for (Binding *b = n->scope->first; b; b = b->next) b->decl_end = TOK.pos;
        n->d = parse_loop_body(p);
        if (!n->d) return NULL;
        pop_scope(p);
        finish(p, n);
        return n;
    }
    if (init && !decl) {
        Node *ci = find_cover_init(init, 0);
        if (ci) { error_node(p, ci, "Invalid shorthand property initializer"); return NULL; }
    }
    if (decl) {
        for (int i = 0; i < init->list.n; i++) {
            Node *d = init->list.v[i];
            if (!d->b && (decl_kind == V_CONST || d->a->type != N_IDENT)) {
                error_node(p, d, decl_kind == V_CONST ? "Missing initializer in const declaration" : "Missing initializer in destructuring declaration");
                return NULL;
            }
        }
    }
    n->a = init;
    if (expect(p, T_SEMI)) return NULL;
    if (!at(p, T_SEMI)) { n->b = parse_expression(p, true); if (!n->b) return NULL; }
    if (expect(p, T_SEMI)) return NULL;
    if (!at(p, T_RPAREN)) { n->c = parse_expression(p, true); if (!n->c) return NULL; }
    if (expect(p, T_RPAREN)) return NULL;
    n->d = parse_loop_body(p);
    if (!n->d) return NULL;
    pop_scope(p);
    finish(p, n);
    return n;
}

static Node *parse_switch(Parser *p) {
    Node *n = new_node(p, N_SWITCH);
    if (!n || next(p) || expect(p, T_LPAREN)) return NULL;
    n->a = parse_expression(p, true);
    if (!n->a || expect(p, T_RPAREN) || expect(p, T_LBRACE)) return NULL;
    n->scope = push_scope(p, SC_SWITCH);
    if (!n->scope) return NULL;
    bool has_default = false;
    p->switch_depth++;
    while (!at(p, T_RBRACE)) {
        Node *c = new_node(p, N_CASE);
        if (!c) return NULL;
        if (at_kw(p, KW_case)) {
            if (next(p)) return NULL;
            c->a = parse_expression(p, true);
            if (!c->a) return NULL;
        } else if (at_kw(p, KW_default)) {
            if (has_default) { error_tok(p, "More than one default clause in switch statement"); return NULL; }
            has_default = true;
            if (next(p)) return NULL;
        } else { unexpected(p); return NULL; }
        if (expect(p, T_COLON)) return NULL;
        while (!at(p, T_RBRACE) && !at_kw(p, KW_case) && !at_kw(p, KW_default)) {
            Node *s = parse_statement_list_item(p);
            if (!s || kz_list_push(p, &c->list, s)) return NULL;
        }
        finish(p, c);
        if (kz_list_push(p, &n->list, c)) return NULL;
    }
    p->switch_depth--;
    if (next(p)) return NULL;
    pop_scope(p);
    finish(p, n);
    return n;
}

static Node *parse_try(Parser *p) {
    Node *n = new_node(p, N_TRY);
    if (!n || next(p)) return NULL;
    n->a = parse_block(p, SC_BLOCK);
    if (!n->a) return NULL;
    if (at_kw(p, KW_catch)) {
        if (next(p)) return NULL;
        n->scope = push_scope(p, SC_CATCH);
        if (!n->scope) return NULL;
        if (at(p, T_LPAREN)) {
            if (next(p)) return NULL;
            Node *param;
            if (at(p, T_LBRACKET) || at(p, T_LBRACE)) param = parse_binding_target(p, BK_LET);
            else {
                param = parse_binding_identifier(p, 0);
                if (param) {
                    Binding *b = add_binding(p, p->scope, param->val, BK_CATCH);
                    if (!b) return NULL;
                    b->flags |= BF_SIMPLE_CATCH;
                }
            }
            if (!param) return NULL;
            if (param->type != N_IDENT) {
                /* pattern names were declared as let; mark them catch parameters */
                for (Binding *b = p->scope->first; b; b = b->next) b->kind = BK_CATCH;
            }
            n->b = param;
            if (expect(p, T_RPAREN)) return NULL;
        }
        /* the catch block's statements live in the catch scope (so `let e` conflicts with the parameter) */
        Node *blk = new_node(p, N_BLOCK);
        if (!blk) return NULL;
        if (expect(p, T_LBRACE)) return NULL;
        blk->scope = p->scope;
        while (!at(p, T_RBRACE)) {
            Node *s = parse_statement_list_item(p);
            if (!s || kz_list_push(p, &blk->list, s)) return NULL;
        }
        if (next(p)) return NULL;
        finish(p, blk);
        blk->ival = 1;      /* shares the catch scope */
        n->c = blk;
        pop_scope(p);
    }
    if (at_kw(p, KW_finally)) {
        if (next(p)) return NULL;
        n->d = parse_block(p, SC_BLOCK);
        if (!n->d) return NULL;
    }
    if (!n->c && !n->d) { error_tok(p, "Missing catch or finally after try"); return NULL; }
    finish(p, n);
    return n;
}

/* A statement in a position where Annex B lets a sloppy function declaration be the body (if, labels): wrap it in a
   block so it gets the usual block semantics. */
static Node *parse_sub_statement_fn_ok(Parser *p) {
    if (at_kw(p, KW_function) && !p->strict) {
        Node *blk = new_node(p, N_BLOCK);
        if (!blk) return NULL;
        blk->scope = push_scope(p, SC_BLOCK);
        if (!blk->scope) return NULL;
        Node *f = parse_function(p, 0, true, false);
        if (!f) return NULL;
        if (f->func->flags & (FN_GENERATOR | FN_ASYNC)) { error_node(p, f, "Generators can only be declared at the top level or inside a block."); return NULL; }
        if (kz_list_push(p, &blk->list, f)) return NULL;
        pop_scope(p);
        finish(p, blk);
        return blk;
    }
    return parse_statement(p, false);
}

static Node *parse_statement(Parser *p, bool labelled_fn_ok) {
    DEPTH_ENTER();
    Node *n = NULL;
    JSRuntime *rt = p->ctx->rt;
    int saved_labels = p->labels_n;
    int pending = p->label_pending;        /* labels directly on this statement */
    p->label_pending = 0;
    switch (TOK.type) {
    case T_LBRACE:
        n = parse_block(p, SC_BLOCK);
        goto out;
    case T_SEMI:
        n = new_node(p, N_EMPTY);
        if (n && next(p)) n = NULL;
        goto out;
    case T_NAME:
        if (TOK.escaped) break;
        switch (TOK.kw) {
        case KW_var:
            n = parse_var_decl(p, V_VAR, true, false);
            if (n && semicolon(p)) n = NULL;
            goto out;
        case KW_if: {
            n = new_node(p, N_IF);
            if (!n || next(p) || expect(p, T_LPAREN)) { n = NULL; goto out; }
            n->a = parse_expression(p, true);
            if (!n->a || expect(p, T_RPAREN)) { n = NULL; goto out; }
            n->b = parse_sub_statement_fn_ok(p);
            if (!n->b) { n = NULL; goto out; }
            if (at_kw(p, KW_else)) {
                if (next(p)) { n = NULL; goto out; }
                n->c = parse_sub_statement_fn_ok(p);
                if (!n->c) { n = NULL; goto out; }
            }
            finish(p, n);
            goto out;
        }
        case KW_for:
            for (int i = p->labels_n - pending; i < p->labels_n; i++) p->label_is_loop[i] = true;
            n = parse_for(p);
            goto out;
        case KW_while: {
            for (int i = p->labels_n - pending; i < p->labels_n; i++) p->label_is_loop[i] = true;
            n = new_node(p, N_WHILE);
            if (!n || next(p) || expect(p, T_LPAREN)) { n = NULL; goto out; }
            n->a = parse_expression(p, true);
            if (!n->a || expect(p, T_RPAREN)) { n = NULL; goto out; }
            n->d = parse_loop_body(p);
            if (!n->d) { n = NULL; goto out; }
            finish(p, n);
            goto out;
        }
        case KW_do: {
            for (int i = p->labels_n - pending; i < p->labels_n; i++) p->label_is_loop[i] = true;
            n = new_node(p, N_DO_WHILE);
            if (!n || next(p)) { n = NULL; goto out; }
            n->d = parse_loop_body(p);
            if (!n->d) { n = NULL; goto out; }
            if (!at_kw(p, KW_while)) { unexpected(p); n = NULL; goto out; }
            if (next(p) || expect(p, T_LPAREN)) { n = NULL; goto out; }
            n->a = parse_expression(p, true);
            if (!n->a || expect(p, T_RPAREN)) { n = NULL; goto out; }
            if (at(p, T_SEMI) && next(p)) { n = NULL; goto out; }      /* ASI after do-while always applies */
            finish(p, n);
            goto out;
        }
        case KW_continue: case KW_break: {
            bool is_break = TOK.kw == KW_break;
            n = new_node(p, is_break ? N_BREAK : N_CONTINUE);
            if (!n || next(p)) { n = NULL; goto out; }
            n->val = 0;
            if (at(p, T_NAME) && !TOK.nl_before && !(TOK.kw && TOK.kw < KW_FIRST_STRICT && !TOK.escaped && TOK.kw != KW_yield && TOK.kw != KW_await)) {
                JSValue label = TOK.val;
                int found = -1;
                for (int i = p->labels_n - 1; i >= 0; i--) if (p->labels[i] == label) { found = i; break; }
                if (found < 0) { char b[80]; name_text(label, b, sizeof b); error_tok(p, "Undefined label '%s'", b); n = NULL; goto out; }
                if (!is_break && !p->label_is_loop[found]) { char b[80]; name_text(label, b, sizeof b); error_tok(p, "Illegal continue statement: '%s' does not denote an iteration statement", b); n = NULL; goto out; }
                n->val = label;
                if (next(p)) { n = NULL; goto out; }
            } else {
                if (is_break && !p->loop_depth && !p->switch_depth) { error_node(p, n, "Illegal break statement"); n = NULL; goto out; }
                if (!is_break && !p->loop_depth) { error_node(p, n, "Illegal continue statement: no surrounding iteration statement"); n = NULL; goto out; }
            }
            if (semicolon(p)) { n = NULL; goto out; }
            finish(p, n);
            goto out;
        }
        case KW_return: {
            if (!p->in_function || (p->fn->flags & (FN_FIELD_INIT | FN_STATIC_BLOCK))) { error_tok(p, "Illegal return statement"); goto out; }
            n = new_node(p, N_RETURN);
            if (!n || next(p)) { n = NULL; goto out; }
            if (!at(p, T_SEMI) && !at(p, T_RBRACE) && !at(p, T_EOF) && !TOK.nl_before) {
                n->a = parse_expression(p, true);
                if (!n->a) { n = NULL; goto out; }
            }
            if (semicolon(p)) { n = NULL; goto out; }
            finish(p, n);
            goto out;
        }
        case KW_with: {
            if (p->strict) { error_tok(p, "Strict mode code may not include a with statement"); goto out; }
            n = new_node(p, N_WITH);
            if (!n || next(p) || expect(p, T_LPAREN)) { n = NULL; goto out; }
            n->a = parse_expression(p, true);
            if (!n->a || expect(p, T_RPAREN)) { n = NULL; goto out; }
            n->scope = push_scope(p, SC_WITH);
            if (!n->scope) { n = NULL; goto out; }
            n->scope->with_obj = kz_declare_hidden(p, n->scope, JV_MKINT(0));
            if (!n->scope->with_obj) { n = NULL; goto out; }
            n->b = parse_statement(p, false);
            if (!n->b) { n = NULL; goto out; }
            pop_scope(p);
            finish(p, n);
            goto out;
        }
        case KW_switch:
            n = parse_switch(p);
            goto out;
        case KW_throw: {
            n = new_node(p, N_THROW);
            if (!n || next(p)) { n = NULL; goto out; }
            if (TOK.nl_before) { error_tok(p, "Illegal newline after throw"); n = NULL; goto out; }
            n->a = parse_expression(p, true);
            if (!n->a || semicolon(p)) { n = NULL; goto out; }
            finish(p, n);
            goto out;
        }
        case KW_try:
            n = parse_try(p);
            goto out;
        case KW_debugger:
            n = new_node(p, N_DEBUGGER);
            if (!n || next(p) || semicolon(p)) { n = NULL; goto out; }
            goto out;
        case KW_function:
            /* only reachable in statement position: an error except via the Annex B paths */
            error_tok(p, p->strict ? "In strict mode code, functions can only be declared at top level or inside a block." : "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement.");
            goto out;
        case KW_class: case KW_const:
            error_tok(p, "Lexical declaration cannot appear in a single-statement context");
            goto out;
        case KW_let: {
            bool nl;
            int kw;
            int t = peek_type(p, &nl, &kw);
            if (t == T_LBRACKET && !TOK.escaped) { error_tok(p, "Lexical declaration cannot appear in a single-statement context"); goto out; }
            (void)kw;
            break;
        }
        case KW_import: {
            int t = peek_type(p, NULL, NULL);
            if (t != T_LPAREN && t != T_DOT) { error_tok(p, "Cannot use import statement outside a module"); goto out; }
            break;
        }
        case KW_export:
            error_tok(p, "Unexpected token 'export'");
            goto out;
        default:
            break;
        }
        /* a label? */
        if (TOK.kw == KW_NONE || TOK.kw >= KW_FIRST_STRICT || TOK.kw == KW_yield || TOK.kw == KW_await) {
            bool nl;
            if (peek_type(p, &nl, NULL) == T_COLON) {
                if (!check_identifier(p, false)) goto out;
                JSValue label = TOK.val;
                for (int i = 0; i < p->labels_n; i++)
                    if (p->labels[i] == label) { char b[80]; name_text(label, b, sizeof b); error_tok(p, "Label '%s' has already been declared", b); goto out; }
                n = new_node(p, N_LABELED);
                if (!n) goto out;
                n->val = label;
                if (next(p) || next(p)) { n = NULL; goto out; }
                if (push_label(p, label, false)) { n = NULL; goto out; }
                /* a function declaration after labels: sloppy mode only, and not a generator/async */
                if (at_kw(p, KW_function)) {
                    if (p->strict) { error_tok(p, "In strict mode code, functions can only be declared at top level or inside a block."); n = NULL; goto out; }
                    n->a = parse_function(p, 0, true, false);
                    if (n->a && (n->a->func->flags & (FN_GENERATOR | FN_ASYNC))) { error_node(p, n->a, "Generators can only be declared at the top level or inside a block."); n = NULL; goto out; }
                } else {
                    p->label_pending = pending + 1;
                    n->a = parse_statement(p, true);
                }
                if (!n->a) { n = NULL; goto out; }
                finish(p, n);
                goto out;
            }
        }
        break;
    default:
        break;
    }
    /* an expression statement */
    {
        if (at_kw(p, KW_async)) {
            bool nl;
            int kw;
            if (peek_type(p, &nl, &kw) == T_NAME && kw == KW_function && !nl) {
                error_tok(p, "Async functions can only be declared at the top level or inside a block.");
                goto out;
            }
        }
        n = new_node(p, N_EXPR_STMT);
        if (!n) goto out;
        n->a = parse_expression(p, true);
        if (!n->a || semicolon(p)) { n = NULL; goto out; }
        finish(p, n);
    }
    (void)labelled_fn_ok;
    (void)rt;
out:
    p->labels_n = saved_labels;
    DEPTH_LEAVE();
    return n;
}

/* ------------------------------------------------------------------ modules */

static Node *module_specifier(Parser *p, Node *n) {
    if (!at(p, T_STRING)) { unexpected(p); return NULL; }
    n->val = TOK.val;
    if (next(p)) return NULL;
    /* import attributes: with { type: "json" } (accepted and ignored) */
    if ((at_kw(p, KW_with) || (at(p, T_NAME) && TOK.val == p->ctx->rt->atom[KZ_ATOM_as])) && !TOK.nl_before) {
        if (next(p)) return NULL;
        if (expect(p, T_LBRACE)) return NULL;
        while (!at(p, T_RBRACE)) {
            if (next(p)) return NULL;
            if (expect(p, T_COLON)) return NULL;
            if (!at(p, T_STRING)) { unexpected(p); return NULL; }
            if (next(p)) return NULL;
            if (at(p, T_COMMA) && next(p)) return NULL;
        }
        if (next(p)) return NULL;
    }
    return n;
}

static bool module_export_name(Parser *p, JSValue *out) {
    if (at(p, T_NAME)) { *out = TOK.val; return next(p) == 0; }
    if (at(p, T_STRING)) {
        /* must be well-formed Unicode */
        JSString *s = JV_STR(TOK.val);
        s = kz_str_flat(p->ctx, s);
        if (!s) { p->failed = true; return false; }
        for (uint32_t i = 0; i < kz_str_len(s); i++) {
            uint32_t c = kz_str_at(s, i);
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (i + 1 < kz_str_len(s) && kz_str_at(s, i + 1) >= 0xDC00 && kz_str_at(s, i + 1) <= 0xDFFF) { i++; continue; }
                error_tok(p, "Invalid module export name: contains unpaired surrogate");
                return false;
            }
            if (c >= 0xDC00 && c <= 0xDFFF) { error_tok(p, "Invalid module export name: contains unpaired surrogate"); return false; }
        }
        JSValue a = kz_atom_from_str(p->ctx, TOK.val);
        if (a == JS_EXCEPTION || kz_parser_keep(p, a) == JS_EXCEPTION) { p->failed = true; return false; }
        *out = a;
        return next(p) == 0;
    }
    unexpected(p);
    return false;
}

static Node *parse_import(Parser *p) {
    JSRuntime *rt = p->ctx->rt;
    Node *n = new_node(p, N_IMPORT);
    if (!n || next(p)) return NULL;
    if (at(p, T_STRING)) {
        if (!module_specifier(p, n) || semicolon(p)) return NULL;
        finish(p, n);
        return n;
    }
    /* a default binding: import x from "m" */
    if (at(p, T_NAME) && !(TOK.kw && TOK.kw < KW_FIRST_STRICT && !TOK.escaped && TOK.kw != KW_await && TOK.kw != KW_yield)) {
        Node *spec = new_node(p, N_IMPORT_SPEC);
        if (!spec) return NULL;
        spec->val = rt->atom[KZ_ATOM_default];
        Node *id = parse_binding_identifier(p, 0);
        if (!id) return NULL;
        Binding *b = declare_lexical(p, p->scope, id->val, BK_IMPORT, id->line, id->col);
        if (!b) return NULL;
        spec->val2 = id->val;
        spec->a = id;
        if (kz_list_push(p, &n->list, spec)) return NULL;
        if (!at(p, T_COMMA)) goto from;
        if (next(p)) return NULL;
    }
    if (at(p, T_STAR)) {
        Node *spec = new_node(p, N_IMPORT_SPEC);
        if (!spec || next(p)) return NULL;
        if (!(at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_as] && !TOK.escaped)) { unexpected(p); return NULL; }
        if (next(p)) return NULL;
        spec->val = rt->atom[KZ_ATOM_star];
        Node *id = parse_binding_identifier(p, 0);
        if (!id) return NULL;
        Binding *b = declare_lexical(p, p->scope, id->val, BK_IMPORT, id->line, id->col);
        if (!b) return NULL;
        spec->val2 = id->val;
        spec->a = id;
        if (kz_list_push(p, &n->list, spec)) return NULL;
    } else if (at(p, T_LBRACE)) {
        if (next(p)) return NULL;
        while (!at(p, T_RBRACE)) {
            Node *spec = new_node(p, N_IMPORT_SPEC);
            if (!spec) return NULL;
            bool was_string = at(p, T_STRING);
            Token nt = TOK;
            JSValue imported;
            if (!module_export_name(p, &imported)) return NULL;
            spec->val = imported;
            if (at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_as] && !TOK.escaped) {
                if (next(p)) return NULL;
                Node *id = parse_binding_identifier(p, 0);
                if (!id) return NULL;
                spec->val2 = id->val;
                spec->a = id;
            } else {
                if (was_string) { unexpected(p); return NULL; }
                Lexer saved = p->lx;
                p->lx.tok = nt;
                bool ok = check_identifier(p, true);
                p->lx = saved;
                if (!ok) return NULL;
                if (nt.kw && nt.kw < KW_FIRST_STRICT) { error_at(p, nt.line, nt.col, "Unexpected reserved word"); return NULL; }
                Node *id = kz_arena_alloc(p, sizeof(Node));
                if (!id) return NULL;
                id->type = N_IDENT;
                id->val = imported;
                id->line = nt.line;
                id->col = nt.col;
                spec->val2 = imported;
                spec->a = id;
            }
            Binding *b = declare_lexical(p, p->scope, spec->val2, BK_IMPORT, spec->a->line, spec->a->col);
            if (!b) return NULL;
            if (kz_list_push(p, &n->list, spec)) return NULL;
            if (at(p, T_RBRACE)) break;
            if (expect(p, T_COMMA)) return NULL;
        }
        if (next(p)) return NULL;
    } else if (!n->list.n) { unexpected(p); return NULL; }
from:
    if (!(at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_from] && !TOK.escaped)) { unexpected(p); return NULL; }
    if (next(p)) return NULL;
    if (!module_specifier(p, n) || semicolon(p)) return NULL;
    finish(p, n);
    return n;
}

static Node *parse_export(Parser *p) {
    JSRuntime *rt = p->ctx->rt;
    Node *n = new_node(p, N_EXPORT);
    if (!n || next(p)) return NULL;
    if (at_kw(p, KW_default)) {
        if (next(p)) return NULL;
        if (at_kw(p, KW_function) || (at_kw(p, KW_async) && peek_type(p, NULL, NULL) == T_NAME)) {
            bool is_async = at_kw(p, KW_async);
            if (is_async) {
                bool nl;
                int kw;
                if (peek_type(p, &nl, &kw) == T_NAME && kw == KW_function && !nl) {
                    if (next(p)) return NULL;
                } else goto default_expr;
            }
            n->op = EX_DEFAULT_DECL;
            n->a = parse_function(p, is_async ? FN_ASYNC : 0, true, true);
            if (!n->a) return NULL;
            if (n->a->val == JS_UNDEFINED) {
                /* export default function () {}: bound as *default* */
                Binding *b = declare_var(p, rt->atom[KZ_ATOM_star_default_star], BK_FUNC, n->line, n->col);
                if (!b) return NULL;
                b->decl = n->a;
                n->a->val = rt->atom[KZ_ATOM_star_default_star];
                n->a->func->name = rt->atom[KZ_ATOM_default];
            }
            finish(p, n);
            return n;
        }
        if (at_kw(p, KW_class)) {
            n->op = EX_DEFAULT_DECL;
            n->a = parse_class(p, true, true);
            if (!n->a) return NULL;
            if (n->a->val == JS_UNDEFINED) {
                Binding *b = declare_lexical(p, p->scope, rt->atom[KZ_ATOM_star_default_star], BK_CLASS, n->line, n->col);
                if (!b) return NULL;
                n->a->val = rt->atom[KZ_ATOM_star_default_star];
            }
            finish(p, n);
            return n;
        }
    default_expr:
        n->op = EX_DEFAULT_EXPR;
        n->a = parse_assignment(p, true);
        if (!n->a || semicolon(p)) return NULL;
        Binding *b = declare_lexical(p, p->scope, rt->atom[KZ_ATOM_star_default_star], BK_CONST, n->line, n->col);
        if (!b) return NULL;
        finish(p, n);
        return n;
    }
    if (at(p, T_STAR)) {
        if (next(p)) return NULL;
        n->op = EX_ALL;
        if (at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_as] && !TOK.escaped) {
            if (next(p)) return NULL;
            Node *spec = new_node(p, N_EXPORT_SPEC);
            if (!spec) return NULL;
            JSValue name;
            if (!module_export_name(p, &name)) return NULL;
            spec->val = rt->atom[KZ_ATOM_star];
            spec->val2 = name;
            if (kz_list_push(p, &n->list, spec)) return NULL;
            n->op = EX_FROM;
        }
        if (!(at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_from] && !TOK.escaped)) { unexpected(p); return NULL; }
        if (next(p)) return NULL;
        if (!module_specifier(p, n) || semicolon(p)) return NULL;
        finish(p, n);
        return n;
    }
    if (at(p, T_LBRACE)) {
        if (next(p)) return NULL;
        bool any_string = false;
        while (!at(p, T_RBRACE)) {
            Node *spec = new_node(p, N_EXPORT_SPEC);
            if (!spec) return NULL;
            if (at(p, T_STRING)) any_string = true;
            Token lt = TOK;
            JSValue local;
            if (!module_export_name(p, &local)) return NULL;
            spec->val = local;
            spec->val2 = local;
            spec->line = lt.line;
            spec->col = lt.col;
            spec->ival = lt.type == T_NAME && lt.kw && lt.kw < KW_FIRST_STRICT && !lt.escaped;   /* reserved word as a local */
            if (at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_as] && !TOK.escaped) {
                if (next(p)) return NULL;
                JSValue exported;
                if (!module_export_name(p, &exported)) return NULL;
                spec->val2 = exported;
            }
            if (kz_list_push(p, &n->list, spec)) return NULL;
            if (at(p, T_RBRACE)) break;
            if (expect(p, T_COMMA)) return NULL;
        }
        if (next(p)) return NULL;
        if (at(p, T_NAME) && TOK.val == rt->atom[KZ_ATOM_from] && !TOK.escaped) {
            if (next(p)) return NULL;
            n->op = EX_FROM;
            if (!module_specifier(p, n)) return NULL;
        } else {
            n->op = EX_NAMED;
            if (any_string) { error_node(p, n, "A string export name requires a 'from' clause"); return NULL; }
            for (int i = 0; i < n->list.n; i++)
                if (n->list.v[i]->ival) { error_node(p, n->list.v[i], "Unexpected reserved word"); return NULL; }
        }
        if (semicolon(p)) return NULL;
        finish(p, n);
        return n;
    }
    n->op = EX_DECL;
    if (at_kw(p, KW_var)) { n->a = parse_var_decl(p, V_VAR, true, false); if (!n->a || semicolon(p)) return NULL; }
    else if (at_kw(p, KW_let) || at_kw(p, KW_const)) { n->a = parse_var_decl(p, at_kw(p, KW_let) ? V_LET : V_CONST, true, false); if (!n->a || semicolon(p)) return NULL; }
    else if (at_kw(p, KW_function)) n->a = parse_function(p, 0, true, false);
    else if (at_kw(p, KW_async)) { if (next(p)) return NULL; n->a = parse_function(p, FN_ASYNC, true, false); }
    else if (at_kw(p, KW_class)) n->a = parse_class(p, true, false);
    else { unexpected(p); return NULL; }
    if (!n->a) return NULL;
    finish(p, n);
    return n;
}

/* ------------------------------------------------------------------ statement list items (declarations) */

static Node *parse_statement_list_item(Parser *p) {
    if (at(p, T_NAME) && !TOK.escaped) {
        switch (TOK.kw) {
        case KW_function: return parse_function(p, 0, true, false);
        case KW_class: return parse_class(p, true, false);
        case KW_const: {
            Node *n = parse_var_decl(p, V_CONST, true, false);
            if (n && semicolon(p)) return NULL;
            return n;
        }
        case KW_let:
            if (let_starts_declaration(p)) {
                Node *n = parse_var_decl(p, V_LET, true, false);
                if (n && semicolon(p)) return NULL;
                return n;
            }
            break;
        case KW_async: {
            bool nl;
            int kw;
            if (peek_type(p, &nl, &kw) == T_NAME && kw == KW_function && !nl) {
                if (next(p)) return NULL;
                return parse_function(p, FN_ASYNC, true, false);
            }
            break;
        }
        case KW_import:
            if (p->module && (p->fn->flags & FN_MODULE) && p->scope->kind == SC_MODULE) {
                int t = peek_type(p, NULL, NULL);
                if (t != T_LPAREN && t != T_DOT) return parse_import(p);
            }
            break;
        case KW_export:
            if (p->module && p->scope->kind == SC_MODULE) return parse_export(p);
            break;
        default:
            break;
        }
    }
    return parse_statement(p, false);
}

/* ------------------------------------------------------------------ the program */

Node *kz_parse_program(Parser *p) {
    Node *prog = kz_arena_alloc(p, sizeof(Node));
    if (!prog) return NULL;
    prog->type = N_PROGRAM;
    prog->val = prog->val2 = JS_UNDEFINED;
    uint32_t kind = p->module ? FN_MODULE : p->eval_kind ? FN_EVAL : FN_SCRIPT;
    FuncInfo *fi = new_func(p, kind | (p->strict || p->module ? FN_STRICT : 0) | FN_SIMPLE);
    if (!fi) return NULL;
    p->fn = fi;
    prog->func = fi;
    fi->node = prog;
    if (p->module) p->strict = true;
    if (p->module) p->lx.html_comments = false;
    p->in_function = p->eval_kind && p->eval_in_function;
    p->in_class_field = p->eval_in_class_field;
    fi->scope = push_scope(p, p->module ? SC_MODULE : p->eval_kind ? SC_EVAL : SC_SCRIPT);
    if (!fi->scope) return NULL;
    fi->body_scope = fi->scope;
    prog->scope = fi->scope;
    if (next(p)) return NULL;
    if (parse_directives_and_body(p, &prog->list, fi, T_EOF)) return NULL;
    fi->body = prog->list;          /* the analysis walks the function's body */
    if (p->strict) fi->flags |= FN_STRICT;
    /* module: export names must refer to declared bindings (checked by the compiler) */
    fi->src_end = p->lx.len;
    pop_scope(p);
    if (p->nprivate_refs) {
        struct PrivateRef *r = &p->private_refs[0];
        bool outer = false;
        for (int j = 0; j < p->nouter_private; j++) if (p->outer_private[j] == r->name) outer = true;
        if (!outer) {
            char buf[80];
            name_text(r->name, buf, sizeof buf);
            error_at(p, r->line, r->col, "Private field '#%s' must be declared in an enclosing class", buf);
            return NULL;
        }
    }
    return prog;
}
