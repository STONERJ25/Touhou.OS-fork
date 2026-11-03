/* lex.inc -- tokens, the symbol table and the expression evaluator (used by the preprocessor and by the assembler). */

enum { T_END, T_ID, T_NUM, T_STR, T_OP, T_PARAM, T_PASTE };

typedef struct {
    int kind;
    char *text; /* identifiers, strings, operators ("<<" ...) */
    i64 num;    /* numbers, and the index of a %N parameter */
    int space;  /* white space before the token (to rebuild text for macros that paste) */
} Tok;

typedef struct {
    Tok *t;
    int n, cap;
} TokList;

static void tl_push(TokList *l, Tok t) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->t = xrealloc(l->t, (size_t)l->cap * sizeof(Tok));
    }
    l->t[l->n++] = t;
}

static Tok mk_tok(int kind, const char *text, i64 num) {
    Tok t;
    t.kind = kind;
    t.text = text ? xstrdup(text) : NULL;
    t.num = num;
    t.space = 0;
    return t;
}

static int is_id_start(int c) {
    return isalpha(c) || c == '_' || c == '.' || c == '?' || c == '$';          /* (~ # @ may be inside a name, not start one) */
}

static int is_id_char(int c) {
    return isalnum(c) || c == '_' || c == '$' || c == '#' || c == '@' || c == '~' || c == '.' || c == '?';
}

/* a number written NASM style: 123, 0x1F, 0X1f, 1Fh, 0b101, 101b, 17q, 0o17, 12d; returns the length consumed or 0 */
static size_t parse_number(const char *s, i64 *out) {
    size_t n = 0;
    while (isalnum((u8)s[n]) || s[n] == '_') n++;
    if (!n || !isdigit((u8)s[0])) return 0;
    char tmp[80];
    if (n >= sizeof tmp) fatal("number too long");
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '_') tmp[m++] = s[i];
    tmp[m] = 0;
    int base = 10;
    size_t len = m;
    if (m > 2 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) base = 16, len -= 2;
    else if (m > 2 && tmp[0] == '0' && (tmp[1] == 'b' || tmp[1] == 'B')) base = 2, len -= 2;
    else if (m > 2 && tmp[0] == '0' && (tmp[1] == 'o' || tmp[1] == 'O')) base = 8, len -= 2;
    else if (m > 1 && (tmp[m - 1] == 'h' || tmp[m - 1] == 'H')) base = 16, len -= 1, tmp[m - 1] = 0;
    else if (m > 1 && (tmp[m - 1] == 'b' || tmp[m - 1] == 'B') && !strpbrk(tmp, "2345679")) base = 2, len -= 1, tmp[m - 1] = 0;
    else if (m > 1 && (tmp[m - 1] == 'q' || tmp[m - 1] == 'Q' || tmp[m - 1] == 'o' || tmp[m - 1] == 'O')) base = 8, len -= 1, tmp[m - 1] = 0;
    else if (m > 1 && (tmp[m - 1] == 'd' || tmp[m - 1] == 'D')) len -= 1, tmp[m - 1] = 0;
    const char *digits = base == 10 ? tmp : (tmp[1] == 'x' || tmp[1] == 'X' || tmp[1] == 'b' || tmp[1] == 'B' || tmp[1] == 'o' || tmp[1] == 'O') && tmp[0] == '0' && len + 2 == m ? tmp + 2 : tmp;
    if (len == 0) return 0;
    u64 v = 0;
    for (const char *p = digits; *p; p++) {
        int d = isdigit((u8)*p) ? *p - '0' : tolower((u8)*p) - 'a' + 10;
        if (d < 0 || d >= base) return 0;
        v = v * (u64)base + (u64)d;
    }
    *out = (i64)v;
    return n;
}

