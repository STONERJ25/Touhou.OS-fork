/* mmake -- Marisa's make.  A small make for TouhouOS, written from the POSIX make specification and the GNU make manual's description
 * of the behaviour that Makefiles lean on, so that the system can rebuild itself without a host (docs/SELFHOST.md).
 *
 * What it does: variables (= := ::= ?= += != and the command line, the environment), $(...) expansion with the usual text functions,
 * ifeq/ifneq/ifdef/ifndef, define/endef, include, explicit rules with several targets, .PHONY, order-only prerequisites, pattern rules
 * and static pattern rules (% stems), the automatic variables $@ $< $^ $+ $? $* with the D and F forms, recipes run by /bin/sh one line
 * at a time (@ - + prefixes), -f -C -n -s -k -i -B -j (accepted, always one job) and VAR=value on the command line.
 * What it leaves out: parallel jobs, suffix rules (.c.o:), target-specific variables, secondary expansion, vpath, remaking makefiles,
 * $(eval), archive members; a Makefile that needs one of those is told so ("not supported") instead of getting something else.
 *
 * The file is plain C that mcc compiles (the system builds mmake with mcc and mmake builds the system) and gcc/clang compile on a host,
 * where tests/host/test_mmake.py checks it against GNU make. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MMAKE_VERSION "0.1"

extern char **environ;      /* (libgap declares it too; glibc only with _GNU_SOURCE) */

/* ------------------------------------------------------------------------------------------------------ small helpers */

static const char *progname = "mmake";
static int opt_n, opt_s, opt_k, opt_i, opt_B;           /* -n dry run, -s silent, -k keep going, -i ignore errors, -B make everything */
static int errors_seen;
static int commands_started;                            /* recipe lines run (or shown, with -n): "up to date" is only said when there were none */

static const char *cur_file = "<command line>";         /* the makefile being read and its line, for messages */
static int cur_line;
static int reading;                                     /* 1 while a makefile is being read: errors then carry file:line */

