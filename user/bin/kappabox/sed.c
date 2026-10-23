/* sed: the stream editor. Addresses (N, $, /re/, first~step, a,b, a,+N, 0,/re/, !), commands { } s y d D p P n N q Q h H g G x a i c = l b t T : r w. */
#include "kappabox.h"
#include <regex.h>

typedef struct {
    int kind;                       /* 0 none, 1 line, 2 last, 3 regex, 4 first~step, 5 +N (second address only) */
    long n, step;
    regex_t re;
} Addr;

typedef struct {
    Addr a1, a2;
    int negate;
    char name;
    char *text;                     /* a i c text, r/w file, b/t/T/: label */
    regex_t re;
    int has_re;                     /* s: pattern compiled (empty pattern = reuse last) */
    char *repl;
    int s_global, s_print, s_nth, s_write_fd;
    unsigned char *ytab;
    int target;                     /* branch destination, or index of matching } */
    int in_range;
    long range_end;
} Cmd;

static Cmd *g_cmds;
static int g_ncmds;
static int g_quiet, g_extended, g_separate, g_inplace;
static const char *g_suffix;
static regex_t *g_last_re;

/* ------------------------------------------------------------------ script text */

typedef struct { const char *p; } Src;

static void sed_die(const char *msg, const char *ctx) {
    fprintf(stderr, "sed: -e expression: %s%s%s\n", msg, ctx ? ": " : "", ctx ? ctx : "");
    exit(1);
}

static void skip_ws(Src *s) { while (*s->p == ' ' || *s->p == '\t') s->p++; }
static void skip_sep(Src *s) { while (*s->p == ' ' || *s->p == '\t' || *s->p == ';' || *s->p == '\n') s->p++; }

