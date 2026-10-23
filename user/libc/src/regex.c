/* POSIX-style regular expressions (BRE and ERE, plus a few GNU escapes) compiled to a backtracking VM. */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_BOL, OP_EOL, OP_WORDB, OP_NWORDB, OP_WORDSTART, OP_WORDEND,
       OP_BACKREF, OP_MATCH };

typedef struct { int op, x, y; } Inst;

typedef struct {
    Inst *code;
    int n, cap;
    unsigned char (*classes)[32];
    int nclasses;
    int ngroups;
    int icase, newline;
} Prog;

/* ------------------------------------------------------------------ parser -> AST */

typedef enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WSTART, N_WEND, N_GROUP, N_CAT, N_ALT, N_REP, N_BACKREF, N_EMPTY } NKind;

typedef struct Node {
    NKind kind;
    int c;                       /* char, class index, group number, backref number */
    int min, max;                /* N_REP: max = -1 for unbounded */
    struct Node *a, *b;          /* children (CAT/ALT: a then b; REP/GROUP: a) */
} Node;

typedef struct {
    const char *p;
    int ere;
    int icase;
    Prog *prog;
    int ngroups;
    int error;
} Parser;

static Node *mk(NKind k, Node *a, Node *b) {
    Node *n = calloc(1, sizeof *n);
    n->kind = k;
    n->a = a;
    n->b = b;
    return n;
}

static void free_node(Node *n) {
    if (!n) return;
    free_node(n->a);
    free_node(n->b);
    free(n);
}

static int add_class(Prog *pr, const unsigned char *bits) {
    pr->classes = realloc(pr->classes, (size_t)(pr->nclasses + 1) * 32);
    memcpy(pr->classes[pr->nclasses], bits, 32);
    return pr->nclasses++;
}

static void set_bit(unsigned char *b, int c) { b[c >> 3] |= (unsigned char)(1 << (c & 7)); }

static int class_from_name(const char *name, size_t n, unsigned char *bits) {
    static const struct { const char *name; int (*fn)(int); } tab[] = {
        { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "upper", isupper }, { "lower", islower },
        { "space", isspace }, { "blank", isblank }, { "punct", ispunct }, { "print", isprint }, { "graph", isgraph },
        { "cntrl", iscntrl }, { "xdigit", isxdigit },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strlen(tab[i].name) == n && !strncmp(tab[i].name, name, n)) {
            for (int c = 0; c < 256; c++) if (tab[i].fn(c)) set_bit(bits, c);
            return 1;
        }
    }
    return 0;
}

static Node *parse_bracket(Parser *ps) {
    unsigned char bits[32];
    memset(bits, 0, sizeof bits);
    int neg = 0, first = 1;
    if (*ps->p == '^') { neg = 1; ps->p++; }
    while (*ps->p && (*ps->p != ']' || first)) {
        first = 0;
        if (ps->p[0] == '[' && ps->p[1] == ':') {
            const char *e = strstr(ps->p + 2, ":]");
            if (e && class_from_name(ps->p + 2, (size_t)(e - ps->p - 2), bits)) { ps->p = e + 2; continue; }
        }
        int lo = (unsigned char)*ps->p++;
        if (lo == '\\' && *ps->p && !ps->ere) { /* in BRE brackets, backslash is literal */ }
        if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
            int hi = (unsigned char)ps->p[1];
            ps->p += 2;
            for (int c = lo; c <= hi; c++) { set_bit(bits, c); if (ps->icase) { set_bit(bits, tolower(c)); set_bit(bits, toupper(c)); } }
        } else {
            set_bit(bits, lo);
            if (ps->icase) { set_bit(bits, tolower(lo)); set_bit(bits, toupper(lo)); }
        }
    }
    if (*ps->p != ']') { ps->error = REG_BADPAT; return mk(N_EMPTY, NULL, NULL); }
    ps->p++;
    if (neg) for (int i = 0; i < 32; i++) bits[i] = (unsigned char)~bits[i];
    Node *n = mk(N_CLASS, NULL, NULL);
    n->c = add_class(ps->prog, bits);
    return n;
}

static Node *parse_alt(Parser *ps, int depth);

static Node *class_node(Parser *ps, int (*fn)(int), int negate) {
    unsigned char bits[32];
    memset(bits, 0, sizeof bits);
    for (int c = 0; c < 256; c++) if ((fn(c) != 0) != negate) set_bit(bits, c);
    Node *n = mk(N_CLASS, NULL, NULL);
    n->c = add_class(ps->prog, bits);
    return n;
}

static int is_word(int c) { return isalnum(c) || c == '_'; }