static void die(const char *fmt, ...) __attribute__((noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    if (reading) fprintf(stderr, "%s:%d: *** ", cur_file, cur_line);
    else fprintf(stderr, "%s: *** ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ".  Stop.\n");
    exit(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

/* a growing string that always ends in a NUL once it has any content */
typedef struct {
    char *p;
    size_t len, cap;
} Str;

static void s_putn(Str *s, const char *t, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2 + 16;
        s->p = xrealloc(s->p, s->cap);
    }
    memcpy(s->p + s->len, t, n);
    s->len += n;
    s->p[s->len] = 0;
}

static void s_puts(Str *s, const char *t) {
    s_putn(s, t, strlen(t));
}

static void s_putc(Str *s, char c) {
    s_putn(s, &c, 1);
}

/* the contents as a malloc'd string (empty string for an empty Str); the Str is empty afterwards */
static char *s_take(Str *s) {
    char *r = s->p ? s->p : xstrdup("");
    s->p = NULL;
    s->len = s->cap = 0;
    return r;
}

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int is_space(char c) {
    return isspace((unsigned char)c);
}

/* The words of a string.  The array and the words are one allocation (free the array and everything is gone). */
static char **split_words(const char *s, int *count) {
    int n = 0;
    for (const char *p = s; *p;) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        while (*p && !is_space(*p)) p++;
        n++;
    }
    char **w = xmalloc((size_t)(n + 1) * sizeof(char *) + strlen(s) + 1);
    char *buf = (char *)(w + n + 1);
    int i = 0;
    for (const char *p = s; *p;) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;
        const char *b = p;
        while (*p && !is_space(*p)) p++;
        memcpy(buf, b, (size_t)(p - b));
        buf[p - b] = 0;
        w[i++] = buf;
        buf += p - b + 1;
    }
    w[n] = NULL;
    *count = n;
    return w;
}

static char *trim(const char *s) {
    while (*s && is_space(*s)) s++;
    size_t n = strlen(s);
    while (n && is_space(s[n - 1])) n--;
    return xstrndup(s, n);
}

static void put_word(Str *out, const char *w, size_t n) {
    if (out->len) s_putc(out, ' ');
    s_putn(out, w, n);
}

static void list_add(char ***arr, int *n, int *cap, const char *w) {
    if (*n + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = xrealloc(*arr, (size_t)*cap * sizeof(char *));
    }
    (*arr)[(*n)++] = xstrdup(w);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ------------------------------------------------------------------------------------------------------------ variables */

enum { O_DEFAULT, O_ENV, O_FILE, O_CMD, O_OVERRIDE };       /* where a variable came from; a lower origin never replaces a higher one */

typedef struct Var {
    char *name, *value;
    int recursive;      /* flavor: 1 = "=" (expanded when used), 0 = ":=" (expanded when set) */
    int origin;
    int busy;           /* being expanded: catches a variable that refers to itself */
    int exported;
    char *def_file;     /* where it was defined (NULL: not in a makefile) */
    int def_line;
    struct Var *next;
} Var;

#define NVARS 509
static Var *var_tab[NVARS];

static unsigned name_hash(const char *s, size_t n) {
    unsigned h = 5381;
    for (size_t i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i];
    return h;
}

static Var *var_find(const char *name, size_t n) {
    for (Var *v = var_tab[name_hash(name, n) % NVARS]; v; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v;
    return NULL;
}

static char *expand(const char *s);

/* Defines a variable.  recursive 1: the text is kept and expanded at every use; 0: it is expanded now; 2: it is a plain value that is
 * never expanded (the loop variable of foreach, a word of $(call)). */
static Var *var_set(const char *name, const char *value, int recursive, int origin) {
    size_t n = strlen(name);
    Var *v = var_find(name, n);
    if (v) {
        if (origin < v->origin) return v;
        free(v->value);
    } else {
        v = xmalloc(sizeof *v);
        memset(v, 0, sizeof *v);
        v->name = xstrndup(name, n);
        unsigned h = name_hash(name, n) % NVARS;
        v->next = var_tab[h];
        var_tab[h] = v;
    }
    v->origin = origin;
    v->recursive = recursive == 1;
    v->value = recursive == 0 ? expand(value) : xstrdup(value);
    free(v->def_file);
    v->def_file = reading ? xstrdup(cur_file) : NULL;
    v->def_line = cur_line;
    return v;
}

static void die_recursive(Var *v) __attribute__((noreturn));
static void die_recursive(Var *v) {
    if (v->def_file) {
        cur_file = v->def_file;
        cur_line = v->def_line;
        reading = 1;
    }
    die("Recursive variable '%s' references itself (eventually)", v->name);
}

static void var_remove(const char *name) {
    size_t n = strlen(name);
    Var **pp = &var_tab[name_hash(name, n) % NVARS];
    for (; *pp; pp = &(*pp)->next) {
        if (strlen((*pp)->name) == n && !memcmp((*pp)->name, name, n)) {
            Var *v = *pp;
            *pp = v->next;
            free(v->name);
            free(v->value);
            free(v->def_file);
            free(v);
            return;
        }
    }
}

static void var_append(const char *name, const char *value, int origin) {
    Var *v = var_find(name, strlen(name));
    if (!v) {
        var_set(name, value, 1, origin);
        return;
    }
    if (origin < v->origin) return;
    Str s = {0};
    s_puts(&s, v->value);
    if (v->value[0] && value[0]) s_putc(&s, ' ');
    if (v->recursive) s_puts(&s, value);
    else {
        char *e = expand(value);
        s_puts(&s, e);
        free(e);
    }
    free(v->value);
    v->value = s_take(&s);
}

/* A variable that exists for the duration of one function call (foreach, call): the old state is put back afterwards. */
typedef struct {
    char *name;
    char *value;
    int existed, recursive, origin, exported;
} Saved;

static void scope_set(Saved *sv, const char *name, const char *value) {
    Var *old = var_find(name, strlen(name));
    sv->name = xstrdup(name);
    sv->existed = old != NULL;
    sv->value = old ? xstrdup(old->value) : NULL;
    sv->recursive = old ? old->recursive : 0;
    sv->origin = old ? old->origin : 0;
    sv->exported = old ? old->exported : 0;
    var_remove(name);
    var_set(name, value, 2, O_OVERRIDE);
}

static void scope_restore(Saved *sv) {
    var_remove(sv->name);
    if (sv->existed) {
        Var *v = var_set(sv->name, sv->value, 2, sv->origin);
        v->recursive = sv->recursive;
        v->exported = sv->exported;
    }
    free(sv->name);
    free(sv->value);
}

/* ------------------------------------------------------------------------------------------------- patterns and text */

/* Does `s` match the pattern (one % at most)?  The text the % stood for goes to *stem (a pointer into s) and *stemlen. */
static int pat_match(const char *pat, const char *s, const char **stem, size_t *stemlen) {
    const char *pct = strchr(pat, '%');
    if (!pct) {
        if (strcmp(pat, s)) return 0;
        *stem = s;
        *stemlen = 0;
        return 1;
    }
    size_t pre = (size_t)(pct - pat), suf = strlen(pct + 1), n = strlen(s);
    if (n < pre + suf) return 0;
    if (memcmp(pat, s, pre) || memcmp(pct + 1, s + n - suf, suf)) return 0;
    *stem = s + pre;
    *stemlen = n - pre - suf;
    return 1;
}

/* Appends `pat` with its % replaced by the stem. */
static void pat_apply(Str *out, const char *pat, const char *stem, size_t stemlen) {
    const char *pct = strchr(pat, '%');
    if (!pct) {
        s_puts(out, pat);
        return;
    }
    s_putn(out, pat, (size_t)(pct - pat));
    s_putn(out, stem, stemlen);
    s_puts(out, pct + 1);
}

/* ---------------------------------------------------------------------------------------------------------- automatics */

typedef struct {
    const char *target, *first, *all, *all_dups, *newer, *stem;
} Auto;

static Auto autov;

/* $(@D) and $(@F): the directory of each word (without the slash; "." when there is none) or its last component */
static void auto_df(Str *dest, const char *list, int want_dir) {
    int n;
    Str res = {0}, *out = &res;
    char **w = split_words(list, &n);
    for (int i = 0; i < n; i++) {
        const char *sl = strrchr(w[i], '/');
        if (want_dir) {
            if (!sl) put_word(out, ".", 1);
            else if (sl == w[i]) put_word(out, "/", 1);
            else put_word(out, w[i], (size_t)(sl - w[i]));
        } else {
            put_word(out, sl ? sl + 1 : w[i], strlen(sl ? sl + 1 : w[i]));
        }
    }
    free(w);
    if (res.p) s_puts(dest, res.p);
    free(res.p);
}

static const char *auto_value(char c) {
    switch (c) {
    case '@': return autov.target;
    case '<': return autov.first;
    case '^': return autov.all;
    case '+': return autov.all_dups;
    case '?': return autov.newer;
    case '*': return autov.stem;
    default: return NULL;
    }
}

/* ----------------------------------------------------------------------------------------------- functions and expansion */

static void s_expand_into(Str *out, const char *s, size_t n);

/* Splits "a,b,c" at the commas that are outside any $( ), ${ } or ( ); at most `max` pieces (the last keeps its commas). */
static int split_args(const char *s, size_t n, int max, char **out) {
    int cnt = 0, depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') {
            if (depth) depth--;
        } else if (c == ',' && depth == 0 && cnt + 1 < max) {
            out[cnt++] = xstrndup(s + start, i - start);
            start = i + 1;
        }
    }
    out[cnt++] = xstrndup(s + start, n - start);
    return cnt;
}

static int has_wild(const char *s) {
    return strpbrk(s, "*?[") != NULL;
}

static void glob_add(char ***res, int *n, int *cap, const char *dir, const char *name) {
    if (*n + 1 >= *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *res = xrealloc(*res, (size_t)*cap * sizeof(char *));
    }
    Str s = {0};
    s_puts(&s, dir);
    s_puts(&s, name);
    (*res)[(*n)++] = s_take(&s);
}

/* The files named by one glob pattern, any path component may have wildcards; appended to out, sorted. */
static void wildcard_one(Str *out, const char *pattern) {
    struct stat st;
    if (!has_wild(pattern)) {
        if (stat(pattern, &st) == 0) put_word(out, pattern, strlen(pattern));
        return;
    }
    char **cur = xmalloc(sizeof(char *));
    int ncur = 1;
    cur[0] = xstrdup("");
    const char *p = pattern;
    if (*p == '/') {
        free(cur[0]);
        cur[0] = xstrdup("/");
        p++;
    }
    while (*p && ncur) {
        const char *sl = strchr(p, '/');
        size_t clen = sl ? (size_t)(sl - p) : strlen(p);
        char *comp = xstrndup(p, clen);
        char **next = NULL;
        int nnext = 0, capnext = 0;
        for (int i = 0; i < ncur; i++) {
            if (has_wild(comp)) {
                DIR *d = opendir(cur[i][0] ? cur[i] : ".");
                struct dirent *e;
                while (d && (e = readdir(d)) != NULL) {
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                    if (e->d_name[0] == '.' && comp[0] != '.') continue;
                    if (fnmatch(comp, e->d_name, 0)) continue;
                    glob_add(&next, &nnext, &capnext, cur[i], e->d_name);
                }
                if (d) closedir(d);
            } else {
                Str s = {0};
                s_puts(&s, cur[i]);
                s_puts(&s, comp);
                char *cand = s_take(&s);
                if (sl || stat(cand, &st) == 0) {
                    glob_add(&next, &nnext, &capnext, cand, "");
                }
                free(cand);
            }
        }
        for (int i = 0; i < ncur; i++) free(cur[i]);
        free(cur);
        free(comp);
        cur = next;
        ncur = nnext;
        if (sl) {
            /* a directory component keeps its slash and must be a directory */
            int keep = 0;
            for (int i = 0; i < ncur; i++) {
                if (stat(cur[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                    Str s = {0};
                    s_puts(&s, cur[i]);
                    s_putc(&s, '/');
                    free(cur[i]);
                    cur[keep++] = s_take(&s);
                } else free(cur[i]);
            }
            ncur = keep;
            p = sl + 1;
        } else p += clen;
    }
    if (cur) {
        qsort(cur, (size_t)ncur, sizeof(char *), cmp_str);
        for (int i = 0; i < ncur; i++) {
            put_word(out, cur[i], strlen(cur[i]));
            free(cur[i]);
        }
        free(cur);
    }
}

/* Runs a command and returns what it printed with the newlines turned into spaces (and the last one dropped). */
static char *run_shell(const char *cmd) {
    fflush(stdout);
    FILE *p = popen(cmd, "r");
    Str out = {0};
    if (p) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) s_putn(&out, buf, n);
        pclose(p);
    }
    char *r = s_take(&out);
    size_t len = strlen(r);
    while (len && (r[len - 1] == '\n' || r[len - 1] == '\r')) r[--len] = 0;
    for (size_t i = 0; i < len; i++)
        if (r[i] == '\n' || r[i] == '\r') r[i] = ' ';
    return r;
}

static const char *origin_name(Var *v) {
    if (!v) return "undefined";
    switch (v->origin) {
    case O_DEFAULT: return "default";
    case O_ENV: return "environment";
    case O_FILE: return "file";
    case O_CMD: return "command line";
    default: return "override";
    }
}

static void fn_abspath(Str *out, const char *w) {
    char buf[1024];
    Str p = {0};
    if (w[0] != '/') {
        if (!getcwd(buf, sizeof buf)) buf[0] = 0;
        s_puts(&p, buf);
        s_putc(&p, '/');
    }
    s_puts(&p, w);
    /* fold ./ ../ and // without asking the file system */
    char **seg = xmalloc((strlen(p.p) / 2 + 2) * sizeof(char *));
    int nseg = 0;
    for (char *q = p.p; *q;) {
        while (*q == '/') q++;
        char *b = q;
        while (*q && *q != '/') q++;
        size_t l = (size_t)(q - b);
        if (!l || (l == 1 && b[0] == '.')) continue;
        if (l == 2 && b[0] == '.' && b[1] == '.') {
            if (nseg) free(seg[--nseg]);
            continue;
        }
        seg[nseg++] = xstrndup(b, l);
    }
    if (out->len) s_putc(out, ' ');
    if (!nseg) s_putc(out, '/');
    for (int k = 0; k < nseg; k++) {
        s_putc(out, '/');
        s_puts(out, seg[k]);
        free(seg[k]);
    }
    free(seg);
    free(p.p);
}

/* the functions whose arguments are all expanded first; the result is appended to out */
static void call_plain(Str *out, const char *name, int nargs, char **a) {
    (void)nargs;
    if (!strcmp(name, "subst")) {
        size_t fl = strlen(a[0]);
        if (!fl) s_puts(out, a[2]);
        else {
            for (const char *p = a[2]; *p;) {
                if (!strncmp(p, a[0], fl)) {
                    s_puts(out, a[1]);
                    p += fl;
                } else s_putc(out, *p++);
            }
        }
    } else if (!strcmp(name, "patsubst")) {
        char *pat = trim(a[0]), *rep = trim(a[1]);
        int n;
        char **w = split_words(a[2], &n);
        for (int i = 0; i < n; i++) {
            const char *stem;
            size_t sl;
            if (out->len) s_putc(out, ' ');
            if (pat_match(pat, w[i], &stem, &sl) && strchr(pat, '%')) pat_apply(out, rep, stem, sl);
            else s_puts(out, w[i]);
        }
        free(w);
        free(pat);
        free(rep);
    } else if (!strcmp(name, "strip")) {
        int n;
        char **w = split_words(a[0], &n);
        for (int i = 0; i < n; i++) put_word(out, w[i], strlen(w[i]));
        free(w);
    } else if (!strcmp(name, "findstring")) {
        if (strstr(a[1], a[0])) s_puts(out, a[0]);
    } else if (!strcmp(name, "filter") || !strcmp(name, "filter-out")) {
        int keep = name[6] == 0;
        int np, nw;
        char **pats = split_words(a[0], &np), **w = split_words(a[1], &nw);
        for (int i = 0; i < nw; i++) {
            int m = 0;
            for (int j = 0; j < np; j++) {
                const char *stem;
                size_t sl;
                if (pat_match(pats[j], w[i], &stem, &sl)) m = 1;
            }
            if (m == keep) put_word(out, w[i], strlen(w[i]));
        }
        free(pats);
        free(w);
    } else if (!strcmp(name, "sort")) {
        int n;
        char **w = split_words(a[0], &n);
        qsort(w, (size_t)n, sizeof(char *), cmp_str);
        for (int i = 0; i < n; i++)
            if (i == 0 || strcmp(w[i], w[i - 1])) put_word(out, w[i], strlen(w[i]));
        free(w);
    } else if (!strcmp(name, "word")) {
        char *num = trim(a[0]);
        long k = strtol(num, NULL, 10);
        free(num);
        if (k < 1) die("first argument to 'word' function must be greater than 0");
        int n;
        char **w = split_words(a[1], &n);
        if (k <= n) s_puts(out, w[k - 1]);
        free(w);
    } else if (!strcmp(name, "wordlist")) {
        long s = strtol(a[0], NULL, 10), e = strtol(a[1], NULL, 10);
        if (s < 1) die("invalid first argument to 'wordlist' function: '%s'", a[0]);
        const char *q = a[2], *from = NULL, *to = NULL;
        long k = 0;
        while (*q) {
            while (*q && is_space(*q)) q++;
            if (!*q) break;
            const char *b = q;
            while (*q && !is_space(*q)) q++;
            k++;
            if (k == s) from = b;
            if (k <= e && k >= s) to = q;
        }
        if (from && to) s_putn(out, from, (size_t)(to - from));
    } else if (!strcmp(name, "words")) {
        int n;
        char **w = split_words(a[0], &n);
        char tmp[24];
        snprintf(tmp, sizeof tmp, "%d", n);
        s_puts(out, tmp);
        free(w);
    } else if (!strcmp(name, "firstword") || !strcmp(name, "lastword")) {
        int n;
        char **w = split_words(a[0], &n);
        if (n) s_puts(out, name[0] == 'f' ? w[0] : w[n - 1]);
        free(w);
    } else if (!strcmp(name, "dir") || !strcmp(name, "notdir") || !strcmp(name, "suffix") || !strcmp(name, "basename")) {
        int n;
        char **w = split_words(a[0], &n);
        for (int i = 0; i < n; i++) {
            const char *sl = strrchr(w[i], '/');
            const char *base = sl ? sl + 1 : w[i];
            const char *dot = strrchr(base, '.');
            if (name[0] == 's' && !dot) continue;           /* a word without a suffix adds nothing, not even a space */
            if (out->len) s_putc(out, ' ');
            if (name[0] == 'd') s_putn(out, w[i], sl ? (size_t)(sl - w[i]) + 1 : 0), sl ? (void)0 : s_puts(out, "./");
            else if (name[0] == 'n') s_puts(out, base);
            else if (name[0] == 's') s_puts(out, dot);
            else s_putn(out, w[i], dot ? (size_t)(dot - w[i]) : strlen(w[i]));
        }
        free(w);
    } else if (!strcmp(name, "addsuffix") || !strcmp(name, "addprefix")) {
        int n;
        char **w = split_words(a[1], &n);
        for (int i = 0; i < n; i++) {
            if (out->len) s_putc(out, ' ');
            if (name[3] == 'p') {
                s_puts(out, a[0]);
                s_puts(out, w[i]);
            } else {
                s_puts(out, w[i]);
                s_puts(out, a[0]);
            }
        }
        free(w);
    } else if (!strcmp(name, "join")) {
        int n1, n2;
        char **w1 = split_words(a[0], &n1), **w2 = split_words(a[1], &n2);
        for (int i = 0; i < (n1 > n2 ? n1 : n2); i++) {
            if (out->len) s_putc(out, ' ');
            if (i < n1) s_puts(out, w1[i]);
            if (i < n2) s_puts(out, w2[i]);
        }
        free(w1);
        free(w2);
    } else if (!strcmp(name, "wildcard")) {
        int n;
        char **w = split_words(a[0], &n);
        for (int i = 0; i < n; i++) wildcard_one(out, w[i]);
        free(w);
    } else if (!strcmp(name, "abspath") || !strcmp(name, "realpath")) {
        int n;
        char **w = split_words(a[0], &n);
        for (int i = 0; i < n; i++) {
            char buf[1024];
            if (name[0] == 'a') fn_abspath(out, w[i]);
            else if (realpath(w[i], buf)) put_word(out, buf, strlen(buf));
        }
        free(w);
    } else if (!strcmp(name, "shell")) {
        char *r = run_shell(a[0]);
        s_puts(out, r);
        free(r);
    } else if (!strcmp(name, "error")) {
        die("%s", a[0]);
    } else if (!strcmp(name, "warning")) {
        fflush(stdout);
        fprintf(stderr, "%s: warning: %s\n", progname, a[0]);
    } else if (!strcmp(name, "info")) {
        printf("%s\n", a[0]);
        fflush(stdout);
    } else if (!strcmp(name, "origin")) {
        char *v = trim(a[0]);
        s_puts(out, origin_name(var_find(v, strlen(v))));
        free(v);
    } else if (!strcmp(name, "flavor")) {
        char *v = trim(a[0]);
        Var *x = var_find(v, strlen(v));
        s_puts(out, !x ? "undefined" : x->recursive ? "recursive" : "simple");
        free(v);
    } else if (!strcmp(name, "value")) {
        char *v = trim(a[0]);
        Var *x = var_find(v, strlen(v));
        if (x) s_puts(out, x->value);
        free(v);
    } else {
        die("the function '%s' is not supported by mmake", name);
    }
}

/* Calls function `name` with the unexpanded argument text; returns 0 when `name` is not a function. */
static int call_function(Str *dest, const char *name, const char *argtext, size_t argn) {
    Str res = {0}, *out = &res;             /* the result is collected here and appended to dest at the end */
    static const struct {
        const char *name;
        int min, max;
    } fns[] = {
        {"subst", 3, 3}, {"patsubst", 3, 3}, {"strip", 1, 1}, {"findstring", 2, 2}, {"filter", 2, 2}, {"filter-out", 2, 2}, {"sort", 1, 1},
        {"word", 2, 2}, {"wordlist", 3, 3}, {"words", 1, 1}, {"firstword", 1, 1}, {"lastword", 1, 1}, {"dir", 1, 1}, {"notdir", 1, 1},
        {"suffix", 1, 1}, {"basename", 1, 1}, {"addsuffix", 2, 2}, {"addprefix", 2, 2}, {"join", 2, 2}, {"wildcard", 1, 1}, {"abspath", 1, 1},
        {"realpath", 1, 1}, {"shell", 1, 1}, {"if", 2, 3}, {"or", 1, 99}, {"and", 1, 99}, {"foreach", 3, 3}, {"call", 1, 99}, {"error", 1, 1},
        {"warning", 1, 1}, {"info", 1, 1}, {"origin", 1, 1}, {"value", 1, 1}, {"flavor", 1, 1}, {"eval", 1, 1},
    };
    int idx = -1;
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
        if (!strcmp(name, fns[i].name)) idx = (int)i;
    if (idx < 0) return 0;
    char *raw[100];
    int nraw = split_args(argtext, argn, fns[idx].max, raw);
    if (nraw < fns[idx].min) die("insufficient number of arguments (%d) to function '%s'", nraw, name);

    if (!strcmp(name, "if")) {
        char *c = expand(raw[0]);
        char *t = trim(c);
        int yes = t[0] != 0;
        free(c);
        free(t);
        if (yes) s_expand_into(out, raw[1], strlen(raw[1]));
        else if (nraw > 2) s_expand_into(out, raw[2], strlen(raw[2]));
    } else if (!strcmp(name, "or") || !strcmp(name, "and")) {
        int is_or = name[0] == 'o';
        char *last = xstrdup("");
        for (int i = 0; i < nraw; i++) {
            free(last);
            char *e = expand(raw[i]);
            last = trim(e);
            free(e);
            if (is_or ? last[0] != 0 : last[0] == 0) break;
        }
        s_puts(out, last);
        free(last);
    } else if (!strcmp(name, "foreach")) {
        char *var = trim(raw[0]);
        char *list = expand(raw[1]);
        int n;
        char **w = split_words(list, &n);
        Saved sv;
        scope_set(&sv, var, "");
        for (int i = 0; i < n; i++) {
            var_set(var, w[i], 2, O_OVERRIDE);
            char *e = expand(raw[2]);
            if (i) s_putc(out, ' ');
            s_puts(out, e);
            free(e);
        }
        scope_restore(&sv);
        free(w);
        free(list);
        free(var);
    } else if (!strcmp(name, "call")) {
        char *fe = expand(raw[0]);
        char *fname = trim(fe);
        free(fe);
        /* inside the function $(1) is the first argument, $(2) the second ... and $(0) its name */
        Saved sv[10];
        char digit[2] = {0, 0};
        char *val[10];
        for (int i = 0; i < 10; i++) val[i] = i == 0 ? xstrdup(fname) : (i < nraw ? expand(raw[i]) : xstrdup(""));
        for (int i = 0; i < 10; i++) {
            digit[0] = (char)('0' + i);
            scope_set(&sv[i], digit, val[i]);
            free(val[i]);
        }
        Var *f = var_find(fname, strlen(fname));
        if (f) {
            if (f->busy) die_recursive(f);
            if (f->recursive) {
                f->busy = 1;
                s_expand_into(out, f->value, strlen(f->value));
                f->busy = 0;
            } else s_puts(out, f->value);
        }
        for (int i = 9; i >= 0; i--) scope_restore(&sv[i]);
        free(fname);
    } else if (!strcmp(name, "eval")) {
        die("$(eval) is not supported by mmake");
    } else {
        char *a[100];
        for (int i = 0; i < nraw; i++) a[i] = expand(raw[i]);
        call_plain(out, name, nraw, a);
        for (int i = 0; i < nraw; i++) free(a[i]);
    }
    for (int i = 0; i < nraw; i++) free(raw[i]);
    if (res.p) s_puts(dest, res.p);
    free(res.p);
    return 1;
}

/* index just past the bracket that closes the one at s[i] (a "(" or "{"), or n if there is none */
static size_t match_close(const char *s, size_t i, size_t n) {
    char open = s[i], close = open == '(' ? ')' : '}';
    int depth = 0;
    for (size_t j = i; j < n; j++) {
        if (s[j] == open) depth++;
        else if (s[j] == close && --depth == 0) return j + 1;
    }
    return n + 1;
}

static void var_value_into(Str *out, const char *name, size_t nlen) {
    if (nlen == 1) {
        const char *a = auto_value(name[0]);
        if (a) {
            s_puts(out, a);
            return;
        }
        if (strchr("@<^+?*", name[0])) return;          /* an automatic variable outside a recipe is empty */
    }
    if (nlen == 2 && (name[1] == 'D' || name[1] == 'F') && strchr("@<^+?*", name[0])) {
        const char *a = auto_value(name[0]);
        if (a) auto_df(out, a, name[1] == 'D');
        return;
    }
    Var *v = var_find(name, nlen);
    if (!v) return;
    if (!v->recursive) {
        s_puts(out, v->value);
        return;
    }
    if (v->busy) die_recursive(v);
    v->busy = 1;
    s_expand_into(out, v->value, strlen(v->value));
    v->busy = 0;
}

/* the text between $( and ): a function call, a variable, or a substitution reference $(VAR:a=b) */
static void do_expand_ref(Str *out, const char *inner, size_t n) {
    size_t i = 0;
    while (i < n && !is_blank(inner[i]) && inner[i] != '$') i++;
    if (i > 0 && i < n && is_blank(inner[i])) {
        char *fname = xstrndup(inner, i);
        size_t j = i;
        while (j < n && is_blank(inner[j])) j++;
        int done = call_function(out, fname, inner + j, n - j);
        free(fname);
        if (done) return;
    }
    Str tmp = {0};
    s_expand_into(&tmp, inner, n);
    char *name = s_take(&tmp);
    char *colon = strchr(name, ':');
    char *eq = colon ? strchr(colon, '=') : NULL;
    if (colon && eq) {
        /* $(VAR:suffix=new) and $(VAR:%.c=%.o) */
        Str val = {0};
        var_value_into(&val, name, (size_t)(colon - name));
        char *from = xstrndup(colon + 1, (size_t)(eq - colon - 1)), *to = xstrdup(eq + 1);
        int has_pct = strchr(from, '%') != NULL;
        Str pat = {0}, rep = {0};
        if (!has_pct) {
            s_putc(&pat, '%');
            s_putc(&rep, '%');
        }
        s_puts(&pat, from);
        s_puts(&rep, to);
        int cnt;
        char **w = split_words(val.p ? val.p : "", &cnt);
        for (int k = 0; k < cnt; k++) {
            const char *stem;
            size_t sl;
            if (k) s_putc(out, ' ');
            if (pat_match(pat.p, w[k], &stem, &sl)) pat_apply(out, rep.p, stem, sl);
            else s_puts(out, w[k]);
        }
        free(w);
        free(val.p);
        free(pat.p);
        free(rep.p);
        free(from);
        free(to);
    } else {
        var_value_into(out, name, strlen(name));
    }
    free(name);
}

static void s_expand_into(Str *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '$') {
            s_putc(out, s[i]);
            continue;
        }
        if (i + 1 >= n) {
            s_putc(out, '$');
            break;
        }
        char c = s[i + 1];
        if (c == '$') {
            s_putc(out, '$');
            i++;
        } else if (c == '(' || c == '{') {
            size_t end = match_close(s, i + 1, n);
            if (end > n) die("unterminated variable reference");
            do_expand_ref(out, s + i + 2, end - 1 - (i + 2));
            i = end - 1;
        } else {
            var_value_into(out, &s[i + 1], 1);
            i++;
        }
    }
}