/* Reads text up to the unescaped delimiter; "\delim" becomes the bare delimiter, "\n" becomes a newline. */
static char *read_delimited(Src *s, char delim, int regex_mode) {
    size_t cap = 64, n = 0;
    char *out = malloc(cap);
    int in_bracket = 0;
    while (*s->p && (*s->p != delim || in_bracket)) {
        if (n + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
        if (regex_mode && *s->p == '[' && !in_bracket) {
            in_bracket = 1;
            out[n++] = *s->p++;
            if (*s->p == '^') out[n++] = *s->p++;
            if (*s->p == ']') out[n++] = *s->p++;
            continue;
        }
        if (in_bracket && *s->p == ']') { in_bracket = 0; out[n++] = *s->p++; continue; }
        if (*s->p == '\\' && s->p[1]) {
            if (s->p[1] == delim) { out[n++] = delim; s->p += 2; continue; }
            if (s->p[1] == 'n' && regex_mode) { out[n++] = '\n'; s->p += 2; continue; }
            out[n++] = *s->p++;
            out[n++] = *s->p++;
            continue;
        }
        out[n++] = *s->p++;
    }
    if (*s->p != delim) sed_die("unterminated `s' command", NULL);
    s->p++;
    out[n] = 0;
    return out;
}

static void compile_re(regex_t *re, const char *pat, int icase) {
    int rc = regcomp(re, pat, (g_extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0));
    if (rc) { char msg[100]; regerror(rc, re, msg, sizeof msg); sed_die(msg, pat); }
}

static int parse_addr(Src *s, Addr *a) {
    memset(a, 0, sizeof *a);
    if (isdigit((unsigned char)*s->p)) {
        a->n = strtol(s->p, (char **)&s->p, 10);
        if (*s->p == '~') { s->p++; a->step = strtol(s->p, (char **)&s->p, 10); a->kind = 4; }
        else a->kind = 1;
        return 1;
    }
    if (*s->p == '$') { s->p++; a->kind = 2; return 1; }
    if (*s->p == '/' || *s->p == '\\') {
        char delim = '/';
        if (*s->p == '\\') { s->p++; delim = *s->p; }
        s->p++;
        char *pat = read_delimited(s, delim, 1);
        int icase = 0;
        while (*s->p == 'I') { icase = 1; s->p++; }
        if (*pat) { compile_re(&a->re, pat, icase); }
        a->kind = 3;
        a->n = *pat ? 0 : 1;                        /* 1: empty regex means the last one used */
        free(pat);
        return 1;
    }
    return 0;
}

static char *read_label(Src *s) {
    skip_ws(s);
    const char *b = s->p;
    while (*s->p && *s->p != '\n' && *s->p != ';' && *s->p != ' ' && *s->p != '}') s->p++;
    return strndup(b, (size_t)(s->p - b));
}

/* Text of a, i, c: either "a\<newline>text" or the GNU one-liner "a text". Backslash-newline continues the text. */
static char *read_text(Src *s) {
    skip_ws(s);
    if (*s->p == '\\') {
        s->p++;
        skip_ws(s);
        if (*s->p == '\n') s->p++;
    }
    size_t cap = 64, n = 0;
    char *out = malloc(cap);
    while (*s->p && *s->p != '\n') {
        if (n + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
        if (*s->p == '\\' && s->p[1]) { s->p++; if (*s->p == '\n') { out[n++] = '\n'; s->p++; continue; } }
        out[n++] = *s->p++;
    }
    out[n++] = '\n';
    out[n] = 0;
    return out;
}

static void compile(const char *script) {
    Src s = { script };
    int brace_stack[64], nbrace = 0;
    for (;;) {
        skip_sep(&s);
        if (!*s.p) break;
        if (*s.p == '#') { while (*s.p && *s.p != '\n') s.p++; continue; }
        g_cmds = realloc(g_cmds, (size_t)(g_ncmds + 1) * sizeof(Cmd));
        Cmd *c = &g_cmds[g_ncmds];
        memset(c, 0, sizeof *c);
        c->target = -1;
        c->s_write_fd = -1;
        if (parse_addr(&s, &c->a1)) {
            if (*s.p == ',') {
                s.p++;
                if (*s.p == '+') { s.p++; c->a2.kind = 5; c->a2.n = strtol(s.p, (char **)&s.p, 10); }
                else if (!parse_addr(&s, &c->a2)) sed_die("unexpected `,'", NULL);
            }
        }
        skip_ws(&s);
        while (*s.p == '!') { c->negate = 1; s.p++; skip_ws(&s); }
        c->name = *s.p++;
        switch (c->name) {
        case '{':
            if (nbrace >= 64) sed_die("too many nested {", NULL);
            brace_stack[nbrace++] = g_ncmds;
            break;
        case '}':
            if (!nbrace) sed_die("unexpected `}'", NULL);
            g_cmds[brace_stack[--nbrace]].target = g_ncmds;
            break;
        case 's': {
            char delim = *s.p++;
            char *pat = read_delimited(&s, delim, 1);
            c->repl = read_delimited(&s, delim, 0);
            int icase = 0;
            for (;; s.p++) {
                if (*s.p == 'g') c->s_global = 1;
                else if (*s.p == 'p') c->s_print = 1;
                else if (*s.p == 'i' || *s.p == 'I') icase = 1;
                else if (isdigit((unsigned char)*s.p)) { c->s_nth = (int)strtol(s.p, (char **)&s.p, 10); s.p--; }
                else if (*s.p == 'w') { s.p++; skip_ws(&s); const char *b = s.p; while (*s.p && *s.p != '\n') s.p++; char *fn = strndup(b, (size_t)(s.p - b)); c->s_write_fd = !strcmp(fn, "/dev/stdout") ? 1 : open(fn, O_WRONLY | O_CREAT | O_TRUNC, 0644); free(fn); s.p--; }
                else break;
            }
            if (*pat) { compile_re(&c->re, pat, icase); c->has_re = 1; }
            free(pat);
            break;
        }
        case 'y': {
            char delim = *s.p++;
            char *from = read_delimited(&s, delim, 0), *to = read_delimited(&s, delim, 0);
            c->ytab = malloc(256);
            for (int i = 0; i < 256; i++) c->ytab[i] = (unsigned char)i;
            const char *f = from, *t = to;
            while (*f && *t) {
                int a = (unsigned char)*f++, b = (unsigned char)*t++;
                if (a == '\\' && *f) { a = *f++ == 'n' ? '\n' : f[-1]; }
                if (b == '\\' && *t) { b = *t++ == 'n' ? '\n' : t[-1]; }
                c->ytab[a] = (unsigned char)b;
            }
            free(from); free(to);
            break;
        }
        case 'a': case 'i': case 'c': c->text = read_text(&s); break;
        case 'b': case 't': case 'T': case ':': c->text = read_label(&s); break;
        case 'r': case 'w': {
            skip_ws(&s);
            const char *b = s.p;
            while (*s.p && *s.p != '\n') s.p++;
            c->text = strndup(b, (size_t)(s.p - b));
            if (c->name == 'w') c->s_write_fd = !strcmp(c->text, "/dev/stdout") ? 1 : open(c->text, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            break;
        }
        case 'q': case 'Q': case 'l': case 'L':
            skip_ws(&s);
            if (isdigit((unsigned char)*s.p)) c->s_nth = (int)strtol(s.p, (char **)&s.p, 10);
            break;
        case 'd': case 'D': case 'p': case 'P': case 'n': case 'N': case 'h': case 'H': case 'g': case 'G': case 'x': case '=': case 'z': break;
        default: {
            char tmp[2] = { c->name, 0 };
            sed_die("unknown command", tmp);
        }
        }
        g_ncmds++;
    }
    if (nbrace) sed_die("unmatched `{'", NULL);
    for (int i = 0; i < g_ncmds; i++) {                 /* resolve labels */
        Cmd *c = &g_cmds[i];
        if (c->name != 'b' && c->name != 't' && c->name != 'T') continue;
        c->target = g_ncmds;
        if (!*c->text) continue;
        int found = 0;
        for (int j = 0; j < g_ncmds; j++)
            if (g_cmds[j].name == ':' && !strcmp(g_cmds[j].text, c->text)) { c->target = j; found = 1; break; }
        if (!found) sed_die("can't find label for jump to", c->text);
    }
}

/* ------------------------------------------------------------------ execution */

typedef struct {
    char **files;
    int nfiles, idx;
    FILE *cur;
    char *look;                     /* one line of lookahead */
    size_t looklen;
    int have_look;
    long lineno;
    const char *curname;
} Input;

static Input g_in;
static FILE *g_out;
static char *g_ps, *g_hs;
static size_t g_psl, g_hsl, g_pscap, g_hscap;

static void buf_set(char **b, size_t *len, size_t *cap, const char *s, size_t n) {
    if (n + 1 > *cap) { *cap = n + 64; *b = realloc(*b, *cap); }
    memcpy(*b, s, n);
    (*b)[n] = 0;
    *len = n;
}

static void buf_append(char **b, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) { *cap = *len + n + 64; *b = realloc(*b, *cap); }
    memcpy(*b + *len, s, n);
    *len += n;
    (*b)[*len] = 0;
}

static void open_next(void);

/* Fetches the next raw line into the lookahead slot, moving on to the next file when one runs out. */
static int fill_look(void) {
    static char *buf;
    static size_t cap;
    for (;;) {
        if (!g_in.cur) {
            if (g_in.idx >= g_in.nfiles) return 0;
            open_next();
            continue;
        }
        ssize_t n = getline(&buf, &cap, g_in.cur);
        if (n >= 0) {
            if (n && buf[n - 1] == '\n') n--;
            g_in.look = realloc(g_in.look, (size_t)n + 1);
            memcpy(g_in.look, buf, (size_t)n);
            g_in.look[n] = 0;
            g_in.looklen = (size_t)n;
            g_in.have_look = 1;
            return 1;
        }
        if (g_in.cur != stdin) fclose(g_in.cur);
        g_in.cur = NULL;
        if (g_separate) return 0;
    }
}

static int g_rc;

static void open_next(void) {
    const char *name = g_in.files[g_in.idx++];
    g_in.curname = name;
    if (!strcmp(name, "-")) { g_in.cur = stdin; return; }
    g_in.cur = fopen(name, "r");
    if (!g_in.cur) { kb_error("can't read %s: %s", name, strerror(errno)); g_rc = 2; }
}

static int read_line(void) {
    if (!g_in.have_look && !fill_look()) return 0;
    buf_set(&g_ps, &g_psl, &g_pscap, g_in.look, g_in.looklen);
    g_in.have_look = 0;
    g_in.lineno++;
    return 1;
}

static int is_last(void) {
    if (g_in.have_look) return 0;
    return !fill_look();
}

static int addr_match(Cmd *c, Addr *a) {
    switch (a->kind) {
    case 1: return g_in.lineno == a->n;
    case 2: return is_last();
    case 3: {
        regex_t *re = a->n ? g_last_re : &a->re;
        if (!re) return 0;
        g_last_re = re;
        return regexec(re, g_ps, 0, NULL, 0) == 0;
    }
    case 4: return a->step ? (g_in.lineno >= a->n && (g_in.lineno - a->n) % a->step == 0) : g_in.lineno == a->n;
    }
    (void)c;
    return 1;
}

static int selected(Cmd *c) {
    int r;
    if (!c->a1.kind) r = 1;
    else if (!c->a2.kind) r = addr_match(c, &c->a1);
    else if (c->in_range) {
        r = 1;
        if (c->a2.kind == 1 || c->a2.kind == 5) { if (g_in.lineno >= c->range_end) c->in_range = 0; }
        else if (addr_match(c, &c->a2)) c->in_range = 0;
    } else {
        r = c->a1.kind == 1 && c->a1.n == 0 ? 0 : addr_match(c, &c->a1);
        if (c->a1.kind == 1 && c->a1.n == 0 && c->a2.kind == 3) { c->a1.n = -1; c->in_range = 1; }     /* 0,/re/ */
        if (r) {
            if (c->a2.kind == 1) { if (c->a2.n > g_in.lineno) { c->in_range = 1; c->range_end = c->a2.n; } }
            else if (c->a2.kind == 5) { if (c->a2.n > 0) { c->in_range = 1; c->range_end = g_in.lineno + c->a2.n; } }
            else if (c->a2.kind == 2) { c->in_range = !is_last(); }
            else c->in_range = 1;
        }
    }
    return c->negate ? !r : r;
}

static void out_ps(FILE *f) { fwrite(g_ps, 1, g_psl, f); fputc('\n', f); }

static void do_subst(Cmd *c, int *replaced) {
    regex_t *re = c->has_re ? &c->re : g_last_re;
    if (!re) sed_die("no previous regular expression", NULL);
    g_last_re = re;
    size_t cap = g_psl + 64, n = 0, pos = 0;
    char *out = malloc(cap);
    int count = 0;
    regmatch_t m[10];
    *replaced = 0;
    while (pos <= g_psl) {
        if (regexec(re, g_ps + pos, 10, m, pos ? REG_NOTBOL : 0) != 0) break;
        size_t ms = pos + (size_t)m[0].rm_so, me = pos + (size_t)m[0].rm_eo;
        count++;
        size_t need = n + (ms - pos) + strlen(c->repl) + g_psl + 16;
        if (need > cap) { cap = need * 2; out = realloc(out, cap); }
        memcpy(out + n, g_ps + pos, ms - pos);
        n += ms - pos;
        if (!c->s_nth || count >= c->s_nth) {
            *replaced = 1;
            for (const char *r = c->repl; *r; r++) {
                if (n + g_psl + 8 > cap) { cap = (n + g_psl + 8) * 2; out = realloc(out, cap); }
                if (*r == '&') { memcpy(out + n, g_ps + ms, me - ms); n += me - ms; }
                else if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
                    int k = r[1] - '0';
                    r++;
                    if (m[k].rm_so >= 0) { size_t len = (size_t)(m[k].rm_eo - m[k].rm_so); memcpy(out + n, g_ps + pos + m[k].rm_so, len); n += len; }
                } else if (*r == '\\' && r[1] == 'n') { out[n++] = '\n'; r++; }
                else if (*r == '\\' && r[1] == 't') { out[n++] = '\t'; r++; }
                else if (*r == '\\' && r[1]) { out[n++] = r[1]; r++; }
                else out[n++] = *r;
            }
            if (!c->s_global) { pos = me; if (me == ms && ms < g_psl) { out[n++] = g_ps[ms]; pos = ms + 1; } break; }
        } else {
            memcpy(out + n, g_ps + ms, me - ms);
            n += me - ms;
        }
        if (me == ms) {                                 /* empty match: step over one character */
            if (ms < g_psl) { if (n + 2 > cap) { cap *= 2; out = realloc(out, cap); } out[n++] = g_ps[ms]; }
            pos = ms + 1;
        } else pos = me;
    }
    if (*replaced) {
        if (pos < g_psl) { if (n + (g_psl - pos) + 1 > cap) { cap = n + g_psl; out = realloc(out, cap + 1); } memcpy(out + n, g_ps + pos, g_psl - pos); n += g_psl - pos; }
        buf_set(&g_ps, &g_psl, &g_pscap, out, n);
    }
    free(out);
}

static void flush_appends(char **queue, int *nq) {
    for (int i = 0; i < *nq; i++) {
        if (queue[i][0] == 1) {                         /* marker: r file */
            FILE *f = fopen(queue[i] + 1, "r");
            if (f) { char b[1024]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) fwrite(b, 1, k, g_out); fclose(f); }
        } else fputs(queue[i], g_out);
        free(queue[i]);
    }
    *nq = 0;
}

