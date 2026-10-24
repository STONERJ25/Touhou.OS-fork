/* pp.inc -- the preprocessor: directives, macros (Prosser's hide-set algorithm), #include, #if. */

typedef struct MacroParam MacroParam;
struct MacroParam {
    MacroParam *next;
    char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
    MacroArg *next;
    char *name;
    bool is_va_args;
    Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro {
    char *name;
    bool is_objlike;
    MacroParam *params;
    char *va_args_name;
    Token *body;
    macro_handler_fn *handler;
};

enum { IN_THEN, IN_ELIF, IN_ELSE };

typedef struct CondIncl CondIncl;
struct CondIncl {
    CondIncl *next;
    int ctx;
    Token *tok;
    bool included;
};

struct Hideset {
    Hideset *next;
    char *name;
};

static Map macros;
static CondIncl *cond_incl;
static Map pragma_once;
static Map include_guards;
static char **include_paths;
static int n_include_paths;

void pp_add_include_path(char *path) {
    include_paths = xrealloc(include_paths, (size_t)(n_include_paths + 1) * sizeof(char *));
    include_paths[n_include_paths++] = path;
}

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok) {
    return tok->at_bol && equal(tok, "#");
}

/* Some preprocessor directives take a single line; skip to the beginning of the next one. */
static Token *skip_line(Token *tok) {
    if (tok->at_bol) return tok;
    warn_tok(tok, "extra token");
    while (tok->at_bol == false) tok = tok->next;
    return tok;
}

static Token *copy_token(Token *tok) {
    Token *t = xmalloc(sizeof(Token));
    *t = *tok;
    t->next = NULL;
    return t;
}

static Token *new_eof(Token *tok) {
    Token *t = copy_token(tok);
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

static Hideset *new_hideset(char *name) {
    Hideset *hs = xcalloc(sizeof(Hideset));
    hs->name = name;
    return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
    Hideset head;
    memset(&head, 0, sizeof head);
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next) cur = cur->next = new_hideset(hs1->name);
    cur->next = hs2;
    return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
    for (; hs; hs = hs->next)
        if ((int)strlen(hs->name) == len && !strncmp(hs->name, s, (size_t)len)) return true;
    return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
    Hideset head;
    memset(&head, 0, sizeof head);
    Hideset *cur = &head;
    for (; hs1; hs1 = hs1->next)
        if (hideset_contains(hs2, hs1->name, (int)strlen(hs1->name))) cur = cur->next = new_hideset(hs1->name);
    return head.next;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    for (; tok; tok = tok->next) {
        Token *t = copy_token(tok);
        t->hideset = hideset_union(t->hideset, hs);
        cur = cur->next = t;
    }
    return head.next;
}

static Token *append(Token *tok1, Token *tok2) {
    if (!tok1 || tok1->kind == TK_EOF) return tok2;
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    for (; tok1 && tok1->kind != TK_EOF; tok1 = tok1->next) cur = cur->next = copy_token(tok1);
    cur->next = tok2;
    return head.next;
}

static Token *skip_cond_incl2(Token *tok) {
    while (tok->kind != TK_EOF) {
        if (is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) && equal(tok->next, "endif")) return tok->next->next;
        tok = tok->next;
    }
    return tok;
}

/* Skip until the next #else, #elif or #endif.  Nested #if..#endif groups are skipped as a whole. */
static Token *skip_cond_incl(Token *tok) {
    while (tok->kind != TK_EOF) {
        if (is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) && (equal(tok->next, "elif") || equal(tok->next, "else") || equal(tok->next, "endif"))) break;
        tok = tok->next;
    }
    return tok;
}

/* Double-quote a given string and returns it. */
static char *quote_string(char *str) {
    int bufsize = 3;
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\\' || str[i] == '"') bufsize++;
        bufsize++;
    }
    char *buf = xcalloc((size_t)bufsize);
    char *p = buf;
    *p++ = '"';
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\\' || str[i] == '"') *p++ = '\\';
        *p++ = str[i];
    }
    *p++ = '"';
    *p++ = '\0';
    return buf;
}

static Token *new_str_token(char *str, Token *tmpl) {
    char *buf = quote_string(str);
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

/* Copy all tokens until the next newline, terminate them with an EOF token and then returns them. */
static Token *copy_line(Token **rest, Token *tok) {
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    for (; !tok->at_bol; tok = tok->next) cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

static Token *new_num_token(int val, Token *tmpl) {
    char *buf = xformat("%d\n", val);
    return tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
}

/* Concatenates the spelling of tokens (a space where the source had one). */
static char *join_tokens(Token *tok, Token *end) {
    int len = 1;
    for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
        if (t != tok && t->has_space) len++;
        len += t->len;
    }
    char *buf = xcalloc((size_t)len);
    int pos = 0;
    for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
        if (t != tok && t->has_space) buf[pos++] = ' ';
        memcpy(buf + pos, t->loc, (size_t)t->len);
        pos += t->len;
    }
    buf[pos] = '\0';
    return buf;
}

/* ---- include path search */