static char *expand(const char *s) {
    Str out = {0};
    s_expand_into(&out, s, strlen(s));
    return s_take(&out);
}

/* ---------------------------------------------------------------------------------------------------------- rule data */

typedef struct Target {
    char *name;
    char **pre;                 /* prerequisites in the order written (a pattern rule's come first once it is chosen) */
    int npre, cap_pre;
    char **oo;                  /* order-only prerequisites */
    int noo, cap_oo;
    char **recipe;              /* the lines as written, with their @ - + prefixes */
    int nrecipe;
    int has_recipe;
    int is_explicit;            /* it appeared as a target in a rule */
    int phony;
    int state;                  /* 0 not looked at, 1 being made (meeting it again is a cycle), 2 done */
    int updated;                /* remade during this run, or counts as remade */
    int failed;
    int exists;
    long long mtime;
    int stat_done;
    int implicit_done;          /* the pattern rule search has been made */
    int recipe_block;           /* which rule line the recipe lines belong to (a later rule line with a recipe replaces them) */
    char *rfile;                /* where the recipe starts, for the "overriding recipe" warning */
    int rline;
    int blk_start, blk_count;   /* the prerequisites the latest rule line added: when it brings the recipe they move to the front */
    char *stem;
    struct Target *next;
} Target;

typedef struct PRule {
    char *target;               /* the pattern, containing % */
    char **pre;
    int npre, cap_pre;
    char **recipe;
    int nrecipe;
    int recipe_block;
    struct PRule *next;
} PRule;

