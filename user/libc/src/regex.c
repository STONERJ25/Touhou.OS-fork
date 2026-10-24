/* POSIX regular expressions (BRE and ERE, plus a few GNU escapes) compiled to a backtracking VM.
 *
 * regexec returns the leftmost match and, among the matches that start there, the longest one (POSIX). The VM tries alternatives
 * in priority order (left alternative first, greedy loops first) on an explicit stack in the heap, so `.*` over a megabyte costs
 * no C stack, and it does not stop at the first match: it keeps exploring and remembers the longest end. Among matches of equal
 * length the one a plain backtracking search finds first supplies the submatches (what Go's POSIX mode does as well).
 *
 * Without back-references the future of a state depends only on (instruction, position), so every join state is visited at most
 * once per call (a bitmap indexed by position); that bounds the exhaustive search by pattern size times text length, and the
 * visited states stay valid for the following start positions, because a start that fails visited only dead states. Loops over a
 * single byte (`.*`, `[a-z]+`, `x{2,5}`) are one instruction that scans, with the shorter alternatives kept as one stack frame.
 * With back-references the search is exhaustive but capped by a step count; if the text is so long that the bitmap would pass
 * 16 MB, the search falls back to the first match found (leftmost-first, the old behaviour).
 *
 * Without REG_STARTEND the string is never measured: the matcher stops at the NUL itself, so a loop that calls regexec on the
 * rest of a long line again and again (sed's s///g) does not pay for strlen each time. A pattern that starts with a literal, or
 * with a small set of bytes, jumps from candidate to candidate with memmem/memchr instead of trying every position. */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_SAVE, OP_BOL, OP_EOL, OP_WORDB, OP_NWORDB, OP_WORDSTART, OP_WORDEND,
       OP_BACKREF, OP_MATCH, OP_REP1, OP_LOOPINIT, OP_MARK, OP_PROGRESS };
enum { RK_ANY, RK_CHAR, RK_CLASS };             /* what an OP_REP1 loop repeats */

#define MAX_PROG      200000                    /* instructions: stops ((a{100}){100}){100} from eating the memory */
#define MAX_NEST      100                       /* nested parentheses */
#define MAX_STACKED   16                        /* operators stacked on one atom: a*+?... */
#define MAX_DUP       32767                     /* biggest count in {n,m} */
#ifndef MAX_FRAMES
#define MAX_FRAMES    (1L << 22)                /* backtrack stack: 4M frames of 24 bytes */
#endif
#ifndef MEMO_MAX_BITS
#define MEMO_MAX_BITS (1L << 27)                /* visited bitmap: 16 MB */
#endif
#define STEP_LIMIT    20000000L                 /* per start position, when states cannot be remembered */

/* inst: x, y, z, w mean
 *   OP_CHAR x y        byte x, or byte y (the other case with REG_ICASE)
 *   OP_CLASS x         class number
 *   OP_SPLIT x y       try x first, y when that fails
 *   OP_JMP x
 *   OP_SAVE x          register x gets the position (2 per group)
 *   OP_BACKREF x       group x
 *   OP_REP1 x y z w    greedy x..y (y < 0: any number) repeats of one byte: z = RK_*, w = byte or class number
 *   OP_LOOPINIT x y    a loop whose body can match the empty string starts: register x = "fresh" (y: an empty first iteration is allowed)
 *   OP_MARK x          an iteration starts: register x = position * 2 + (1 if it is the first iteration and may be empty)
 *   OP_PROGRESS x y    an iteration ended: if it consumed nothing, it counts only when it is the first one, and then the
 *                      loop is left (go to y); an empty iteration after a real one fails, as in glibc and POSIX ("(a?b?)*"
 *                      on "b" has \1 = "b", not "b" and then an empty "") */
typedef struct { int op, x, y, z, w; } Inst;

typedef struct {
    Inst *code;
    int n, cap;
    unsigned char (*classes)[32];
    int nclasses;
    int ngroups;
    int nregs;                  /* 2 per group (group 0 included), then one per empty-body loop */
    int icase, newline;
    int has_backref;
    int overflow;               /* out of memory or program too big while compiling */
    int njoin, nrep;            /* instructions that can be reached from two places; unbounded one-byte loops */
    int *jidx, *ridx;           /* per instruction: its number among the joins / the loops, or -1 */
    int anchored;               /* every match starts at the beginning of the string (^ without REG_NEWLINE) */
    int first_known;            /* the pattern cannot match the empty string, so a match starts with a byte of first[] */
    unsigned char first[32];
    int first_byte;             /* the only byte of first[], or -1 */
    char *prefix;               /* literal text every match starts with (without REG_ICASE) */
    int prefix_len;
} Prog;

/* ------------------------------------------------------------------ parser -> AST */

typedef enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WSTART, N_WEND, N_GROUP, N_CAT, N_ALT, N_REP, N_BACKREF, N_EMPTY } NKind;

/* N_CAT and N_ALT are lists: a node is one cell, `a` the item and `b` the next cell (NULL at the end), so long patterns are
 * walked in loops and not by recursion. */
typedef struct Node {
    NKind kind;
    int c;                       /* char, class index, group number, backref number */
    int min, max;                /* N_REP: max = -1 for unbounded */
    struct Node *a, *b;
} Node;

#define CHUNK_NODES 64
typedef struct Chunk { struct Chunk *next; int used; Node nodes[CHUNK_NODES]; } Chunk;

