/* parse.inc -- the C parser, part 1: scopes, declarations, types, struct layout, initializers.
 * The design follows chibicc (recursive descent, AST with types, local initializers lowered to assignments).
 */

typedef struct VarScope VarScope;
struct VarScope {
    Obj *var;
    Type *type_def;
    Type *enum_ty;
    int enum_val;
};

typedef struct Scope Scope;
struct Scope {
    Scope *next;
    Map vars;
    Map tags;
    int frame_mark; /* frame_cur at the start of the scope: locals of sibling blocks share stack space */
};

typedef struct {
    bool is_typedef;
    bool is_static;
    bool is_extern;
    bool is_inline;
    bool is_tls;
    bool is_weak;
    bool is_noreturn;
    int align;
} VarAttr;

typedef struct {
    bool packed;
    bool weak;
    bool used;
    int align;
} AttrSet;

typedef struct Initializer Initializer;
struct Initializer {
    Initializer *next;
    Type *ty;
    Token *tok;
    bool is_flexible;
    Node *expr;
    Initializer **children;
    Member *mem;
};

typedef struct InitDesg InitDesg;
struct InitDesg {
    InitDesg *next;
    int idx;
    Member *member;
    Obj *var;
};

static Obj *locals;
static Obj *globals;
static Scope global_scope;
static Scope *scope = &global_scope;
static Obj *current_fn;
static Node *gotos;
static Node *labels;
static int cur_brk_label;
static int cur_cont_label;
static Node *current_switch;
static int label_counter;
static int unique_counter;
static bool in_param_decl;
static Type *va_list_type;
static int frame_cur, frame_max; /* bytes of the stack frame in use / the most ever in use */
static bool stmt_pinned;        /* a compound literal lives in the current statement: its slot must outlive it */

static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *typename(Token **rest, Token *tok);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static void initializer2(Token **rest, Token *tok, Initializer *init);
static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty);
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var);
static void gvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static i64 eval(Node *node);
static i64 eval2(Node *node, Obj **label);
static i64 eval_rval(Node *node, Obj **label);
static bool is_const_expr(Node *node);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static double eval_double(Node *node);
static Node *cast(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Token *parse_typedef(Token *tok, Type *basety);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, VarAttr *attr);
static bool is_typename(Token *tok);

static int align_to(int n, int align) {
    return (n + align - 1) / align * align;
}

static int align_down(int n, int align) {
    return align_to(n - align + 1, align);
}

static int next_label(void) {
    return ++label_counter;
}

/* ---- scopes */

static void enter_scope(void) {
    Scope *sc = xcalloc(sizeof(Scope));
    sc->next = scope;
    sc->frame_mark = frame_cur;
    scope = sc;
}

static void leave_scope(void) {
    frame_cur = scope->frame_mark;
    scope = scope->next;
}

static VarScope *find_var(Token *tok) {
    for (Scope *sc = scope; sc; sc = sc->next) {
        VarScope *sc2 = map_get2(&sc->vars, tok->loc, tok->len);
        if (sc2) return sc2;
    }
    return NULL;
}

static Type *find_tag(Token *tok) {
    for (Scope *sc = scope; sc; sc = sc->next) {
        Type *ty = map_get2(&sc->tags, tok->loc, tok->len);
        if (ty) return ty;
    }
    return NULL;
}

static VarScope *push_scope(char *name) {
    VarScope *sc = xcalloc(sizeof(VarScope));
    map_put(&scope->vars, name, sc);
    return sc;
}

static void push_tag_scope(Token *tok, Type *ty) {
    map_put2(&scope->tags, tok->loc, tok->len, ty);
}

static char *get_ident(Token *tok) {
    if (tok->kind != TK_IDENT) error_tok(tok, "expected an identifier");
    return xstrndup(tok->loc, (size_t)tok->len);
}

static char *new_unique_name(void) {
    return xformat(".L..%d", unique_counter++);
}

static Type *find_typedef(Token *tok) {
    if (tok->kind == TK_IDENT) {
        VarScope *sc = find_var(tok);
        if (sc) return sc->type_def;
    }
    return NULL;
}

/* ---- AST helpers */

static Node *new_node(NodeKind kind, Token *tok) {
    Node *node = xcalloc(sizeof(Node));
    node->kind = kind;
    node->tok = tok;
    return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = expr;
    return node;
}

static Node *new_num(i64 val, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->val = val;
    return node;
}

static Node *new_long(i64 val, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->val = val;
    node->ty = ty_long;
    return node;
}

static Node *new_ulong(i64 val, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->val = val;
    node->ty = ty_ulong;
    return node;
}

static void add_ref(Obj *from, Obj *to) {
    for (int i = 0; i < from->nrefs; i++)
        if (from->refs[i] == to) return;
    from->refs = xrealloc(from->refs, (size_t)(from->nrefs + 1) * sizeof(Obj *));
    from->refs[from->nrefs++] = to;
}

static Node *new_var_node(Obj *var, Token *tok) {
    Node *node = new_node(ND_VAR, tok);
    node->var = var;
    if (!var->is_local && current_fn) add_ref(current_fn, var);
    return node;
}

/* ---- variables */

static Obj *new_var(char *name, Type *ty) {
    Obj *var = xcalloc(sizeof(Obj));
    var->name = name;
    var->ty = ty;
    var->align = ty->align;
    if (name && *name) push_scope(name)->var = var;
    return var;
}

/* gives a local variable its place in the frame: the slots are reused once the block (or statement) ends */
static void alloc_lvar(Obj *var) {
    int size = var->ty->size > 0 ? var->ty->size : 1;
    int align = (var->ty->kind == TY_ARRAY && var->ty->size >= 16) ? MAX(16, var->align) : var->align;
    if (align < 1) align = 1;
    frame_cur = align_to(frame_cur + size, align);
    if (frame_cur > frame_max) frame_max = frame_cur;
    var->offset = -frame_cur;
}

static Obj *new_lvar_noalloc(char *name, Type *ty) {
    Obj *var = new_var(name, ty);
    var->is_local = true;
    var->next = locals;
    locals = var;
    return var;
}

static Obj *new_lvar(char *name, Type *ty) {
    Obj *var = new_lvar_noalloc(name, ty);
    if (ty->size >= 0) alloc_lvar(var);
    return var;
}