#define NTARGETS 1021
static Target *targets[NTARGETS];
static PRule *prules, *prules_tail;
static char *default_goal;

static Target *target_get(const char *name, int create) {
    unsigned h = name_hash(name, strlen(name)) % NTARGETS;
    for (Target *t = targets[h]; t; t = t->next)
        if (!strcmp(t->name, name)) return t;
    if (!create) return NULL;
    Target *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->name = xstrdup(name);
    t->next = targets[h];
    targets[h] = t;
    return t;
}

/* ----------------------------------------------------------------------------------------------------------- the parser */

typedef struct {
    int active;                 /* the lines of this branch are read */
    int taken;                  /* some branch of this if has been true already */
    int parent_active;
} Cond;

#define MAX_COND 64
static Cond conds[MAX_COND];
static int ncond;

static Target **cur_targets;    /* the rule whose recipe lines are being read */
static int ncur_targets;
static PRule **cur_prules;
static int ncur_prules;
static int export_all;

static void parse_error(const char *what) {
    die("%s", what);
}

static int cond_active(void) {
    return ncond == 0 || conds[ncond - 1].active;
}

static void end_rule(void) {
    free(cur_targets);
    free(cur_prules);
    cur_targets = NULL;
    cur_prules = NULL;
    ncur_targets = ncur_prules = 0;
}

/* the first character of `stops` outside any $(...) or ${...} in s, or NULL */
static const char *scan_top(const char *s, const char *stops) {
    for (; *s; s++) {
        if (*s == '$' && (s[1] == '(' || s[1] == '{')) {
            size_t len = strlen(s), end = match_close(s, 1, len);
            if (end > len) return NULL;         /* an unterminated reference swallows the rest of the line */
            s += end - 1;
            continue;
        }
        if (strchr(stops, *s)) return s;
    }
    return NULL;
}

static void define_variable(const char *name, int op, const char *value, int origin, int exported) {
    char *en = expand(name);
    char *n = trim(en);
    free(en);
    if (!n[0]) parse_error("empty variable name");
    if (op == '+') var_append(n, value, origin);
    else if (op == '?') {
        if (!var_find(n, strlen(n))) var_set(n, value, 1, origin);
    } else if (op == '!') {
        char *cmd = expand(value);
        char *r = run_shell(cmd);
        var_set(n, r, 2, origin);
        free(cmd);
        free(r);
    } else var_set(n, value, op == '=' ? 1 : 0, origin);
    if (exported) {
        Var *v = var_find(n, strlen(n));
        if (v) v->exported = 1;
    }
    free(n);
}

/* If the line is an assignment, the position of its "=" and the operator character (= : ? + !), else NULL. */
static const char *find_assignment(const char *line, int *op) {
    const char *eq = scan_top(line, "=");
    if (!eq) return NULL;
    const char *colon = scan_top(line, ":");
    if (colon && colon < eq) {
        /* "a: b = c" is a rule; "a := c" and "a ::= c" are assignments */
        const char *p = colon;
        while (p < eq && *p == ':') p++;
        if (p != eq) return NULL;
        *op = ':';
        return eq;
    }
    if (eq > line && (eq[-1] == '?' || eq[-1] == '+' || eq[-1] == '!')) *op = eq[-1];
    else *op = '=';
    return eq;
}

static void add_words(char ***arr, int *n, int *cap, const char *list) {
    int cnt;
    char **w = split_words(list, &cnt);
    for (int i = 0; i < cnt; i++) list_add(arr, n, cap, w[i]);
    free(w);
}

static void set_default_goal(const char *name) {
    if (!default_goal && name[0] != '.') default_goal = xstrdup(name);
}

static int rule_block;      /* counts the rule lines read; the recipe lines that follow one belong to its block */