typedef struct {
    const char *p;
    int ere;
    int icase;
    Prog *prog;
    int ngroups;
    int has_backref;
    int error;
    Chunk *chunks;
    Node dummy;                  /* handed out when memory runs out, so the parser can finish without crashing */
} Parser;

static Node *mk(Parser *ps, NKind k, Node *a, Node *b) {
    Chunk *c = ps->chunks;
    if (!c || c->used == CHUNK_NODES) {
        c = malloc(sizeof *c);
        if (!c) {
            ps->error = REG_ESPACE;
            memset(&ps->dummy, 0, sizeof ps->dummy);
            ps->dummy.kind = N_EMPTY;
            return &ps->dummy;
        }
        c->next = ps->chunks;
        c->used = 0;
        ps->chunks = c;
    }
    Node *n = &c->nodes[c->used++];
    n->kind = k;
    n->c = n->min = n->max = 0;
    n->a = a;
    n->b = b;
    return n;
}

static void free_chunks(Chunk *c) {
    while (c) { Chunk *next = c->next; free(c); c = next; }
}

static int add_class(Prog *pr, const unsigned char *bits) {
    unsigned char (*nc)[32] = realloc(pr->classes, (size_t)(pr->nclasses + 1) * 32);
    if (!nc) { pr->overflow = 1; return 0; }
    pr->classes = nc;
    memcpy(pr->classes[pr->nclasses], bits, 32);
    return pr->nclasses++;
}

static void set_bit(unsigned char *b, int c) { b[c >> 3] |= (unsigned char)(1 << (c & 7)); }

static int other_case(int c) { return isupper(c) ? tolower(c) : islower(c) ? toupper(c) : c; }

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
            if (e && class_from_name(ps->p + 2, (size_t)(e - ps->p - 2), bits)) {
                ps->p = e + 2;
                if (ps->icase) for (int c = 0; c < 256; c++) if ((bits[c >> 3] >> (c & 7)) & 1) set_bit(bits, other_case(c));    /* [[:upper:]] matches a with REG_ICASE */
                continue;
            }
        }
        if (ps->p[0] == '[' && (ps->p[1] == '=' || ps->p[1] == '.') && ps->p[2] && ps->p[3] == ps->p[1] && ps->p[4] == ']') {
            int c = (unsigned char)ps->p[2];                /* [=c=] and [.c.]: one character */
            set_bit(bits, c);
            if (ps->icase) { set_bit(bits, tolower(c)); set_bit(bits, toupper(c)); }
            ps->p += 5;
            continue;
        }
        int lo = (unsigned char)*ps->p++;
        if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
            int hi = (unsigned char)ps->p[1];
            ps->p += 2;
            for (int c = lo; c <= hi; c++) { set_bit(bits, c); if (ps->icase) { set_bit(bits, tolower(c)); set_bit(bits, toupper(c)); } }
        } else {
            set_bit(bits, lo);
            if (ps->icase) { set_bit(bits, tolower(lo)); set_bit(bits, toupper(lo)); }
        }
    }
    if (*ps->p != ']') { ps->error = REG_BADPAT; return mk(ps, N_EMPTY, NULL, NULL); }
    ps->p++;
    if (neg) {
        for (int i = 0; i < 32; i++) bits[i] = (unsigned char)~bits[i];
        if (ps->prog->newline) bits['\n' >> 3] &= (unsigned char)~(1 << ('\n' & 7));    /* [^a] does not match a newline with REG_NEWLINE */
    }
    Node *n = mk(ps, N_CLASS, NULL, NULL);
    n->c = add_class(ps->prog, bits);
    return n;
}

static Node *parse_alt(Parser *ps, int depth);

static Node *class_node(Parser *ps, int (*fn)(int), int negate) {
    unsigned char bits[32];
    memset(bits, 0, sizeof bits);
    for (int c = 0; c < 256; c++) if ((fn(c) != 0) != negate) set_bit(bits, c);
    Node *n = mk(ps, N_CLASS, NULL, NULL);
    n->c = add_class(ps->prog, bits);
    return n;
}

static int is_word(int c) { return isalnum(c) || c == '_'; }

static Node *char_node(Parser *ps, int c) {
    Node *n = mk(ps, N_CHAR, NULL, NULL);
    n->c = c;
    return n;
}

