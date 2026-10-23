/*
 * dc: reverse-Polish desk calculator (GNU dc commands).  Numbers come from bignum.c; strings are macros that x and the
 * conditional commands run.  A macro called as the last thing a macro does reuses the running frame, so loops written
 * as recursion do not eat stack.
 */
#include "kappabox.h"
#include "bignum.h"
#include <stdarg.h>

static char g_obuf[1 << 16];
static size_t g_olen;
static int g_col;
static int g_linelen = 70;

static void dc_flush(void) {
    size_t off = 0;
    while (off < g_olen) {
        ssize_t w = write(1, g_obuf + off, g_olen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    g_olen = 0;
}

static void put(int c) {
    if (g_olen == sizeof g_obuf) dc_flush();
    g_obuf[g_olen++] = (char)c;
    if (c == '\n') g_col = 0;
    else g_col++;
}

static void put_wrapped(int c) {
    if (g_linelen > 0 && g_col >= g_linelen - 1) {
        put('\\');
        put('\n');
    }
    put(c);
}

static void dc_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dc_err(const char *fmt, ...) {
    va_list ap;
    dc_flush();
    fputs("dc: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void dc_exit(int status) {
    dc_flush();
    exit(status);
}

/* ---- values: a number or a string */

typedef struct { Num *n; char *s; } Item;

static Item *g_stack;
static int g_sp, g_cap;

static void item_free(Item *it) {
    num_free(it->n);
    free(it->s);
    it->n = NULL;
    it->s = NULL;
}

static Item item_copy(const Item *it) {
    Item r = { NULL, NULL };
    if (it->s) {
        r.s = strdup(it->s);
        if (!r.s) kb_die("out of memory");
    } else r.n = it->n ? num_copy(it->n) : num_new();
    return r;
}

static void push(Item it) {
    if (g_sp == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_stack = realloc(g_stack, (size_t)g_cap * sizeof(Item));
        if (!g_stack) kb_die("out of memory");
    }
    g_stack[g_sp++] = it;
}

static void push_num(Num *n) { Item it = { n, NULL }; push(it); }
static void push_str(char *s) { Item it = { NULL, s }; push(it); }

/* ---- registers: a stack of values and an array each */

typedef struct { Item *v; int n, cap; } Arr;
typedef struct { Item *stk; int sp, cap; Arr arr; } Reg;
static Reg g_regs[256];

static void reg_push(Reg *r, Item it) {
    if (r->sp == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->stk = realloc(r->stk, (size_t)r->cap * sizeof(Item));
        if (!r->stk) kb_die("out of memory");
    }
    r->stk[r->sp++] = it;
}

static int g_scale, g_ibase = 10, g_obase = 10;
static int g_quit;          /* levels of macro execution still to leave after q or Q */
static int g_depth;         /* nesting of macro calls */

/* ---- helpers */

static int need(int n) {
    if (g_sp < n) {
        dc_err("stack empty");
        return 0;
    }
    return 1;
}

/* pops the top two items when both are numbers; leaves the stack alone otherwise */
static int pop2(Num **a, Num **b) {
    if (!need(2)) return 0;
    Item *t = &g_stack[g_sp - 1], *u = &g_stack[g_sp - 2];
    if (t->s || u->s) {
        dc_err("non-numeric value");
        return 0;
    }
    *b = t->n;
    *a = u->n;
    g_sp -= 2;
    return 1;
}

static void unpop2(Num *a, Num *b) {
    push_num(a);
    push_num(b);
}

static void print_item(const Item *it, int nl) {
    if (it->s) {
        for (const char *p = it->s; *p; p++) put((unsigned char)*p);
    } else {
        char *s = num_to_string(it->n, g_obase);
        for (const char *p = s; *p; p++) put_wrapped((unsigned char)*p);
        free(s);
    }
    if (nl) put('\n');
}

static void print_bytes(const Num *n) {
    Num *ip = num_setscale(n, 0);
    if (ip->neg) ip->neg = 0;
    /* base 256 digits, most significant first */
    unsigned char *bytes = malloc((size_t)ip->n * 4 + 4);
    int nb = 0;
    uint32_t *w = malloc((size_t)(ip->n + 1) * sizeof(uint32_t));
    if (!bytes || !w) kb_die("out of memory");
    memcpy(w, ip->d, (size_t)ip->n * sizeof(uint32_t));
    int nw = ip->n;
    while (nw > 0) {
        uint64_t rem = 0;
        for (int i = nw - 1; i >= 0; i--) {
            uint64_t cur = rem * 1000000000u + w[i];
            w[i] = (uint32_t)(cur / 256);
            rem = cur % 256;
        }
        while (nw > 0 && w[nw - 1] == 0) nw--;
        bytes[nb++] = (unsigned char)rem;
    }
    for (int i = nb - 1; i >= 0; i--) put(bytes[i]);
    free(bytes);
    free(w);
    num_free(ip);
}

static int small_int(const Item *it, long *out) {
    if (it->s) return 0;
    return num_to_long(it->n, out);
}

static Item *arr_slot(Arr *a, long idx) {
    if (idx >= a->cap) {
        int nc = a->cap ? a->cap : 16;
        while (nc <= idx) nc *= 2;
        a->v = realloc(a->v, (size_t)nc * sizeof(Item));
        if (!a->v) kb_die("out of memory");
        memset(a->v + a->cap, 0, (size_t)(nc - a->cap) * sizeof(Item));
        a->cap = nc;
    }
    if (idx >= a->n) a->n = (int)idx + 1;
    return &a->v[idx];
}

static void exec(const char *code, int owned, int nested);

static char *read_line(void) {
    char *line = NULL;
    size_t cap = 0;
    dc_flush();
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return NULL;
    }
    return line;
}

/* runs the macro held in register contents or a string item at the tail of a frame: returns a copy to run next */
static char *macro_text(const Item *it) {
    if (!it->s) return NULL;
    char *s = strdup(it->s);
    if (!s) kb_die("out of memory");
    return s;
}

static int tail_position(const unsigned char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return *p == 0;
}

static void exec(const char *code0, int owned0, int nested) {
    char *own = owned0 ? (char *)code0 : NULL;
    const unsigned char *p = (const unsigned char *)code0;
    g_depth++;
    for (;;) {
        int c = *p;
        if (!c) break;
        p++;
        char *call = NULL;          /* macro to run after this command */
        switch (c) {
        case ' ': case '\t': case '\n': case '\r':
            break;
        case '#':
            while (*p && *p != '\n') p++;
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case '.': case '_': {
            int neg = 0;
            if (c == '_') neg = 1;
            else p--;
            const char *end;
            Num *n = num_parse((const char *)p, g_ibase, &end);
            if (neg && n->n) n->neg = 1;
            p = (const unsigned char *)end;
            push_num(n);
            break;
        }
        case '[': {
            int depth = 1;
            const unsigned char *start = p;
            while (*p) {
                if (*p == '[') depth++;
                else if (*p == ']' && --depth == 0) break;
                p++;
            }
            char *s = malloc((size_t)(p - start) + 1);
            if (!s) kb_die("out of memory");
            memcpy(s, start, (size_t)(p - start));
            s[p - start] = 0;
            if (*p) p++;
            push_str(s);
            break;
        }
        case '+': case '-': case '*': case '/': case '%': case '~': case '^': {
            Num *a, *b, *r = NULL, *q = NULL;
            if (!pop2(&a, &b)) break;
            switch (c) {
            case '+': r = num_add(a, b); break;
            case '-': r = num_sub(a, b); break;
            case '*': r = num_mul(a, b, g_scale); break;
            case '/':
                r = num_div(a, b, g_scale);
                if (!r) { dc_err("division by zero"); unpop2(a, b); a = b = NULL; }
                break;
            case '%': case '~':
                if (num_divmod(a, b, &q, &r) < 0) { dc_err("remainder by zero"); unpop2(a, b); a = b = NULL; r = NULL; q = NULL; }
                else if (c == '%') { num_free(q); q = NULL; }
                break;
            case '^':
                r = num_pow(a, b, g_scale);
                if (!r) { dc_err("exponent too large"); unpop2(a, b); a = b = NULL; }
                break;
            }
            num_free(a);
            num_free(b);
            if (q) push_num(q);
            if (r) push_num(r);
            break;
        }
        case '|': {
            if (!need(3)) break;
            Item *m = &g_stack[g_sp - 1], *e = &g_stack[g_sp - 2], *b = &g_stack[g_sp - 3];
            if (m->s || e->s || b->s) { dc_err("non-numeric value"); break; }
            if (num_is_zero(m->n)) { dc_err("remainder by zero"); break; }
            if (e->n->neg) { dc_err("negative exponent"); break; }
            Num *r = num_modexp(b->n, e->n, m->n);
            item_free(m); item_free(e); item_free(b);
            g_sp -= 3;
            push_num(r ? r : num_new());
            break;
        }
        case 'v': {
            if (!need(1)) break;
            Item *t = &g_stack[g_sp - 1];
            if (t->s) { dc_err("non-numeric value"); break; }
            if (t->n->neg) { dc_err("square root of negative number"); break; }
            Num *r = num_sqrt(t->n, g_scale);
            item_free(t);
            g_sp--;
            push_num(r);
            break;
        }
        case 'p':
            if (!need(1)) break;
            print_item(&g_stack[g_sp - 1], 1);
            break;
        case 'n':
            if (!need(1)) break;
            print_item(&g_stack[g_sp - 1], 0);
            item_free(&g_stack[--g_sp]);
            break;
        case 'P':
            if (!need(1)) break;
            if (g_stack[g_sp - 1].s) print_item(&g_stack[g_sp - 1], 0);
            else print_bytes(g_stack[g_sp - 1].n);
            item_free(&g_stack[--g_sp]);
            break;
        case 'f':
            for (int i = g_sp - 1; i >= 0; i--) print_item(&g_stack[i], 1);
            break;
        case 'c':
            while (g_sp > 0) item_free(&g_stack[--g_sp]);
            break;
        case 'd':
            if (!need(1)) break;
            push(item_copy(&g_stack[g_sp - 1]));
            break;
        case 'r': {
            if (!need(2)) break;
            Item t = g_stack[g_sp - 1];
            g_stack[g_sp - 1] = g_stack[g_sp - 2];
            g_stack[g_sp - 2] = t;
            break;
        }
        case 'R':
            if (!need(1)) break;
            item_free(&g_stack[--g_sp]);
            break;
        case 'z':
            push_num(num_from_long(g_sp));
            break;
        case 'Z': {
            if (!need(1)) break;
            Item *t = &g_stack[g_sp - 1];
            long len = t->s ? (long)strlen(t->s) : num_length(t->n);
            item_free(t);
            g_sp--;
            push_num(num_from_long(len));
            break;
        }
        case 'X': {
            if (!need(1)) break;
            Item *t = &g_stack[g_sp - 1];
            long sc = t->s ? 0 : t->n->scale;
            item_free(t);
            g_sp--;
            push_num(num_from_long(sc));
            break;
        }
        case 'x': {
            if (!need(1)) break;
            Item it = g_stack[--g_sp];
            if (it.s) call = it.s;
            else push(it);
            break;
        }
        case 'k': case 'i': case 'o': {
            if (!need(1)) break;
            long v;
            if (!small_int(&g_stack[g_sp - 1], &v)) {
                dc_err(c == 'k' ? "scale must be a nonnegative number" : c == 'i' ? "input base must be a number between 2 and 16 (inclusive)"
                                                                                    : "output base must be a number greater than 1");
                break;
            }
            if (c == 'k' && (v < 0 || v > 100000000L)) { dc_err("scale must be a nonnegative number"); break; }
            if (c == 'i' && (v < 2 || v > 16)) { dc_err("input base must be a number between 2 and 16 (inclusive)"); break; }
            if (c == 'o' && (v < 2 || v > 99999)) { dc_err("output base must be a number greater than 1"); break; }
            item_free(&g_stack[--g_sp]);
            if (c == 'k') g_scale = (int)v;
            else if (c == 'i') g_ibase = (int)v;
            else g_obase = (int)v;
            break;
        }
        case 'K': push_num(num_from_long(g_scale)); break;
        case 'I': push_num(num_from_long(g_ibase)); break;
        case 'O': push_num(num_from_long(g_obase)); break;
        case 's': case 'S': case 'l': case 'L': {
            if (!*p) break;
            int rn = *p++;
            Reg *r = &g_regs[rn];
            if (c == 's' || c == 'S') {
                if (!need(1)) break;
                Item it = g_stack[--g_sp];
                if (c == 's' && r->sp > 0) {
                    item_free(&r->stk[r->sp - 1]);
                    r->stk[r->sp - 1] = it;
                } else reg_push(r, it);
            } else if (c == 'l') {
                if (r->sp == 0) {
                    /* a register that was never set holds 0 */
                    push_num(num_new());
                } else push(item_copy(&r->stk[r->sp - 1]));
            } else {
                if (r->sp == 0) { dc_err("stack register '%c' (0%o) is empty", rn, rn); break; }
                push(r->stk[--r->sp]);
            }
            break;
        }
        case ':': {
            if (!*p) break;
            int rn = *p++;
            if (!need(2)) break;
            long idx;
            if (!small_int(&g_stack[g_sp - 1], &idx) || idx < 0 || idx > 10000000L) {
                dc_err("array index must be a nonnegative integer");
                break;
            }
            item_free(&g_stack[--g_sp]);
            Item val = g_stack[--g_sp];
            Item *slot = arr_slot(&g_regs[rn].arr, idx);
            item_free(slot);
            *slot = val;
            break;
        }
        case ';': {
            if (!*p) break;
            int rn = *p++;
            if (!need(1)) break;
            long idx;
            if (!small_int(&g_stack[g_sp - 1], &idx) || idx < 0 || idx > 10000000L) {
                dc_err("array index must be a nonnegative integer");
                break;
            }
            item_free(&g_stack[--g_sp]);
            Arr *a = &g_regs[rn].arr;
            if (idx < a->n && (a->v[idx].n || a->v[idx].s)) push(item_copy(&a->v[idx]));
            else push_num(num_new());
            break;
        }
        case '<': case '>': case '=': case '!': {
            int neg = 0, op = c;
            if (c == '!') {
                if (*p == '<' || *p == '>' || *p == '=') { neg = 1; op = *p++; }
                else {
                    /* shell escape: the rest of the line */
                    const unsigned char *s = p;
                    while (*p && *p != '\n') p++;
                    char *cmd = malloc((size_t)(p - s) + 1);
                    if (!cmd) kb_die("out of memory");
                    memcpy(cmd, s, (size_t)(p - s));
                    cmd[p - s] = 0;
                    dc_flush();
                    if (system(cmd) < 0) dc_err("cannot run the command");
                    free(cmd);
                    break;
                }
            }
            if (!*p) break;
            int rn = *p++;
            Num *a, *b;
            if (!pop2(&a, &b)) break;
            int cmp = num_cmp(b, a);           /* top of stack against the second item */
            int truth = op == '<' ? cmp < 0 : op == '>' ? cmp > 0 : cmp == 0;
            num_free(a);
            num_free(b);
            if (neg) truth = !truth;
            if (truth) {
                Reg *r = &g_regs[rn];
                if (r->sp == 0) dc_err("register '%c' (0%o) is empty", rn, rn);
                else if (r->stk[r->sp - 1].s) call = macro_text(&r->stk[r->sp - 1]);
                else push(item_copy(&r->stk[r->sp - 1]));
            }
            break;
        }
        case '?': {
            char *line = read_line();
            if (line) exec(line, 1, 1);
            break;
        }
        case 'a': {
            if (!need(1)) break;
            Item *t = &g_stack[g_sp - 1];
            char *s = malloc(2);
            if (!s) kb_die("out of memory");
            if (t->s) s[0] = t->s[0];
            else {
                Num *ip = num_setscale(t->n, 0);
                long v = 0;
                if (!num_to_long(ip, &v)) v = 0;
                num_free(ip);
                s[0] = (char)(v & 255);
            }
            s[1] = 0;
            if (s[0] == 0) s[0] = 0;
            item_free(t);
            g_sp--;
            push_str(s);
            break;
        }
        case 'q':
            g_quit = 2;
            break;
        case 'Q': {
            if (!need(1)) break;
            long v;
            if (!small_int(&g_stack[g_sp - 1], &v) || v < 1) {
                dc_err("Q command requires a number >= 1");
                break;
            }
            item_free(&g_stack[--g_sp]);
            if (v > g_depth) {
                dc_err("Q command argument exceeded string execution depth");
                break;
            }
            g_quit = (int)v;
            break;
        }
        default:
            dc_err("'%c' (0%o) unimplemented", c, c);
            break;
        }
        if (call) {
            if (tail_position(p)) {
                /* reuse this frame: the new macro replaces what is left of the current one */
                free(own);
                own = call;
                p = (const unsigned char *)call;
            } else {
                exec(call, 1, 1);
            }
        }
        if (g_quit) break;
    }
    free(own);
    g_depth--;
    if (nested && g_quit > 0) g_quit--;
}

/* A chunk of program text: one line, or several when a string is still open at the end of the line. */
static char *read_chunk(FILE *f) {
    char *buf = NULL, *line = NULL;
    size_t blen = 0, cap = 0;
    int depth = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) >= 0) {
        buf = realloc(buf, blen + (size_t)n + 1);
        if (!buf) kb_die("out of memory");
        memcpy(buf + blen, line, (size_t)n + 1);
        blen += (size_t)n;
        for (ssize_t i = 0; i < n; i++) {
            char ch = line[i];
            if (ch == '#' && depth == 0) break;
            if (ch == '[') depth++;
            else if (ch == ']' && depth > 0) depth--;
        }
        if (depth == 0) break;
    }
    free(line);
    return buf;
}

