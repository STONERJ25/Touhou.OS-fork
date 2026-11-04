/* find and xargs. find supports the usual tests (-name -iname -path -type -size -mtime -mmin -newer -user -group -perm -empty
 * -links -executable), operators ( ) ! -a -o ',', and actions (-print -print0 -printf -ls -delete -prune -exec ;/+ -ok -quit). */
#include "kappabox.h"
#include <fnmatch.h>
#include <regex.h>

typedef enum {
    T_NAME, T_INAME, T_PATH, T_IPATH, T_REGEX, T_TYPE, T_SIZE, T_MTIME, T_ATIME, T_CTIME, T_MMIN, T_NEWER, T_USER, T_GROUP,
    T_PERM, T_EMPTY, T_LINKS, T_EXEC_OK, T_TRUE, T_FALSE, T_INUM, T_UID, T_GID, T_READABLE, T_WRITABLE,
    A_PRINT, A_PRINT0, A_PRINTF, A_LS, A_DELETE, A_PRUNE, A_EXEC, A_OK, A_QUIT,
    N_AND, N_OR, N_NOT, N_COMMA, N_LPAREN, N_RPAREN,
} Kind;

typedef struct Expr {
    Kind kind;
    struct Expr *l, *r;
    const char *s;                  /* pattern / format / type chars */
    long n;
    int cmp;                        /* -1: less than (-n), 0: exactly n, +1: more (+n) */
    int perm_mode;                  /* 0 exact, 1 all bits (-mode), 2 any bit (/mode) */
    char **cmd;                     /* -exec argv */
    int ncmd, plus;                 /* plus: "-exec ... {} +" */
    regex_t re;
    long long ref_mtime;
} Expr;

typedef struct {
    const char *path;
    const struct stat *st;
    int depth;
    const char *name;               /* base name */
} Entry;

static int g_maxdepth = INT_MAX, g_mindepth, g_depth_first, g_follow, g_has_action, g_quit, g_prune, g_rc;
static time_t g_now;
static char **g_batch;
static int g_nbatch, g_batch_cap;
static Expr *g_plus_expr;

static Expr *mk(Kind k) { Expr *e = calloc(1, sizeof *e); e->kind = k; return e; }

/* ------------------------------------------------------------------ parsing */

static char **g_av;
static int g_ac, g_ai;

static Expr *parse_or(void);

static long parse_num(const char *s, int *cmp, char *unit) {
    *cmp = 0;
    if (*s == '+') { *cmp = 1; s++; } else if (*s == '-') { *cmp = -1; s++; }
    char *end;
    long v = strtol(s, &end, 10);
    if (unit) *unit = *end;
    return v;
}