static Node *parse_atom(Parser *ps, int depth, int at_start) {
    char c = *ps->p;
    if (!c) return NULL;
    if (ps->ere ? c == '(' : (c == '\\' && ps->p[1] == '(')) {
        if (depth >= MAX_NEST) { ps->error = REG_ESPACE; return NULL; }
        ps->p += ps->ere ? 1 : 2;
        int idx = ++ps->ngroups;
        Node *inner = parse_alt(ps, depth + 1);
        if (ps->ere ? *ps->p == ')' : (ps->p[0] == '\\' && ps->p[1] == ')')) ps->p += ps->ere ? 1 : 2;
        else if (!ps->error) ps->error = REG_BADPAT;
        Node *g = mk(ps, N_GROUP, inner, NULL);
        g->c = idx;
        return g;
    }
    if (c == '[') { ps->p++; return parse_bracket(ps); }
    if (c == '.') { ps->p++; return mk(ps, N_ANY, NULL, NULL); }
    if (c == '^' && (ps->ere || at_start == 1)) { ps->p++; return mk(ps, N_BOL, NULL, NULL); }
    if (c == '$' && (ps->ere || !ps->p[1] || (ps->p[1] == '\\' && (ps->p[2] == ')' || ps->p[2] == '|')))) { ps->p++; return mk(ps, N_EOL, NULL, NULL); }
    if (c == '*' && at_start && !ps->ere) { ps->p++; return char_node(ps, '*'); }
    if (c == '\\') {
        char e = ps->p[1];
        if (!e) { ps->error = REG_BADPAT; return NULL; }
        ps->p += 2;
        Node *n;
        switch (e) {
        case 'n': return char_node(ps, '\n');
        case 't': return char_node(ps, '\t');
        case 'd': return class_node(ps, isdigit, 0);
        case 'D': return class_node(ps, isdigit, 1);
        case 'w': return class_node(ps, is_word, 0);
        case 'W': return class_node(ps, is_word, 1);
        case 's': return class_node(ps, isspace, 0);
        case 'S': return class_node(ps, isspace, 1);
        case 'b': return mk(ps, N_WORDB, NULL, NULL);
        case 'B': return mk(ps, N_NWORDB, NULL, NULL);
        case '<': return mk(ps, N_WSTART, NULL, NULL);
        case '>': return mk(ps, N_WEND, NULL, NULL);
        default:
            if (e >= '1' && e <= '9') { n = mk(ps, N_BACKREF, NULL, NULL); n->c = e - '0'; ps->has_backref = 1; return n; }
            return char_node(ps, (unsigned char)e);
        }
    }
    ps->p++;
    return char_node(ps, (unsigned char)c);
}

/* After the opening brace: m}, m,} , m,n} or ,n} (the closing brace is \} in a BRE). */
static int parse_interval(Parser *ps, int *mn, int *mx) {
    const char *p = ps->p;
    long a = 0, b;
    int have = 0;
    while (isdigit((unsigned char)*p)) { if (a <= MAX_DUP) a = a * 10 + (*p - '0'); p++; have = 1; }
    if (!have && *p != ',') return 0;
    b = a;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) { b = 0; while (isdigit((unsigned char)*p)) { if (b <= MAX_DUP) b = b * 10 + (*p - '0'); p++; } }
        else if (!have) return 0;                           /* "{,}" is not a count */
        else b = -1;
    }
    if (ps->ere) { if (*p != '}') return 0; p++; }
    else { if (p[0] != '\\' || p[1] != '}') return 0; p += 2; }
    if (a > MAX_DUP || b > MAX_DUP || (b >= 0 && b < a)) { ps->error = REG_BADPAT; return 0; }
    ps->p = p;
    *mn = (int)a;
    *mx = (int)b;
    return 1;
}

static Node *parse_repeat(Parser *ps, int depth, int at_start) {
    Node *atom = parse_atom(ps, depth, at_start);
    if (!atom) return NULL;
    if (!ps->ere && atom->kind == N_BOL) return atom;       /* a * after the leading ^ of a BRE is an ordinary character */
    int stacked = 0;
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
        if (++stacked > MAX_STACKED) { ps->error = REG_ESPACE; break; }
        Node *r = mk(ps, N_REP, atom, NULL);
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
    Node *head = NULL, *tail = NULL;
    int first = 1;                                          /* 1: the start of the expression, 2: right after its leading ^ (BRE) */
    while (!at_alt_end(ps, depth) && !ps->error) {
        Node *r = parse_repeat(ps, depth, first);
        if (!r) break;
        first = !ps->ere && first == 1 && r->kind == N_BOL ? 2 : 0;
        Node *cell = mk(ps, N_CAT, r, NULL);
        if (tail) tail->b = cell; else head = cell;
        tail = cell;
    }
    if (!head) return mk(ps, N_EMPTY, NULL, NULL);
    if (!head->b) return head->a;                            /* a single item needs no list */
    return head;
}

static Node *parse_alt(Parser *ps, int depth) {
    Node *left = parse_cat(ps, depth);
    if (!(ps->ere ? *ps->p == '|' : (ps->p[0] == '\\' && ps->p[1] == '|'))) return left;
    Node *head = mk(ps, N_ALT, left, NULL), *tail = head;
    while (ps->ere ? *ps->p == '|' : (ps->p[0] == '\\' && ps->p[1] == '|')) {
        ps->p += ps->ere ? 1 : 2;
        Node *cell = mk(ps, N_ALT, parse_cat(ps, depth), NULL);
        tail->b = cell;
        tail = cell;
    }
    return head;
}

/* ------------------------------------------------------------------ analysis of the AST */

/* Can n match the empty string? */
static int nullable(const Node *n) {
    switch (n->kind) {
    case N_CHAR: case N_ANY: case N_CLASS: return 0;
    case N_GROUP: return nullable(n->a);
    case N_CAT: for (const Node *c = n; c; c = c->b) if (!nullable(c->a)) return 0; return 1;
    case N_ALT: for (const Node *c = n; c; c = c->b) if (nullable(c->a)) return 1; return 0;
    case N_REP: return n->min == 0 || nullable(n->a);
    default: return 1;                                      /* assertions, empty, back-references */
    }
}