static Obj *new_gvar(char *name, Type *ty) {
    Obj *var = new_var(name, ty);
    var->next = globals;
    globals = var;
    return var;
}

static Obj *new_anon_gvar(Type *ty) {
    Obj *var = xcalloc(sizeof(Obj));
    var->name = new_unique_name();
    var->ty = ty;
    var->align = ty->align;
    var->is_static = true;
    var->is_definition = true;
    var->next = globals;
    globals = var;
    return var;
}

/* Identical string literals of one translation unit share one object, as they do with gcc and clang (code compares them, and the
   program gets smaller).  The key is the element size followed by the bytes, the terminator included. */
static Map string_pool;

static Obj *new_string_literal(char *p, Type *ty) {
    char *key = NULL;
    int keylen = 0;
    if (ty->kind == TY_ARRAY && ty->size > 0 && ty->size <= 65536) {
        keylen = ty->size + 1;
        key = xmalloc((size_t)keylen);
        key[0] = (char)ty->base->size;
        memcpy(key + 1, p, (size_t)ty->size);
        Obj *old = map_get2(&string_pool, key, keylen);
        if (old) {
            free(key);
            return old;
        }
    }
    Obj *var = new_anon_gvar(ty);
    var->init_data = p;
    var->is_string = true;
    var->is_const_data = true;
    if (key) map_put2(&string_pool, key, keylen, var);
    return var;
}

static Obj *find_global(char *name) {
    Scope *sc = scope;
    while (sc->next) sc = sc->next;
    VarScope *sc2 = map_get(&sc->vars, name);
    if (sc2 && sc2->var && !sc2->var->is_local) return sc2->var;
    return NULL;
}

/* ---- attributes */

static Token *skip_balanced(Token *tok) {
    int level = 0;
    do {
        if (tok->kind == TK_EOF) error_tok(tok, "unbalanced parentheses");
        if (equal(tok, "(")) level++;
        else if (equal(tok, ")")) level--;
        tok = tok->next;
    } while (level > 0);
    return tok;
}

static Token *parse_attributes(Token *tok, AttrSet *as) {
    while (equal(tok, "__attribute__")) {
        tok = skip(tok->next, "(");
        tok = skip(tok, "(");
        while (!equal(tok, ")")) {
            if (tok->kind == TK_EOF) error_tok(tok, "unterminated __attribute__");
            Token *name = tok;
            tok = tok->next;
            bool is_aligned = equal(name, "aligned") || equal(name, "__aligned__");
            if (equal(tok, "(")) {
                if (is_aligned && as) {
                    Token *args = tok->next;
                    if (equal(args, ")")) as->align = 16;
                    else {
                        Token *dummy;
                        Node *e = conditional(&dummy, args);
                        as->align = (int)eval(e);
                    }
                }
                tok = skip_balanced(tok);
            } else if (as) {
                if (equal(name, "packed") || equal(name, "__packed__")) as->packed = true;
                else if (equal(name, "weak") || equal(name, "__weak__")) as->weak = true;
                else if (equal(name, "used") || equal(name, "__used__")) as->used = true;
                else if (is_aligned) as->align = 16;
            }
            if (equal(tok, ",")) tok = tok->next;
        }
        tok = skip(tok, ")");
        tok = skip(tok, ")");
    }
    return tok;
}

/* `asm ("label")` after a declarator renames the symbol (ignored); for a local `register` variable it names the register */
static char *last_asm_label;

static Token *skip_asm_label(Token *tok) {
    last_asm_label = NULL;
    while (equal(tok, "asm") || equal(tok, "__attribute__")) {
        if (equal(tok, "asm")) {
            tok = skip(tok->next, "(");
            if (tok->kind == TK_STR) last_asm_label = tok->str;
            while (!equal(tok, ")") && tok->kind != TK_EOF) tok = tok->next;
            tok = skip(tok, ")");
        } else {
            tok = parse_attributes(tok, NULL);
        }
    }
    return tok;
}

/* ---- declaration specifiers */

static bool is_typename(Token *tok) {
    static Map map;
    if (map.capacity == 0) {
        static const char *kw[] = {
            "void", "_Bool", "char", "short", "int", "long", "struct", "union", "typedef", "enum", "static", "extern",
            "_Alignas", "signed", "unsigned", "const", "volatile", "auto", "register", "restrict", "__restrict",
            "__restrict__", "_Noreturn", "float", "double", "typeof", "inline", "_Thread_local", "__thread", "_Atomic",
            "__attribute__", "__extension__",
        };
        for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++) map_put(&map, kw[i], (void *)1);
    }
    return map_get2(&map, tok->loc, tok->len) != NULL || find_typedef(tok);
}

static Type *new_qualified_type(Type *ty) {
    Type *ret = copy_type(ty);
    return ret;
}

static Type *struct_decl(Token **rest, Token *tok, bool is_union);
static Type *enum_specifier(Token **rest, Token *tok);