/* Runs the script over the whole input. Returns when input is exhausted or q/Q fires. */
static void process(void) {
    char *queue[64];
    int nq = 0, quit = 0, replaced_flag = 0;
    while (!quit && read_line()) {
        int pc = 0;
        int append_line = 0;
restart:
        replaced_flag = replaced_flag && append_line;
        append_line = 0;
        while (pc < g_ncmds) {
            Cmd *c = &g_cmds[pc];
            if (c->name == '}' || c->name == ':') { pc++; continue; }
            if (!selected(c)) { pc = c->name == '{' ? c->target + 1 : pc + 1; continue; }
            switch (c->name) {
            case '{': break;
            case 's': { int r; do_subst(c, &r); if (r) { replaced_flag = 1; if (c->s_print) out_ps(g_out); if (c->s_write_fd > 0) { FILE *wf = c->s_write_fd == 1 ? g_out : fdopen(c->s_write_fd, "a"); out_ps(wf); fflush(wf); } } break; }
            case 'y': for (size_t i = 0; i < g_psl; i++) g_ps[i] = (char)c->ytab[(unsigned char)g_ps[i]]; break;
            case 'd': goto next_cycle_nopr;
            case 'D': {
                char *nl = memchr(g_ps, '\n', g_psl);
                if (!nl) goto next_cycle_nopr;
                size_t rest = g_psl - (size_t)(nl - g_ps) - 1;
                memmove(g_ps, nl + 1, rest);
                g_psl = rest;
                g_ps[g_psl] = 0;
                flush_appends(queue, &nq);
                pc = 0;
                append_line = 1;
                goto restart;
            }
            case 'p': out_ps(g_out); break;
            case 'P': { char *nl = memchr(g_ps, '\n', g_psl); fwrite(g_ps, 1, nl ? (size_t)(nl - g_ps) : g_psl, g_out); fputc('\n', g_out); break; }
            case 'n':
                if (is_last()) { if (!g_quiet) out_ps(g_out); flush_appends(queue, &nq); quit = 1; goto done; }
                if (!g_quiet) out_ps(g_out);
                flush_appends(queue, &nq);
                read_line();
                break;
            case 'N':
                if (is_last()) { if (!g_quiet) out_ps(g_out); quit = 1; goto done; }
                buf_append(&g_ps, &g_psl, &g_pscap, "\n", 1);
                { int had = g_in.have_look; (void)had; }
                if (!g_in.have_look) fill_look();
                buf_append(&g_ps, &g_psl, &g_pscap, g_in.look, g_in.looklen);
                g_in.have_look = 0;
                g_in.lineno++;
                break;
            case 'q': if (!g_quiet) out_ps(g_out); flush_appends(queue, &nq); quit = 1; g_rc = c->s_nth; goto done;
            case 'Q': quit = 1; g_rc = c->s_nth; goto done;
            case 'h': buf_set(&g_hs, &g_hsl, &g_hscap, g_ps, g_psl); break;
            case 'H': buf_append(&g_hs, &g_hsl, &g_hscap, "\n", 1); buf_append(&g_hs, &g_hsl, &g_hscap, g_ps, g_psl); break;
            case 'g': buf_set(&g_ps, &g_psl, &g_pscap, g_hs ? g_hs : "", g_hsl); break;
            case 'G': buf_append(&g_ps, &g_psl, &g_pscap, "\n", 1); buf_append(&g_ps, &g_psl, &g_pscap, g_hs ? g_hs : "", g_hsl); break;
            case 'x': { char *t = g_ps; size_t tl = g_psl, tc = g_pscap; g_ps = g_hs ? g_hs : strdup(""); g_psl = g_hsl; g_pscap = g_hs ? g_hscap : 1; g_hs = t; g_hsl = tl; g_hscap = tc; break; }
            case 'z': g_psl = 0; if (g_ps) g_ps[0] = 0; break;
            case 'a': if (nq < 64) queue[nq++] = strdup(c->text); break;
            case 'i': fputs(c->text, g_out); break;
            case 'c':
                if (!c->a2.kind || !c->in_range) fputs(c->text, g_out);
                goto next_cycle_nopr;
            case 'r': if (nq < 64) { char *q = malloc(strlen(c->text) + 2); q[0] = 1; strcpy(q + 1, c->text); queue[nq++] = q; } break;
            case 'w': if (c->s_write_fd > 0) { FILE *wf = c->s_write_fd == 1 ? g_out : fdopen(c->s_write_fd, "a"); out_ps(wf); fflush(wf); } break;
            case '=': fprintf(g_out, "%ld\n", g_in.lineno); break;
            case 'l': {
                int col = 0;
                for (size_t i = 0; i < g_psl; i++) {
                    unsigned char ch = (unsigned char)g_ps[i];
                    char tmp[8];
                    if (ch == '\\') strcpy(tmp, "\\\\");
                    else if (ch == '\n') strcpy(tmp, "\\n");
                    else if (ch == '\t') strcpy(tmp, "\\t");
                    else if (ch < 32 || ch > 126) snprintf(tmp, sizeof tmp, "\\%03o", ch);
                    else { tmp[0] = (char)ch; tmp[1] = 0; }
                    if (col + (int)strlen(tmp) > 69) { fputs("\\\n", g_out); col = 0; }
                    fputs(tmp, g_out);
                    col += (int)strlen(tmp);
                }
                fputs("$\n", g_out);
                break;
            }
            case 'b': pc = c->target; continue;
            case 't': if (replaced_flag) { replaced_flag = 0; pc = c->target; continue; } break;
            case 'T': if (!replaced_flag) { pc = c->target; continue; } replaced_flag = 0; break;
            }
            pc++;
        }
        if (!g_quiet) out_ps(g_out);
        flush_appends(queue, &nq);
        replaced_flag = 0;
        continue;
next_cycle_nopr:
        flush_appends(queue, &nq);
        replaced_flag = 0;
    }
done:
    flush_appends(queue, &nq);
}