/* Adds to set the bytes a match of n can begin with; returns 1 when n can match the empty string (what follows then counts too). */
static int firstset(const Prog *p, const Node *n, unsigned char *set) {
    switch (n->kind) {
    case N_CHAR:
        set_bit(set, n->c);
        if (p->icase) set_bit(set, other_case(n->c));
        return 0;
    case N_ANY: memset(set, 0xff, 32); return 0;
    case N_CLASS: for (int i = 0; i < 32; i++) set[i] |= p->classes[n->c][i]; return 0;
    case N_GROUP: return firstset(p, n->a, set);
    case N_CAT: for (const Node *c = n; c; c = c->b) if (!firstset(p, c->a, set)) return 0; return 1;
    case N_ALT: { int e = 0; for (const Node *c = n; c; c = c->b) if (firstset(p, c->a, set)) e = 1; return e; }
    case N_REP: { int e = firstset(p, n->a, set); return e || n->min == 0; }
    case N_BACKREF: memset(set, 0xff, 32); return 1;
    default: return 1;
    }
}

/* Does every match have to start with a ^ that matches? */
static int anchored_node(const Node *n) {
    switch (n->kind) {
    case N_BOL: return 1;
    case N_GROUP: return anchored_node(n->a);
    case N_CAT: return anchored_node(n->a);
    case N_ALT: for (const Node *c = n; c; c = c->b) if (!anchored_node(c->a)) return 0; return 1;
    case N_REP: return n->min >= 1 && anchored_node(n->a);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ AST -> program */

static int emit(Prog *p, int op, int x, int y) {
    if (p->overflow) return 0;
    if (p->n >= MAX_PROG) { p->overflow = 1; return 0; }
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        Inst *c = realloc(p->code, (size_t)nc * sizeof(Inst));
        if (!c) { p->overflow = 1; return 0; }
        p->code = c;
        p->cap = nc;
    }
    p->code[p->n] = (Inst){ op, x, y, 0, 0 };
    return p->n++;
}

static int single_matcher(Prog *p, const Node *n, int *kind, int *arg) {
    switch (n->kind) {
    case N_ANY: *kind = RK_ANY; *arg = 0; return 1;
    case N_CLASS: *kind = RK_CLASS; *arg = n->c; return 1;
    case N_CHAR:
        if (p->icase && other_case(n->c) != n->c) {
            unsigned char bits[32];
            memset(bits, 0, sizeof bits);
            set_bit(bits, n->c);
            set_bit(bits, other_case(n->c));
            *kind = RK_CLASS;
            *arg = add_class(p, bits);
        } else { *kind = RK_CHAR; *arg = n->c; }
        return 1;
    default: return 0;
    }
}

static void gen(Prog *p, const Node *n);

static void gen_rep(Prog *p, const Node *n) {
    int mn = n->min, mx = n->max, kind, arg;
    const Node *body = n->a;
    if (single_matcher(p, body, &kind, &arg)) {
        if (p->overflow || mx == 0) return;
        if (mn == 1 && mx == 1) { gen(p, body); return; }
        int k = emit(p, OP_REP1, mn, mx);
        if (!p->overflow) { p->code[k].z = kind; p->code[k].w = arg; }
        return;
    }
    for (int i = 0; i < mn && !p->overflow; i++) gen(p, body);
    if (mx < 0) {                                           /* body* */
        int guard = nullable(body);                         /* an empty iteration must end the loop, not repeat for ever */
        int reg = guard ? p->nregs++ : -1;
        if (guard) emit(p, OP_LOOPINIT, reg, mn == 0);      /* with mandatory copies before it, no iteration of the loop may be empty */
        int split = emit(p, OP_SPLIT, p->n + 1, 0);
        if (guard) emit(p, OP_MARK, reg, 0);
        gen(p, body);
        int progress = guard ? emit(p, OP_PROGRESS, reg, 0) : -1;
        emit(p, OP_JMP, split, 0);
        if (!p->overflow) { p->code[split].y = p->n; if (guard) p->code[progress].y = p->n; }
    } else {                                                /* mx - mn nested optional copies; the splits are chained through y until the end is known */
        int chain = -1;
        int guard = nullable(body);                         /* an optional copy after the first iteration must not be empty (see OP_PROGRESS) */
        for (int i = mn; i < mx && !p->overflow; i++) {
            chain = emit(p, OP_SPLIT, p->n + 1, chain);
            int reg = guard && i > 0 ? p->nregs++ : -1;
            if (reg >= 0) emit(p, OP_MARK, reg, 0);
            gen(p, body);
            if (reg >= 0) emit(p, OP_PROGRESS, reg, p->n + 1);
        }
        if (!p->overflow) while (chain >= 0) { int prev = p->code[chain].y; p->code[chain].y = p->n; chain = prev; }
    }
}

static void gen(Prog *p, const Node *n) {
    if (p->overflow) return;
    switch (n->kind) {
    case N_EMPTY: break;
    case N_CHAR: emit(p, OP_CHAR, n->c, p->icase ? other_case(n->c) : n->c); break;
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
    case N_CAT: for (const Node *c = n; c; c = c->b) gen(p, c->a); break;
    case N_ALT: {                                           /* the jumps to the end are chained through x until the end is known */
        int jumps = -1;
        for (const Node *c = n; c && !p->overflow; c = c->b) {
            if (c->b) {
                int split = emit(p, OP_SPLIT, p->n + 1, 0);
                gen(p, c->a);
                jumps = emit(p, OP_JMP, jumps, 0);
                if (!p->overflow) p->code[split].y = p->n;
            } else gen(p, c->a);
        }
        if (!p->overflow) while (jumps >= 0) { int prev = p->code[jumps].x; p->code[jumps].x = p->n; jumps = prev; }
        break;
    }
    case N_REP: gen_rep(p, n); break;
    }
}

/* Marks the instructions that can be entered from two places (the only places where two paths of the search can meet). */
static int find_joins(Prog *p) {
    int n = p->n;
    int *indeg = calloc((size_t)n + 1, sizeof(int));
    int *idx = malloc((size_t)(2 * n) * sizeof(int));
    if (!indeg || !idx) { free(indeg); free(idx); return 0; }
    indeg[0] = 1;                                           /* the entry */
    for (int pc = 0; pc < n; pc++) {
        const Inst *in = &p->code[pc];
        switch (in->op) {
        case OP_JMP: indeg[in->x]++; break;
        case OP_SPLIT: indeg[in->x]++; indeg[in->y]++; break;
        case OP_PROGRESS: if (in->y != pc + 1) indeg[in->y]++; indeg[pc + 1]++; break;
        case OP_REP1: indeg[pc + 1] += in->y < 0 || in->y > in->x ? 2 : 1; break;      /* the next instruction is entered at several positions */
        case OP_MATCH: break;
        default: indeg[pc + 1]++; break;
        }
    }
    p->jidx = idx;
    p->ridx = idx + n;
    p->njoin = p->nrep = 0;
    for (int pc = 0; pc < n; pc++) {
        p->jidx[pc] = indeg[pc] >= 2 ? p->njoin++ : -1;
        p->ridx[pc] = p->code[pc].op == OP_REP1 && p->code[pc].y < 0 ? p->nrep++ : -1;
    }
    free(indeg);
    return 1;
}

/* ------------------------------------------------------------------ matcher */

typedef struct { long pos, aux; int pc, kind; } Frame;
enum { F_BRANCH, F_RESTORE, F_RANGE };      /* try (pc, pos) later / put register pc back to pos / try pc at pos, pos-1 .. aux */

#define NSTACK0 32
#define NREGS0  24
#ifndef NBITS0
#define NBITS0  1024                /* bits of the visited bitmap that live in Run */
#endif
#define NREP0   4

typedef struct {
    const Prog *p;
    const char *start;          /* the string the offsets are relative to; what ^ and \b look back into */
    const char *end;            /* end of the window, or NULL: the string ends at its NUL and has not been measured */
    const char *base;           /* the position bit 0 of the visited bitmap stands for */
    const char *nul_end;        /* the NUL of an unmeasured string, once a back-reference has needed it */
    int notbol, exhaustive, memo, err, have;
    int savelim;                /* OP_SAVE only records registers below this one */
    long best_end;
    long *regs;                 /* captures, 2 per group with group 0 first, then the loop registers */
    long *best;                 /* captures of the best match found so far */
    long *riv;                  /* per unbounded one-byte loop: positions [lo, hi] from which everything has been explored */
    Frame *stk;
    size_t sp, cap;
    unsigned char *bits;
    size_t bitcap;              /* bits */
    long steps;
    Frame stk0[NSTACK0];
    long regs0[NREGS0 * 2];
    long riv0[NREP0 * 2];
    unsigned char bits0[NBITS0 / 8];
} Run;

static int grow_stack(Run *r) {
    size_t nc = r->cap * 2;
    if (nc > (size_t)MAX_FRAMES) return 0;
    Frame *n;
    if (r->stk == r->stk0) {
        n = malloc(nc * sizeof *n);
        if (n) memcpy(n, r->stk0, r->sp * sizeof *n);
    } else n = realloc(r->stk, nc * sizeof *n);
    if (!n) return 0;
    r->stk = n;
    r->cap = nc;
    return 1;
}

static int grow_bits(Run *r, size_t bit) {
    size_t nb = r->bitcap * 2;
    if (nb <= bit) nb = (bit | 1023) + 1;
    if (nb > (size_t)MEMO_MAX_BITS) return 0;
    size_t oldb = r->bitcap >> 3, newb = nb >> 3;
    unsigned char *n;
    if (r->bits == r->bits0) {
        n = malloc(newb);
        if (n) memcpy(n, r->bits0, oldb);
    } else n = realloc(r->bits, newb);
    if (!n) return 0;
    memset(n + oldb, 0, newb - oldb);
    r->bits = n;
    r->bitcap = nb;
    return 1;
}

/* Has join number j been entered at s before? Marks it. When the bitmap cannot grow any more, remembering stops and the search
 * settles for the first match it finds. */
static inline int visit(Run *r, int j, const char *s) {
    size_t bit = (size_t)(s - r->base) * (size_t)r->p->njoin + (size_t)j;
    if (bit >= r->bitcap && !grow_bits(r, bit)) { r->memo = 0; r->exhaustive = 0; return 0; }
    unsigned char m = (unsigned char)(1u << (bit & 7));
    unsigned char *b = &r->bits[bit >> 3];
    if (*b & m) return 1;
    *b |= m;
    return 0;
}

#define PUSH(k, pcv, posv, auxv) do { \
        if (r->sp == r->cap && !grow_stack(r)) { r->err = REG_ESPACE; return 0; } \
        Frame *f_ = &r->stk[r->sp++]; \
        f_->kind = (k); f_->pc = (pcv); f_->pos = (posv); f_->aux = (auxv); \
    } while (0)