static Expr *parse_primary(void) {
    if (g_ai >= g_ac) kb_die("missing expression");
    const char *a = g_av[g_ai++];
    Expr *e;
    if (!strcmp(a, "(")) {
        e = parse_or();
        if (g_ai >= g_ac || strcmp(g_av[g_ai], ")")) kb_die("missing ')'");
        g_ai++;
        return e;
    }
    if (!strcmp(a, "!") || !strcmp(a, "-not")) { e = mk(N_NOT); e->l = parse_primary(); return e; }
#define ARG() (g_ai < g_ac ? g_av[g_ai++] : (kb_die("missing argument to '%s'", a), (char *)0))
    if (!strcmp(a, "-name")) { e = mk(T_NAME); e->s = ARG(); return e; }
    if (!strcmp(a, "-iname")) { e = mk(T_INAME); e->s = ARG(); return e; }
    if (!strcmp(a, "-path") || !strcmp(a, "-wholename")) { e = mk(T_PATH); e->s = ARG(); return e; }
    if (!strcmp(a, "-ipath")) { e = mk(T_IPATH); e->s = ARG(); return e; }
    if (!strcmp(a, "-regex")) {
        e = mk(T_REGEX);
        const char *pat = ARG();
        if (regcomp(&e->re, pat, 0)) kb_die("invalid regex '%s'", pat);
        return e;
    }
    if (!strcmp(a, "-type")) { e = mk(T_TYPE); e->s = ARG(); return e; }
    if (!strcmp(a, "-size")) {
        e = mk(T_SIZE);
        char unit = 'b';
        e->n = parse_num(ARG(), &e->cmp, &unit);
        e->perm_mode = unit;
        return e;
    }
    if (!strcmp(a, "-mtime")) { e = mk(T_MTIME); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-atime")) { e = mk(T_ATIME); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-ctime")) { e = mk(T_CTIME); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-mmin")) { e = mk(T_MMIN); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-newer")) {
        e = mk(T_NEWER);
        struct stat st;
        const char *f = ARG();
        if (stat(f, &st) < 0) kb_die("'%s': %s", f, strerror(errno));
        e->ref_mtime = st.st_mtime;
        return e;
    }
    if (!strcmp(a, "-user")) { e = mk(T_USER); e->s = ARG(); e->n = kb_user_id(e->s); if (e->n < 0) kb_die("'%s' is not the name of a known user", e->s); return e; }
    if (!strcmp(a, "-group")) { e = mk(T_GROUP); e->s = ARG(); e->n = kb_group_id(e->s); if (e->n < 0) kb_die("'%s' is not the name of a known group", e->s); return e; }
    if (!strcmp(a, "-uid")) { e = mk(T_UID); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-gid")) { e = mk(T_GID); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-perm")) {
        e = mk(T_PERM);
        const char *m = ARG();
        if (*m == '-') { e->perm_mode = 1; m++; } else if (*m == '/') { e->perm_mode = 2; m++; }
        e->n = strtol(m, NULL, 8);
        return e;
    }
    if (!strcmp(a, "-links")) { e = mk(T_LINKS); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-inum")) { e = mk(T_INUM); e->n = parse_num(ARG(), &e->cmp, NULL); return e; }
    if (!strcmp(a, "-empty")) return mk(T_EMPTY);
    if (!strcmp(a, "-executable")) { e = mk(T_EXEC_OK); return e; }
    if (!strcmp(a, "-readable")) return mk(T_READABLE);
    if (!strcmp(a, "-writable")) return mk(T_WRITABLE);
    if (!strcmp(a, "-true")) return mk(T_TRUE);
    if (!strcmp(a, "-false")) return mk(T_FALSE);
    if (!strcmp(a, "-print")) { g_has_action = 1; return mk(A_PRINT); }
    if (!strcmp(a, "-print0")) { g_has_action = 1; return mk(A_PRINT0); }
    if (!strcmp(a, "-printf")) { g_has_action = 1; e = mk(A_PRINTF); e->s = ARG(); return e; }
    if (!strcmp(a, "-ls")) { g_has_action = 1; return mk(A_LS); }
    if (!strcmp(a, "-delete")) { g_has_action = 1; g_depth_first = 1; return mk(A_DELETE); }
    if (!strcmp(a, "-prune")) return mk(A_PRUNE);
    if (!strcmp(a, "-quit")) { g_has_action = 1; return mk(A_QUIT); }
    if (!strcmp(a, "-depth")) { g_depth_first = 1; return mk(T_TRUE); }
    if (!strcmp(a, "-maxdepth")) { g_maxdepth = atoi(ARG()); return mk(T_TRUE); }
    if (!strcmp(a, "-mindepth")) { g_mindepth = atoi(ARG()); return mk(T_TRUE); }
    if (!strcmp(a, "-xdev") || !strcmp(a, "-mount") || !strcmp(a, "-noleaf") || !strcmp(a, "-follow")) { if (a[1] == 'f') g_follow = 1; return mk(T_TRUE); }
    if (!strcmp(a, "-exec") || !strcmp(a, "-ok") || !strcmp(a, "-execdir")) {
        g_has_action = 1;
        e = mk(a[1] == 'o' ? A_OK : A_EXEC);
        int start = g_ai;
        while (g_ai < g_ac && strcmp(g_av[g_ai], ";") && !(g_ai > start && !strcmp(g_av[g_ai], "+") && !strcmp(g_av[g_ai - 1], "{}"))) g_ai++;
        if (g_ai >= g_ac) kb_die("missing argument to '%s'", a);
        e->plus = !strcmp(g_av[g_ai], "+");
        e->ncmd = g_ai - start;
        e->cmd = calloc((size_t)e->ncmd + 1, sizeof(char *));
        for (int i = 0; i < e->ncmd; i++) e->cmd[i] = g_av[start + i];
        g_ai++;
        if (e->plus) g_plus_expr = e;
        return e;
    }
#undef ARG
    kb_die("unknown predicate '%s'", a);
    return NULL;
}