static char *search_include_from(char *filename, int from, int *found_idx) {
    for (int i = from; i < n_include_paths; i++) {
        char *path = join_path(include_paths[i], filename);
        if (file_exists(path)) {
            if (found_idx) *found_idx = i;
            return path;
        }
    }
    return NULL;
}

static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote) {
    if (tok->kind == TK_STR) {
        /* no escape sequences are interpreted in a header name: use the raw spelling */
        *is_dquote = true;
        *rest = skip_line(tok->next);
        return xstrndup(tok->loc + 1, (size_t)tok->len - 2);
    }
    if (equal(tok, "<")) {
        Token *start = tok;
        for (; !equal(tok, ">"); tok = tok->next)
            if (tok->at_bol || tok->kind == TK_EOF) error_tok(tok, "expected '>'");
        *is_dquote = false;
        *rest = skip_line(tok->next);
        return join_tokens(start->next, tok);
    }
    if (tok->kind == TK_IDENT) {
        Token *tok2 = preprocess2(copy_line(rest, tok));
        return read_include_filename(&tok2, tok2, is_dquote);
    }
    error_tok(tok, "expected a filename");
}

/* Detects the #ifndef X / #define X ... #endif pattern so that a second #include of the file costs nothing. */
static char *detect_include_guard(Token *tok) {
    if (!is_hash(tok) || !equal(tok->next, "ifndef")) return NULL;
    tok = tok->next->next;
    if (tok->kind != TK_IDENT) return NULL;
    char *macro = xstrndup(tok->loc, (size_t)tok->len);
    tok = tok->next;
    if (!is_hash(tok) || !equal(tok->next, "define") || !equal(tok->next->next, macro)) return NULL;
    while (tok->kind != TK_EOF) {
        if (!is_hash(tok)) {
            tok = tok->next;
            continue;
        }
        if (equal(tok->next, "endif") && tok->next->next->kind == TK_EOF) return macro;
        if (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef")) tok = skip_cond_incl2(tok->next->next);
        else tok = tok->next;
    }
    return NULL;
}

static Token *include_file(Token *tok, char *path, Token *filename_tok, int inc_idx) {
    char *guard_name = map_get(&include_guards, path);
    if (guard_name && map_get(&macros, guard_name)) return tok;
    if (map_get(&pragma_once, path)) return tok;
    int depth = filename_tok->file->depth + 1;
    if (depth > 200) error_tok(filename_tok, "#include nested too deeply (Patchouli says that book contains itself)");
    File *file = load_file(path);
    if (!file) error_tok(filename_tok, "%s: cannot open file: %s", path, strerror(errno));
    file->inc_idx = inc_idx;
    file->depth = depth;
    Token *tok2 = tokenize(file);
    guard_name = detect_include_guard(tok2);
    if (guard_name) map_put(&include_guards, path, guard_name);
    return append(tok2, tok);
}

/* ---- #if expressions */

typedef struct {
    i64 v;
    bool u;
} PV;

static Token *pp_t;

static PV pp_ternary(void);

static PV pp_primary(void) {
    Token *t = pp_t;
    PV r;
    r.v = 0;
    r.u = false;
    if (equal(t, "(")) {
        pp_t = t->next;
        r = pp_ternary();
        if (!equal(pp_t, ")")) error_tok(pp_t, "expected ')'");
        pp_t = pp_t->next;
        return r;
    }
    if (t->kind == TK_PP_NUM) {
        if (!convert_pp_int(t)) error_tok(t, "floating constant in preprocessor expression");
    }
    if (t->kind == TK_NUM) {
        if (is_flonum(t->ty)) error_tok(t, "floating constant in preprocessor expression");
        r.v = t->val;
        r.u = t->ty->is_unsigned && t->ty->size >= 4;
        pp_t = t->next;
        return r;
    }
    if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
        pp_t = t->next;
        return r;
    }
    error_tok(t, "invalid expression in #if");
}

static PV pp_unary(void) {
    Token *t = pp_t;
    if (equal(t, "+") || equal(t, "-") || equal(t, "!") || equal(t, "~")) {
        pp_t = t->next;
        PV a = pp_unary();
        if (equal(t, "-")) a.v = (i64)(0 - (u64)a.v);
        else if (equal(t, "!")) {
            a.v = !a.v;
            a.u = false;
        } else if (equal(t, "~")) a.v = ~a.v;
        return a;
    }
    return pp_primary();
}

static int pp_prec(Token *t) {
    static const char *ops[] = {"*", "/", "%", "+", "-", "<<", ">>", "<", "<=", ">", ">=", "==", "!=", "&", "^", "|", "&&", "||"};
    static const int prec[] = {10, 10, 10, 9, 9, 8, 8, 7, 7, 7, 7, 6, 6, 5, 4, 3, 2, 1};
    if (t->kind != TK_PUNCT) return 0;
    for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++)
        if (equal(t, ops[i])) return prec[i];
    return 0;
}

static int pp_noeval; /* > 0 inside the operand of && || ?: that is not evaluated: division by zero is not an error there */

