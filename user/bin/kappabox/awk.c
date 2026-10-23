/*
 * awk: pattern scanning and processing language (POSIX awk with a few gawk extras).
 * Layout: values and memory (arena for temporaries, cells for variables, hash tables for arrays), regex glue,
 * lexer and recursive-descent parser building an AST, a tree-walking interpreter, builtins and printf, I/O.
 * Strings are (pointer, length) pairs everywhere; temporaries live in an arena that is reset after every statement.
 */
#include "kappabox.h"
#include <regex.h>
#include <setjmp.h>
#include <math.h>
#include <stdarg.h>

#define AWK_VERSION "awk version 20260101 (kappabox)"

/* ------------------------------------------------------------------ errors and memory */

static void out_flush_all(void);

static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fatal(const char *fmt, ...) {
    va_list ap;
    out_flush_all();
    fprintf(stderr, "awk: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) fatal("out of memory"); return p; }
static void *xcalloc(size_t n, size_t m) { void *p = calloc(n ? n : 1, m ? m : 1); if (!p) fatal("out of memory"); return p; }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->len + need + 1) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
}
static void buf_add(Buf *b, const char *s, size_t n) { buf_grow(b, n); memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; }
static void buf_addc(Buf *b, char c) { buf_grow(b, 1); b->p[b->len++] = c; b->p[b->len] = 0; }

/* arena for temporaries: allocation is a pointer bump, a statement's temporaries are dropped with one reset */
typedef struct Chunk { struct Chunk *next; size_t cap, used; char data[]; } Chunk;
typedef struct { Chunk *c; size_t used; } Mark;
static Chunk *g_ar;

static Chunk *chunk_new(size_t cap) {
    Chunk *c = xmalloc(sizeof *c + cap);
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

static char *talloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!g_ar) g_ar = chunk_new(1 << 18);
    if (g_ar->cap - g_ar->used < n) {
        Chunk *nx = g_ar->next;
        if (nx && nx->cap >= n) { nx->used = 0; g_ar = nx; }
        else {
            Chunk *c = chunk_new(n > (1 << 18) ? n : (1 << 18));
            c->next = nx;
            g_ar->next = c;
            g_ar = c;
        }
    }
    char *p = g_ar->data + g_ar->used;
    g_ar->used += n;
    return p;
}
static Mark amark(void) { if (!g_ar) g_ar = chunk_new(1 << 18); return (Mark){ g_ar, g_ar->used }; }
static void areset(Mark m) { g_ar = m.c; g_ar->used = m.used; }

/* ------------------------------------------------------------------ values */

enum { T_UNINIT, T_NUM, T_STR, T_STRNUM, T_INPUT };   /* STRNUM: string that is also a number; INPUT: from input, numeric-ness undecided */
typedef struct { int t; double n; const char *s; size_t len; } Val;

static const Val V_UNINIT = { T_UNINIT, 0, "", 0 };
static Val vnum(double n) { Val v = { T_NUM, n, NULL, 0 }; return v; }
static Val vstr(const char *s, size_t len) { Val v = { T_STR, 0, s, len }; return v; }
static Val vcopy(const char *s, size_t len) {
    char *p = talloc(len + 1);
    memcpy(p, s, len);
    p[len] = 0;
    return vstr(p, len);
}

static const char *g_convfmt = "%.6g", *g_ofmt = "%.6g";

static const double P10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17,
                              1e18, 1e19, 1e20, 1e21, 1e22 };

/* Scans a decimal number at p; returns the end or NULL when there is none. */
static const char *scan_num(const char *p, const char *end, double *out) {
    const char *q = p;
    int neg = 0, digits = 0, exp10 = 0, sig = 0;
    unsigned long long m = 0;
    if (q < end && (*q == '+' || *q == '-')) { neg = *q == '-'; q++; }
    while (q < end && *q >= '0' && *q <= '9') {
        digits++;
        if (sig < 19) { m = m * 10 + (unsigned)(*q - '0'); if (m) sig++; } else exp10++;
        q++;
    }
    if (q < end && *q == '.') {
        q++;
        while (q < end && *q >= '0' && *q <= '9') {
            digits++;
            if (sig < 19) { m = m * 10 + (unsigned)(*q - '0'); if (m) sig++; exp10--; }
            q++;
        }
    }
    if (!digits) return NULL;
    if (q < end && (*q == 'e' || *q == 'E')) {
        const char *r = q + 1;
        int eneg = 0, e = 0, ed = 0;
        if (r < end && (*r == '+' || *r == '-')) { eneg = *r == '-'; r++; }
        while (r < end && *r >= '0' && *r <= '9') { if (e < 10000) e = e * 10 + (*r - '0'); ed++; r++; }
        if (ed) { exp10 += eneg ? -e : e; q = r; }
    }
    double v = (double)m;
    if (m) {
        if (exp10 > 0) {
            while (exp10 > 22 && !isinf(v)) { v *= 1e22; exp10 -= 22; }
            if (exp10 <= 22) v *= P10[exp10];
        } else if (exp10 < 0) {
            int e = -exp10;
            while (e > 22 && v != 0) { v /= 1e22; e -= 22; }
            if (e <= 22) v /= P10[e];
        }
    }
    *out = neg ? -v : v;
    return q;
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* The whole string is a number (leading and trailing blanks allowed). */
static int looks_numeric(const char *s, size_t len, double *out) {
    const char *p = s, *end = s + len;
    while (p < end && is_blank(*p)) p++;
    const char *q = scan_num(p, end, out);
    if (!q) return 0;
    while (q < end && is_blank(*q)) q++;
    return q == end;
}

static double str_to_num(const char *s, size_t len) {
    const char *p = s, *end = s + len;
    double d;
    while (p < end && is_blank(*p)) p++;
    return scan_num(p, end, &d) ? d : 0;
}

static double num_of(Val v) {
    switch (v.t) {
    case T_NUM: case T_STRNUM: return v.n;
    case T_UNINIT: return 0;
    default: return str_to_num(v.s, v.len);
    }
}

static size_t num_str(double n, const char *fmt, char *buf, size_t cap) {
    if (fabs(n) < 1e18 && n == (double)(long long)n) {
        long long x = (long long)n;
        char tmp[24];
        int i = 0, neg = x < 0;
        unsigned long long u = neg ? 0ULL - (unsigned long long)x : (unsigned long long)x;
        do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
        size_t k = 0;
        if (neg) buf[k++] = '-';
        while (i) buf[k++] = tmp[--i];
        buf[k] = 0;
        return k;
    }
    int r = snprintf(buf, cap, fmt, n);
    if (r < 0) r = 0;
    if ((size_t)r >= cap) r = (int)cap - 1;
    return (size_t)r;
}

static __attribute__((noinline)) Val num_to_strval(double d) {
    char b[512];
    size_t n = num_str(d, g_convfmt, b, sizeof b);
    return vcopy(b, n);
}

/* The string value of v (numbers are converted with CONVFMT into the arena). */
static Val tostr(Val v) {
    if (v.t == T_NUM) return num_to_strval(v.n);
    if (v.t == T_UNINIT) return vstr("", 0);
    return v;
}

static int truth(Val v) {
    switch (v.t) {
    case T_NUM: case T_STRNUM: return v.n != 0;
    case T_STR: return v.len > 0;
    case T_INPUT: {
        double d;
        if (looks_numeric(v.s, v.len, &d)) return d != 0;
        return v.len > 0;
    }
    default: return 0;
    }
}

static int is_numeric_val(Val v, double *d) {
    switch (v.t) {
    case T_NUM: case T_STRNUM: *d = v.n; return 1;
    case T_UNINIT: *d = 0; return 1;
    case T_INPUT: return looks_numeric(v.s, v.len, d);
    default: return 0;
    }
}

static int compare_vals(Val a, Val b) {
    double x, y;
    if (is_numeric_val(a, &x) && is_numeric_val(b, &y)) return x < y ? -1 : x > y ? 1 : x == y ? 0 : 2;
    a = tostr(a);
    b = tostr(b);
    size_t m = a.len < b.len ? a.len : b.len;
    int c = m ? memcmp(a.s, b.s, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

/* ------------------------------------------------------------------ cells and arrays */

typedef struct Array Array;
typedef struct { Val v; size_t cap; Array *arr; int borrowed; } Cell;   /* v.s is owned by the cell */
typedef struct Elem { struct Elem *chain, *prev, *next; unsigned hash; size_t klen; Cell cell; char key[]; } Elem;
struct Array { Elem **tab; size_t nb, n; Elem *head, *tail; };

static void cell_set(Cell *c, Val v) {
    if (c->arr) fatal("can't assign to an array name");
    if (v.t == T_NUM || v.t == T_UNINIT) { c->v.t = v.t; c->v.n = v.n; c->v.len = 0; return; }
    if (v.len + 1 > c->cap) {
        size_t nc = (v.len + 16) & ~(size_t)15;
        char *nb = xmalloc(nc);
        memcpy(nb, v.s, v.len);
        free((void *)c->v.s);
        c->v.s = nb;
        c->cap = nc;
    } else {
        memmove((char *)c->v.s, v.s, v.len);
    }
    ((char *)c->v.s)[v.len] = 0;
    c->v.t = v.t;
    c->v.n = v.n;
    c->v.len = v.len;
}

static Val cell_val(Cell *c) {
    if (c->arr) fatal("can't read value of an array name");
    if (c->v.t == T_UNINIT) return V_UNINIT;
    return c->v;
}

static unsigned hash_str(const char *s, size_t n) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static Array *arr_new(void) {
    Array *a = xcalloc(1, sizeof *a);
    a->nb = 16;
    a->tab = xcalloc(a->nb, sizeof(Elem *));
    return a;
}

static void arr_grow(Array *a) {
    size_t nb = a->nb * 2;
    Elem **t = xcalloc(nb, sizeof(Elem *));
    for (Elem *e = a->head; e; e = e->next) {
        size_t i = e->hash & (nb - 1);
        e->chain = t[i];
        t[i] = e;
    }
    free(a->tab);
    a->tab = t;
    a->nb = nb;
}

static Elem *arr_lookup(Array *a, const char *k, size_t kl, int create) {
    unsigned h = hash_str(k, kl);
    for (Elem *e = a->tab[h & (a->nb - 1)]; e; e = e->chain)
        if (e->hash == h && e->klen == kl && !memcmp(e->key, k, kl)) return e;
    if (!create) return NULL;
    if (a->n >= a->nb) arr_grow(a);
    Elem *e = xcalloc(1, sizeof(Elem) + kl + 1);
    memcpy(e->key, k, kl);
    e->hash = h;
    e->klen = kl;
    size_t i = h & (a->nb - 1);
    e->chain = a->tab[i];
    a->tab[i] = e;
    e->prev = a->tail;
    if (a->tail) a->tail->next = e; else a->head = e;
    a->tail = e;
    a->n++;
    return e;
}

static void cell_free(Cell *c);
static void arr_free(Array *a);

static void elem_free(Elem *e) {
    cell_free(&e->cell);
    free(e);
}

static void arr_delete(Array *a, const char *k, size_t kl) {
    unsigned h = hash_str(k, kl);
    Elem **pp = &a->tab[h & (a->nb - 1)];
    for (Elem *e = *pp; e; pp = &e->chain, e = e->chain) {
        if (e->hash == h && e->klen == kl && !memcmp(e->key, k, kl)) {
            *pp = e->chain;
            if (e->prev) e->prev->next = e->next; else a->head = e->next;
            if (e->next) e->next->prev = e->prev; else a->tail = e->prev;
            a->n--;
            elem_free(e);
            return;
        }
    }
}

static void arr_clear(Array *a) {
    Elem *e = a->head;
    while (e) { Elem *nx = e->next; elem_free(e); e = nx; }
    a->head = a->tail = NULL;
    a->n = 0;
    memset(a->tab, 0, a->nb * sizeof(Elem *));
}

static void arr_free(Array *a) {
    arr_clear(a);
    free(a->tab);
    free(a);
}

static void cell_free(Cell *c) {
    free((void *)c->v.s);
    c->v.s = NULL;
    c->cap = 0;
    if (c->arr && !c->borrowed) arr_free(c->arr);
    c->arr = NULL;
}

/* ------------------------------------------------------------------ regular expressions */

typedef struct Regex {
    struct Regex *next;
    char *src;
    size_t srclen;
    regex_t re;
    int lit;                     /* no metacharacters: plain substring search */
    char *litstr;
    size_t litlen;
} Regex;

static void add_set_range(unsigned char *set, int lo, int hi) { for (int c = lo; c <= hi && c < 256; c++) if (c > 0) set[c] = 1; }

/* Decodes the escape at s[*i] (s[*i] is the char after the backslash); returns the character code, or -1 for "keep as is". */
static int decode_escape(const char *s, size_t n, size_t *i) {
    char c = s[*i];
    switch (c) {
    case 'n': (*i)++; return '\n';
    case 't': (*i)++; return '\t';
    case 'r': (*i)++; return '\r';
    case 'f': (*i)++; return '\f';
    case 'v': (*i)++; return '\v';
    case 'a': (*i)++; return 7;
    case 'b': (*i)++; return 8;
    case '/': (*i)++; return '/';
    case '"': (*i)++; return '"';
    case '\\': (*i)++; return '\\';
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = 0, k = 0;
        while (k < 3 && *i < n && s[*i] >= '0' && s[*i] <= '7') { v = v * 8 + (s[*i] - '0'); (*i)++; k++; }
        return v & 255;
    }
    default: return -1;
    }
}

static int is_ere_special(int c) { return c && strchr("\\^$.[]|()*+?{}", c) != NULL; }

/* Converts the bracket expression starting at s[i] == '[' to one the libc engine reads; returns the index after it. */
static size_t convert_bracket(const char *s, size_t n, size_t i, Buf *o) {
    size_t j = i + 1;
    unsigned char set[256];
    int neg = 0, first = 1;
    memset(set, 0, sizeof set);
    if (j < n && s[j] == '^') { neg = 1; j++; }
    while (j < n && (s[j] != ']' || first)) {
        first = 0;
        if (s[j] == '[' && j + 1 < n && s[j + 1] == ':') {
            const char *e = NULL;
            for (size_t k = j + 2; k + 1 < n; k++) if (s[k] == ':' && s[k + 1] == ']') { e = s + k; break; }
            if (e) {
                char name[16];
                size_t nl = (size_t)(e - (s + j + 2));
                if (nl < sizeof name) {
                    memcpy(name, s + j + 2, nl);
                    name[nl] = 0;
                    static const struct { const char *nm; int (*fn)(int); } cl[] = {
                        { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "upper", isupper }, { "lower", islower },
                        { "space", isspace }, { "blank", isblank }, { "punct", ispunct }, { "print", isprint }, { "graph", isgraph },
                        { "cntrl", iscntrl }, { "xdigit", isxdigit } };
                    for (unsigned k = 0; k < sizeof cl / sizeof cl[0]; k++)
                        if (!strcmp(cl[k].nm, name)) { for (int c = 1; c < 256; c++) if (cl[k].fn(c)) set[c] = 1; break; }
                    j = (size_t)(e - s) + 2;
                    continue;
                }
            }
        }
        int lo = (unsigned char)s[j++];
        if (lo == '\\' && j < n) { int d = decode_escape(s, n, &j); lo = d >= 0 ? d : (unsigned char)s[j++]; }
        if (j + 1 < n && s[j] == '-' && s[j + 1] != ']') {
            j++;
            int hi = (unsigned char)s[j++];
            if (hi == '\\' && j < n) { int d = decode_escape(s, n, &j); hi = d >= 0 ? d : (unsigned char)s[j++]; }
            add_set_range(set, lo, hi);
        } else if (lo > 0) {
            set[lo] = 1;
        }
    }
    if (j >= n) {                              /* unterminated: a literal [ */
        buf_add(o, "\\[", 2);
        return i + 1;
    }
    j++;
    int any = 0, has_rb = set[']'], has_caret = set['^'], has_dash = set['-'];
    for (int c = 1; c < 256; c++) if (set[c]) { any = 1; break; }
    if (!any) { buf_add(o, neg ? "." : "a^", neg ? 1 : 2); return j; }
    if (!neg && has_caret) {
        int others = 0;
        for (int c = 1; c < 256; c++) if (set[c] && c != '^') others = 1;
        if (!others) { buf_add(o, "\\^", 2); return j; }
    }
    set[']'] = set['^'] = set['-'] = 0;
    buf_addc(o, '[');
    if (neg) buf_addc(o, '^');
    if (has_rb) buf_addc(o, ']');
    for (int c = 1; c < 256; c++) {
        if (!set[c]) continue;
        int e = c;
        while (e + 1 < 256 && set[e + 1]) e++;
        if (e - c >= 2) { buf_addc(o, (char)c); buf_addc(o, '-'); buf_addc(o, (char)e); }
        else for (int k = c; k <= e; k++) buf_addc(o, (char)k);
        c = e;
    }
    if (has_caret) buf_addc(o, '^');
    if (has_dash) buf_addc(o, '-');
    buf_addc(o, ']');
    return j;
}

/* awk regular expression text -> POSIX ERE as the libc engine wants it */
static void ere_convert(const char *s, size_t n, Buf *o) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 >= n) { buf_add(o, "\\\\", 2); i++; continue; }
            size_t j = i + 1;
            if (s[j] == 'y') { buf_add(o, "\\b", 2); i += 2; continue; }
            if (s[j] == 'b') { buf_addc(o, 8); i += 2; continue; }
            int d = decode_escape(s, n, &j);
            if (d < 0) { buf_addc(o, '\\'); buf_addc(o, s[j]); i = j + 1; continue; }
            if (s[i + 1] == '\\') { buf_add(o, "\\\\", 2); i = j; continue; }
            if (d == 0) { i = j; continue; }
            if (is_ere_special(d)) buf_addc(o, '\\');
            buf_addc(o, (char)d);
            i = j;
        } else if (c == '[') {
            i = convert_bracket(s, n, i, o);
        } else {
            buf_addc(o, (char)c);
            i++;
        }
    }
}