static Expr *parse_and(void) {
    Expr *l = parse_primary();
    for (;;) {
        if (g_ai >= g_ac) break;
        const char *a = g_av[g_ai];
        if (!strcmp(a, "-o") || !strcmp(a, "-or") || !strcmp(a, ")") || !strcmp(a, ",")) break;
        if (!strcmp(a, "-a") || !strcmp(a, "-and")) g_ai++;
        Expr *e = mk(N_AND);
        e->l = l;
        e->r = parse_primary();
        l = e;
    }
    return l;
}

static Expr *parse_or(void) {
    Expr *l = parse_and();
    while (g_ai < g_ac && (!strcmp(g_av[g_ai], "-o") || !strcmp(g_av[g_ai], "-or") || !strcmp(g_av[g_ai], ","))) {
        int comma = g_av[g_ai][0] == ',';
        g_ai++;
        Expr *e = mk(comma ? N_COMMA : N_OR);
        e->l = l;
        e->r = parse_and();
        l = e;
    }
    return l;
}

/* ------------------------------------------------------------------ evaluation */

static int num_cmp(long actual, long n, int cmp) { return cmp > 0 ? actual > n : cmp < 0 ? actual < n : actual == n; }

static int run_command(char **argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(argv[0], argv); fprintf(stderr, "find: '%s': %s\n", argv[0], strerror(errno)); _exit(127); }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}

static void flush_plus(Expr *e) {
    if (!e || !g_nbatch) return;
    int base = e->ncmd - 1;                         /* everything before the trailing {} */
    char **argv = calloc((size_t)(base + g_nbatch + 1), sizeof(char *));
    for (int i = 0; i < base; i++) argv[i] = e->cmd[i];
    for (int i = 0; i < g_nbatch; i++) argv[base + i] = g_batch[i];
    if (run_command(argv) != 0) g_rc = 1;
    for (int i = 0; i < g_nbatch; i++) free(g_batch[i]);
    g_nbatch = 0;
    free(argv);
}

static char *subst_braces(const char *arg, const char *path) {
    size_t cap = strlen(arg) + strlen(path) * 4 + 1;
    char *out = malloc(cap), *o = out;
    for (const char *p = arg; *p;) {
        if (p[0] == '{' && p[1] == '}') { size_t l = strlen(path); memcpy(o, path, l); o += l; p += 2; }
        else *o++ = *p++;
    }
    *o = 0;
    return out;
}

static void print_printf(const char *fmt, const Entry *en) {
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') putchar('\n'); else if (*p == 't') putchar('\t'); else if (*p == '0') putchar(0); else if (*p == '\\') putchar('\\'); else putchar(*p);
        } else if (*p == '%' && p[1]) {
            p++;
            switch (*p) {
            case 'p': fputs(en->path, stdout); break;
            case 'f': fputs(en->name, stdout); break;
            case 'h': { char tmp[PATH_MAX]; strlcpy(tmp, en->path, sizeof tmp); char *s = strrchr(tmp, '/'); if (s) { if (s == tmp) s[1] = 0; else *s = 0; fputs(tmp, stdout); } else putchar('.'); break; }
            case 's': printf("%lld", (long long)en->st->st_size); break;
            case 'm': printf("%o", en->st->st_mode & 07777); break;
            case 'u': fputs(kb_user_name(en->st->st_uid), stdout); break;
            case 'g': fputs(kb_group_name(en->st->st_gid), stdout); break;
            case 'd': printf("%d", en->depth); break;
            case 'i': printf("%llu", (unsigned long long)en->st->st_ino); break;
            case 'T': case 't': printf("%lld", (long long)en->st->st_mtime); break;
            case 'y': putchar(S_ISDIR(en->st->st_mode) ? 'd' : S_ISLNK(en->st->st_mode) ? 'l' : S_ISCHR(en->st->st_mode) ? 'c' : S_ISBLK(en->st->st_mode) ? 'b' : S_ISFIFO(en->st->st_mode) ? 'p' : 'f'); break;
            case '%': putchar('%'); break;
            default: putchar('%'); putchar(*p);
            }
        } else putchar(*p);
    }
}