static Type *typeof_specifier(Token **rest, Token *tok) {
    tok = skip(tok, "(");
    Type *ty;
    if (is_typename(tok)) {
        ty = typename(&tok, tok);
    } else {
        Node *node = expr(&tok, tok);
        add_type(node);
        ty = node->ty;
    }
    *rest = skip(tok, ")");
    return ty;
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
    /* one integer counts every kind of type keyword; e.g. bits 0..1 say how many times "void" was seen */
    enum {
        VOID = 1 << 0, BOOL = 1 << 2, CHAR = 1 << 4, SHORT = 1 << 6, INT = 1 << 8, LONG = 1 << 10, FLOAT = 1 << 12,
        DOUBLE = 1 << 14, OTHER = 1 << 16, SIGNED = 1 << 17, UNSIGNED = 1 << 18,
    };

    Type *ty = ty_int;
    int counter = 0;
    bool is_const = false;
    bool is_volatile = false;
    AttrSet as;
    memset(&as, 0, sizeof as);

    while (is_typename(tok)) {
        if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") || equal(tok, "inline") ||
            equal(tok, "_Thread_local")) {
            if (!attr) error_tok(tok, "storage class specifier is not allowed in this context");
            if (equal(tok, "typedef")) attr->is_typedef = true;
            else if (equal(tok, "static")) attr->is_static = true;
            else if (equal(tok, "extern")) attr->is_extern = true;
            else if (equal(tok, "inline")) attr->is_inline = true;
            else attr->is_tls = true;
            if (attr->is_typedef && (attr->is_static || attr->is_extern || attr->is_inline || attr->is_tls))
                error_tok(tok, "typedef may not be used together with static, extern, inline, __thread or _Thread_local");
            tok = tok->next;
            continue;
        }

        if (equal(tok, "__attribute__")) {
            tok = parse_attributes(tok, &as);
            continue;
        }
        if (equal(tok, "__extension__")) {
            tok = tok->next;
            continue;
        }
        if (equal(tok, "const")) {
            is_const = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "volatile")) {
            is_volatile = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "auto") || equal(tok, "register") || equal(tok, "restrict") || equal(tok, "__restrict") ||
            equal(tok, "__restrict__")) {
            tok = tok->next;
            continue;
        }
        if (equal(tok, "_Noreturn")) {
            if (attr) attr->is_noreturn = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "_Atomic")) {
            tok = tok->next;
            if (equal(tok, "(")) {
                ty = typename(&tok, tok->next);
                tok = skip(tok, ")");
                counter += OTHER;
            }
            continue;
        }
        if (equal(tok, "_Alignas")) {
            if (!attr) error_tok(tok, "_Alignas is not allowed in this context");
            tok = skip(tok->next, "(");
            if (is_typename(tok)) {
                attr->align = typename(&tok, tok)->align;
            } else {
                Node *e = conditional(&tok, tok);
                attr->align = (int)eval(e);
            }
            tok = skip(tok, ")");
            continue;
        }

        /* user-defined types */
        Type *ty2 = find_typedef(tok);
        if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") || equal(tok, "typeof") || ty2) {
            if (counter) break;
            if (equal(tok, "struct")) ty = struct_decl(&tok, tok->next, false);
            else if (equal(tok, "union")) ty = struct_decl(&tok, tok->next, true);
            else if (equal(tok, "enum")) ty = enum_specifier(&tok, tok->next);
            else if (equal(tok, "typeof")) ty = typeof_specifier(&tok, tok->next);
            else {
                ty = ty2;
                tok = tok->next;
            }
            counter += OTHER;
            continue;
        }

        /* built-in types */
        if (equal(tok, "void")) counter += VOID;
        else if (equal(tok, "_Bool")) counter += BOOL;
        else if (equal(tok, "char")) counter += CHAR;
        else if (equal(tok, "short")) counter += SHORT;
        else if (equal(tok, "int")) counter += INT;
        else if (equal(tok, "long")) counter += LONG;
        else if (equal(tok, "float")) counter += FLOAT;
        else if (equal(tok, "double")) counter += DOUBLE;
        else if (equal(tok, "signed")) counter |= SIGNED;
        else if (equal(tok, "unsigned")) counter |= UNSIGNED;
        else ICE();

        switch (counter) {
        case VOID:
            ty = ty_void;
            break;
        case BOOL:
            ty = ty_bool;
            break;
        case CHAR:
        case SIGNED + CHAR:
            ty = ty_char;
            break;
        case UNSIGNED + CHAR:
            ty = ty_uchar;
            break;
        case SHORT:
        case SHORT + INT:
        case SIGNED + SHORT:
        case SIGNED + SHORT + INT:
            ty = ty_short;
            break;
        case UNSIGNED + SHORT:
        case UNSIGNED + SHORT + INT:
            ty = ty_ushort;
            break;
        case INT:
        case SIGNED:
        case SIGNED + INT:
            ty = ty_int;
            break;
        case UNSIGNED:
        case UNSIGNED + INT:
            ty = ty_uint;
            break;
        case LONG:
        case LONG + INT:
        case LONG + LONG:
        case LONG + LONG + INT:
        case SIGNED + LONG:
        case SIGNED + LONG + INT:
        case SIGNED + LONG + LONG:
        case SIGNED + LONG + LONG + INT:
            ty = ty_long;
            break;
        case UNSIGNED + LONG:
        case UNSIGNED + LONG + INT:
        case UNSIGNED + LONG + LONG:
        case UNSIGNED + LONG + LONG + INT:
            ty = ty_ulong;
            break;
        case FLOAT:
            ty = ty_float;
            break;
        case DOUBLE:
        case LONG + DOUBLE:
            ty = ty_double;
            break;
        default:
            error_tok(tok, "invalid type");
        }
        tok = tok->next;
    }

    if (is_const || is_volatile) {
        ty = new_qualified_type(ty);
        ty->is_const = is_const;
        ty->is_volatile = is_volatile;
    }
    if (attr) {
        if (as.weak) attr->is_weak = true;
        if (as.align > attr->align) attr->align = as.align;
    }
    *rest = tok;
    return ty;
}

/* ---- declarators */

static Type *func_params(Token **rest, Token *tok, Type *ty) {
    if (equal(tok, "void") && equal(tok->next, ")")) {
        *rest = tok->next->next;
        return func_type(ty);
    }

    Type head;
    memset(&head, 0, sizeof head);
    Type *cur = &head;
    bool is_variadic = false;
    bool saved = in_param_decl;
    in_param_decl = true;

    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        if (equal(tok, "...")) {
            is_variadic = true;
            tok = tok->next;
            skip(tok, ")");
            break;
        }

        Type *ty2 = declspec(&tok, tok, NULL);
        ty2 = declarator(&tok, tok, ty2);
        tok = parse_attributes(tok, NULL);

        Token *name = ty2->name;
        if (ty2->kind == TY_ARRAY) {
            /* "array of T" is converted to "pointer to T" only in the parameter context */
            ty2 = pointer_to(ty2->base);
            ty2->name = name;
        } else if (ty2->kind == TY_FUNC) {
            ty2 = pointer_to(ty2);
            ty2->name = name;
        }
        cur = cur->next = copy_type(ty2);
    }
    in_param_decl = saved;

    Type *fty = func_type(ty);
    if (cur == &head) fty->has_proto = false; /* int f(): the parameters are unspecified */
    fty->params = head.next;
    fty->is_variadic = is_variadic;
    *rest = tok->next;
    return fty;
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty);