static Regex *regex_compile(const char *src, size_t n) {
    Regex *r = xcalloc(1, sizeof *r);
    Buf b = { 0 };
    r->src = xstrndup(src, n);
    r->srclen = n;
    ere_convert(src, n, &b);
    if (!b.p) buf_add(&b, "", 0);
    int lit = 1;
    for (size_t i = 0; i < b.len; i++) if (is_ere_special((unsigned char)b.p[i])) { lit = 0; break; }
    if (lit) {
        r->lit = 1;
        r->litstr = b.p;
        r->litlen = b.len;
        return r;
    }
    int rc = regcomp(&r->re, b.p, REG_EXTENDED);
    if (rc) fatal("syntax error in regular expression %.*s", (int)(n > 60 ? 60 : n), src);
    free(b.p);
    return r;
}

#define RE_BUCKETS 256
static Regex *g_recache[RE_BUCKETS];
static int g_recount;

static Regex *regex_get(const char *s, size_t n) {
    unsigned h = hash_str(s, n) & (RE_BUCKETS - 1);
    for (Regex *r = g_recache[h]; r; r = r->next) if (r->srclen == n && !memcmp(r->src, s, n)) return r;
    if (g_recount > 3000) {                    /* a program that makes endless distinct patterns: start over */
        for (int i = 0; i < RE_BUCKETS; i++) {
            Regex *r = g_recache[i];
            while (r) { Regex *nx = r->next; if (!r->lit) regfree(&r->re); free(r->src); free(r->litstr); free(r); r = nx; }
            g_recache[i] = NULL;
        }
        g_recount = 0;
    }
    Regex *r = regex_compile(s, n);
    r->next = g_recache[h];
    g_recache[h] = r;
    g_recount++;
    return r;
}

/* First match of r in s[0..len) starting at or after from. */
static int re_search(Regex *r, const char *s, size_t len, size_t from, size_t *ms, size_t *me) {
    if (from > len) return 0;
    if (r->lit) {
        if (r->litlen == 0) { *ms = *me = from; return 1; }
        const char *p = memmem(s + from, len - from, r->litstr, r->litlen);
        if (!p) return 0;
        *ms = (size_t)(p - s);
        *me = *ms + r->litlen;
        return 1;
    }
    regmatch_t m[1];
    m[0].rm_so = (regoff_t)from;
    m[0].rm_eo = (regoff_t)len;
    if (regexec(&r->re, s, 1, m, REG_STARTEND)) return 0;
    *ms = (size_t)m[0].rm_so;
    *me = (size_t)m[0].rm_eo;
    return 1;
}

/* ------------------------------------------------------------------ AST and symbols */

enum {
    N_NUM, N_STR, N_REGEX, N_VAR, N_FIELD, N_ELEM, N_GROUP, N_GROUPLIST, N_ASSIGN, N_COND, N_OR, N_AND, N_NOT, N_NEG, N_PLUS,
    N_BINOP, N_CMP, N_MATCH, N_CONCAT, N_IN, N_INCR, N_CALL, N_BUILTIN, N_GETLINE,
    N_BLOCK, N_EXPR, N_PRINT, N_PRINTF, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN, N_NEXT, N_NEXTFILE, N_EXIT, N_RETURN, N_BREAK,
    N_CONTINUE, N_DELETE
};

enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT,
       B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH, B_SYSTIME, B_STRFTIME };

typedef struct Node Node;
struct Node {
    int type, line;
    Node *a, *b, *c, *d;
    Node *next;
    double num;
    const char *str;
    size_t slen;
    int idx, scope, nargs;
    Regex *re;
};

static int g_lexline;

static Node *mk(int type) {
    Node *n = xcalloc(1, sizeof *n);
    n->type = type;
    n->line = g_lexline;
    return n;
}
static Node *mk1(int type, Node *a) { Node *n = mk(type); n->a = a; return n; }
static Node *mk2(int type, Node *a, Node *b) { Node *n = mk(type); n->a = a; n->b = b; return n; }

/* special variables have fixed global indices */
enum { V_NF, V_NR, V_FNR, V_FS, V_OFS, V_ORS, V_RS, V_FILENAME, V_SUBSEP, V_RSTART, V_RLENGTH, V_CONVFMT, V_OFMT, V_ENVIRON, V_ARGC,
       V_ARGV, NSPECIAL };
static const char *const g_special_names[NSPECIAL] = { "NF", "NR", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "RSTART",
                                                        "RLENGTH", "CONVFMT", "OFMT", "ENVIRON", "ARGC", "ARGV" };

static char **g_gnames;
static int g_nglob, g_globcap;

static int global_index(const char *name, size_t n) {
    for (int i = 0; i < g_nglob; i++) if (!strncmp(g_gnames[i], name, n) && !g_gnames[i][n]) return i;
    if (g_nglob == g_globcap) { g_globcap = g_globcap ? g_globcap * 2 : 64; g_gnames = xrealloc(g_gnames, (size_t)g_globcap * sizeof(char *)); }
    g_gnames[g_nglob] = xstrndup(name, n);
    return g_nglob++;
}

typedef struct { char *name; int np; Node *body; int defined; char **params; } Func;
static Func *g_funcs;
static int g_nfuncs, g_funccap;
static Func *g_curfunc;

static int func_index(const char *name, size_t n) {
    for (int i = 0; i < g_nfuncs; i++) if (!strncmp(g_funcs[i].name, name, n) && !g_funcs[i].name[n]) return i;
    if (g_nfuncs == g_funccap) { g_funccap = g_funccap ? g_funccap * 2 : 16; g_funcs = xrealloc(g_funcs, (size_t)g_funccap * sizeof(Func)); }
    memset(&g_funcs[g_nfuncs], 0, sizeof(Func));
    g_funcs[g_nfuncs].name = xstrndup(name, n);
    return g_nfuncs++;
}

typedef struct { Node *p1, *p2, *action; int in_range; } Rule;
static Rule *g_rules;
static int g_nrules, g_rulecap;
static Node **g_begins, **g_ends;
static int g_nbegin, g_nend;

/* ------------------------------------------------------------------ lexer */

enum {
    TK_EOF, TK_NL, TK_NUM, TK_STR, TK_ERE, TK_NAME, TK_FNAME, TK_BUILTIN,
    TK_BEGIN, TK_END, TK_FUNCTION, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO, TK_BREAK, TK_CONTINUE, TK_NEXT, TK_NEXTFILE, TK_EXIT,
    TK_RETURN, TK_DELETE, TK_IN, TK_GETLINE, TK_PRINT, TK_PRINTF,
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET, TK_SEMI, TK_COMMA,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_CARET, TK_NOT, TK_GT, TK_LT, TK_PIPE, TK_QUESTION, TK_COLON, TK_TILDE,
    TK_NOMATCH, TK_DOLLAR, TK_ASSIGN, TK_ADDASSIGN, TK_SUBASSIGN, TK_MULASSIGN, TK_DIVASSIGN, TK_MODASSIGN, TK_POWASSIGN,
    TK_EQ, TK_LE, TK_GE, TK_NE, TK_INCR, TK_DECR, TK_AND, TK_OR, TK_APPEND
};

typedef struct { const char *p; int line; int tok; double num; const char *str; size_t slen; int bi; const char *tstart; } Lex;
static Lex lx;

static void syntax(void) { fatal("syntax error at source line %d", lx.line); }

static const struct { const char *name; int tok, bi; } g_keywords[] = {
    { "BEGIN", TK_BEGIN, 0 }, { "END", TK_END, 0 }, { "function", TK_FUNCTION, 0 }, { "func", TK_FUNCTION, 0 }, { "if", TK_IF, 0 },
    { "else", TK_ELSE, 0 }, { "while", TK_WHILE, 0 }, { "for", TK_FOR, 0 }, { "do", TK_DO, 0 }, { "break", TK_BREAK, 0 },
    { "continue", TK_CONTINUE, 0 }, { "next", TK_NEXT, 0 }, { "nextfile", TK_NEXTFILE, 0 }, { "exit", TK_EXIT, 0 },
    { "return", TK_RETURN, 0 }, { "delete", TK_DELETE, 0 }, { "in", TK_IN, 0 }, { "getline", TK_GETLINE, 0 }, { "print", TK_PRINT, 0 },
    { "printf", TK_PRINTF, 0 },
    { "length", TK_BUILTIN, B_LENGTH }, { "substr", TK_BUILTIN, B_SUBSTR }, { "index", TK_BUILTIN, B_INDEX },
    { "split", TK_BUILTIN, B_SPLIT }, { "sub", TK_BUILTIN, B_SUB }, { "gsub", TK_BUILTIN, B_GSUB }, { "match", TK_BUILTIN, B_MATCH },
    { "sprintf", TK_BUILTIN, B_SPRINTF }, { "sin", TK_BUILTIN, B_SIN }, { "cos", TK_BUILTIN, B_COS }, { "atan2", TK_BUILTIN, B_ATAN2 },
    { "exp", TK_BUILTIN, B_EXP }, { "log", TK_BUILTIN, B_LOG }, { "sqrt", TK_BUILTIN, B_SQRT }, { "int", TK_BUILTIN, B_INT },
    { "rand", TK_BUILTIN, B_RAND }, { "srand", TK_BUILTIN, B_SRAND }, { "tolower", TK_BUILTIN, B_TOLOWER },
    { "toupper", TK_BUILTIN, B_TOUPPER }, { "system", TK_BUILTIN, B_SYSTEM }, { "close", TK_BUILTIN, B_CLOSE },
    { "fflush", TK_BUILTIN, B_FFLUSH }, { "systime", TK_BUILTIN, B_SYSTIME }, { "strftime", TK_BUILTIN, B_STRFTIME },
};

static int is_name_start(int c) { return isalpha(c) || c == '_'; }
static int is_name_char(int c) { return isalnum(c) || c == '_'; }

/* Reads the escapes of a string literal or of a command-line value into a new string. */
static char *unescape_str(const char *p, const char *end, size_t *outlen) {
    Buf b = { 0 };
    size_t n = (size_t)(end - p);
    size_t i = 0;
    while (i < n) {
        if (p[i] == '\\' && i + 1 < n) {
            i++;
            if (p[i] == '\n') { i++; continue; }
            int d = decode_escape(p, n, &i);
            if (d < 0) { buf_addc(&b, '\\'); buf_addc(&b, p[i]); i++; }
            else buf_addc(&b, (char)d);
        } else {
            buf_addc(&b, p[i++]);
        }
    }
    if (!b.p) buf_add(&b, "", 0);
    if (outlen) *outlen = b.len;
    return b.p;
}

