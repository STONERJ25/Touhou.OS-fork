/* expr.inc -- the C parser, part 2: constant expressions, expressions, statements, functions, the top level. */

/* ---- constant expressions */

static i64 eval(Node *node) {
    return eval2(node, NULL);
}

static i64 eval_div_check(Node *node, i64 d) {
    if (d == 0) error_tok(node->tok, "division by zero in a constant expression");
    return d;
}

/* Evaluate a given node as a constant expression.  A constant expression is either just a number or ptr+n where ptr
 * is a pointer to a global variable and n is a positive/negative number; the latter form is accepted only as an
 * initialization expression for a global variable (then *label is set to the variable). */
static i64 eval2(Node *node, Obj **label) {
    add_type(node);

    if (is_flonum(node->ty)) return (i64)eval_double(node);

    switch (node->kind) {
    case ND_ADD:
        return eval2(node->lhs, label) + eval(node->rhs);
    case ND_SUB: {
        Obj *l2 = NULL;
        i64 a = eval2(node->lhs, label);
        i64 b = eval2(node->rhs, label ? &l2 : NULL);
        if (l2) {
            /* the difference of two addresses inside the same object is a constant */
            if (*label != l2) error_tok(node->tok, "not a compile-time constant");
            *label = NULL;
        }
        return a - b;
    }
    case ND_MUL:
        return eval(node->lhs) * eval(node->rhs);
    case ND_DIV: {
        i64 l = eval(node->lhs), r = eval_div_check(node, eval(node->rhs));
        if (node->ty->is_unsigned) return (i64)((u64)l / (u64)r);
        return r == -1 ? (i64)(0 - (u64)l) : l / r;
    }
    case ND_NEG:
        return (i64)(0 - (u64)eval(node->lhs));
    case ND_MOD: {
        i64 l = eval(node->lhs), r = eval_div_check(node, eval(node->rhs));
        if (node->ty->is_unsigned) return (i64)((u64)l % (u64)r);
        return r == -1 ? 0 : l % r;
    }
    case ND_BITAND:
        return eval(node->lhs) & eval(node->rhs);
    case ND_BITOR:
        return eval(node->lhs) | eval(node->rhs);
    case ND_BITXOR:
        return eval(node->lhs) ^ eval(node->rhs);
    case ND_SHL:
        return (i64)((u64)eval(node->lhs) << (eval(node->rhs) & 63));
    case ND_SHR:
        if (node->ty->is_unsigned && node->ty->size == 8) return (i64)((u64)eval(node->lhs) >> (eval(node->rhs) & 63));
        return eval(node->lhs) >> (eval(node->rhs) & 63);
    case ND_EQ:
        return eval(node->lhs) == eval(node->rhs);
    case ND_NE:
        return eval(node->lhs) != eval(node->rhs);
    case ND_LT:
        if (node->lhs->ty->is_unsigned && node->lhs->ty->size == 8) return (u64)eval(node->lhs) < (u64)eval(node->rhs);
        return eval(node->lhs) < eval(node->rhs);
    case ND_LE:
        if (node->lhs->ty->is_unsigned && node->lhs->ty->size == 8) return (u64)eval(node->lhs) <= (u64)eval(node->rhs);
        return eval(node->lhs) <= eval(node->rhs);
    case ND_COND:
        return eval(node->cond) ? eval2(node->then, label) : eval2(node->els, label);
    case ND_COMMA:
        return eval2(node->rhs, label);
    case ND_NOT:
        return !eval(node->lhs);
    case ND_BITNOT:
        return ~eval(node->lhs);
    case ND_LOGAND:
        return eval(node->lhs) && eval(node->rhs);
    case ND_LOGOR:
        return eval(node->lhs) || eval(node->rhs);
    case ND_CAST: {
        i64 val = eval2(node->lhs, label);
        if (node->ty->kind == TY_BOOL) return val != 0;
        if (is_integer(node->ty)) {
            switch (node->ty->size) {
            case 1:
                return node->ty->is_unsigned ? (i64)(u8)val : (i64)(i8)val;
            case 2:
                return node->ty->is_unsigned ? (i64)(u16)val : (i64)(i16)val;
            case 4:
                return node->ty->is_unsigned ? (i64)(u32)val : (i64)(i32)val;
            }
        }
        return val;
    }
    case ND_ADDR:
        return eval_rval(node->lhs, label);
    case ND_MEMBER:
        if (!label) error_tok(node->tok, "not a compile-time constant");
        if (node->ty->kind != TY_ARRAY) error_tok(node->tok, "invalid initializer");
        return eval_rval(node->lhs, label) + node->member->offset;
    case ND_VAR:
        if (!label) error_tok(node->tok, "not a compile-time constant");
        if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC) error_tok(node->tok, "invalid initializer");
        if (node->var->is_local) error_tok(node->tok, "not a compile-time constant");
        *label = node->var;
        return 0;
    case ND_NUM:
        return node->val;
    default:
        break;
    }

    error_tok(node->tok, "not a compile-time constant");
    return 0;
}

static i64 eval_rval(Node *node, Obj **label) {
    switch (node->kind) {
    case ND_VAR:
        if (node->var->is_local) error_tok(node->tok, "not a compile-time constant");
        if (!label) error_tok(node->tok, "not a compile-time constant");
        *label = node->var;
        return 0;
    case ND_DEREF:
        return eval2(node->lhs, label);
    case ND_MEMBER:
        return eval_rval(node->lhs, label) + node->member->offset;
    default:
        break;
    }

    error_tok(node->tok, "invalid initializer");
    return 0;
}

static bool is_const_expr(Node *node) {
    add_type(node);

    switch (node->kind) {
    case ND_ADD:
    case ND_SUB:
    case ND_MUL:
    case ND_DIV:
    case ND_BITAND:
    case ND_BITOR:
    case ND_BITXOR:
    case ND_SHL:
    case ND_SHR:
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_LOGAND:
    case ND_LOGOR:
    case ND_MOD:
        return is_const_expr(node->lhs) && is_const_expr(node->rhs);
    case ND_COND:
        if (!is_const_expr(node->cond)) return false;
        return is_const_expr(eval(node->cond) ? node->then : node->els);
    case ND_COMMA:
        return is_const_expr(node->rhs);
    case ND_NEG:
    case ND_NOT:
    case ND_BITNOT:
    case ND_CAST:
        return is_const_expr(node->lhs);
    case ND_NUM:
        return true;
    default:
        return false;
    }
}

static i64 const_expr(Token **rest, Token *tok) {
    Node *node = conditional(rest, tok);
    return eval(node);
}

static double eval_double(Node *node) {
    add_type(node);

    if (is_integer(node->ty)) {
        if (node->ty->is_unsigned) return (double)(u64)eval(node);
        return (double)eval(node);
    }

    switch (node->kind) {
    case ND_ADD:
        return eval_double(node->lhs) + eval_double(node->rhs);
    case ND_SUB:
        return eval_double(node->lhs) - eval_double(node->rhs);
    case ND_MUL:
        return eval_double(node->lhs) * eval_double(node->rhs);
    case ND_DIV:
        return eval_double(node->lhs) / eval_double(node->rhs);
    case ND_NEG:
        return -eval_double(node->lhs);
    case ND_COND:
        return eval_double(node->cond) ? eval_double(node->then) : eval_double(node->els);
    case ND_COMMA:
        return eval_double(node->rhs);
    case ND_CAST: {
        double d;
        if (is_flonum(node->lhs->ty)) d = eval_double(node->lhs);
        else d = node->lhs->ty->is_unsigned && node->lhs->ty->size == 8 ? (double)(u64)eval(node->lhs) : (double)eval(node->lhs);
        if (node->ty->kind == TY_FLOAT) d = (double)(float)d;
        return d;
    }
    case ND_NUM:
        return node->fval;
    default:
        break;
    }

    error_tok(node->tok, "not a compile-time constant");
    return 0;
}

