/* ex.c: the ":" command line: ranges, substitute, global, files, options. */
#include "vi.h"

char last_ex[512];
char last_sub_pat[256], last_sub_rep[256], last_sub_flags[16];
int in_global;
extern int undo_nest;
static PtBuf ex_out;                                     /* text printed by :p, :reg, ... shown after the command */
static int quit_warned;
static int ex_error;

void pager_show(const char *text);
void undo_mark_saved(void);

/* ------------------------------------------------------------------ small helpers */

static void err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_err = 1;
    ex_error = 1;
    ta_flush();
}

static const char *skipws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static void out_line(const char *s, size_t n) {
    pb_add(&ex_out, s, n);
    pb_addc(&ex_out, '\n');
}

/* ------------------------------------------------------------------ running other programs */

int run_filter(const char *cmd, const char *input, size_t inlen, char **out, size_t *outlen, int *status) {
    char inname[64], outname[64];
    int pid = (int)getpid();
    snprintf(inname, sizeof inname, "/tmp/vi%d.in", pid);
    snprintf(outname, sizeof outname, "/tmp/vi%d.out", pid);
    int infd = open(inname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (infd < 0) return -1;
    size_t off = 0;
    while (off < inlen) {
        ssize_t w = write(infd, input + off, inlen - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    close(infd);
    int ifd = open(inname, O_RDONLY);
    int ofd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ifd < 0 || ofd < 0) { if (ifd >= 0) close(ifd); if (ofd >= 0) close(ofd); unlink(inname); return -1; }
    pid_t child = fork();
    if (child < 0) { close(ifd); close(ofd); unlink(inname); unlink(outname); return -1; }
    if (child == 0) {
        dup2(ifd, 0);
        dup2(ofd, 1);
        dup2(ofd, 2);
        close(ifd);
        close(ofd);
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/gapsh";
        execl("/bin/gapsh", "gapsh", "-c", cmd, (char *)NULL);
        execl(sh, sh, "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(ifd);
    close(ofd);
    int st = 0;
    while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    if (status) *status = st;
    int rfd = open(outname, O_RDONLY);
    PtBuf b = { 0 };
    if (rfd >= 0) {
        char tmp[4096];
        ssize_t r;
        while ((r = read(rfd, tmp, sizeof tmp)) > 0) pb_add(&b, tmp, (size_t)r);
        close(rfd);
    }
    unlink(inname);
    unlink(outname);
    *out = b.p ? b.p : strdup("");
    *outlen = b.len;
    return 0;
}

/* :!cmd: the command runs on the plain terminal, then "Press ENTER" (a ':' starts a command, other keys are given back). */
static void run_interactive(const char *cmd) {
    pt_term_restore(&term);
    pid_t child = fork();
    if (child == 0) {
        execl("/bin/gapsh", "gapsh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st;
    while (child > 0 && waitpid(child, &st, 0) < 0 && errno == EINTR) {}
    const char *m = "\r\nPress ENTER or type command to continue";
    ssize_t w = write(term.out_fd, m, strlen(m));
    (void)w;
    pt_term_enter(&term, 0);
    int k = getkey_plain();
    pt_term_enter(&term, 1);
    force_repaint();
    if (k != K_ENTER && k != ' ' && k != K_ESC && k != 3 && k != K_EOF) { int kk = k | K_NOMAP; ta_push(&kk, 1); }
}

/* ------------------------------------------------------------------ addresses */

static long cur1(void) { return cur.l + 1; }

/* Parses one address at *pp; returns 1 if there was one (value in *out, 1-based, may be 0), -1 on error, 0 for none. */
static int parse_addr(const char **pp, long base, long *out) {
    const char *p = skipws(*pp);
    long a = base;
    int have = 0;
    if (isdigit((unsigned char)*p)) { a = strtol(p, (char **)&p, 10); have = 1; }
    else if (*p == '.') { a = base; p++; have = 1; }
    else if (*p == '$') { a = nl; p++; have = 1; }
    else if (*p == '\'') {
        p++;
        int k = *p++;
        Pos mk;
        mk.l = -1;
        if (k >= 'a' && k <= 'z') mk = marks[k - 'a'];
        else if (k == '<') mk = mark_lt;
        else if (k == '>') mk = mark_gt;
        else if (k == '\'' || k == '`') mk = mark_ctx;
        else if (k == '[') mk = mark_lb;
        else if (k == ']') mk = mark_rb;
        else if (k == '.') mk = mark_dot;
        if (mk.l < 0) { err("E20: Mark not set"); return -1; }
        a = mk.l + 1;
        have = 1;
    } else if (*p == '/' || *p == '?') {
        char delim = *p++;
        char pat[256];
        size_t n = 0;
        while (*p && *p != delim) {
            if (*p == '\\' && p[1]) { if (p[1] != delim && n < sizeof pat - 1) pat[n++] = '\\'; p++; }
            if (n < sizeof pat - 1) pat[n++] = *p;
            p++;
        }
        pat[n] = 0;
        if (*p == delim) p++;
        if (!pat[0]) snprintf(pat, sizeof pat, "%s", last_search);
        else snprintf(last_search, sizeof last_search, "%s", pat);
        if (!pat[0]) { err("E35: No previous regular expression"); return -1; }
        Pos from;
        from.l = base - 1;
        if (from.l < 0) from.l = 0;
        from.c = delim == '/' ? (int)LN(from.l).len : 0;
        if (base == 0 && delim == '/') { from.l = 0; from.c = -1; }
        Pos res;
        int r = search_find(pat, delim == '/' ? 1 : -1, from, &res, o.ws, base == 0 && delim == '/');
        if (r <= 0) { err("E486: Pattern not found: %s", pat); return -1; }
        a = res.l + 1;
        have = 1;
    } else if (*p == '+' || *p == '-') {
        a = base;
    } else { *pp = p; return 0; }
    for (;;) {
        p = skipws(p);
        if (*p == '+' || *p == '-') {
            int sign = *p == '-' ? -1 : 1;
            p++;
            long n = 1;
            if (isdigit((unsigned char)*p)) n = strtol(p, (char **)&p, 10);
            a += sign * n;
            have = 1;
        } else if (isdigit((unsigned char)*p) && have) {
            a += strtol(p, (char **)&p, 10);
        } else break;
    }
    *out = a;
    *pp = p;
    return have;
}

/* ------------------------------------------------------------------ options */

typedef struct { const char *name, *abbr; int *ptr; int is_bool; } OptDef;

static OptDef optdefs[] = {
    { "number", "nu", &o.number, 1 }, { "relativenumber", "rnu", &o.rnu, 1 }, { "list", "list", &o.list, 1 },
    { "ignorecase", "ic", &o.ic, 1 }, { "smartcase", "scs", &o.scs, 1 }, { "wrapscan", "ws", &o.ws, 1 },
    { "hlsearch", "hls", &o.hls, 1 }, { "incsearch", "is", &o.is, 1 }, { "wrap", "wrap", &o.wrap, 1 },
    { "expandtab", "et", &o.et, 1 }, { "autoindent", "ai", &o.ai, 1 }, { "readonly", "ro", &o.ro, 1 },
    { "showmode", "smd", &o.showmode, 1 }, { "ruler", "ru", &o.ruler, 1 }, { "cursorline", "cul", &o.cursorline, 1 },
    { "magic", "magic", &o.magic, 1 }, { "joinspaces", "js", &o.js, 1 }, { "fixendofline", "fixeol", &o.fixeol, 1 },
    { "tabstop", "ts", &o.ts, 0 }, { "shiftwidth", "sw", &o.sw, 0 }, { "softtabstop", "sts", &o.sts, 0 },
    { "textwidth", "tw", &o.tw, 0 },
};

/* Options of Vim that this editor has no use for: they are accepted (so a pasted ":set" line works) and ignored. */
static const char *const ignored_opts[] = {
    "backspace", "bs", "hidden", "hid", "showcmd", "sc", "scrolloff", "so", "smartindent", "si", "cindent", "cin", "timeout", "to",
    "ttimeout", "ttimeoutlen", "ttm", "timeoutlen", "tm", "esckeys", "ek", "compatible", "cp", "encoding", "enc", "fileencoding", "fenc",
    "syntax", "syn", "filetype", "ft", "title", "visualbell", "vb", "errorbells", "eb", "lazyredraw", "lz", "ttyfast", "tf", "history",
    "hi", "undolevels", "ul", "swapfile", "swf", "backup", "bk", "writebackup", "wb", "wildmenu", "wmnu", "laststatus", "ls", "mouse",
    "foldenable", "fen", "nrformats", "nf", "matchpairs", "mps", "whichwrap", "ww", "background", "bg", "cursorcolumn", "cuc",
    "colorcolumn", "cc", "listchars", "lcs", "shell", "sh", "binary", "bin", "modifiable", "ma", "autoread", "ar", "confirm", "cf",
    "startofline", "sol", "wildmode", "wim", "display", "dy", "formatoptions", "fo", "shortmess", "shm", "cpoptions", "cpo", "smarttab", "sta",
    "numberwidth", "nuw", "signcolumn", "scl", "termguicolors", "tgc", "updatetime", "ut", "clipboard", "cb", "spell", "endofline", "eol",
};

static int is_ignored_option(const char *name) {
    for (size_t i = 0; i < sizeof ignored_opts / sizeof *ignored_opts; i++) if (!strcmp(name, ignored_opts[i])) return 1;
    return 0;
}

static void set_option(const char *arg) {
    PtBuf shown = { 0 };
    const char *p = arg;
    while (*p) {
        p = skipws(p);
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') e++;
        char word[64];
        size_t n = (size_t)(e - p) < sizeof word - 1 ? (size_t)(e - p) : sizeof word - 1;
        memcpy(word, p, n);
        word[n] = 0;
        p = e;
        int query = 0, neg = 0, inv = 0, reset = 0;
        char *eq = strpbrk(word, "=:");
        char val[64] = "";
        if (eq) { snprintf(val, sizeof val, "%s", eq + 1); *eq = 0; }
        size_t wl = strlen(word);
        if (wl && word[wl - 1] == '?') { query = 1; word[--wl] = 0; }
        if (wl && word[wl - 1] == '!') { inv = 1; word[--wl] = 0; }
        if (wl && word[wl - 1] == '&') { reset = 1; word[--wl] = 0; }
        char *name = word;
        if (!strncmp(name, "no", 2) && strcmp(name, "number") && strcmp(name, "nu") && !eq) { neg = 1; name += 2; }
        else if (!strncmp(name, "inv", 3)) { inv = 1; name += 3; }
        if (!strcmp(name, "all")) { continue; }
        if (!strcmp(name, "fileformat") || !strcmp(name, "ff")) {
            if (eq && !query) {
                int dos = !strcmp(val, "dos");
                if (!dos && strcmp(val, "unix")) { err("E474: Invalid argument: %s", arg); pb_free(&shown); return; }
                if (dos != ff_dos) { ff_dos = dos; undo_force_modified(); }
            } else pb_addf(&shown, "  fileformat=%s  ", ff_dos ? "dos" : "unix");
            continue;
        }
        OptDef *d = NULL;
        for (size_t i = 0; i < sizeof optdefs / sizeof *optdefs; i++) if (!strcmp(name, optdefs[i].name) || !strcmp(name, optdefs[i].abbr)) d = &optdefs[i];
        if (!d && is_ignored_option(name)) continue;
        if (!d) { err("E518: Unknown option: %s", word); pb_free(&shown); return; }
        if (reset) { continue; }
        if (d->is_bool) {
            if (eq) { err("E474: Invalid argument: %s", arg); pb_free(&shown); return; }
            if (query) pb_addf(&shown, "%s%s  ", *d->ptr ? "  " : "no", d->name);
            else if (inv) *d->ptr = !*d->ptr;
            else *d->ptr = !neg;
            if (d->ptr == &o.ro && !*d->ptr) view_mode = 0;
        } else {
            if (query || !eq) pb_addf(&shown, "  %s=%d  ", d->name, *d->ptr);
            else {
                char *end;
                long v = strtol(val, &end, 10);
                if (*end || (v < 0)) { err("E521: Number required after =: %s", arg); pb_free(&shown); return; }
                if ((d->ptr == &o.ts || d->ptr == &o.sw) && v == 0) { if (d->ptr == &o.sw) v = o.ts; else v = 8; }
                if (d->ptr == &o.ts && v > 64) v = 64;
                *d->ptr = (int)v;
            }
        }
    }
    if (shown.len) set_msg("%s", shown.p);
    pb_free(&shown);
}

/* ------------------------------------------------------------------ substitute */

/* Appends text to out, applying \u \l (the next character) and \U \L (until \E) to it. */
static void add_cased(PtBuf *out, const char *txt, size_t tl, int *oneshot, int *mode_ul) {
    size_t i = 0;
    while (i < tl) {
        uint32_t cp;
        int n = pt_u8_decode((const unsigned char *)txt + i, tl - i, &cp);
        uint32_t m = cp;
        if (*oneshot) { m = *oneshot == 'u' ? cp_upper(cp) : cp_lower(cp); *oneshot = 0; }
        else if (*mode_ul) m = *mode_ul == 'U' ? cp_upper(cp) : cp_lower(cp);
        if (m != cp && !pt_is_rawbyte(cp)) { char u[4]; pb_add(out, u, (size_t)pt_u8_encode(m, u)); }
        else pb_add(out, txt + i, (size_t)n);
        i += (size_t)n;
    }
}

/* Expands the replacement for a match: & \0 the match, \1-\9 groups, \n a NUL, \r a line break, \t a tab, \u \U \l \L \e \E. */
static void expand_rep(PtBuf *out, const char *rep, const char *line, const regmatch_t *m, int ngroups) {
    int oneshot = 0, mode_ul = 0;
    for (const char *r = rep; *r; r++) {
        const char *txt = NULL;
        size_t tl = 0;
        char single[2];
        if (*r == '&') { txt = line + m[0].rm_so; tl = (size_t)(m[0].rm_eo - m[0].rm_so); }
        else if (*r == '\\' && r[1]) {
            r++;
            if (*r >= '0' && *r <= '9') {
                int g = *r - '0';
                if (g <= ngroups && m[g].rm_so >= 0) { txt = line + m[g].rm_so; tl = (size_t)(m[g].rm_eo - m[g].rm_so); }
                else { txt = ""; tl = 0; }
            } else if (*r == 'n') { single[0] = 0; txt = single; tl = 1; }
            else if (*r == 'r') { single[0] = '\n'; txt = single; tl = 1; }
            else if (*r == 't') { single[0] = '\t'; txt = single; tl = 1; }
            else if (*r == 'u') { oneshot = 'u'; continue; }
            else if (*r == 'l') { oneshot = 'l'; continue; }
            else if (*r == 'U') { mode_ul = 'U'; continue; }
            else if (*r == 'L') { mode_ul = 'L'; continue; }
            else if (*r == 'e' || *r == 'E') { mode_ul = 0; continue; }
            else { single[0] = *r; txt = single; tl = 1; }
        } else if (*r == '\r') { single[0] = '\n'; txt = single; tl = 1; }
        else {                                           /* a plain character: all its bytes */
            int n = pt_u8_next(r, strlen(r), 0);
            txt = r;
            tl = (size_t)n;
            r += n - 1;
        }
        add_cased(out, txt, tl, &oneshot, &mode_ul);
    }
}

static int pattern_has_newline(const char *pat) {
    for (const char *p = pat; *p; p++) {
        if (*p == '\\' && p[1]) { if (p[1] == 'n') return 1; p++; }
    }
    return 0;
}

/* :s on lines l1..l2 (0-based). A pattern with \n can match across line breaks: the lines are searched joined, and the lines a
 * match used up are replaced together with the first one (Vim keeps going on the joined text while it is within the range). */
int ex_substitute_range(long l1, long l2, const char *pat, const char *rep, const char *flags) {
    int g = 0, confirm = 0, count_only = 0, icase = -1, noerr = 0;
    for (const char *f = flags; *f; f++) {
        switch (*f) {
        case 'g': g = !g; break;
        case 'c': confirm = 1; break;
        case 'n': count_only = 1; break;
        case 'i': icase = 1; break;
        case 'I': icase = 0; break;
        case 'e': noerr = 1; break;
        default: break;
        }
    }
    int multi = pattern_has_newline(pat), extra = 0;
    if (multi) for (const char *p = pat; *p; p++) { if (*p == '\\' && p[1]) { if (p[1] == 'n') extra++; p++; } }
    regex_t re;
    re_multiline = multi;
    int rc = re_compile(&re, pat, icase);
    re_multiline = 0;
    if (rc != 0) { err("E486: Invalid pattern: %s", pat); return 0; }
    int ng = (int)re.re_nsub;
    if (ng > 9) ng = 9;
    long subs = 0, lines_changed = 0, last_changed = -1;
    int quit_all = 0, do_all = 0;
    long end = l2;
    for (long l = l1; l <= end && l < nl && !quit_all;) {
        if (intr_check()) { err("Interrupted"); break; }
        long cl = l;                                     /* the line being scanned (a match may have used up the ones before it) */
        size_t cpos = 0, ccopied = 0;                    /* scan position and first byte not yet copied, in line cl */
        PtBuf out = { 0 };
        int changed = 0;
        for (;;) {
            PtBuf subj = { 0 };                          /* line cl, and for \n patterns the lines after it */
            size_t len0 = LN(cl).len;
            pb_add(&subj, ls(cl), len0);
            long hi = cl;
            for (int e = 0; e < extra && hi + 1 < nl; e++) { hi++; pb_addc(&subj, '\n'); pb_add(&subj, ls(hi), LN(hi).len); }
            if (multi && hi == nl - 1) pb_addc(&subj, '\n');       /* the line break after the last line can be matched too */
            const char *s = subj.p ? subj.p : "";
            regmatch_t m[10];
            if (cpos > len0 || re_exec_at(&re, s, subj.len, cpos, m, 10) != 0) { pb_free(&subj); break; }
            size_t so = (size_t)m[0].rm_so, eo = (size_t)m[0].rm_eo;
            if (so > len0) { pb_free(&subj); break; }    /* the match starts on a later line: that line's turn */
            int doit = 1;
            if (confirm && !do_all && !count_only) {
                Pos save = cur;
                cur.l = cl; cur.c = (int)so;
                char q[300];
                snprintf(q, sizeof q, "replace with %s (y/n/a/q/l/^E/^Y)?", rep);
                set_msg("%s", q);
                redraw();
                int k = getkey();
                cur = save;
                msg[0] = 0;
                if (k == 'y') doit = 1;
                else if (k == 'n') doit = 0;
                else if (k == 'a') do_all = 1;
                else if (k == 'l') quit_all = 1;
                else { quit_all = 1; doit = 0; }
            }
            long consumed = 0;                           /* line breaks inside the match */
            for (size_t i = so; i < eo; i++) if (s[i] == '\n') consumed++;
            if (doit) {
                pb_add(&out, s + ccopied, so - ccopied);
                if (!count_only) expand_rep(&out, rep, s, m, ng);
                changed++;
                subs++;
                if (consumed) {
                    size_t line_start = 0;               /* offset in subj of the line the match ends in */
                    long k = 0;
                    for (size_t i = 0; i < eo && k < consumed; i++) if (s[i] == '\n') { k++; line_start = i + 1; }
                    int past_end = cl + consumed > nl - 1;              /* the match took the line break after the last line */
                    if (past_end) { cl = nl - 1; ccopied = cpos = LN(cl).len + 1; }
                    else { cl += consumed; ccopied = cpos = eo - line_start; }
                    pb_free(&subj);
                    if (quit_all || past_end) break;
                    if (cl <= end || g) continue;        /* the joined line is looked at again */
                    break;
                }
                ccopied = eo;
            }
            if (quit_all && !doit) { pb_free(&subj); break; }
            if (eo == so) {                              /* an empty match: step over one character */
                if (so >= len0) { pb_free(&subj); break; }
                size_t nx = (size_t)next_c(cl, (int)so);
                if (doit) { pb_add(&out, s + so, nx - so); ccopied = nx; }
                cpos = nx;
            } else cpos = eo;
            pb_free(&subj);
            if (!g || quit_all) break;
        }
        if (changed && !count_only) {
            if (LN(cl).len > ccopied) pb_add(&out, ls(cl) + ccopied, LN(cl).len - ccopied);
            /* the replacement may contain line breaks (from \r): the lines l..cl become as many lines as there are pieces */
            size_t olen = out.len, p = 0;
            long at = l, old_count = cl - l + 1, pieces = 0;
            while (p <= olen) {
                const char *e = olen - p ? memchr(out.p + p, '\n', olen - p) : NULL;
                size_t n = e ? (size_t)(e - (out.p + p)) : olen - p;
                const char *piece = out.p ? out.p + p : "";
                if (pieces == 0) ln_set(at, piece, n);
                else ln_insert(at, piece, n);
                at++;
                pieces++;
                if (!e) break;
                p += n + 1;
            }
            if (old_count > 1) ln_delete(at, old_count - 1);
            end += pieces - old_count;
            lines_changed++;
            last_changed = l + pieces - 1;
            l += pieces;
        } else {
            if (changed) { lines_changed++; last_changed = l; }
            l = (count_only ? cl : l) + 1;
        }
        pb_free(&out);
    }
    regfree(&re);
    if (!subs) {
        if (!noerr && !in_global) err("E486: Pattern not found: %s", pat);
        return 0;
    }
    if (last_changed >= 0 && !count_only) { cur.l = imin(last_changed, nl - 1); cur.c = first_nonblank(cur.l); }
    if (count_only) set_msg("%ld match%s on %ld line%s", subs, subs == 1 ? "" : "es", lines_changed, lines_changed == 1 ? "" : "s");
    else if (subs > 2) set_msg("%ld substitution%s on %ld line%s", subs, subs == 1 ? "" : "s", lines_changed, lines_changed == 1 ? "" : "s");
    return 1;
}

/* :s/pat/rep/flags [count] and :&, :&&, :~ ; p points after the command name. */
static int cmd_substitute(long l1, long l2, const char *p, int kind) {
    char pat[256] = "", rep[256] = "", flags[16] = "";
    p = skipws(p);
    int have_pat = 0;
    if (kind == 's' && *p && !isalnum((unsigned char)*p) && *p != ' ' && *p != '&' && *p != '"' && *p != '|') {
        char d = *p++;
        size_t n = 0;
        while (*p && *p != d) {
            if (*p == '\\' && p[1]) { if (p[1] == d && d != '/' ) { } else if (n < sizeof pat - 1) pat[n++] = *p; p++; }
            if (n < sizeof pat - 1) pat[n++] = *p;
            p++;
        }
        pat[n] = 0;
        have_pat = 1;
        if (*p == d) {
            p++;
            n = 0;
            while (*p && *p != d) {
                if (*p == '\\' && p[1]) { if (p[1] != d && n < sizeof rep - 1) rep[n++] = *p; p++; }
                if (n < sizeof rep - 1) rep[n++] = *p;
                p++;
            }
            rep[n] = 0;
            if (*p == d) p++;
        }
        if (!pat[0]) snprintf(pat, sizeof pat, "%s", last_search);
        if (!strcmp(rep, "~") && 0) { }
    } else {
        /* :s, :& and :~ repeat the last substitute */
        if (!last_sub_pat[0]) { err("E35: No previous regular expression"); return 0; }
        snprintf(pat, sizeof pat, "%s", kind == '~' ? last_search : last_sub_pat);
        snprintf(rep, sizeof rep, "%s", last_sub_rep);
        if (*p == '&') { snprintf(flags, sizeof flags, "%s", last_sub_flags); p++; }
    }
    size_t fn = strlen(flags);
    while (*p && *p != ' ' && !isdigit((unsigned char)*p) && fn < sizeof flags - 1) {
        if (*p == '&') { snprintf(flags, sizeof flags, "%s", last_sub_flags); fn = strlen(flags); p++; continue; }
        if (!strchr("gcniIepr#l", *p)) break;
        flags[fn++] = *p++;
    }
    flags[fn] = 0;
    p = skipws(p);
    if (isdigit((unsigned char)*p)) {                    /* a count: that many lines starting at the last line of the range */
        long c = strtol(p, NULL, 10);
        l1 = l2;
        l2 = imin(nl, l1 + c - 1);
    }
    if (have_pat && strchr(rep, '~')) {                  /* an unescaped ~ stands for the previous replacement string */
        char out[sizeof rep];
        size_t k = 0;
        for (const char *r = rep; *r && k < sizeof out - 1; r++) {
            if (*r == '\\' && r[1]) { out[k++] = *r++; if (k < sizeof out - 1) out[k++] = *r; continue; }
            if (*r == '~') { for (const char *t = last_sub_rep; *t && k < sizeof out - 1; t++) out[k++] = *t; continue; }
            out[k++] = *r;
        }
        out[k] = 0;
        snprintf(rep, sizeof rep, "%s", out);
    }
    snprintf(last_sub_pat, sizeof last_sub_pat, "%s", pat);
    snprintf(last_search, sizeof last_search, "%s", pat);
    snprintf(last_sub_rep, sizeof last_sub_rep, "%s", rep);
    snprintf(last_sub_flags, sizeof last_sub_flags, "%s", flags);
    return ex_substitute_range(l1 - 1, l2 - 1, pat, rep, flags);
}

/* ------------------------------------------------------------------ files */

static int do_write(long l1, long l2, const char *arg, int bang, int whole, int have_range, int saveas) {
    arg = skipws(arg);
    if (whole) { l1 = 1; l2 = nl; }
    int append = 0;
    if (arg[0] == '>' && arg[1] == '>') { append = 1; arg = skipws(arg + 2); }
    if (*arg == '!') {                                   /* :w !cmd */
        size_t n;
        Range r = { { l1 - 1, 0 }, { l2 - 1, 0 }, 'l', 0, 0, 0 };
        char *t = range_text(&r, &n);
        char *outp;
        size_t outl;
        run_filter(arg + 1, t, n, &outp, &outl, NULL);
        free(t);
        if (outl) pb_add(&ex_out, outp, outl);
        free(outp);
        return 1;
    }
    char name[MAXPATH];
    if (*arg) snprintf(name, sizeof name, "%s", arg);
    else if (fname[0]) snprintf(name, sizeof name, "%s", fname);
    else { err("E32: No file name"); return 0; }
    int own = !strcmp(name, fname);
    if (own && (o.ro || view_mode) && !bang) { err("E45: 'readonly' option is set (add ! to override)"); return 0; }
    if (!own && !append && !bang) {
        struct stat st;
        if (stat(name, &st) == 0) { err("E13: File exists (add ! to override)"); return 0; }
    }
    if (own && !whole && !bang) { err("E140: Use ! to write partial buffer"); return 0; }
    long nb;
    if (buf_write(name, l1 - 1, l2 - 1, append, &nb) != 0) {
        err("E212: Can't open file for writing");
        return 0;
    }
    int renamed = saveas || (!fname[0] && !have_range);
    if (renamed) snprintf(fname, sizeof fname, "%s", name);
    if ((own || renamed) && whole) undo_mark_saved();
    set_msg("\"%s\" %ldL, %ldB %s", name, l2 - l1 + 1, nb, append ? "appended" : "written");
    return 1;
}

int ex_edit(const char *path, int force) {
    if (modified && !force && (strcmp(path, fname) || path[0] == 0)) { err("E37: No write since last change (add ! to override)"); return 0; }
    int nlines;
    long nbytes;
    char name[MAXPATH];
    snprintf(name, sizeof name, "%s", path[0] ? path : fname);
    if (!name[0]) { err("E32: No file name"); return 0; }
    int rc = buf_load(name, &nlines, &nbytes);
    snprintf(fname, sizeof fname, "%s", name);
    cur.l = 0; cur.c = 0;
    topline = 0;
    if (rc == 1) set_msg("\"%s\" [New]", name);
    else if (rc < 0) { err("\"%s\" %s", name, strerror(errno)); }
    else set_msg("\"%s\"%s %dL, %ldB", name, noeol ? " [noeol]" : "", nlines, nbytes);
    file_ro = access(name, W_OK) != 0 && rc == 0;
    if (file_ro) { o.ro = 1; }
    else if (!view_mode) o.ro = 0;
    cur.c = 0;
    return 1;
}

static void cmd_read(long after, const char *arg, int bang) {
    arg = skipws(arg);
    char *data;
    size_t n;
    if (bang || *arg == '!') {
        if (*arg == '!') arg++;
        int st;
        if (run_filter(arg, "", 0, &data, &n, &st) != 0) { err("E485: Can't read command output"); return; }
    } else {
        const char *name = *arg ? arg : fname;
        int fd = open(name, O_RDONLY);
        if (fd < 0) { err("E484: Can't open file %s", name); return; }
        PtBuf b = { 0 };
        char tmp[4096];
        ssize_t r;
        while ((r = read(fd, tmp, sizeof tmp)) > 0) pb_add(&b, tmp, (size_t)r);
        close(fd);
        data = b.p ? b.p : strdup("");
        n = b.len;
    }
    long at = after;                                     /* insert after line `after` (1-based; 0 = before the first) */
    long count = 0;
    size_t p = 0;
    while (p < n) {
        char *e = memchr(data + p, '\n', n - p);
        size_t len = e ? (size_t)(e - (data + p)) : n - p;
        ln_insert(at++, data + p, len);
        count++;
        p += len + 1;
    }
    free(data);
    if (count) { cur.l = after; cur.c = first_nonblank(cur.l); }
    if (count > 2) set_msg("%ld more lines", count);
}

int ex_write_all(int force) { (void)force; return 1; }

int ex_quit(int force) {
    if (modified && !force) { err("E37: No write since last change (add ! to override)"); return 0; }
    if (!force && argidx + 1 < nargfiles && !quit_warned) {
        err("E173: %d more file%s to edit", nargfiles - argidx - 1, nargfiles - argidx - 1 == 1 ? "" : "s");
        quit_warned = 1;
        return 0;
    }
    quit_flag = 1;
    return 1;
}

static int goto_arg(int idx, int force) {
    if (idx < 0 || idx >= nargfiles) { err("E163: There is only one file to edit"); return 0; }
    if (modified && !force) { err("E37: No write since last change (add ! to override)"); return 0; }
    argidx = idx;
    quit_warned = 0;
    return ex_edit(argfiles[idx], 1);
}

/* ------------------------------------------------------------------ line commands */

static void put_lines_text(long after, const char *t, size_t n) {
    long at = after;
    size_t p = 0;
    while (p < n) {
        const char *e = memchr(t + p, '\n', n - p);
        size_t len = e ? (size_t)(e - (t + p)) : n - p;
        ln_insert(at++, t + p, len);
        p += len + 1;
    }
}

static int addr_ok(long a, int allow0) {
    if (a < (allow0 ? 0 : 1) || a > nl) { err("E16: Invalid range"); return 0; }
    return 1;
}

typedef struct { char *s; size_t len; double key; long idx; } SortLine;
static int sort_numeric, sort_rev, sort_icase;

static int sort_cmp0(const SortLine *a, const SortLine *b) {
    int c;
    if (sort_numeric) c = a->key < b->key ? -1 : a->key > b->key ? 1 : 0;
    else {
        size_t n = a->len < b->len ? a->len : b->len;
        c = sort_icase ? strncasecmp(a->s, b->s, n) : memcmp(a->s, b->s, n);
        if (!c) c = a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
    }
    return c;
}

static int sort_cmp(const void *pa, const void *pb) {
    const SortLine *a = pa, *b = pb;
    if (a->idx == b->idx) return 0;
    int c = sort_cmp0(a, b);
    if (sort_rev) c = -c;
    if (!c) c = a->idx < b->idx ? -1 : 1;                /* equal lines keep their order */
    return c;
}

/* :sort [n x f] [r] [u] [i] and :sort! */
static void cmd_sort(long l1, long l2, const char *arg, int bang) {
    int uniq = 0, kind = 0;
    sort_numeric = 0;
    sort_rev = bang;
    sort_icase = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == 'n' || *p == 'x' || *p == 'f') { kind = *p; sort_numeric = 1; }
        else if (*p == 'r') sort_rev = 1;
        else if (*p == 'u') uniq = 1;
        else if (*p == 'i') sort_icase = 1;
    }
    long n = l2 - l1 + 1;
    if (n < 2) return;
    SortLine *a = malloc((size_t)n * sizeof *a);
    if (!a) pt_oom();
    for (long i = 0; i < n; i++) {
        a[i].len = LN(l1 - 1 + i).len;
        a[i].s = malloc(a[i].len + 1);
        if (!a[i].s) pt_oom();
        memcpy(a[i].s, ls(l1 - 1 + i), a[i].len);
        a[i].s[a[i].len] = 0;
        a[i].key = 0;
        a[i].idx = i;
        if (sort_numeric) {
            const char *q = a[i].s;
            a[i].key = -1e300;                           /* lines without a number come first */
            if (kind == 'x') {
                while (*q && !isxdigit((unsigned char)*q)) q++;
                if (*q) { int neg = q > a[i].s && q[-1] == '-'; if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2; a[i].key = (double)strtoul(q, NULL, 16) * (neg ? -1 : 1); }
            } else {
                while (*q && !isdigit((unsigned char)*q) && !(*q == '-' && isdigit((unsigned char)q[1])) && !(kind == 'f' && (*q == '.' || *q == '-' || *q == '+') && isdigit((unsigned char)q[1]))) q++;
                if (*q) a[i].key = kind == 'f' ? strtod(q, NULL) : (double)strtol(q, NULL, 10);
            }
        }
    }
    qsort(a, (size_t)n, sizeof *a, sort_cmp);
    long w = 0;
    for (long i = 0; i < n; i++) {
        if (uniq && w > 0 && sort_cmp0(&a[i], &a[w - 1]) == 0) { free(a[i].s); continue; }
        a[w++] = a[i];
    }
    for (long i = 0; i < w; i++) { ln_set(l1 - 1 + i, a[i].s, a[i].len); free(a[i].s); }
    if (w < n) ln_delete(l1 - 1 + w, n - w);
    free(a);
    cur.l = l1 - 1;
    cur.c = 0;
}

/* :global. A matching line gets a number (Line.gid); a line that leaves the buffer is noted in global_dead[], so the lines still
 * to be done are found again wherever the commands moved them. */
static unsigned char *global_dead;
static unsigned global_n;

static void global_hook(const Line *l) {
    if (l->gid && l->gid <= global_n) global_dead[l->gid - 1] = 1;
}

static long find_gid(unsigned id, long hint) {
    if (hint < 0) hint = 0;
    if (hint > nl) hint = nl;
    for (long i = hint; i < nl; i++) if (LN(i).gid == id) return i;
    for (long i = 0; i < hint; i++) if (LN(i).gid == id) return i;
    return -1;
}

static void cmd_global(long l1, long l2, const char *p, int invert) {
    p = skipws(p);
    if (!*p) { err("E35: No previous regular expression"); return; }
    char d = *p++;
    char pat[256];
    size_t n = 0;
    while (*p && *p != d) {
        if (*p == '\\' && p[1]) { if (p[1] != d && n < sizeof pat - 1) pat[n++] = '\\'; p++; }
        if (n < sizeof pat - 1) pat[n++] = *p;
        p++;
    }
    pat[n] = 0;
    if (*p == d) p++;
    const char *cmd = *p ? p : "p";
    if (!pat[0]) snprintf(pat, sizeof pat, "%s", last_search);
    else snprintf(last_search, sizeof last_search, "%s", pat);
    regex_t re;
    if (re_compile(&re, pat, -1) != 0) { err("E486: Invalid pattern: %s", pat); return; }
    for (long i = 0; i < nl; i++) LN(i).gid = 0;
    unsigned id = 0;
    for (long i = l1 - 1; i <= l2 - 1 && i < nl; i++) {
        int hit = re_exec_at(&re, ls(i), LN(i).len, 0, NULL, 0) == 0;
        if (hit != invert) LN(i).gid = ++id;
    }
    regfree(&re);
    if (!id) { err(invert ? "E538: Pattern found in every line: %s" : "E486: Pattern not found: %s", pat); return; }
    unsigned char *saved_dead = global_dead;
    unsigned saved_n = global_n;
    void (*saved_hook)(const Line *) = line_deleted_hook;
    global_dead = calloc(id, 1);
    if (!global_dead) pt_oom();
    global_n = id;
    line_deleted_hook = global_hook;
    in_global++;
    undo_nest++;
    long hint = 0;
    char *cmdcopy = strdup(cmd);
    for (unsigned k = 1; k <= id && !quit_flag; k++) {
        if (intr_check()) { err("Interrupted"); break; }
        if (global_dead[k - 1]) continue;
        long i = find_gid(k, hint);
        if (i < 0) continue;
        LN(i).gid = 0;
        cur.l = i;
        cur.c = 0;
        ex_execute(cmdcopy);
        if (ex_error) break;
        hint = imin(i, nl > 0 ? nl - 1 : 0);
    }
    free(cmdcopy);
    undo_nest--;
    in_global--;
    line_deleted_hook = saved_hook;
    free(global_dead);
    global_dead = saved_dead;
    global_n = saved_n;
    for (long i = 0; i < nl; i++) LN(i).gid = 0;
    if (cur.l >= nl) cur.l = nl - 1;
}

static const char *help_text =
    "vi: a vi/vim-lite editor.  Keys: i a o O insert, Esc normal, x dd D cc dw d$ delete, y yank, p P put, u undo, Ctrl-R redo,\n"
    "  /pat ?pat n N search, . repeat, q{r} @{r} macros, v V Ctrl-V visual, :w :q :wq :x :q! ZZ ZQ, :s/a/b/g, :g/pat/cmd,\n"
    "  :set {opt}, :e file, :r file, :! cmd, :help.  Text objects: iw aw ip i( i\" it.  See vi(1) in the manual (man vi).\n";

/* ------------------------------------------------------------------ the dispatcher */

typedef struct { const char *name; int minlen; } CmdDef;
static const CmdDef cmds[] = {
    { "substitute", 1 }, { "set", 2 }, { "setlocal", 6 }, { "write", 1 }, { "wq", 2 }, { "wall", 2 }, { "wqall", 3 }, { "xall", 2 }, { "quit", 1 }, { "qall", 2 },
    { "quitall", 5 }, { "xit", 1 }, { "exit", 3 }, { "delete", 1 }, { "yank", 1 }, { "put", 2 }, { "copy", 2 }, { "t", 1 }, { "move", 1 },
    { "join", 1 }, { "normal", 4 }, { "global", 1 }, { "vglobal", 1 }, { "read", 1 }, { "edit", 1 }, { "file", 1 }, { "mark", 2 }, { "k", 1 },
    { "print", 1 }, { "number", 2 }, { "undo", 1 }, { "redo", 3 }, { "registers", 3 }, { "display", 2 }, { "marks", 5 }, { "nohlsearch", 3 },
    { "sort", 3 }, { "help", 1 }, { "map", 3 }, { "nmap", 2 }, { "noremap", 2 }, { "nnoremap", 2 }, { "unmap", 3 }, { "nunmap", 3 }, { "imap", 2 }, { "inoremap", 3 }, { "iunmap", 3 },
    { "abbreviate", 2 }, { "iabbrev", 2 }, { "unabbreviate", 3 }, { "iunabbrev", 4 }, { "abclear", 3 }, { "iabclear", 5 }, { "next", 1 }, { "previous", 4 }, { "Next", 1 }, { "args", 2 }, { "cquit", 2 }, { "version", 2 },
    { "jumps", 2 }, { "list", 1 }, { "retab", 3 }, { "ascii", 2 }, { "wnext", 2 }, { "rewind", 3 }, { "first", 3 }, { "last", 2 }, { "syntax", 2 }, { "filetype", 5 },
    { "left", 2 }, { "right", 2 }, { "center", 2 }, { "update", 2 }, { "stop", 2 }, { "suspend", 3 }, { "redraw", 4 }, { "echo", 2 }, { "let", 3 }, { "startinsert", 4 },
    { "stopinsert", 5 }, { "saveas", 3 }, { "changes", 7 }, { "undolist", 5 }, { "sview", 2 }, { "view", 3 }, { "visual", 2 }, { "vi", 2 }, { "vim", 3 }, { "append", 1 },
};

static int match_cmd(const char *word, size_t wl, const char **full) {
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) {
        size_t fl = strlen(cmds[i].name);
        if (wl >= (size_t)cmds[i].minlen && wl <= fl && !strncmp(cmds[i].name, word, wl)) { *full = cmds[i].name; return 1; }
    }
    return 0;
}

static int ex_one_main(const char *line, char **scratch);
static const char *ex_rest;                              /* the commands after a "|" that ended the one just run */

static int ex_one(const char *line) {
    char *scratch = NULL;
    int r = ex_one_main(line, &scratch);
    free(scratch);
    return r;
}

/* True if "|" ends the argument of this command. :global and :normal take the rest of the line, and so do the commands that
 * hand it to the shell. */
static int cmd_takes_bar(const char *cname, const char *arg) {
    if (!strcmp(cname, "global") || !strcmp(cname, "vglobal") || !strcmp(cname, "normal") || !strcmp(cname, "!")) return 0;
    if (!strcmp(cname, "read") || !strcmp(cname, "write")) { const char *a = skipws(arg); if (*a == '!') return 0; }
    return 1;
}

/* The "|" that ends the argument (not one that is escaped with a backslash), or NULL. For :s the pattern and the replacement
 * come first: a "|" in them is part of the text. */
static const char *find_bar(const char *cname, const char *arg) {
    const char *p = arg;
    if (!strcmp(cname, "substitute")) {
        const char *q = skipws(p);
        if (*q && !isalnum((unsigned char)*q) && *q != ' ' && *q != '"' && *q != '|' && *q != '&') {
            char d = *q++;
            for (int part = 0; part < 2; part++) {
                while (*q && *q != d) { if (*q == '\\' && q[1]) q++; q++; }
                if (*q == d) q++;
            }
            p = q;
        }
    }
    for (; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '|') return p;
    }
    return NULL;
}

int ex_execute(const char *line) {
    const char *outer_rest = ex_rest;                    /* this may run inside a command that has more commands after a "|" */
    ex_error = 0;
    pb_clear(&ex_out);
    if (!in_global) msg[0] = 0;
    int ok = 1;
    for (const char *p = line; p; p = ex_rest) {
        ex_rest = NULL;
        ok = ex_one(p);
        if (!ok || ex_error) break;
    }
    ex_rest = outer_rest;
    if (ex_out.len && !in_global) {
        size_t lines = 0;
        for (size_t i = 0; i < ex_out.len; i++) if (ex_out.p[i] == '\n') lines++;
        if (lines <= 1) {
            if (ex_out.len && ex_out.p[ex_out.len - 1] == '\n') ex_out.len--;
            ex_out.p[ex_out.len] = 0;
            set_msg("%s", ex_out.p);
        } else pager_show(ex_out.p);
        pb_clear(&ex_out);
    }
    return ok && !ex_error;
}

static int ex_one_main(const char *line, char **scratch) {
    const char *p = line;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (!*p) return 1;
    if (*p == '"') return 1;
    long l1 = cur1(), l2 = cur1();
    int naddr = 0;
    /* ---- range */
    if (*p == '%') { l1 = 1; l2 = nl; naddr = 2; p++; }
    else if (*p == '*') { l1 = mark_lt.l + 1; l2 = mark_gt.l + 1; naddr = 2; p++; }
    else {
        long base = cur1();
        for (;;) {
            long a;
            int r = parse_addr(&p, base, &a);
            if (r < 0) return 0;
            if (r == 0) a = base;
            int had = r > 0;
            p = skipws(p);
            if (*p == ',' || *p == ';') {
                if (!had) a = base;
                l1 = a;
                naddr = naddr ? naddr : 1;
                if (*p == ';') base = a;
                p++;
                p = skipws(p);
                long b;
                int r2 = parse_addr(&p, base, &b);
                if (r2 < 0) return 0;
                l2 = r2 > 0 ? b : cur1();
                naddr = 2;
                if (*p == ',' || *p == ';') { l1 = l2; continue; }
                break;
            }
            if (had) { l1 = l2 = a; naddr = naddr > 1 ? naddr : 1; }
            break;
        }
    }
    if (naddr >= 2 && l1 > l2) { err("E493: Backwards range given"); return 0; }
    p = skipws(p);
    while (*p == ':') p++;
    if (!*p || *p == '|') {
        if (naddr) {                                     /* just a line number: go there */
            if (l2 < 1) l2 = 1;
            if (l2 > nl) { if (naddr && l2 > nl) { err("E16: Invalid range"); return 0; } }
            push_jump();
            cur.l = l2 - 1;
            cur.c = first_nonblank(cur.l);
        }
        if (*p == '|') ex_rest = p + 1;
        return 1;
    }
    /* ---- command name */
    char name[24];
    size_t nlen = 0;
    const char *full = NULL;
    if (*p == '!' ) { name[0] = '!'; nlen = 1; p++; }
    else if (strchr("&<>=#~@*", *p)) { name[0] = *p; nlen = 1; p++; }
    else {
        const char *q = p;
        while (isalpha((unsigned char)*q) && nlen < sizeof name - 1) name[nlen++] = *q++;
        /* "s" followed by letters such as :sno is not supported; :s#..# and :s/../ end the run at the delimiter */
        if (nlen > 1 && name[0] == 'k' && !match_cmd(name, nlen, &full)) { nlen = 1; q = p + 1; }
        if (nlen > 1 && !match_cmd(name, nlen, &full)) {
            /* things like :dl, :s with flags, :ya x ... try a shorter command followed by an argument */
            if (name[0] == 's' && !isalpha((unsigned char)p[1])) { nlen = 1; q = p + 1; }
            else if ((name[0] == 'd' || name[0] == 'y') && !match_cmd(name, nlen, &full)) { nlen = 1; q = p + 1; }
            else if (name[0] == 'g' && 0) { }
        }
        if (!nlen) { err("E492: Not an editor command: %s", line); return 0; }
        p = q;
    }
    name[nlen] = 0;
    int bang = 0;
    if (*p == '!' && name[0] != '!' ) {
        if (!((name[0] == 'r') && nlen == 1) ) { bang = 1; p++; }
        else bang = 1;
    }
    const char *cname;
    if (nlen == 1 && strchr("!&<>=#~@*", name[0])) cname = name;
    else if (match_cmd(name, nlen, &full)) cname = full;
    else { err("E492: Not an editor command: %s", name); return 0; }
    const char *arg = p;                                  /* includes leading spaces */
    if (cmd_takes_bar(cname, arg)) {
        const char *bar = find_bar(cname, arg);
        if (bar) {                                        /* "\|" in the argument is a plain "|" */
            *scratch = malloc((size_t)(bar - arg) + 1);
            if (!*scratch) pt_oom();
            size_t k = 0;
            for (const char *q = arg; q < bar; q++) {
                if (*q == '\\' && q + 1 < bar && q[1] == '|') continue;
                (*scratch)[k++] = *q;
            }
            (*scratch)[k] = 0;
            arg = *scratch;
            ex_rest = bar + 1;
        }
    }

    #define IS(s) (!strcmp(cname, s))
    /* defaults for ranges */
    if (naddr == 0) { l1 = l2 = cur1(); }
    int whole_default = IS("global") || IS("vglobal") || IS("write") || IS("sort") || IS("wq") || IS("xit") || IS("exit");
    if (naddr == 0 && IS("global")) { l1 = 1; l2 = nl; }
    if (naddr == 0 && IS("vglobal")) { l1 = 1; l2 = nl; }
    if (naddr == 0 && IS("sort")) { l1 = 1; l2 = nl; }
    (void)whole_default;
    int allow0 = IS("read") || IS("put") || IS("copy") || IS("t") || IS("move") || IS("normal") || IS("mark") || IS("k");
    if (naddr && !IS("!") ) {
        if (!addr_ok(l1, allow0 && naddr < 2) || !addr_ok(l2, allow0 && naddr < 2)) return 0;
    }
    if (nl < 1) return 0;
    if (l1 > nl) l1 = nl;
    if (l2 > nl) l2 = nl;

    if (!IS("normal") && !IS("global") && !IS("vglobal") && !in_global && !undo_nest) undo_begin();

    if (IS("write") || IS("update") || IS("saveas")) {
        if (IS("update") && !modified) return 1;
        return do_write(l1, l2, arg, bang, naddr == 0, naddr != 0, IS("saveas"));
    }
    if (IS("wq") || IS("xit") || IS("exit")) {
        if ((IS("wq") || modified) ) {
            if (!do_write(1, nl, arg, bang, 1, 0, 0)) return 0;
        }
        quit_flag = 1;
        return 1;
    }
    if (IS("wall") || IS("update")) return do_write(1, nl, "", bang, 1, 0, 0);
    if (IS("wqall") || IS("xall")) { if (modified && !do_write(1, nl, "", bang, 1, 0, 0)) return 0; quit_flag = 1; return 1; }
    if (IS("quit") || IS("qall") || IS("quitall")) {
        if (IS("qall") || IS("quitall")) { if (modified && !bang) { err("E37: No write since last change (add ! to override)"); return 0; } quit_flag = 1; return 1; }
        return ex_quit(bang);
    }
    if (IS("cquit")) { quit_flag = 1; exit_status = 1; return 1; }
    if (IS("edit") || IS("view") || IS("visual") || IS("vi") || IS("vim") || IS("sview")) {
        const char *a = skipws(arg);
        if (!*a && bang) { if (!fname[0]) { err("E32: No file name"); return 0; } return ex_edit(fname, 1); }
        if (!*a) { if (modified && !bang) { err("E37: No write since last change (add ! to override)"); return 0; } if (!fname[0]) { err("E32: No file name"); return 0; } return ex_edit(fname, 1); }
        return ex_edit(a, bang);
    }
    if (IS("read")) {
        cmd_read(naddr ? l2 : cur1(), arg, bang);
        return !ex_error;
    }
    if (IS("file")) {
        const char *a = skipws(arg);
        if (*a) snprintf(fname, sizeof fname, "%s", a);
        set_msg("\"%s\"%s %ld line%s --%ld%%--", fname[0] ? fname : "[No Name]", modified ? " [Modified]" : "", nl, nl == 1 ? "" : "s", cur1() * 100 / nl);
        return 1;
    }
    if (IS("next")) { const char *a = skipws(arg); (void)a; return goto_arg(argidx + 1, bang); }
    if (IS("previous") || IS("Next")) return goto_arg(argidx - 1, bang);
    if (IS("rewind") || IS("first")) return goto_arg(0, bang);
    if (IS("last")) return goto_arg(nargfiles - 1, bang);
    if (IS("args")) {
        PtBuf b = { 0 };
        for (int i = 0; i < nargfiles; i++) pb_addf(&b, i == argidx ? "[%s] " : "%s ", argfiles[i]);
        set_msg("%s", b.p ? b.p : "");
        pb_free(&b);
        return 1;
    }
    if (IS("delete") || IS("yank")) {
        const char *a = skipws(arg);
        int reg = 0;
        if (*a && !isdigit((unsigned char)*a)) { reg = *a++; a = skipws(a); }
        if (isdigit((unsigned char)*a)) { long c = strtol(a, NULL, 10); l1 = l2; l2 = imin(nl, l1 + c - 1); }
        Range r = { { l1 - 1, 0 }, { l2 - 1, 0 }, 'l', 0, 0, 0 };
        Pos sv = cur;
        if (IS("delete")) { do_op_range('d', &r, reg, 1); }
        else { size_t n; char *t = range_text(&r, &n); reg_set(reg, t, n, 'l', 0); free(t); cur = sv; }
        return 1;
    }
    if (IS("put")) {
        const char *a = skipws(arg);
        int reg = *a ? *a : 0;
        Reg *rg = reg_get(reg);
        if (!rg) { err("E353: Nothing in register %c", reg ? reg : '"'); return 0; }
        long at = l2;
        if (bang) at = l2 - 1;
        long first = at;
        if (rg->type == 'l' || 1) {
            size_t n = rg->len;
            char *t = malloc(n + 1);
            memcpy(t, rg->s, n);
            if (rg->type != 'l' && n && t[n - 1] != '\n') t[n++] = '\n';
            put_lines_text(at, t, n);
            free(t);
        }
        cur.l = first >= 0 ? imin(nl - 1, first + (rg->type == 'l' ? (long)0 : 0)) : 0;
        long cnt2 = 0;
        for (size_t i = 0; i < rg->len; i++) if (rg->s[i] == '\n') cnt2++;
        if (rg->type != 'l') cnt2 = cnt2 + 1;
        cur.l = imin(nl - 1, first + (cnt2 ? cnt2 - 1 : 0));
        cur.c = first_nonblank(cur.l);
        return 1;
    }
    if (IS("copy") || IS("t") || IS("move")) {
        const char *a = arg;
        long dest;
        int r = parse_addr(&a, cur1(), &dest);
        if (r <= 0) { err("E14: Invalid address"); return 0; }
        if (dest < 0 || dest > nl) { err("E16: Invalid range"); return 0; }
        size_t n;
        Range rg = { { l1 - 1, 0 }, { l2 - 1, 0 }, 'l', 0, 0, 0 };
        char *t = range_text(&rg, &n);
        long cnt = l2 - l1 + 1;
        if (IS("move")) {
            if (dest >= l1 && dest < l2) { free(t); err("E134: Cannot move a range of lines into itself"); return 0; }
            if (dest == l2 || dest == l1 - 1) { free(t); cur.l = l2 - 1; cur.c = first_nonblank(cur.l); return 1; }
            if (dest > l2) { put_lines_text(dest, t, n); ln_delete(l1 - 1, cnt); cur.l = dest - 1; }
            else { ln_delete(l1 - 1, cnt); put_lines_text(dest, t, n); cur.l = dest + cnt - 1; }
        } else {
            put_lines_text(dest, t, n);
            cur.l = dest + cnt - 1;
        }
        free(t);
        cur.c = first_nonblank(cur.l);
        return 1;
    }
    if (IS("join")) {
        const char *a = skipws(arg);
        if (isdigit((unsigned char)*a)) { long c = strtol(a, NULL, 10); l1 = l2; l2 = imin(nl, l1 + c - 1); }
        if (l2 <= l1) { l2 = imin(nl, l1 + 1); }
        if (l1 >= nl) return 1;
        join_lines(l1 - 1, l2 - l1 + 1, !bang);
        return 1;
    }
    if (IS(">") || IS("<")) {
        int amount = 1;
        while (*arg == name[0]) { amount++; arg++; }
        shift_lines(l1 - 1, l2 - 1, (name[0] == '>' ? 1 : -1) * amount);
        cur.l = l2 - 1;
        cur.c = first_nonblank(cur.l);
        return 1;
    }
    if (IS("normal")) {
        const char *a = skipws(arg);
        if (!*a) { err("E471: Argument required"); return 0; }
        if (naddr == 0) { normal_run_keys(a, strlen(a)); return 1; }
        char *keys = strdup(a);
        for (long l = l1; l <= l2 && l <= nl; l++) {
            if (intr_check()) { err("Interrupted"); break; }
            cur.l = l - 1;
            cur.c = 0;
            normal_run_keys(keys, strlen(keys));
        }
        free(keys);
        return 1;
    }
    if (IS("mark") || IS("k")) {
        const char *a = skipws(arg);
        int k = *a;
        if (k >= 'a' && k <= 'z') { marks[k - 'a'].l = l2 - 1; marks[k - 'a'].c = 0; }
        else if (k == '\'' || k == '`') { mark_ctx.l = l2 - 1; mark_ctx.c = 0; }
        else { err("E191: Argument must be a letter or forward/backward quote"); return 0; }
        return 1;
    }
    if (IS("substitute") || IS("&") || IS("~")) {
        int kind = IS("&") ? '&' : IS("~") ? '~' : 's';
        if (kind == '&' && *arg == '&') { /* :&& keeps the flags */ }
        return cmd_substitute(l1, l2, arg, kind);
    }
    if (IS("global") || IS("vglobal")) {
        cmd_global(l1, l2, arg, IS("vglobal") || bang);
        return !ex_error;
    }
    if (IS("!")) {
        const char *cmd = arg;
        if (naddr == 0) {
            char *expanded = NULL;
            if (*cmd == '!') {                           /* :!! repeats the last command */
                extern char last_shell[512];
                if (!last_shell[0]) { err("E34: No previous command"); return 0; }
                expanded = strdup(last_shell);
                cmd = expanded;
            } else { extern char last_shell[512]; snprintf(last_shell, sizeof last_shell, "%s", cmd); }
            run_interactive(cmd);
            free(expanded);
            return 1;
        }
        extern char last_shell[512];
        if (*cmd == '!') cmd = last_shell; else snprintf(last_shell, sizeof last_shell, "%s", cmd);
        size_t n;
        Range rg = { { l1 - 1, 0 }, { l2 - 1, 0 }, 'l', 0, 0, 0 };
        char *t = range_text(&rg, &n);
        char *outp;
        size_t outl;
        int st = 0;
        if (run_filter(cmd, t, n, &outp, &outl, &st) != 0) { free(t); err("E485: Can't run the command"); return 0; }
        free(t);
        /* replace the lines with the output */
        ln_delete(l1 - 1, l2 - l1 + 1);
        long at = l1 - 1;
        if (nl == 1 && LN(0).len == 0 && l1 == 1 && at == 0) {
            if (outl) { put_lines_text(0, outp, outl); ln_delete(nl - 1, 1); }
        } else put_lines_text(at, outp, outl);
        long newlines = 0;
        for (size_t i = 0; i < outl; i++) if (outp[i] == '\n') newlines++;
        if (outl && outp[outl - 1] != '\n') newlines++;
        free(outp);
        cur.l = imin(nl - 1, l1 - 1);
        cur.c = first_nonblank(cur.l);
        if (newlines > 2 || l2 - l1 + 1 > 2) set_msg("%ld lines filtered", l2 - l1 + 1);
        return 1;
    }
    if (IS("print") || IS("number") || IS("list") || IS("#") || IS("=")) {
        if (IS("=")) { char tmp[32]; snprintf(tmp, sizeof tmp, "%ld\n", naddr ? l2 : nl); pb_adds(&ex_out, tmp); return 1; }
        for (long l = l1; l <= l2; l++) {
            if (IS("number") || IS("#")) { char pre[24]; snprintf(pre, sizeof pre, "%*ld ", 3, l); pb_adds(&ex_out, pre); }
            out_line(ls(l - 1), LN(l - 1).len);
        }
        cur.l = l2 - 1;
        cur.c = first_nonblank(cur.l);
        return 1;
    }
    if (IS("undo")) { undo_counts_reset(); if (!undo_do(0)) set_msg("Already at oldest change"); else undo_message(0); return 1; }
    if (IS("redo")) { undo_counts_reset(); if (!undo_do(1)) set_msg("Already at newest change"); else undo_message(1); return 1; }
    if (IS("nohlsearch")) { o.hls = 0; return 1; }
    if (IS("set") || IS("setlocal")) { set_option(skipws(arg)); return !ex_error; }
    if (IS("registers") || IS("display")) {
        const char *want = skipws(arg);
        int any = 0;
        for (const char *p = "\"0123456789abcdefghijklmnopqrstuvwxyz-.:%/"; *p; p++) {       /* Vim's order */
            int c = (unsigned char)*p;
            if (*want && !strchr(want, c)) continue;
            Reg *r = reg_get(c);
            if (!r || !r->s || (c == '.' && !r->len)) continue;
            if (!any) pb_adds(&ex_out, "Type Name Content\n");
            any = 1;
            pb_addf(&ex_out, "  %c  \"%c   ", r->type == 'l' ? 'l' : r->type == 'b' ? 'b' : 'c', c);
            for (size_t i = 0; i < r->len && i < 70; i++) {
                unsigned char ch = (unsigned char)r->s[i];
                if (ch == '\n') pb_adds(&ex_out, "^J");
                else if (ch < 32) { pb_addc(&ex_out, '^'); pb_addc(&ex_out, (char)(ch + 64)); }
                else if (ch == 127) pb_adds(&ex_out, "^?");
                else pb_addc(&ex_out, (char)ch);
            }
            pb_addc(&ex_out, '\n');
        }
        if (!any) pb_adds(&ex_out, "No registers\n");
        return 1;
    }
    if (IS("marks")) {
        const char *want = skipws(arg);
        struct { int name; Pos *p; } all[26 + 8];
        int n = 0;
        all[n].name = '\''; all[n++].p = &mark_ctx;
        for (int i = 0; i < 26; i++) { all[n].name = 'a' + i; all[n++].p = &marks[i]; }
        all[n].name = '['; all[n++].p = &mark_lb;
        all[n].name = ']'; all[n++].p = &mark_rb;
        all[n].name = '^'; all[n++].p = &mark_ins;
        all[n].name = '.'; all[n++].p = &mark_dot;
        all[n].name = '<'; all[n++].p = &mark_lt;
        all[n].name = '>'; all[n++].p = &mark_gt;
        int any = 0;
        for (int i = 0; i < n; i++) {
            Pos *p = all[i].p;
            if (p->l < 0 || p->l >= nl) continue;
            if (*want && !strchr(want, all[i].name)) continue;
            if (!any) pb_adds(&ex_out, "mark line  col file/text\n");
            any = 1;
            const char *t = ls(p->l);
            unsigned tl = LN(p->l).len;
            while (tl && (*t == ' ' || *t == '\t')) { t++; tl--; }
            pb_addf(&ex_out, " %c %6ld %4d ", all[i].name, p->l + 1, p->c);
            out_line(t, tl);
        }
        if (!any) pb_adds(&ex_out, "No marks set\n");
        return 1;
    }
    if (IS("jumps")) {
        pb_adds(&ex_out, " jump line  col file/text\n");
        for (int i = 0; i < njumps; i++) {
            if (jumps[i].l < 0 || jumps[i].l >= nl) continue;
            const char *t = ls(jumps[i].l);
            unsigned tl = LN(jumps[i].l).len;
            while (tl && (*t == ' ' || *t == '\t')) { t++; tl--; }
            pb_addf(&ex_out, " %c %2d %5ld %4d ", i == jumpidx ? '>' : ' ', i > jumpidx ? i - jumpidx : jumpidx - i, jumps[i].l + 1, jumps[i].c);
            out_line(t, tl);
        }
        if (jumpidx >= njumps) pb_adds(&ex_out, ">\n");
        return 1;
    }
    if (IS("changes") || IS("undolist")) { pb_adds(&ex_out, "(not kept)\n"); return 1; }
    if (IS("sort")) { cmd_sort(l1, l2, skipws(arg), bang); return 1; }
    if (IS("help")) { pb_adds(&ex_out, help_text); return 1; }
    if (IS("version")) { pb_adds(&ex_out, "vi (TouhouOS) 1.0 - a small vi/vim-lite\n"); return 1; }
    if (IS("map") || IS("nmap") || IS("noremap") || IS("nnoremap") || IS("imap") || IS("inoremap")) {
        int im = name[0] == 'i';
        const char *a = skipws(arg);
        if (!*a) { map_list(im); return 1; }
        const char *e = a;
        while (*e && *e != ' ' && *e != '\t') e++;
        char lhs[64];
        size_t ln = (size_t)(e - a) < sizeof lhs - 1 ? (size_t)(e - a) : sizeof lhs - 1;
        memcpy(lhs, a, ln);
        lhs[ln] = 0;
        const char *rhs = skipws(e);
        if (!*rhs) { set_msg("%s  (no mapping)", lhs); return 1; }
        map_add(im, lhs, rhs);
        return 1;
    }
    if (IS("unmap") || IS("nunmap") || IS("iunmap")) {
        const char *a = skipws(arg);
        if (!map_del(name[0] == 'i', a)) { err("E31: No such mapping"); return 0; }
        return 1;
    }
    if (IS("abbreviate") || IS("iabbrev")) {
        const char *a = skipws(arg);
        if (!*a) { abbr_list(&ex_out); return 1; }
        const char *e = a;
        while (*e && *e != ' ' && *e != '\t') e++;
        if (!*e) return 1;
        char lhs[64];
        size_t ln = (size_t)(e - a) < sizeof lhs - 1 ? (size_t)(e - a) : sizeof lhs - 1;
        memcpy(lhs, a, ln);
        lhs[ln] = 0;
        abbr_add(lhs, skipws(e));
        return 1;
    }
    if (IS("unabbreviate") || IS("iunabbrev")) {
        if (!abbr_del(skipws(arg))) { err("E24: No such abbreviation"); return 0; }
        return 1;
    }
    if (IS("abclear") || IS("iabclear")) { abbr_clear(); return 1; }
    if (IS("ascii")) { uint32_t cp = cp_at(cur.l, cur.c); set_msg("<%c>  %u,  Hex %02x", cp >= 32 && cp < 127 ? (int)cp : '?', cp, cp); return 1; }
    if (IS("redraw")) { force_repaint(); return 1; }
    if (IS("stop") || IS("suspend")) { force_repaint(); return 1; }
    if (IS("syntax") || IS("filetype") || IS("echo") || IS("let")) return 1;
    if (IS("startinsert")) { return 1; }
    if (IS("retab")) {
        for (long l = l1 - 1; l <= l2 - 1; l++) {
            PtBuf b = { 0 };
            const char *s = ls(l);
            int v = 0;
            for (unsigned i = 0; i < LN(l).len; i++) {
                if (s[i] == '\t') { int w = o.ts - v % o.ts; if (o.et) for (int k = 0; k < w; k++) pb_addc(&b, ' '); else pb_addc(&b, '\t'); v += w; }
                else { pb_addc(&b, s[i]); if (((unsigned char)s[i] & 0xC0) != 0x80) v++; }
            }
            if (b.len != LN(l).len || (b.len && memcmp(b.p, s, b.len))) ln_set(l, b.p ? b.p : "", b.len);
            pb_free(&b);
        }
        return 1;
    }
    if (IS("left") || IS("right") || IS("center")) {
        int width = isdigit((unsigned char)*skipws(arg)) ? atoi(skipws(arg)) : (o.tw > 0 ? o.tw : 80);
        for (long l = l1 - 1; l <= l2 - 1; l++) {
            const char *s = ls(l);
            int a0 = 0, e0 = (int)LN(l).len;
            while (a0 < e0 && (s[a0] == ' ' || s[a0] == '\t')) a0++;
            while (e0 > a0 && (s[e0 - 1] == ' ' || s[e0 - 1] == '\t')) e0--;
            PtBuf b = { 0 };
            int tw = (int)pt_u8_count(s + a0, (size_t)(e0 - a0));
            int pad = 0;
            if (IS("left")) pad = isdigit((unsigned char)*skipws(arg)) ? atoi(skipws(arg)) : 0;
            else if (IS("right")) pad = width - tw;
            else pad = (width - tw) / 2;
            if (pad < 0) pad = 0;
            if (e0 > a0) { for (int k = 0; k < pad; k++) pb_addc(&b, ' '); pb_add(&b, s + a0, (size_t)(e0 - a0)); }
            ln_set(l, b.p ? b.p : "", b.len);
            pb_free(&b);
        }
        return 1;
    }
    if (IS("append")) return 1;
    err("E492: Not an editor command: %s", name);
    return 0;
}

char last_shell[512];

void ex_line_prompt(const char *init) {
    PtBuf b = { 0 };
    char *copy = NULL;
    if (cmdline_read(":", init, &b)) {
        copy = strdup(b.p ? b.p : "");
    }
    pb_free(&b);
    if (!copy) return;
    if (copy[0]) snprintf(last_ex, sizeof last_ex, "%s", copy);   /* the ": register and @: */
    ex_execute(copy);
    free(copy);
}