static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
    while (equal(tok, "static") || equal(tok, "restrict") || equal(tok, "const") || equal(tok, "volatile") || equal(tok, "__restrict")) tok = tok->next;

    if (equal(tok, "]")) {
        ty = type_suffix(rest, tok->next, ty);
        return array_of(ty, -1);
    }

    Node *e = conditional(&tok, tok);
    tok = skip(tok, "]");
    ty = type_suffix(rest, tok, ty);
    if (!is_const_expr(e)) {
        if (in_param_decl) return array_of(ty, -1);
        error_tok(e->tok, "variable length arrays are not supported by mcc");
    }
    i64 len = eval(e);
    if (len < 0) error_tok(e->tok, "array size is negative");
    return array_of(ty, (int)len);
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
    if (equal(tok, "(")) return func_params(rest, tok->next, ty);
    if (equal(tok, "[")) return array_dimensions(rest, tok->next, ty);
    *rest = tok;
    return ty;
}

static Type *pointers(Token **rest, Token *tok, Type *ty) {
    while (consume(&tok, tok, "*")) {
        ty = pointer_to(ty);
        for (;;) {
            if (equal(tok, "const")) {
                ty->is_const = true;
                tok = tok->next;
            } else if (equal(tok, "volatile") || equal(tok, "restrict") || equal(tok, "__restrict") || equal(tok, "__restrict__") ||
                       equal(tok, "_Atomic")) {
                tok = tok->next;
            } else if (equal(tok, "__attribute__")) {
                tok = parse_attributes(tok, NULL);
            } else {
                break;
            }
        }
    }
    *rest = tok;
    return ty;
}

/* declarator = pointers ("(" ident ")" | "(" declarator ")" | ident) type-suffix */
static Type *declarator(Token **rest, Token *tok, Type *ty) {
    ty = pointers(&tok, tok, ty);

    if (equal(tok, "(") && !(is_typename(tok->next) || equal(tok->next, ")"))) {
        Token *start = tok;
        Type dummy;
        memset(&dummy, 0, sizeof dummy);
        declarator(&tok, start->next, &dummy);
        tok = skip(tok, ")");
        ty = type_suffix(rest, tok, ty);
        return declarator(&tok, start->next, ty);
    }

    Token *name = NULL;
    Token *name_pos = tok;

    if (tok->kind == TK_IDENT) {
        name = tok;
        tok = tok->next;
    }

    ty = type_suffix(rest, tok, ty);
    ty->name = name;
    ty->name_pos = name_pos;
    return ty;
}

static Type *abstract_declarator(Token **rest, Token *tok, Type *ty) {
    return declarator(rest, tok, ty);
}

/* type-name = declspec abstract-declarator */
static Type *typename(Token **rest, Token *tok) {
    Type *ty = declspec(&tok, tok, NULL);
    ty = abstract_declarator(&tok, tok, ty);
    ty = copy_type(ty);
    ty->name = NULL;
    *rest = tok;
    return ty;
}

static bool is_end(Token *tok) {
    return equal(tok, "}") || (equal(tok, ",") && equal(tok->next, "}"));
}

static bool consume_end(Token **rest, Token *tok) {
    if (equal(tok, "}")) {
        *rest = tok->next;
        return true;
    }
    if (equal(tok, ",") && equal(tok->next, "}")) {
        *rest = tok->next->next;
        return true;
    }
    return false;
}

/* enum-specifier = ident? "{" enum-list? "}" | ident ("{" enum-list? "}")? */
static Type *enum_specifier(Token **rest, Token *tok) {
    Type *ty = enum_type();

    if (equal(tok, "__attribute__")) tok = parse_attributes(tok, NULL);

    Token *tag = NULL;
    if (tok->kind == TK_IDENT) {
        tag = tok;
        tok = tok->next;
    }

    if (tag && !equal(tok, "{")) {
        Type *ty2 = find_tag(tag);
        if (!ty2) {
            /* a forward reference (a GNU extension): the enum is an int */
            push_tag_scope(tag, ty);
            *rest = tok;
            return ty;
        }
        if (ty2->kind != TY_ENUM) error_tok(tag, "not an enum tag");
        *rest = tok;
        return ty2;
    }

    tok = skip(tok, "{");

    int i = 0;
    i64 val = 0;
    while (!consume_end(rest, tok)) {
        if (i++ > 0) tok = skip(tok, ",");
        char *name = get_ident(tok);
        tok = tok->next;
        if (equal(tok, "=")) {
            Node *e = conditional(&tok, tok->next);
            val = eval(e);
        }
        VarScope *sc = push_scope(name);
        sc->enum_ty = ty;
        sc->enum_val = (int)val++;
        tok = parse_attributes(tok, NULL);
    }

    if (tag) push_tag_scope(tag, ty);
    return ty;
}

/* ---- struct and union */

static void struct_members(Token **rest, Token *tok, Type *ty) {
    Member head;
    memset(&head, 0, sizeof head);
    Member *cur = &head;
    int idx = 0;

    while (!equal(tok, "}")) {
        if (tok->kind == TK_EOF) error_tok(tok, "unterminated struct or union");
        VarAttr attr;
        memset(&attr, 0, sizeof attr);
        if (equal(tok, "_Static_assert")) {
            tok = skip(tok->next, "(");
            Node *e = conditional(&tok, tok);
            if (!eval(e)) error_tok(tok, "static assertion failed");
            if (equal(tok, ",")) tok = tok->next->next;
            tok = skip(tok, ")");
            tok = skip(tok, ";");
            continue;
        }
        Type *basety = declspec(&tok, tok, &attr);
        bool first = true;

        /* anonymous struct member */
        if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION) && consume(&tok, tok, ";")) {
            Member *mem = xcalloc(sizeof(Member));
            mem->ty = basety;
            mem->idx = idx++;
            mem->align = attr.align ? attr.align : mem->ty->align;
            cur = cur->next = mem;
            continue;
        }

        /* regular struct members */
        while (!consume(&tok, tok, ";")) {
            if (!first) tok = skip(tok, ",");
            first = false;

            Member *mem = xcalloc(sizeof(Member));
            mem->ty = declarator(&tok, tok, basety);
            mem->name = mem->ty->name;
            mem->tok = tok;
            mem->idx = idx++;
            tok = parse_attributes(tok, NULL);
            mem->align = attr.align ? attr.align : mem->ty->align;

            if (consume(&tok, tok, ":")) {
                mem->is_bitfield = true;
                Node *e = conditional(&tok, tok);
                mem->bit_width = (int)eval(e);
            }
            tok = parse_attributes(tok, NULL);
            cur = cur->next = mem;
        }
    }

    /* a trailing array of unknown length is a "flexible array member": a zero-sized array */
    if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
        cur->ty = array_of(cur->ty->base, 0);
        ty->is_flexible = true;
    }

    *rest = tok->next;
    ty->members = head.next;
}