#define AT_END(q) (nul ? *(q) == '\0' : (q) >= end)
#define WORD_BEFORE(q) ((q) > start && is_word((unsigned char)(q)[-1]))
#define WORD_AFTER(q) (!AT_END(q) && is_word((unsigned char)*(q)))

/* Looks for a match that starts exactly at s. Returns 1 when there is one (r->best, r->best_end). `nul` is a constant in the two
 * copies that regexec calls. */
static inline __attribute__((always_inline)) int exec_at(Run *r, const char *s, const int nul) {
    const Prog *const p = r->p;
    const Inst *const code = p->code;
    const char *const start = r->start;
    const char *const end = r->end;
    long *const regs = r->regs;
    int pc = 0;

    regs[0] = s - start;
    r->sp = 0;
    r->have = 0;
    r->steps = 0;
    for (;;) {
        for (;;) {
            if (r->memo) {
                int j = p->jidx[pc];
                if (j >= 0 && visit(r, j, s)) goto fail;
            } else if (++r->steps > STEP_LIMIT) {
                while (r->sp) { Frame *f = &r->stk[--r->sp]; if (f->kind == F_RESTORE) regs[f->pc] = f->pos; }
                return r->have;
            }
            const Inst *in = &code[pc];
            switch (in->op) {
            case OP_CHAR: {
                if (AT_END(s)) goto fail;
                int c = (unsigned char)*s;
                if (c != in->x && c != in->y) goto fail;
                s++; pc++;
                break;
            }
            case OP_ANY:
                if (AT_END(s) || (p->newline && *s == '\n')) goto fail;
                s++; pc++;
                break;
            case OP_CLASS:
                if (AT_END(s) || !(p->classes[in->x][(unsigned char)*s >> 3] & (1 << ((unsigned char)*s & 7)))) goto fail;
                s++; pc++;
                break;
            case OP_BOL:
                if (!((s == start && !r->notbol) || (p->newline && s > start && s[-1] == '\n'))) goto fail;
                pc++;
                break;
            case OP_EOL:
                if (!(AT_END(s) || (p->newline && *s == '\n'))) goto fail;
                pc++;
                break;
            case OP_WORDB: if (WORD_BEFORE(s) == WORD_AFTER(s)) goto fail; pc++; break;
            case OP_NWORDB: if (WORD_BEFORE(s) != WORD_AFTER(s)) goto fail; pc++; break;
            case OP_WORDSTART: if (WORD_BEFORE(s) || !WORD_AFTER(s)) goto fail; pc++; break;
            case OP_WORDEND: if (!WORD_BEFORE(s) || WORD_AFTER(s)) goto fail; pc++; break;
            case OP_BACKREF: {
                int g = in->x;
                if (g > p->ngroups || regs[g * 2] < 0 || regs[g * 2 + 1] < 0) goto fail;
                long len = regs[g * 2 + 1] - regs[g * 2];
                const char *cap = start + regs[g * 2];
                if (nul) {                                  /* a back-reference is the one thing that has to know where the string ends */
                    if (!r->nul_end) r->nul_end = s + strlen(s);
                    if (r->nul_end - s < len) goto fail;
                } else if (end - s < len) goto fail;
                for (long i = 0; i < len; i++) {
                    unsigned char a = (unsigned char)s[i], b = (unsigned char)cap[i];
                    if (a != b && !(p->icase && tolower(a) == tolower(b))) goto fail;
                }
                s += len; pc++;
                break;
            }
            case OP_JMP: pc = in->x; break;
            case OP_SPLIT:
                PUSH(F_BRANCH, in->y, s - start, 0);
                pc = in->x;
                break;
            case OP_SAVE:
                if (in->x < r->savelim) {                           /* groups nobody asked for are not recorded (and cost no stack) */
                    PUSH(F_RESTORE, in->x, regs[in->x], 0);
                    regs[in->x] = s - start;
                }
                pc++;
                break;
            case OP_LOOPINIT:
                PUSH(F_RESTORE, in->x, regs[in->x], 0);
                regs[in->x] = in->y ? -2 : -3;
                pc++;
                break;
            case OP_MARK: {
                long nv = (s - start) * 2 + (regs[in->x] == -2);
                PUSH(F_RESTORE, in->x, regs[in->x], 0);
                regs[in->x] = nv;
                pc++;
                break;
            }
            case OP_PROGRESS: {
                long v = regs[in->x];
                if ((v >> 1) != s - start) pc++;                            /* the iteration consumed something: go round again */
                else if (v & 1) pc = in->y;                                 /* an empty first iteration: the loop is over */
                else goto fail;
                break;
            }
            case OP_REP1: {
                /* An unbounded loop remembers one interval [lo, hi] of positions it has been entered at: all the run that ends at
                 * hi has been explored from lo on. Entering at a position inside is pointless; entering below lo only needs a scan
                 * up to lo, and only the continuations below the explored ones are new. */
                long pos = s - start, lim = in->y, ivlo = 0, ivhi = 0;
                long *iv = NULL;
                int joined = 0;
                if (r->memo && lim < 0) {
                    iv = &r->riv[2 * p->ridx[pc]];
                    if (pos >= iv[0] && pos <= iv[1]) goto fail;
                    if (iv[0] <= iv[1] && pos < iv[0]) { ivlo = iv[0]; ivhi = iv[1]; lim = ivlo - pos; joined = 1; }
                }
                const char *q = s;
                switch (in->z) {
                case RK_ANY:
                    if (p->newline) {
                        if (nul) { while (*q && *q != '\n' && (lim < 0 || q - s < lim)) q++; }
                        else {
                            size_t avail = (size_t)(end - s);
                            if (lim >= 0 && (size_t)lim < avail) avail = (size_t)lim;
                            const char *nl = memchr(s, '\n', avail);
                            q = nl ? nl : s + avail;
                        }
                    } else if (nul) {
                        q = s + (lim < 0 ? strlen(s) : strnlen(s, (size_t)lim));
                    } else {
                        size_t avail = (size_t)(end - s);
                        if (lim >= 0 && (size_t)lim < avail) avail = (size_t)lim;
                        q = s + avail;
                    }
                    break;
                case RK_CHAR: {
                    const unsigned char ch = (unsigned char)in->w;
                    while ((lim < 0 || q - s < lim) && !AT_END(q) && (unsigned char)*q == ch) q++;
                    break;
                }
                default: {
                    const unsigned char *cls = p->classes[in->w];
                    while ((lim < 0 || q - s < lim) && !AT_END(q) && ((cls[(unsigned char)*q >> 3] >> ((unsigned char)*q & 7)) & 1)) q++;
                    break;
                }
                }
                long top = q - start, bottom = pos + in->x;         /* the longest and the shortest continuation to try */
                if (joined && q - s == lim) {                       /* the whole stretch below lo matched: the run goes on to hi */
                    top = ivhi;
                    if (top > ivlo + in->x - 1) top = ivlo + in->x - 1;     /* what starts at lo or higher has been tried */
                    if (iv) { iv[0] = pos; iv[1] = ivhi; }
                } else if (iv) { iv[0] = pos; iv[1] = top; }        /* also when the run is too short: so is every shorter run inside it */
                if (top < bottom) goto fail;
                pc++;
                if (top > bottom && code[pc].op != OP_MATCH)        /* shorter runs: if the next instruction is OP_MATCH they would only end earlier */
                    PUSH(F_RANGE, pc, top - 1, bottom);
                s = start + top;
                break;
            }
            case OP_MATCH: {
                long e = s - start;
                if (!r->have || e > r->best_end) {
                    r->have = 1;
                    r->best_end = e;
                    memcpy(r->best, regs, (size_t)(p->ngroups + 1) * 2 * sizeof(long));
                }
                if (!r->exhaustive || AT_END(s)) return 1;      /* the first match is wanted, or no match can be longer */
                goto fail;
            }
            }
        }
    fail:
        for (;;) {
            if (r->sp == 0) return r->have;
            Frame *f = &r->stk[r->sp - 1];
            if (f->kind == F_RESTORE) { regs[f->pc] = f->pos; r->sp--; continue; }
            pc = f->pc;
            s = start + f->pos;
            if (f->kind == F_RANGE && f->pos > f->aux) f->pos--; else r->sp--;
            break;
        }
    }
}

