/* Word expansion: tilde, parameters, command substitution, arithmetic, field splitting, globbing. */
#include "gapsh.h"

extern int g_last_bg;

typedef struct {
    char *s;
    unsigned char *lit;          /* 1 = quoted: never acts as glob or separator */
    size_t n, cap;
    int started;                 /* a field exists even if empty (e.g. "") */
} Field;

static void fld_add(Field *f, char c, int lit) {
    if (f->n + 2 > f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->s = xrealloc(f->s, f->cap);
        f->lit = xrealloc(f->lit, f->cap);
    }
    f->s[f->n] = c;
    f->lit[f->n] = (unsigned char)lit;
    f->n++;
    f->s[f->n] = 0;
}

static void fld_reset(Field *f) {
    f->n = 0;
    f->started = 0;
    if (f->s) f->s[0] = 0;
}

/* ------------------------------------------------------------------ globbing */

int glob_match(const char *p, const char *s) {
    for (;;) {
        switch (*p) {
        case 0:
            return *s == 0;
        case '*':
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (glob_match(p, s)) return 1;
            return glob_match(p, s);
        case '?':
            if (!*s) return 0;
            p++; s++;
            break;
        case '[': {
            if (!*s) return 0;
            const char *q = p + 1;
            int neg = 0, ok = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            int first = 1;
            while (*q && (*q != ']' || first)) {
                first = 0;
                unsigned char lo = (unsigned char)*q;
                if (*q == '\\' && q[1]) lo = (unsigned char)*++q;
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char hi = (unsigned char)q[2];
                    if ((unsigned char)*s >= lo && (unsigned char)*s <= hi) ok = 1;
                    q += 3;
                } else {
                    if ((unsigned char)*s == lo) ok = 1;
                    q++;
                }
            }
            if (!*q) return 0;                          /* unterminated: no match */
            if (ok == neg) return 0;
            p = q + 1;
            s++;
            break;
        }
        case '\\':
            if (p[1]) p++;
            /* fall through */
        default:
            if (*p != *s) return 0;
            p++; s++;
            break;
        }
    }
}

static int has_meta(const char *p) {
    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '*' || *p == '?' || *p == '[') return 1;
    }
    return 0;
}

static char *unescape_pat(const char *p) {
    char *o = xmalloc(strlen(p) + 1);
    size_t n = 0;
    for (; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        o[n++] = *p;
    }
    o[n] = 0;
    return o;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void glob_walk(const char *prefix, char **comps, int idx, int n, StrVec *res) {
    const char *comp = comps[idx];
    int last = idx == n - 1;
    if (!has_meta(comp)) {
        char *lit = unescape_pat(comp);
        char *path = xasprintf("%s%s", prefix, lit);
        free(lit);
        struct stat st;
        /* intermediate components are followed through symlinks (/tmp is one); a final dangling link still counts */
        if ((last ? lstat(path, &st) : stat(path, &st)) == 0) {
            if (last) { sv_push(res, path); return; }
            if (S_ISDIR(st.st_mode)) {
                char *np = xasprintf("%s/", path);
                free(path);
                glob_walk(np, comps, idx + 1, n, res);
                free(np);
                return;
            }
        }
        free(path);
        return;
    }
    DIR *d = opendir(*prefix ? prefix : ".");
    if (!d) return;
    StrVec names = { 0 };
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' && comp[0] != '.') continue;
        if (glob_match(comp, e->d_name)) sv_push(&names, xstrdup(e->d_name));
    }
    closedir(d);
    for (int i = 0; i < names.n; i++) {
        char *path = xasprintf("%s%s", prefix, names.v[i]);
        if (last) { sv_push(res, path); }
        else {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                char *np = xasprintf("%s/", path);
                glob_walk(np, comps, idx + 1, n, res);
                free(np);
            }
            free(path);
        }
        free(names.v[i]);
    }
    free(names.v);
}