static Member *get_struct_member(Type *ty, Token *tok) {
    for (Member *mem = ty->members; mem; mem = mem->next) {
        /* anonymous struct member */
        if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) && !mem->name) {
            if (get_struct_member(mem->ty, tok)) return mem;
            continue;
        }
        /* regular struct member */
        if (mem->name && mem->name->len == tok->len && !strncmp(mem->name->loc, tok->loc, (size_t)tok->len)) return mem;
    }
    return NULL;
}

static void struct_layout(Type *ty) {
    int bits = 0;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        if (mem->is_bitfield && mem->bit_width == 0) {
            /* a zero-width bit-field only forces the next one to start at a new unit */
            bits = align_to(bits, mem->ty->size * 8);
        } else if (mem->is_bitfield) {
            int sz = mem->ty->size;
            if (bits / (sz * 8) != (bits + mem->bit_width - 1) / (sz * 8)) bits = align_to(bits, sz * 8);
            mem->offset = align_down(bits / 8, sz);
            mem->bit_offset = bits % (sz * 8);
            bits += mem->bit_width;
        } else {
            if (!ty->is_packed) bits = align_to(bits, mem->align * 8);
            mem->offset = bits / 8;
            bits += mem->ty->size * 8;
        }
        if (!ty->is_packed && ty->align < mem->align) ty->align = mem->align;
    }
    if (ty->attr_align > ty->align) ty->align = ty->attr_align;
    ty->size = align_to(bits, ty->align * 8) / 8;
}

static void union_layout(Type *ty) {
    for (Member *mem = ty->members; mem; mem = mem->next) {
        if (ty->align < mem->align) ty->align = mem->align;
        if (ty->size < mem->ty->size) ty->size = mem->ty->size;
    }
    if (ty->attr_align > ty->align) ty->align = ty->attr_align;
    ty->size = align_to(ty->size, ty->align);
}

/* struct-union-decl = attribute* ident? ("{" struct-members "}")? attribute* */
static Type *struct_decl(Token **rest, Token *tok, bool is_union) {
    AttrSet as;
    memset(&as, 0, sizeof as);
    tok = parse_attributes(tok, &as);

    Token *tag = NULL;
    if (tok->kind == TK_IDENT) {
        tag = tok;
        tok = tok->next;
    }

    if (tag && !equal(tok, "{")) {
        /* a reference to (or forward declaration of) a tagged type */
        Type *ty2 = NULL;
        if (equal(tok, ";")) ty2 = map_get2(&scope->tags, tag->loc, tag->len);
        else ty2 = find_tag(tag);
        if (ty2) {
            *rest = tok;
            return ty2;
        }
        Type *ty = struct_type();
        ty->kind = is_union ? TY_UNION : TY_STRUCT;
        ty->size = -1;
        push_tag_scope(tag, ty);
        *rest = tok;
        return ty;
    }

    tok = skip(tok, "{");

    Type *ty = struct_type();
    ty->kind = is_union ? TY_UNION : TY_STRUCT;
    struct_members(&tok, tok, ty);
    tok = parse_attributes(tok, &as);
    ty->is_packed = as.packed;
    ty->attr_align = as.align;
    if (is_union) union_layout(ty);
    else struct_layout(ty);

    if (tag) {
        /* a definition completes an earlier forward declaration in this scope */
        Type *ty2 = map_get2(&scope->tags, tag->loc, tag->len);
        if (ty2) {
            *ty2 = *ty;
            *rest = tok;
            return ty2;
        }
        push_tag_scope(tag, ty);
    }
    *rest = tok;
    return ty;
}

/* ---- typedef */

static Token *parse_typedef(Token *tok, Type *basety) {
    bool first = true;
    while (!consume(&tok, tok, ";")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        Type *ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(ty->name_pos, "typedef name omitted");
        tok = parse_attributes(tok, NULL);
        push_scope(get_ident(ty->name))->type_def = ty;
    }
    return tok;
}

/* ---- initializers */

static Initializer *new_initializer(Type *ty, bool is_flexible) {
    Initializer *init = xcalloc(sizeof(Initializer));
    init->ty = ty;

    if (ty->kind == TY_ARRAY) {
        if (is_flexible && ty->size < 0) {
            init->is_flexible = true;
            return init;
        }
        /* the element initializers are created when they are needed: big zero-filled arrays stay cheap */
        init->children = xcalloc((size_t)(ty->array_len ? ty->array_len : 1) * sizeof(Initializer *));
        return init;
    }

    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        int len = 0;
        for (Member *mem = ty->members; mem; mem = mem->next) len++;
        init->children = xcalloc((size_t)(len ? len : 1) * sizeof(Initializer *));

        for (Member *mem = ty->members; mem; mem = mem->next) {
            if (is_flexible && ty->is_flexible && !mem->next) {
                Initializer *child = xcalloc(sizeof(Initializer));
                child->ty = mem->ty;
                child->is_flexible = true;
                init->children[mem->idx] = child;
            }
        }
        return init;
    }
    return init;
}

static Initializer *array_child(Initializer *init, int i) {
    if (!init->children[i]) init->children[i] = new_initializer(init->ty->base, false);
    return init->children[i];
}

static Initializer *member_child(Initializer *init, Member *mem) {
    if (!init->children[mem->idx]) init->children[mem->idx] = new_initializer(mem->ty, false);
    return init->children[mem->idx];
}

static Token *skip_excess_element(Token *tok) {
    if (equal(tok, "{")) {
        tok = skip_excess_element(tok->next);
        return skip(tok, "}");
    }
    assign(&tok, tok);
    return tok;
}