static void clear_lines(char ***lines, int *n) {
    for (int i = 0; i < *n; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *n = 0;
}

/* Adds a recipe line to every target of the rule being read.  The first line of a block replaces the recipe an earlier rule line gave the
 * same target (GNU make warns about that; here the last one wins). */
static void add_recipe_line(const char *raw) {
    for (int i = 0; i < ncur_targets; i++) {
        Target *t = cur_targets[i];
        if (t->recipe_block != rule_block) {
            if (t->nrecipe) {
                fflush(stdout);
                fprintf(stderr, "%s:%d: warning: overriding recipe for target '%s'\n", cur_file, cur_line, t->name);
                fprintf(stderr, "%s:%d: warning: ignoring old recipe for target '%s'\n", t->rfile, t->rline, t->name);
            }
            clear_lines(&t->recipe, &t->nrecipe);
            t->recipe_block = rule_block;
            free(t->rfile);
            t->rfile = xstrdup(cur_file);
            t->rline = cur_line;
            if (t->blk_start > 0 && t->blk_count > 0 && t->blk_start + t->blk_count <= t->npre) {
                /* GNU make lists the prerequisites of the rule that has the recipe first ($< is the first of them) */
                char **moved = xmalloc((size_t)t->blk_count * sizeof(char *));
                memcpy(moved, t->pre + t->blk_start, (size_t)t->blk_count * sizeof(char *));
                memmove(t->pre + t->blk_count, t->pre, (size_t)t->blk_start * sizeof(char *));
                memcpy(t->pre, moved, (size_t)t->blk_count * sizeof(char *));
                free(moved);
            }
            t->blk_start = 0;
            t->blk_count = 0;
        }
        t->has_recipe = 1;
        t->recipe = xrealloc(t->recipe, (size_t)(t->nrecipe + 1) * sizeof(char *));
        t->recipe[t->nrecipe++] = xstrdup(raw);
    }
    for (int i = 0; i < ncur_prules; i++) {
        PRule *r = cur_prules[i];
        if (r->recipe_block != rule_block) {
            clear_lines(&r->recipe, &r->nrecipe);
            r->recipe_block = rule_block;
        }
        r->recipe = xrealloc(r->recipe, (size_t)(r->nrecipe + 1) * sizeof(char *));
        r->recipe[r->nrecipe++] = xstrdup(raw);
    }
}

static int same_words(char **a, int na, char **b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i], b[i])) return 0;
    return 1;
}

/* "targets : prerequisites [| order-only] [; recipe]" and the static pattern form "targets : pattern : prerequisite-patterns" */
static void parse_rule(char *line) {
    char *colon = (char *)scan_top(line, ":");
    if (!colon) parse_error("missing separator");
    rule_block++;
    char *semi_recipe = NULL;
    char *semi = (char *)scan_top(colon, ";");
    if (semi) {
        *semi = 0;
        semi_recipe = semi + 1;
    }
    char *left = xstrndup(line, (size_t)(colon - line));
    char *rest = colon + 1;
    if (*rest == ':') rest++;
    char *tl = expand(left);
    free(left);
    int ntl;
    char **tw = split_words(tl, &ntl);

    char *pat = NULL;
    char *colon2 = (char *)scan_top(rest, ":");
    char *prelist = rest;
    if (colon2) {
        pat = xstrndup(rest, (size_t)(colon2 - rest));
        prelist = colon2 + 1;
    }
    end_rule();
    char *pe = expand(prelist);
    char *oo = NULL;
    char *bar = strchr(pe, '|');
    if (bar) {
        *bar = 0;
        oo = bar + 1;
    }
    cur_targets = xmalloc((size_t)(ntl + 1) * sizeof(Target *));
    cur_prules = xmalloc((size_t)(ntl + 1) * sizeof(PRule *));
    char *epat = pat ? expand(pat) : NULL;
    for (int i = 0; i < ntl; i++) {
        const char *name = tw[i];
        if (epat) {
            /* static pattern rule: the stem of each target fills the % of the prerequisites */
            char *ptn = trim(epat);
            const char *stem;
            size_t sl;
            if (!strchr(ptn, '%') || !pat_match(ptn, name, &stem, &sl)) {
                free(ptn);
                parse_error("target does not match the target pattern");
            }
            Target *t = target_get(name, 1);
            Str pres = {0}, oos = {0};
            int np;
            char **pw = split_words(pe, &np);
            for (int k = 0; k < np; k++) {
                if (pres.len) s_putc(&pres, ' ');
                pat_apply(&pres, pw[k], stem, sl);
            }
            free(pw);
            if (oo) {
                int no;
                char **ow = split_words(oo, &no);
                for (int k = 0; k < no; k++) {
                    if (oos.len) s_putc(&oos, ' ');
                    pat_apply(&oos, ow[k], stem, sl);
                }
                free(ow);
            }
            t->blk_start = t->npre;
            if (pres.p) add_words(&t->pre, &t->npre, &t->cap_pre, pres.p);
            t->blk_count = t->npre - t->blk_start;
            if (oos.p) add_words(&t->oo, &t->noo, &t->cap_oo, oos.p);
            free(pres.p);
            free(oos.p);
            free(t->stem);
            t->stem = xstrndup(stem, sl);
            t->is_explicit = 1;
            cur_targets[ncur_targets++] = t;
            set_default_goal(name);
            free(ptn);
        } else if (strchr(name, '%')) {
            int np, cap = 0;
            char **pw = split_words(pe, &np);
            PRule *r = NULL;
            /* the same pattern with the same prerequisites replaces the old rule in place (that is how a Makefile overrides a built-in one) */
            for (PRule *q = prules; q; q = q->next)
                if (!strcmp(q->target, name) && same_words(q->pre, q->npre, pw, np)) r = q;
            if (!r) {
                r = xmalloc(sizeof *r);
                memset(r, 0, sizeof *r);
                r->target = xstrdup(name);
                for (int k = 0; k < np; k++) list_add(&r->pre, &r->npre, &cap, pw[k]);
                r->cap_pre = cap;
                if (prules_tail) prules_tail->next = r;
                else prules = r;
                prules_tail = r;
            }
            free(pw);
            cur_prules[ncur_prules++] = r;
        } else {
            Target *t = target_get(name, 1);
            t->blk_start = t->npre;
            add_words(&t->pre, &t->npre, &t->cap_pre, pe);
            t->blk_count = t->npre - t->blk_start;
            if (oo) add_words(&t->oo, &t->noo, &t->cap_oo, oo);
            t->is_explicit = 1;
            if (!strcmp(name, ".PHONY")) {
                int n;
                char **w = split_words(pe, &n);
                for (int k = 0; k < n; k++) target_get(w[k], 1)->phony = 1;
                free(w);
            }
            cur_targets[ncur_targets++] = t;
            set_default_goal(name);
        }
    }
    free(epat);
    free(pat);
    free(pe);
    free(tl);
    free(tw);
    if (semi_recipe) {
        char *r = trim(semi_recipe);
        if (r[0]) add_recipe_line(r);
        free(r);
    }
}

static void read_makefile(const char *path, int required);

/* ifeq (a,b) / ifeq "a" "b" / ifeq 'a' 'b'  ->  1 when equal */
static int cond_equal(const char *args) {
    char *e = trim(args);
    char *a, *b;
    if (e[0] == '(') {
        size_t n = strlen(e);
        if (e[n - 1] != ')') parse_error("invalid syntax in conditional");
        char *inner = xstrndup(e + 1, n - 2);
        char *parts[3];
        int cnt = split_args(inner, strlen(inner), 2, parts);
        if (cnt != 2) parse_error("invalid syntax in conditional");
        a = expand(parts[0]);
        b = expand(parts[1]);
        free(parts[0]);
        free(parts[1]);
        free(inner);
    } else {
        char q = e[0];
        if (q != '"' && q != '\'') parse_error("invalid syntax in conditional");
        char *end1 = strchr(e + 1, q);
        if (!end1) parse_error("invalid syntax in conditional");
        char *s2 = end1 + 1;
        while (*s2 && is_space(*s2)) s2++;
        char q2 = *s2;
        if (q2 != '"' && q2 != '\'') parse_error("invalid syntax in conditional");
        char *end2 = strchr(s2 + 1, q2);
        if (!end2) parse_error("invalid syntax in conditional");
        char *ra = xstrndup(e + 1, (size_t)(end1 - e - 1)), *rb = xstrndup(s2 + 1, (size_t)(end2 - s2 - 1));
        a = expand(ra);
        b = expand(rb);
        free(ra);
        free(rb);
    }
    const char *ta = a, *tb = b;
    while (*ta && is_space(*ta)) ta++;
    while (*tb && is_space(*tb)) tb++;
    int eq = !strcmp(ta, tb);
    free(a);
    free(b);
    free(e);
    return eq;
}

/* evaluates the condition of ifdef/ifndef/ifeq/ifneq; kw is the directive, args the rest of the line */
static int cond_value(const char *kw, const char *args) {
    if (!strcmp(kw, "ifdef") || !strcmp(kw, "ifndef")) {
        char *en = expand(args);
        char *n = trim(en);
        free(en);
        Var *v = var_find(n, strlen(n));
        int defined = v && v->value[0];
        free(n);
        return !strcmp(kw, "ifdef") ? defined : !defined;
    }
    int eq = cond_equal(args);
    return !strcmp(kw, "ifeq") ? eq : !eq;
}