static TokList tokenize(const char *s) {
    TokList l = {0};
    int space = 0;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            space = 1;
            s++;
            continue;
        }
        if (*s == ';') break;
        Tok t;
        memset(&t, 0, sizeof t);
        t.space = space;
        space = 0;
        if (*s == '%' && s[1] == '+') {
            t.kind = T_PASTE;
            s += 2;
        } else if (*s == '%' && isdigit((u8)s[1])) {
            t.kind = T_PARAM;
            t.num = s[1] - '0';
            s += 2;
        } else if (*s == '%' && s[1] == '%' && is_id_start((u8)s[2])) {
            const char *b = s;
            s += 2;
            while (is_id_char((u8)*s)) s++;
            t.kind = T_ID;
            t.text = xstrndup(b, (size_t)(s - b));
        } else if (*s == '%' && isalpha((u8)s[1])) {
            const char *b = s++;
            while (isalnum((u8)*s) || *s == '_') s++;
            t.kind = T_ID;
            t.text = xstrndup(b, (size_t)(s - b));
        } else if (isdigit((u8)*s)) {
            size_t n = parse_number(s, &t.num);
            if (!n) fatal("bad number '%.12s'", s);
            t.kind = T_NUM;
            s += n;
        } else if (*s == '$' && isxdigit((u8)s[1]) && isdigit((u8)s[1])) {
            /* $1F: hex written with a dollar */
            i64 v = 0;
            s++;
            while (isxdigit((u8)*s)) v = v * 16 + (isdigit((u8)*s) ? *s - '0' : tolower((u8)*s) - 'a' + 10), s++;
            t.kind = T_NUM;
            t.num = v;
        } else if (is_id_start((u8)*s)) {
            const char *b = s;
            s++;
            while (is_id_char((u8)*s)) s++;
            t.kind = T_ID;
            t.text = xstrndup(b, (size_t)(s - b));
        } else if (*s == '"' || *s == '\'' || *s == '`') {
            char q = *s++;
            Buf b = {0};
            while (*s && *s != q) {
                if (q == '`' && *s == '\\' && s[1]) {
                    s++;
                    buf_u8(&b, *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s == '0' ? 0 : (u8)*s);
                    s++;
                    continue;
                }
                buf_u8(&b, (u8)*s++);
            }
            if (*s != q) fatal("unterminated string");
            s++;
            buf_u8(&b, 0);
            t.kind = T_STR;
            t.text = (char *)b.p;
            t.num = (i64)b.len - 1;
        } else {
            static const char *two[] = {"<<", ">>", "<=", ">=", "==", "!=", "<>", "&&", "||", "^^", "//", "%%"};
            t.kind = T_OP;
            for (size_t i = 0; i < sizeof two / sizeof two[0]; i++)
                if (s[0] == two[i][0] && s[1] == two[i][1]) {
                    t.text = xstrndup(s, 2);
                    s += 2;
                    break;
                }
            if (!t.text) {
                t.text = xstrndup(s, 1);
                s++;
            }
        }
        tl_push(&l, t);
    }
    return l;
}

static int tok_is(const Tok *t, int kind, const char *text) {
    return t->kind == kind && (!text || (t->text && !strcmp(t->text, text)));
}

/* the text of a token list as source again (for messages and for %define bodies kept as text) */
static char *tl_text(const TokList *l, int from, int to) {
    Buf b = {0};
    for (int i = from; i < to; i++) {
        const Tok *t = &l->t[i];
        if (i > from && t->space) buf_u8(&b, ' ');
        if (t->kind == T_NUM) {
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%lld", t->num);
            buf_put(&b, tmp, strlen(tmp));
        } else if (t->kind == T_PARAM) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "%%%d", (int)t->num);
            buf_put(&b, tmp, strlen(tmp));
        } else if (t->kind == T_PASTE) {
            buf_put(&b, "%+", 2);
        } else if (t->kind == T_STR) {
            buf_u8(&b, '"');
            buf_put(&b, t->text, strlen(t->text));
            buf_u8(&b, '"');
        } else if (t->text) {
            buf_put(&b, t->text, strlen(t->text));
        }
    }
    buf_u8(&b, 0);
    return (char *)b.p;
}

/* ------------------------------------------------------------------------------------------------------------ symbols */

enum { SEC_UNDEF = -1, SEC_ABS = -2 };

typedef struct Sym {
    char *name;
    int sec;            /* section index, SEC_UNDEF, or SEC_ABS (an equ constant) */
    i64 val;            /* offset in the section, or the constant */
    int defined;
    int is_global;      /* declared with global */
    int is_extern;      /* declared with extern */
    int created;        /* creation order: the symbol table lists symbols in it */
    int labeled;        /* a label or equ with this name has been read (a second one is an error) */
    int symidx;         /* index in .symtab, set by the writer */
    struct Sym *next;
} Sym;

#define NSYM_BUCKETS 4093
static Sym *sym_tab[NSYM_BUCKETS];
static int sym_counter;
static char *pending_globals[256];
static int npending_globals;

static unsigned str_hash(const char *s) {
    unsigned h = 5381;
    for (; *s; s++) h = h * 33 + (u8)*s;
    return h;
}