/* ---- expressions */

static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
    add_type(lhs);
    add_type(rhs);

    /* num + num */
    if (is_numeric(lhs->ty) && is_numeric(rhs->ty)) return new_binary(ND_ADD, lhs, rhs, tok);

    if (lhs->ty->base && rhs->ty->base) error_tok(tok, "invalid operands to binary +");

    /* canonicalize `num + ptr` to `ptr + num` */
    if (!lhs->ty->base && rhs->ty->base) {
        Node *tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }
    if (!lhs->ty->base || !is_integer(rhs->ty)) error_tok(tok, "invalid operands to binary +");
    if (lhs->ty->base->size < 0) error_tok(tok, "arithmetic on a pointer to an incomplete type");

    /* ptr + num */
    rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
    return new_binary(ND_ADD, lhs, rhs, tok);
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
    add_type(lhs);
    add_type(rhs);

    /* num - num */
    if (is_numeric(lhs->ty) && is_numeric(rhs->ty)) return new_binary(ND_SUB, lhs, rhs, tok);

    /* ptr - num */
    if (lhs->ty->base && is_integer(rhs->ty)) {
        if (lhs->ty->base->size < 0) error_tok(tok, "arithmetic on a pointer to an incomplete type");
        rhs = new_binary(ND_MUL, rhs, new_long(lhs->ty->base->size, tok), tok);
        add_type(rhs);
        Node *node = new_binary(ND_SUB, lhs, rhs, tok);
        node->ty = lhs->ty;
        return node;
    }

    /* ptr - ptr, which returns how many elements are between the two */
    if (lhs->ty->base && rhs->ty->base) {
        int sz = lhs->ty->base->size;
        if (sz < 0) error_tok(tok, "arithmetic on a pointer to an incomplete type");
        Node *node = new_binary(ND_SUB, lhs, rhs, tok);
        node->ty = ty_long;
        return new_binary(ND_DIV, node, new_long(sz ? sz : 1, tok), tok);
    }

    error_tok(tok, "invalid operands to binary -");
    return NULL;
}

/* Convert `A op= B` to `tmp = &A, *tmp = *tmp op B` where tmp is a fresh pointer variable */
static Node *to_assign(Node *binary) {
    add_type(binary->lhs);
    add_type(binary->rhs);
    Token *tok = binary->tok;

    if (is_const_type(binary->lhs->ty)) error_tok(tok, "assignment of read-only location");

    /* Convert `A.x op= C` to `tmp = &A, (*tmp).x = (*tmp).x op C` (the member may be a bit-field) */
    if (binary->lhs->kind == ND_MEMBER) {
        Obj *var = new_lvar("", pointer_to(binary->lhs->lhs->ty));

        Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok), new_unary(ND_ADDR, binary->lhs->lhs, tok), tok);

        Node *expr2 = new_unary(ND_MEMBER, new_unary(ND_DEREF, new_var_node(var, tok), tok), tok);
        expr2->member = binary->lhs->member;

        Node *expr3 = new_unary(ND_MEMBER, new_unary(ND_DEREF, new_var_node(var, tok), tok), tok);
        expr3->member = binary->lhs->member;

        Node *expr4 = new_binary(ND_ASSIGN, expr2, new_binary(binary->kind, expr3, binary->rhs, tok), tok);
        return new_binary(ND_COMMA, expr1, expr4, tok);
    }

    /* Convert `A op= B` to `tmp = &A, *tmp = *tmp op B` */
    Obj *var = new_lvar("", pointer_to(binary->lhs->ty));

    Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok), new_unary(ND_ADDR, binary->lhs, tok), tok);

    Node *expr2 = new_binary(ND_ASSIGN, new_unary(ND_DEREF, new_var_node(var, tok), tok),
                             new_binary(binary->kind, new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs, tok), tok);

    return new_binary(ND_COMMA, expr1, expr2, tok);
}

/* assign = conditional (assign-op assign)? */
static Node *assign(Token **rest, Token *tok) {
    Node *node = conditional(&tok, tok);

    if (equal(tok, "=")) {
        Node *rhs = assign(rest, tok->next);
        add_type(node);
        if (is_const_type(node->ty) && node->ty->kind != TY_ARRAY) error_tok(tok, "assignment of read-only location");
        return new_binary(ND_ASSIGN, node, rhs, tok);
    }

    if (equal(tok, "+=")) return to_assign(new_add(node, assign(rest, tok->next), tok));
    if (equal(tok, "-=")) return to_assign(new_sub(node, assign(rest, tok->next), tok));
    if (equal(tok, "*=")) return to_assign(new_binary(ND_MUL, node, assign(rest, tok->next), tok));
    if (equal(tok, "/=")) return to_assign(new_binary(ND_DIV, node, assign(rest, tok->next), tok));
    if (equal(tok, "%=")) return to_assign(new_binary(ND_MOD, node, assign(rest, tok->next), tok));
    if (equal(tok, "&=")) return to_assign(new_binary(ND_BITAND, node, assign(rest, tok->next), tok));
    if (equal(tok, "|=")) return to_assign(new_binary(ND_BITOR, node, assign(rest, tok->next), tok));
    if (equal(tok, "^=")) return to_assign(new_binary(ND_BITXOR, node, assign(rest, tok->next), tok));
    if (equal(tok, "<<=")) return to_assign(new_binary(ND_SHL, node, assign(rest, tok->next), tok));
    if (equal(tok, ">>=")) return to_assign(new_binary(ND_SHR, node, assign(rest, tok->next), tok));

    *rest = tok;
    return node;
}

/* conditional = logor ("?" expr? ":" conditional)? */
static Node *conditional(Token **rest, Token *tok) {
    Node *cond = logor(&tok, tok);

    if (!equal(tok, "?")) {
        *rest = tok;
        return cond;
    }

    if (equal(tok->next, ":")) {
        /* [GNU] Compile `a ?: b` as `tmp = a, tmp ? tmp : b` */
        add_type(cond);
        Obj *var = new_lvar("", cond->ty);
        Node *lhs = new_binary(ND_ASSIGN, new_var_node(var, tok), cond, tok);
        Node *rhs = new_node(ND_COND, tok);
        rhs->cond = new_var_node(var, tok);
        rhs->then = new_var_node(var, tok);
        rhs->els = conditional(rest, tok->next->next);
        return new_binary(ND_COMMA, lhs, rhs, tok);
    }

    Node *node = new_node(ND_COND, tok);
    node->cond = cond;
    node->then = expr(&tok, tok->next);
    tok = skip(tok, ":");
    node->els = conditional(rest, tok);
    return node;
}