static Node *parse_atom(Parser *ps, int depth, int at_start) {
    char c = *ps->p;
    if (!c) return NULL;
    if (ps->ere ? c == '(' : (c == '\\' && ps->p[1] == '(')) {
        ps->p += ps->ere ? 1 : 2;
        int idx = ++ps->ngroups;
        Node *inner = parse_alt(ps, depth + 1);
        if (ps->ere ? *ps->p == ')' : (ps->p[0] == '\\' && ps->p[1] == ')')) ps->p += ps->ere ? 1 : 2;
        else ps->error = REG_BADPAT;
        Node *g = mk(N_GROUP, inner, NULL);
        g->c = idx;
        return g;
    }
    if (c == '[') { ps->p++; return parse_bracket(ps); }
    if (c == '.') { ps->p++; return mk(N_ANY, NULL, NULL); }
    if (c == '^' && (ps->ere || at_start)) { ps->p++; return mk(N_BOL, NULL, NULL); }
    if (c == '$' && (ps->ere || !ps->p[1] || (ps->p[1] == '\\' && (ps->p[2] == ')' || ps->p[2] == '|')))) { ps->p++; return mk(N_EOL, NULL, NULL); }
    if (c == '*' && at_start && !ps->ere) { ps->p++; Node *n = mk(N_CHAR, NULL, NULL); n->c = '*'; return n; }
    if (c == '\\') {
        char e = ps->p[1];
        if (!e) { ps->error = REG_BADPAT; return NULL; }
        ps->p += 2;
        Node *n;
        switch (e) {
        case 'n': n = mk(N_CHAR, NULL, NULL); n->c = '\n'; return n;
        case 't': n = mk(N_CHAR, NULL, NULL); n->c = '\t'; return n;
        case 'd': return class_node(ps, isdigit, 0);
        case 'D': return class_node(ps, isdigit, 1);
        case 'w': return class_node(ps, is_word, 0);
        case 'W': return class_node(ps, is_word, 1);
        case 's': return class_node(ps, isspace, 0);
        case 'S': return class_node(ps, isspace, 1);
        case 'b': return mk(N_WORDB, NULL, NULL);
        case 'B': return mk(N_NWORDB, NULL, NULL);
        case '<': return mk(N_WSTART, NULL, NULL);
        case '>': return mk(N_WEND, NULL, NULL);
        default:
            if (e >= '1' && e <= '9') { n = mk(N_BACKREF, NULL, NULL); n->c = e - '0'; return n; }
            n = mk(N_CHAR, NULL, NULL);
            n->c = (unsigned char)e;
            return n;
        }
    }
    ps->p++;
    Node *n = mk(N_CHAR, NULL, NULL);
    n->c = (unsigned char)c;
    return n;
}

static int parse_interval(Parser *ps, int *mn, int *mx) {
    const char *p = ps->p;
    int a = 0, b = -1, have = 0;
    while (isdigit((unsigned char)*p)) { a = a * 10 + (*p++ - '0'); have = 1; }
    if (!have) return 0;
    b = a;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) { b = 0; while (isdigit((unsigned char)*p)) b = b * 10 + (*p++ - '0'); }
        else b = -1;
    }
    if (ps->ere) { if (*p != '}') return 0; p++; }
    else { if (p[0] != '\\' || p[1] != '}') return 0; p += 2; }
    ps->p = p;
    *mn = a;
    *mx = b;
    return 1;
}

static Node *parse_repeat(Parser *ps, int depth, int at_start) {
    Node *atom = parse_atom(ps, depth, at_start);
    if (!atom) return NULL;
    for (;;) {
        char c = *ps->p;
        int mn, mx;
        if (c == '*') { ps->p++; mn = 0; mx = -1; }
        else if (ps->ere && c == '+') { ps->p++; mn = 1; mx = -1; }
        else if (ps->ere && c == '?') { ps->p++; mn = 0; mx = 1; }
        else if (!ps->ere && c == '\\' && ps->p[1] == '+') { ps->p += 2; mn = 1; mx = -1; }
        else if (!ps->ere && c == '\\' && ps->p[1] == '?') { ps->p += 2; mn = 0; mx = 1; }
        else if (ps->ere && c == '{') { ps->p++; if (!parse_interval(ps, &mn, &mx)) { ps->p--; break; } }
        else if (!ps->ere && c == '\\' && ps->p[1] == '{') { ps->p += 2; if (!parse_interval(ps, &mn, &mx)) { ps->error = REG_BADPAT; break; } }
        else break;
        Node *r = mk(N_REP, atom, NULL);
        r->min = mn;
        r->max = mx;
        atom = r;
    }
    return atom;
}

