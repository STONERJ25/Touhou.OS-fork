/* type.inc -- C types, and the pass that gives every expression node its type (usual arithmetic conversions etc.). */

static Type t_void = {TY_VOID, 1, 1};
static Type t_bool = {TY_BOOL, 1, 1};
static Type t_char = {TY_CHAR, 1, 1};
static Type t_short = {TY_SHORT, 2, 2};
static Type t_int = {TY_INT, 4, 4};
static Type t_long = {TY_LONG, 8, 8};
static Type t_uchar = {TY_CHAR, 1, 1, true};
static Type t_ushort = {TY_SHORT, 2, 2, true};
static Type t_uint = {TY_INT, 4, 4, true};
static Type t_ulong = {TY_LONG, 8, 8, true};
static Type t_float = {TY_FLOAT, 4, 4};
static Type t_double = {TY_DOUBLE, 8, 8};

Type *ty_void = &t_void;
Type *ty_bool = &t_bool;
Type *ty_char = &t_char;
Type *ty_short = &t_short;
Type *ty_int = &t_int;
Type *ty_long = &t_long;
Type *ty_uchar = &t_uchar;
Type *ty_ushort = &t_ushort;
Type *ty_uint = &t_uint;
Type *ty_ulong = &t_ulong;
Type *ty_float = &t_float;
Type *ty_double = &t_double;

static Type *new_type(TypeKind kind, int size, int align) {
    Type *ty = xcalloc(sizeof(Type));
    ty->kind = kind;
    ty->size = size;
    ty->align = align;
    return ty;
}

bool is_integer(Type *ty) {
    TypeKind k = ty->kind;
    return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG || k == TY_ENUM;
}

bool is_flonum(Type *ty) {
    return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE;
}

bool is_numeric(Type *ty) {
    return is_integer(ty) || is_flonum(ty);
}

bool is_compatible(Type *t1, Type *t2) {
    if (t1 == t2) return true;
    if (t1->origin) return is_compatible(t1->origin, t2);
    if (t2->origin) return is_compatible(t1, t2->origin);
    if (t1->kind != t2->kind) return false;
    switch (t1->kind) {
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
        return t1->is_unsigned == t2->is_unsigned;
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_VOID:
    case TY_BOOL:
    case TY_ENUM:
        return true;
    case TY_PTR:
        return is_compatible(t1->base, t2->base);
    case TY_FUNC: {
        if (!is_compatible(t1->return_ty, t2->return_ty)) return false;
        if (t1->is_variadic != t2->is_variadic) return false;
        Type *p1 = t1->params;
        Type *p2 = t2->params;
        for (; p1 && p2; p1 = p1->next, p2 = p2->next)
            if (!is_compatible(p1, p2)) return false;
        return p1 == NULL && p2 == NULL;
    }
    case TY_ARRAY:
        if (!is_compatible(t1->base, t2->base)) return false;
        return t1->array_len < 0 || t2->array_len < 0 || t1->array_len == t2->array_len;
    default:
        return false;
    }
}

Type *copy_type(Type *ty) {
    Type *ret = xcalloc(sizeof(Type));
    *ret = *ty;
    ret->origin = ty;
    return ret;
}

Type *pointer_to(Type *base) {
    Type *ty = new_type(TY_PTR, 8, 8);
    ty->base = base;
    ty->is_unsigned = true;
    return ty;
}

Type *func_type(Type *return_ty) {
    /* sizeof(function) is 1 like in GNU C, so pointer arithmetic on function pointers works byte-wise */
    Type *ty = new_type(TY_FUNC, 1, 1);
    ty->return_ty = return_ty;
    ty->has_proto = true;
    return ty;
}

Type *array_of(Type *base, int len) {
    Type *ty = new_type(TY_ARRAY, len < 0 ? -1 : base->size * len, base->align);
    ty->base = base;
    ty->array_len = len;
    return ty;
}

Type *enum_type(void) {
    return new_type(TY_ENUM, 4, 4);
}

Type *struct_type(void) {
    return new_type(TY_STRUCT, 0, 1);
}

Node *new_cast(Node *expr, Type *ty) {
    add_type(expr);
    Node *node = xcalloc(sizeof(Node));
    node->kind = ND_CAST;
    node->tok = expr->tok;
    node->lhs = expr;
    node->ty = copy_type(ty);
    return node;
}