static PV pp_binary(int minprec) {
    PV lhs = pp_unary();
    for (;;) {
        Token *op = pp_t;
        int p = pp_prec(op);
        if (p == 0 || p < minprec) return lhs;
        pp_t = op->next;
        bool skip_rhs = (equal(op, "&&") && !lhs.v) || (equal(op, "||") && lhs.v);
        if (skip_rhs) pp_noeval++;
        PV rhs = pp_binary(p + 1);
        if (skip_rhs) pp_noeval--;
        bool u = lhs.u || rhs.u;
        PV r;
        r.u = u;
        i64 a = lhs.v, b = rhs.v;
        if (equal(op, "*")) r.v = (i64)((u64)a * (u64)b);
        else if (equal(op, "/") || equal(op, "%")) {
            if (b == 0) {
                if (!pp_noeval) error_tok(op, "division by zero in #if");
                b = 1;
            }
            if (u) r.v = equal(op, "/") ? (i64)((u64)a / (u64)b) : (i64)((u64)a % (u64)b);
            else r.v = equal(op, "/") ? (b == -1 ? (i64)(0 - (u64)a) : a / b) : (b == -1 ? 0 : a % b);
        } else if (equal(op, "+")) r.v = (i64)((u64)a + (u64)b);
        else if (equal(op, "-")) r.v = (i64)((u64)a - (u64)b);
        else if (equal(op, "<<")) {
            r.u = lhs.u;
            r.v = (i64)((u64)a << (b & 63));
        } else if (equal(op, ">>")) {
            r.u = lhs.u;
            r.v = lhs.u ? (i64)((u64)a >> (b & 63)) : (a >> (b & 63));
        } else if (equal(op, "<")) {
            r.v = u ? (u64)a < (u64)b : a < b;
            r.u = false;
        } else if (equal(op, "<=")) {
            r.v = u ? (u64)a <= (u64)b : a <= b;
            r.u = false;
        } else if (equal(op, ">")) {
            r.v = u ? (u64)a > (u64)b : a > b;
            r.u = false;
        } else if (equal(op, ">=")) {
            r.v = u ? (u64)a >= (u64)b : a >= b;
            r.u = false;
        } else if (equal(op, "==")) {
            r.v = a == b;
            r.u = false;
        } else if (equal(op, "!=")) {
            r.v = a != b;
            r.u = false;
        } else if (equal(op, "&")) r.v = a & b;
        else if (equal(op, "^")) r.v = a ^ b;
        else if (equal(op, "|")) r.v = a | b;
        else if (equal(op, "&&")) {
            r.v = a && b;
            r.u = false;
        } else {
            r.v = a || b;
            r.u = false;
        }
        lhs = r;
    }
}

static PV pp_ternary(void) {
    PV c = pp_binary(1);
    if (!equal(pp_t, "?")) return c;
    pp_t = pp_t->next;
    if (!c.v) pp_noeval++;
    PV a = pp_ternary();
    if (!c.v) pp_noeval--;
    if (!equal(pp_t, ":")) error_tok(pp_t, "expected ':'");
    pp_t = pp_t->next;
    if (c.v) pp_noeval++;
    PV b = pp_ternary();
    if (c.v) pp_noeval--;
    PV r = c.v ? a : b;
    r.u = a.u || b.u;
    return r;
}

static Token *read_const_expr(Token **rest, Token *tok) {
    tok = copy_line(rest, tok);
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    while (tok->kind != TK_EOF) {
        /* "defined(foo)" or "defined foo" becomes 1 if the macro is defined, otherwise 0 */
        if (equal(tok, "defined")) {
            Token *start = tok;
            bool has_paren = consume(&tok, tok->next, "(");
            if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) error_tok(start, "macro name must be an identifier");
            Macro *m = find_macro(tok);
            tok = tok->next;
            if (has_paren) tok = skip(tok, ")");
            cur = cur->next = new_num_token(m ? 1 : 0, start);
            continue;
        }
        if (equal(tok, "__has_include") || equal(tok, "__has_include_next")) {
            Token *start = tok;
            tok = skip(tok->next, "(");
            bool is_dquote = false;
            char *name;
            if (tok->kind == TK_STR) {
                is_dquote = true;
                name = xstrndup(tok->loc + 1, (size_t)tok->len - 2);
                tok = tok->next;
            } else {
                Token *lt = tok;
                tok = skip(tok, "<");
                Token *st = tok;
                while (!equal(tok, ">")) {
                    if (tok->kind == TK_EOF) error_tok(lt, "expected '>'");
                    tok = tok->next;
                }
                name = join_tokens(st, tok);
                tok = tok->next;
            }
            tok = skip(tok, ")");
            bool found = false;
            if (is_dquote && name[0] != '/') {
                char *p = join_path(dir_of(start->file->name), name);
                if (file_exists(p)) found = true;
            }
            if (!found) found = name[0] == '/' ? file_exists(name) : search_include_from(name, 0, NULL) != NULL;
            cur = cur->next = new_num_token(found ? 1 : 0, start);
            continue;
        }
        cur = cur->next = tok;
        tok = tok->next;
    }
    cur->next = tok;
    return head.next;
}