/* string-initializer = string-literal */
static void string_initializer(Token **rest, Token *tok, Initializer *init) {
    if (init->is_flexible) *init = *new_initializer(array_of(init->ty->base, tok->ty->array_len), false);

    int len = MIN(init->ty->array_len, tok->ty->array_len);

    switch (init->ty->base->size) {
    case 1: {
        char *str = tok->str;
        for (int i = 0; i < len; i++) array_child(init, i)->expr = new_num(str[i], tok);
        break;
    }
    case 2: {
        u16 *str = (u16 *)tok->str;
        for (int i = 0; i < len; i++) array_child(init, i)->expr = new_num(str[i], tok);
        break;
    }
    case 4: {
        u32 *str = (u32 *)tok->str;
        for (int i = 0; i < len; i++) array_child(init, i)->expr = new_num((i32)str[i], tok);
        break;
    }
    default:
        error_tok(tok, "invalid string initializer");
    }

    *rest = tok->next;
}

static void designation(Token **rest, Token *tok, Initializer *init);
static void struct_initializer2(Token **rest, Token *tok, Initializer *init, Member *mem);
static void array_initializer2(Token **rest, Token *tok, Initializer *init, int i);

static int count_array_init_elements(Token *tok, Type *ty) {
    bool first = true;
    Initializer *dummy = new_initializer(ty->base, true);

    int i = 0, max = 0;

    while (!consume_end(&tok, tok)) {
        if (!first) tok = skip(tok, ",");
        first = false;

        if (equal(tok, "[")) {
            Node *e = conditional(&tok, tok->next);
            i = (int)eval(e);
            if (equal(tok, "...")) {
                Node *e2 = conditional(&tok, tok->next);
                i = (int)eval(e2);
            }
            tok = skip(tok, "]");
            designation(&tok, tok, dummy);
        } else {
            initializer2(&tok, tok, dummy);
        }

        i++;
        max = MAX(max, i);
    }
    return max;
}

/* array-initializer1 = "{" initializer ("," initializer)* ","? "}" */
static void array_initializer1(Token **rest, Token *tok, Initializer *init) {
    tok = skip(tok, "{");

    if (init->is_flexible) {
        int len = count_array_init_elements(tok, init->ty);
        *init = *new_initializer(array_of(init->ty->base, len), false);
    }

    bool first = true;

    for (int i = 0; !consume_end(rest, tok); i++) {
        if (!first) tok = skip(tok, ",");
        first = false;

        if (equal(tok, "[")) {
            Node *e = conditional(&tok, tok->next);
            int begin = (int)eval(e);
            int end = begin;
            if (equal(tok, "...")) {
                Node *e2 = conditional(&tok, tok->next);
                end = (int)eval(e2);
            }
            if (begin < 0 || end >= init->ty->array_len || begin > end) error_tok(tok, "array designator index out of bounds");
            Token *start = skip(tok, "]");
            for (int j = begin; j <= end; j++) {
                /* a range designator repeats the same initializer */
                Token *t = start;
                designation(&t, t, array_child(init, j));
                tok = t;
            }
            i = end;
            continue;
        }

        if (i < init->ty->array_len) initializer2(&tok, tok, array_child(init, i));
        else tok = skip_excess_element(tok);
    }
}

/* array-initializer2 = initializer ("," initializer)* */
static void array_initializer2(Token **rest, Token *tok, Initializer *init, int i) {
    if (init->is_flexible) {
        int len = count_array_init_elements(tok, init->ty);
        *init = *new_initializer(array_of(init->ty->base, len), false);
    }

    for (; i < init->ty->array_len && !is_end(tok); i++) {
        Token *start = tok;
        if (i > 0) tok = skip(tok, ",");

        if (equal(tok, "[") || equal(tok, ".")) {
            *rest = start;
            return;
        }

        initializer2(&tok, tok, array_child(init, i));
    }
    *rest = tok;
}

/* struct-initializer1 = "{" initializer ("," initializer)* ","? "}" */
static void struct_initializer1(Token **rest, Token *tok, Initializer *init) {
    tok = skip(tok, "{");

    Member *mem = init->ty->members;
    bool first = true;

    while (!consume_end(rest, tok)) {
        if (!first) tok = skip(tok, ",");
        first = false;

        if (equal(tok, ".")) {
            Token *dot = tok;
            Token *name = tok->next;
            if (name->kind != TK_IDENT) error_tok(name, "expected a field designator");
            Member *m2 = get_struct_member(init->ty, name);
            if (!m2) error_tok(name, "struct has no such member");
            if (m2->name) {
                tok = name->next;
                designation(&tok, tok, member_child(init, m2));
            } else {
                /* a member of an anonymous struct or union: the designator is handled one level down */
                tok = dot;
                designation(&tok, tok, member_child(init, m2));
            }
            mem = m2->next;
            continue;
        }

        if (mem) {
            initializer2(&tok, tok, member_child(init, mem));
            mem = mem->next;
        } else {
            tok = skip_excess_element(tok);
        }
    }
}

/* struct-initializer2 = initializer ("," initializer)* */
static void struct_initializer2(Token **rest, Token *tok, Initializer *init, Member *mem) {
    bool first = true;

    for (; mem && !is_end(tok); mem = mem->next) {
        Token *start = tok;

        if (!first) tok = skip(tok, ",");
        first = false;

        if (equal(tok, "[") || equal(tok, ".")) {
            *rest = start;
            return;
        }

        initializer2(&tok, tok, member_child(init, mem));
    }
    *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Initializer *init) {
    /* Unlike structs, union initializers take only one initializer, and that initializes the first member by default.
     * Another member can be chosen with a designator. */
    if (equal(tok, "{") && equal(tok->next, ".")) {
        Token *t = tok->next;
        t = skip(t, ".");
        Member *mem = get_struct_member(init->ty, t);
        if (!mem) error_tok(t, "union has no such member");
        init->mem = mem;
        if (!mem->name) {
            Token *dot = tok->next;
            designation(&dot, dot, init);
            tok = dot;
        } else {
            t = t->next;
            designation(&t, t, member_child(init, mem));
            tok = t;
        }
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
        return;
    }

    init->mem = init->ty->members;

    if (equal(tok, "{")) {
        initializer2(&tok, tok->next, member_child(init, init->mem));
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
    } else {
        initializer2(rest, tok, member_child(init, init->mem));
    }
}