static int exec_nul(Run *r, const char *s) { return exec_at(r, s, 1); }
static int exec_len(Run *r, const char *s) { return exec_at(r, s, 0); }

static int run_init(Run *r, const Prog *p, const char *str, const char *end, int eflags, size_t nmatch) {
    r->p = p;
    r->start = str;
    r->end = end;
    r->notbol = (eflags & REG_NOTBOL) != 0;
    r->exhaustive = nmatch > 0;                 /* a yes/no question is answered by the first match */
    r->memo = !p->has_backref;
    r->savelim = p->has_backref || nmatch > (size_t)p->ngroups + 1 ? p->nregs : (int)(2 * nmatch);
    r->err = r->have = 0;
    r->best_end = 0;
    r->nul_end = NULL;
    size_t nr = (size_t)p->nregs;
    r->regs = r->regs0;
    if (nr > NREGS0) {
        r->regs = malloc(2 * nr * sizeof(long));
        if (!r->regs) return REG_ESPACE;
    }
    r->best = r->regs + nr;
    for (size_t i = 0; i < nr; i++) r->regs[i] = -1;
    r->riv = r->riv0;
    if (p->nrep > NREP0) {
        r->riv = malloc((size_t)p->nrep * 2 * sizeof(long));
        if (!r->riv) { if (r->regs != r->regs0) free(r->regs); return REG_ESPACE; }
    }
    for (int i = 0; i < p->nrep; i++) { r->riv[2 * i] = LONG_MAX; r->riv[2 * i + 1] = -1; }
    r->stk = r->stk0;
    r->sp = 0;
    r->cap = NSTACK0;
    r->bits = r->bits0;
    r->bitcap = NBITS0;
    memset(r->bits0, 0, sizeof r->bits0);
    return 0;
}