static Sym *sym_find(const char *name) {
    for (Sym *s = sym_tab[str_hash(name) % NSYM_BUCKETS]; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    return NULL;
}

/* the entry of a symbol, made when first asked for; `created` is only given when it is defined or declared extern */
static Sym *sym_get(const char *name) {
    Sym *s = sym_find(name);
    if (s) return s;
    s = xcalloc(sizeof *s);
    s->name = xstrdup(name);
    s->sec = SEC_UNDEF;
    unsigned h = str_hash(name) % NSYM_BUCKETS;
    s->next = sym_tab[h];
    sym_tab[h] = s;
    for (int i = 0; i < npending_globals; i++)
        if (!strcmp(pending_globals[i], name)) s->is_global = 1;
    return s;
}

static void sym_define(Sym *s, int sec, i64 val) {
    if (s->defined && (s->sec != sec || s->val != val) && s->created) {
        /* a second definition in a later pass is the normal way symbols reach their final value; two in one pass is an error (checked by the caller) */
    }
    if (!s->created) s->created = ++sym_counter;
    s->defined = 1;
    s->sec = sec;
    s->val = val;
}

/* ------------------------------------------------------------------------------------------------------- expressions */

enum { V_CONST, V_SEC, V_EXT, V_UNK };

typedef struct {
    int kind;
    int sec;        /* V_SEC: the section the value is an offset in */
    i64 num;        /* the constant, or the offset / addend */
    Sym *ext;       /* V_EXT: the undefined symbol the value is relative to */
} Val;

/* what the evaluator needs from the assembler: the position `$`, the start of the section `$$`, and whether symbols may be used */
typedef struct {
    int sec;
    i64 pos;
    int pp_only;    /* preprocessor expression: no symbols at all */
} EvalCtx;

static EvalCtx ectx;

static Val v_const(i64 n) {
    Val v;
    v.kind = V_CONST;
    v.sec = -1;
    v.num = n;
    v.ext = NULL;
    return v;
}

static Val v_unknown(void) {
    Val v = v_const(0);
    v.kind = V_UNK;
    return v;
}

typedef struct {
    Tok *t;
    int n, pos;
} Parser;

static Val parse_cond(Parser *p);

static int p_op(Parser *p, const char *op) {
    if (p->pos < p->n && p->t[p->pos].kind == T_OP && !strcmp(p->t[p->pos].text, op)) {
        p->pos++;
        return 1;
    }
    return 0;
}

static int p_peek_op(Parser *p, const char *op) {
    return p->pos < p->n && p->t[p->pos].kind == T_OP && !strcmp(p->t[p->pos].text, op);
}

static Val v_add(Val a, Val b, int sign) {
    if (a.kind == V_UNK || b.kind == V_UNK) return v_unknown();
    if (b.kind == V_CONST) {
        a.num += sign * b.num;
        return a;
    }
    if (a.kind == V_CONST && sign > 0) {
        b.num += a.num;
        return b;
    }
    if (sign < 0 && a.kind == V_SEC && b.kind == V_SEC && a.sec == b.sec) return v_const(a.num - b.num);
    if (sign < 0 && a.kind == V_SEC && b.kind == V_SEC) fatal("the difference of labels in two sections is not supported");
    fatal("this expression cannot be turned into a relocation");
}

static void need_const(Val a, Val b, const char *op) {
    if ((a.kind != V_CONST && a.kind != V_UNK) || (b.kind != V_CONST && b.kind != V_UNK)) fatal("'%s' needs constant operands (it was given an address)", op);
}

static Val parse_primary(Parser *p) {
    if (p->pos >= p->n) fatal("expression expected");
    Tok *t = &p->t[p->pos++];
    if (t->kind == T_NUM) return v_const(t->num);
    if (t->kind == T_STR) {
        i64 v = 0;
        for (i64 i = 0; i < t->num && i < 8; i++) v |= (i64)(u8)t->text[i] << (8 * i);
        return v_const(v);
    }
    if (t->kind == T_OP && !strcmp(t->text, "(")) {
        Val v = parse_cond(p);
        if (!p_op(p, ")")) fatal("')' expected");
        return v;
    }
    if (t->kind == T_ID) {
        if (!strcmp(t->text, "$") || !strcmp(t->text, "$$")) {
            if (ectx.pp_only) fatal("'%s' cannot be used in a preprocessor expression", t->text);
            Val v = v_const(0);
            v.kind = V_SEC;
            v.sec = ectx.sec;
            v.num = t->text[1] ? 0 : ectx.pos;
            return v;
        }
        if (ectx.pp_only) fatal("'%s' is not a number (preprocessor expressions know numbers and %%assign names only)", t->text);
        Sym *s = sym_get(t->text);
        if (s->defined && s->sec == SEC_ABS) return v_const(s->val);
        if (s->defined) {
            Val v = v_const(0);
            v.kind = V_SEC;
            v.sec = s->sec;
            v.num = s->val;
            return v;
        }
        if (s->is_extern) {
            Val v = v_const(0);
            v.kind = V_EXT;
            v.ext = s;
            return v;
        }
        Val u = v_unknown();                                        /* defined later: the next pass knows it */
        u.ext = s;
        return u;
    }
    fatal("unexpected '%s' in an expression", t->text ? t->text : "?");
}

static Val parse_unary(Parser *p) {
    if (p_op(p, "-")) {
        Val v = parse_unary(p);
        if (v.kind == V_UNK) return v;
        if (v.kind != V_CONST) fatal("cannot negate an address");
        v.num = -v.num;
        return v;
    }
    if (p_op(p, "+")) return parse_unary(p);
    if (p_op(p, "~")) {
        Val v = parse_unary(p);
        if (v.kind == V_UNK) return v;
        if (v.kind != V_CONST) fatal("cannot complement an address");
        v.num = ~v.num;
        return v;
    }
    if (p_op(p, "!")) {
        Val v = parse_unary(p);
        if (v.kind == V_UNK) return v;
        if (v.kind != V_CONST) fatal("'!' needs a constant");
        v.num = !v.num;
        return v;
    }
    return parse_primary(p);
}

static Val parse_mul(Parser *p) {
    Val a = parse_unary(p);
    for (;;) {
        static const char *mul_ops[] = {"*", "/", "%", "//", "%%"};
        const char *op = NULL;
        for (int i = 0; i < 5; i++)
            if (p_peek_op(p, mul_ops[i])) op = mul_ops[i];
        if (!op) return a;
        p->pos++;
        Val b = parse_unary(p);
        need_const(a, b, op);
        if (a.kind == V_UNK || b.kind == V_UNK) {
            a = v_unknown();
            continue;
        }
        if (op[0] == '*') a.num *= b.num;
        else {
            if (!b.num) fatal("division by zero");
            if (!strcmp(op, "/")) a.num = (i64)((u64)a.num / (u64)b.num);
            else if (!strcmp(op, "//")) a.num = a.num / b.num;
            else if (!strcmp(op, "%")) a.num = (i64)((u64)a.num % (u64)b.num);
            else a.num = a.num % b.num;
        }
    }
}

static Val parse_add(Parser *p) {
    Val a = parse_mul(p);
    for (;;) {
        if (p_op(p, "+")) a = v_add(a, parse_mul(p), 1);
        else if (p_op(p, "-")) a = v_add(a, parse_mul(p), -1);
        else return a;
    }
}

static Val parse_shift(Parser *p) {
    Val a = parse_add(p);
    for (;;) {
        int left = p_peek_op(p, "<<");
        if (!left && !p_peek_op(p, ">>")) return a;
        p->pos++;
        Val b = parse_add(p);
        need_const(a, b, left ? "<<" : ">>");
        if (a.kind == V_UNK || b.kind == V_UNK) a = v_unknown();
        else a.num = left ? (i64)((u64)a.num << (b.num & 63)) : (i64)((u64)a.num >> (b.num & 63));
    }
}

static Val parse_binary_level(Parser *p, int level);

/* & then ^ then | then comparisons then && ^^ || (NASM's precedence, loosest last) */
static Val parse_binary_level(Parser *p, int level) {
    static const char *ops[][4] = {{"&", NULL}, {"^", NULL}, {"|", NULL}, {"==", "!=", "<>", "="}, {"<", ">", "<=", ">="}, {"&&", NULL}, {"^^", NULL}, {"||", NULL}};
    if (level < 0) return parse_shift(p);
    Val a = parse_binary_level(p, level - 1);
    for (;;) {
        const char *op = NULL;
        for (int i = 0; i < 4 && ops[level][i]; i++)
            if (p_peek_op(p, ops[level][i])) op = ops[level][i];
        if (!op) return a;
        p->pos++;
        Val b = parse_binary_level(p, level - 1);
        need_const(a, b, op);
        if (a.kind == V_UNK || b.kind == V_UNK) {
            a = v_unknown();
            continue;
        }
        i64 x = a.num, y = b.num, r;
        if (!strcmp(op, "&")) r = x & y;
        else if (!strcmp(op, "^")) r = x ^ y;
        else if (!strcmp(op, "|")) r = x | y;
        else if (!strcmp(op, "==") || !strcmp(op, "=")) r = x == y;
        else if (!strcmp(op, "!=") || !strcmp(op, "<>")) r = x != y;
        else if (!strcmp(op, "<")) r = x < y;
        else if (!strcmp(op, ">")) r = x > y;
        else if (!strcmp(op, "<=")) r = x <= y;
        else if (!strcmp(op, ">=")) r = x >= y;
        else if (!strcmp(op, "&&")) r = x && y;
        else if (!strcmp(op, "^^")) r = (x != 0) != (y != 0);
        else r = x || y;
        a = v_const(r);
    }
}

static Val parse_cond(Parser *p) {
    return parse_binary_level(p, 7);
}

/* evaluates tokens [from,to) as one expression; the whole range must be used */
static Val eval_range(Tok *t, int from, int to) {
    Parser p = {t + from, to - from, 0};
    Val v = parse_cond(&p);
    if (p.pos != p.n) fatal("unexpected '%s' after the expression", p.t[p.pos].text ? p.t[p.pos].text : "token");
    return v;
}