/* Reads and evaluates a constant expression. */
static i64 eval_const_expr(Token **rest, Token *tok) {
    Token *start = tok;
    Token *expr = read_const_expr(rest, tok->next);
    expr = preprocess2(expr);
    if (expr->kind == TK_EOF) error_tok(start, "no expression");
    /* identifiers that survived macro expansion count as 0 */
    for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
        if (t->kind == TK_IDENT) {
            Token *next = t->next;
            *t = *new_num_token(0, t);
            t->next = next;
        }
    }
    pp_t = expr;
    PV v = pp_ternary();
    if (pp_t->kind != TK_EOF) error_tok(pp_t, "extra token in #if expression");
    return v.v;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
    CondIncl *ci = xcalloc(sizeof(CondIncl));
    ci->next = cond_incl;
    ci->ctx = IN_THEN;
    ci->tok = tok;
    ci->included = included;
    cond_incl = ci;
    return ci;
}

/* ---- macros */

static Macro *find_macro(Token *tok) {
    if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) return NULL;
    return map_get2(&macros, tok->loc, tok->len);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
    Macro *m = xcalloc(sizeof(Macro));
    m->name = name;
    m->is_objlike = is_objlike;
    m->body = body;
    map_put(&macros, name, m);
    return m;
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name) {
    MacroParam head;
    memset(&head, 0, sizeof head);
    MacroParam *cur = &head;
    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        if (equal(tok, "...")) {
            *va_args_name = "__VA_ARGS__";
            *rest = skip(tok->next, ")");
            return head.next;
        }
        if (tok->kind != TK_IDENT) error_tok(tok, "expected an identifier");
        if (equal(tok->next, "...")) {
            *va_args_name = xstrndup(tok->loc, (size_t)tok->len);
            *rest = skip(tok->next->next, ")");
            return head.next;
        }
        MacroParam *m = xcalloc(sizeof(MacroParam));
        m->name = xstrndup(tok->loc, (size_t)tok->len);
        cur = cur->next = m;
        tok = tok->next;
    }
    *rest = tok->next;
    return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
    if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) error_tok(tok, "macro name must be an identifier");
    char *name = xstrndup(tok->loc, (size_t)tok->len);
    tok = tok->next;
    if (!tok->has_space && equal(tok, "(")) {
        char *va_args_name = NULL;
        MacroParam *params = read_macro_params(&tok, tok->next, &va_args_name);
        Macro *m = add_macro(name, false, copy_line(rest, tok));
        m->params = params;
        m->va_args_name = va_args_name;
    } else {
        add_macro(name, true, copy_line(rest, tok));
    }
}

static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;
    int level = 0;
    for (;;) {
        if (level == 0 && equal(tok, ")")) break;
        if (level == 0 && !read_rest && equal(tok, ",")) break;
        if (tok->kind == TK_EOF) error_tok(tok, "premature end of input");
        if (equal(tok, "(")) level++;
        else if (equal(tok, ")")) level--;
        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }
    cur->next = new_eof(tok);
    MacroArg *arg = xcalloc(sizeof(MacroArg));
    arg->tok = head.next;
    *rest = tok;
    return arg;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_args_name) {
    Token *start = tok;
    tok = tok->next->next;
    MacroArg head;
    memset(&head, 0, sizeof head);
    MacroArg *cur = &head;
    MacroParam *pp = params;
    for (; pp; pp = pp->next) {
        if (cur != &head) tok = skip(tok, ",");
        cur = cur->next = read_macro_arg_one(&tok, tok, false);
        cur->name = pp->name;
    }
    if (va_args_name) {
        MacroArg *arg;
        if (equal(tok, ")")) {
            arg = xcalloc(sizeof(MacroArg));
            arg->tok = new_eof(tok);
        } else {
            if (pp != params) tok = skip(tok, ",");
            arg = read_macro_arg_one(&tok, tok, true);
        }
        arg->name = va_args_name;
        arg->is_va_args = true;
        cur = cur->next = arg;
    } else if (!equal(tok, ")")) {
        error_tok(start, "too many arguments to macro '%.*s'", start->len, start->loc);
    }
    skip(tok, ")");
    *rest = tok;
    return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
    for (MacroArg *ap = args; ap; ap = ap->next)
        if (tok->len == (int)strlen(ap->name) && !strncmp(tok->loc, ap->name, (size_t)tok->len)) return ap;
    return NULL;
}

/* Concatenates all tokens in `arg` and returns a new string token (for the # operator). */
static Token *stringize(Token *hash, Token *arg) {
    char *s = join_tokens(arg, NULL);
    return new_str_token(s, hash);
}

/* Concatenates two tokens to create a new token (for the ## operator). */
static Token *paste(Token *lhs, Token *rhs) {
    char *buf = xformat("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
    Token *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, buf));
    if (tok->next->kind != TK_EOF) error_tok(lhs, "pasting forms '%s', an invalid token", buf);
    tok->at_bol = false;
    tok->has_space = lhs->has_space;
    return tok;
}