/* designation = ("[" const-expr "]" | "." ident)* "="? initializer */
static void designation(Token **rest, Token *tok, Initializer *init) {
    if (equal(tok, "[")) {
        if (init->ty->kind != TY_ARRAY) error_tok(tok, "array index in non-array initializer");
        Node *e = conditional(&tok, tok->next);
        int i = (int)eval(e);
        if (i < 0 || i >= init->ty->array_len) error_tok(tok, "array designator index exceeds array bounds");
        tok = skip(tok, "]");
        designation(&tok, tok, array_child(init, i));
        array_initializer2(rest, tok, init, i + 1);
        return;
    }

    if (equal(tok, ".") && init->ty->kind == TY_STRUCT) {
        Token *start = tok;
        tok = skip(tok, ".");
        if (tok->kind != TK_IDENT) error_tok(tok, "expected a field designator");
        Member *mem = get_struct_member(init->ty, tok);
        if (!mem) error_tok(tok, "struct has no such member");
        if (!mem->name) {
            /* the designator names a member of an anonymous struct or union */
            designation(&start, start, member_child(init, mem));
            init->expr = NULL;
            struct_initializer2(rest, start, init, mem->next);
            return;
        }
        tok = tok->next;
        designation(&tok, tok, member_child(init, mem));
        init->expr = NULL;
        struct_initializer2(rest, tok, init, mem->next);
        return;
    }

    if (equal(tok, ".") && init->ty->kind == TY_UNION) {
        Token *start = tok;
        tok = skip(tok, ".");
        if (tok->kind != TK_IDENT) error_tok(tok, "expected a field designator");
        Member *mem = get_struct_member(init->ty, tok);
        if (!mem) error_tok(tok, "union has no such member");
        init->mem = mem;
        if (!mem->name) {
            designation(rest, start, member_child(init, mem));
            return;
        }
        tok = tok->next;
        designation(rest, tok, member_child(init, mem));
        return;
    }

    if (equal(tok, ".")) error_tok(tok, "field name not in struct or union initializer");

    if (equal(tok, "=")) tok = tok->next;
    initializer2(rest, tok, init);
}

/* An initializer for a scalar variable, a string or an aggregate, or the elements of a bigger one */
static void initializer2(Token **rest, Token *tok, Initializer *init) {
    if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR) {
        string_initializer(rest, tok, init);
        return;
    }

    if (init->ty->kind == TY_ARRAY) {
        if (equal(tok, "{")) array_initializer1(rest, tok, init);
        else array_initializer2(rest, tok, init, 0);
        return;
    }

    if (init->ty->kind == TY_STRUCT) {
        if (equal(tok, "{")) {
            struct_initializer1(rest, tok, init);
            return;
        }

        /* A struct can be initialized with another struct: `struct T x = y;` */
        Node *e = assign(rest, tok);
        add_type(e);
        if (e->ty->kind == TY_STRUCT) {
            init->expr = e;
            return;
        }

        struct_initializer2(rest, tok, init, init->ty->members);
        return;
    }

    if (init->ty->kind == TY_UNION) {
        if (equal(tok, "{")) {
            union_initializer(rest, tok, init);
            return;
        }

        Node *e = assign(rest, tok);
        add_type(e);
        if (e->ty->kind == TY_UNION) {
            init->expr = e;
            return;
        }

        union_initializer(rest, tok, init);
        return;
    }

    if (equal(tok, "{")) {
        /* an initializer for a scalar may be surrounded by braces: `int x = {3};` */
        initializer2(&tok, tok->next, init);
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
        return;
    }

    init->expr = assign(rest, tok);
}

static Type *copy_struct_type(Type *ty) {
    ty = copy_type(ty);

    Member head;
    memset(&head, 0, sizeof head);
    Member *cur = &head;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        Member *m = xcalloc(sizeof(Member));
        *m = *mem;
        cur = cur->next = m;
    }

    ty->members = head.next;
    return ty;
}

static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty) {
    Initializer *init = new_initializer(ty, true);
    initializer2(rest, tok, init);

    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
        ty = copy_struct_type(ty);

        Member *mem = ty->members;
        while (mem->next) mem = mem->next;
        if (init->children[mem->idx]) {
            mem->ty = init->children[mem->idx]->ty;
            ty->size += mem->ty->size;
        }

        *new_ty = ty;
        return init;
    }

    *new_ty = init->ty;
    return init;
}

static Node *init_desg_expr(InitDesg *desg, Token *tok) {
    if (desg->var) return new_var_node(desg->var, tok);

    if (desg->member) {
        Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
        node->member = desg->member;
        return node;
    }

    Node *lhs = init_desg_expr(desg->next, tok);
    Node *rhs = new_num(desg->idx, tok);
    return new_unary(ND_DEREF, new_add(lhs, rhs, tok), tok);
}

static Node *create_lvar_init(Initializer *init, Type *ty, InitDesg *desg, Token *tok) {
    if (!init) return new_node(ND_NULL_EXPR, tok);

    if (ty->kind == TY_ARRAY) {
        Node *node = new_node(ND_NULL_EXPR, tok);
        for (int i = 0; i < ty->array_len; i++) {
            if (!init->children[i]) continue;
            InitDesg desg2 = {desg, i, NULL, NULL};
            Node *rhs = create_lvar_init(init->children[i], ty->base, &desg2, tok);
            node = new_binary(ND_COMMA, node, rhs, tok);
        }
        return node;
    }

    if (ty->kind == TY_STRUCT && !init->expr) {
        Node *node = new_node(ND_NULL_EXPR, tok);

        for (Member *mem = ty->members; mem; mem = mem->next) {
            if (!init->children[mem->idx]) continue;
            InitDesg desg2 = {desg, 0, mem, NULL};
            Node *rhs = create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
            node = new_binary(ND_COMMA, node, rhs, tok);
        }
        return node;
    }

    if (ty->kind == TY_UNION && !init->expr) {
        Member *mem = init->mem ? init->mem : ty->members;
        if (!init->children[mem->idx]) return new_node(ND_NULL_EXPR, tok);
        InitDesg desg2 = {desg, 0, mem, NULL};
        return create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
    }

    if (!init->expr) return new_node(ND_NULL_EXPR, tok);

    Node *lhs = init_desg_expr(desg, tok);
    return new_binary(ND_ASSIGN, lhs, init->expr, tok);
}