static void lex_next(void) {
    const char *p = lx.p;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\\' && p[1] == '\n') { p += 2; lx.line++; continue; }
        if (*p == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; lx.line++; continue; }
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    lx.tstart = p;
    g_lexline = lx.line;
    char c = *p;
    if (!c) { lx.tok = TK_EOF; lx.p = p; return; }
    if (c == '\n') { lx.tok = TK_NL; lx.p = p + 1; lx.line++; return; }
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p[1]))) {
        if (c == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
            const char *q = p + 2;
            double v = 0;
            while (isxdigit((unsigned char)*q)) { v = v * 16 + (isdigit((unsigned char)*q) ? *q - '0' : (tolower(*q) - 'a' + 10)); q++; }
            lx.num = v;
            lx.p = q;
        } else {
            double v = 0;
            const char *q = scan_num(p, p + strlen(p), &v);
            lx.num = v;
            lx.p = q;
        }
        lx.tok = TK_NUM;
        return;
    }
    if (is_name_start((unsigned char)c)) {
        const char *q = p;
        while (is_name_char((unsigned char)*q)) q++;
        size_t n = (size_t)(q - p);
        lx.p = q;
        for (unsigned i = 0; i < sizeof g_keywords / sizeof g_keywords[0]; i++) {
            if (strlen(g_keywords[i].name) == n && !memcmp(g_keywords[i].name, p, n)) {
                lx.tok = g_keywords[i].tok;
                lx.bi = g_keywords[i].bi;
                return;
            }
        }
        lx.str = xstrndup(p, n);
        lx.slen = n;
        lx.tok = *q == '(' ? TK_FNAME : TK_NAME;
        return;
    }
    if (c == '"') {
        const char *q = p + 1;
        while (*q != '"') {
            if (!*q || *q == '\n') { lx.line = g_lexline; fatal("non-terminated string %.10s... at source line %d", p, lx.line); }
            if (*q == '\\' && q[1]) q++;
            q++;
        }
        lx.str = unescape_str(p + 1, q, &lx.slen);
        lx.p = q + 1;
        for (const char *z = p; z < q; z++) if (*z == '\n') lx.line++;
        lx.tok = TK_STR;
        return;
    }
    lx.p = p + 1;
    char d = p[1];
    switch (c) {
    case '{': lx.tok = TK_LBRACE; return;
    case '}': lx.tok = TK_RBRACE; return;
    case '(': lx.tok = TK_LPAREN; return;
    case ')': lx.tok = TK_RPAREN; return;
    case '[': lx.tok = TK_LBRACKET; return;
    case ']': lx.tok = TK_RBRACKET; return;
    case ';': lx.tok = TK_SEMI; return;
    case ',': lx.tok = TK_COMMA; return;
    case '?': lx.tok = TK_QUESTION; return;
    case ':': lx.tok = TK_COLON; return;
    case '~': lx.tok = TK_TILDE; return;
    case '$': lx.tok = TK_DOLLAR; return;
    case '+':
        if (d == '+') { lx.p++; lx.tok = TK_INCR; } else if (d == '=') { lx.p++; lx.tok = TK_ADDASSIGN; } else lx.tok = TK_PLUS;
        return;
    case '-':
        if (d == '-') { lx.p++; lx.tok = TK_DECR; } else if (d == '=') { lx.p++; lx.tok = TK_SUBASSIGN; } else lx.tok = TK_MINUS;
        return;
    case '*':
        if (d == '*') {
            lx.p++;
            if (p[2] == '=') { lx.p++; lx.tok = TK_POWASSIGN; } else lx.tok = TK_CARET;
        } else if (d == '=') { lx.p++; lx.tok = TK_MULASSIGN; } else lx.tok = TK_STAR;
        return;
    case '/':
        if (d == '=') { lx.p++; lx.tok = TK_DIVASSIGN; } else lx.tok = TK_SLASH;
        return;
    case '%':
        if (d == '=') { lx.p++; lx.tok = TK_MODASSIGN; } else lx.tok = TK_PERCENT;
        return;
    case '^':
        if (d == '=') { lx.p++; lx.tok = TK_POWASSIGN; } else lx.tok = TK_CARET;
        return;
    case '!':
        if (d == '=') { lx.p++; lx.tok = TK_NE; } else if (d == '~') { lx.p++; lx.tok = TK_NOMATCH; } else lx.tok = TK_NOT;
        return;
    case '=':
        if (d == '=') { lx.p++; lx.tok = TK_EQ; } else lx.tok = TK_ASSIGN;
        return;
    case '<':
        if (d == '=') { lx.p++; lx.tok = TK_LE; } else lx.tok = TK_LT;
        return;
    case '>':
        if (d == '=') { lx.p++; lx.tok = TK_GE; } else if (d == '>') { lx.p++; lx.tok = TK_APPEND; } else lx.tok = TK_GT;
        return;
    case '&':
        if (d == '&') { lx.p++; lx.tok = TK_AND; return; }
        break;
    case '|':
        if (d == '|') { lx.p++; lx.tok = TK_OR; } else lx.tok = TK_PIPE;
        return;
    }
    syntax();
}

/* The current token is a / or /=: rescan from its start as a regular expression literal. */
static void lex_regex(void) {
    const char *p = lx.tstart + 1;
    Buf b = { 0 };
    int in_br = 0;
    while (*p && *p != '\n') {
        if (*p == '\\' && p[1] && p[1] != '\n') {
            if (p[1] == '/') buf_addc(&b, '/'); else { buf_addc(&b, '\\'); buf_addc(&b, p[1]); }
            p += 2;
            continue;
        }
        if (!in_br && *p == '[') {
            in_br = 1;
            buf_addc(&b, *p++);
            if (*p == '^') buf_addc(&b, *p++);
            if (*p == ']') buf_addc(&b, *p++);
            continue;
        }
        if (in_br && *p == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e && !memchr(p, '\n', (size_t)(e - p))) { buf_add(&b, p, (size_t)(e - p) + 2); p = e + 2; continue; }
        }
        if (in_br && *p == ']') { in_br = 0; buf_addc(&b, *p++); continue; }
        if (!in_br && *p == '/') break;
        buf_addc(&b, *p++);
    }
    if (*p != '/') fatal("non-terminated regular expression %.10s... at source line %d", lx.tstart, lx.line);
    lx.p = p + 1;
    if (!b.p) buf_add(&b, "", 0);
    lx.str = b.p;
    lx.slen = b.len;
    lx.tok = TK_ERE;
}

static int next_is_getline(void) {
    const char *p = lx.p;
    while (*p == ' ' || *p == '\t') p++;
    return !strncmp(p, "getline", 7) && !is_name_char((unsigned char)p[7]);
}

/* ------------------------------------------------------------------ parser */

static int g_no_gt;                    /* inside a print list: > is a redirection, not a comparison */
static char **g_lnames;
static int g_nloc;

static void advance(void) { lex_next(); }
static void expect(int tok) { if (lx.tok != tok) syntax(); advance(); }
static void opt_nl(void) { while (lx.tok == TK_NL) advance(); }
static int is_stmt_end(int t) { return t == TK_NL || t == TK_SEMI || t == TK_RBRACE || t == TK_EOF; }
static void end_stmt(void) {
    if (lx.tok == TK_SEMI || lx.tok == TK_NL) { advance(); return; }
    if (lx.tok == TK_RBRACE || lx.tok == TK_EOF) return;
    syntax();
}

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_concat(void);
static Node *parse_primary(void);
static Node *parse_unary(void);

static Node *var_node(const char *name, size_t n) {
    Node *v = mk(N_VAR);
    for (int i = 0; i < g_nloc; i++) {
        if (!strncmp(g_lnames[i], name, n) && !g_lnames[i][n]) { v->scope = 1; v->idx = i; return v; }
    }
    v->scope = 0;
    v->idx = global_index(name, n);
    return v;
}

static int is_lvalue(Node *n) { return n->type == N_VAR || n->type == N_ELEM || n->type == N_FIELD; }

static Node *parse_subscripts(int *count) {
    Node *head = NULL, *tail = NULL;
    int n = 0, save = g_no_gt;
    g_no_gt = 0;
    for (;;) {
        Node *e = parse_expr();
        if (tail) tail->next = e; else head = e;
        tail = e;
        n++;
        if (lx.tok == TK_COMMA) { advance(); opt_nl(); continue; }
        break;
    }
    expect(TK_RBRACKET);
    g_no_gt = save;
    *count = n;
    return head;
}

static Node *parse_dollar_operand(void) {
    if (lx.tok == TK_INCR || lx.tok == TK_DECR) {
        int delta = lx.tok == TK_INCR ? 1 : -1;
        advance();
        Node *lv = parse_dollar_operand();
        if (!is_lvalue(lv)) syntax();
        Node *n = mk1(N_INCR, lv);
        n->idx = delta;
        n->scope = 0;
        return n;
    }
    if (lx.tok == TK_MINUS) { advance(); return mk1(N_NEG, parse_dollar_operand()); }
    if (lx.tok == TK_PLUS) { advance(); return mk1(N_PLUS, parse_dollar_operand()); }
    if (lx.tok == TK_NOT) { advance(); return mk1(N_NOT, parse_dollar_operand()); }
    return parse_primary();
}

static Node *parse_lvalue_opt(void) {
    if (lx.tok == TK_NAME) {
        Node *v = var_node(lx.str, lx.slen);
        advance();
        if (lx.tok == TK_LBRACKET) {
            advance();
            Node *e = mk(N_ELEM);
            e->a = v;
            e->b = parse_subscripts(&e->nargs);
            return e;
        }
        return v;
    }
    if (lx.tok == TK_DOLLAR) { advance(); return mk1(N_FIELD, parse_dollar_operand()); }
    return NULL;
}

static Node *parse_args(Node *call, int *count) {
    Node *head = NULL, *tail = NULL;
    int n = 0, save = g_no_gt;
    g_no_gt = 0;
    opt_nl();
    if (lx.tok != TK_RPAREN) {
        for (;;) {
            Node *e = parse_expr();
            if (tail) tail->next = e; else head = e;
            tail = e;
            n++;
            opt_nl();
            if (lx.tok == TK_COMMA) { advance(); opt_nl(); continue; }
            break;
        }
    }
    expect(TK_RPAREN);
    g_no_gt = save;
    *count = n;
    (void)call;
    return head;
}

static const struct { int bi, min, max; } g_arity[] = {
    [B_LENGTH] = { B_LENGTH, 0, 1 }, [B_SUBSTR] = { B_SUBSTR, 2, 3 }, [B_INDEX] = { B_INDEX, 2, 2 }, [B_SPLIT] = { B_SPLIT, 2, 3 },
    [B_SUB] = { B_SUB, 2, 3 }, [B_GSUB] = { B_GSUB, 2, 3 }, [B_MATCH] = { B_MATCH, 2, 2 }, [B_SPRINTF] = { B_SPRINTF, 1, 1000 },
    [B_SIN] = { B_SIN, 1, 1 }, [B_COS] = { B_COS, 1, 1 }, [B_ATAN2] = { B_ATAN2, 2, 2 }, [B_EXP] = { B_EXP, 1, 1 },
    [B_LOG] = { B_LOG, 1, 1 }, [B_SQRT] = { B_SQRT, 1, 1 }, [B_INT] = { B_INT, 1, 1 }, [B_RAND] = { B_RAND, 0, 0 },
    [B_SRAND] = { B_SRAND, 0, 1 }, [B_TOLOWER] = { B_TOLOWER, 1, 1 }, [B_TOUPPER] = { B_TOUPPER, 1, 1 },
    [B_SYSTEM] = { B_SYSTEM, 1, 1 }, [B_CLOSE] = { B_CLOSE, 1, 2 }, [B_FFLUSH] = { B_FFLUSH, 0, 1 }, [B_SYSTIME] = { B_SYSTIME, 0, 0 },
    [B_STRFTIME] = { B_STRFTIME, 0, 3 },
};

static Node *parse_primary(void) {
    Node *n;
    switch (lx.tok) {
    case TK_NUM:
        n = mk(N_NUM);
        n->num = lx.num;
        advance();
        return n;
    case TK_STR:
        n = mk(N_STR);
        n->str = lx.str;
        n->slen = lx.slen;
        advance();
        return n;
    case TK_SLASH: case TK_DIVASSIGN:
        lex_regex();
        n = mk(N_REGEX);
        n->re = regex_compile(lx.str, lx.slen);
        advance();
        return n;
    case TK_LPAREN: {
        int save = g_no_gt;
        g_no_gt = 0;
        advance();
        opt_nl();
        Node *e = parse_expr();
        opt_nl();
        if (lx.tok == TK_COMMA) {
            Node *list = mk(N_GROUPLIST), *tail = e;
            list->a = e;
            list->nargs = 1;
            while (lx.tok == TK_COMMA) {
                advance();
                opt_nl();
                Node *x = parse_expr();
                opt_nl();
                tail->next = x;
                tail = x;
                list->nargs++;
            }
            expect(TK_RPAREN);
            g_no_gt = save;
            return list;
        }
        expect(TK_RPAREN);
        g_no_gt = save;
        return mk1(N_GROUP, e);
    }
    case TK_DOLLAR:
        advance();
        return mk1(N_FIELD, parse_dollar_operand());
    case TK_NAME: {
        Node *v = var_node(lx.str, lx.slen);
        advance();
        if (lx.tok == TK_LBRACKET) {
            advance();
            n = mk(N_ELEM);
            n->a = v;
            n->b = parse_subscripts(&n->nargs);
            return n;
        }
        return v;
    }
    case TK_FNAME: {
        n = mk(N_CALL);
        n->idx = func_index(lx.str, lx.slen);
        advance();
        expect(TK_LPAREN);
        n->a = parse_args(n, &n->nargs);
        return n;
    }
    case TK_BUILTIN: {
        int bi = lx.bi;
        n = mk(N_BUILTIN);
        n->idx = bi;
        advance();
        if (lx.tok == TK_LPAREN) {
            advance();
            n->a = parse_args(n, &n->nargs);
        } else if (bi != B_LENGTH) {
            syntax();
        }
        if (n->nargs < g_arity[bi].min || n->nargs > g_arity[bi].max) syntax();
        return n;
    }
    case TK_GETLINE: {
        advance();
        n = mk(N_GETLINE);
        n->a = parse_lvalue_opt();
        if (lx.tok == TK_LT) {
            advance();
            n->idx = 1;
            n->b = parse_primary();
        }
        return n;
    }
    default:
        syntax();
    }
    return NULL;
}

static Node *parse_incdec(void) {
    if (lx.tok == TK_INCR || lx.tok == TK_DECR) {
        int delta = lx.tok == TK_INCR ? 1 : -1;
        advance();
        Node *lv = parse_primary();
        if (!is_lvalue(lv)) syntax();
        Node *n = mk1(N_INCR, lv);
        n->idx = delta;
        return n;
    }
    Node *e = parse_primary();
    if ((lx.tok == TK_INCR || lx.tok == TK_DECR) && is_lvalue(e)) {
        Node *n = mk1(N_INCR, e);
        n->idx = lx.tok == TK_INCR ? 1 : -1;
        n->scope = 1;                  /* postfix */
        advance();
        return n;
    }
    return e;
}

static Node *parse_pow(void) {
    Node *base = parse_incdec();
    if (lx.tok == TK_CARET) {
        advance();
        Node *exp = parse_unary();     /* right associative; the exponent may carry a sign */
        Node *n = mk2(N_BINOP, base, exp);
        n->idx = '^';
        return n;
    }
    return base;
}

static Node *parse_unary(void) {
    if (lx.tok == TK_NOT) { advance(); return mk1(N_NOT, parse_unary()); }
    if (lx.tok == TK_MINUS) { advance(); return mk1(N_NEG, parse_unary()); }
    if (lx.tok == TK_PLUS) { advance(); return mk1(N_PLUS, parse_unary()); }
    return parse_pow();
}

