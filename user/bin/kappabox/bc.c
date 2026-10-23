/*
 * bc: arbitrary precision calculator language (POSIX bc plus the usual GNU extensions).
 * The program is read statement by statement, parsed into a small tree and executed at once, so errors in one statement
 * do not stop the next.  Numbers come from bignum.c.  The math library (-l) is bc source at the bottom of this file.
 */
#include "kappabox.h"
#include "bignum.h"
#include <setjmp.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ output with GNU style line wrapping */

static char g_obuf[1 << 16];
static size_t g_olen;
static int g_col;                       /* column of the output cursor, for wrapping numbers */
static int g_linelen = 70;              /* BC_LINE_LENGTH; 0 = never wrap */

static void bc_flush(void) {
    size_t off = 0;
    while (off < g_olen) {
        ssize_t w = write(1, g_obuf + off, g_olen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    g_olen = 0;
}

static void put(int c) {
    if (g_olen == sizeof g_obuf) bc_flush();
    g_obuf[g_olen++] = (char)c;
    if (c == '\n') g_col = 0;
    else g_col++;
}

static void put_wrapped(int c) {
    if (g_linelen > 0 && g_col >= g_linelen - 1) {
        put('\\');
        put('\n');
        g_col = 0;
    }
    put(c);
}

static void put_str(const char *s) { while (*s) put((unsigned char)*s++); }

static void err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err(const char *fmt, ...) {
    va_list ap;
    bc_flush();
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void bc_exit(int status) {
    bc_flush();
    exit(status);
}

/* ------------------------------------------------------------------ names and variables */

typedef struct { Num **v; int n, cap; } Arr;

typedef struct {
    char *name;
    Num *val;                           /* simple variable (NULL = 0) */
    Arr *arr;                           /* array (NULL = empty) */
} Var;

typedef struct { Var *v; int n, cap; int *hash; int hcap; } VarTab;

static VarTab g_svars, g_avars;         /* simple variables and arrays have separate name spaces */

static unsigned hash_name(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static int intern(VarTab *t, const char *name) {
    if (t->hcap == 0 || t->n * 2 >= t->hcap) {
        int nc = t->hcap ? t->hcap * 2 : 64;
        int *nh = malloc((size_t)nc * sizeof(int));
        if (!nh) kb_die("out of memory");
        for (int i = 0; i < nc; i++) nh[i] = -1;
        for (int i = 0; i < t->n; i++) {
            unsigned h = hash_name(t->v[i].name) & (unsigned)(nc - 1);
            while (nh[h] >= 0) h = (h + 1) & (unsigned)(nc - 1);
            nh[h] = i;
        }
        free(t->hash);
        t->hash = nh;
        t->hcap = nc;
    }
    unsigned h = hash_name(name) & (unsigned)(t->hcap - 1);
    while (t->hash[h] >= 0) {
        if (!strcmp(t->v[t->hash[h]].name, name)) return t->hash[h];
        h = (h + 1) & (unsigned)(t->hcap - 1);
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->v = realloc(t->v, (size_t)t->cap * sizeof(Var));
        if (!t->v) kb_die("out of memory");
    }
    t->v[t->n].name = strdup(name);
    t->v[t->n].val = NULL;
    t->v[t->n].arr = NULL;
    t->hash[h] = t->n;
    return t->n++;
}

static void arr_free(Arr *a) {
    if (!a) return;
    for (int i = 0; i < a->n; i++) num_free(a->v[i]);
    free(a->v);
    free(a);
}

static Arr *arr_copy(const Arr *a) {
    Arr *r = calloc(1, sizeof *r);
    if (!r) kb_die("out of memory");
    if (a && a->n) {
        r->v = calloc((size_t)a->n, sizeof(Num *));
        if (!r->v) kb_die("out of memory");
        r->n = r->cap = a->n;
        for (int i = 0; i < a->n; i++) r->v[i] = a->v[i] ? num_copy(a->v[i]) : NULL;
    }
    return r;
}

/* ------------------------------------------------------------------ interpreter state */

static int g_scale, g_ibase = 10, g_obase = 10;
static Num *g_last;
static int g_quiet, g_mathlib;
static int g_depth;
static char *g_stack_top;

/* saved values of variables that function calls shadow; unwound on return and after a runtime error */
typedef struct { int is_arr, id; Num *val; Arr *arr; } Save;
static Save *g_saves;
static int g_nsaves, g_capsaves;

static void push_save(int is_arr, int id) {
    if (g_nsaves == g_capsaves) {
        g_capsaves = g_capsaves ? g_capsaves * 2 : 256;
        g_saves = realloc(g_saves, (size_t)g_capsaves * sizeof(Save));
        if (!g_saves) kb_die("out of memory");
    }
    Save *s = &g_saves[g_nsaves++];
    s->is_arr = is_arr;
    s->id = id;
    if (is_arr) { s->arr = g_avars.v[id].arr; g_avars.v[id].arr = NULL; s->val = NULL; }
    else { s->val = g_svars.v[id].val; g_svars.v[id].val = NULL; s->arr = NULL; }
}

static void unwind(int depth) {
    while (g_nsaves > depth) {
        Save *s = &g_saves[--g_nsaves];
        if (s->is_arr) { arr_free(g_avars.v[s->id].arr); g_avars.v[s->id].arr = s->arr; }
        else { num_free(g_svars.v[s->id].val); g_svars.v[s->id].val = s->val; }
    }
}

/* ------------------------------------------------------------------ syntax tree */

enum {
    N_NUM, N_VAR, N_ELEM, N_SPECIAL, N_ASSIGN, N_BIN, N_NEG, N_NOT, N_INCDEC, N_CALL, N_SCALEFN, N_LENGTH, N_SQRT, N_READ,
    N_AND, N_OR, N_REL, N_ARRAYARG,
    S_EXPR, S_STR, S_PRINT, S_IF, S_WHILE, S_FOR, S_BREAK, S_CONTINUE, S_RETURN, S_HALT, S_BLOCK, S_NOP
};
enum { SP_SCALE, SP_IBASE, SP_OBASE, SP_LAST };

typedef struct Node {
    int type;
    int op;                             /* operator, or SP_x, or flags */
    int id;                             /* variable or function index */
    int flag;                           /* postfix for ++/--, "this is an assignment" for statements */
    struct Node *a, *b, *c, *d;
    struct Node *next;                  /* statement list and argument list */
    char *str;
    Num *num;
    int nbase;                          /* ibase the cached constant was converted with */
} Node;

typedef struct {
    char *name;
    int defined, is_void;
    int nparams, nautos;
    int *pid, *parr, *aid, *aarr;
    Node *body;
} Func;

static Func *g_funcs;
static int g_nfuncs, g_capfuncs;
static VarTab g_ftab;                   /* function names -> index into g_funcs */

static int func_index(const char *name) {
    int id = intern(&g_ftab, name);
    while (id >= g_capfuncs) {
        int nc = g_capfuncs ? g_capfuncs * 2 : 32;
        g_funcs = realloc(g_funcs, (size_t)nc * sizeof(Func));
        if (!g_funcs) kb_die("out of memory");
        memset(g_funcs + g_capfuncs, 0, (size_t)(nc - g_capfuncs) * sizeof(Func));
        g_capfuncs = nc;
    }
    if (id >= g_nfuncs) g_nfuncs = id + 1;
    g_funcs[id].name = g_ftab.v[id].name;
    return id;
}

static Node *node(int type) {
    Node *n = calloc(1, sizeof *n);
    if (!n) kb_die("out of memory");
    n->type = type;
    n->id = -1;
    return n;
}

static void node_free(Node *n) {
    while (n) {
        Node *next = n->next;
        node_free(n->a); node_free(n->b); node_free(n->c); node_free(n->d);
        free(n->str);
        num_free(n->num);
        free(n);
        n = next;
    }
}

/* ------------------------------------------------------------------ input sources */

typedef struct {
    FILE *f;
    const char *text;
    size_t tpos;
    const char *name;
    int line;
} Src;

static Src g_src;
static int g_srcline;

static int rd(void) {
    int c;
    if (g_src.f) c = fgetc(g_src.f);
    else if (g_src.text[g_src.tpos]) c = (unsigned char)g_src.text[g_src.tpos++];
    else c = EOF;
    if (c == '\n') g_src.line++;
    return c;
}

static void unrd(int c) {
    if (c == EOF) return;
    if (c == '\n') g_src.line--;
    if (g_src.f) ungetc(c, g_src.f);
    else g_src.tpos--;
}

/* ------------------------------------------------------------------ lexer */

enum {
    T_EOF = 256, T_NL, T_NUM, T_ID, T_STR, T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR, T_INC, T_DEC, T_OPEQ,
    T_DEFINE, T_BREAK, T_QUIT, T_LENGTH, T_RETURN, T_FOR, T_IF, T_WHILE, T_SQRT, T_SCALE, T_IBASE, T_OBASE, T_AUTO,
    T_ELSE, T_HALT, T_LAST, T_CONTINUE, T_PRINT, T_READ, T_LIMITS
};

static const struct { const char *name; int tok; } g_keywords[] = {
    { "define", T_DEFINE }, { "break", T_BREAK }, { "quit", T_QUIT }, { "length", T_LENGTH }, { "return", T_RETURN },
    { "for", T_FOR }, { "if", T_IF }, { "while", T_WHILE }, { "sqrt", T_SQRT }, { "scale", T_SCALE }, { "ibase", T_IBASE },
    { "obase", T_OBASE }, { "auto", T_AUTO }, { "else", T_ELSE }, { "halt", T_HALT }, { "last", T_LAST },
    { "continue", T_CONTINUE }, { "print", T_PRINT }, { "read", T_READ }, { "limits", T_LIMITS }, { NULL, 0 }
};

static int g_tok, g_tok_valid, g_tok_op;
static char *g_tokstr;
static size_t g_tokcap;
static jmp_buf g_parse_jmp;

static void tok_put(size_t i, int c) {
    if (i + 2 > g_tokcap) {
        g_tokcap = g_tokcap ? g_tokcap * 2 : 256;
        g_tokstr = realloc(g_tokstr, g_tokcap);
        if (!g_tokstr) kb_die("out of memory");
    }
    g_tokstr[i] = (char)c;
    g_tokstr[i + 1] = 0;
}

static int g_tok_line;

static void syntax_error(const char *msg) {
    err("%s %d: %s", g_src.name, g_tok_valid ? g_tok_line : g_src.line, msg);
    longjmp(g_parse_jmp, 1);
}

static int is_numch(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

static int lex(void) {
    int c;
    for (;;) {
        g_tok_line = g_src.line;
        c = rd();
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (c == '\\') {
            int d = rd();
            if (d == '\n') continue;
            if (d == '\r') { int e = rd(); if (e == '\n') continue; unrd(e); }
            unrd(d);
            return '\\';
        }
        if (c == '#') {
            while ((c = rd()) != '\n' && c != EOF) ;
            return c == EOF ? T_EOF : T_NL;
        }
        if (c == '/') {
            int d = rd();
            if (d == '*') {
                int prev = 0;
                for (;;) {
                    c = rd();
                    if (c == EOF) { err("%s %d: unterminated comment", g_src.name, g_src.line); return T_EOF; }
                    if (prev == '*' && c == '/') break;
                    prev = c;
                }
                continue;
            }
            unrd(d);
        }
        break;
    }
    if (c == EOF) return T_EOF;
    if (c == '\n') return T_NL;
    if (is_numch(c) || (c == '.')) {
        size_t i = 0;
        int dot = 0;
        if (c == '.') {
            int d = rd();
            unrd(d);
            if (!is_numch(d)) return '.';
        }
        for (;;) {
            if (c == '.') { if (dot) break; dot = 1; }
            else if (!is_numch(c)) {
                if (c == '\\') {            /* line continuation inside a number */
                    int d = rd();
                    if (d == '\n') { c = rd(); continue; }
                    unrd(d);
                }
                break;
            }
            tok_put(i++, c);
            c = rd();
        }
        unrd(c);
        return T_NUM;
    }
    if ((c >= 'a' && c <= 'z') || c == '_') {
        size_t i = 0;
        while ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') { tok_put(i++, c); c = rd(); }
        unrd(c);
        for (int k = 0; g_keywords[k].name; k++)
            if (!strcmp(g_keywords[k].name, g_tokstr)) return g_keywords[k].tok;
        return T_ID;
    }
    if (c == '"') {
        size_t i = 0;
        tok_put(0, 0);
        while ((c = rd()) != '"') {
            if (c == EOF) { err("%s %d: unterminated string", g_src.name, g_src.line); return T_EOF; }
            tok_put(i++, c);
        }
        tok_put(i, 0);
        return T_STR;
    }
    int d;
    switch (c) {
    case '=': d = rd(); if (d == '=') return T_EQ; unrd(d); return '=';
    case '!': d = rd(); if (d == '=') return T_NE; unrd(d); return '!';
    case '<': d = rd(); if (d == '=') return T_LE; unrd(d); return '<';
    case '>': d = rd(); if (d == '=') return T_GE; unrd(d); return '>';
    case '&': d = rd(); if (d == '&') return T_AND; unrd(d); return '&';
    case '|': d = rd(); if (d == '|') return T_OR; unrd(d); return '|';
    case '+': d = rd(); if (d == '+') return T_INC; if (d == '=') { g_tok_op = '+'; return T_OPEQ; } unrd(d); return '+';
    case '-': d = rd(); if (d == '-') return T_DEC; if (d == '=') { g_tok_op = '-'; return T_OPEQ; } unrd(d); return '-';
    case '*': d = rd(); if (d == '=') { g_tok_op = '*'; return T_OPEQ; } unrd(d); return '*';
    case '/': d = rd(); if (d == '=') { g_tok_op = '/'; return T_OPEQ; } unrd(d); return '/';
    case '%': d = rd(); if (d == '=') { g_tok_op = '%'; return T_OPEQ; } unrd(d); return '%';
    case '^': d = rd(); if (d == '=') { g_tok_op = '^'; return T_OPEQ; } unrd(d); return '^';
    default: return c;
    }
}

static int peek(void) {
    if (!g_tok_valid) { g_tok = lex(); g_tok_valid = 1; }
    return g_tok;
}

static void adv(void) { peek(); g_tok_valid = 0; }

static void expect(int t) {
    if (peek() != t) syntax_error("syntax error");
    adv();
}

static void skip_nl(void) { while (peek() == T_NL) adv(); }

/* ------------------------------------------------------------------ parser */

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_unary(void);
static Node *parse_add(void);
static Node *parse_rel(void);
static int exec_stmt(Node *);

static Node *bin(int op, Node *a, Node *b) {
    Node *n = node(N_BIN);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static int is_lvalue(const Node *n) { return n->type == N_VAR || n->type == N_ELEM || n->type == N_SPECIAL; }

static Node *parse_call_args(Node *call) {
    Node **tail = &call->a;
    if (peek() != ')') {
        for (;;) {
            Node *arg = parse_expr();
            *tail = arg;
            tail = &arg->next;
            if (peek() == ',') { adv(); continue; }
            break;
        }
    }
    expect(')');
    return call;
}

static Node *parse_primary(void) {
    int t = peek();
    Node *n;
    switch (t) {
    case T_NUM:
        n = node(N_NUM);
        n->str = strdup(g_tokstr);
        adv();
        return n;
    case '(':
        adv();
        n = parse_expr();
        expect(')');
        {
            Node *p = node(N_BIN);          /* a parenthesised assignment still prints */
            p->op = 'p';
            p->a = n;
            return p;
        }
    case T_ID: {
        char name[256];
        strlcpy(name, g_tokstr, sizeof name);
        adv();
        if (peek() == '(') {
            adv();
            n = node(N_CALL);
            n->id = func_index(name);
            return parse_call_args(n);
        }
        if (peek() == '[') {
            adv();
            if (peek() == ']') {                /* name[] passes (or measures) a whole array */
                adv();
                n = node(N_ARRAYARG);
                n->id = intern(&g_avars, name);
                return n;
            }
            n = node(N_ELEM);
            n->id = intern(&g_avars, name);
            n->a = parse_expr();
            expect(']');
            return n;
        }
        n = node(N_VAR);
        n->id = intern(&g_svars, name);
        return n;
    }
    case T_SCALE:
        adv();
        if (peek() == '(') {
            adv();
            n = node(N_SCALEFN);
            n->a = parse_expr();
            expect(')');
            return n;
        }
        n = node(N_SPECIAL);
        n->op = SP_SCALE;
        return n;
    case T_IBASE: adv(); n = node(N_SPECIAL); n->op = SP_IBASE; return n;
    case T_OBASE: adv(); n = node(N_SPECIAL); n->op = SP_OBASE; return n;
    case T_LAST: case '.': adv(); n = node(N_SPECIAL); n->op = SP_LAST; return n;
    case T_LENGTH:
        adv();
        expect('(');
        n = node(N_LENGTH);
        n->a = parse_expr();
        expect(')');
        return n;
    case T_SQRT:
        adv();
        expect('(');
        n = node(N_SQRT);
        n->a = parse_expr();
        expect(')');
        return n;
    case T_READ:
        adv();
        expect('(');
        expect(')');
        return node(N_READ);
    default:
        syntax_error("syntax error");
    }
    return NULL;
}

static Node *parse_postfix(void) {
    Node *n = parse_primary();
    if (is_lvalue(n)) {
        int t = peek();
        if (t == T_INC || t == T_DEC) {
            Node *p = node(N_INCDEC);
            p->op = t == T_INC ? '+' : '-';
            p->flag = 1;                    /* postfix: value is the old one */
            p->a = n;
            adv();
            return p;
        }
        if (t == '=' || t == T_OPEQ) {
            int op = t == '=' ? 0 : g_tok_op;
            adv();
            Node *as = node(N_ASSIGN);
            as->op = op;
            as->a = n;
            /* the right side takes everything that binds tighter than a comparison, and nested assignments */
            as->b = parse_add();
            return as;
        }
    }
    return n;
}

static Node *parse_unary(void) {
    int t = peek();
    if (t == '-') {
        adv();
        Node *n = node(N_NEG);
        n->a = parse_unary();
        return n;
    }
    if (t == '!') {
        adv();
        Node *n = node(N_NOT);
        n->a = parse_rel();
        return n;
    }
    if (t == T_INC || t == T_DEC) {
        adv();
        Node *lv = parse_primary();
        if (!is_lvalue(lv)) syntax_error("syntax error");
        Node *n = node(N_INCDEC);
        n->op = t == T_INC ? '+' : '-';
        n->a = lv;
        return n;
    }
    return parse_postfix();
}

static Node *parse_pow(void) {
    Node *l = parse_unary();
    if (peek() == '^') {
        adv();
        return bin('^', l, parse_pow());
    }
    return l;
}

static Node *parse_mul(void) {
    Node *l = parse_pow();
    for (;;) {
        int t = peek();
        if (t != '*' && t != '/' && t != '%') return l;
        adv();
        l = bin(t, l, parse_pow());
    }
}

static Node *parse_add(void) {
    Node *l = parse_mul();
    for (;;) {
        int t = peek();
        if (t != '+' && t != '-') return l;
        adv();
        l = bin(t, l, parse_mul());
    }
}

static Node *parse_rel(void) {
    Node *l = parse_add();
    for (;;) {
        int t = peek(), op;
        switch (t) {
        case '<': op = '<'; break;
        case '>': op = '>'; break;
        case T_LE: op = 'l'; break;
        case T_GE: op = 'g'; break;
        case T_EQ: op = '='; break;
        case T_NE: op = '!'; break;
        default: return l;
        }
        adv();
        Node *n = node(N_REL);
        n->op = op;
        n->a = l;
        n->b = parse_add();
        l = n;
    }
}

static Node *parse_and(void) {
    Node *l = parse_rel();
    while (peek() == T_AND) {
        adv();
        Node *n = node(N_AND);
        n->a = l;
        n->b = parse_rel();
        l = n;
    }
    return l;
}

static Node *parse_expr(void) {
    Node *l = parse_and();
    while (peek() == T_OR) {
        adv();
        Node *n = node(N_OR);
        n->a = l;
        n->b = parse_and();
        l = n;
    }
    return l;
}

/* statements */

static void end_stmt(void) {
    int t = peek();
    if (t == T_NL || t == ';') adv();
    else if (t != '}' && t != T_EOF && t != T_ELSE) syntax_error("syntax error");
}

static Node *parse_block(void) {
    /* after '{' */
    Node *head = NULL, **tail = &head;
    for (;;) {
        int t = peek();
        if (t == T_NL || t == ';') { adv(); continue; }
        if (t == '}') { adv(); break; }
        if (t == T_EOF) syntax_error("syntax error");
        Node *s = parse_stmt();
        if (s) { *tail = s; tail = &s->next; }
    }
    Node *b = node(S_BLOCK);
    b->a = head;
    return b;
}

static Node *parse_body(void) {
    skip_nl();
    return parse_stmt();
}

static Node *parse_stmt(void) {
    int t = peek();
    Node *n;
    switch (t) {
    case T_NL: case ';':
        adv();
        return node(S_NOP);
    case '{':
        adv();
        return parse_block();
    case T_IF:
        adv();
        expect('(');
        n = node(S_IF);
        n->a = parse_expr();
        expect(')');
        n->b = parse_body();
        {
            /* "else" may follow after newlines; anything else stays for the next statement */
            if (peek() == T_NL) skip_nl();
            if (peek() == T_ELSE) {
                adv();
                n->c = parse_body();
            }
        }
        return n;
    case T_WHILE:
        adv();
        expect('(');
        n = node(S_WHILE);
        n->a = parse_expr();
        expect(')');
        n->b = parse_body();
        return n;
    case T_FOR:
        adv();
        expect('(');
        n = node(S_FOR);
        if (peek() != ';') n->a = parse_expr();
        expect(';');
        if (peek() != ';') n->b = parse_expr();
        expect(';');
        if (peek() != ')') n->c = parse_expr();
        expect(')');
        n->d = parse_body();
        return n;
    case T_BREAK: adv(); end_stmt(); return node(S_BREAK);
    case T_CONTINUE: adv(); end_stmt(); return node(S_CONTINUE);
    case T_HALT: adv(); end_stmt(); return node(S_HALT);
    case T_QUIT:
        bc_exit(0);
        return NULL;
    case T_LIMITS:
        adv();
        end_stmt();
        n = node(S_STR);
        n->str = strdup("BC_BASE_MAX     = 99\nBC_DIM_MAX      = 2147483647\nBC_SCALE_MAX    = 2147483647\nBC_STRING_MAX   = 2147483647\n");
        return n;
    case T_RETURN:
        adv();
        n = node(S_RETURN);
        if (peek() != T_NL && peek() != ';' && peek() != '}' && peek() != T_EOF) {
            if (peek() == '(') {
                adv();
                if (peek() == ')') adv();       /* return () */
                else { n->a = parse_expr(); expect(')'); }
            } else n->a = parse_expr();
        }
        end_stmt();
        return n;
    case T_STR:
        n = node(S_STR);
        n->str = strdup(g_tokstr);
        adv();
        end_stmt();
        return n;
    case T_PRINT: {
        adv();
        n = node(S_PRINT);
        Node **tail = &n->a;
        for (;;) {
            Node *e;
            if (peek() == T_STR) {
                e = node(S_STR);
                e->str = strdup(g_tokstr);
                adv();
            } else e = parse_expr();
            *tail = e;
            tail = &e->next;
            if (peek() == ',') { adv(); continue; }
            break;
        }
        end_stmt();
        return n;
    }
    case T_AUTO:
    case T_DEFINE:
        syntax_error("syntax error");
        return NULL;
    default:
        n = node(S_EXPR);
        n->a = parse_expr();
        n->flag = n->a->type == N_ASSIGN;
        end_stmt();
        return n;
    }
}

/* define name(params) { auto ...; statements } */
static void parse_define(void) {
    adv();
    int is_void = 0;
    if (peek() == T_ID && !strcmp(g_tokstr, "void")) {
        adv();
        is_void = 1;
    }
    if (peek() != T_ID) syntax_error("syntax error");
    char name[256];
    strlcpy(name, g_tokstr, sizeof name);
    adv();
    expect('(');
    int np = 0, cp = 8;
    int *pid = malloc((size_t)cp * sizeof(int)), *parr = malloc((size_t)cp * sizeof(int));
    if (!pid || !parr) kb_die("out of memory");
    if (peek() != ')') {
        for (;;) {
            if (peek() != T_ID) syntax_error("syntax error");
            char pn[256];
            strlcpy(pn, g_tokstr, sizeof pn);
            adv();
            int isarr = 0;
            if (peek() == '[') { adv(); expect(']'); isarr = 1; }
            if (np == cp) { cp *= 2; pid = realloc(pid, (size_t)cp * sizeof(int)); parr = realloc(parr, (size_t)cp * sizeof(int)); }
            pid[np] = intern(isarr ? &g_avars : &g_svars, pn);
            parr[np++] = isarr;
            if (peek() == ',') { adv(); continue; }
            break;
        }
    }
    expect(')');
    skip_nl();
    expect('{');
    int na = 0, ca = 8;
    int *aid = malloc((size_t)ca * sizeof(int)), *aarr = malloc((size_t)ca * sizeof(int));
    if (!aid || !aarr) kb_die("out of memory");
    for (;;) {
        while (peek() == T_NL || peek() == ';') adv();
        if (peek() != T_AUTO) break;
        adv();
        for (;;) {
            if (peek() != T_ID) syntax_error("syntax error");
            char an[256];
            strlcpy(an, g_tokstr, sizeof an);
            adv();
            int isarr = 0;
            if (peek() == '[') { adv(); expect(']'); isarr = 1; }
            if (na == ca) { ca *= 2; aid = realloc(aid, (size_t)ca * sizeof(int)); aarr = realloc(aarr, (size_t)ca * sizeof(int)); }
            aid[na] = intern(isarr ? &g_avars : &g_svars, an);
            aarr[na++] = isarr;
            if (peek() == ',') { adv(); continue; }
            break;
        }
        end_stmt();
    }
    Node *body = parse_block();
    int fid = func_index(name);
    Func *f = &g_funcs[fid];
    if (f->defined) {
        node_free(f->body);
        free(f->pid); free(f->parr); free(f->aid); free(f->aarr);
    }
    f->defined = 1;
    f->is_void = is_void;
    f->nparams = np; f->pid = pid; f->parr = parr;
    f->nautos = na; f->aid = aid; f->aarr = aarr;
    f->body = body;
}

/* ------------------------------------------------------------------ interpreter */

static jmp_buf g_rt_jmp;
static Func *g_curfunc;
static Num *g_retval;

static void rt_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void rt_error(const char *fmt, ...) {
    va_list ap;
    char msg[256];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    err("Runtime error (func=%s, adr=%d): %s", g_curfunc ? g_curfunc->name : "(main)", g_nsaves, msg);
    longjmp(g_rt_jmp, 1);
}

static Num *eval(Node *n);

static long to_long(Num *x, const char *what) {
    long v;
    if (!num_to_long(x, &v)) rt_error("%s too large", what);
    return v;
}

static Num *get_var(int id) {
    Num *v = g_svars.v[id].val;
    return v ? num_copy(v) : num_new();
}

static Num *arr_get(int id, long idx) {
    Arr *a = g_avars.v[id].arr;
    if (idx < 0) rt_error("Array index is negative");
    if (a && idx < a->n && a->v[idx]) return num_copy(a->v[idx]);
    return num_new();
}

static void arr_set(int id, long idx, Num *val) {
    Arr *a = g_avars.v[id].arr;
    if (idx < 0) rt_error("Array index is negative");
    if (idx > 100000000L) rt_error("Array too big");
    if (!a) {
        a = calloc(1, sizeof *a);
        if (!a) kb_die("out of memory");
        g_avars.v[id].arr = a;
    }
    if (idx >= a->cap) {
        int nc = a->cap ? a->cap : 16;
        while (nc <= idx) nc *= 2;
        a->v = realloc(a->v, (size_t)nc * sizeof(Num *));
        if (!a->v) kb_die("out of memory");
        memset(a->v + a->cap, 0, (size_t)(nc - a->cap) * sizeof(Num *));
        a->cap = nc;
    }
    if (idx >= a->n) a->n = (int)idx + 1;
    num_free(a->v[idx]);
    a->v[idx] = val;
}

/* stores VAL (taken over) in the lvalue node */
static void store(Node *lv, Num *val) {
    switch (lv->type) {
    case N_VAR:
        num_free(g_svars.v[lv->id].val);
        g_svars.v[lv->id].val = val;
        break;
    case N_ELEM: {
        Num *ix = eval(lv->a);
        long i = to_long(ix, "Array index");
        num_free(ix);
        arr_set(lv->id, i, val);
        break;
    }
    case N_SPECIAL: {
        long v;
        switch (lv->op) {
        case SP_SCALE:
            v = to_long(val, "Scale");
            if (v < 0 || v > 100000000L) rt_error("Invalid scale");
            g_scale = (int)v;
            break;
        case SP_IBASE:
            v = to_long(val, "ibase");
            if (v < 2 || v > 16) rt_error("ibase must be between 2 and 16");
            g_ibase = (int)v;
            break;
        case SP_OBASE:
            v = to_long(val, "obase");
            if (v < 2 || v > 99999) rt_error("obase must be at least 2");
            g_obase = (int)v;
            break;
        default:
            num_free(g_last);
            g_last = num_copy(val);
            break;
        }
        num_free(val);
        break;
    }
    }
}

static Num *fetch(Node *lv) {
    switch (lv->type) {
    case N_VAR: return get_var(lv->id);
    case N_ELEM: {
        Num *ix = eval(lv->a);
        long i = to_long(ix, "Array index");
        num_free(ix);
        return arr_get(lv->id, i);
    }
    default:
        switch (lv->op) {
        case SP_SCALE: return num_from_long(g_scale);
        case SP_IBASE: return num_from_long(g_ibase);
        case SP_OBASE: return num_from_long(g_obase);
        default: return num_copy(g_last);
        }
    }
}

static Num *arith(int op, Num *a, Num *b) {
    Num *r = NULL;
    switch (op) {
    case '+': return num_add(a, b);
    case '-': return num_sub(a, b);
    case '*': return num_mul(a, b, g_scale);
    case '/':
        r = num_div(a, b, g_scale);
        if (!r) rt_error("Divide by zero");
        return r;
    case '%':
        r = num_mod(a, b, g_scale);
        if (!r) rt_error("Modulo by zero");
        return r;
    case '^':
        if (b->n == 0) return num_from_long(1);
        r = num_pow(a, b, g_scale);
        if (!r) rt_error("exponent too large in raise");
        return r;
    }
    return num_new();
}

static Num *eval_const(Node *n) {
    if (!n->num || n->nbase != g_ibase) {
        const char *end;
        num_free(n->num);
        n->num = num_parse(n->str, g_ibase, &end);
        n->nbase = g_ibase;
    }
    return num_copy(n->num);
}

static Num *call_func(Node *call);

static Num *eval(Node *n) {
    Num *a, *b, *r;
    switch (n->type) {
    case N_NUM: return eval_const(n);
    case N_VAR: case N_ELEM: case N_SPECIAL: return fetch(n);
    case N_BIN:
        if (n->op == 'p') return eval(n->a);
        a = eval(n->a);
        b = eval(n->b);
        r = arith(n->op, a, b);
        num_free(a);
        num_free(b);
        return r;
    case N_NEG:
        a = eval(n->a);
        r = num_neg(a);
        num_free(a);
        return r;
    case N_NOT:
        a = eval(n->a);
        r = num_from_long(num_is_zero(a));
        num_free(a);
        return r;
    case N_AND:
        a = eval(n->a);
        if (num_is_zero(a)) return a;
        num_free(a);
        b = eval(n->b);
        r = num_from_long(!num_is_zero(b));
        num_free(b);
        return r;
    case N_OR:
        a = eval(n->a);
        if (!num_is_zero(a)) { num_free(a); return num_from_long(1); }
        num_free(a);
        b = eval(n->b);
        r = num_from_long(!num_is_zero(b));
        num_free(b);
        return r;
    case N_REL: {
        a = eval(n->a);
        b = eval(n->b);
        int c = num_cmp(a, b), v = 0;
        switch (n->op) {
        case '<': v = c < 0; break;
        case '>': v = c > 0; break;
        case 'l': v = c <= 0; break;
        case 'g': v = c >= 0; break;
        case '=': v = c == 0; break;
        case '!': v = c != 0; break;
        }
        num_free(a);
        num_free(b);
        return num_from_long(v);
    }
    case N_ASSIGN: {
        b = eval(n->b);
        if (n->op) {
            a = fetch(n->a);
            r = arith(n->op, a, b);
            num_free(a);
            num_free(b);
            b = r;
        }
        store(n->a, num_copy(b));
        return b;
    }
    case N_INCDEC: {
        a = fetch(n->a);
        Num *one = num_from_long(1);
        r = n->op == '+' ? num_add(a, one) : num_sub(a, one);
        num_free(one);
        Num *res = n->flag ? a : num_copy(r);
        if (!n->flag) num_free(a);
        store(n->a, r);
        return res;
    }
    case N_SCALEFN:
        a = eval(n->a);
        r = num_from_long(a->scale);
        num_free(a);
        return r;
    case N_LENGTH:
        if (n->a->type == N_ARRAYARG) {
            Arr *ar = g_avars.v[n->a->id].arr;
            return num_from_long(ar ? ar->n : 0);
        }
        a = eval(n->a);
        r = num_from_long(num_length(a));
        num_free(a);
        return r;
    case N_SQRT:
        a = eval(n->a);
        r = num_sqrt(a, g_scale);
        num_free(a);
        if (!r) rt_error("Square root of a negative number");
        return r;
    case N_READ: {
        char *line = NULL;
        size_t cap = 0;
        bc_flush();
        ssize_t len = getline(&line, &cap, stdin);
        if (len < 0) { free(line); return num_new(); }
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        else if (*p == '+') p++;
        const char *end;
        r = num_parse(p, g_ibase, &end);
        if (neg && r->n) r->neg = 1;
        free(line);
        return r;
    }
    case N_CALL: return call_func(n);
    default:
        rt_error("bad expression");
    }
}

/* function calls: arguments are evaluated first, then the parameters and autos shadow the outer variables */
static Num *call_func(Node *call) {
    Func *f = &g_funcs[call->id];
    if (!f->defined) rt_error("Function %s not defined.", f->name);
    int nargs = 0;
    for (Node *a = call->a; a; a = a->next) nargs++;
    if (nargs != f->nparams) rt_error("Parameter number mismatch");
    char probe;
    if (!g_stack_top) g_stack_top = &probe;
    if (g_stack_top - &probe > 6 * 1024 * 1024) rt_error("Function call nesting too deep");
    Num **vals = calloc((size_t)(nargs ? nargs : 1), sizeof(Num *));
    Arr **arrs = calloc((size_t)(nargs ? nargs : 1), sizeof(Arr *));
    if (!vals || !arrs) kb_die("out of memory");
    int i = 0;
    for (Node *a = call->a; a; a = a->next, i++) {
        if (f->parr[i]) {
            if (a->type == N_ARRAYARG) arrs[i] = arr_copy(g_avars.v[a->id].arr);
            else rt_error("Parameter type mismatch");
        } else {
            if (a->type == N_ARRAYARG) rt_error("Parameter type mismatch");
            vals[i] = eval(a);
        }
    }
    int depth = g_nsaves;
    for (i = 0; i < f->nparams; i++) {
        push_save(f->parr[i], f->pid[i]);
        if (f->parr[i]) g_avars.v[f->pid[i]].arr = arrs[i];
        else g_svars.v[f->pid[i]].val = vals[i];
    }
    for (i = 0; i < f->nautos; i++) push_save(f->aarr[i], f->aid[i]);
    free(vals);
    free(arrs);
    Func *saved_func = g_curfunc;
    g_curfunc = f;
    num_free(g_retval);
    g_retval = NULL;
    int st = exec_stmt(f->body);
    (void)st;
    g_curfunc = saved_func;
    Num *r = g_retval ? g_retval : num_new();
    g_retval = NULL;
    unwind(depth);
    return r;
}

static void print_num(Num *x) {
    char *s = num_to_string(x, g_obase);
    for (const char *p = s; *p; p++) put_wrapped((unsigned char)*p);
    free(s);
}

static void print_string(const char *s, int escapes) {
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (escapes && c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'a': c = 7; break;
            case 'b': c = 8; break;
            case 'e': c = 27; break;
            case 'f': c = 12; break;
            case 'n': c = '\n'; break;
            case 'q': c = '"'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            default: continue;
            }
        }
        put(c);
    }
}

enum { X_NORMAL, X_BREAK, X_CONTINUE, X_RETURN };

static int exec_stmt(Node *n) {
    int st;
    switch (n->type) {
    case S_NOP: return X_NORMAL;
    case S_EXPR: {
        Num *v = eval(n->a);
        int silent = n->flag;
        if (n->a->type == N_CALL && g_funcs[n->a->id].is_void) silent = 1;
        if (!silent) {
            print_num(v);
            put('\n');
            num_free(g_last);
            g_last = num_copy(v);
        }
        num_free(v);
        return X_NORMAL;
    }
    case S_STR:
        print_string(n->str, 0);
        return X_NORMAL;
    case S_PRINT:
        for (Node *e = n->a; e; e = e->next) {
            if (e->type == S_STR) print_string(e->str, 1);
            else {
                Num *v = eval(e);
                print_num(v);
                num_free(v);
            }
        }
        return X_NORMAL;
    case S_BLOCK:
        for (Node *s = n->a; s; s = s->next) {
            st = exec_stmt(s);
            if (st != X_NORMAL) return st;
        }
        return X_NORMAL;
    case S_IF: {
        Num *c = eval(n->a);
        int t = !num_is_zero(c);
        num_free(c);
        if (t) return exec_stmt(n->b);
        if (n->c) return exec_stmt(n->c);
        return X_NORMAL;
    }
    case S_WHILE:
        for (;;) {
            Num *c = eval(n->a);
            int t = !num_is_zero(c);
            num_free(c);
            if (!t) break;
            st = exec_stmt(n->b);
            if (st == X_BREAK) break;
            if (st == X_RETURN) return st;
        }
        return X_NORMAL;
    case S_FOR:
        if (n->a) num_free(eval(n->a));
        for (;;) {
            if (n->b) {
                Num *c = eval(n->b);
                int t = !num_is_zero(c);
                num_free(c);
                if (!t) break;
            }
            st = exec_stmt(n->d);
            if (st == X_BREAK) break;
            if (st == X_RETURN) return st;
            if (n->c) num_free(eval(n->c));
        }
        return X_NORMAL;
    case S_BREAK: return X_BREAK;
    case S_CONTINUE: return X_CONTINUE;
    case S_RETURN:
        num_free(g_retval);
        g_retval = n->a ? eval(n->a) : num_new();
        return X_RETURN;
    case S_HALT:
        bc_exit(0);
    }
    return X_NORMAL;
}

/* ------------------------------------------------------------------ driver */

static int g_interactive;

/* Runs every statement of the current source. */
static void run_source(void) {
    g_tok_valid = 0;
    for (;;) {
        Node *s = NULL;
        if (setjmp(g_parse_jmp)) {
            /* syntax error: drop the rest of the line (unless the newline was the offending token) */
            int c = g_tok_valid && g_tok == T_NL ? '\n' : 0;
            g_tok_valid = 0;
            while (c != '\n' && c != EOF) c = rd();
            continue;
        }
        int t = peek();
        if (t == T_EOF) break;
        if (t == T_NL || t == ';') { adv(); continue; }
        if (t == T_DEFINE) {
            parse_define();
            continue;
        }
        s = parse_stmt();
        if (!s) continue;
        if (setjmp(g_rt_jmp) == 0) {
            g_curfunc = NULL;
            exec_stmt(s);
        } else {
            unwind(0);
            g_curfunc = NULL;
            g_depth = 0;
        }
        node_free(s);
        if (g_interactive) bc_flush();
    }
}

static void run_text(const char *text, const char *name) {
    Src saved = g_src;
    int tv = g_tok_valid;
    g_src.f = NULL;
    g_src.text = text;
    g_src.tpos = 0;
    g_src.name = name;
    g_src.line = 1;
    run_source();
    g_src = saved;
    g_tok_valid = tv;
}

static int run_file(const char *path) {
    FILE *f;
    int is_stdin = !strcmp(path, "-");
    f = is_stdin ? stdin : fopen(path, "r");
    if (!f) {
        err("bc: cannot open file %s: %s", path, strerror(errno));
        return -1;
    }
    g_src.f = f;
    g_src.text = NULL;
    g_src.name = is_stdin ? "(standard_in)" : path;
    g_src.line = 1;
    run_source();
    if (!is_stdin) fclose(f);
    return 0;
}

extern const char bc_mathlib_source[];

APPLET(bc) {
    static const struct option longopts[] = {
        { "mathlib", no_argument, NULL, 'l' }, { "quiet", no_argument, NULL, 'q' }, { "standard", no_argument, NULL, 's' },
        { "warn", no_argument, NULL, 'w' }, { "version", no_argument, NULL, 'v' }, { "help", no_argument, NULL, 'h' },
        { "expression", required_argument, NULL, 'e' }, { "file", required_argument, NULL, 'f' },
        { "interactive", no_argument, NULL, 'i' }, { "no-prompt", no_argument, NULL, 'P' }, { "no-read-prompt", no_argument, NULL, 'R' },
        { NULL, 0, NULL, 0 }
    };
    /* work items in command line order: -e expressions and files */
    struct { int is_expr; const char *arg; } *items = calloc((size_t)argc + 1, sizeof *items);
    int nitems = 0, c;
    if (!items) kb_die("out of memory");
    optind = 0;
    while ((c = getopt_long(argc, argv, "lqswvhie:f:PR", longopts, NULL)) != -1) {
        switch (c) {
        case 'l': g_mathlib = 1; break;
        case 'q': g_quiet = 1; break;
        case 's': case 'w': case 'i': case 'P': case 'R': break;
        case 'v':
            puts("bc 1.07.1 (Nitori's kappabox)");
            return 0;
        case 'h':
            puts("usage: bc [-lqsw] [-e EXPRESSION] [-f FILE] [FILE...]");
            return 0;
        case 'e': items[nitems].is_expr = 1; items[nitems++].arg = optarg; break;
        case 'f': items[nitems].is_expr = 0; items[nitems++].arg = optarg; break;
        default:
            fputs("usage: bc [-lqsw] [-e EXPRESSION] [-f FILE] [FILE...]\n", stderr);
            return 1;
        }
    }
    for (int i = optind; i < argc; i++) { items[nitems].is_expr = 0; items[nitems++].arg = argv[i]; }
    const char *ll = getenv("BC_LINE_LENGTH");
    if (ll) {
        int v = atoi(ll);
        g_linelen = v < 0 ? 70 : v;
    }
    g_interactive = isatty(0) && isatty(1);
    if (!g_quiet && g_interactive && nitems == 0)
        puts("bc 1.07.1 (Nitori's kappabox)\nThis is free software with ABSOLUTELY NO WARRANTY.\nFor details type `warranty'.");
    g_scale = 0;
    g_last = num_new();
    if (g_mathlib) {
        g_scale = 20;
        run_text(bc_mathlib_source, "(math library)");
        g_scale = 20;
    }
    for (int i = 0; i < nitems; i++) {
        if (items[i].is_expr) {
            char *buf = malloc(strlen(items[i].arg) + 2);
            if (!buf) kb_die("out of memory");
            sprintf(buf, "%s\n", items[i].arg);
            run_text(buf, "(standard_in)");
            free(buf);
        } else if (run_file(items[i].arg) < 0) bc_exit(1);
    }
    if (nitems == 0) run_file("-");
    free(items);
    bc_exit(0);
    return 0;
}

/* The math library: s c a l e j, written in bc.  Each function works with guard digits and truncates at the end. */
const char bc_mathlib_source[] =
"define e(x) {\n"
"  auto a, d, e, f, i, n, v, w, z\n"
"  z = scale\n"
"  if (x < 0) { n = 1; x = -x }\n"
"  w = z + 8 + .44 * x\n"
"  scale = w\n"
"  while (x > 1) { f += 1; x /= 2 }\n"
"  scale = w + f / 3 + 2\n"
"  v = 1 + x; a = x; d = 1\n"
"  for (i = 2; 1; i++) {\n"
"    a *= x; d *= i\n"
"    e = a / d\n"
"    if (e == 0) break\n"
"    v += e\n"
"  }\n"
"  while (f--) v *= v\n"
"  scale = z\n"
"  if (n) return (1 / v)\n"
"  return (v / 1)\n"
"}\n"
"define a(x) {\n"
"  auto f, i, m, n, t, v, w, z\n"
"  z = scale\n"
"  w = z + 8\n"
"  scale = w\n"
"  if (x < 0) { n = 1; x = -x }\n"
"  f = 1\n"
"  while (x > .1) { x = x / (1 + sqrt(1 + x * x)); f *= 2 }\n"
"  v = x; m = x; t = x * x\n"
"  for (i = 3; 1; i += 2) {\n"
"    m = -m * t\n"
"    if (m / i == 0) break\n"
"    v += m / i\n"
"  }\n"
"  v *= f\n"
"  scale = z\n"
"  if (n) return (-v / 1)\n"
"  return (v / 1)\n"
"}\n"
"define s(x) {\n"
"  auto e, i, m, n, p, q, v, w, z\n"
"  z = scale\n"
"  w = z + 8 + length(x)\n"
"  scale = w\n"
"  p = 4 * a(1)\n"
"  q = x / (2 * p)\n"
"  scale = 0\n"
"  if (q < 0) n = (q - .5) / 1\n"
"  if (q >= 0) n = (q + .5) / 1\n"
"  scale = w\n"
"  x = x - n * 2 * p\n"
"  v = x; e = x; m = x * x\n"
"  for (i = 3; 1; i += 2) {\n"
"    e = -e * m / (i * (i - 1))\n"
"    if (e == 0) break\n"
"    v += e\n"
"  }\n"
"  scale = z\n"
"  return (v / 1)\n"
"}\n"
"define c(x) {\n"
"  auto v, z\n"
"  z = scale\n"
"  scale = z + 8 + length(x)\n"
"  v = s(x + 2 * a(1))\n"
"  scale = z\n"
"  return (v / 1)\n"
"}\n"
"define l(x) {\n"
"  auto e, f, i, m, n, s, t, v, w, z\n"
"  if (x <= 0) { return ((1 - 10 ^ scale) / 1) }\n"
"  z = scale\n"
"  w = z + 8 + length(x)\n"
"  scale = w\n"
"  while (x >= 2) { f += 1; x /= 2 }\n"
"  while (x < .5) { f -= 1; x *= 2 }\n"
"  s = (x - 1) / (x + 1); t = s * s; v = s; e = s\n"
"  for (i = 3; 1; i += 2) {\n"
"    e *= t\n"
"    m = e / i\n"
"    if (m == 0) break\n"
"    v += m\n"
"  }\n"
"  v *= 2\n"
"  if (f != 0) {\n"
"    s = 1 / 3; t = s * s; n = s; e = s\n"
"    for (i = 3; 1; i += 2) {\n"
"      e *= t\n"
"      m = e / i\n"
"      if (m == 0) break\n"
"      n += m\n"
"    }\n"
"    v += f * 2 * n\n"
"  }\n"
"  scale = z\n"
"  return (v / 1)\n"
"}\n"
"define j(n, x) {\n"
"  auto d, e, i, k, q, s, t, v, w, z\n"
"  z = scale\n"
"  scale = 0\n"
"  n = n / 1\n"
"  s = 1\n"
"  if (n < 0) { n = -n; if (n % 2) s = -1 }\n"
"  k = x\n"
"  if (k < 0) k = -k\n"
"  k = k / 1\n"
"  scale = z + 10 + n + k\n"
"  t = x / 2; q = t * t\n"
"  d = 1\n"
"  for (i = 2; i <= n; i++) d *= i\n"
"  v = t ^ n / d; e = v\n"
"  for (i = 1; 1; i++) {\n"
"    e = -e * q / (i * (i + n))\n"
"    if (e == 0) break\n"
"    v += e\n"
"  }\n"
"  scale = z\n"
"  return (s * v / 1)\n"
"}\n";