static int at_alt_end(Parser *ps, int depth) {
    if (!*ps->p) return 1;
    if (ps->ere) return *ps->p == '|' || (*ps->p == ')' && depth > 0);
    return (ps->p[0] == '\\' && ps->p[1] == '|') || (ps->p[0] == '\\' && ps->p[1] == ')' && depth > 0);
}

static Node *parse_cat(Parser *ps, int depth) {
    Node *head = NULL;
    int first = 1;
    while (!at_alt_end(ps, depth) && !ps->error) {
        Node *r = parse_repeat(ps, depth, first);
        first = 0;
        if (!r) break;
        head = head ? mk(N_CAT, head, r) : r;
    }
    return head ? head : mk(N_EMPTY, NULL, NULL);
}

static Node *parse_alt(Parser *ps, int depth) {
    Node *left = parse_cat(ps, depth);
    for (;;) {
        if (ps->ere ? *ps->p == '|' : (ps->p[0] == '\\' && ps->p[1] == '|')) {
            ps->p += ps->ere ? 1 : 2;
            Node *right = parse_cat(ps, depth);
            left = mk(N_ALT, left, right);
        } else break;
    }
    return left;
}

/* ------------------------------------------------------------------ AST -> program */

static int emit(Prog *p, int op, int x, int y) {
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->code = realloc(p->code, (size_t)p->cap * sizeof(Inst)); }
    p->code[p->n] = (Inst){ op, x, y };
    return p->n++;
}