static bool has_varargs(MacroArg *args) {
    for (MacroArg *ap = args; ap; ap = ap->next)
        if (ap->is_va_args) return ap->tok->kind != TK_EOF;
    return false;
}

/* Replaces func-like macro parameters with given arguments. */
static Token *subst(Token *tok, MacroArg *args) {
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;

    while (tok->kind != TK_EOF) {
        /* "#" followed by a parameter is replaced with stringized actuals */
        if (equal(tok, "#")) {
            MacroArg *arg = find_arg(args, tok->next);
            if (!arg) error_tok(tok->next, "'#' is not followed by a macro parameter");
            cur = cur->next = stringize(tok, arg->tok);
            tok = tok->next->next;
            continue;
        }

        /* [GNU] `, ## __VA_ARGS__` disappears together with the comma when there are no variadic arguments */
        if (equal(tok, ",") && equal(tok->next, "##")) {
            MacroArg *arg = find_arg(args, tok->next->next);
            if (arg && arg->is_va_args) {
                if (arg->tok->kind == TK_EOF) {
                    tok = tok->next->next->next;
                } else {
                    cur = cur->next = copy_token(tok);
                    tok = tok->next->next;
                }
                continue;
            }
        }

        if (equal(tok, "##")) {
            if (cur == &head) error_tok(tok, "'##' cannot appear at start of macro expansion");
            if (tok->next->kind == TK_EOF) error_tok(tok, "'##' cannot appear at end of macro expansion");
            MacroArg *arg = find_arg(args, tok->next);
            if (arg) {
                if (arg->tok->kind != TK_EOF) {
                    *cur = *paste(cur, arg->tok);
                    for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
                }
                tok = tok->next->next;
                continue;
            }
            *cur = *paste(cur, tok->next);
            tok = tok->next->next;
            continue;
        }

        MacroArg *arg = find_arg(args, tok);

        if (arg && equal(tok->next, "##")) {
            Token *rhs = tok->next->next;
            if (arg->tok->kind == TK_EOF) {
                MacroArg *arg2 = find_arg(args, rhs);
                if (arg2) {
                    for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
                } else {
                    cur = cur->next = copy_token(rhs);
                }
                tok = rhs->next;
                continue;
            }
            for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }

        /* __VA_OPT__(x) is x when variadic arguments were given, else nothing */
        if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")) {
            Token *inner_start = tok->next->next;
            int level = 1;
            Token *t = inner_start;
            while (t->kind != TK_EOF) {
                if (equal(t, "(")) level++;
                else if (equal(t, ")") && --level == 0) break;
                t = t->next;
            }
            if (t->kind == TK_EOF) error_tok(tok, "unterminated __VA_OPT__");
            if (has_varargs(args)) {
                Token tmp = *t;
                tmp.kind = TK_EOF;
                tmp.next = NULL;
                /* substitute inside the parentheses */
                Token *copy_head = NULL, *copy_cur = NULL;
                for (Token *u = inner_start; u != t; u = u->next) {
                    Token *c = copy_token(u);
                    if (!copy_head) copy_head = copy_cur = c;
                    else copy_cur = copy_cur->next = c;
                }
                Token *eof = new_eof(t);
                if (copy_cur) copy_cur->next = eof;
                else copy_head = eof;
                Token *sub = subst(copy_head, args);
                for (Token *u = sub; u->kind != TK_EOF; u = u->next) cur = cur->next = copy_token(u);
            }
            tok = t->next;
            continue;
        }

        /* macro arguments are completely macro-expanded before they are substituted */
        if (arg) {
            Token *t = preprocess2(arg->tok);
            t->at_bol = tok->at_bol;
            t->has_space = tok->has_space;
            for (; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }

        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }

    cur->next = tok;
    return head.next;
}

/* If tok is a macro, expands it and returns true. Otherwise, does nothing and returns false. */
static bool expand_macro(Token **rest, Token *tok) {
    if (hideset_contains(tok->hideset, tok->loc, tok->len)) return false;

    Macro *m = find_macro(tok);
    if (!m) return false;

    if (m->handler) {
        *rest = m->handler(tok);
        (*rest)->next = tok->next;
        return true;
    }

    if (m->is_objlike) {
        Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
        Token *body = add_hideset(m->body, hs);
        for (Token *t = body; t->kind != TK_EOF; t = t->next) t->origin = tok;
        *rest = append(body, tok->next);
        (*rest)->at_bol = tok->at_bol;
        (*rest)->has_space = tok->has_space;
        return true;
    }

    /* a function-like macro name not followed by "(" is an ordinary identifier */
    if (!equal(tok->next, "(")) return false;

    Token *macro_token = tok;
    MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
    Token *rparen = tok;

    Hideset *hs = hideset_intersection(macro_token->hideset, rparen->hideset);
    hs = hideset_union(hs, new_hideset(m->name));

    Token *body = subst(m->body, args);
    body = add_hideset(body, hs);
    for (Token *t = body; t->kind != TK_EOF; t = t->next) t->origin = macro_token;
    *rest = append(body, tok->next);
    (*rest)->at_bol = macro_token->at_bol;
    (*rest)->has_space = macro_token->has_space;
    return true;
}