static int is_cond_word(const char *w) {
    return !strcmp(w, "ifdef") || !strcmp(w, "ifndef") || !strcmp(w, "ifeq") || !strcmp(w, "ifneq");
}

/* the first word of a line and the rest after it */
static char *first_word(const char *line, const char **rest) {
    const char *p = line;
    while (*p && !is_space(*p)) p++;
    char *w = xstrndup(line, (size_t)(p - line));
    while (*p && is_space(*p)) p++;
    *rest = p;
    return w;
}

static Str define_body;
static char *define_name;
static int define_op, define_origin, define_exported, define_depth;

static void finish_define(void) {
    char *body = s_take(&define_body);
    size_t n = strlen(body);
    if (n && body[n - 1] == '\n') body[n - 1] = 0;
    define_variable(define_name, define_op == ':' ? ':' : '=', body, define_origin, define_exported);
    free(body);
    free(define_name);
    define_name = NULL;
}

/* the logical line is `line` (backslash-newlines still in it), modified */
static void process_line(char *line) {
    /* inside define ... endef every line belongs to the body */
    if (define_name) {
        const char *rest;
        char *t = trim(line);
        char *w = first_word(t, &rest);
        if (!strcmp(w, "define")) define_depth++;
        if (!strcmp(w, "endef") && define_depth-- == 0) {
            free(w);
            free(t);
            finish_define();
            return;
        }
        free(w);
        free(t);
        s_puts(&define_body, line);
        s_putc(&define_body, '\n');
        return;
    }

    if (line[0] == '\t' && ncur_targets + ncur_prules > 0) {
        if (!cond_active()) return;
        /* a recipe line: the tab goes, a backslash-newline keeps its meaning for the shell (one tab after it goes too) */
        Str s = {0};
        for (const char *p = line + 1; *p; p++) {
            s_putc(&s, *p);
            if (*p == '\n' && p[1] == '\t') p++;
        }
        char *r = s_take(&s);
        add_recipe_line(r);
        free(r);
        return;
    }

    /* comments and continuation lines */
    Str s = {0};
    for (char *p = line; *p; p++) {
        if (*p == '\\' && p[1] == '\n') {
            while (s.len && is_blank(s.p[s.len - 1])) s.p[--s.len] = 0;
            s_putc(&s, ' ');
            p++;
            while (p[1] && is_blank(p[1])) p++;
        } else if (*p == '\\' && p[1] == '#') {
            s_putc(&s, '#');
            p++;
        } else if (*p == '#') {
            break;
        } else s_putc(&s, *p);
    }
    char *text = s_take(&s);
    char *st = text;
    while (*st && is_space(*st)) st++;
    if (!*st) {
        free(text);
        return;
    }
    end_rule();
    const char *rest;
    char *kw = first_word(st, &rest);
    char *args = trim(rest);

    /* conditionals are read even in a branch that is switched off, to keep the nesting right */
    if (is_cond_word(kw)) {
        if (ncond >= MAX_COND) parse_error("conditionals nested too deeply");
        int parent = cond_active();
        Cond *c = &conds[ncond++];
        c->parent_active = parent;
        c->active = 0;
        c->taken = 0;
        if (c->parent_active) {
            c->active = cond_value(kw, args);
            c->taken = c->active;
        }
        goto done;
    }
    if (!strcmp(kw, "else")) {
        if (!ncond) parse_error("extraneous 'else'");
        Cond *c = &conds[ncond - 1];
        const char *rest2;
        char *kw2 = args[0] ? first_word(args, &rest2) : NULL;
        if (kw2 && is_cond_word(kw2)) {
            char *args2 = trim(rest2);
            if (c->parent_active && !c->taken) {
                c->active = cond_value(kw2, args2);
                c->taken = c->active;
            } else c->active = 0;
            free(args2);
        } else {
            c->active = c->parent_active && !c->taken;
            c->taken = 1;
        }
        free(kw2);
        goto done;
    }
    if (!strcmp(kw, "endif")) {
        if (!ncond) parse_error("extraneous 'endif'");
        ncond--;
        goto done;
    }
    if (!cond_active()) goto done;

    if (!strcmp(kw, "include") || !strcmp(kw, "-include") || !strcmp(kw, "sinclude")) {
        char *names = expand(args);
        int n;
        char **w = split_words(names, &n);
        for (int i = 0; i < n; i++) read_makefile(w[i], kw[0] == 'i');
        free(w);
        free(names);
        goto done;
    }
    if (!strcmp(kw, "define")) {
        /* define NAME [=|:=|::=|+=|?=] */
        char *nm = trim(args);
        int op = '=';
        size_t l = strlen(nm);
        char *eq = strchr(nm, '=');
        if (eq) {
            op = eq > nm && eq[-1] == ':' ? ':' : eq > nm && (eq[-1] == '+' || eq[-1] == '?') ? eq[-1] : '=';
            char *cut = eq;
            while (cut > nm && (cut[-1] == ':' || cut[-1] == '+' || cut[-1] == '?' || is_space(cut[-1]))) cut--;
            nm[cut - nm] = 0;
            (void)l;
        }
        define_name = xstrdup(nm);
        define_op = op;
        define_origin = O_FILE;
        define_exported = 0;
        define_depth = 0;
        free(nm);
        goto done;
    }
    if (!strcmp(kw, "override") || !strcmp(kw, "export") || !strcmp(kw, "unexport")) {
        int origin = !strcmp(kw, "override") ? O_OVERRIDE : O_FILE;
        int exported = !strcmp(kw, "export");
        if (!strcmp(kw, "unexport")) {
            char *names = expand(args);
            int n;
            char **w = split_words(names, &n);
            for (int i = 0; i < n; i++) {
                Var *v = var_find(w[i], strlen(w[i]));
                if (v) v->exported = 0;
            }
            free(w);
            free(names);
            goto done;
        }
        if (exported && !args[0]) {
            export_all = 1;
            goto done;
        }
        int op;
        const char *eq = find_assignment(args, &op);
        if (eq) {
            const char *nameend = eq - (op == '=' ? 0 : 1);
            if (op == ':' ) while (nameend > args && nameend[-1] == ':') nameend--;
            char *nm = xstrndup(args, (size_t)(nameend - args));
            const char *val = eq + 1;
            while (*val && is_blank(*val)) val++;
            define_variable(nm, op, val, origin, exported);
            free(nm);
        } else if (exported) {
            char *names = expand(args);
            int n;
            char **w = split_words(names, &n);
            for (int i = 0; i < n; i++) {
                Var *v = var_find(w[i], strlen(w[i]));
                if (v) v->exported = 1;
            }
            free(w);
            free(names);
        } else parse_error("missing separator");
        goto done;
    }
    if (!strcmp(kw, "vpath") || !strcmp(kw, "undefine") ) {
        if (!strcmp(kw, "undefine")) {
            char *n = expand(args);
            char *t = trim(n);
            var_remove(t);
            free(t);
            free(n);
        } else fprintf(stderr, "%s:%d: warning: vpath is not supported by mmake\n", cur_file, cur_line);
        goto done;
    }

    int op;
    const char *eq = find_assignment(st, &op);
    if (eq) {
        const char *nameend = eq - (op == '=' ? 0 : 1);
        if (op == ':') while (nameend > st && nameend[-1] == ':') nameend--;
        char *nm = xstrndup(st, (size_t)(nameend - st));
        const char *val = eq + 1;
        while (*val && is_blank(*val)) val++;
        define_variable(nm, op, val, O_FILE, 0);
        free(nm);
    } else if (!scan_top(st, ":")) {
        char *e = expand(st);
        char *tt = trim(e);
        int blank = !tt[0];
        free(tt);
        free(e);
        if (!blank) parse_error("missing separator");
    } else {
        parse_rule(st);
    }
done:
    free(kw);
    free(args);
    free(text);
}

static void read_makefile(const char *path, int required) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (required) {
            fflush(stdout);
            if (reading) fprintf(stderr, "%s:%d: %s: No such file or directory\n", cur_file, cur_line, path);
            else fprintf(stderr, "%s: %s: No such file or directory\n", progname, path);
            reading = 0;
            die("No rule to make target '%s'", path);
        }
        return;
    }
    const char *saved_file = cur_file;
    int saved_line = cur_line, saved_reading = reading;
    var_append("MAKEFILE_LIST", path, O_FILE);          /* $(lastword $(MAKEFILE_LIST)) is the file being read, $(firstword ...) the main one */
    cur_file = path;
    cur_line = 0;
    reading = 1;
    char *buf = NULL;
    size_t cap = 0;
    ssize_t got;
    Str logical = {0};
    int phys = 0, start_line = 0, pending = 0;
    while ((got = getline(&buf, &cap, f)) >= 0) {
        phys++;
        if (!pending) start_line = phys;
        size_t len = (size_t)got;
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
        /* a line ending in an odd number of backslashes continues on the next one */
        size_t bs = 0;
        while (bs < len && buf[len - 1 - bs] == '\\') bs++;
        s_putn(&logical, buf, len);
        if (bs % 2 == 1) {
            s_putc(&logical, '\n');
            pending = 1;
            continue;
        }
        pending = 0;
        char *l = s_take(&logical);
        cur_line = start_line;
        process_line(l);
        free(l);
    }
    if (pending) {
        char *l = s_take(&logical);
        cur_line = start_line;
        process_line(l);
        free(l);
    }
    if (define_name) parse_error("missing 'endef', unterminated 'define'");
    free(buf);
    fclose(f);
    cur_file = saved_file;
    cur_line = saved_line;
    reading = saved_reading;
}