static int eval(Expr *e, const Entry *en) {
    const struct stat *st = en->st;
    switch (e->kind) {
    case N_AND: return eval(e->l, en) && eval(e->r, en);
    case N_OR: return eval(e->l, en) || eval(e->r, en);
    case N_COMMA: eval(e->l, en); return eval(e->r, en);
    case N_NOT: return !eval(e->l, en);
    case T_TRUE: return 1;
    case T_FALSE: return 0;
    case T_NAME: return fnmatch(e->s, en->name, 0) == 0;
    case T_INAME: return fnmatch(e->s, en->name, FNM_CASEFOLD) == 0;
    case T_PATH: return fnmatch(e->s, en->path, 0) == 0;
    case T_IPATH: return fnmatch(e->s, en->path, FNM_CASEFOLD) == 0;
    case T_REGEX: return regexec(&e->re, en->path, 0, NULL, 0) == 0;
    case T_TYPE:
        for (const char *t = e->s; *t; t++) {
            int m = st->st_mode & S_IFMT;
            if ((*t == 'f' && m == S_IFREG) || (*t == 'd' && m == S_IFDIR) || (*t == 'l' && m == S_IFLNK) || (*t == 'c' && m == S_IFCHR) ||
                (*t == 'b' && m == S_IFBLK) || (*t == 'p' && m == S_IFIFO))
                return 1;
        }
        return 0;
    case T_SIZE: {
        long long unit = 512;
        switch (e->perm_mode) { case 'c': unit = 1; break; case 'k': unit = 1024; break; case 'M': unit = 1024 * 1024; break; case 'G': unit = 1024LL * 1024 * 1024; break; case 'w': unit = 2; break; }
        long long blocks = (st->st_size + unit - 1) / unit;
        return num_cmp((long)blocks, e->n, e->cmp);
    }
    case T_MTIME: return num_cmp((long)((g_now - st->st_mtime) / 86400), e->n, e->cmp);
    case T_ATIME: return num_cmp((long)((g_now - st->st_atime) / 86400), e->n, e->cmp);
    case T_CTIME: return num_cmp((long)((g_now - st->st_ctime) / 86400), e->n, e->cmp);
    case T_MMIN: return num_cmp((long)((g_now - st->st_mtime) / 60), e->n, e->cmp);
    case T_NEWER: return st->st_mtime > e->ref_mtime;
    case T_USER: return (long)st->st_uid == e->n;
    case T_GROUP: return (long)st->st_gid == e->n;
    case T_UID: return num_cmp((long)st->st_uid, e->n, e->cmp);
    case T_GID: return num_cmp((long)st->st_gid, e->n, e->cmp);
    case T_LINKS: return num_cmp((long)st->st_nlink, e->n, e->cmp);
    case T_INUM: return num_cmp((long)st->st_ino, e->n, e->cmp);
    case T_PERM: {
        long m = st->st_mode & 07777;
        if (e->perm_mode == 1) return (m & e->n) == e->n;
        if (e->perm_mode == 2) return e->n == 0 || (m & e->n) != 0;
        return m == e->n;
    }
    case T_EMPTY:
        if (S_ISREG(st->st_mode)) return st->st_size == 0;
        if (S_ISDIR(st->st_mode)) {
            DIR *d = opendir(en->path);
            if (!d) return 0;
            struct dirent *de;
            int empty = 1;
            while ((de = readdir(d))) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { empty = 0; break; }
            closedir(d);
            return empty;
        }
        return 0;
    case T_EXEC_OK: return access(en->path, X_OK) == 0;
    case T_READABLE: return access(en->path, R_OK) == 0;
    case T_WRITABLE: return access(en->path, W_OK) == 0;
    case A_PRINT: puts(en->path); return 1;
    case A_PRINT0: fputs(en->path, stdout); putchar(0); return 1;
    case A_PRINTF: print_printf(e->s, en); return 1;
    case A_LS: {
        char mode[12], tm[32];
        kb_mode_string(st->st_mode, mode);
        struct tm t;
        time_t mt = st->st_mtime;
        localtime_r(&mt, &t);
        strftime(tm, sizeof tm, "%b %e %H:%M", &t);
        printf("%6llu %4lld %s %3u %-8s %-8s %8lld %s %s\n", (unsigned long long)st->st_ino, (long long)((st->st_blocks + 1) / 2), mode,
               (unsigned)st->st_nlink, kb_user_name(st->st_uid), kb_group_name(st->st_gid), (long long)st->st_size, tm, en->path);
        return 1;
    }
    case A_DELETE:
        if (S_ISDIR(st->st_mode) ? rmdir(en->path) : unlink(en->path)) { kb_error("cannot delete '%s': %s", en->path, strerror(errno)); g_rc = 1; return 0; }
        return 1;
    case A_PRUNE: g_prune = 1; return 1;
    case A_QUIT: g_quit = 1; return 1;
    case A_OK:
    case A_EXEC: {
        if (e->plus) {
            if (g_nbatch == g_batch_cap) { g_batch_cap = g_batch_cap ? g_batch_cap * 2 : 64; g_batch = realloc(g_batch, (size_t)g_batch_cap * sizeof(char *)); }
            g_batch[g_nbatch++] = strdup(en->path);
            if (g_nbatch >= 128) flush_plus(e);
            return 1;
        }
        if (e->kind == A_OK) {
            fprintf(stderr, "< %s ... %s > ? ", e->cmd[0], en->path);
            char ans[8];
            if (!fgets(ans, sizeof ans, stdin) || (ans[0] != 'y' && ans[0] != 'Y')) return 0;
        }
        char **argv = calloc((size_t)e->ncmd + 1, sizeof(char *));
        for (int i = 0; i < e->ncmd; i++) argv[i] = subst_braces(e->cmd[i], en->path);
        int rc = run_command(argv);
        for (int i = 0; i < e->ncmd; i++) free(argv[i]);
        free(argv);
        return rc == 0;
    }
    default: return 1;
    }
}