/* #line and "# 123 "file"" markers */
static void read_line_marker(Token **rest, Token *tok) {
    Token *start = tok;
    tok = preprocess2(copy_line(rest, tok));
    if (tok->kind != TK_PP_NUM) error_tok(tok, "invalid line marker");
    convert_pp_number(tok);
    start->file->line_delta = (int)tok->val - start->line_no - 1;
    tok = tok->next;
    if (tok->kind == TK_EOF) return;
    if (tok->kind != TK_STR) error_tok(tok, "filename expected");
    start->file->display_name = tok->str;
}

static Macro *add_builtin(char *name, macro_handler_fn *fn) {
    Macro *m = add_macro(name, true, NULL);
    m->handler = fn;
    return m;
}

static Token *file_macro(Token *tmpl) {
    while (tmpl->origin) tmpl = tmpl->origin;
    return new_str_token(tmpl->file->display_name, tmpl);
}

static Token *line_macro(Token *tmpl) {
    while (tmpl->origin) tmpl = tmpl->origin;
    int i = tmpl->line_no + tmpl->file->line_delta;
    return new_num_token(i, tmpl);
}

static Token *counter_macro(Token *tmpl) {
    static int i = 0;
    return new_num_token(i++, tmpl);
}

static Token *base_file_macro(Token *tmpl) {
    return new_str_token(input_files[0]->name, tmpl);
}

static Token *include_level_macro(Token *tmpl) {
    while (tmpl->origin) tmpl = tmpl->origin;
    return new_num_token(tmpl->file->depth, tmpl);
}

static char *g_date, *g_time;

static Token *date_macro(Token *tmpl) {
    return new_str_token(g_date, tmpl);
}

static Token *time_macro(Token *tmpl) {
    return new_str_token(g_time, tmpl);
}

static void make_date_time(void) {
    static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    long long t = (long long)time(NULL);
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    /* civil-from-days (Howard Hinnant) */
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    g_date = xformat("%s %2d %d", mon[m - 1], (int)d, (int)y);
    g_time = xformat("%02d:%02d:%02d", (int)(secs / 3600), (int)(secs / 60 % 60), (int)(secs % 60));
}

void define_macro(char *name, char *buf) {
    Token *tok = tokenize(new_file("<built-in>", 1, buf));
    add_macro(name, true, tok);
}

void undef_macro(char *name) {
    map_delete2(&macros, name, (int)strlen(name));
}

static void define_function_macro(char *def) {
    /* "name(params) body" in one string, for the predefined function-like macros */
    Token *tok = tokenize(new_file("<built-in>", 1, xformat("%s\n", def)));
    Token *rest;
    read_macro_definition(&rest, tok);
}