static void run_free(Run *r) {
    if (r->regs != r->regs0) free(r->regs);
    if (r->riv != r->riv0) free(r->riv);
    if (r->stk != r->stk0) free(r->stk);
    if (r->bits != r->bits0) free(r->bits);
}

/* The next position at or after s where a match could begin, or NULL. */
static const char *next_start(const Prog *p, const char *s, const char *end) {
    if (p->prefix_len > 1) return end ? memmem(s, (size_t)(end - s), p->prefix, (size_t)p->prefix_len) : strstr(s, p->prefix);
    if (p->first_byte >= 0) return end ? memchr(s, p->first_byte, (size_t)(end - s)) : strchr(s, p->first_byte);
    const unsigned char *f = p->first;
    if (end) {
        while (s < end && !((f[(unsigned char)*s >> 3] >> ((unsigned char)*s & 7)) & 1)) s++;
        return s < end ? s : NULL;
    }
    while (*s && !((f[(unsigned char)*s >> 3] >> ((unsigned char)*s & 7)) & 1)) s++;
    return *s ? s : NULL;
}

/* ------------------------------------------------------------------ public API */

int regcomp(regex_t *re, const char *pattern, int flags) {
    re->prog = NULL;
    re->re_nsub = 0;
    re->flags = flags;
    Prog *pr = calloc(1, sizeof *pr);
    if (!pr) return REG_ESPACE;
    pr->icase = (flags & REG_ICASE) != 0;
    pr->newline = (flags & REG_NEWLINE) != 0;
    pr->first_byte = -1;
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.p = pattern;
    ps.ere = (flags & REG_EXTENDED) != 0;
    ps.icase = pr->icase;
    ps.prog = pr;
    Node *ast = parse_alt(&ps, 0);
    if (!ps.error && *ps.p) ps.error = REG_BADPAT;                  /* stray ')' */
    if (!ps.error && pr->overflow) ps.error = REG_ESPACE;
    if (!ps.error) {
        pr->ngroups = ps.ngroups;
        pr->nregs = 2 * (ps.ngroups + 1);
        pr->has_backref = ps.has_backref;
        gen(pr, ast);
        emit(pr, OP_MATCH, 0, 0);
        if (pr->overflow) ps.error = REG_ESPACE;
        else if (!find_joins(pr)) ps.error = REG_ESPACE;
    }
    if (!ps.error) {
        pr->first_known = !firstset(pr, ast, pr->first);
        int bits = 0, only = -1;
        for (int c = 0; c < 256; c++) if ((pr->first[c >> 3] >> (c & 7)) & 1) { bits++; only = c; }
        if (bits == 1) pr->first_byte = only;
        pr->anchored = !pr->newline && anchored_node(ast);
        char lit[64];
        int len = 0;
        if (!pr->icase) {
            if (ast->kind == N_CHAR) lit[len++] = (char)ast->c;
            else if (ast->kind == N_CAT) for (const Node *c = ast; c && c->a->kind == N_CHAR && len < 63; c = c->b) lit[len++] = (char)c->a->c;
        }
        if (len > 1 && pr->first_known) {
            pr->prefix = malloc((size_t)len + 1);
            if (pr->prefix) { memcpy(pr->prefix, lit, (size_t)len); pr->prefix[len] = 0; pr->prefix_len = len; }
        }
    }
    free_chunks(ps.chunks);
    if (ps.error) {
        free(pr->classes);
        free(pr->code);
        free(pr->jidx);
        free(pr->prefix);
        free(pr);
        return ps.error;
    }
    re->prog = pr;
    re->re_nsub = (size_t)ps.ngroups;
    return 0;
}