static Expr *g_root;

static void visit(const char *path, int depth, const char *name) {
    if (g_quit) return;
    struct stat st;
    int r = (g_follow || depth == 0) ? stat(path, &st) : lstat(path, &st);
    if (r < 0 && depth == 0) r = lstat(path, &st);
    if (r < 0) { kb_error("'%s': %s", path, strerror(errno)); g_rc = 1; return; }
    Entry en = { path, &st, depth, name };
    int is_dir = S_ISDIR(st.st_mode);
    g_prune = 0;
    if (!g_depth_first && depth >= g_mindepth) {
        eval(g_root, &en);
        if (g_quit) return;
    }
    if (is_dir && depth < g_maxdepth && !(g_prune && !g_depth_first)) {
        DIR *d = opendir(path);
        if (!d) { kb_error("'%s': %s", path, strerror(errno)); g_rc = 1; }
        else {
            char **names = NULL;
            int n = 0;
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                names = realloc(names, (size_t)(n + 1) * sizeof *names);
                names[n++] = strdup(de->d_name);
            }
            closedir(d);
            for (int i = 0; i < n; i++) {
                if (!g_quit) {
                    char *child = kb_join(path, names[i]);
                    visit(child, depth + 1, names[i]);
                    free(child);
                }
                free(names[i]);
            }
            free(names);
        }
    }
    if (g_depth_first && depth >= g_mindepth && !g_quit) {
        Entry en2 = { path, &st, depth, name };
        eval(g_root, &en2);
    }
}

APPLET(find) {
    int i = 1;
    for (; i < argc && (!strcmp(argv[i], "-L") || !strcmp(argv[i], "-H") || !strcmp(argv[i], "-P")); i++) if (argv[i][1] == 'L') g_follow = 1;
    int first_path = i;
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "(") && strcmp(argv[i], "!")) i++;
    int npaths = i - first_path;
    g_av = argv;
    g_ac = argc;
    g_ai = i;
    g_now = time(NULL);
    g_root = g_ai < g_ac ? parse_or() : mk(T_TRUE);
    if (g_ai < g_ac) kb_die("unexpected argument '%s'", g_av[g_ai]);
    if (!g_has_action) {
        Expr *p = mk(A_PRINT), *a = mk(N_AND);
        a->l = g_root;
        a->r = p;
        g_root = a;
    }
    if (npaths == 0) visit(".", 0, ".");
    for (int k = 0; k < npaths; k++) {
        const char *p = argv[first_path + k];
        const char *base = strrchr(p, '/');
        visit(p, 0, base && base[1] ? base + 1 : p);
    }
    flush_plus(g_plus_expr);
    fflush(stdout);
    return g_rc;
}

/* ------------------------------------------------------------------ xargs */

#define XARGS_BUDGET    (128 * 1024)    /* bytes of items (and their pointers) per command line */
#define XARGS_MAX_ITEMS 16384