static void init_macros(void) {
    define_macro("_LP64", "1");
    define_macro("__C99_MACRO_WITH_VA_ARGS", "1");
    define_macro("__ELF__", "1");
    define_macro("__LP64__", "1");
    define_macro("__SIZEOF_DOUBLE__", "8");
    define_macro("__SIZEOF_FLOAT__", "4");
    define_macro("__SIZEOF_INT__", "4");
    define_macro("__SIZEOF_LONG_DOUBLE__", "8");
    define_macro("__SIZEOF_LONG_LONG__", "8");
    define_macro("__SIZEOF_LONG__", "8");
    define_macro("__SIZEOF_POINTER__", "8");
    define_macro("__SIZEOF_PTRDIFF_T__", "8");
    define_macro("__SIZEOF_SHORT__", "2");
    define_macro("__SIZEOF_SIZE_T__", "8");
    define_macro("__SIZE_TYPE__", "unsigned long");
    define_macro("__PTRDIFF_TYPE__", "long");
    define_macro("__WCHAR_TYPE__", "int");
    define_macro("__INTMAX_TYPE__", "long");
    define_macro("__UINTMAX_TYPE__", "unsigned long");
    define_macro("__INTPTR_TYPE__", "long");
    define_macro("__UINTPTR_TYPE__", "unsigned long");
    define_macro("__CHAR_BIT__", "8");
    define_macro("__SCHAR_MAX__", "127");
    define_macro("__SHRT_MAX__", "32767");
    define_macro("__INT_MAX__", "2147483647");
    define_macro("__LONG_MAX__", "9223372036854775807L");
    define_macro("__LONG_LONG_MAX__", "9223372036854775807LL");
    define_macro("__STDC_HOSTED__", "1");
    define_macro("__STDC_NO_ATOMICS__", "1");
    define_macro("__STDC_NO_COMPLEX__", "1");
    define_macro("__STDC_NO_THREADS__", "1");
    define_macro("__STDC_NO_VLA__", "1");
    define_macro("__STDC_UTF_16__", "1");
    define_macro("__STDC_UTF_32__", "1");
    define_macro("__STDC_VERSION__", "201112L");
    define_macro("__STDC__", "1");
    define_macro("__USER_LABEL_PREFIX__", "");
    define_macro("__amd64", "1");
    define_macro("__amd64__", "1");
    define_macro("__x86_64", "1");
    define_macro("__x86_64__", "1");
    define_macro("__BYTE_ORDER__", "1234");
    define_macro("__ORDER_LITTLE_ENDIAN__", "1234");
    define_macro("__ORDER_BIG_ENDIAN__", "4321");
    define_macro("__LITTLE_ENDIAN__", "1");
    define_macro("__MCC__", "1");
    define_macro("__mcc__", "1");
    define_macro("__TOUHOU__", "1");
    define_macro("__touhou__", "1");
    define_macro("__GENSOKYO__", "1");
    define_macro("__NO_INLINE__", "1");
    define_macro("__ATOMIC_RELAXED", "0");
    define_macro("__ATOMIC_CONSUME", "1");
    define_macro("__ATOMIC_ACQUIRE", "2");
    define_macro("__ATOMIC_RELEASE", "3");
    define_macro("__ATOMIC_ACQ_REL", "4");
    define_macro("__ATOMIC_SEQ_CST", "5");
    define_macro("__FLT_MAX__", "3.40282346638528859812e+38F");
    define_macro("__FLT_MIN__", "1.17549435082228750797e-38F");
    define_macro("__FLT_EPSILON__", "1.1920928955078125e-07F");
    define_macro("__FLT_DIG__", "6");
    define_macro("__FLT_MANT_DIG__", "24");
    define_macro("__DBL_MAX__", "1.79769313486231570815e+308");
    define_macro("__DBL_MIN__", "2.22507385850720138309e-308");
    define_macro("__DBL_EPSILON__", "2.22044604925031308085e-16");
    define_macro("__DBL_DIG__", "15");
    define_macro("__DBL_MANT_DIG__", "53");
    define_macro("__LDBL_MAX__", "1.79769313486231570815e+308");
    define_macro("__LDBL_MIN__", "2.22507385850720138309e-308");
    define_macro("__LDBL_EPSILON__", "2.22044604925031308085e-16");
    define_macro("__LDBL_DIG__", "15");
    define_macro("__LDBL_MANT_DIG__", "53");
    define_macro("__has_include", "1");
    define_macro("__has_include_next", "1");
    define_function_macro("__has_feature(x) 0");
    define_function_macro("__has_extension(x) 0");
    define_function_macro("__has_builtin(x) 0");
    define_function_macro("__has_attribute(x) 0");
    define_function_macro("__has_cpp_attribute(x) 0");

    add_builtin("__FILE__", file_macro);
    add_builtin("__LINE__", line_macro);
    add_builtin("__COUNTER__", counter_macro);
    add_builtin("__BASE_FILE__", base_file_macro);
    add_builtin("__INCLUDE_LEVEL__", include_level_macro);
    make_date_time();
    add_builtin("__DATE__", date_macro);
    add_builtin("__TIME__", time_macro);
}

/* ---- the main loop */