static void gen(Prog *p, Node *n) {
    switch (n->kind) {
    case N_EMPTY: break;
    case N_CHAR: emit(p, OP_CHAR, n->c, 0); break;
    case N_ANY: emit(p, OP_ANY, 0, 0); break;
    case N_CLASS: emit(p, OP_CLASS, n->c, 0); break;
    case N_BOL: emit(p, OP_BOL, 0, 0); break;
    case N_EOL: emit(p, OP_EOL, 0, 0); break;
    case N_WORDB: emit(p, OP_WORDB, 0, 0); break;
    case N_NWORDB: emit(p, OP_NWORDB, 0, 0); break;
    case N_WSTART: emit(p, OP_WORDSTART, 0, 0); break;
    case N_WEND: emit(p, OP_WORDEND, 0, 0); break;
    case N_BACKREF: emit(p, OP_BACKREF, n->c, 0); break;
    case N_GROUP:
        emit(p, OP_SAVE, n->c * 2, 0);
        gen(p, n->a);
        emit(p, OP_SAVE, n->c * 2 + 1, 0);
        break;
    case N_CAT: gen(p, n->a); gen(p, n->b); break;
    case N_ALT: {
        int split = emit(p, OP_SPLIT, 0, 0);
        p->code[split].x = p->n;
        gen(p, n->a);
        int jmp = emit(p, OP_JMP, 0, 0);
        p->code[split].y = p->n;
        gen(p, n->b);
        p->code[jmp].x = p->n;
        break;
    }
    case N_REP: {
        for (int i = 0; i < n->min; i++) gen(p, n->a);
        if (n->max < 0) {                                   /* x* */
            int split = emit(p, OP_SPLIT, 0, 0);
            p->code[split].x = p->n;
            gen(p, n->a);
            emit(p, OP_JMP, split, 0);
            p->code[split].y = p->n;
        } else {
            int splits[64], ns = 0;
            for (int i = n->min; i < n->max && ns < 64; i++) {
                splits[ns] = emit(p, OP_SPLIT, 0, 0);
                p->code[splits[ns]].x = p->n;
                gen(p, n->a);
                ns++;
            }
            for (int i = 0; i < ns; i++) p->code[splits[i]].y = p->n;
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ matcher */

typedef struct {
    const Prog *p;
    const char *start, *end;
    regoff_t caps[20];
    int ncaps;
    int notbol;
    long steps;
} Run;

static int word_at(const Run *r, const char *s, int before) {
    if (before) return s > r->start && is_word((unsigned char)s[-1]);
    return s < r->end && is_word((unsigned char)*s);
}

static int run(Run *r, int pc, const char *s) {
    const Prog *p = r->p;
    for (;;) {
        if (++r->steps > 20000000) return 0;
        const Inst *in = &p->code[pc];
        switch (in->op) {
        case OP_CHAR:
            if (s >= r->end) return 0;
            if (p->icase ? tolower((unsigned char)*s) != tolower(in->x) : (unsigned char)*s != in->x) return 0;
            s++; pc++;
            break;
        case OP_ANY:
            if (s >= r->end || (p->newline && *s == '\n')) return 0;
            s++; pc++;
            break;
        case OP_CLASS:
            if (s >= r->end) return 0;
            if (!(p->classes[in->x][(unsigned char)*s >> 3] & (1 << ((unsigned char)*s & 7)))) return 0;
            s++; pc++;
            break;
        case OP_BOL:
            if (!((s == r->start && !r->notbol) || (p->newline && s > r->start && s[-1] == '\n'))) return 0;
            pc++;
            break;
        case OP_EOL:
            if (!(s == r->end || (p->newline && *s == '\n'))) return 0;
            pc++;
            break;
        case OP_WORDB: if (word_at(r, s, 1) == word_at(r, s, 0)) return 0; pc++; break;
        case OP_NWORDB: if (word_at(r, s, 1) != word_at(r, s, 0)) return 0; pc++; break;
        case OP_WORDSTART: if (word_at(r, s, 1) || !word_at(r, s, 0)) return 0; pc++; break;
        case OP_WORDEND: if (!word_at(r, s, 1) || word_at(r, s, 0)) return 0; pc++; break;
        case OP_BACKREF: {
            int g = in->x;
            if (g * 2 + 1 >= r->ncaps || r->caps[g * 2] < 0 || r->caps[g * 2 + 1] < 0) return 0;
            long len = r->caps[g * 2 + 1] - r->caps[g * 2];
            if (s + len > r->end || strncmp(r->start + r->caps[g * 2], s, (size_t)len)) return 0;
            s += len; pc++;
            break;
        }
        case OP_JMP: pc = in->x; break;
        case OP_SPLIT:
            if (run(r, in->x, s)) return 1;
            pc = in->y;
            break;
        case OP_SAVE: {
            regoff_t old = in->x < r->ncaps ? r->caps[in->x] : 0;
            if (in->x < r->ncaps) r->caps[in->x] = s - r->start;
            if (run(r, pc + 1, s)) return 1;
            if (in->x < r->ncaps) r->caps[in->x] = old;
            return 0;
        }
        case OP_MATCH:
            r->caps[1] = s - r->start;
            return 1;
        }
    }
}

/* ------------------------------------------------------------------ public API */

int regcomp(regex_t *re, const char *pattern, int flags) {
    Prog *pr = calloc(1, sizeof *pr);
    pr->icase = flags & REG_ICASE;
    pr->newline = flags & REG_NEWLINE;
    Parser ps = { pattern, (flags & REG_EXTENDED) != 0, pr->icase, pr, 0, 0 };
    Node *ast = parse_alt(&ps, 0);
    if (!ps.error && *ps.p) ps.error = REG_BADPAT;                  /* stray ')' */
    if (ps.error) { free_node(ast); free(pr->classes); free(pr->code); free(pr); return ps.error; }
    emit(pr, OP_SAVE, 0, 0);
    gen(pr, ast);
    emit(pr, OP_MATCH, 0, 0);
    pr->ngroups = ps.ngroups;
    free_node(ast);
    re->prog = pr;
    re->re_nsub = (size_t)ps.ngroups;
    re->flags = flags;
    return 0;
}

int regexec(const regex_t *re, const char *str, size_t nmatch, regmatch_t pmatch[], int eflags) {
    const Prog *p = re->prog;
    Run r;
    memset(&r, 0, sizeof r);
    r.p = p;
    r.start = str;
    r.end = str + strlen(str);
    r.ncaps = (p->ngroups + 1) * 2;
    if (r.ncaps > 20) r.ncaps = 20;
    r.notbol = eflags & REG_NOTBOL;
    for (const char *s = str; s <= r.end; s++) {
        for (int i = 0; i < r.ncaps; i++) r.caps[i] = -1;
        r.steps = 0;
        if (run(&r, 0, s)) {
            r.caps[0] = s - str;
            for (size_t i = 0; i < nmatch; i++) {
                if ((int)(i * 2 + 1) < r.ncaps) { pmatch[i].rm_so = r.caps[i * 2]; pmatch[i].rm_eo = r.caps[i * 2 + 1]; }
                else { pmatch[i].rm_so = pmatch[i].rm_eo = -1; }
            }
            return 0;
        }
    }
    return REG_NOMATCH;
}

void regfree(regex_t *re) {
    Prog *p = re->prog;
    if (!p) return;
    free(p->code);
    free(p->classes);
    free(p);
    re->prog = NULL;
}

size_t regerror(int code, const regex_t *re, char *buf, size_t n) {
    (void)re;
    const char *m = code == REG_NOMATCH ? "no match" : code == REG_BADPAT ? "invalid regular expression" : "regex error";
    if (n) { strncpy(buf, m, n - 1); buf[n - 1] = 0; }
    return strlen(m) + 1;
}