static Type *get_common_type(Type *ty1, Type *ty2) {
    if (ty1->base) return pointer_to(ty1->base);
    if (ty1->kind == TY_FUNC) return pointer_to(ty1);
    if (ty2->kind == TY_FUNC) return pointer_to(ty2);
    if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE) return ty_double;
    if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT) return ty_float;
    if (ty1->size < 4) ty1 = ty_int;
    if (ty2->size < 4) ty2 = ty_int;
    if (ty1->kind == TY_ENUM) ty1 = ty_int;
    if (ty2->kind == TY_ENUM) ty2 = ty_int;
    if (ty1->size != ty2->size) return (ty1->size < ty2->size) ? ty2 : ty1;
    if (ty2->is_unsigned) return ty2;
    return ty1;
}

/* The usual arithmetic conversions (6.3.1.8): both operands end up with the same type. */
static void usual_arith_conv(Node **lhs, Node **rhs) {
    Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
    *lhs = new_cast(*lhs, ty);
    *rhs = new_cast(*rhs, ty);
}

void add_type(Node *node) {
    if (!node || node->ty) return;

    add_type(node->lhs);
    add_type(node->rhs);
    add_type(node->cond);
    add_type(node->then);
    add_type(node->els);
    add_type(node->init);
    add_type(node->inc);

    for (Node *n = node->body; n; n = n->next) add_type(n);
    for (Node *n = node->args; n; n = n->next) add_type(n);

    switch (node->kind) {
    case ND_NUM:
        node->ty = ty_int;
        return;
    case ND_ADD:
    case ND_SUB:
    case ND_MUL:
    case ND_DIV:
    case ND_MOD:
    case ND_BITAND:
    case ND_BITOR:
    case ND_BITXOR:
        usual_arith_conv(&node->lhs, &node->rhs);
        node->ty = node->lhs->ty;
        return;
    case ND_NEG: {
        Type *ty = get_common_type(ty_int, node->lhs->ty);
        node->lhs = new_cast(node->lhs, ty);
        node->ty = ty;
        return;
    }
    case ND_ASSIGN:
        if (node->lhs->ty->kind == TY_ARRAY) error_tok(node->lhs->tok, "not an lvalue");
        if (node->lhs->ty->kind != TY_STRUCT && node->lhs->ty->kind != TY_UNION) node->rhs = new_cast(node->rhs, node->lhs->ty);
        node->ty = node->lhs->ty;
        return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
        usual_arith_conv(&node->lhs, &node->rhs);
        node->ty = ty_int;
        return;
    case ND_FUNCALL:
        node->ty = node->func_ty->return_ty;
        return;
    case ND_NOT:
    case ND_LOGOR:
    case ND_LOGAND:
        node->ty = ty_int;
        return;
    case ND_BITNOT:
    case ND_SHL:
    case ND_SHR: {
        /* the result has the promoted type of the left operand */
        Type *ty = node->lhs->ty;
        if (ty->size < 4 || ty->kind == TY_ENUM) {
            node->lhs = new_cast(node->lhs, ty_int);
            ty = ty_int;
        }
        node->ty = ty;
        return;
    }
    case ND_VAR:
        node->ty = node->var->ty;
        return;
    case ND_COND:
        if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
            node->ty = ty_void;
        } else if ((node->then->ty->kind == TY_STRUCT || node->then->ty->kind == TY_UNION) &&
                   node->then->ty->kind == node->els->ty->kind) {
            node->ty = node->then->ty;
        } else {
            usual_arith_conv(&node->then, &node->els);
            node->ty = node->then->ty;
        }
        return;
    case ND_COMMA:
        node->ty = node->rhs->ty;
        return;
    case ND_MEMBER:
        node->ty = node->member->ty;
        return;
    case ND_ADDR: {
        Type *ty = node->lhs->ty;
        node->ty = pointer_to(ty);
        return;
    }
    case ND_DEREF:
        if (!node->lhs->ty->base) error_tok(node->tok, "invalid pointer dereference");
        if (node->lhs->ty->base->kind == TY_VOID) error_tok(node->tok, "dereferencing a void pointer");
        node->ty = node->lhs->ty->base;
        return;
    case ND_STMT_EXPR:
        if (node->body) {
            Node *stmt = node->body;
            while (stmt->next) stmt = stmt->next;
            if (stmt->kind == ND_EXPR_STMT) {
                node->ty = stmt->lhs->ty;
                return;
            }
        }
        node->ty = ty_void;
        return;
    default:
        return;
    }
}