/* ------------------------------------------------------------------------------------------------------ the build engine */

static void stat_target(Target *t) {
    struct stat st;
    if (t->stat_done) return;
    t->stat_done = 1;
    if (stat(t->name, &st) == 0) {
        t->exists = 1;
        t->mtime = (long long)st.st_mtime;
    } else {
        t->exists = 0;
        t->mtime = 0;
    }
}

static int prereq_available(const char *name, int depth);

/* The pattern rule that would make `name`: among those that match and whose prerequisites are there (or can be made), the one with the
 * shortest stem, the first written on a tie.  *stem_out receives a malloc'd copy of the stem. */
static PRule *find_prule(const char *name, char **stem_out, int depth) {
    PRule *best = NULL;
    size_t best_len = 0;
    char *best_stem = NULL;
    for (PRule *r = prules; r; r = r->next) {
        const char *stem;
        size_t sl;
        if (!strchr(r->target, '%') || !pat_match(r->target, name, &stem, &sl)) continue;
        int ok = 1;
        for (int k = 0; k < r->npre && ok; k++) {
            Str p = {0};
            pat_apply(&p, r->pre[k], stem, sl);
            ok = prereq_available(p.p ? p.p : "", depth);
            free(p.p);
        }
        if (!ok) continue;
        if (!best || sl < best_len) {
            best = r;
            best_len = sl;
            free(best_stem);
            best_stem = xstrndup(stem, sl);
        }
    }
    if (stem_out) *stem_out = best_stem;
    else free(best_stem);
    return best;
}

static int prereq_available(const char *name, int depth) {
    Target *t = target_get(name, 0);
    struct stat st;
    if (t && (t->is_explicit || t->phony)) return 1;
    if (stat(name, &st) == 0) return 1;
    if (depth < 4) return find_prule(name, NULL, depth + 1) != NULL;
    return 0;
}

/* A target that has no recipe of its own gets one from a pattern rule: the rule's prerequisites come before the ones written for the target. */
static void choose_implicit(Target *t) {
    if (t->has_recipe || t->phony || t->implicit_done) return;
    t->implicit_done = 1;
    char *stem;
    PRule *r = find_prule(t->name, &stem, 0);
    if (!r) return;
    char **np = NULL;
    int nn = 0, ncap = 0;
    for (int k = 0; k < r->npre; k++) {
        Str p = {0};
        pat_apply(&p, r->pre[k], stem, strlen(stem));
        list_add(&np, &nn, &ncap, p.p ? p.p : "");
        free(p.p);
    }
    for (int k = 0; k < t->npre; k++) {
        list_add(&np, &nn, &ncap, t->pre[k]);
        free(t->pre[k]);
    }
    free(t->pre);
    t->pre = np;
    t->npre = nn;
    t->cap_pre = ncap;
    t->recipe = xmalloc((size_t)(r->nrecipe + 1) * sizeof(char *));
    for (int k = 0; k < r->nrecipe; k++) t->recipe[k] = xstrdup(r->recipe[k]);
    t->nrecipe = r->nrecipe;
    t->has_recipe = r->nrecipe > 0;
    free(t->stem);
    t->stem = stem;
}

/* the variables marked for export go into the environment of the recipes */
static void export_variables(void) {
    for (int h = 0; h < NVARS; h++)
        for (Var *v = var_tab[h]; v; v = v->next) {
            if (!v->exported && !(export_all && v->origin != O_DEFAULT)) continue;
            char *val = v->recursive ? expand(v->value) : xstrdup(v->value);
            setenv(v->name, val, 1);
            free(val);
        }
}

/* Does the command line need a shell?  GNU make runs a line without shell syntax directly, and so does mmake: the kernel limits one
 * argument (the whole line, for `sh -c`) to about a kilobyte, and a link line with fifty objects is longer than that. */
static int needs_shell(const char *s) {
    static const char *words[] = {".", ":", "[", "alias", "break", "case", "cd", "continue", "do", "done", "elif", "else", "esac", "eval", "exec",
                                  "exit", "export", "fi", "for", "if", "in", "read", "readonly", "return", "set", "shift", "test", "then", "times",
                                  "trap", "ulimit", "umask", "unalias", "unset", "until", "wait", "while"};
    if (strpbrk(s, "#;\"'`*?[]&|<>(){}$\\\n~")) return 1;
    const char *w = s;
    while (*w && is_blank(*w)) w++;
    size_t n = 0;
    while (w[n] && !is_blank(w[n])) n++;
    if (memchr(w, '=', n)) return 1;                                /* VAR=value command */
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strlen(words[i]) == n && !memcmp(words[i], w, n)) return 1;
    return 0;
}

/* Runs a command line and waits; the wait status, or -1 when nothing could be started.  A line without shell syntax is started by
 * itself; one whose program is not found (a shell built-in the system has no program for) is handed to /bin/sh after all. */
static int run_sh(const char *line) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!needs_shell(line)) {
            int n;
            char **w = split_words(line, &n);
            if (n > 0) {
                execvp(w[0], w);
                if (errno != ENOENT) {
                    fprintf(stderr, "%s: %s: %s\n", progname, w[0], strerror(errno));
                    _exit(errno == EACCES ? 126 : 127);
                }
            }
        }
        char *argv[4];
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)line;
        argv[3] = NULL;
        execv("/bin/sh", argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return st;
}

/* One line of a recipe, after expansion: the @ - + prefixes, the echo, the shell.  Returns 1 when the command failed and that counts. */
static int run_line(Target *t, const char *c) {
    int silent = 0, ignore = 0;
    for (;;) {
        while (*c && is_blank(*c)) c++;
        if (*c == '@') silent = 1;
        else if (*c == '-') ignore = 1;
        else if (*c != '+') break;
        c++;
    }
    if (!*c) return 0;
    commands_started++;
    if (opt_n || (!silent && !opt_s)) {
        printf("%s\n", c);
        fflush(stdout);
    }
    if (opt_n) return 0;
    fflush(stdout);
    fflush(stderr);
    int st = run_sh(c);
    int code = st == -1 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    if (code && (ignore || opt_i)) {
        fprintf(stderr, "%s: [%s] Error %d (ignored)\n", progname, t->name, code);
    } else if (code) {
        fprintf(stderr, "%s: *** [%s] Error %d\n", progname, t->name, code);
        return 1;
    }
    return 0;
}

static int run_recipe(Target *t) {
    export_variables();
    for (int i = 0; i < t->nrecipe; i++) {
        char *e = expand(t->recipe[i]);
        /* a variable that holds several lines makes several recipe lines, each run by its own shell */
        char *seg = e;
        for (char *p = e;; p++) {
            if (*p == '\n' && (p == e || p[-1] != '\\')) {
                *p = 0;
                if (run_line(t, seg)) {
                    free(e);
                    return 1;
                }
                seg = p + 1;
            } else if (!*p) {
                int bad = run_line(t, seg);
                free(e);
                if (bad) return 1;
                break;
            }
        }
    }
    return 0;
}

static void no_rule(Target *t, const char *needed_by) {
    fflush(stdout);
    if (needed_by) fprintf(stderr, "%s: *** No rule to make target '%s', needed by '%s'.", progname, t->name, needed_by);
    else fprintf(stderr, "%s: *** No rule to make target '%s'.", progname, t->name);
    if (!opt_k) {
        fprintf(stderr, "  Stop.\n");
        exit(2);
    }
    fprintf(stderr, "\n");
}

/* Brings one target up to date.  Returns 1 when it (or something it needs) could not be made. */
static int update(Target *t, const char *needed_by) {
    if (t->state == 2) return t->failed;
    if (t->state == 1) {
        fflush(stdout);
        fprintf(stderr, "%s: Circular %s <- %s dependency dropped.\n", progname, needed_by ? needed_by : "?", t->name);
        return 0;
    }
    t->state = 1;
    choose_implicit(t);

    int failed = 0, dep_updated = 0;
    for (int i = 0; i < t->npre && !(failed && !opt_k); i++) {
        Target *p = target_get(t->pre[i], 1);
        if (update(p, t->name)) failed = 1;
        else if (p->updated) dep_updated = 1;
    }
    for (int i = 0; i < t->noo && !(failed && !opt_k); i++) {
        Target *p = target_get(t->oo[i], 1);
        if (update(p, t->name)) failed = 1;
    }
    if (failed) {
        t->failed = 1;
        t->state = 2;
        if (opt_k) {
            fflush(stdout);
            fprintf(stderr, "%s: Target '%s' not remade because of errors.\n", progname, t->name);
        }
        return 1;
    }

    stat_target(t);
    if (!t->has_recipe) {
        /* nothing to run: a file is just there, a target that is only a list of prerequisites passes their state on */
        if (!t->exists && !t->is_explicit && !t->phony) {
            t->failed = 1;
            t->state = 2;
            no_rule(t, needed_by);
            errors_seen = 1;
            return 1;
        }
        t->updated = t->phony || t->exists ? dep_updated : 1;        /* a file that cannot be made counts as always out of date (FORCE: idiom) */
        t->state = 2;
        return 0;
    }

    int must = opt_B || t->phony || !t->exists || dep_updated;
    Str all = {0}, dups = {0}, newer = {0};
    for (int i = 0; i < t->npre; i++) {
        Target *p = target_get(t->pre[i], 1);
        stat_target(p);
        int newer_by_time = t->exists && p->exists && p->mtime > t->mtime;
        if (newer_by_time) must = 1;
        if (!t->exists || p->updated || newer_by_time) put_word(&newer, p->name, strlen(p->name));
        put_word(&dups, p->name, strlen(p->name));
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(t->pre[j], t->pre[i])) seen = 1;
        if (!seen) put_word(&all, p->name, strlen(p->name));
    }
    if (must) {
        autov.target = t->name;
        autov.first = t->npre ? t->pre[0] : "";
        autov.all = all.p ? all.p : "";
        autov.all_dups = dups.p ? dups.p : "";
        autov.newer = newer.p ? newer.p : "";
        autov.stem = t->stem ? t->stem : "";
        int bad = run_recipe(t);
        memset(&autov, 0, sizeof autov);
        if (bad) {
            t->failed = 1;
            t->state = 2;
            errors_seen = 1;
            free(all.p);
            free(dups.p);
            free(newer.p);
            return 1;
        }
        t->updated = 1;
        t->stat_done = 0;
        stat_target(t);
    }
    free(all.p);
    free(dups.p);
    free(newer.p);
    t->state = 2;
    return 0;
}