static void glob_expand(const char *pat, StrVec *out, const char *fallback) {
    char *copy = xstrdup(pat);
    char *comps[256];
    int n = 0;
    const char *prefix = "";
    char *s = copy;
    if (*s == '/') { prefix = "/"; while (*s == '/') s++; }
    while (*s && n < 256) {
        comps[n++] = s;
        char *slash = strchr(s, '/');
        if (!slash) break;
        *slash = 0;
        s = slash + 1;
        while (*s == '/') s++;
    }
    StrVec res = { 0 };
    if (n) glob_walk(prefix, comps, 0, n, &res);
    if (res.n) {
        qsort(res.v, (size_t)res.n, sizeof(char *), cmp_str);
        for (int i = 0; i < res.n; i++) sv_push(out, res.v[i]);
        free(res.v);
    } else {
        sv_push(out, xstrdup(fallback));
    }
    free(copy);
}

/* ------------------------------------------------------------------ fields */

static void fld_finish(Field *f, StrVec *out, int glob) {
    if (!f->started && f->n == 0) return;
    if (glob) {
        int meta = 0;
        for (size_t i = 0; i < f->n; i++) if (!f->lit[i] && (f->s[i] == '*' || f->s[i] == '?' || f->s[i] == '[')) meta = 1;
        if (meta) {
            char *pat = xmalloc(f->n * 2 + 1);
            size_t k = 0;
            for (size_t i = 0; i < f->n; i++) {
                char c = f->s[i];
                if (f->lit[i] && (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\')) pat[k++] = '\\';
                pat[k++] = c;
            }
            pat[k] = 0;
            glob_expand(pat, out, f->s);
            free(pat);
            fld_reset(f);
            return;
        }
    }
    sv_push(out, xstrdup(f->s ? f->s : ""));
    fld_reset(f);
}

static void emit_value(Field *f, StrVec *out, const char *val, int quoted, int split) {
    if (quoted) f->started = 1;
    if (quoted || !split) {
        for (; *val; val++) fld_add(f, *val, quoted);
        return;
    }
    const char *ifs = var_get("IFS");
    if (!ifs) ifs = " \t\n";
    for (; *val; val++) {
        if (strchr(ifs, *val)) {
            if (f->n || f->started) fld_finish(f, out, 1);
        } else {
            fld_add(f, *val, 0);
        }
    }
}

/* ------------------------------------------------------------------ parameter lookup */

char **g_pos_params;
int g_npos;
char *g_arg0 = "gapsh";
extern int g_status;

static char *param_value(const char *name, int *is_set) {
    *is_set = 1;
    if (!strcmp(name, "?")) return xasprintf("%d", g_status);
    if (!strcmp(name, "$")) return xasprintf("%d", getpid());
    if (!strcmp(name, "#")) return xasprintf("%d", g_npos);
    if (!strcmp(name, "!")) return xasprintf("%d", g_last_bg);
    if (!strcmp(name, "-")) return xstrdup("");
    if (!strcmp(name, "0")) return xstrdup(g_arg0);
    if (isdigit((unsigned char)name[0])) {
        int i = atoi(name);
        if (i >= 1 && i <= g_npos) return xstrdup(g_pos_params[i - 1]);
        *is_set = 0;
        return xstrdup("");
    }
    if (!strcmp(name, "*") || !strcmp(name, "@")) {
        size_t len = 1;
        for (int i = 0; i < g_npos; i++) len += strlen(g_pos_params[i]) + 1;
        char *s = xmalloc(len);
        s[0] = 0;
        for (int i = 0; i < g_npos; i++) { if (i) strcat(s, " "); strcat(s, g_pos_params[i]); }
        return s;
    }
    const char *v = var_get(name);
    if (!v) { *is_set = 0; return xstrdup(""); }
    return xstrdup(v);
}

/* Removes the shortest/longest prefix or suffix matching pat. */
static char *trim_pattern(const char *val, const char *pat, int suffix, int longest) {
    size_t n = strlen(val);
    for (size_t k = 0; k <= n; k++) {
        if (!suffix) {                                  /* test val[0..i): longest prefix = largest i */
            size_t i = longest ? n - k : k;
            char *head = xasprintf("%.*s", (int)i, val);
            int m = glob_match(pat, head);
            free(head);
            if (m) return xstrdup(val + i);
        } else {                                        /* test val[i..n): longest suffix = smallest i */
            size_t i = longest ? k : n - k;
            if (glob_match(pat, val + i)) return xasprintf("%.*s", (int)i, val);
        }
    }
    return xstrdup(val);
}

static char *expand_pat(const char *w) {
    /* a pattern word keeps its quoting so quoted glob characters stay literal: expand without losing '\' escapes */
    char *s = expand_string(w);
    return s;
}

static char *param_expand(const char *spec, int *ok) {
    /* spec is the text between ${ and } */
    *ok = 1;
    if (spec[0] == '#' && spec[1]) {
        int set;
        char *v = param_value(spec + 1, &set);
        char *r = xasprintf("%zu", strlen(v));
        free(v);
        return r;
    }
    size_t nl = 0;
    if (isdigit((unsigned char)spec[0])) while (isdigit((unsigned char)spec[nl])) nl++;
    else if (strchr("?$#!@*-", spec[0])) nl = 1;
    else while (isalnum((unsigned char)spec[nl]) || spec[nl] == '_') nl++;
    char *name = xmalloc(nl + 1);
    memcpy(name, spec, nl);
    name[nl] = 0;
    const char *op = spec + nl;
    int set;
    char *val = param_value(name, &set);
    int empty = !val[0];
    char *result = NULL;

    if (!*op) {
        result = val;
    } else if (op[0] == ':' && (op[1] == '-' || op[1] == '=' || op[1] == '+' || op[1] == '?')) {
        char c = op[1];
        const char *w = op + 2;
        if (c == '-') result = (!set || empty) ? expand_string(w) : val;
        else if (c == '=') {
            if (!set || empty) { result = expand_string(w); var_set(name, result); } else result = val;
        } else if (c == '+') result = (set && !empty) ? expand_string(w) : xstrdup("");
        else {
            if (!set || empty) {
                char *m = expand_string(w);
                fprintf(stderr, "gapsh: %s: %s\n", name, *m ? m : "parameter null or not set");
                free(m);
                *ok = 0;
                result = xstrdup("");
            } else result = val;
        }
        if (result != val) free(val);
    } else if (op[0] == '-' || op[0] == '=' || op[0] == '+') {
        const char *w = op + 1;
        if (op[0] == '-') result = set ? val : expand_string(w);
        else if (op[0] == '=') { if (!set) { result = expand_string(w); var_set(name, result); } else result = val; }
        else result = set ? expand_string(w) : xstrdup("");
        if (result != val) free(val);
    } else if (op[0] == '#' || op[0] == '%') {
        int longest = op[1] == op[0];
        char *pat = expand_pat(op + (longest ? 2 : 1));
        result = trim_pattern(val, pat, op[0] == '%', longest);
        free(pat);
        free(val);
    } else if (op[0] == '/') {
        int all = op[1] == '/';
        const char *rest = op + (all ? 2 : 1);
        const char *slash = strchr(rest, '/');
        char *pat = slash ? xasprintf("%.*s", (int)(slash - rest), rest) : xstrdup(rest);
        char *rep = slash ? expand_string(slash + 1) : xstrdup("");
        char *pp = expand_pat(pat);
        size_t vl = strlen(val);
        char *out = xmalloc(vl * (strlen(rep) + 1) + 1);
        size_t o = 0, i = 0;
        int done = 0;
        while (i <= vl) {
            size_t best = 0;
            if (!done || all) {
                for (size_t len = vl - i; len > 0; len--) {
                    char *piece = xasprintf("%.*s", (int)len, val + i);
                    int m = glob_match(pp, piece);
                    free(piece);
                    if (m) { best = len; break; }
                }
            }
            if (best) { memcpy(out + o, rep, strlen(rep)); o += strlen(rep); i += best; done = 1; }
            else { if (i < vl) out[o++] = val[i]; i++; }
        }
        out[o] = 0;
        result = out;
        free(pat); free(rep); free(pp); free(val);
    } else if (op[0] == ':') {
        long off = strtol(op + 1, (char **)&op, 10);
        long len = -1;
        if (*op == ':') len = strtol(op + 1, NULL, 10);
        long vl = (long)strlen(val);
        if (off < 0) off += vl;
        if (off < 0) off = 0;
        if (off > vl) off = vl;
        if (len < 0 || off + len > vl) len = vl - off;
        result = xasprintf("%.*s", (int)len, val + off);
        free(val);
    } else {
        result = val;
    }
    free(name);
    return result;
}

/* ------------------------------------------------------------------ arithmetic */

typedef struct { const char *p; int ok; } Arith;

static long arith_expr(Arith *a, int minprec);

static void a_skip(Arith *a) { while (*a->p == ' ' || *a->p == '\t') a->p++; }

static long a_primary(Arith *a) {
    a_skip(a);
    if (*a->p == '(') {
        a->p++;
        long v = arith_expr(a, 0);
        a_skip(a);
        if (*a->p == ')') a->p++; else a->ok = 0;
        return v;
    }
    if (*a->p == '-') { a->p++; return -a_primary(a); }
    if (*a->p == '+') { a->p++; return a_primary(a); }
    if (*a->p == '!') { a->p++; return !a_primary(a); }
    if (*a->p == '~') { a->p++; return ~a_primary(a); }
    if (isdigit((unsigned char)*a->p)) {
        char *end;
        long v = strtol(a->p, &end, 0);
        a->p = end;
        return v;
    }
    if (isalpha((unsigned char)*a->p) || *a->p == '_') {
        const char *s = a->p;
        while (isalnum((unsigned char)*a->p) || *a->p == '_') a->p++;
        char *name = xasprintf("%.*s", (int)(a->p - s), s);
        a_skip(a);
        const char *v = var_get(name);
        long cur = v ? strtol(v, NULL, 0) : 0;
        long res = cur;
        if (a->p[0] == '+' && a->p[1] == '+') { a->p += 2; char b[32]; snprintf(b, sizeof b, "%ld", cur + 1); var_set(name, b); }
        else if (a->p[0] == '-' && a->p[1] == '-') { a->p += 2; char b[32]; snprintf(b, sizeof b, "%ld", cur - 1); var_set(name, b); }
        else if (a->p[0] == '=' && a->p[1] != '=') {
            a->p++;
            res = arith_expr(a, 0);
            char b[32];
            snprintf(b, sizeof b, "%ld", res);
            var_set(name, b);
        } else if (strchr("+-*/%", a->p[0]) && a->p[1] == '=') {
            char op = a->p[0];
            a->p += 2;
            long rhs = arith_expr(a, 0);
            res = op == '+' ? cur + rhs : op == '-' ? cur - rhs : op == '*' ? cur * rhs : op == '/' ? (rhs ? cur / rhs : 0) : (rhs ? cur % rhs : 0);
            char b[32];
            snprintf(b, sizeof b, "%ld", res);
            var_set(name, b);
        }
        free(name);
        return res;
    }
    a->ok = 0;
    return 0;
}

static int a_prec(const char *p, int *len) {
    static const struct { const char *op; int prec; } ops[] = {
        { "||", 1 }, { "&&", 2 }, { "|", 3 }, { "^", 4 }, { "&", 5 }, { "==", 6 }, { "!=", 6 }, { "<=", 7 }, { ">=", 7 },
        { "<<", 8 }, { ">>", 8 }, { "<", 7 }, { ">", 7 }, { "+", 9 }, { "-", 9 }, { "*", 10 }, { "/", 10 }, { "%", 10 },
    };
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        size_t l = strlen(ops[i].op);
        if (!strncmp(p, ops[i].op, l)) { *len = (int)l; return ops[i].prec; }
    }
    return -1;
}

static long arith_expr(Arith *a, int minprec) {
    long lhs = a_primary(a);
    for (;;) {
        a_skip(a);
        if (*a->p == '?' && minprec == 0) {
            a->p++;
            long t = arith_expr(a, 0);
            a_skip(a);
            if (*a->p == ':') a->p++; else a->ok = 0;
            long f = arith_expr(a, 0);
            lhs = lhs ? t : f;
            continue;
        }
        int len;
        int prec = a_prec(a->p, &len);
        if (prec < 0 || prec < minprec) break;
        char op[3] = { a->p[0], len > 1 ? a->p[1] : 0, 0 };
        a->p += len;
        long rhs = arith_expr(a, prec + 1);
        if (!strcmp(op, "||")) lhs = lhs || rhs;
        else if (!strcmp(op, "&&")) lhs = lhs && rhs;
        else if (!strcmp(op, "|")) lhs |= rhs;
        else if (!strcmp(op, "^")) lhs ^= rhs;
        else if (!strcmp(op, "&")) lhs &= rhs;
        else if (!strcmp(op, "==")) lhs = lhs == rhs;
        else if (!strcmp(op, "!=")) lhs = lhs != rhs;
        else if (!strcmp(op, "<=")) lhs = lhs <= rhs;
        else if (!strcmp(op, ">=")) lhs = lhs >= rhs;
        else if (!strcmp(op, "<<")) lhs <<= rhs;
        else if (!strcmp(op, ">>")) lhs >>= rhs;
        else if (!strcmp(op, "<")) lhs = lhs < rhs;
        else if (!strcmp(op, ">")) lhs = lhs > rhs;
        else if (!strcmp(op, "+")) lhs += rhs;
        else if (!strcmp(op, "-")) lhs -= rhs;
        else if (!strcmp(op, "*")) lhs *= rhs;
        else if (!strcmp(op, "/")) { if (!rhs) { a->ok = 0; return 0; } lhs /= rhs; }
        else if (!strcmp(op, "%")) { if (!rhs) { a->ok = 0; return 0; } lhs %= rhs; }
    }
    return lhs;
}

long arith_eval(const char *expr, int *ok) {
    Arith a = { expr, 1 };
    long v = arith_expr(&a, 0);
    a_skip(&a);
    if (*a.p) a.ok = 0;
    if (ok) *ok = a.ok;
    return v;
}

/* ------------------------------------------------------------------ the main expansion loop */

static size_t find_close(const char *w, size_t i, char open, char close) {
    /* w[i] is the opening character; returns the index of the matching close (or strlen on failure) */
    int depth = 0;
    for (; w[i]; i++) {
        unsigned char c = (unsigned char)w[i];
        if (c == Q_ESC && w[i + 1]) { i++; continue; }
        if (w[i] == open) depth++;
        else if (w[i] == close && --depth == 0) return i;
    }
    return i;
}

static char *strip_for_exec(const char *s, size_t n) {
    /* command text for $(...) keeps its quoting as ordinary characters: markers never occur inside it (lexer copied it raw) */
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static size_t expand_dollar(const char *w, size_t i, Field *f, StrVec *out, int quoted, int split) {
    char c = w[i + 1];
    if (c == '(' && w[i + 2] == '(') {
        size_t close = find_close(w, i + 1, '(', ')');
        if (w[close]) {
            char *expr = strip_for_exec(w + i + 3, close - i - 4 > 0 ? close - i - 4 : 0);
            char *e2 = expand_string(expr);
            int ok;
            long v = arith_eval(e2, &ok);
            if (!ok) fprintf(stderr, "gapsh: arithmetic syntax error: %s\n", e2);
            char b[32];
            snprintf(b, sizeof b, "%ld", v);
            emit_value(f, out, b, quoted, split);
            free(expr); free(e2);
            return close;
        }
    }
    if (c == '(') {
        size_t close = find_close(w, i + 1, '(', ')');
        char *cmd = strip_for_exec(w + i + 2, close - i - 2);
        char *res = run_capture(cmd);
        emit_value(f, out, res, quoted, split);
        free(cmd); free(res);
        return w[close] ? close : close - 1;
    }
    if (c == '{') {
        size_t close = find_close(w, i + 1, '{', '}');
        char *spec = strip_for_exec(w + i + 2, close - i - 2);
        int ok;
        char *v = param_expand(spec, &ok);
        emit_value(f, out, v, quoted, split);
        free(spec); free(v);
        return w[close] ? close : close - 1;
    }
    if (c == '@' && quoted) {
        for (int k = 0; k < g_npos; k++) {
            if (k) fld_finish(f, out, 0);
            emit_value(f, out, g_pos_params[k], 1, 0);
            f->started = 1;
        }
        return i + 1;
    }
    char name[64];
    size_t nl = 0;
    if (isdigit((unsigned char)c)) { name[nl++] = c; }
    else if (strchr("?$#!@*-", c) && c) { name[nl++] = c; }
    else if (isalpha((unsigned char)c) || c == '_') {
        while ((isalnum((unsigned char)w[i + 1 + nl]) || w[i + 1 + nl] == '_') && nl < sizeof name - 1) { name[nl] = w[i + 1 + nl]; nl++; }
    } else {
        fld_add(f, '$', quoted);
        return i;
    }
    name[nl] = 0;
    int set;
    char *v = param_value(name, &set);
    emit_value(f, out, v, quoted, split);
    free(v);
    return i + nl;
}

static const char *home_of(const char *user) {
    if (!*user) return var_get("HOME");
    static char home[256];
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NULL;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fields[8];
        int n = 0;
        for (char *p = line, *tok; n < 7 && (tok = strsep(&p, ":")); ) fields[n++] = tok;
        if (n >= 6 && !strcmp(fields[0], user)) { strlcpy(home, fields[5], sizeof home); fclose(f); return home; }
    }
    fclose(f);
    return NULL;
}

static void expand_one(const char *w, StrVec *out, int split) {
    Field f = { 0 };
    int q = 0;
    for (size_t i = 0; w[i]; i++) {
        unsigned char c = (unsigned char)w[i];
        switch (c) {
        case Q_SQ_BEGIN:
            f.started = 1;
            for (i++; w[i] && (unsigned char)w[i] != Q_SQ_END; i++) fld_add(&f, w[i], 1);
            break;
        case Q_DQ_BEGIN: q = 1; f.started = 1; break;
        case Q_DQ_END: q = 0; break;
        case Q_ESC:
            if (w[i + 1]) { i++; fld_add(&f, w[i], 1); f.started = 1; }
            break;
        case '$':
            i = expand_dollar(w, i, &f, out, q, split);
            break;
        case '`': {
            size_t j = i + 1;
            while (w[j] && w[j] != '`') { if (w[j] == '\\' && w[j + 1]) j++; j++; }
            char *cmd = strip_for_exec(w + i + 1, j - i - 1);
            char *res = run_capture(cmd);
            emit_value(&f, out, res, q, split);
            free(cmd); free(res);
            i = w[j] ? j : j - 1;
            break;
        }
        case '~':
            if (!q && i == 0 && f.n == 0) {
                size_t j = 1;
                while (w[j] && w[j] != '/' && (unsigned char)w[j] > 5) j++;
                char *user = xasprintf("%.*s", (int)(j - 1), w + 1);
                const char *home = home_of(user);
                free(user);
                if (home) {
                    for (const char *h = home; *h; h++) fld_add(&f, *h, 1);
                    f.started = 1;
                    i = j - 1;
                    break;
                }
            }
            fld_add(&f, '~', q);
            break;
        default:
            fld_add(&f, (char)c, q);
            break;
        }
    }
    fld_finish(&f, out, split);
    free(f.s);
    free(f.lit);
}

char **expand_words(char **words, int n, int *out_n) {
    StrVec out = { 0 };
    out.v = xmalloc(8 * sizeof(char *));
    out.v[0] = NULL;
    out.cap = 8;
    for (int i = 0; i < n; i++) expand_one(words[i], &out, 1);
    if (out_n) *out_n = out.n;
    return out.v;
}

char *expand_string(const char *w) {
    StrVec out = { 0 };
    expand_one(w, &out, 0);
    size_t len = 1;
    for (int i = 0; i < out.n; i++) len += strlen(out.v[i]) + 1;
    char *r = xmalloc(len);
    r[0] = 0;
    for (int i = 0; i < out.n; i++) { if (i) strcat(r, " "); strcat(r, out.v[i]); free(out.v[i]); }
    free(out.v);
    return r;
}

char *expand_heredoc(const char *body) {
    char *w = xmalloc(strlen(body) * 2 + 4);
    size_t n = 0;
    w[n++] = Q_DQ_BEGIN;
    for (const char *p = body; *p; p++) {
        if (*p == '\\' && (p[1] == '$' || p[1] == '`' || p[1] == '\\')) { w[n++] = Q_ESC; w[n++] = *++p; continue; }
        if (*p == '\\' && p[1] == '\n') { p++; continue; }
        if ((unsigned char)*p < 6) continue;
        w[n++] = *p;
    }
    w[n++] = Q_DQ_END;
    w[n] = 0;
    char *r = expand_string(w);
    free(w);
    return r;
}

char *run_capture(const char *cmd) {
    int fds[2];
    if (pipe(fds) < 0) return xstrdup("");
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return xstrdup(""); }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        int st = run_string(cmd, "(substitution)");
        fflush(NULL);
        _exit(st);
    }
    close(fds[1]);
    size_t cap = 256, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len + 128 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
        ssize_t r = read(fds[0], buf + len, cap - len - 1);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        len += (size_t)r;
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    g_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    while (len && buf[len - 1] == '\n') len--;
    buf[len] = 0;
    return buf;
}