APPLET(xargs) {
    int max_args = 0, null_sep = 0, trace = 0, no_run_empty = 0, c;
    const char *repl = NULL;
    optind = 0;
    while ((c = getopt(argc, argv, "n:I:0tr")) != -1) {
        switch (c) {
        case 'n': max_args = atoi(optarg); break;
        case 'I': repl = optarg; max_args = 1; break;
        case '0': null_sep = 1; break;
        case 't': trace = 1; break;
        case 'r': no_run_empty = 1; break;
        default: return 1;
        }
    }
    char **cmd = &argv[optind];
    int ncmd = argc - optind;
    static char *default_cmd[] = { "echo", NULL };
    if (ncmd == 0) { cmd = default_cmd; ncmd = 1; }

    /* tokenise stdin */
    size_t len;
    char *data = kb_read_all(0, &len);
    char **items = NULL;
    int n = 0, cap = 0;
    size_t i = 0;
    char *scratch = malloc(len + 1);                        /* one buffer for the token being read; each item is then copied at its real size */
    while (i < len) {
        char *tok = scratch;
        size_t t = 0;
        if (null_sep) {
            while (i < len && data[i]) tok[t++] = data[i++];
            i++;
        } else if (repl) {                                  /* -I: one line per item, spaces preserved */
            while (i < len && data[i] != '\n') tok[t++] = data[i++];
            i++;
        } else {
            while (i < len && isspace((unsigned char)data[i])) i++;
            if (i >= len) break;
            int quote = 0;
            while (i < len) {
                char ch = data[i];
                if (quote) { if (ch == quote) { quote = 0; i++; continue; } }
                else if (ch == '"' || ch == '\'') { quote = ch; i++; continue; }
                else if (ch == '\\' && i + 1 < len) { tok[t++] = data[i + 1]; i += 2; continue; }
                else if (isspace((unsigned char)ch)) break;
                tok[t++] = ch;
                i++;
            }
        }
        tok[t] = 0;
        if (repl && !t) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; items = realloc(items, (size_t)cap * sizeof *items); }
        items[n++] = strdup(tok);
    }
    free(scratch);
    free(data);
    if (n == 0 && no_run_empty) return 0;
    int rc = 0;
    /* Without -n a command line takes as many items as fit in a budget well under the kernel's argument limit (512 KiB for
     * strings and pointers together, environment included), then the command runs again for the next batch. */
    size_t fixed = 0;
    for (int j = 0; j < ncmd; j++) fixed += strlen(cmd[j]) + 1 + sizeof(char *);
    for (int start = 0, cnt; start < n || (start == 0 && n == 0); start += cnt) {
        cnt = 0;
        if (max_args > 0) {
            cnt = n - start < max_args ? n - start : max_args;
        } else {
            size_t used = fixed;
            while (start + cnt < n) {
                size_t cost = strlen(items[start + cnt]) + 1 + sizeof(char *);
                if (cnt > 0 && (used + cost > XARGS_BUDGET || cnt >= XARGS_MAX_ITEMS)) break;
                used += cost;
                cnt++;
            }
        }
        char **av = calloc((size_t)(ncmd + cnt + 2), sizeof(char *));
        int k = 0;
        if (repl) {
            for (int j = 0; j < ncmd; j++) {
                av[k++] = strstr(cmd[j], repl) ? NULL : cmd[j];
                if (!av[k - 1]) {
                    const char *src = cmd[j];
                    size_t cap2 = strlen(src) + strlen(items[start]) * 8 + 1;
                    char *out = malloc(cap2), *o = out;
                    while (*src) {
                        if (!strncmp(src, repl, strlen(repl))) { size_t l = strlen(items[start]); memcpy(o, items[start], l); o += l; src += strlen(repl); }
                        else *o++ = *src++;
                    }
                    *o = 0;
                    av[k - 1] = out;
                }
            }
        } else {
            for (int j = 0; j < ncmd; j++) av[k++] = cmd[j];
            for (int j = 0; j < cnt; j++) av[k++] = items[start + j];
        }
        if (trace) { for (int j = 0; j < k; j++) fprintf(stderr, "%s%s", j ? " " : "", av[j]); fputc('\n', stderr); }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) { execvp(av[0], av); fprintf(stderr, "xargs: %s: %s\n", av[0], strerror(errno)); _exit(errno == ENOENT ? 127 : 126); }
        int status = 0;
        if (pid > 0) waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) rc = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? 127 : 123;
        free(av);
        if (n == 0) break;
    }
    return rc;
}