static Node *parse_mul(void) {
    Node *l = parse_unary();
    while (lx.tok == TK_STAR || lx.tok == TK_SLASH || lx.tok == TK_PERCENT) {
        int op = lx.tok == TK_STAR ? '*' : lx.tok == TK_SLASH ? '/' : '%';
        advance();
        Node *r = parse_unary();
        l = mk2(N_BINOP, l, r);
        l->idx = op;
    }
    return l;
}

static Node *parse_additive(void) {
    Node *l = parse_mul();
    while (lx.tok == TK_PLUS || lx.tok == TK_MINUS) {
        int op = lx.tok == TK_PLUS ? '+' : '-';
        advance();
        Node *r = parse_mul();
        l = mk2(N_BINOP, l, r);
        l->idx = op;
    }
    return l;
}

static int starts_operand(int t) {
    switch (t) {
    case TK_NUM: case TK_STR: case TK_NAME: case TK_FNAME: case TK_BUILTIN: case TK_DOLLAR: case TK_LPAREN: case TK_INCR:
    case TK_DECR: case TK_NOT: case TK_MINUS: case TK_PLUS:
        return t != TK_NOT && t != TK_MINUS && t != TK_PLUS;
    }
    return 0;
}

static Node *parse_concat(void) {
    Node *l = parse_additive();
    while (starts_operand(lx.tok)) {
        Node *r = parse_additive();
        l = mk2(N_CONCAT, l, r);
    }
    return l;
}

static Node *parse_cmp(void) {
    Node *l = parse_concat();
    for (;;) {
        if (lx.tok == TK_PIPE && next_is_getline()) {
            advance();
            advance();
            Node *g = mk(N_GETLINE);
            g->idx = 2;
            g->b = l;
            g->a = parse_lvalue_opt();
            l = g;
            continue;
        }
        break;
    }
    int op = 0;
    switch (lx.tok) {
    case TK_LT: case TK_LE: case TK_NE: case TK_EQ: case TK_GE: op = lx.tok; break;
    case TK_GT: if (!g_no_gt) op = lx.tok; break;
    }
    if (op) {
        advance();
        Node *r = parse_concat();
        Node *n = mk2(N_CMP, l, r);
        n->idx = op;
        return n;
    }
    return l;
}

static Node *parse_match(void) {
    Node *l = parse_cmp();
    while (lx.tok == TK_TILDE || lx.tok == TK_NOMATCH) {
        int neg = lx.tok == TK_NOMATCH;
        advance();
        Node *r = parse_cmp();
        l = mk2(N_MATCH, l, r);
        l->idx = neg;
    }
    return l;
}

static Node *parse_in(void) {
    Node *l = parse_match();
    while (lx.tok == TK_IN) {
        advance();
        if (lx.tok != TK_NAME) syntax();
        Node *arr = var_node(lx.str, lx.slen);
        advance();
        Node *n = mk(N_IN);
        if (l->type == N_GROUPLIST) { n->b = l->a; n->nargs = l->nargs; }
        else { n->b = l; n->nargs = 1; }
        n->a = arr;
        l = n;
    }
    if (l->type == N_GROUPLIST) syntax();
    return l;
}

static Node *parse_and(void) {
    Node *l = parse_in();
    while (lx.tok == TK_AND) {
        advance();
        opt_nl();
        l = mk2(N_AND, l, parse_in());
    }
    return l;
}

static Node *parse_or(void) {
    Node *l = parse_and();
    while (lx.tok == TK_OR) {
        advance();
        opt_nl();
        l = mk2(N_OR, l, parse_and());
    }
    return l;
}

static Node *parse_ternary(void) {
    Node *c = parse_or();
    if (lx.tok == TK_QUESTION) {
        advance();
        opt_nl();
        Node *a = parse_expr();
        opt_nl();
        expect(TK_COLON);
        opt_nl();
        Node *b = parse_expr();
        Node *n = mk(N_COND);
        n->a = c;
        n->b = a;
        n->c = b;
        return n;
    }
    return c;
}

static Node *parse_expr(void) {
    Node *l = parse_ternary();
    switch (lx.tok) {
    case TK_ASSIGN: case TK_ADDASSIGN: case TK_SUBASSIGN: case TK_MULASSIGN: case TK_DIVASSIGN: case TK_MODASSIGN: case TK_POWASSIGN:
        if (is_lvalue(l)) {
            int tk = lx.tok;
            advance();
            opt_nl();
            Node *r = parse_expr();
            Node *n = mk2(N_ASSIGN, l, r);
            n->idx = tk;
            return n;
        }
    }
    return l;
}

/* print / printf */
static Node *parse_print(void) {
    int is_printf = lx.tok == TK_PRINTF;
    Node *n = mk(is_printf ? N_PRINTF : N_PRINT);
    advance();
    Node *head = NULL, *tail = NULL;
    int count = 0;
    int ends = 0;
    if (lx.tok == TK_LPAREN) {                      /* try print (a, b) > "file" */
        Lex save = lx;
        int save_gt = g_no_gt;
        g_no_gt = 0;
        advance();
        opt_nl();
        Node *first = parse_expr();
        opt_nl();
        Node *h = first, *t = first;
        int c = 1;
        while (lx.tok == TK_COMMA) {
            advance();
            opt_nl();
            Node *x = parse_expr();
            opt_nl();
            t->next = x;
            t = x;
            c++;
        }
        g_no_gt = save_gt;
        if (lx.tok == TK_RPAREN) {
            advance();
            if (is_stmt_end(lx.tok) || lx.tok == TK_GT || lx.tok == TK_APPEND || lx.tok == TK_PIPE) {
                head = h;
                count = c;
                ends = 1;
            }
        }
        if (!ends) lx = save;
    }
    if (!ends && !(is_stmt_end(lx.tok) || lx.tok == TK_GT || lx.tok == TK_APPEND || lx.tok == TK_PIPE)) {
        g_no_gt = 1;
        for (;;) {
            Node *e = parse_expr();
            if (tail) tail->next = e; else head = e;
            tail = e;
            count++;
            if (lx.tok == TK_COMMA) { advance(); opt_nl(); continue; }
            break;
        }
        g_no_gt = 0;
    }
    n->a = head;
    n->nargs = count;
    if (lx.tok == TK_GT || lx.tok == TK_APPEND || lx.tok == TK_PIPE) {
        n->idx = lx.tok == TK_GT ? 1 : lx.tok == TK_APPEND ? 2 : 3;
        advance();
        n->b = parse_concat();
    }
    if (is_printf && !head) syntax();
    return n;
}

static Node *parse_simple(void) {
    Node *n;
    switch (lx.tok) {
    case TK_PRINT: case TK_PRINTF: return parse_print();
    case TK_NEXT: advance(); return mk(N_NEXT);
    case TK_NEXTFILE: advance(); return mk(N_NEXTFILE);
    case TK_BREAK: advance(); return mk(N_BREAK);
    case TK_CONTINUE: advance(); return mk(N_CONTINUE);
    case TK_EXIT:
        advance();
        n = mk(N_EXIT);
        if (!is_stmt_end(lx.tok)) n->a = parse_expr();
        return n;
    case TK_RETURN:
        if (!g_curfunc) fatal("return not in function at source line %d", lx.line);
        advance();
        n = mk(N_RETURN);
        if (!is_stmt_end(lx.tok)) n->a = parse_expr();
        return n;
    case TK_DELETE: {
        advance();
        if (lx.tok != TK_NAME) syntax();
        n = mk(N_DELETE);
        n->a = var_node(lx.str, lx.slen);
        advance();
        if (lx.tok == TK_LBRACKET) {
            advance();
            n->b = parse_subscripts(&n->nargs);
        }
        return n;
    }
    default:
        return mk1(N_EXPR, parse_expr());
    }
}

static Node *parse_block(void) {
    expect(TK_LBRACE);
    Node *blk = mk(N_BLOCK), *tail = NULL;
    for (;;) {
        while (lx.tok == TK_NL || lx.tok == TK_SEMI) advance();
        if (lx.tok == TK_RBRACE) break;
        if (lx.tok == TK_EOF) syntax();
        Node *s = parse_stmt();
        if (tail) tail->next = s; else blk->a = s;
        tail = s;
    }
    advance();
    return blk;
}

static Node *parse_stmt(void) {
    Node *n;
    switch (lx.tok) {
    case TK_LBRACE: return parse_block();
    case TK_SEMI: advance(); return mk(N_BLOCK);
    case TK_IF: {
        advance();
        expect(TK_LPAREN);
        n = mk(N_IF);
        n->a = parse_expr();
        expect(TK_RPAREN);
        opt_nl();
        n->b = parse_stmt();
        Lex save = lx;
        while (lx.tok == TK_NL || lx.tok == TK_SEMI) advance();
        if (lx.tok == TK_ELSE) { advance(); opt_nl(); n->c = parse_stmt(); }
        else lx = save;
        return n;
    }
    case TK_WHILE: {
        advance();
        expect(TK_LPAREN);
        n = mk(N_WHILE);
        n->a = parse_expr();
        expect(TK_RPAREN);
        if (lx.tok == TK_SEMI) { advance(); n->b = mk(N_BLOCK); return n; }
        opt_nl();
        n->b = parse_stmt();
        return n;
    }
    case TK_DO: {
        advance();
        opt_nl();
        n = mk(N_DO);
        n->a = parse_stmt();
        while (lx.tok == TK_NL || lx.tok == TK_SEMI) advance();
        if (lx.tok != TK_WHILE) syntax();
        advance();
        expect(TK_LPAREN);
        n->b = parse_expr();
        expect(TK_RPAREN);
        end_stmt();
        return n;
    }
    case TK_FOR: {
        advance();
        expect(TK_LPAREN);
        Lex save = lx;
        if (lx.tok == TK_NAME) {
            Node *v = var_node(lx.str, lx.slen);
            advance();
            if (lx.tok == TK_IN) {
                advance();
                if (lx.tok == TK_NAME) {
                    Node *arr = var_node(lx.str, lx.slen);
                    advance();
                    if (lx.tok == TK_RPAREN) {
                        advance();
                        opt_nl();
                        n = mk(N_FORIN);
                        n->a = v;
                        n->b = arr;
                        n->c = parse_stmt();
                        return n;
                    }
                }
            }
        }
        lx = save;
        n = mk(N_FOR);
        if (lx.tok != TK_SEMI) n->a = parse_simple();
        expect(TK_SEMI);
        opt_nl();
        if (lx.tok != TK_SEMI) n->b = parse_expr();
        expect(TK_SEMI);
        opt_nl();
        if (lx.tok != TK_RPAREN) n->c = parse_simple();
        expect(TK_RPAREN);
        if (lx.tok == TK_SEMI) { advance(); n->d = mk(N_BLOCK); return n; }
        opt_nl();
        n->d = parse_stmt();
        return n;
    }
    default:
        n = parse_simple();
        end_stmt();
        return n;
    }
}

static void parse_function(void) {
    advance();
    if (lx.tok != TK_NAME && lx.tok != TK_FNAME) syntax();
    int fi = func_index(lx.str, lx.slen);
    Func *f = &g_funcs[fi];
    if (f->defined) fatal("function %s redefined at source line %d", f->name, lx.line);
    advance();
    expect(TK_LPAREN);
    g_nloc = 0;
    g_lnames = NULL;
    opt_nl();
    while (lx.tok == TK_NAME) {
        g_lnames = xrealloc(g_lnames, (size_t)(g_nloc + 1) * sizeof(char *));
        g_lnames[g_nloc++] = (char *)lx.str;
        advance();
        opt_nl();
        if (lx.tok == TK_COMMA) { advance(); opt_nl(); } else break;
    }
    expect(TK_RPAREN);
    opt_nl();
    f = &g_funcs[fi];
    f->np = g_nloc;
    f->params = g_lnames;
    f->defined = 1;
    g_curfunc = f;
    Node *body = parse_block();
    f = &g_funcs[fi];
    f->body = body;
    g_curfunc = NULL;
    g_nloc = 0;
    g_lnames = NULL;
}

static void parse_program(const char *src) {
    memset(&lx, 0, sizeof lx);
    lx.p = src;
    lx.line = 1;
    for (int i = 0; i < NSPECIAL; i++) global_index(g_special_names[i], strlen(g_special_names[i]));
    advance();
    for (;;) {
        while (lx.tok == TK_NL || lx.tok == TK_SEMI) advance();
        if (lx.tok == TK_EOF) break;
        if (lx.tok == TK_FUNCTION) { parse_function(); continue; }
        if (lx.tok == TK_BEGIN || lx.tok == TK_END) {
            int is_begin = lx.tok == TK_BEGIN;
            advance();
            if (lx.tok != TK_LBRACE) syntax();
            Node *blk = parse_block();
            if (is_begin) { g_begins = xrealloc(g_begins, (size_t)(g_nbegin + 1) * sizeof(Node *)); g_begins[g_nbegin++] = blk; }
            else { g_ends = xrealloc(g_ends, (size_t)(g_nend + 1) * sizeof(Node *)); g_ends[g_nend++] = blk; }
            continue;
        }
        Rule r;
        memset(&r, 0, sizeof r);
        if (lx.tok != TK_LBRACE) {
            r.p1 = parse_expr();
            if (lx.tok == TK_COMMA) { advance(); opt_nl(); r.p2 = parse_expr(); }
        }
        if (lx.tok == TK_LBRACE) r.action = parse_block();
        else if (!is_stmt_end(lx.tok)) syntax();
        if (g_nrules == g_rulecap) { g_rulecap = g_rulecap ? g_rulecap * 2 : 16; g_rules = xrealloc(g_rules, (size_t)g_rulecap * sizeof(Rule)); }
        g_rules[g_nrules++] = r;
    }
    for (int i = 0; i < g_nfuncs; i++)
        if (!g_funcs[i].defined) fatal("calling undefined function %s", g_funcs[i].name);
}

/* ------------------------------------------------------------------ runtime state */

enum { F_NORMAL, F_BREAK, F_CONTINUE, F_NEXT, F_NEXTFILE, F_EXIT, F_RETURN };

static Cell *g_cells;
typedef struct { Cell *loc; int n; } Frame;
static Frame *g_frame;
static int g_depth;
static char *g_stack_base;
static double g_nr, g_fnr;
static int g_nf;

typedef struct { char *s; size_t len; } Str;
static Str g_fs, g_ofs, g_ors, g_rs, g_subsep;
static Regex *g_fs_re, *g_rs_re;
static int g_exit_status, g_exit_set, g_errstatus;
static jmp_buf g_jmp;
static int g_pending;
static Cell g_retcell;

static void set_nf(long n);
static void ensure_split(void);

static void str_assign(Str *d, Val v) {
    v = tostr(v);
    free(d->s);
    d->s = xstrndup(v.s, v.len);
    d->len = v.len;
}