/* ----------------------------------------------------------------------------------------------------------------- main */

static const char *default_makefile_text =
    "CC = cc\n"
    "AR = mar\n"
    "ARFLAGS = rcs\n"
    "RM = rm -f\n"
    "SHELL = /bin/sh\n"
    "%.o: %.c\n"
    "\t$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c -o $@ $<\n";

static void parse_text(const char *text, int origin_hint) {
    /* the built-in definitions are read as if they were a makefile named <built-in>, with the lowest origin */
    (void)origin_hint;
    const char *saved = cur_file;
    cur_file = "<built-in>";
    reading = 1;
    char *copy = xstrdup(text);
    for (char *p = copy; *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        cur_line++;
        process_line(p);
        if (!nl) break;
        p = nl + 1;
    }
    free(copy);
    end_rule();
    cur_file = saved;
    cur_line = 0;
    reading = 0;
}

static void usage(void) {
    printf("usage: mmake [-f file] [-C dir] [-n] [-s] [-k] [-i] [-B] [-j n] [VAR=value ...] [target ...]\n");
    printf("  -f FILE    read FILE as the makefile (default: makefile or Makefile)\n");
    printf("  -C DIR     change to DIR first;  -n  print the recipes, run none;  -s  do not echo them\n");
    printf("  -k         keep going after an error;  -i  ignore errors of recipes;  -B  make everything\n");
    printf("  -j N       accepted and ignored: mmake runs one recipe at a time\n");
    printf("mmake is a small GNU-flavoured make: variables, functions, pattern rules, conditionals, include (docs/SELFHOST.md).\n");
}

int main(int argc, char **argv) {
    const char *self = strrchr(argv[0], '/');
    progname = self ? self + 1 : argv[0];
    char **files = xmalloc((size_t)(argc + 1) * sizeof(char *));
    char **goals = xmalloc((size_t)(argc + 1) * sizeof(char *));
    char **cmdvars = xmalloc((size_t)(argc + 1) * sizeof(char *));
    char **dirs = xmalloc((size_t)(argc + 1) * sizeof(char *));
    int nfiles = 0, ngoals = 0, ncmdvars = 0, ndirs = 0, no_more_opts = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!no_more_opts && a[0] == '-' && a[1] == '-') {
            if (!a[2]) no_more_opts = 1;
            else if (!strcmp(a, "--version")) {
                printf("mmake %s -- Marisa Kirisame's make, a small GNU-flavoured make for TouhouOS (ze!)\n", MMAKE_VERSION);
                return 0;
            } else if (!strcmp(a, "--help")) {
                usage();
                return 0;
            } else if (!strncmp(a, "--file=", 7)) files[nfiles++] = a + 7;
            else if (!strcmp(a, "--file") && i + 1 < argc) files[nfiles++] = argv[++i];
            else if (!strncmp(a, "--directory=", 12)) dirs[ndirs++] = a + 12;
            else if (!strcmp(a, "--directory") && i + 1 < argc) dirs[ndirs++] = argv[++i];
            else if (!strcmp(a, "--just-print") || !strcmp(a, "--dry-run") || !strcmp(a, "--recon")) opt_n = 1;
            else if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) opt_s = 1;
            else if (!strcmp(a, "--keep-going")) opt_k = 1;
            else if (!strcmp(a, "--ignore-errors")) opt_i = 1;
            else if (!strcmp(a, "--always-make")) opt_B = 1;
            else if (!strncmp(a, "--jobs", 6)) {
                /* ignored, with its number */
                if (!a[6] && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) i++;
            } else die("unrecognized option '%s'", a);
            continue;
        }
        if (!no_more_opts && a[0] == '-' && a[1]) {
            for (char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'f':
                case 'C': {
                    char *arg = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!arg) die("option requires an argument -- '%c'", *p);
                    if (*p == 'f') files[nfiles++] = arg;
                    else dirs[ndirs++] = arg;
                    p += strlen(p) - 1;
                    break;
                }
                case 'j':
                    if (p[1] == 0 && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) i++;
                    while (isdigit((unsigned char)p[1])) p++;
                    break;
                case 'n': opt_n = 1; break;
                case 's': opt_s = 1; break;
                case 'k': opt_k = 1; break;
                case 'i': opt_i = 1; break;
                case 'B': opt_B = 1; break;
                case 'w': case 'S': case 'r': case 'R': case 'e': break;
                case 'h':
                    usage();
                    return 0;
                default:
                    die("invalid option -- '%c'", *p);
                }
            }
            continue;
        }
        if (strchr(a, '=')) cmdvars[ncmdvars++] = a;
        else goals[ngoals++] = a;
    }

    for (int i = 0; i < ndirs; i++) {
        if (chdir(dirs[i]) != 0) die("%s: %s", dirs[i], strerror(errno));
    }
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (ndirs && !opt_s) printf("%s: Entering directory '%s'\n", progname, cwd);

    /* built-in variables and rule, then the environment, then the command line */
    parse_text(default_makefile_text, O_DEFAULT);
    for (int h = 0; h < NVARS; h++)
        for (Var *v = var_tab[h]; v; v = v->next) v->origin = O_DEFAULT;
    var_set("MAKE", argv[0], 2, O_DEFAULT);
    var_set("CURDIR", cwd, 2, O_DEFAULT);
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *name = xstrndup(*e, (size_t)(eq - *e));
        if (strcmp(name, "MAKEFLAGS") && strcmp(name, "SHELL")) var_set(name, eq + 1, 2, O_ENV);
        free(name);
    }
    for (int i = 0; i < ncmdvars; i++) {
        char *eq = strchr(cmdvars[i], '=');
        int op = '=';
        char *nend = eq;
        if (eq > cmdvars[i] && (eq[-1] == ':' || eq[-1] == '+' || eq[-1] == '?')) {
            op = eq[-1];
            nend = eq - 1;
        }
        char *name = xstrndup(cmdvars[i], (size_t)(nend - cmdvars[i]));
        define_variable(name, op, eq + 1, O_CMD, 1);
        free(name);
    }
    if (ngoals) {
        Str g = {0};
        for (int i = 0; i < ngoals; i++) put_word(&g, goals[i], strlen(goals[i]));
        var_set("MAKECMDGOALS", g.p, 2, O_DEFAULT);
        free(g.p);
    }

    if (!nfiles) {
        static const char *candidates[] = {"GNUmakefile", "makefile", "Makefile"};
        for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
            struct stat st;
            if (stat(candidates[i], &st) == 0) {
                files[nfiles++] = (char *)candidates[i];
                break;
            }
        }
        if (!nfiles) die("No targets specified and no makefile found");
    }
    for (int i = 0; i < nfiles; i++) read_makefile(files[i], 1);
    end_rule();

    Target *silent = target_get(".SILENT", 0);
    if (silent && silent->is_explicit && silent->npre == 0) opt_s = 1;
    if (target_get(".ONESHELL", 0) || target_get(".SECONDEXPANSION", 0)) die(".ONESHELL and .SECONDEXPANSION are not supported by mmake");

    if (!ngoals) {
        Var *dg = var_find(".DEFAULT_GOAL", 13);
        const char *g = dg && dg->value[0] ? dg->value : default_goal;
        if (!g) die("No targets");
        goals[ngoals++] = (char *)g;
    }
    int rc = 0;
    for (int i = 0; i < ngoals; i++) {
        Target *t = target_get(goals[i], 1);
        int before = commands_started;
        if (update(t, NULL)) {
            rc = 2;
            if (!opt_k) break;
        } else if (commands_started == before && !opt_s) {
            printf(t->has_recipe && !t->phony ? "%s: '%s' is up to date.\n" : "%s: Nothing to be done for '%s'.\n", progname, t->name);
        }
    }
    fflush(stdout);
    if (ndirs && !opt_s) printf("%s: Leaving directory '%s'\n", progname, cwd);
    return rc;
}