static Node *logand(Token **rest, Token *tok);
static Node *bitor_(Token **rest, Token *tok);
static Node *bitxor_(Token **rest, Token *tok);
static Node *bitand_(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);

static Node *logor(Token **rest, Token *tok) {
    Node *node = logand(&tok, tok);
    while (equal(tok, "||")) {
        Token *start = tok;
        node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *logand(Token **rest, Token *tok) {
    Node *node = bitor_(&tok, tok);
    while (equal(tok, "&&")) {
        Token *start = tok;
        node = new_binary(ND_LOGAND, node, bitor_(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitor_(Token **rest, Token *tok) {
    Node *node = bitxor_(&tok, tok);
    while (equal(tok, "|")) {
        Token *start = tok;
        node = new_binary(ND_BITOR, node, bitxor_(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitxor_(Token **rest, Token *tok) {
    Node *node = bitand_(&tok, tok);
    while (equal(tok, "^")) {
        Token *start = tok;
        node = new_binary(ND_BITXOR, node, bitand_(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitand_(Token **rest, Token *tok) {
    Node *node = equality(&tok, tok);
    while (equal(tok, "&")) {
        Token *start = tok;
        node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *equality(Token **rest, Token *tok) {
    Node *node = relational(&tok, tok);

    for (;;) {
        Token *start = tok;

        if (equal(tok, "==")) {
            node = new_binary(ND_EQ, node, relational(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, "!=")) {
            node = new_binary(ND_NE, node, relational(&tok, tok->next), start);
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *relational(Token **rest, Token *tok) {
    Node *node = shift(&tok, tok);

    for (;;) {
        Token *start = tok;

        if (equal(tok, "<")) {
            node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, "<=")) {
            node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, ">")) {
            node = new_binary(ND_LT, shift(&tok, tok->next), node, start);
            continue;
        }

        if (equal(tok, ">=")) {
            node = new_binary(ND_LE, shift(&tok, tok->next), node, start);
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *shift(Token **rest, Token *tok) {
    Node *node = add(&tok, tok);

    for (;;) {
        Token *start = tok;

        if (equal(tok, "<<")) {
            node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, ">>")) {
            node = new_binary(ND_SHR, node, add(&tok, tok->next), start);
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *add(Token **rest, Token *tok) {
    Node *node = mul(&tok, tok);

    for (;;) {
        Token *start = tok;

        if (equal(tok, "+")) {
            node = new_add(node, mul(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, "-")) {
            node = new_sub(node, mul(&tok, tok->next), start);
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *mul(Token **rest, Token *tok) {
    Node *node = cast(&tok, tok);

    for (;;) {
        Token *start = tok;

        if (equal(tok, "*")) {
            node = new_binary(ND_MUL, node, cast(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, "/")) {
            node = new_binary(ND_DIV, node, cast(&tok, tok->next), start);
            continue;
        }

        if (equal(tok, "%")) {
            node = new_binary(ND_MOD, node, cast(&tok, tok->next), start);
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *cast(Token **rest, Token *tok) {
    if (equal(tok, "(") && is_typename(tok->next)) {
        Token *start = tok;
        Type *ty = typename(&tok, tok->next);
        tok = skip(tok, ")");

        /* compound literal */
        if (equal(tok, "{")) return unary(rest, start);

        /* type cast */
        Node *node = new_cast(cast(rest, tok), ty);
        node->tok = start;
        return node;
    }

    return unary(rest, tok);
}

/* Convert `A++` to `(typeof A)((A += 1) - 1)` */
static Node *new_inc_dec(Node *node, Token *tok, int addend) {
    add_type(node);
    return new_cast(new_add(to_assign(new_add(node, new_num(addend, tok), tok)), new_num(-addend, tok), tok), node->ty);
}

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "+")) {
        Node *node = cast(rest, tok->next);
        add_type(node);
        if (is_integer(node->ty) && (node->ty->size < 4 || node->ty->kind == TY_ENUM)) return new_cast(node, ty_int);
        return node;
    }

    if (equal(tok, "-")) return new_unary(ND_NEG, cast(rest, tok->next), tok);

    if (equal(tok, "&")) {
        Node *lhs = cast(rest, tok->next);
        add_type(lhs);
        if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) error_tok(tok, "cannot take address of bit-field");
        return new_unary(ND_ADDR, lhs, tok);
    }

    if (equal(tok, "*")) {
        /* dereferencing a function does nothing: `*foo`, `**foo` are all the same as `foo` */
        Node *node = cast(rest, tok->next);
        add_type(node);
        if (node->ty->kind == TY_FUNC) return node;
        return new_unary(ND_DEREF, node, tok);
    }

    if (equal(tok, "!")) return new_unary(ND_NOT, cast(rest, tok->next), tok);

    if (equal(tok, "~")) return new_unary(ND_BITNOT, cast(rest, tok->next), tok);

    /* pre-increment and pre-decrement */
    if (equal(tok, "++")) return to_assign(new_add(unary(rest, tok->next), new_num(1, tok), tok));

    if (equal(tok, "--")) return to_assign(new_sub(unary(rest, tok->next), new_num(1, tok), tok));

    return postfix(rest, tok);
}

/* struct-ref = postfix-expr "." ident; `x->y` is `(*x).y` */
static Node *struct_ref(Node *node, Token *tok) {
    add_type(node);
    if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION) error_tok(node->tok, "not a struct nor a union");
    if (node->ty->size < 0) error_tok(node->tok, "member access in an incomplete type");

    Type *ty = node->ty;

    for (;;) {
        Member *mem = get_struct_member(ty, tok);
        if (!mem) error_tok(tok, "no such member");
        node = new_unary(ND_MEMBER, node, tok);
        node->member = mem;
        if (mem->name) break;
        ty = mem->ty;
    }
    return node;
}

/* Convert op= assignments etc. need the node type; function calls are checked against the prototype */
static Node *funcall(Token **rest, Token *tok, Node *fn) {
    add_type(fn);

    if (fn->ty->kind != TY_FUNC && (fn->ty->kind != TY_PTR || fn->ty->base->kind != TY_FUNC)) error_tok(fn->tok, "called object is not a function");

    Type *ty = (fn->ty->kind == TY_FUNC) ? fn->ty : fn->ty->base;
    Type *param_ty = ty->params;

    Node head;
    memset(&head, 0, sizeof head);
    Node *cur = &head;

    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");

        Node *arg = assign(&tok, tok);
        add_type(arg);

        if (!param_ty && !ty->is_variadic && ty->has_proto) error_tok(tok, "too many arguments to function call");

        if (param_ty) {
            if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION) arg = new_cast(arg, param_ty);
            param_ty = param_ty->next;
        } else if (arg->ty->kind == TY_FLOAT) {
            /* arguments without a parameter (in "..." or in an unprototyped call) get the default promotions */
            arg = new_cast(arg, ty_double);
        } else if (is_integer(arg->ty) && (arg->ty->size < 4 || arg->ty->kind == TY_ENUM)) {
            arg = new_cast(arg, ty_int);
        }

        cur = cur->next = arg;
    }

    if (param_ty) error_tok(tok, "too few arguments to function call");

    *rest = skip(tok, ")");

    Node *node = new_unary(ND_FUNCALL, fn, tok);
    node->func_ty = ty;
    node->ty = ty->return_ty;
    node->args = head.next;

    /* if a function returns a struct, the caller allocates a space for the return value */
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) node->ret_buffer = new_lvar("", node->ty);
    return node;
}

/* postfix = "(" type-name ")" "{" initializer-list "}" | primary ("[" expr "]" | "." ident | "->" ident | "++" | "--")* */
static Node *postfix(Token **rest, Token *tok) {
    if (equal(tok, "(") && is_typename(tok->next)) {
        /* compound literal */
        Token *start = tok;
        Type *ty = typename(&tok, tok->next);
        tok = skip(tok, ")");

        if (scope->next == NULL) {
            Obj *var = new_anon_gvar(ty);
            gvar_initializer(rest, tok, var);
            return new_var_node(var, start);
        }

        Obj *var = new_lvar("", ty);
        Node *lhs = lvar_initializer(rest, tok, var);
        Node *rhs = new_var_node(var, tok);
        return new_binary(ND_COMMA, lhs, rhs, start);
    }

    Node *node = primary(&tok, tok);

    for (;;) {
        if (equal(tok, "(")) {
            node = funcall(&tok, tok->next, node);
            continue;
        }

        if (equal(tok, "[")) {
            /* x[y] is short for *(x+y) */
            Token *start = tok;
            Node *idx = expr(&tok, tok->next);
            tok = skip(tok, "]");
            node = new_unary(ND_DEREF, new_add(node, idx, start), start);
            continue;
        }

        if (equal(tok, ".")) {
            node = struct_ref(node, tok->next);
            tok = tok->next->next;
            continue;
        }

        if (equal(tok, "->")) {
            /* x->y is short for (*x).y */
            node = new_unary(ND_DEREF, node, tok);
            node = struct_ref(node, tok->next);
            tok = tok->next->next;
            continue;
        }

        if (equal(tok, "++")) {
            node = new_inc_dec(node, tok, 1);
            tok = tok->next;
            continue;
        }

        if (equal(tok, "--")) {
            node = new_inc_dec(node, tok, -1);
            tok = tok->next;
            continue;
        }

        *rest = tok;
        return node;
    }
}

static Node *generic_selection(Token **rest, Token *tok) {
    Token *start = tok;
    tok = skip(tok->next, "(");

    Node *ctrl = assign(&tok, tok);
    add_type(ctrl);

    Type *t1 = ctrl->ty;
    if (t1->kind == TY_FUNC) t1 = pointer_to(t1);
    else if (t1->kind == TY_ARRAY) t1 = pointer_to(t1->base);

    Node *ret = NULL;

    while (consume(rest, tok, ",")) {
        tok = *rest;
        if (equal(tok, "default")) {
            tok = skip(tok->next, ":");
            Node *node = assign(&tok, tok);
            if (!ret) ret = node;
            continue;
        }

        Type *t2 = typename(&tok, tok);
        tok = skip(tok, ":");
        Node *node = assign(&tok, tok);
        if (is_compatible(t1, t2)) ret = node;
    }

    if (!ret) error_tok(start, "controlling expression type not compatible with any generic association type");

    *rest = skip(tok, ")");
    return ret;
}

/* ---- builtins */

static Node *builtin_node(int op, Type *ty, Node *arg0, Node *arg1, Token *tok) {
    Node *node = new_node(ND_BUILTIN, tok);
    node->op = op;
    node->ty = ty;
    node->lhs = arg0;
    node->rhs = arg1;
    add_type(arg0);
    add_type(arg1);
    return node;
}

enum {
    BI_UNREACHABLE = 1, BI_TRAP, BI_BSWAP16, BI_BSWAP32, BI_BSWAP64, BI_CLZ, BI_CTZ, BI_POPCOUNT, BI_FFS, BI_FRAME_ADDRESS,
    BI_RETURN_ADDRESS, BI_FABS, BI_SQRT, BI_CLZ64, BI_CTZ64, BI_POPCOUNT64, BI_FFS64, BI_FENCE,
};

static Node *double_const(double d, Type *ty, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->fval = d;
    node->ty = ty;
    return node;
}

static double mcc_inf(void) {
    u64 bits = 0x7FF0000000000000ULL;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static double mcc_nan(void) {
    u64 bits = 0x7FF8000000000000ULL;
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

/* Evaluates `arg` once into a fresh temporary: returns the assignment; *var gets the temporary. */
static Node *spill(Node *arg, Obj **var, Token *tok) {
    add_type(arg);
    *var = new_lvar("", arg->ty);
    return new_binary(ND_ASSIGN, new_var_node(*var, tok), arg, tok);
}

static Node *read_offsetof(Token **rest, Token *tok) {
    Token *start = tok;
    tok = skip(tok->next, "(");
    Type *ty = typename(&tok, tok);
    tok = skip(tok, ",");
    i64 offset = 0;
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) error_tok(start, "offsetof needs a struct or union type");
    bool first = true;
    for (;;) {
        if (equal(tok, ")")) break;
        if (equal(tok, "[")) {
            Node *e = expr(&tok, tok->next);
            tok = skip(tok, "]");
            if (ty->kind != TY_ARRAY) error_tok(start, "offsetof: not an array");
            offset += eval(e) * ty->base->size;
            ty = ty->base;
            continue;
        }
        if (equal(tok, ".")) tok = tok->next;
        else if (!first) error_tok(tok, "expected '.' or '[' in offsetof");
        first = false;
        if (tok->kind != TK_IDENT) error_tok(tok, "expected a member name");
        if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) error_tok(tok, "offsetof: not a struct");
        for (;;) {
            Member *mem = get_struct_member(ty, tok);
            if (!mem) error_tok(tok, "no such member");
            offset += mem->offset;
            ty = mem->ty;
            if (mem->name) break;
        }
        tok = tok->next;
    }
    *rest = skip(tok, ")");
    return new_ulong(offset, start);
}

/* Returns the node for a __builtin_* call, or NULL if tok is not a builtin we know. */
static Node *builtin_call(Token **rest, Token *tok) {
    Token *start = tok;
    char *name = xstrndup(tok->loc, (size_t)tok->len);

    if (!strcmp(name, "__builtin_offsetof")) return read_offsetof(rest, tok);

    if (!strcmp(name, "__builtin_va_arg")) {
        tok = skip(tok->next, "(");
        Node *ap = assign(&tok, tok);
        tok = skip(tok, ",");
        Type *ty = typename(&tok, tok);
        *rest = skip(tok, ")");
        add_type(ap);
        Node *node = new_unary(ND_VA_ARG, ap, start);
        node->ty = ty;
        return node;
    }

    if (!strcmp(name, "__builtin_types_compatible_p")) {
        tok = skip(tok->next, "(");
        Type *t1 = typename(&tok, tok);
        tok = skip(tok, ",");
        Type *t2 = typename(&tok, tok);
        *rest = skip(tok, ")");
        return new_num(is_compatible(t1, t2), start);
    }

    if (!strcmp(name, "__builtin_choose_expr")) {
        tok = skip(tok->next, "(");
        Node *c = assign(&tok, tok);
        tok = skip(tok, ",");
        Node *a = assign(&tok, tok);
        tok = skip(tok, ",");
        Node *b = assign(&tok, tok);
        *rest = skip(tok, ")");
        return eval(c) ? a : b;
    }

    /* the rest take an ordinary argument list */
    if (!str_starts(name, "__builtin_")) return NULL;
    if (!equal(tok->next, "(")) return NULL;

    Node *args[4];
    int nargs = 0;
    tok = tok->next->next;
    if (!strcmp(name, "__builtin_va_start") || !strcmp(name, "__builtin_va_end") || !strcmp(name, "__builtin_va_copy")) {
        while (!equal(tok, ")")) {
            if (nargs) tok = skip(tok, ",");
            if (nargs < 4) args[nargs++] = assign(&tok, tok);
            else assign(&tok, tok);
        }
        *rest = skip(tok, ")");
        if (!strcmp(name, "__builtin_va_start")) {
            if (!current_fn || !current_fn->va_area) error_tok(start, "va_start used in a function that is not variadic");
            add_type(args[0]);
            Node *node = new_unary(ND_VA_START, args[0], start);
            node->ty = ty_void;
            return node;
        }
        if (!strcmp(name, "__builtin_va_end")) {
            Node *node = new_cast(args[0], ty_void);
            return node;
        }
        if (nargs < 2) error_tok(start, "va_copy needs two arguments");
        return new_binary(ND_ASSIGN, new_unary(ND_DEREF, args[0], start), new_unary(ND_DEREF, args[1], start), start);
    }

    while (!equal(tok, ")")) {
        if (nargs) tok = skip(tok, ",");
        Node *a = assign(&tok, tok);
        if (nargs < 4) args[nargs++] = a;
    }
    *rest = skip(tok, ")");

    if (!strcmp(name, "__builtin_expect") || !strcmp(name, "__builtin_expect_with_probability")) {
        if (nargs < 1) error_tok(start, "too few arguments");
        return args[0];
    }
    if (!strcmp(name, "__builtin_assume") || !strcmp(name, "__builtin_prefetch")) return new_num(0, start);
    if (!strcmp(name, "__builtin_unreachable")) return builtin_node(BI_UNREACHABLE, ty_void, NULL, NULL, start);
    if (!strcmp(name, "__builtin_trap") || !strcmp(name, "__builtin_abort")) return builtin_node(BI_TRAP, ty_void, NULL, NULL, start);
    if (!strcmp(name, "__builtin_constant_p")) {
        if (nargs < 1) error_tok(start, "too few arguments");
        return new_num(is_const_expr(args[0]) ? 1 : 0, start);
    }
    if (!strcmp(name, "__builtin_huge_val") || !strcmp(name, "__builtin_inf")) return double_const(mcc_inf(), ty_double, start);
    if (!strcmp(name, "__builtin_huge_valf") || !strcmp(name, "__builtin_inff")) return double_const(mcc_inf(), ty_float, start);
    if (!strcmp(name, "__builtin_nan") || !strcmp(name, "__builtin_nans")) return double_const(mcc_nan(), ty_double, start);
    if (!strcmp(name, "__builtin_nanf") || !strcmp(name, "__builtin_nansf")) return double_const(mcc_nan(), ty_float, start);
    if (!strcmp(name, "__builtin_thread_pointer")) {
        Node *node = new_node(ND_THREAD_PTR, start);
        node->ty = pointer_to(ty_void);
        return node;
    }
    if (!strcmp(name, "__builtin_bswap16") && nargs == 1) return builtin_node(BI_BSWAP16, ty_ushort, new_cast(args[0], ty_ushort), NULL, start);
    if (!strcmp(name, "__builtin_bswap32") && nargs == 1) return builtin_node(BI_BSWAP32, ty_uint, new_cast(args[0], ty_uint), NULL, start);
    if (!strcmp(name, "__builtin_bswap64") && nargs == 1) return builtin_node(BI_BSWAP64, ty_ulong, new_cast(args[0], ty_ulong), NULL, start);
    if ((!strcmp(name, "__builtin_clz")) && nargs == 1) return builtin_node(BI_CLZ, ty_int, new_cast(args[0], ty_uint), NULL, start);
    if ((!strcmp(name, "__builtin_clzl") || !strcmp(name, "__builtin_clzll")) && nargs == 1) return builtin_node(BI_CLZ64, ty_int, new_cast(args[0], ty_ulong), NULL, start);
    if ((!strcmp(name, "__builtin_ctz")) && nargs == 1) return builtin_node(BI_CTZ, ty_int, new_cast(args[0], ty_uint), NULL, start);
    if ((!strcmp(name, "__builtin_ctzl") || !strcmp(name, "__builtin_ctzll")) && nargs == 1) return builtin_node(BI_CTZ64, ty_int, new_cast(args[0], ty_ulong), NULL, start);
    if ((!strcmp(name, "__builtin_popcount")) && nargs == 1) return builtin_node(BI_POPCOUNT, ty_int, new_cast(args[0], ty_uint), NULL, start);
    if ((!strcmp(name, "__builtin_popcountl") || !strcmp(name, "__builtin_popcountll")) && nargs == 1) return builtin_node(BI_POPCOUNT64, ty_int, new_cast(args[0], ty_ulong), NULL, start);
    if ((!strcmp(name, "__builtin_ffs")) && nargs == 1) return builtin_node(BI_FFS, ty_int, new_cast(args[0], ty_uint), NULL, start);
    if ((!strcmp(name, "__builtin_ffsl") || !strcmp(name, "__builtin_ffsll")) && nargs == 1) return builtin_node(BI_FFS64, ty_int, new_cast(args[0], ty_ulong), NULL, start);
    if (!strcmp(name, "__builtin_frame_address")) return builtin_node(BI_FRAME_ADDRESS, pointer_to(ty_void), NULL, NULL, start);
    if (!strcmp(name, "__builtin_return_address")) return builtin_node(BI_RETURN_ADDRESS, pointer_to(ty_void), NULL, NULL, start);
    if ((!strcmp(name, "__builtin_fabs") || !strcmp(name, "__builtin_fabsl")) && nargs == 1) return builtin_node(BI_FABS, ty_double, new_cast(args[0], ty_double), NULL, start);
    if (!strcmp(name, "__builtin_fabsf") && nargs == 1) return builtin_node(BI_FABS, ty_float, new_cast(args[0], ty_float), NULL, start);
    if ((!strcmp(name, "__builtin_sqrt") || !strcmp(name, "__builtin_sqrtl")) && nargs == 1) return builtin_node(BI_SQRT, ty_double, new_cast(args[0], ty_double), NULL, start);
    if (!strcmp(name, "__builtin_sqrtf") && nargs == 1) return builtin_node(BI_SQRT, ty_float, new_cast(args[0], ty_float), NULL, start);
    if (!strcmp(name, "__builtin_isnan") && nargs == 1) {
        Obj *tmp;
        Node *set = spill(new_cast(args[0], ty_double), &tmp, start);
        Node *test = new_binary(ND_NE, new_var_node(tmp, start), new_var_node(tmp, start), start);
        return new_binary(ND_COMMA, set, test, start);
    }
    if ((!strcmp(name, "__builtin_isinf") || !strcmp(name, "__builtin_isinf_sign")) && nargs == 1) {
        Obj *tmp;
        Node *set = spill(new_cast(args[0], ty_double), &tmp, start);
        Node *pinf = new_binary(ND_EQ, new_var_node(tmp, start), double_const(mcc_inf(), ty_double, start), start);
        Node *ninf = new_binary(ND_EQ, new_var_node(tmp, start), double_const(-mcc_inf(), ty_double, start), start);
        return new_binary(ND_COMMA, set, new_binary(ND_LOGOR, pinf, ninf, start), start);
    }
    if (!strcmp(name, "__builtin_isfinite") && nargs == 1) {
        Obj *tmp;
        Node *set = spill(new_cast(args[0], ty_double), &tmp, start);
        Node *notnan = new_binary(ND_EQ, new_var_node(tmp, start), new_var_node(tmp, start), start);
        Node *lt = new_binary(ND_LT, new_var_node(tmp, start), double_const(mcc_inf(), ty_double, start), start);
        Node *gt = new_binary(ND_LT, double_const(-mcc_inf(), ty_double, start), new_var_node(tmp, start), start);
        return new_binary(ND_COMMA, set, new_binary(ND_LOGAND, notnan, new_binary(ND_LOGAND, lt, gt, start), start), start);
    }
    if (!strcmp(name, "__builtin_alloca") && nargs == 1) {
        Node *node = new_unary(ND_ALLOCA, new_cast(args[0], ty_ulong), start);
        add_type(node->lhs);
        node->ty = pointer_to(ty_void);
        return node;
    }

    return NULL;
}

/* primary = "(" "{" stmt+ "}" ")" | "(" expr ")" | "sizeof" ... | ident | str | num */
static Node *primary(Token **rest, Token *tok) {
    Token *start = tok;

    if (equal(tok, "(") && equal(tok->next, "{")) {
        /* [GNU] statement expression */
        Node *node = new_node(ND_STMT_EXPR, tok);
        node->body = compound_stmt(&tok, tok->next->next)->body;
        *rest = skip(tok, ")");
        return node;
    }

    if (equal(tok, "(")) {
        Node *node = expr(&tok, tok->next);
        *rest = skip(tok, ")");
        return node;
    }

    if (equal(tok, "sizeof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
        Type *ty = typename(&tok, tok->next->next);
        *rest = skip(tok, ")");
        if (ty->size < 0 && ty->kind != TY_VOID) error_tok(start, "invalid application of 'sizeof' to an incomplete type");
        return new_ulong(ty->size, start);
    }

    if (equal(tok, "sizeof")) {
        Node *node = unary(rest, tok->next);
        add_type(node);
        if (node->ty->size < 0) error_tok(start, "invalid application of 'sizeof' to an incomplete type");
        return new_ulong(node->ty->size, tok);
    }

    if (equal(tok, "_Alignof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
        Type *ty = typename(&tok, tok->next->next);
        *rest = skip(tok, ")");
        return new_ulong(ty->align, tok);
    }

    if (equal(tok, "_Alignof")) {
        Node *node = unary(rest, tok->next);
        add_type(node);
        return new_ulong(node->ty->align, tok);
    }

    if (equal(tok, "_Generic")) return generic_selection(rest, tok);

    if (tok->kind == TK_IDENT) {
        /* __builtin_xxx intrinsics that are not ordinary functions */
        if (tok->len > 10 && !strncmp(tok->loc, "__builtin_", 10)) {
            VarScope *known = find_var(tok);
            if (!known) {
                Node *b = builtin_call(rest, tok);
                if (b) return b;
            }
        }

        /* variable or enum constant */
        VarScope *sc = find_var(tok);
        *rest = tok->next;
        if (sc) {
            if (sc->var) return new_var_node(sc->var, tok);
            if (sc->enum_ty) return new_num(sc->enum_val, tok);
        }

        if (equal(tok->next, "(")) {
            /* an undeclared function is implicitly declared `int name()` (C89) */
            char *name = get_ident(tok);
            if (tok->len > 10 && !strncmp(tok->loc, "__builtin_", 10)) {
                /* __builtin_memcpy and friends: the library function */
                VarScope *lib = NULL;
                Token fake = *tok;
                fake.loc = tok->loc + 10;
                fake.len = tok->len - 10;
                lib = find_var(&fake);
                if (lib && lib->var) return new_var_node(lib->var, tok);
                error_tok(tok, "unknown builtin function %s", name);
            }
            Type *fty = func_type(ty_int);
            fty->has_proto = false;
            Scope *saved = scope;
            scope = &global_scope;
            Obj *fn = new_gvar(name, fty);
            scope = saved;
            fn->is_function = true;
            fn->is_definition = false;
            return new_var_node(fn, tok);
        }
        error_tok(tok, "'%.*s' undeclared", tok->len, tok->loc);
    }

    if (tok->kind == TK_STR) {
        Obj *var = new_string_literal(tok->str, tok->ty);
        *rest = tok->next;
        return new_var_node(var, tok);
    }

    if (tok->kind == TK_NUM) {
        Node *node;
        if (is_flonum(tok->ty)) {
            node = new_node(ND_NUM, tok);
            node->fval = tok->fval;
        } else {
            node = new_num(tok->val, tok);
        }
        node->ty = tok->ty;
        *rest = tok->next;
        return node;
    }

    if (tok->kind == TK_PUNCT && (tok->loc[0] == '"' || tok->loc[0] == '\'')) error_tok(tok, "unclosed %s literal", tok->loc[0] == '"' ? "string" : "char");
    error_tok(tok, "expected an expression");
    return NULL;
}

/* expr = assign ("," expr)? */
static Node *expr(Token **rest, Token *tok) {
    Node *node = assign(&tok, tok);

    if (equal(tok, ",")) return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);

    *rest = tok;
    return node;
}

/* ---- statements */

static Node *expr_stmt(Token **rest, Token *tok) {
    if (equal(tok, ";")) {
        *rest = tok->next;
        return new_node(ND_BLOCK, tok);
    }

    Node *node = new_node(ND_EXPR_STMT, tok);
    node->lhs = expr(&tok, tok);
    *rest = skip(tok, ";");
    return node;
}

static Node *asm_stmt(Token **rest, Token *tok) {
    Node *node = new_node(ND_ASM, tok);
    tok = tok->next;

    while (equal(tok, "volatile") || equal(tok, "inline") || equal(tok, "goto")) tok = tok->next;

    tok = skip(tok, "(");
    if (tok->kind != TK_STR || tok->ty->base->size != 1) error_tok(tok, "expected string literal");
    node->str = tok->str;
    tok = tok->next;

    int level = 1;
    while (level > 0) {
        if (tok->kind == TK_EOF) error_tok(tok, "unterminated asm");
        if (equal(tok, "(")) level++;
        else if (equal(tok, ")")) level--;
        if (level > 0) tok = tok->next;
    }
    tok = tok->next;
    *rest = skip(tok, ";");
    return node;
}

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN, tok);
        if (consume(rest, tok->next, ";")) return node;

        Node *exp = expr(&tok, tok->next);
        *rest = skip(tok, ";");

        add_type(exp);
        Type *ty = current_fn->ty->return_ty;
        if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) exp = new_cast(exp, ty);

        node->lhs = exp;
        return node;
    }

    if (equal(tok, "if")) {
        Node *node = new_node(ND_IF, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        if (equal(tok, "else")) node->els = stmt(&tok, tok->next);
        *rest = tok;
        return node;
    }

    if (equal(tok, "switch")) {
        Node *node = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        add_type(node->cond);
        if (!is_integer(node->cond->ty)) error_tok(node->tok, "switch quantity is not an integer");
        if (node->cond->ty->size < 8) node->cond = new_cast(node->cond, node->cond->ty->size == 4 && node->cond->ty->kind == TY_INT ? node->cond->ty : ty_int);

        Node *sw = current_switch;
        current_switch = node;

        int brk = cur_brk_label;
        node->brk_label = cur_brk_label = next_label();

        node->then = stmt(rest, tok);

        current_switch = sw;
        cur_brk_label = brk;
        return node;
    }

    if (equal(tok, "case")) {
        if (!current_switch) error_tok(tok, "stray case");

        Node *node = new_node(ND_CASE, tok);
        i64 begin = const_expr(&tok, tok->next);
        i64 end;

        if (equal(tok, "...")) {
            /* [GNU] case ranges, e.g. "case 1 ... 5:" */
            end = const_expr(&tok, tok->next);
            if (end < begin) error_tok(tok, "empty case range specified");
        } else {
            end = begin;
        }

        tok = skip(tok, ":");
        node->label_id = next_label();
        node->lhs = stmt(rest, tok);
        node->val = begin;
        node->val2 = end;
        node->case_next = current_switch->case_next;
        current_switch->case_next = node;
        return node;
    }

    if (equal(tok, "default")) {
        if (!current_switch) error_tok(tok, "stray default");

        Node *node = new_node(ND_CASE, tok);
        tok = skip(tok->next, ":");
        node->label_id = next_label();
        node->lhs = stmt(rest, tok);
        current_switch->default_case = node;
        return node;
    }

    if (equal(tok, "for")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");

        enter_scope();

        int brk = cur_brk_label;
        int cont = cur_cont_label;
        node->brk_label = cur_brk_label = next_label();
        node->cont_label = cur_cont_label = next_label();

        if (is_typename(tok)) {
            Type *basety = declspec(&tok, tok, NULL);
            node->init = declaration(&tok, tok, basety, NULL);
        } else {
            node->init = expr_stmt(&tok, tok);
        }

        if (!equal(tok, ";")) node->cond = expr(&tok, tok);
        tok = skip(tok, ";");

        if (!equal(tok, ")")) node->inc = expr(&tok, tok);
        tok = skip(tok, ")");

        node->then = stmt(rest, tok);

        leave_scope();
        cur_brk_label = brk;
        cur_cont_label = cont;
        return node;
    }

    if (equal(tok, "while")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");

        int brk = cur_brk_label;
        int cont = cur_cont_label;
        node->brk_label = cur_brk_label = next_label();
        node->cont_label = cur_cont_label = next_label();

        node->then = stmt(rest, tok);

        cur_brk_label = brk;
        cur_cont_label = cont;
        return node;
    }

    if (equal(tok, "do")) {
        Node *node = new_node(ND_DO, tok);

        int brk = cur_brk_label;
        int cont = cur_cont_label;
        node->brk_label = cur_brk_label = next_label();
        node->cont_label = cur_cont_label = next_label();

        node->then = stmt(&tok, tok->next);

        cur_brk_label = brk;
        cur_cont_label = cont;

        tok = skip(tok, "while");
        tok = skip(tok, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        *rest = skip(tok, ";");
        return node;
    }

    if (equal(tok, "asm")) return asm_stmt(rest, tok);

    if (equal(tok, "goto")) {
        if (equal(tok->next, "*")) error_tok(tok, "computed goto is not supported");
        Node *node = new_node(ND_GOTO, tok);
        node->label = get_ident(tok->next);
        node->goto_next = gotos;
        gotos = node;
        *rest = skip(tok->next->next, ";");
        return node;
    }

    if (equal(tok, "break")) {
        if (!cur_brk_label) error_tok(tok, "stray break");
        Node *node = new_node(ND_GOTO, tok);
        node->label_id = cur_brk_label;
        *rest = skip(tok->next, ";");
        return node;
    }

    if (equal(tok, "continue")) {
        if (!cur_cont_label) error_tok(tok, "stray continue");
        Node *node = new_node(ND_GOTO, tok);
        node->label_id = cur_cont_label;
        *rest = skip(tok->next, ";");
        return node;
    }

    if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
        Node *node = new_node(ND_LABEL, tok);
        node->label = xstrndup(tok->loc, (size_t)tok->len);
        node->label_id = next_label();
        node->goto_next = labels;
        labels = node;
        Token *after = tok->next->next;
        if (equal(after, "}")) {
            node->lhs = new_node(ND_BLOCK, after);
            *rest = after;
        } else {
            node->lhs = stmt(rest, after);
        }
        return node;
    }

    if (equal(tok, "{")) return compound_stmt(rest, tok->next);

    return expr_stmt(rest, tok);
}

static Node *compound_stmt(Token **rest, Token *tok) {
    Node *node = new_node(ND_BLOCK, tok);
    Node head;
    memset(&head, 0, sizeof head);
    Node *cur = &head;

    enter_scope();

    while (!equal(tok, "}")) {
        if (tok->kind == TK_EOF) error_tok(tok, "unclosed block");

        if (equal(tok, "_Static_assert")) {
            tok = skip(tok->next, "(");
            Node *e = conditional(&tok, tok);
            bool ok = eval(e) != 0;
            char *msg = "";
            if (equal(tok, ",")) {
                msg = tok->next->str ? tok->next->str : "";
                tok = tok->next->next;
            }
            if (!ok) error_tok(tok, "static assertion failed: %s", msg);
            tok = skip(tok, ")");
            tok = skip(tok, ";");
            continue;
        }

        if (is_typename(tok) && !equal(tok->next, ":")) {
            VarAttr attr;
            memset(&attr, 0, sizeof attr);
            Type *basety = declspec(&tok, tok, &attr);

            if (attr.is_typedef) {
                tok = parse_typedef(tok, basety);
                continue;
            }

            if (is_function(tok)) {
                tok = function(tok, basety, &attr);
                continue;
            }

            if (attr.is_extern) {
                tok = global_variable(tok, basety, &attr);
                continue;
            }

            cur = cur->next = declaration(&tok, tok, basety, &attr);
        } else {
            cur = cur->next = stmt(&tok, tok);
        }
        add_type(cur);
    }

    leave_scope();

    node->body = head.next;
    *rest = tok->next;
    return node;
}

/* ---- functions and the top level */

static void create_param_lvars(Type *param) {
    if (param) {
        create_param_lvars(param->next);
        if (!param->name) {
            new_lvar("", param);
            return;
        }
        new_lvar(get_ident(param->name), param);
    }
}

static void resolve_goto_labels(void) {
    for (Node *x = gotos; x; x = x->goto_next) {
        for (Node *y = labels; y; y = y->goto_next) {
            if (!strcmp(x->label, y->label)) {
                x->label_id = y->label_id;
                break;
            }
        }

        if (x->label_id == 0) error_tok(x->tok->next, "use of undeclared label");
    }

    gotos = labels = NULL;
}

static Token *function(Token *tok, Type *basety, VarAttr *attr) {
    Type *ty = declarator(&tok, tok, basety);
    if (!ty->name) error_tok(ty->name_pos, "function name omitted");
    char *name_str = get_ident(ty->name);
    tok = skip_asm_label(tok);

    Obj *fn = find_global(name_str);
    if (fn) {
        /* redeclaration */
        if (!fn->is_function) error_tok(ty->name, "'%s' redeclared as a different kind of symbol", name_str);
        if (fn->is_definition && equal(tok, "{")) error_tok(ty->name, "redefinition of '%s'", name_str);
        fn->is_definition = fn->is_definition || equal(tok, "{");
        if (equal(tok, "{") || !fn->ty->has_proto) fn->ty = ty;
    } else {
        Scope *saved = scope;
        scope = &global_scope;
        fn = new_gvar(name_str, ty);
        scope = saved;
        fn->is_function = true;
        fn->is_definition = equal(tok, "{");
        fn->tok = ty->name;
    }
    if (attr->is_static) fn->decl_static = true;
    if (attr->is_inline) fn->is_inline = true;
    if (!attr->is_inline || attr->is_extern) fn->decl_noninline = true;
    if (attr->is_weak) fn->is_weak = true;

    if (consume(&tok, tok, ";")) return tok;

    if (!equal(tok, "{")) error_tok(tok, "expected a function body or ';'");

    current_fn = fn;
    locals = NULL;
    enter_scope();
    in_param_decl = false;
    create_param_lvars(ty->params);

    /* a buffer for a struct/union return value is passed as the hidden first parameter */
    Type *rty = ty->return_ty;
    if ((rty->kind == TY_STRUCT || rty->kind == TY_UNION) && rty->size > 16) new_lvar("", pointer_to(rty));

    fn->params = locals;

    if (ty->is_variadic) {
        fn->va_area = new_lvar("__va_area__", array_of(ty_char, 176));
        fn->va_area->align = 16;
    }

    tok = skip(tok, "{");

    /* __func__ is a local string holding the function name */
    push_scope("__func__")->var = new_string_literal(fn->name, array_of(ty_char, (int)strlen(fn->name) + 1));
    push_scope("__FUNCTION__")->var = new_string_literal(fn->name, array_of(ty_char, (int)strlen(fn->name) + 1));
    push_scope("__PRETTY_FUNCTION__")->var = new_string_literal(fn->name, array_of(ty_char, (int)strlen(fn->name) + 1));

    fn->body = compound_stmt(&tok, tok);
    fn->locals = locals;
    leave_scope();
    resolve_goto_labels();
    current_fn = NULL;
    return tok;
}

static Token *global_variable(Token *tok, Type *basety, VarAttr *attr) {
    bool first = true;

    while (!consume(&tok, tok, ";")) {
        if (!first) tok = skip(tok, ",");
        first = false;

        Type *ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(ty->name_pos, "variable name omitted");
        tok = skip_asm_label(tok);
        AttrSet as;
        memset(&as, 0, sizeof as);
        tok = parse_attributes(tok, &as);
        char *name = get_ident(ty->name);

        Obj *var = find_global(name);
        if (var && !var->is_function) {
            /* a second declaration of a global: merge (the type may complete an array of unknown size) */
            if (var->ty->kind == TY_ARRAY && var->ty->array_len < 0 && ty->kind == TY_ARRAY) var->ty = ty;
            else if (var->ty->size < 0 && ty->size >= 0) var->ty = ty;
        } else {
            if (var) error_tok(ty->name, "'%s' redeclared as a different kind of symbol", name);
            Scope *saved = scope;
            scope = &global_scope;
            var = new_gvar(name, ty);
            scope = saved;
            var->tok = ty->name;
        }

        if (!attr->is_extern) var->is_definition = true;
        if (attr->is_static) var->is_static = true;
        if (attr->is_tls) var->is_tls = true;
        if (attr->is_weak || as.weak) var->is_weak = true;
        if (attr->align) var->align = attr->align;
        if (as.align > var->align) var->align = as.align;

        if (equal(tok, "=")) {
            if (attr->is_extern) var->is_definition = true;
            gvar_initializer(&tok, tok->next, var);
            var->is_const_data = is_const_type(var->ty) && !var->is_tls;
            var->is_tentative = false;
        } else if (!attr->is_extern && !var->init_data) {
            var->is_tentative = true;
        }
    }
    return tok;
}

static void mark_live(Obj *var) {
    if (var->is_live) return;
    var->is_live = true;
    for (int i = 0; i < var->nrefs; i++) mark_live(var->refs[i]);
}

/* Builds `__builtin_va_list`: struct { unsigned gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; }[1] */
static Type *make_va_list_type(void) {
    Type *st = struct_type();
    st->kind = TY_STRUCT;
    Member *m[4];
    Type *tys[4] = {ty_uint, ty_uint, pointer_to(ty_void), pointer_to(ty_void)};
    int offs[4] = {0, 4, 8, 16};
    for (int i = 0; i < 4; i++) {
        m[i] = xcalloc(sizeof(Member));
        m[i]->ty = tys[i];
        m[i]->idx = i;
        m[i]->align = tys[i]->align;
        m[i]->offset = offs[i];
        if (i) m[i - 1]->next = m[i];
    }
    st->members = m[0];
    st->size = 24;
    st->align = 8;
    return array_of(st, 1);
}

Obj *parse(Token *tok) {
    va_list_type = make_va_list_type();
    push_scope("__builtin_va_list")->type_def = va_list_type;
    globals = NULL;

    while (tok->kind != TK_EOF) {
        if (equal(tok, ";")) {
            tok = tok->next;
            continue;
        }

        if (equal(tok, "_Static_assert")) {
            tok = skip(tok->next, "(");
            Node *e = conditional(&tok, tok);
            bool ok = eval(e) != 0;
            char *msg = "";
            if (equal(tok, ",")) {
                msg = tok->next->str ? tok->next->str : "";
                tok = tok->next->next;
            }
            if (!ok) error_tok(tok, "static assertion failed: %s", msg);
            tok = skip(tok, ")");
            tok = skip(tok, ";");
            continue;
        }

        if (equal(tok, "asm")) {
            /* file-scope assembly is not supported; skip it with a warning */
            warn_tok(tok, "file-scope asm is ignored");
            tok = skip_asm_label(tok);
            tok = skip(tok, ";");
            continue;
        }

        VarAttr attr;
        memset(&attr, 0, sizeof attr);
        Type *basety = declspec(&tok, tok, &attr);

        /* typedef */
        if (attr.is_typedef) {
            tok = parse_typedef(tok, basety);
            continue;
        }

        /* function */
        if (is_function(tok)) {
            tok = function(tok, basety, &attr);
            continue;
        }

        /* global variable */
        tok = global_variable(tok, basety, &attr);
    }

    /* declaration order, and the final linkage of functions (inline functions are static unless declared otherwise) */
    Obj head;
    memset(&head, 0, sizeof head);
    Obj *list = NULL;
    for (Obj *var = globals; var;) {
        Obj *next = var->next;
        var->next = list;
        list = var;
        var = next;
    }
    globals = list;
    (void)head;

    for (Obj *var = globals; var; var = var->next) {
        if (var->is_function) var->is_static = var->decl_static || (var->is_inline && !var->decl_noninline);
        else if (var->is_static) var->is_static = true;
    }

    for (Obj *var = globals; var; var = var->next)
        if (var->is_definition && !var->is_static) mark_live(var);

    return globals;
}