static void special_changed(int idx) {
    Cell *c = &g_cells[idx];
    Val v = cell_val(c);
    switch (idx) {
    case V_FS: str_assign(&g_fs, v); g_fs_re = NULL; break;
    case V_OFS: str_assign(&g_ofs, v); break;
    case V_ORS: str_assign(&g_ors, v); break;
    case V_RS: str_assign(&g_rs, v); g_rs_re = NULL; g_fs_re = NULL; break;
    case V_SUBSEP: str_assign(&g_subsep, v); break;
    case V_CONVFMT: { Str t = { 0 }; str_assign(&t, v); g_convfmt = t.s; break; }
    case V_OFMT: { Str t = { 0 }; str_assign(&t, v); g_ofmt = t.s; break; }
    case V_NR: g_nr = num_of(v); break;
    case V_FNR: g_fnr = num_of(v); break;
    case V_NF: set_nf((long)num_of(v)); break;
    }
}

static Cell *var_cell(int scope, int idx) { return scope ? &g_frame->loc[idx] : &g_cells[idx]; }

static Val var_get(int scope, int idx) {
    if (!scope && idx < NSPECIAL) {
        if (idx == V_NF) { ensure_split(); return vnum(g_nf); }
        if (idx == V_NR) return vnum(g_nr);
        if (idx == V_FNR) return vnum(g_fnr);
    }
    return cell_val(var_cell(scope, idx));
}

static void var_set(int scope, int idx, Val v) {
    Cell *c = var_cell(scope, idx);
    if (!scope && idx < NSPECIAL) {
        if (idx == V_NF) { set_nf((long)num_of(v)); return; }
        if (idx == V_ENVIRON || idx == V_ARGV) fatal("can't assign to %s; it's an array name.", g_special_names[idx]);
        cell_set(c, v);
        special_changed(idx);
        return;
    }
    cell_set(c, v);
}

static Array *get_array(Node *v) {
    Cell *c = var_cell(v->scope, v->idx);
    if (!c->arr) {
        if (c->v.t != T_UNINIT) fatal("can't use a scalar as an array");
        c->arr = arr_new();
    }
    return c->arr;
}

/* ------------------------------------------------------------------ records and fields */

typedef struct { const char *s; size_t len; int t; double n; char *own; size_t owncap; } Fld;
static Fld *g_fld;
static int g_fldcap;
static char *g_rec;
static size_t g_reclen, g_reccap;
static int g_rec_valid = 1, g_split_done = 1;

typedef struct { size_t off, len; } Span;
static Span *g_sp;
static size_t g_spcap;

static void sp_add(int n, size_t off, size_t len) {
    if ((size_t)n >= g_spcap) { g_spcap = g_spcap ? g_spcap * 2 : 64; g_sp = xrealloc(g_sp, g_spcap * sizeof(Span)); }
    g_sp[n].off = off;
    g_sp[n].len = len;
}

/* Splits s into g_sp[]; re is the compiled separator when it is longer than one character. */
static int do_split(const char *s, size_t len, const char *fs, size_t fslen, Regex *re, int para) {
    int n = 0;
    size_t i = 0;
    if (!re && fslen == 1 && fs[0] == ' ') {
        for (;;) {
            while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
            if (i >= len) break;
            size_t st = i;
            while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') i++;
            sp_add(n++, st, i - st);
        }
    } else if (!re && fslen == 0) {
        for (i = 0; i < len; i++) sp_add(n++, i, 1);
    } else if (!re) {
        char c = fs[0];
        if (len == 0) return 0;
        size_t st = 0;
        if (!para) {
            const char *q;
            while (st <= len && (q = memchr(s + st, c, len - st))) {
                sp_add(n++, st, (size_t)(q - s) - st);
                st = (size_t)(q - s) + 1;
            }
        } else {
            for (i = 0; i < len; i++) if (s[i] == c || s[i] == '\n') { sp_add(n++, st, i - st); st = i + 1; }
        }
        sp_add(n++, st, len - st);
    } else {
        if (len == 0) return 0;
        size_t st = 0, pos = 0, ms, me;
        while (pos <= len && re_search(re, s, len, pos, &ms, &me)) {
            if (me == ms) { pos = ms + 1; continue; }
            sp_add(n++, st, ms - st);
            st = me;
            pos = me;
        }
        sp_add(n++, st, len - st);
    }
    return n;
}

static void fld_reserve(int n) {
    if (n > g_fldcap) {
        int nc = g_fldcap ? g_fldcap : 32;
        while (nc < n) nc *= 2;
        g_fld = xrealloc(g_fld, (size_t)nc * sizeof(Fld));
        memset(g_fld + g_fldcap, 0, (size_t)(nc - g_fldcap) * sizeof(Fld));
        g_fldcap = nc;
    }
}

static void split_record(void) {
    int para = g_rs.len == 0;
    Regex *re = NULL;
    g_split_done = 1;
    if (g_fs.len > 1 || (para && g_fs.len > 1)) {
        if (!g_fs_re) {
            if (para) {
                Buf b = { 0 };
                buf_addc(&b, '(');
                buf_add(&b, g_fs.s, g_fs.len);
                buf_add(&b, ")|\n", 3);
                g_fs_re = regex_compile(b.p, b.len);
                free(b.p);
            } else {
                g_fs_re = regex_compile(g_fs.s, g_fs.len);
            }
        }
        re = g_fs_re;
    }
    int n = do_split(g_rec, g_reclen, g_fs.s, g_fs.len, re, para);
    fld_reserve(n);
    for (int i = 0; i < n; i++) {
        g_fld[i].s = g_rec + g_sp[i].off;
        g_fld[i].len = g_sp[i].len;
        g_fld[i].t = T_INPUT;
    }
    g_nf = n;
}

static void ensure_split(void) { if (!g_split_done) split_record(); }

static void set_record(const char *s, size_t len) {
    if (len + 1 > g_reccap) {
        size_t nc = (len + 256) & ~(size_t)255;
        char *nb = xmalloc(nc);
        memcpy(nb, s, len);
        free(g_rec);
        g_rec = nb;
        g_reccap = nc;
    } else {
        memmove(g_rec, s, len);
    }
    g_rec[len] = 0;
    g_reclen = len;
    g_rec_valid = 1;
    g_split_done = 0;
}

static void rebuild_record(void) {
    Buf b = { 0 };
    size_t total = 0;
    for (int i = 0; i < g_nf; i++) total += g_fld[i].len + g_ofs.len;
    buf_grow(&b, total);
    size_t *offs = (size_t *)talloc((size_t)(g_nf + 1) * sizeof(size_t));
    for (int i = 0; i < g_nf; i++) {
        if (i) buf_add(&b, g_ofs.s, g_ofs.len);
        offs[i] = b.len;
        buf_add(&b, g_fld[i].s, g_fld[i].len);
    }
    if (!b.p) buf_add(&b, "", 0);
    free(g_rec);
    g_rec = b.p;
    g_reclen = b.len;
    g_reccap = b.cap;
    for (int i = 0; i < g_nf; i++) g_fld[i].s = g_rec + offs[i];
    g_rec_valid = 1;
}

static Val get_field(long i) {
    if (i == 0) {
        if (!g_rec_valid) rebuild_record();
        Val v = { T_INPUT, 0, g_rec ? g_rec : "", g_reclen };
        return v;
    }
    if (i < 0) fatal("trying to access out of range field %ld", i);
    ensure_split();
    if (i > g_nf) return V_UNINIT;
    Fld *f = &g_fld[i - 1];
    Val v = { f->t, f->n, f->s, f->len };
    return v;
}

static void extend_fields(long n) {
    if (n > 10000000) fatal("cannot create field %ld", n);
    fld_reserve((int)n);
    for (int i = g_nf; i < n; i++) { g_fld[i].s = ""; g_fld[i].len = 0; g_fld[i].t = T_INPUT; g_fld[i].n = 0; }
    g_nf = (int)n;
}

static void set_nf(long n) {
    ensure_split();
    if (n < 0) fatal("cannot set NF to a negative value");
    if (n > g_nf) extend_fields(n);
    else g_nf = (int)n;
    g_rec_valid = 0;
}

static void set_field(long i, Val v) {
    if (i == 0) {
        Val s = tostr(v);
        set_record(s.s, s.len);
        return;
    }
    if (i < 0) fatal("trying to access out of range field %ld", i);
    ensure_split();
    if (i > g_nf) extend_fields(i);
    Fld *f = &g_fld[i - 1];
    Val s = tostr(v);
    if (s.len + 1 > f->owncap) {
        size_t nc = (s.len + 16) & ~(size_t)15;
        char *nb = xmalloc(nc);
        memcpy(nb, s.s, s.len);
        free(f->own);
        f->own = nb;
        f->owncap = nc;
    } else {
        memmove(f->own, s.s, s.len);
    }
    f->own[s.len] = 0;
    f->s = f->own;
    f->len = s.len;
    f->t = (v.t == T_NUM || v.t == T_UNINIT) ? T_STRNUM : v.t;
    f->n = v.t == T_NUM ? v.n : 0;
    g_rec_valid = 0;
}

/* ------------------------------------------------------------------ input and output streams */

typedef struct Reader { int fd; char *buf; size_t cap, pos, end; int eof; pid_t pid; } Reader;
typedef struct In { struct In *next; char *name; int is_cmd; Reader *rd; } In;
typedef struct Out { struct Out *next; char *name; int fd; int is_pipe; pid_t pid; Buf b; int autoflush; } Out;

static In *g_ins;
static Out *g_outs;
static Out g_out_stdout = { .name = "/dev/stdout", .fd = 1 };
static Out g_out_stderr = { .name = "/dev/stderr", .fd = 2, .autoflush = 1 };

static void out_flush(Out *o) {
    size_t off = 0;
    while (off < o->b.len) {
        ssize_t w = write(o->fd, o->b.p + off, o->b.len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            o->b.len = 0;
            if (o->fd == 1) exit(2);
            return;
        }
        off += (size_t)w;
    }
    o->b.len = 0;
}

static void out_put(Out *o, const char *s, size_t n) {
    if (n >= 65536) {
        out_flush(o);
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(o->fd, s + off, n - off);
            if (w < 0) { if (errno == EINTR) continue; if (o->fd == 1) exit(2); return; }
            off += (size_t)w;
        }
        return;
    }
    if (o->b.len + n > 65536) out_flush(o);
    buf_add(&o->b, s, n);
}

static void out_flush_all(void) {
    out_flush(&g_out_stdout);
    for (Out *o = g_outs; o; o = o->next) out_flush(o);
}

static int wait_status(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 256 + WTERMSIG(st);
}

static void close_io_in_child(void) {
    for (Out *o = g_outs; o; o = o->next) if (o->fd > 2) close(o->fd);
    for (In *i = g_ins; i; i = i->next) if (i->rd && i->rd->fd > 2) close(i->rd->fd);
}