/* A variable definition with an initializer is a shorthand notation for a variable definition followed by assignments.
 * This function generates assignment expressions for an initializer.  For example, `int x[2][2] = {{6, 7}, {8, 9}}` is
 * converted to the following expressions: x[0][0] = 6; x[0][1] = 7; x[1][0] = 8; x[1][1] = 9; */
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
    Initializer *init = initializer(rest, tok, var->ty, &var->ty);
    InitDesg desg = {NULL, 0, NULL, var};

    /* a partial initializer list leaves the rest zero: zero the whole object first */
    Node *lhs = new_node(ND_MEMZERO, tok);
    lhs->var = var;

    Node *rhs = create_lvar_init(init, var->ty, &desg, tok);
    return new_binary(ND_COMMA, lhs, rhs, tok);
}

static u64 read_buf(char *buf, int sz) {
    if (sz == 1) return *(u8 *)buf;
    if (sz == 2) return *(u16 *)buf;
    if (sz == 4) return *(u32 *)buf;
    if (sz == 8) return *(u64 *)buf;
    ICE();
    return 0;
}

static void write_buf(char *buf, u64 val, int sz) {
    if (sz == 1) *(u8 *)buf = (u8)val;
    else if (sz == 2) *(u16 *)buf = (u16)val;
    else if (sz == 4) *(u32 *)buf = (u32)val;
    else if (sz == 8) *(u64 *)buf = val;
    else ICE();
}

static Relocation *write_gvar_data(Obj *owner, Relocation *cur, Initializer *init, Type *ty, char *buf, int offset) {
    if (!init) return cur;

    if (ty->kind == TY_ARRAY) {
        int sz = ty->base->size;
        for (int i = 0; i < ty->array_len; i++) cur = write_gvar_data(owner, cur, init->children[i], ty->base, buf, offset + sz * i);
        return cur;
    }

    if (ty->kind == TY_STRUCT) {
        for (Member *mem = ty->members; mem; mem = mem->next) {
            if (mem->is_bitfield) {
                if (!init->children[mem->idx]) continue;
                Node *e = init->children[mem->idx]->expr;
                if (!e) continue;

                char *loc = buf + offset + mem->offset;
                u64 oldval = read_buf(loc, mem->ty->size);
                u64 newval = (u64)eval(e);
                u64 mask = (mem->bit_width >= 64) ? ~0ULL : ((1ULL << mem->bit_width) - 1);
                u64 combined = oldval | ((newval & mask) << mem->bit_offset);
                write_buf(loc, combined, mem->ty->size);
            } else {
                cur = write_gvar_data(owner, cur, init->children[mem->idx], mem->ty, buf, offset + mem->offset);
            }
        }
        return cur;
    }

    if (ty->kind == TY_UNION) {
        if (!init->mem) return cur;
        return write_gvar_data(owner, cur, init->children[init->mem->idx], init->mem->ty, buf, offset);
    }

    if (!init->expr) return cur;

    if (ty->kind == TY_FLOAT) {
        float f = (float)eval_double(init->expr);
        memcpy(buf + offset, &f, 4);
        return cur;
    }

    if (ty->kind == TY_DOUBLE) {
        double d = eval_double(init->expr);
        memcpy(buf + offset, &d, 8);
        return cur;
    }

    Obj *label = NULL;
    u64 val = (u64)eval2(init->expr, &label);

    if (!label) {
        write_buf(buf + offset, val, ty->size);
        return cur;
    }

    Relocation *rel = xcalloc(sizeof(Relocation));
    rel->offset = offset;
    rel->target = label;
    rel->addend = (i64)val;
    cur->next = rel;
    add_ref(owner, label);
    return cur->next;
}

/* Initializers for global variables are evaluated at compile time and embedded in the data section. */
static void gvar_initializer(Token **rest, Token *tok, Obj *var) {
    Initializer *init = initializer(rest, tok, var->ty, &var->ty);

    Relocation head;
    memset(&head, 0, sizeof head);
    char *buf = xcalloc((size_t)(var->ty->size > 0 ? var->ty->size : 1));
    write_gvar_data(var, &head, init, var->ty, buf, 0);
    var->init_data = buf;
    var->rel = head.next;
}

/* ---- declarations in a block */

static bool is_function(Token *tok) {
    if (equal(tok, ";")) return false;

    Type dummy;
    memset(&dummy, 0, sizeof dummy);
    Type *ty = declarator(&tok, tok, &dummy);
    return ty->kind == TY_FUNC;
}

static bool is_const_type(Type *ty) {
    while (ty->kind == TY_ARRAY) ty = ty->base;
    return ty->is_const;
}

static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
    Node head;
    memset(&head, 0, sizeof head);
    Node *cur = &head;
    int i = 0;

    while (!equal(tok, ";")) {
        if (i++ > 0) tok = skip(tok, ",");

        Type *ty = declarator(&tok, tok, basety);
        if (ty->kind == TY_VOID) error_tok(tok, "variable declared void");
        if (!ty->name) error_tok(ty->name_pos, "variable name omitted");
        tok = skip_asm_label(tok);

        if (attr && attr->is_static) {
            /* static local variable */
            Obj *var = new_anon_gvar(ty);
            var->is_const_data = is_const_type(ty);
            var->is_tls = attr->is_tls;
            if (attr->align) var->align = attr->align;
            if (current_fn) var->name = xformat("%s.%.*s.%d", current_fn->name, ty->name->len, ty->name->loc, unique_counter++);
            push_scope(get_ident(ty->name))->var = var;
            if (equal(tok, "=")) gvar_initializer(&tok, tok->next, var);
            else var->is_const_data = false;
            continue;
        }

        Obj *var = new_lvar_noalloc(get_ident(ty->name), ty);
        var->asm_reg = last_asm_label;
        if (attr && attr->align) var->align = attr->align;

        if (equal(tok, "=")) {
            Node *e = lvar_initializer(&tok, tok->next, var);
            cur = cur->next = new_unary(ND_EXPR_STMT, e, tok);
        }

        if (var->ty->size < 0) error_tok(ty->name, "variable has incomplete type");
        if (var->ty->kind == TY_VOID) error_tok(ty->name, "variable declared void");
        alloc_lvar(var);
    }

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    *rest = tok->next;
    return node;
}