static void run_stream(FILE *f) {
    char *chunk;
    while (!g_quit && (chunk = read_chunk(f))) {
        dc_flush();
        exec(chunk, 0, 0);
        free(chunk);
    }
}

APPLET(dc) {
    static const struct option longopts[] = {
        { "expression", required_argument, NULL, 'e' }, { "file", required_argument, NULL, 'f' },
        { "version", no_argument, NULL, 'V' }, { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    struct { int is_expr; const char *arg; } *items = calloc((size_t)argc + 1, sizeof *items);
    int nitems = 0, c;
    if (!items) kb_die("out of memory");
    optind = 0;
    while ((c = getopt_long(argc, argv, "e:f:Vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'e': items[nitems].is_expr = 1; items[nitems++].arg = optarg; break;
        case 'f': items[nitems].is_expr = 0; items[nitems++].arg = optarg; break;
        case 'V':
            puts("dc (Nitori's kappabox) 1.4.1 compatible");
            return 0;
        case 'h':
            puts("usage: dc [-V] [-h] [-e EXPR] [-f FILE] [FILE...]");
            return 0;
        default:
            fputs("usage: dc [-V] [-h] [-e EXPR] [-f FILE] [FILE...]\n", stderr);
            return 1;
        }
    }
    for (int i = optind; i < argc; i++) { items[nitems].is_expr = 0; items[nitems++].arg = argv[i]; }
    const char *ll = getenv("DC_LINE_LENGTH");
    if (!ll) ll = getenv("BC_LINE_LENGTH");
    if (ll) {
        int v = atoi(ll);
        g_linelen = v < 0 ? 70 : v;
    }
    for (int i = 0; i < nitems && !g_quit; i++) {
        if (items[i].is_expr) {
            exec(items[i].arg, 0, 0);
        } else {
            FILE *f = !strcmp(items[i].arg, "-") ? stdin : fopen(items[i].arg, "r");
            if (!f) {
                dc_err("cannot open file %s: %s", items[i].arg, strerror(errno));
                dc_exit(1);
            }
            run_stream(f);
            if (f != stdin) fclose(f);
        }
    }
    if (nitems == 0) run_stream(stdin);
    free(items);
    dc_exit(0);
    return 0;
}