int regexec(const regex_t *re, const char *str, size_t nmatch, regmatch_t pmatch[], int eflags) {
    const Prog *p = re->prog;
    if (!p) return REG_BADPAT;
    const char *from = str, *end = NULL;
    if (eflags & REG_STARTEND) {                /* a window into str: the context before it still counts for ^ and \b */
        if (pmatch[0].rm_so < 0 || pmatch[0].rm_so > pmatch[0].rm_eo) return REG_NOMATCH;
        from = str + pmatch[0].rm_so;
        end = str + pmatch[0].rm_eo;
    }
    if (re->flags & REG_NOSUB) nmatch = 0;
    Run r;
    int rc = run_init(&r, p, str, end, eflags, nmatch);
    if (rc) return rc;
    r.base = from;
    int found = 0;
    const char *s = from;
    if (p->anchored) {
        found = s == str && !r.notbol && (end ? exec_len(&r, s) : exec_nul(&r, s));
    } else {
        for (;;) {
            if (p->first_known) {
                s = next_start(p, s, end);
                if (!s) break;
            }
            if (end ? exec_len(&r, s) : exec_nul(&r, s)) { found = 1; break; }
            if (r.err || (end ? s >= end : !*s)) break;
            s++;
        }
    }
    if (found) {
        for (size_t i = 0; i < nmatch; i++) {
            if (i == 0) { pmatch[0].rm_so = r.best[0]; pmatch[0].rm_eo = r.best_end; }
            else if ((int)i <= p->ngroups && r.best[2 * i] >= 0 && r.best[2 * i + 1] >= 0) { pmatch[i].rm_so = r.best[2 * i]; pmatch[i].rm_eo = r.best[2 * i + 1]; }
            else pmatch[i].rm_so = pmatch[i].rm_eo = -1;
        }
    }
    rc = r.err ? r.err : found ? 0 : REG_NOMATCH;
    run_free(&r);
    return rc;
}

void regfree(regex_t *re) {
    Prog *p = re->prog;
    if (!p) return;
    free(p->code);
    free(p->classes);
    free(p->jidx);
    free(p->prefix);
    free(p);
    re->prog = NULL;
}

size_t regerror(int code, const regex_t *re, char *buf, size_t n) {
    (void)re;
    const char *m = code == REG_NOMATCH ? "no match" : code == REG_BADPAT ? "invalid regular expression" : code == REG_ESPACE ? "out of memory" : "regex error";
    if (n) { strncpy(buf, m, n - 1); buf[n - 1] = 0; }
    return strlen(m) + 1;
}