static char *g_script;

static void add_script(const char *text) {
    size_t old = g_script ? strlen(g_script) : 0;
    g_script = realloc(g_script, old + strlen(text) + 2);
    if (old) g_script[old++] = '\n';
    strcpy(g_script + old, text);
}

APPLET(sed) {
    int c;
    optind = 0;
    static const struct option lopts[] = {
        { "quiet", 0, NULL, 'n' }, { "silent", 0, NULL, 'n' }, { "expression", 1, NULL, 'e' }, { "file", 1, NULL, 'f' },
        { "in-place", 2, NULL, 'i' }, { "regexp-extended", 0, NULL, 'r' }, { "separate", 0, NULL, 's' }, { 0, 0, 0, 0 },
    };
    while ((c = getopt_long(argc, argv, "ne:f:i::rEsz", lopts, NULL)) != -1) {
        switch (c) {
        case 'n': g_quiet = 1; break;
        case 'e': add_script(optarg); break;
        case 'f': {
            FILE *sf = fopen(optarg, "r");
            if (!sf) { kb_error("couldn't open file %s: %s", optarg, strerror(errno)); return 1; }
            char *text = kb_read_all(fileno(sf), NULL);
            size_t l = strlen(text);
            if (l && text[l - 1] == '\n') text[l - 1] = 0;
            if (!strncmp(text, "#n\n", 3)) g_quiet = 1;
            add_script(text);
            free(text);
            fclose(sf);
            break;
        }
        case 'i': g_inplace = 1; g_separate = 1; g_suffix = optarg; break;
        case 'r': case 'E': g_extended = 1; break;
        case 's': g_separate = 1; break;
        case 'z': break;
        default: return 1;
        }
    }
    if (!g_script) {
        if (optind >= argc) { fprintf(stderr, "Usage: sed [OPTION]... {script-only-if-no-other-script} [input-file]...\n"); return 1; }
        add_script(argv[optind++]);
    }
    compile(g_script);
    static char *dash[] = { "-" };
    g_in.files = optind < argc ? &argv[optind] : dash;
    g_in.nfiles = optind < argc ? argc - optind : 1;
    g_out = stdout;

    if (!g_inplace) {
        process();
        fflush(stdout);
        return g_rc;
    }
    for (int i = 0; i < g_in.nfiles; i++) {                 /* -i: each file is its own stream, rewritten via a temp file */
        const char *path = g_in.files[i];
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s.sedtmp", path);
        g_out = fopen(tmp, "w");
        if (!g_out) { kb_error("couldn't open temporary file %s: %s", tmp, strerror(errno)); return 4; }
        Input one = { 0 };
        char *single[1] = { (char *)path };
        one.files = single;
        one.nfiles = 1;
        g_in = one;
        for (int k = 0; k < g_ncmds; k++) g_cmds[k].in_range = 0;
        process();
        fclose(g_out);
        struct stat st;
        if (stat(path, &st) == 0) chmod(tmp, st.st_mode & 07777);
        if (g_suffix && *g_suffix) { char bak[PATH_MAX]; snprintf(bak, sizeof bak, "%s%s", path, g_suffix); rename(path, bak); }
        rename(tmp, path);
    }
    return g_rc;
}