static pid_t spawn_cmd(const char *cmd, int *fd_parent, int for_write) {
    int fds[2];
    if (pipe(fds) < 0) fatal("can't create a pipe");
    out_flush_all();
    pid_t pid = fork();
    if (pid < 0) fatal("can't fork");
    if (pid == 0) {
        dup2(for_write ? fds[0] : fds[1], for_write ? 0 : 1);
        close(fds[0]);
        close(fds[1]);
        close_io_in_child();
        execl("/bin/gapsh", "gapsh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(for_write ? fds[0] : fds[1]);
    *fd_parent = for_write ? fds[1] : fds[0];
    return pid;
}

/* kind: 1 truncate, 2 append, 3 pipe */
static Out *out_open(const char *name, size_t nlen, int kind) {
    if (nlen == 11 && !memcmp(name, "/dev/stdout", 11)) return &g_out_stdout;
    if (nlen == 11 && !memcmp(name, "/dev/stderr", 11)) return &g_out_stderr;
    for (Out *o = g_outs; o; o = o->next)
        if (strlen(o->name) == nlen && !memcmp(o->name, name, nlen) && (o->is_pipe != 0) == (kind == 3)) return o;
    Out *o = xcalloc(1, sizeof *o);
    o->name = xstrndup(name, nlen);
    if (kind == 3) {
        o->is_pipe = 1;
        o->pid = spawn_cmd(o->name, &o->fd, 1);
    } else {
        o->fd = open(o->name, O_WRONLY | O_CREAT | (kind == 2 ? O_APPEND : O_TRUNC), 0666);
        if (o->fd < 0) fatal("can't redirect to %s", o->name);
    }
    o->next = g_outs;
    g_outs = o;
    return o;
}

static int out_close(Out *o) {
    out_flush(o);
    int rc = 0;
    close(o->fd);
    if (o->is_pipe) rc = wait_status(o->pid);
    Out **pp = &g_outs;
    while (*pp && *pp != o) pp = &(*pp)->next;
    if (*pp) *pp = o->next;
    free(o->name);
    free(o->b.p);
    free(o);
    return rc;
}

static Reader *reader_new(int fd) {
    Reader *r = xcalloc(1, sizeof *r);
    r->fd = fd;
    return r;
}

static int reader_fill(Reader *r) {
    if (r->eof) return 0;
    if (r->pos > 0 && r->pos == r->end) r->pos = r->end = 0;
    if (r->end == r->cap) {
        if (r->pos > 0) {
            memmove(r->buf, r->buf + r->pos, r->end - r->pos);
            r->end -= r->pos;
            r->pos = 0;
        } else {
            r->cap = r->cap ? r->cap * 2 : 65536;
            r->buf = xrealloc(r->buf, r->cap);
        }
    }
    ssize_t n;
    do n = read(r->fd, r->buf + r->end, r->cap - r->end); while (n < 0 && errno == EINTR);
    if (n <= 0) { r->eof = 1; return 0; }
    r->end += (size_t)n;
    return 1;
}

/* Reads the next record into *rec (valid until the next call). Returns 1, or 0 at end of input. */
static int read_rec(Reader *r, const char **rec, size_t *rlen) {
    if (g_rs.len == 1) {
        char c = g_rs.s[0];
        size_t scan = 0;
        for (;;) {
            char *q = memchr(r->buf + r->pos + scan, c, r->end - r->pos - scan);
            if (q) {
                *rec = r->buf + r->pos;
                *rlen = (size_t)(q - *rec);
                r->pos = (size_t)(q - r->buf) + 1;
                return 1;
            }
            size_t have = r->end - r->pos;
            if (!reader_fill(r)) {
                if (r->pos < r->end) {
                    *rec = r->buf + r->pos;
                    *rlen = r->end - r->pos;
                    r->pos = r->end;
                    return 1;
                }
                return 0;
            }
            scan = have;
        }
    }
    if (g_rs.len == 0) {                        /* paragraph mode */
        for (;;) {
            while (r->pos < r->end && r->buf[r->pos] == '\n') r->pos++;
            if (r->pos < r->end) break;
            if (!reader_fill(r)) return 0;
        }
        size_t scan = 0;
        for (;;) {
            char *q = r->end - r->pos - scan >= 2 ? memmem(r->buf + r->pos + scan, r->end - r->pos - scan, "\n\n", 2) : NULL;
            if (q) {
                *rec = r->buf + r->pos;
                *rlen = (size_t)(q - *rec);
                r->pos = (size_t)(q - r->buf) + 2;
                return 1;
            }
            size_t have = r->end - r->pos;
            if (!reader_fill(r)) {
                size_t l = r->end - r->pos;
                while (l && r->buf[r->pos + l - 1] == '\n') l--;
                *rec = r->buf + r->pos;
                *rlen = l;
                r->pos = r->end;
                return l > 0;
            }
            scan = have > 0 ? have - 1 : 0;
        }
    }
    if (!g_rs_re) g_rs_re = regex_compile(g_rs.s, g_rs.len);
    for (;;) {
        size_t ms, me, len = r->end - r->pos;
        if (len && re_search(g_rs_re, r->buf + r->pos, len, 0, &ms, &me) && me > ms && (me < len || r->eof)) {
            *rec = r->buf + r->pos;
            *rlen = ms;
            r->pos += me;
            return 1;
        }
        if (!reader_fill(r)) {
            len = r->end - r->pos;
            if (len && re_search(g_rs_re, r->buf + r->pos, len, 0, &ms, &me) && me > ms) {
                *rec = r->buf + r->pos;
                *rlen = ms;
                r->pos += me;
                return 1;
            }
            if (!len) return 0;
            *rec = r->buf + r->pos;
            *rlen = len;
            r->pos = r->end;
            return 1;
        }
    }
}

static void reader_close(Reader *r) {
    if (r->fd > 0) close(r->fd);
    free(r->buf);
    free(r);
}

static In *in_open(const char *name, size_t nlen, int is_cmd) {
    for (In *i = g_ins; i; i = i->next)
        if (strlen(i->name) == nlen && !memcmp(i->name, name, nlen) && i->is_cmd == is_cmd) return i;
    In *in = xcalloc(1, sizeof *in);
    in->name = xstrndup(name, nlen);
    in->is_cmd = is_cmd;
    int fd;
    pid_t pid = 0;
    if (is_cmd) {
        pid = spawn_cmd(in->name, &fd, 0);
    } else if (!strcmp(in->name, "-") || !strcmp(in->name, "/dev/stdin")) {
        fd = 0;
    } else {
        fd = open(in->name, O_RDONLY);
        if (fd < 0) { free(in->name); free(in); return NULL; }
    }
    in->rd = reader_new(fd);
    in->rd->pid = pid;
    in->next = g_ins;
    g_ins = in;
    return in;
}

static int in_close(In *in) {
    int rc = 0;
    if (in->is_cmd) {
        if (in->rd->fd > 0) close(in->rd->fd);
        rc = wait_status(in->rd->pid);
        in->rd->fd = -1;
    }
    reader_close(in->rd);
    In **pp = &g_ins;
    while (*pp && *pp != in) pp = &(*pp)->next;
    if (*pp) *pp = in->next;
    free(in->name);
    free(in);
    return rc;
}

static int close_stream(const char *name, size_t nlen) {
    int rc = -1;
    for (Out *o = g_outs; o;) {
        Out *nx = o->next;
        if (strlen(o->name) == nlen && !memcmp(o->name, name, nlen)) rc = out_close(o);
        o = nx;
    }
    for (In *i = g_ins; i;) {
        In *nx = i->next;
        if (strlen(i->name) == nlen && !memcmp(i->name, name, nlen)) rc = in_close(i);
        i = nx;
    }
    return rc;
}

static void close_all_streams(void) {
    out_flush(&g_out_stdout);
    while (g_outs) out_close(g_outs);
    while (g_ins) in_close(g_ins);
    out_flush(&g_out_stdout);
}

/* ------------------------------------------------------------------ main input (the file operands in ARGV) */

static Reader *g_main_rd;
static int g_argi = 1, g_used_file;

static int is_assignment_arg(const char *s) {
    if (!is_name_start((unsigned char)*s)) return 0;
    const char *p = s;
    while (is_name_char((unsigned char)*p)) p++;
    return *p == '=';
}

static void assign_operand(const char *s) {
    const char *eq = strchr(s, '=');
    size_t nl = (size_t)(eq - s);
    size_t vl;
    char *val = unescape_str(eq + 1, eq + 1 + strlen(eq + 1), &vl);
    for (int i = 0; i < g_nglob; i++) {
        if (!strncmp(g_gnames[i], s, nl) && !g_gnames[i][nl]) {
            Val v = { T_INPUT, 0, val, vl };
            if (i == V_NF) set_nf((long)num_of(v));
            else if (i != V_ENVIRON && i != V_ARGV && !g_cells[i].arr) { cell_set(&g_cells[i], v); if (i < NSPECIAL) special_changed(i); }
            break;
        }
    }
    free(val);
}

static int next_main_file(void) {
    for (;;) {
        double argc = num_of(cell_val(&g_cells[V_ARGC]));
        if (g_argi >= argc) {
            if (g_used_file) return 0;
            g_used_file = 1;
            g_main_rd = reader_new(0);
            g_fnr = 0;
            return 1;
        }
        char key[32];
        size_t kl = num_str(g_argi, "%d", key, sizeof key);
        g_argi++;
        Elem *e = arr_lookup(g_cells[V_ARGV].arr, key, kl, 0);
        if (!e) continue;
        Val a = tostr(cell_val(&e->cell));
        if (a.len == 0) continue;
        const char *arg = a.s;
        if (is_assignment_arg(arg)) { assign_operand(arg); continue; }
        g_used_file = 1;
        int fd;
        if (!strcmp(arg, "-")) fd = 0;
        else {
            fd = open(arg, O_RDONLY);
            if (fd < 0) {
                out_flush_all();
                fprintf(stderr, "awk: can't open file %s (%s)\n", arg, strerror(errno));
                g_errstatus = 2;
                continue;
            }
        }
        cell_set(&g_cells[V_FILENAME], vstr(arg, a.len));
        g_main_rd = reader_new(fd);
        g_fnr = 0;
        return 1;
    }
}

static void close_main(void) {
    if (g_main_rd) { reader_close(g_main_rd); g_main_rd = NULL; }
}

static int main_getrec(const char **rec, size_t *len) {
    for (;;) {
        if (!g_main_rd && !next_main_file()) return 0;
        if (read_rec(g_main_rd, rec, len)) { g_nr++; g_fnr++; return 1; }
        close_main();
    }
}

/* ------------------------------------------------------------------ evaluation */

static Val eval(Node *n);
static int exec(Node *n);

typedef struct { int kind; Cell *cell; int scope, idx; long fld; } LRef;

static long field_index(Node *n) {
    if (n->type == N_NUM) return (long)n->num;
    double d = num_of(eval(n));
    return (long)d;
}

static Val key_of(Node *sub, int count) {
    if (count == 1) return tostr(eval(sub));
    Buf b = { 0 };
    int first = 1;
    for (Node *s = sub; s; s = s->next) {
        if (!first) buf_add(&b, g_subsep.s, g_subsep.len);
        first = 0;
        Val v = tostr(eval(s));
        buf_add(&b, v.s, v.len);
    }
    Val r = vcopy(b.p ? b.p : "", b.len);
    free(b.p);
    return r;
}

static Elem *elem_of(Node *n) {
    Array *a = get_array(n->a);
    if (n->nargs == 1 && n->b->type != N_STR) {
        Val v = eval(n->b);
        if (v.t == T_NUM) {
            char kb[64];
            size_t kl = num_str(v.n, g_convfmt, kb, sizeof kb);
            return arr_lookup(a, kb, kl, 1);
        }
        v = tostr(v);
        return arr_lookup(a, v.s, v.len, 1);
    }
    Val k = key_of(n->b, n->nargs);
    return arr_lookup(a, k.s, k.len, 1);
}

static void lv_resolve(Node *n, LRef *r) {
    switch (n->type) {
    case N_VAR: r->kind = 0; r->scope = n->scope; r->idx = n->idx; break;
    case N_ELEM: r->kind = 1; r->cell = &elem_of(n)->cell; break;
    case N_FIELD: r->kind = 2; r->fld = field_index(n->a); break;
    default: fatal("assignment to a non-lvalue");
    }
}

static Val lv_get(LRef *r) {
    switch (r->kind) {
    case 0: return var_get(r->scope, r->idx);
    case 1: return cell_val(r->cell);
    default: return get_field(r->fld);
    }
}

static void lv_set(LRef *r, Val v) {
    switch (r->kind) {
    case 0: var_set(r->scope, r->idx, v); break;
    case 1: cell_set(r->cell, v); break;
    default: set_field(r->fld, v); break;
    }
}

static double awk_pow(double a, double b) {
    if (b == (double)(long)b && fabs(b) < 2147483648.0) {
        long e = (long)b;
        int neg = e < 0;
        if (neg) e = -e;
        double r = 1, x = a;
        while (e) { if (e & 1) r *= x; x *= x; e >>= 1; }
        return neg ? 1 / r : r;
    }
    return pow(a, b);
}

static double arith(int op, double x, double y) {
    switch (op) {
    case '+': return x + y;
    case '-': return x - y;
    case '*': return x * y;
    case '/': if (y == 0) fatal("division by zero"); return x / y;
    case '%': if (y == 0) fatal("division by zero in %%"); return fmod(x, y);
    default: return awk_pow(x, y);
    }
}

static Regex *regex_of(Node *n) {
    if (n->type == N_REGEX) return n->re;
    Val v = tostr(eval(n));
    return regex_get(v.s, v.len);
}

static Val eval_builtin(Node *n);
static Val call_func(Node *n);
static Val do_getline(Node *n);

static __attribute__((noinline)) Val eval_slow(Node *n) {
    switch (n->type) {
    case N_NUM: return vnum(n->num);
    case N_STR: return vstr(n->str, n->slen);
    case N_REGEX: {
        Val s = get_field(0);
        size_t a, b;
        return vnum(re_search(n->re, s.s, s.len, 0, &a, &b));
    }
    case N_VAR: return var_get(n->scope, n->idx);
    case N_FIELD: return get_field(field_index(n->a));
    case N_ELEM: return cell_val(&elem_of(n)->cell);
    case N_GROUP: return eval(n->a);
    case N_ASSIGN: {
        LRef r;
        lv_resolve(n->a, &r);
        if (n->idx == TK_ASSIGN) {
            Val v = eval(n->b);
            lv_set(&r, v);
        } else {
            double x = num_of(lv_get(&r));
            double y = num_of(eval(n->b));
            int op = n->idx == TK_ADDASSIGN ? '+' : n->idx == TK_SUBASSIGN ? '-' : n->idx == TK_MULASSIGN ? '*' :
                     n->idx == TK_DIVASSIGN ? '/' : n->idx == TK_MODASSIGN ? '%' : '^';
            lv_set(&r, vnum(arith(op, x, y)));
        }
        return lv_get(&r);
    }
    case N_COND: return truth(eval(n->a)) ? eval(n->b) : eval(n->c);
    case N_OR: return vnum(truth(eval(n->a)) || truth(eval(n->b)));
    case N_AND: return vnum(truth(eval(n->a)) && truth(eval(n->b)));
    case N_NOT: return vnum(!truth(eval(n->a)));
    case N_NEG: return vnum(-num_of(eval(n->a)));
    case N_PLUS: return vnum(num_of(eval(n->a)));
    case N_BINOP: {
        double x = num_of(eval(n->a));
        double y = num_of(eval(n->b));
        return vnum(arith(n->idx, x, y));
    }
    case N_CMP: {
        Val a = eval(n->a);
        Val b = eval(n->b);
        int c = compare_vals(a, b);
        int r;
        switch (n->idx) {
        case TK_LT: r = c == -1; break;
        case TK_LE: r = c == -1 || c == 0; break;
        case TK_EQ: r = c == 0; break;
        case TK_NE: r = c != 0; break;
        case TK_GT: r = c == 1; break;
        default: r = c == 1 || c == 0; break;
        }
        return vnum(r);
    }
    case N_MATCH: {
        Val s = tostr(eval(n->a));
        Regex *re = regex_of(n->b);
        size_t a, b;
        int m = re_search(re, s.s, s.len, 0, &a, &b);
        return vnum(n->idx ? !m : m);
    }
    case N_CONCAT: {
        Val a = tostr(eval(n->a));
        Val b = tostr(eval(n->b));
        char *p = talloc(a.len + b.len + 1);
        memcpy(p, a.s, a.len);
        memcpy(p + a.len, b.s, b.len);
        p[a.len + b.len] = 0;
        return vstr(p, a.len + b.len);
    }
    case N_IN: {
        Array *arr = get_array(n->a);
        Val k = key_of(n->b, n->nargs);
        return vnum(arr_lookup(arr, k.s, k.len, 0) != NULL);
    }
    case N_INCR: {
        LRef r;
        lv_resolve(n->a, &r);
        double old = num_of(lv_get(&r));
        lv_set(&r, vnum(old + n->idx));
        return vnum(n->scope ? old : old + n->idx);
    }
    case N_CALL: return call_func(n);
    case N_BUILTIN: return eval_builtin(n);
    case N_GETLINE: return do_getline(n);
    case N_GROUPLIST: fatal("syntax error: unexpected expression list");
    default: fatal("internal error: bad expression node %d", n->type);
    }
    return V_UNINIT;
}

static Val eval(Node *n) {
    switch (n->type) {
    case N_NUM: return vnum(n->num);
    case N_STR: return vstr(n->str, n->slen);
    case N_VAR: return var_get(n->scope, n->idx);
    case N_GROUP: return eval(n->a);
    case N_COND: return truth(eval(n->a)) ? eval(n->b) : eval(n->c);
    case N_BINOP: {
        double x = num_of(eval(n->a));
        double y = num_of(eval(n->b));
        return vnum(arith(n->idx, x, y));
    }
    case N_CALL: return call_func(n);
    default: return eval_slow(n);
    }
}

static Val call_func(Node *n) {
    Func *f = &g_funcs[n->idx];
    int np = f->np;
    if (n->nargs > np) fatal("function %s called with %d args, accepts only %d", f->name, n->nargs, np);
    char probe;
    if (++g_depth > 100000 || (size_t)(g_stack_base - &probe) > (size_t)6 << 20) fatal("function call nesting too deep in %s", f->name);
    Cell *loc = xcalloc((size_t)(np ? np : 1), sizeof(Cell));
    Cell **wb = xcalloc((size_t)(np ? np : 1), sizeof(Cell *));
    int i = 0;
    for (Node *a = n->a; a; a = a->next, i++) {
        if (a->type == N_VAR && !(a->scope == 0 && a->idx < NSPECIAL && a->idx != V_ENVIRON && a->idx != V_ARGV)) {
            Cell *c = var_cell(a->scope, a->idx);
            if (c->arr) { loc[i].arr = c->arr; loc[i].borrowed = 1; continue; }
            if (c->v.t == T_UNINIT) { wb[i] = c; continue; }
            cell_set(&loc[i], c->v);
            continue;
        }
        Val v = eval(a);
        cell_set(&loc[i], v);
    }
    Frame fr = { loc, np };
    Frame *saved = g_frame;
    g_frame = &fr;
    int flow = exec(f->body);
    g_frame = saved;
    g_depth--;
    Val ret = V_UNINIT;
    if (flow == F_RETURN && g_retcell.v.t != T_UNINIT) {
        ret = g_retcell.v;
        if (ret.t != T_NUM) { ret = vcopy(ret.s, ret.len); ret.t = g_retcell.v.t; ret.n = g_retcell.v.n; }
    }
    g_retcell.v.t = T_UNINIT;
    for (i = 0; i < np; i++) {
        if (loc[i].arr && !loc[i].borrowed && wb[i] && !wb[i]->arr) { wb[i]->arr = loc[i].arr; loc[i].arr = NULL; }
        cell_free(&loc[i]);
    }
    free(loc);
    free(wb);
    if (flow == F_EXIT || flow == F_NEXT || flow == F_NEXTFILE) {
        g_pending = flow;
        g_depth = 0;
        g_frame = NULL;
        longjmp(g_jmp, 1);
    }
    return ret;
}

static Out *redirect_target(Node *n) {
    Val t = tostr(eval(n->b));
    return out_open(t.s, t.len, n->idx);
}

static __attribute__((noinline)) void print_val(Out *o, Val v) {
    if (v.t == T_NUM) {
        char b[512];
        size_t k = num_str(v.n, g_ofmt, b, sizeof b);
        out_put(o, b, k);
    } else if (v.t != T_UNINIT) {
        out_put(o, v.s, v.len);
    }
}

static void awk_sprintf(Buf *out, const char *f, size_t fl, Val *av, int na);

static __attribute__((noinline)) int exec_print(Node *n) {
            Mark m = amark();
            Out *o = n->idx ? redirect_target(n) : &g_out_stdout;
            if (!n->a) {
                Val r = get_field(0);
                out_put(o, r.s, r.len);
            } else {
                for (Node *a = n->a; a; a = a->next) {
                    if (a != n->a) out_put(o, g_ofs.s, g_ofs.len);
                    print_val(o, eval(a));
                }
            }
            out_put(o, g_ors.s, g_ors.len);
            if (o->autoflush) out_flush(o);
            areset(m);
            return F_NORMAL;
        }

static __attribute__((noinline)) int exec_printf(Node *n) {
            Mark m = amark();
            Out *o = n->idx ? redirect_target(n) : &g_out_stdout;
            Val fmt = tostr(eval(n->a));
            int na = 0;
            Val *av = (Val *)talloc((size_t)(n->nargs + 1) * sizeof(Val));
            for (Node *a = n->a->next; a; a = a->next) av[na++] = eval(a);
            Buf b = { 0 };
            awk_sprintf(&b, fmt.s, fmt.len, av, na);
            out_put(o, b.p ? b.p : "", b.len);
            free(b.p);
            if (o->autoflush) out_flush(o);
            areset(m);
            return F_NORMAL;
        }

static __attribute__((noinline)) int exec_forin(Node *n) {
            Array *arr = get_array(n->b);
            size_t cnt = arr->n, i = 0;
            char **keys = xmalloc((cnt + 1) * sizeof(char *));
            size_t *lens = xmalloc((cnt + 1) * sizeof(size_t));
            for (Elem *e = arr->head; e; e = e->next, i++) { keys[i] = xstrndup(e->key, e->klen); lens[i] = e->klen; }
            int rc = F_NORMAL;
            for (i = 0; i < cnt; i++) {
                Mark m = amark();
                LRef r;
                lv_resolve(n->a, &r);
                lv_set(&r, vstr(keys[i], lens[i]));
                areset(m);
                int f = exec(n->c);
                if (f == F_BREAK) break;
                if (f && f != F_CONTINUE) { rc = f; break; }
            }
            for (i = 0; i < cnt; i++) free(keys[i]);
            free(keys);
            free(lens);
            return rc;
        }

static int exec(Node *n) {
    for (;;) {
        switch (n->type) {
        case N_BLOCK:
            for (Node *s = n->a; s; s = s->next) {
                int f = exec(s);
                if (f) return f;
            }
            return F_NORMAL;
        case N_EXPR: {
            Mark m = amark();
            eval(n->a);
            areset(m);
            return F_NORMAL;
        }
        case N_PRINT: return exec_print(n);
        case N_PRINTF: return exec_printf(n);
        case N_IF: {
            Mark m = amark();
            int c = truth(eval(n->a));
            areset(m);
            if (c) n = n->b;
            else if (n->c) n = n->c;
            else return F_NORMAL;
            continue;
        }
        case N_WHILE:
            for (;;) {
                Mark m = amark();
                int c = truth(eval(n->a));
                areset(m);
                if (!c) break;
                int f = exec(n->b);
                if (f == F_BREAK) break;
                if (f && f != F_CONTINUE) return f;
            }
            return F_NORMAL;
        case N_DO:
            for (;;) {
                int f = exec(n->a);
                if (f == F_BREAK) break;
                if (f && f != F_CONTINUE) return f;
                Mark m = amark();
                int c = truth(eval(n->b));
                areset(m);
                if (!c) break;
            }
            return F_NORMAL;
        case N_FOR:
            if (n->a) exec(n->a);
            for (;;) {
                if (n->b) {
                    Mark m = amark();
                    int c = truth(eval(n->b));
                    areset(m);
                    if (!c) break;
                }
                int f = exec(n->d);
                if (f == F_BREAK) break;
                if (f && f != F_CONTINUE) return f;
                if (n->c) exec(n->c);
            }
            return F_NORMAL;
        case N_FORIN: return exec_forin(n);
        case N_NEXT: return F_NEXT;
        case N_NEXTFILE: return F_NEXTFILE;
        case N_BREAK: return F_BREAK;
        case N_CONTINUE: return F_CONTINUE;
        case N_EXIT:
            if (n->a) {
                Mark m = amark();
                g_exit_status = (int)num_of(eval(n->a));
                g_exit_set = 1;
                areset(m);
            }
            return F_EXIT;
        case N_RETURN:
            if (n->a) {
                Mark m = amark();
                Val v = eval(n->a);
                cell_set(&g_retcell, v);
                areset(m);
            } else {
                g_retcell.v.t = T_UNINIT;
            }
            return F_RETURN;
        case N_DELETE: {
            Mark m = amark();
            Array *arr = get_array(n->a);
            if (!n->b) arr_clear(arr);
            else {
                Val k = key_of(n->b, n->nargs);
                arr_delete(arr, k.s, k.len);
            }
            areset(m);
            return F_NORMAL;
        }
        default: {
            Mark m = amark();
            eval(n);
            areset(m);
            return F_NORMAL;
        }
        }
    }
}

/* getline in all its forms; idx: 0 plain, 1 from a file, 2 from a command */
static __attribute__((noinline)) Val do_getline(Node *n) {
    const char *rec;
    size_t rlen;
    int r;
    LRef lr;
    if (n->a) lv_resolve(n->a, &lr);
    if (n->idx == 0) {
        r = main_getrec(&rec, &rlen);
    } else {
        Val nm = tostr(eval(n->b));
        In *in = in_open(nm.s, nm.len, n->idx == 2);
        if (!in) return vnum(-1);
        r = read_rec(in->rd, &rec, &rlen);
        if (r > 0 && n->idx == 2) { g_nr++; g_fnr++; }
    }
    if (r > 0) {
        if (n->a) { Val v = { T_INPUT, 0, rec, rlen }; lv_set(&lr, v); }
        else set_record(rec, rlen);
    }
    return vnum(r);
}

/* ------------------------------------------------------------------ printf */

static void bprintf(Buf *o, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_add(o, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(o, big, (size_t)n);
    free(big);
}

static size_t u8len(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static size_t u8encode(unsigned long c, char *out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63)); return 3; }
    out[0] = (char)(0xF0 | ((c >> 18) & 7));
    out[1] = (char)(0x80 | ((c >> 12) & 63));
    out[2] = (char)(0x80 | ((c >> 6) & 63));
    out[3] = (char)(0x80 | (c & 63));
    return 4;
}

static void pad_out(Buf *o, int n) { while (n-- > 0) buf_addc(o, ' '); }

static __attribute__((noinline)) void awk_sprintf(Buf *o, const char *f, size_t fl, Val *av, int na) {
    int ai = 0;
    size_t i = 0;
    while (i < fl) {
        if (f[i] != '%') {
            size_t j = i;
            while (j < fl && f[j] != '%') j++;
            buf_add(o, f + i, j - i);
            i = j;
            continue;
        }
        size_t start = i++;
        if (i >= fl) { buf_addc(o, '%'); break; }
        if (f[i] == '%') { buf_addc(o, '%'); i++; continue; }
        char spec[96];
        int sl = 0, left = 0, width = 0, prec = -1;
        spec[sl++] = '%';
        while (i < fl && strchr("-+ #0", f[i])) {
            if (f[i] == '-') left = 1;
            if (sl < 8) spec[sl++] = f[i];
            i++;
        }
        if (i < fl && f[i] == '*') {
            i++;
            width = (int)num_of(ai < na ? av[ai++] : V_UNINIT);
            if (width < 0) { left = 1; width = -width; spec[sl++] = '-'; }
        } else {
            while (i < fl && isdigit((unsigned char)f[i])) { width = width * 10 + (f[i] - '0'); i++; }
        }
        if (i < fl && f[i] == '.') {
            i++;
            prec = 0;
            if (i < fl && f[i] == '*') { i++; prec = (int)num_of(ai < na ? av[ai++] : V_UNINIT); if (prec < 0) prec = -1; }
            else while (i < fl && isdigit((unsigned char)f[i])) { prec = prec * 10 + (f[i] - '0'); i++; }
        }
        while (i < fl && strchr("hlLqjzt", f[i])) i++;
        if (i >= fl) { buf_add(o, f + start, fl - start); break; }
        char conv = f[i++];
        if (!strchr("diouxXeEfFgGcs", conv)) { buf_add(o, f + start, i - start); continue; }
        Val arg = ai < na ? av[ai] : V_UNINIT;
        ai++;
        if (width > 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, "%d", width);
        if (prec >= 0) sl += snprintf(spec + sl, sizeof spec - (size_t)sl, ".%d", prec);
        switch (conv) {
        case 'd': case 'i': {
            double d = num_of(arg);
            long long v = d >= 9.2233720368547758e18 ? LLONG_MAX : d <= -9.2233720368547758e18 ? LLONG_MIN : (d != d ? 0 : (long long)d);
            strcpy(spec + sl, "lld");
            bprintf(o, spec, v);
            break;
        }
        case 'o': case 'u': case 'x': case 'X': {
            double d = num_of(arg);
            unsigned long long v = d < 0 ? (unsigned long long)(long long)d : d >= 1.8446744073709552e19 ? ULLONG_MAX : (unsigned long long)d;
            spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = conv; spec[sl + 3] = 0;
            bprintf(o, spec, v);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double d = num_of(arg);
            spec[sl] = conv; spec[sl + 1] = 0;
            bprintf(o, spec, d);
            break;
        }
        case 'c': {
            char cb[8];
            size_t cl;
            double d;
            if (is_numeric_val(arg, &d) && arg.t != T_UNINIT) {
                unsigned long code = d < 0 ? 0 : (unsigned long)d;
                if (code < 256) { cb[0] = (char)code; cl = 1; } else cl = u8encode(code, cb);
            } else {
                Val s = tostr(arg);
                if (s.len == 0) { cl = 0; }
                else {
                    cl = 1;
                    unsigned char lead = (unsigned char)s.s[0];
                    size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    while (cl < want && cl < s.len && ((unsigned char)s.s[cl] & 0xC0) == 0x80) cl++;
                    memcpy(cb, s.s, cl);
                }
            }
            int padn = width - (cl ? 1 : 0);
            if (!left) pad_out(o, padn);
            buf_add(o, cb, cl);
            if (left) pad_out(o, padn);
            break;
        }
        default: {                              /* s */
            Val s = tostr(arg);
            size_t sn = s.len;
            if (prec >= 0 && (size_t)prec < sn) {
                size_t k = 0, chars = 0;
                while (k < sn && chars < (size_t)prec) {
                    k++;
                    while (k < sn && ((unsigned char)s.s[k] & 0xC0) == 0x80) k++;
                    chars++;
                }
                sn = k;
            }
            int padn = width - (int)u8len(s.s, sn);
            if (!left) pad_out(o, padn);
            buf_add(o, s.s, sn);
            if (left) pad_out(o, padn);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ builtin functions */

static unsigned long long g_rstate = 0x9E3779B97F4A7C15ULL;
static double g_seed = 0;

static void seed_rand(double seed) {
    unsigned long long z = (unsigned long long)(long long)seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    g_rstate = (z ^ (z >> 31)) | 1;
}

static double awk_rand(void) {
    unsigned long long x = g_rstate;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rstate = x;
    return (double)((x * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0;
}

/* appends the replacement text for one match (& and backslash rules of sub/gsub) */
static void add_replacement(Buf *b, const char *r, size_t rl, const char *m, size_t ml) {
    for (size_t i = 0; i < rl;) {
        if (r[i] == '\\') {
            if (i + 1 < rl && r[i + 1] == '\\') {
                if (i + 3 < rl && r[i + 2] == '\\' && r[i + 3] == '&') { buf_addc(b, '\\'); buf_addc(b, '&'); i += 4; }
                else if (i + 2 < rl && r[i + 2] == '&') { buf_addc(b, '\\'); i += 2; }
                else { buf_addc(b, '\\'); buf_addc(b, '\\'); i += 2; }
            } else if (i + 1 < rl && r[i + 1] == '&') { buf_addc(b, '&'); i += 2; }
            else { buf_addc(b, '\\'); i++; }
        } else if (r[i] == '&') {
            buf_add(b, m, ml);
            i++;
        } else {
            buf_addc(b, r[i++]);
        }
    }
}

static Buf g_subbuf;

static __attribute__((noinline)) Val do_sub(Node *n, int global) {
    Node *a1 = n->a, *a2 = a1->next, *a3 = a2->next;
    Regex *re = regex_of(a1);
    Val repl = tostr(eval(a2));
    LRef lr;
    Val tv;
    if (a3) {
        if (!is_lvalue(a3)) { tv = tostr(eval(a3)); lr.kind = -1; }
        else { lv_resolve(a3, &lr); tv = tostr(lv_get(&lr)); }
    } else {
        lr.kind = 2;
        lr.fld = 0;
        tv = get_field(0);
    }
    const char *s = tv.s;
    size_t len = tv.len, pos = 0, last_end = (size_t)-1;
    int count = 0;
    Buf *b = &g_subbuf;
    b->len = 0;
    buf_grow(b, len + 1);
    while (pos <= len) {
        size_t ms, me;
        if (!re_search(re, s, len, pos, &ms, &me)) break;
        if (me == ms && ms == last_end) {
            if (ms >= len) break;
            buf_addc(b, s[ms]);
            pos = ms + 1;
            continue;
        }
        buf_add(b, s + pos, ms - pos);
        add_replacement(b, repl.s, repl.len, s + ms, me - ms);
        count++;
        if (me == ms) {
            if (ms < len) buf_addc(b, s[ms]);
            pos = ms + 1;
        } else {
            pos = me;
        }
        last_end = me;
        if (!global) break;
    }
    if (count) {
        if (pos < len) buf_add(b, s + pos, len - pos);
        if (lr.kind >= 0) lv_set(&lr, vstr(b->p, b->len));
    }
    return vnum(count);
}

static Val lower_upper(Node *n, int up) {
    Val s = tostr(eval(n->a));
    char *p = talloc(s.len + 1);
    for (size_t i = 0; i < s.len; i++) p[i] = (char)(up ? toupper((unsigned char)s.s[i]) : tolower((unsigned char)s.s[i]));
    p[s.len] = 0;
    return vstr(p, s.len);
}

static __attribute__((noinline)) Val eval_builtin(Node *n) {
    Node *a1 = n->a, *a2 = a1 ? a1->next : NULL, *a3 = a2 ? a2->next : NULL;
    switch (n->idx) {
    case B_LENGTH: {
        if (!a1) { Val s = get_field(0); return vnum((double)s.len); }
        if (a1->type == N_VAR) {
            Cell *c = var_cell(a1->scope, a1->idx);
            if (c->arr) return vnum((double)c->arr->n);
        }
        Val s = tostr(eval(a1));
        return vnum((double)s.len);
    }
    case B_SUBSTR: {
        Val s = tostr(eval(a1));
        double m = num_of(eval(a2));
        double cnt = a3 ? num_of(eval(a3)) : INFINITY;
        if (m != m || cnt != cnt) return vstr("", 0);
        double start = floor(m + 0.5);
        if (a3) cnt = floor(cnt + 0.5);
        double len = (double)s.len;
        if (start < 1) { cnt += start - 1; start = 1; }
        if (cnt <= 0 || start > len) return vstr("", 0);
        double end = start + cnt - 1;
        if (end > len) end = len;
        size_t b = (size_t)start - 1, e = (size_t)end;
        return vcopy(s.s + b, e - b);
    }
    case B_INDEX: {
        Val s = tostr(eval(a1));
        Val t = tostr(eval(a2));
        if (t.len == 0) return vnum(0);
        const char *p = memmem(s.s, s.len, t.s, t.len);
        return vnum(p ? (double)(p - s.s) + 1 : 0);
    }
    case B_SPLIT: {
        Val sv = tostr(eval(a1));
        Val s = vcopy(sv.s, sv.len);
        if (a2->type != N_VAR) syntax();
        Array *arr = get_array(a2);
        Regex *re = NULL;
        const char *fs = g_fs.s;
        size_t fl = g_fs.len;
        if (a3) {
            if (a3->type == N_REGEX) { re = a3->re; fs = ""; fl = 0; }
            else {
                Val f = tostr(eval(a3));
                fs = f.s;
                fl = f.len;
                if (fl > 1) re = regex_get(fs, fl);
            }
        } else if (fl > 1) {
            re = regex_get(fs, fl);
        }
        int cnt = do_split(s.s, s.len, fs, fl, re, 0);
        arr_clear(arr);
        for (int i = 0; i < cnt; i++) {
            char kb[32];
            size_t kl = num_str(i + 1, "%d", kb, sizeof kb);
            Elem *e = arr_lookup(arr, kb, kl, 1);
            Val v = { T_INPUT, 0, s.s + g_sp[i].off, g_sp[i].len };
            cell_set(&e->cell, v);
        }
        return vnum(cnt);
    }
    case B_SUB: return do_sub(n, 0);
    case B_GSUB: return do_sub(n, 1);
    case B_MATCH: {
        Val s = tostr(eval(a1));
        Regex *re = regex_of(a2);
        size_t ms, me;
        if (re_search(re, s.s, s.len, 0, &ms, &me)) {
            cell_set(&g_cells[V_RSTART], vnum((double)ms + 1));
            cell_set(&g_cells[V_RLENGTH], vnum((double)(me - ms)));
            return vnum((double)ms + 1);
        }
        cell_set(&g_cells[V_RSTART], vnum(0));
        cell_set(&g_cells[V_RLENGTH], vnum(-1));
        return vnum(0);
    }
    case B_SPRINTF: {
        Val fmt = tostr(eval(a1));
        int na = 0;
        Val *av = (Val *)talloc((size_t)(n->nargs + 1) * sizeof(Val));
        for (Node *a = a1->next; a; a = a->next) av[na++] = eval(a);
        Buf b = { 0 };
        awk_sprintf(&b, fmt.s, fmt.len, av, na);
        Val r = vcopy(b.p ? b.p : "", b.len);
        free(b.p);
        return r;
    }
    case B_SIN: return vnum(sin(num_of(eval(a1))));
    case B_COS: return vnum(cos(num_of(eval(a1))));
    case B_ATAN2: { double y = num_of(eval(a1)); double x = num_of(eval(a2)); return vnum(atan2(y, x)); }
    case B_EXP: return vnum(exp(num_of(eval(a1))));
    case B_LOG: return vnum(log(num_of(eval(a1))));
    case B_SQRT: return vnum(sqrt(num_of(eval(a1))));
    case B_INT: return vnum(trunc(num_of(eval(a1))));
    case B_RAND: return vnum(awk_rand());
    case B_SRAND: {
        double prev = g_seed;
        g_seed = a1 ? num_of(eval(a1)) : (double)time(NULL);
        seed_rand(g_seed);
        return vnum(prev);
    }
    case B_TOLOWER: return lower_upper(n, 0);
    case B_TOUPPER: return lower_upper(n, 1);
    case B_SYSTEM: {
        Val c = tostr(eval(a1));
        out_flush_all();
        int r = system(c.s);
        return vnum(WIFEXITED(r) ? WEXITSTATUS(r) : 256 + WTERMSIG(r));
    }
    case B_CLOSE: {
        Val nm = tostr(eval(a1));
        return vnum(close_stream(nm.s, nm.len));
    }
    case B_FFLUSH: {
        if (!a1) { out_flush_all(); return vnum(0); }
        Val nm = tostr(eval(a1));
        if (nm.len == 11 && !memcmp(nm.s, "/dev/stdout", 11)) { out_flush(&g_out_stdout); return vnum(0); }
        for (Out *o = g_outs; o; o = o->next)
            if (strlen(o->name) == nm.len && !memcmp(o->name, nm.s, nm.len)) { out_flush(o); return vnum(0); }
        return vnum(-1);
    }
    case B_SYSTIME: return vnum((double)time(NULL));
    case B_STRFTIME: {
        Val fmt = a1 ? tostr(eval(a1)) : vstr("%a %b %e %H:%M:%S %Z %Y", 24);
        time_t t = a2 ? (time_t)num_of(eval(a2)) : time(NULL);
        int utc = a3 ? truth(eval(a3)) : 0;
        struct tm tmv, *tp = utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv);
        char *p = talloc(1024);
        size_t k = tp ? strftime(p, 1024, fmt.s, tp) : 0;
        p[k] = 0;
        return vstr(p, k);
    }
    }
    return V_UNINIT;
}

/* ------------------------------------------------------------------ driver */

static int run_action(Node *blk) {
    if (setjmp(g_jmp)) return g_pending;
    return exec(blk);
}

static int match_pattern(Node *p) {
    Mark m = amark();
    int r = truth(eval(p));
    areset(m);
    return r;
}

static int process_record(void) {
    if (setjmp(g_jmp)) return g_pending;
    for (int i = 0; i < g_nrules; i++) {
        Rule *r = &g_rules[i];
        int hit;
        if (!r->p1) hit = 1;
        else if (!r->p2) hit = match_pattern(r->p1);
        else if (!r->in_range) {
            hit = match_pattern(r->p1);
            if (hit) r->in_range = !match_pattern(r->p2);
        } else {
            hit = 1;
            if (match_pattern(r->p2)) r->in_range = 0;
        }
        if (!hit) continue;
        if (r->action) {
            int f = exec(r->action);
            if (f == F_NEXT) return F_NORMAL;
            if (f == F_NEXTFILE || f == F_EXIT) return f;
        } else {
            Val v = get_field(0);
            out_put(&g_out_stdout, v.s, v.len);
            out_put(&g_out_stdout, g_ors.s, g_ors.len);
            if (g_out_stdout.autoflush) out_flush(&g_out_stdout);
        }
    }
    return F_NORMAL;
}

static void usage(void) {
    fprintf(stderr, "usage: awk [-F fs][-v var=value][prog | -f progfile][file ...]\n");
    exit(2);
}

static void init_cells(void) {
    g_cells = xcalloc((size_t)g_nglob + 8, sizeof(Cell));
    g_cells[V_ENVIRON].arr = arr_new();
    g_cells[V_ARGV].arr = arr_new();
    cell_set(&g_cells[V_FS], vstr(" ", 1));
    cell_set(&g_cells[V_OFS], vstr(" ", 1));
    cell_set(&g_cells[V_ORS], vstr("\n", 1));
    cell_set(&g_cells[V_RS], vstr("\n", 1));
    cell_set(&g_cells[V_SUBSEP], vstr("\034", 1));
    cell_set(&g_cells[V_CONVFMT], vstr("%.6g", 4));
    cell_set(&g_cells[V_OFMT], vstr("%.6g", 4));
    cell_set(&g_cells[V_FILENAME], vstr("", 0));
    cell_set(&g_cells[V_RSTART], vnum(0));
    cell_set(&g_cells[V_RLENGTH], vnum(-1));
    for (int i = V_FS; i <= V_OFMT; i++) if (i != V_RSTART && i != V_RLENGTH && i != V_FILENAME) special_changed(i);
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        Elem *el = arr_lookup(g_cells[V_ENVIRON].arr, *e, (size_t)(eq - *e), 1);
        Val v = { T_INPUT, 0, eq + 1, strlen(eq + 1) };
        cell_set(&el->cell, v);
    }
}

APPLET(awk) {
    char stack_mark;
    g_stack_base = &stack_mark;
    const char *fs_opt = NULL;
    char **assigns = NULL;
    int nassign = 0;
    char **progfiles = NULL;
    int nprogfiles = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--version") || !strcmp(a, "-version") || !strcmp(a, "-V")) {
            printf("%s\n", AWK_VERSION);
            return 0;
        }
        char opt = a[1];
        if (opt != 'F' && opt != 'v' && opt != 'f') {
            if (opt == 'd' || opt == 'S' || opt == 'b' || opt == 'P' || opt == 'c' || opt == 'M' || opt == 'N' || opt == 'O') continue;
            fprintf(stderr, "awk: unknown option %s\n", a);
            usage();
        }
        const char *val;
        if (a[2]) val = a + 2;
        else if (i + 1 < argc) val = argv[++i];
        else { fprintf(stderr, "awk: no argument for option -%c\n", opt); usage(); return 2; }
        if (opt == 'F') fs_opt = val;
        else if (opt == 'v') {
            if (!is_assignment_arg(val)) { fprintf(stderr, "awk: invalid -v argument: %s\n", val); usage(); }
            assigns = xrealloc(assigns, (size_t)(nassign + 1) * sizeof(char *));
            assigns[nassign++] = (char *)val;
        } else {
            progfiles = xrealloc(progfiles, (size_t)(nprogfiles + 1) * sizeof(char *));
            progfiles[nprogfiles++] = (char *)val;
        }
    }
    Buf prog = { 0 };
    if (nprogfiles) {
        for (int k = 0; k < nprogfiles; k++) {
            int fd = strcmp(progfiles[k], "-") ? open(progfiles[k], O_RDONLY) : 0;
            if (fd < 0) fatal("can't open file %s (%s)", progfiles[k], strerror(errno));
            size_t n;
            char *txt = kb_read_all(fd, &n);
            if (fd) close(fd);
            buf_add(&prog, txt, n);
            buf_addc(&prog, '\n');
            free(txt);
        }
    } else {
        if (i >= argc) usage();
        buf_add(&prog, argv[i], strlen(argv[i]));
        i++;
    }
    if (!prog.p) buf_add(&prog, "", 0);
    parse_program(prog.p);
    init_cells();
    cell_set(&g_cells[V_ARGC], vnum(argc - i + 1));
    {
        Elem *e = arr_lookup(g_cells[V_ARGV].arr, "0", 1, 1);
        cell_set(&e->cell, vstr("awk", 3));
        for (int k = i, n = 1; k < argc; k++, n++) {
            char kb[32];
            size_t kl = num_str(n, "%d", kb, sizeof kb);
            e = arr_lookup(g_cells[V_ARGV].arr, kb, kl, 1);
            Val v = { T_INPUT, 0, argv[k], strlen(argv[k]) };
            cell_set(&e->cell, v);
        }
    }
    if (fs_opt && *fs_opt) {
        size_t fl;
        char *fs = !strcmp(fs_opt, "t") ? xstrndup("\t", 1) : unescape_str(fs_opt, fs_opt + strlen(fs_opt), &fl);
        if (!strcmp(fs_opt, "t")) fl = 1;
        Val v = { T_STR, 0, fs, fl };
        cell_set(&g_cells[V_FS], v);
        special_changed(V_FS);
        free(fs);
    }
    for (int k = 0; k < nassign; k++) assign_operand(assigns[k]);
    g_out_stdout.autoflush = isatty(1);
    seed_rand(0);

    Mark base = amark();
    int flow = F_NORMAL;
    for (int k = 0; k < g_nbegin && flow != F_EXIT; k++) {
        flow = run_action(g_begins[k]);
        areset(base);
    }
    if (flow != F_EXIT && (g_nrules || g_nend)) {
        const char *rec;
        size_t rlen;
        while (main_getrec(&rec, &rlen) > 0) {
            set_record(rec, rlen);
            flow = process_record();
            areset(base);
            if (flow == F_EXIT) break;
            if (flow == F_NEXTFILE) close_main();
        }
    }
    int in_end = 0;
    (void)in_end;
    for (int k = 0; k < g_nend; k++) {
        flow = run_action(g_ends[k]);
        areset(base);
        if (flow == F_EXIT) break;
    }
    close_all_streams();
    return g_exit_set ? g_exit_status : g_errstatus;
}