static Token *preprocess2(Token *tok) {
    Token head;
    memset(&head, 0, sizeof head);
    Token *cur = &head;

    while (tok->kind != TK_EOF) {
        if (expand_macro(&tok, tok)) continue;

        /* _Pragma("...") is ignored */
        if (tok->kind == TK_IDENT && equal(tok, "_Pragma") && equal(tok->next, "(")) {
            Token *t = tok->next->next;
            while (t->kind != TK_EOF && !equal(t, ")")) t = t->next;
            tok = t->kind == TK_EOF ? t : t->next;
            continue;
        }

        if (!is_hash(tok)) {
            tok->line_delta = tok->file->line_delta;
            tok->filename = tok->file->display_name;
            cur = cur->next = tok;
            tok = tok->next;
            continue;
        }

        Token *start = tok;
        tok = tok->next;

        if (equal(tok, "include") || equal(tok, "include_next")) {
            bool next = equal(tok, "include_next");
            bool is_dquote;
            char *filename = read_include_filename(&tok, tok->next, &is_dquote);
            if (filename[0] == '/' || (is_dquote && !next)) {
                char *path = filename[0] == '/' ? filename : join_path(dir_of(start->file->name), filename);
                if (file_exists(path)) {
                    tok = include_file(tok, path, start->next->next, -1);
                    continue;
                }
            }
            int from = 0;
            if (next && start->file->inc_idx >= 0) from = start->file->inc_idx + 1;
            int idx = -1;
            char *path = search_include_from(filename, from, &idx);
            if (!path && next) path = search_include_from(filename, 0, &idx);
            if (!path) error_tok(start->next->next, "'%s': file not found (Marisa could not find that book in %d shelves)", filename, n_include_paths);
            tok = include_file(tok, path, start->next->next, idx);
            continue;
        }

        if (equal(tok, "define")) {
            read_macro_definition(&tok, tok->next);
            continue;
        }

        if (equal(tok, "undef")) {
            tok = tok->next;
            if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD) error_tok(tok, "macro name must be an identifier");
            undef_macro(xstrndup(tok->loc, (size_t)tok->len));
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "if")) {
            i64 val = eval_const_expr(&tok, tok);
            push_cond_incl(start, val != 0);
            if (!val) tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "ifdef")) {
            bool defined = find_macro(tok->next) != NULL;
            push_cond_incl(tok, defined);
            tok = skip_line(tok->next->next);
            if (!defined) tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "ifndef")) {
            bool defined = find_macro(tok->next) != NULL;
            push_cond_incl(tok, !defined);
            tok = skip_line(tok->next->next);
            if (defined) tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "elif")) {
            if (!cond_incl || cond_incl->ctx == IN_ELSE) error_tok(start, "stray #elif");
            cond_incl->ctx = IN_ELIF;
            if (!cond_incl->included && eval_const_expr(&tok, tok)) cond_incl->included = true;
            else tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "else")) {
            if (!cond_incl || cond_incl->ctx == IN_ELSE) error_tok(start, "stray #else");
            cond_incl->ctx = IN_ELSE;
            tok = skip_line(tok->next);
            if (cond_incl->included) tok = skip_cond_incl(tok);
            continue;
        }

        if (equal(tok, "endif")) {
            if (!cond_incl) error_tok(start, "stray #endif");
            cond_incl = cond_incl->next;
            tok = skip_line(tok->next);
            continue;
        }

        if (equal(tok, "line")) {
            read_line_marker(&tok, tok->next);
            continue;
        }

        if (tok->kind == TK_PP_NUM) {
            read_line_marker(&tok, tok);
            continue;
        }

        if (equal(tok, "pragma")) {
            if (equal(tok->next, "once")) {
                map_put(&pragma_once, tok->file->name, (void *)1);
                tok = skip_line(tok->next->next);
                continue;
            }
            do {
                tok = tok->next;
            } while (!tok->at_bol);
            continue;
        }

        if (equal(tok, "error") || equal(tok, "warning")) {
            Token *dir = tok;
            Token *line = copy_line(&tok, tok->next);
            char *msg = join_tokens(line, NULL);
            if (equal(dir, "error")) error_tok(dir, "#error %s", msg);
            warn_tok(dir, "#warning %s", msg);
            continue;
        }

        if (equal(tok, "ident") || equal(tok, "sccs") || equal(tok, "assert") || equal(tok, "unassert")) {
            do {
                tok = tok->next;
            } while (!tok->at_bol);
            continue;
        }

        /* `#` alone on a line is a null directive */
        if (tok->at_bol) continue;

        error_tok(tok, "invalid preprocessor directive");
    }

    cur->next = tok;
    return head.next;
}

static void join_adjacent_string_literals(Token *tok1) {
    while (tok1->kind != TK_EOF) {
        if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
            tok1 = tok1->next;
            continue;
        }
        Token *tok2 = tok1->next;
        while (tok2->kind == TK_STR) tok2 = tok2->next;

        /* the widest element type decides how the whole sequence is stored */
        Type *basety = tok1->ty->base;
        for (Token *t = tok1->next; t != tok2; t = t->next)
            if (t->ty->base->size > basety->size) basety = t->ty->base;
        for (Token *t = tok1; t != tok2; t = t->next) {
            if (t->ty->base->size != basety->size) {
                Token *nt = tokenize_string_literal(t, basety);
                nt->next = t->next;
                t->ty = nt->ty;
                t->str = nt->str;
            }
        }

        int len = tok1->ty->array_len;
        for (Token *t = tok1->next; t != tok2; t = t->next) len = len + t->ty->array_len - 1;
        char *buf = xcalloc((size_t)basety->size * (size_t)len);
        int i = 0;
        for (Token *t = tok1; t != tok2; t = t->next) {
            memcpy(buf + i, t->str, (size_t)t->ty->size);
            i = i + t->ty->size - t->ty->base->size;
        }
        Token *copy = copy_token(tok1);
        *tok1 = *copy;
        tok1->ty = array_of(basety, len);
        tok1->str = buf;
        tok1->next = tok2;
        tok1 = tok2;
    }
}

static bool pp_join_strings = true; /* -E keeps adjacent string literals apart */

Token *preprocess(Token *tok) {
    tok = preprocess2(tok);
    if (cond_incl) error_tok(cond_incl->tok, "unterminated conditional directive");
    convert_pp_tokens(tok);
    if (pp_join_strings) join_adjacent_string_literals(tok);
    return tok;
}

/* ---- -E output */

static void print_tokens(FILE *out, Token *tok) {
    int line = 1;
    for (; tok->kind != TK_EOF; tok = tok->next) {
        if (line > 1 && tok->at_bol) fprintf(out, "\n");
        if (tok->has_space && !tok->at_bol) fprintf(out, " ");
        fprintf(out, "%.*s", tok->len, tok->loc);
        line++;
    }
    fprintf(out, "\n");
}
