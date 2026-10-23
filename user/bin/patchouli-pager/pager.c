/*
 * patchouli-pager: Patchouli's pager, a less/more work-alike for the console.
 *
 *   patchouli-pager [options] [+LINE | +/PATTERN] [file ...]
 *
 * Reads files (or standard input; the keyboard is then read from /dev/tty), shows them a screen at a time and lets the
 * reader scroll, search and jump around. `less` and `more` are kappabox applets that start this program (as `more` it
 * quits at the end of the text). When standard output is not a terminal it behaves like cat.
 */
#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>
#include "../ptlib/pt_term.h"

/* ------------------------------------------------------------------ options */

typedef struct {
    int numbers;            /* -N */
    int chop;               /* -S */
    int raw;                /* -R: pass ANSI colour sequences through */
    int quit_one;           /* -F */
    int quit_eof;           /* -e / more */
    int more_mode;
    int icase_always;       /* -I */
    const char *title;      /* --title */
    const char *init_pattern;
    long init_line;
} Opts;

static Opts opt;

/* ------------------------------------------------------------------ documents */

typedef struct Doc {
    char *name;
    char *data;
    size_t len, cap;
    size_t *starts;         /* byte offset of the start of each line seen so far */
    long nstarts, scap;
    size_t scan;
    int fd, eof, is_stdin;
    long top_line;          /* remembered view while another file is shown */
    int top_sub;
    long hoff;
} Doc;

static Doc *g_docs;
static int g_ndocs, g_cur;
static int g_failed;        /* some file could not be opened */

static void doc_add_start(Doc *d, size_t off) {
    if (d->nstarts == d->scap) {
        d->scap = d->scap ? d->scap * 2 : 1024;
        d->starts = realloc(d->starts, (size_t)d->scap * sizeof *d->starts);
        if (!d->starts) pt_oom();
    }
    d->starts[d->nstarts++] = off;
}

static void doc_init(Doc *d, const char *name, int fd, int is_stdin) {
    memset(d, 0, sizeof *d);
    d->name = strdup(name);
    d->fd = fd;
    d->is_stdin = is_stdin;
    doc_add_start(d, 0);
}

static void doc_index(Doc *d) {
    for (size_t p = d->scan; p < d->len; p++)
        if (d->data[p] == '\n') doc_add_start(d, p + 1);
    d->scan = d->len;
}

/* Reads another block from the source. */
static void doc_read_more(Doc *d) {
    if (d->eof) return;
    size_t want = 65536;
    if (d->cap - d->len < want + 1) {
        size_t nc = d->cap ? d->cap * 2 : 131072;
        while (nc - d->len < want + 1) nc *= 2;
        char *nd = realloc(d->data, nc);
        if (!nd) pt_oom();
        d->data = nd;
        d->cap = nc;
    }
    ssize_t n;
    do n = read(d->fd, d->data + d->len, want); while (n < 0 && errno == EINTR);
    if (n <= 0) {
        d->eof = 1;
        if (!d->is_stdin && d->fd > 2) close(d->fd);
        d->fd = -1;
        return;
    }
    d->len += (size_t)n;
    doc_index(d);
}

/* Reads whatever is already waiting (without blocking), so short inputs are known to be complete. */
static void doc_prefetch(Doc *d) {
    for (int budget = 8; !d->eof && budget > 0; budget--) {
        struct pollfd pf;
        pf.fd = d->fd;
        pf.events = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, 0) <= 0) break;
        doc_read_more(d);
    }
}

/* Total number of lines; only meaningful once the whole input has been read. */
static long doc_total(Doc *d) {
    return d->nstarts - (d->starts[d->nstarts - 1] == d->len ? 1 : 0);
}

/* Makes sure line i is complete (or the last, unterminated one at end of input). Returns 0 if there is no such line. */
static int doc_ensure(Doc *d, long i) {
    if (i < 0) return 0;
    for (;;) {
        if (i + 1 < d->nstarts) return 1;
        if (d->eof) return i < doc_total(d);
        doc_read_more(d);
    }
}

static void doc_load_all(Doc *d) {
    while (!d->eof) doc_read_more(d);
}

static void doc_line(Doc *d, long i, const char **s, size_t *n) {
    size_t a = d->starts[i];
    size_t b = i + 1 < d->nstarts ? d->starts[i + 1] - 1 : d->len;
    if (b > a && d->data[b - 1] == '\r') b--;
    *s = d->data + a;
    *n = b - a;
}

static Doc *cur(void) { return &g_docs[g_cur]; }

/* ------------------------------------------------------------------ cells: one per terminal column */

typedef struct { uint32_t cp; uint32_t attr; } Cell;
enum { A_BOLD = 1, A_ULINE = 2, A_REV = 4 };
#define A_FG(a)       (((a) >> 8) & 31u)
#define A_BG(a)       (((a) >> 16) & 31u)
#define A_SETFG(a, v) (((a) & ~(31u << 8)) | ((uint32_t)(v) << 8))
#define A_SETBG(a, v) (((a) & ~(31u << 16)) | ((uint32_t)(v) << 16))

typedef struct { Cell *c; long n, cap; } CellBuf;
static CellBuf g_cells;

static void cb_push(CellBuf *b, uint32_t cp, uint32_t attr) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 512;
        b->c = realloc(b->c, (size_t)b->cap * sizeof(Cell));
        if (!b->c) pt_oom();
    }
    b->c[b->n].cp = cp;
    b->c[b->n].attr = attr;
    b->n++;
}

static uint32_t apply_sgr(uint32_t attr, const int *p, int np) {
    if (np == 0) return 0;
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) attr = 0;
        else if (v == 1) attr |= A_BOLD;
        else if (v == 4) attr |= A_ULINE;
        else if (v == 7) attr |= A_REV;
        else if (v == 22) attr &= ~(uint32_t)A_BOLD;
        else if (v == 24) attr &= ~(uint32_t)A_ULINE;
        else if (v == 27) attr &= ~(uint32_t)A_REV;
        else if (v >= 30 && v <= 37) attr = A_SETFG(attr, v - 29);
        else if (v == 39) attr = A_SETFG(attr, 0);
        else if (v >= 40 && v <= 47) attr = A_SETBG(attr, v - 39);
        else if (v == 49) attr = A_SETBG(attr, 0);
        else if (v >= 90 && v <= 97) attr = A_SETFG(attr, v - 89 + 8);
        else if (v >= 100 && v <= 107) attr = A_SETBG(attr, v - 99 + 8);
        else if ((v == 38 || v == 48) && i + 1 < np) i += p[i + 1] == 5 ? 2 : p[i + 1] == 2 ? 4 : 1;
    }
    return attr;
}

/* Turns one input line into display cells: tabs expand, control characters show as ^X, backspace overstrikes (nroff
 * style bold and underline) become attributes, and ANSI colour sequences either become attributes (-R) or are shown. */
static void build_cells(Doc *d, long li) {
    const char *s;
    size_t n;
    doc_line(d, li, &s, &n);
    g_cells.n = 0;
    uint32_t attr = 0;
    size_t i = 0;
    long col = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 27 && opt.raw && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            int params[16], np = 0, cur_v = 0, have = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                if (s[j] == ';') { if (np < 15) params[np++] = cur_v; cur_v = 0; have = 1; }
                else { cur_v = cur_v * 10 + (s[j] - '0'); have = 1; }
                j++;
            }
            if (have && np < 16) params[np++] = cur_v;
            if (j < n && s[j] == 'm') attr = apply_sgr(attr, params, np);
            if (j < n) { i = j + 1; continue; }     /* any other CSI sequence is swallowed */
            i = n;
            break;
        }
        if (ch == 27 && opt.raw && i + 1 < n && s[i + 1] == ']') {      /* OSC ... BEL or ESC \ */
            size_t j = i + 2;
            while (j < n && s[j] != 7 && !(s[j] == 27 && j + 1 < n && s[j + 1] == '\\')) j++;
            i = j < n ? j + (s[j] == 7 ? 1 : 2) : n;
            continue;
        }
        if (ch == '\b') {
            if (g_cells.n > 0 && i + 1 < n) {
                uint32_t ncp;
                int l = pt_u8_decode((const unsigned char *)s + i + 1, n - i - 1, &ncp);
                Cell *prev = &g_cells.c[g_cells.n - 1];
                if (prev->cp == '_' && ncp != '_') { prev->cp = ncp; prev->attr |= A_ULINE; }
                else if (prev->cp == ncp) prev->attr |= A_BOLD;
                else if (ncp == '_') prev->attr |= A_ULINE;
                else { i++; continue; }
                i += 1 + (size_t)l;
                continue;
            }
            i++;
            continue;
        }
        if (ch == '\t') {
            do { cb_push(&g_cells, ' ', attr); col++; } while (col % 8);
            i++;
            continue;
        }
        if (ch < 0x20 || ch == 0x7F) {
            cb_push(&g_cells, '^', attr | A_REV);
            cb_push(&g_cells, ch == 0x7F ? '?' : (uint32_t)(ch + 0x40), attr | A_REV);
            col += 2;
            i++;
            continue;
        }
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)s + i, n - i, &cp);
        if (pt_is_rawbyte(cp)) cp = '?';
        cb_push(&g_cells, cp, attr);
        col++;
        i += (size_t)l;
    }
}

static const char *attr_sgr(uint32_t a, char *buf, size_t cap) {
    size_t n = (size_t)snprintf(buf, cap, "\x1b[0");
    if (a & A_BOLD) n += (size_t)snprintf(buf + n, cap - n, ";1");
    if (a & A_ULINE) n += (size_t)snprintf(buf + n, cap - n, ";4");
    if (a & A_REV) n += (size_t)snprintf(buf + n, cap - n, ";7");
    unsigned fg = A_FG(a), bg = A_BG(a);
    if (fg) n += (size_t)snprintf(buf + n, cap - n, ";%u", fg <= 8 ? 29 + fg : 89 + (fg - 8));
    if (bg) n += (size_t)snprintf(buf + n, cap - n, ";%u", bg <= 8 ? 39 + bg : 99 + (bg - 8));
    snprintf(buf + n, cap - n, "m");
    return buf;
}

/* ------------------------------------------------------------------ search */

static char g_pattern[256];
static int g_pat_dir = 1;       /* 1 forward, -1 backward */
static regex_t g_re;
static int g_re_ok, g_pat_literal, g_pat_icase;

static int has_upper(const char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') return 1;
    return 0;
}

static int is_plain_pattern(const char *s) {
    return !strpbrk(s, ".[]()*+?{}|^$\\");
}

static void pattern_free(void) {
    if (g_re_ok) { regfree(&g_re); g_re_ok = 0; }
}

/* Compiles g_pattern. Returns 0 on success; invalid expressions fall back to a plain-text search. */
static int pattern_set(const char *pat) {
    pattern_free();
    snprintf(g_pattern, sizeof g_pattern, "%s", pat);
    g_pat_icase = opt.icase_always || !has_upper(g_pattern);
    g_pat_literal = is_plain_pattern(g_pattern);
    if (g_pat_literal) return 0;
    if (regcomp(&g_re, g_pattern, REG_EXTENDED | (g_pat_icase ? REG_ICASE : 0)) == 0) { g_re_ok = 1; return 0; }
    g_pat_literal = 1;
    return 1;
}

static const char *find_sub(const char *h, size_t hn, const char *nd, size_t nn, int icase) {
    if (!nn) return h;
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        if (icase) { while (k < nn && tolower((unsigned char)h[i + k]) == tolower((unsigned char)nd[k])) k++; }
        else { while (k < nn && h[i + k] == nd[k]) k++; }
        if (k == nn) return h + i;
    }
    return NULL;
}

/* Finds the first match at or after byte offset `from` in text (NUL terminated, n bytes). */
static int match_text(const char *text, size_t n, size_t from, size_t *ms, size_t *me) {
    if (g_pat_literal) {
        size_t pl = strlen(g_pattern);
        if (!pl) return 0;
        const char *hit = find_sub(text + from, n - from, g_pattern, pl, g_pat_icase);
        if (!hit) return 0;
        *ms = (size_t)(hit - text);
        *me = *ms + pl;
        return 1;
    }
    regmatch_t m[1];
    m[0].rm_so = (regoff_t)from;            /* REG_STARTEND: the text before `from` still counts for ^ and \b */
    m[0].rm_eo = (regoff_t)n;
    if (regexec(&g_re, text, 1, m, REG_STARTEND) != 0) return 0;
    *ms = (size_t)m[0].rm_so;
    *me = (size_t)m[0].rm_eo;
    if (*me == *ms) return 0;               /* empty matches are not worth showing */
    return 1;
}

static PtBuf g_plain;

/* The text of line li as it appears on screen (no escapes, tabs expanded), NUL terminated, in g_plain. */
static void plain_of_line(Doc *d, long li) {
    const char *s;
    size_t n;
    doc_line(d, li, &s, &n);
    pb_clear(&g_plain);
    int simple = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) { simple = 0; break; }
    }
    if (simple) { pb_add(&g_plain, s, n); return; }
    build_cells(d, li);
    for (long i = 0; i < g_cells.n; i++) {
        char u[4];
        pb_add(&g_plain, u, (size_t)pt_u8_encode(g_cells.c[i].cp, u));
    }
}

static int line_matches(Doc *d, long li) {
    plain_of_line(d, li);
    size_t ms, me;
    return match_text(g_plain.p ? g_plain.p : "", g_plain.len, 0, &ms, &me);
}

/* ------------------------------------------------------------------ view */

static PtTerm term;
static PtScreen scr;
static long top_line;           /* first displayed line */
static int top_sub;             /* ... and which wrapped row of it */
static long hoff;               /* horizontal offset in chop mode */
static int body_rows, text_w, gutter_w;
static char message[200];
static int at_end_flag;         /* last paint reached the end of the input */
static long bot_line;           /* last line (partly) displayed */
static long total_known = -1;

static void layout(void) {
    body_rows = term.rows - 1;
    if (body_rows < 1) body_rows = 1;
    gutter_w = 0;
    if (opt.numbers) {
        long total = cur()->eof ? doc_total(cur()) : cur()->nstarts;
        int digits = 1;
        for (long v = total; v >= 10; v /= 10) digits++;
        gutter_w = (digits < 5 ? 5 : digits) + 1;
    }
    text_w = term.cols - gutter_w;
    if (text_w < 1) text_w = 1;
}

static int line_nseg(Doc *d, long li) {
    if (opt.chop) return 1;
    build_cells(d, li);
    return g_cells.n <= text_w ? 1 : (int)((g_cells.n + text_w - 1) / text_w);
}

/* Moves the top of the window forward by k rows; returns how many rows it really moved. */
static int scroll_fwd(int k) {
    Doc *d = cur();
    int moved = 0;
    while (moved < k) {
        int ns = line_nseg(d, top_line);
        if (top_sub + 1 < ns) top_sub++;
        else if (doc_ensure(d, top_line + 1)) { top_line++; top_sub = 0; }
        else break;
        moved++;
    }
    return moved;
}

static int scroll_back(int k) {
    Doc *d = cur();
    int moved = 0;
    while (moved < k) {
        if (top_sub > 0) top_sub--;
        else if (top_line > 0) { top_line--; top_sub = line_nseg(d, top_line) - 1; }
        else break;
        moved++;
    }
    return moved;
}

/* Number of rows from the current top to the end of the input, counted up to `limit`. */
static int rows_to_end(int limit) {
    Doc *d = cur();
    long l = top_line;
    int sub = top_sub, rows = 0;
    while (rows < limit) {
        if (!doc_ensure(d, l)) break;
        int ns = line_nseg(d, l);
        rows += ns - sub;
        sub = 0;
        l++;
    }
    return rows;
}

/* Never leave blank space below the text when there is text above it. */
static void clamp_top(void) {
    int have = rows_to_end(body_rows);
    if (have < body_rows) scroll_back(body_rows - have);
}

static void goto_line(long n) {
    Doc *d = cur();
    if (n < 1) n = 1;
    if (!doc_ensure(d, n - 1)) {
        doc_load_all(d);
        n = doc_total(d);
        if (n < 1) n = 1;
    }
    top_line = n - 1;
    top_sub = 0;
    clamp_top();
}

static void goto_end(void) {
    Doc *d = cur();
    doc_load_all(d);
    long total = doc_total(d);
    if (total < 1) { top_line = 0; top_sub = 0; return; }
    top_line = total - 1;
    top_sub = line_nseg(d, top_line) - 1;
    scroll_back(body_rows - 1);
}

static void show_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void show_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
}

static void fill_status(PtRow *row, const char *left, const char *right) {
    pr_sgr(row, "7");
    int w = term.cols;
    int used = pr_str(row, left, w);
    int rl = right ? (int)pt_u8_count(right, strlen(right)) : 0;
    if (right && used + rl + 2 <= w) { pr_spaces(row, w - used - rl); pr_str(row, right, rl); }
    else pr_spaces(row, w - used);
}

static char g_prompt_extra[64];     /* "50" while a count is being typed */

static void paint(void) {
    Doc *d = cur();
    doc_prefetch(d);
    layout();
    if (scr.rows != term.rows || scr.cols != term.cols) pts_resize(&scr, term.rows, term.cols);
    long li = top_line;
    int sub = top_sub, r = 0;
    at_end_flag = 0;
    bot_line = top_line;
    char sgr[64];
    int more_text = 1;
    while (r < body_rows) {
        PtRow *row = pts_row(&scr, r);
        if (more_text && !doc_ensure(d, li)) more_text = 0;
        if (!more_text) {
            if (!opt.more_mode) { pr_sgr(row, "90"); pr_str(row, "~", 1); pr_reset(row); }
            r++;
            continue;
        }
        build_cells(d, li);
        long first = 0, last = g_cells.n;
        if (opt.chop) { first = hoff; if (first > g_cells.n) first = g_cells.n; last = first + text_w; }
        else { first = (long)sub * text_w; last = first + text_w; }
        if (last > g_cells.n) last = g_cells.n;
        int nseg = opt.chop ? 1 : (g_cells.n <= text_w ? 1 : (int)((g_cells.n + text_w - 1) / text_w));

        /* search matches on this line, as cell ranges */
        long ms_c[16], me_c[16];
        int nm = 0;
        if (g_pattern[0]) {
            plain_of_line(d, li);
            size_t from = 0, ms, me;
            const char *pt = g_plain.p ? g_plain.p : "";
            while (nm < 16 && from <= g_plain.len && match_text(pt, g_plain.len, from, &ms, &me)) {
                ms_c[nm] = (long)pt_u8_count(pt, ms);
                me_c[nm] = (long)pt_u8_count(pt, me);
                nm++;
                from = me;
            }
        }

        if (gutter_w) {
            if (opt.chop || sub == 0) { pr_sgr(row, "33"); char nb[24]; snprintf(nb, sizeof nb, "%*ld ", gutter_w - 1, li + 1); pr_str(row, nb, gutter_w); pr_reset(row); }
            else pr_spaces(row, gutter_w);
        }
        uint32_t prev_attr = 0;
        for (long c = first; c < last; c++) {
            uint32_t a = g_cells.c[c].attr;
            for (int m = 0; m < nm; m++) if (c >= ms_c[m] && c < me_c[m]) a |= A_REV;
            if (a != prev_attr) { pr_raw(row, attr_sgr(a, sgr, sizeof sgr)); prev_attr = a; }
            pr_cp(row, g_cells.c[c].cp);
        }
        if (prev_attr) pr_reset(row);
        bot_line = li;
        r++;
        if (opt.chop || sub + 1 >= nseg) {
            li++;
            sub = 0;
        } else sub++;
    }
    at_end_flag = sub == 0 && !doc_ensure(d, li);

    total_known = d->eof ? doc_total(d) : -1;
    /* status line */
    char left[300], right[80];
    const char *name = opt.title ? opt.title : d->name;
    long first_l = top_line + 1, last_l = bot_line + 1;
    int pct = 0;
    if (total_known > 0) pct = (int)(last_l * 100 / total_known);
    if (opt.more_mode) {
        if (at_end_flag) snprintf(left, sizeof left, "--More--(END)");
        else snprintf(left, sizeof left, "--More--(%d%%)", total_known > 0 ? pct : 0);
    } else if (total_known == 0) {
        snprintf(left, sizeof left, " %s  (empty)  (END)", name);
    } else {
        char tot[40] = "";
        if (total_known > 0) snprintf(tot, sizeof tot, "/%ld", total_known);
        char tail[32] = "";
        if (at_end_flag) snprintf(tail, sizeof tail, "  (END)");
        else if (total_known > 0) snprintf(tail, sizeof tail, "  %d%%", pct);
        if (g_ndocs > 1) snprintf(left, sizeof left, " %s [%d/%d]  lines %ld-%ld%s%s", name, g_cur + 1, g_ndocs, first_l, last_l, tot, tail);
        else snprintf(left, sizeof left, " %s  lines %ld-%ld%s%s", name, first_l, last_l, tot, tail);
    }
    snprintf(right, sizeof right, "h:help  q:quit ");
    PtRow *st = pts_row(&scr, term.rows - 1);
    if (message[0]) { pr_sgr(st, "1;7"); int u = pr_str(st, message, term.cols); pr_spaces(st, term.cols - u); }
    else fill_status(st, g_prompt_extra[0] ? g_prompt_extra : left, g_prompt_extra[0] ? NULL : right);
    pr_reset(st);
    pts_flush(&scr, &term, term.rows - 1, 0, 0);
}

/* ------------------------------------------------------------------ prompts */

/* Reads a line of input on the bottom row. Returns 1 on Enter, 0 on Esc/^C or when the line is emptied. */
static int read_line(const char *label, char *out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (;;) {
        PtRow *st = pts_row(&scr, term.rows - 1);
        pr_sgr(st, "0");
        char show[300];
        snprintf(show, sizeof show, "%s%s", label, out);
        int u = pr_str(st, show, term.cols - 1);
        pr_spaces(st, term.cols - u);
        pts_flush(&scr, &term, term.rows - 1, u < term.cols ? u : term.cols - 1, 1);
        int k = pt_term_getkey(&term, -1);
        if (k == PT_KEY_EOF) return 0;
        if (k == PT_KEY_NONE) continue;
        if (k == PT_K_ENTER) return 1;
        if (k == PT_K_ESC || k == PT_CTRL('c') || k == PT_CTRL('g')) return 0;
        if (k == PT_K_BACKSPACE) {
            if (len == 0) return 0;
            len = pt_u8_prev(out, len);
            out[len] = 0;
        } else if (k == PT_CTRL('u')) { len = 0; out[0] = 0; }
        else if (k >= 0x20 && k < PT_K_BASE && !(k & PT_M_ALT)) {
            char u8[4];
            int l = pt_u8_encode((uint32_t)k, u8);
            if (len + (size_t)l + 1 < cap) { memcpy(out + len, u8, (size_t)l); len += (size_t)l; out[len] = 0; }
        }
    }
}

/* ------------------------------------------------------------------ help */

static const char *const help_lines[] = {
    "Patchouli's pager -- a quiet corner of the library for long texts.",
    "",
    "MOVING",
    "  SPACE, f, ^F, PgDn       forward one screen (a count first: 5f = five screens)",
    "  b, ^B, PgUp              back one screen",
    "  ENTER, j, e, ^N, Down    forward one line (a count first: 10j)",
    "  k, y, ^P, Up             back one line",
    "  d, ^D                    forward half a screen",
    "  u, ^U                    back half a screen",
    "  g, <, Home               top of the text  (50g = line 50)",
    "  G, >, End                bottom of the text  (50G = line 50)",
    "  p, %                     jump to a percentage:  50p",
    "  Right, Left              scroll sideways (with -S, or while lines are chopped)",
    "",
    "SEARCHING",
    "  /pattern                 search forward (extended regular expression)",
    "  ?pattern                 search backward",
    "  n, N                     repeat the search in the same / the opposite direction",
    "  Searches ignore case unless the pattern contains an upper case letter (smartcase);",
    "  start the program with -I to always ignore case.",
    "",
    "OTHER",
    "  -N                       toggle line numbers        -S   toggle chopping of long lines",
    "  =, ^G                    show the file name and position",
    "  :n, :p                   next / previous file",
    "  r, ^L                    repaint the screen",
    "  h, H                     this help",
    "  q, Q, ZZ, ^C             quit",
    "",
    "Options:  -N line numbers   -S chop long lines   -R pass ANSI colours   -F quit if it fits one screen",
    "          -I always ignore case   -e quit at the end   +N start at line N   +/pat start at a pattern",
    "The LESS environment variable can hold default options.",
};

static void show_help(void) {
    int top = 0, n = (int)(sizeof help_lines / sizeof help_lines[0]);
    for (;;) {
        int body = term.rows - 1;
        for (int r = 0; r < body; r++) {
            PtRow *row = pts_row(&scr, r);
            if (top + r < n) {
                const char *l = help_lines[top + r];
                int heading = l[0] && l[0] != ' ' && strlen(l) < 12 && isupper((unsigned char)l[0]);
                if (heading) pr_sgr(row, "1;31");
                pr_str(row, l, term.cols);
                if (heading) pr_reset(row);
            }
        }
        PtRow *st = pts_row(&scr, term.rows - 1);
        fill_status(st, " HELP  (SPACE: next page, b: previous page, q: back to the text)", NULL);
        pr_reset(st);
        pts_flush(&scr, &term, term.rows - 1, 0, 0);
        int k = pt_term_getkey(&term, 300);
        if (k == PT_KEY_NONE) { if (pt_term_size(&term)) pts_resize(&scr, term.rows, term.cols); continue; }
        if (k == PT_KEY_EOF || k == 'q' || k == 'Q' || k == PT_K_ESC || k == 'h' || k == PT_CTRL('c')) return;
        if (k == ' ' || k == PT_K_PGDN || k == 'f' || k == PT_K_DOWN || k == PT_K_ENTER || k == 'j') {
            int step = (k == ' ' || k == PT_K_PGDN || k == 'f') ? body - 1 : 1;
            if (top + step < n) top += step;
        } else if (k == 'b' || k == PT_K_PGUP || k == PT_K_UP || k == 'k') {
            int step = (k == 'b' || k == PT_K_PGUP) ? body - 1 : 1;
            top = top - step < 0 ? 0 : top - step;
        }
    }
}

/* ------------------------------------------------------------------ commands */

/* Looks for the count-th line matching the current pattern, starting at line `start` and moving in direction dir. */
static void search_from(long start, int dir, long count) {
    Doc *d = cur();
    if (!g_pattern[0]) { show_message("No previous search pattern"); return; }
    if (count < 1) count = 1;
    long found = -1;
    long hits = 0;
    for (long li = start; ; li += dir) {
        if (dir > 0) { if (!doc_ensure(d, li)) break; }
        else if (li < 0) break;
        if (line_matches(d, li)) { found = li; if (++hits >= count) break; }
    }
    if (found < 0 || hits < count) { show_message("Pattern not found: %s", g_pattern); return; }
    top_line = found;
    top_sub = 0;
    clamp_top();
}

/* n / N: the search starts at the line after the top one (forward) or before it (backward), like less. */
static void do_search(int dir, long count) { search_from(dir > 0 ? top_line + 1 : top_line - 1, dir, count); }

static void search_prompt(int dir, long count) {
    char buf[256];
    if (!read_line(dir > 0 ? "/" : "?", buf, sizeof buf)) return;
    if (buf[0]) {
        if (pattern_set(buf)) show_message("Invalid regular expression: searching for the plain text");
    }
    g_pat_dir = dir;
    do_search(dir, count);
}

static void toggle_option(int c) {
    if (c == 'N') { opt.numbers = !opt.numbers; show_message("Line numbers %s", opt.numbers ? "on" : "off"); }
    else if (c == 'S') { opt.chop = !opt.chop; hoff = 0; top_sub = 0; show_message("Long lines are %s", opt.chop ? "chopped" : "wrapped"); }
    else if (c == 'I' || c == 'i') { opt.icase_always = !opt.icase_always; if (g_pattern[0]) { char p[256]; snprintf(p, sizeof p, "%s", g_pattern); pattern_set(p); } show_message("Searches %s", opt.icase_always ? "always ignore case" : "use smartcase"); }
    else if (c == 'R') { opt.raw = !opt.raw; show_message("ANSI colours %s", opt.raw ? "passed through" : "shown as text"); }
    else show_message("Unknown option -%c", c);
}

static void switch_doc(int idx) {
    Doc *d = cur();
    d->top_line = top_line;
    d->top_sub = top_sub;
    d->hoff = hoff;
    g_cur = idx;
    d = cur();
    top_line = d->top_line;
    top_sub = d->top_sub;
    hoff = d->hoff;
}

static void file_info(void) {
    Doc *d = cur();
    long total = d->eof ? doc_total(d) : -1;
    if (total >= 0) show_message("\"%s\"  lines %ld-%ld of %ld  (%ld bytes)", d->name, top_line + 1, bot_line + 1, total, (long)d->len);
    else show_message("\"%s\"  lines %ld-%ld  (%ld bytes read so far)", d->name, top_line + 1, bot_line + 1, (long)d->len);
}

/* Returns 1 to quit. */
static int run_command(int k, long count, int have_count) {
    Doc *d = cur();
    int half = body_rows / 2 > 0 ? body_rows / 2 : 1;
    int page = body_rows - 1 > 0 ? body_rows - 1 : 1;
    switch (k) {
    case ' ': case 'f': case PT_CTRL('f'): case PT_K_PGDN: case 'z':
        if (have_count && k != 'z') { /* a count after space/f means that many lines, as in less */ }
        scroll_fwd(have_count ? (int)count : page);
        clamp_top();
        break;
    case 'b': case PT_CTRL('b'): case PT_K_PGUP: case 'w':
        scroll_back(have_count ? (int)count : page);
        break;
    case PT_K_ENTER: case 'e': case PT_CTRL('e'): case 'j': case PT_CTRL('n'): case PT_K_DOWN:
        scroll_fwd(have_count ? (int)count : 1);
        clamp_top();
        break;
    case 'y': case PT_CTRL('y'): case 'k': case PT_CTRL('p'): case PT_K_UP:
        scroll_back(have_count ? (int)count : 1);
        break;
    case 'd': case PT_CTRL('d'):
        scroll_fwd(have_count ? (int)count : half);
        clamp_top();
        break;
    case 'u': case PT_CTRL('u'):
        scroll_back(have_count ? (int)count : half);
        break;
    case 'g': case '<': case PT_K_HOME:
        goto_line(have_count ? count : 1);
        break;
    case 'G': case '>': case PT_K_END:
        if (have_count) goto_line(count); else goto_end();
        break;
    case 'p': case '%': {
        doc_load_all(d);
        long total = doc_total(d);
        goto_line(total * (have_count ? count : 0) / 100 + 1);
        break;
    }
    case '/': search_prompt(1, have_count ? count : 1); break;
    case '?': search_prompt(-1, have_count ? count : 1); break;
    case 'n': do_search(g_pat_dir, have_count ? count : 1); break;
    case 'N': do_search(-g_pat_dir, have_count ? count : 1); break;
    case PT_K_RIGHT: if (opt.chop) hoff += text_w / 2; break;
    case PT_K_LEFT: if (opt.chop) { hoff -= text_w / 2; if (hoff < 0) hoff = 0; } break;
    case 'r': case 'R': case PT_CTRL('l'): case PT_CTRL('r'): pts_invalidate(&scr); break;
    case '=': case PT_CTRL('g'): file_info(); break;
    case 'h': case 'H': show_help(); pts_invalidate(&scr); break;
    case 'q': case 'Q': case PT_CTRL('c'): return 1;
    case '-': {
        int c = pt_term_getkey(&term, -1);
        if (c > 0 && c < PT_K_BASE) toggle_option(c);
        break;
    }
    case ':': {
        int c = pt_term_getkey(&term, -1);
        if (c == 'q' || c == 'Q') return 1;
        if (c == 'n') { if (g_cur + 1 < g_ndocs) { switch_doc(g_cur + 1); } else show_message("No next file"); }
        else if (c == 'p') { if (g_cur > 0) switch_doc(g_cur - 1); else show_message("No previous file"); }
        break;
    }
    case 'Z': {
        int c = pt_term_getkey(&term, -1);
        if (c == 'Z') return 1;
        break;
    }
    default:
        break;
    }
    return 0;
}

static void interactive(void) {
    pt_term_enter(&term, 1);
    pt_term_size(&term);
    pts_resize(&scr, term.rows, term.cols);
    top_line = 0;
    top_sub = 0;
    hoff = 0;
    layout();
    if (opt.init_pattern) {
        if (pattern_set(opt.init_pattern)) show_message("Invalid regular expression: searching for the plain text");
        search_from(0, 1, 1);
    } else if (opt.init_line > 1) goto_line(opt.init_line);
    long count = 0;
    int have_count = 0;
    for (;;) {
        g_prompt_extra[0] = 0;
        if (have_count) snprintf(g_prompt_extra, sizeof g_prompt_extra, " %ld", count);
        paint();
        message[0] = 0;
        int k = pt_term_getkey(&term, 300);
        if (k == PT_KEY_EOF) break;
        if (k == PT_KEY_NONE) {
            if (pt_term_size(&term)) pts_resize(&scr, term.rows, term.cols);
            continue;
        }
        if (k >= '0' && k <= '9' && (have_count || k != '0')) {
            count = count * 10 + (k - '0');
            if (count > 100000000) count = 100000000;
            have_count = 1;
            continue;
        }
        int wasend = at_end_flag;
        int quit = run_command(k, count, have_count);
        count = 0;
        have_count = 0;
        if (quit) break;
        if (opt.quit_eof && wasend && (k == ' ' || k == 'f' || k == PT_K_PGDN || k == PT_K_ENTER || k == 'j' || k == 'd' || k == PT_K_DOWN || k == PT_CTRL('f'))) break;
    }
    pt_term_leave(&term);
}

/* ------------------------------------------------------------------ non-interactive output */

static void cat_doc(Doc *d) {
    doc_load_all(d);
    long total = doc_total(d);
    if (!opt.numbers) {
        size_t off = 0;
        while (off < d->len) {
            ssize_t w = write(1, d->data + off, d->len - off);
            if (w < 0) { if (errno == EINTR) continue; return; }
            off += (size_t)w;
        }
        return;
    }
    for (long i = 0; i < total; i++) {
        const char *s;
        size_t n;
        doc_line(d, i, &s, &n);
        printf("%6ld  ", i + 1);
        fwrite(s, 1, n, stdout);
        putchar('\n');
    }
    fflush(stdout);
}

/* -F: does the whole input fit on one screen? */
static int fits_one_screen(void) {
    top_line = 0;
    top_sub = 0;
    layout();
    return rows_to_end(body_rows + 1) <= body_rows;
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *f) {
    fprintf(f,
        "usage: patchouli-pager [options] [+LINE | +/PATTERN] [file ...]\n"
        "  -N  line numbers        -S  chop long lines (scroll sideways with the arrows)\n"
        "  -R  pass ANSI colours   -F  quit at once if the text fits one screen\n"
        "  -I  always ignore case in searches (the default is smartcase)\n"
        "  -e  quit when the end is reached    --title TEXT  name shown in the status line\n"
        "  -p PATTERN  start at the first match      +N  start at line N      +/PATTERN  same as -p\n"
        "  -V  version             -h  this help\n"
        "Reads standard input when no file is named. Press h inside for the key bindings.\n");
}

static int parse_flags(const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case 'N': opt.numbers = 1; break;
        case 'S': opt.chop = 1; break;
        case 'R': case 'r': opt.raw = 1; break;
        case 'F': opt.quit_one = 1; break;
        case 'I': opt.icase_always = 1; break;
        case 'e': case 'E': opt.quit_eof = 1; break;
        case 'i': case 'X': case 'q': case 'Q': case 'M': case 'm': case 's': case 'c': case 'd': case 'u': case 'f': case 'n': break;
        case 'h': usage(stdout); exit(0);
        case 'V': puts("patchouli-pager 1.0 (TouhouOS)"); exit(0);
        default:
            fprintf(stderr, "patchouli-pager: unknown option -%c (try -h)\n", *s);
            return -1;
        }
    }
    return 0;
}

static void parse_env_options(void) {
    const char *e = getenv("LESS");
    if (!e) return;
    char *copy = strdup(e), *save, *tok;
    for (tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
        if (tok[0] == '-' && tok[1] != '-') parse_flags(tok + 1);
    free(copy);
}

int main(int argc, char **argv) {
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "more")) { opt.more_mode = 1; opt.quit_eof = 1; opt.quit_one = 1; }
    opt.init_line = 1;
    parse_env_options();
    int nfiles = 0;
    const char **files = calloc((size_t)argc + 1, sizeof *files);
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "--version")) { puts("patchouli-pager 1.0 (TouhouOS)"); return 0; }
        if (!strcmp(a, "--title") && i + 1 < argc) { opt.title = argv[++i]; continue; }
        if (!strncmp(a, "--title=", 8)) { opt.title = a + 8; continue; }
        if (!strcmp(a, "-p") && i + 1 < argc) { opt.init_pattern = argv[++i]; continue; }
        if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "patchouli-pager: unknown option %s (try -h)\n", a); return 2; }
        if (a[0] == '-' && a[1]) { if (parse_flags(a + 1) < 0) return 2; continue; }
        if (a[0] == '+' && a[1]) {
            if (a[1] == '/') opt.init_pattern = a + 2;
            else if (isdigit((unsigned char)a[1])) opt.init_line = atol(a + 1);
            continue;
        }
        files[nfiles++] = a;
    }
    for (; i < argc; i++) files[nfiles++] = argv[i];

    int out_tty = isatty(1);
    g_docs = calloc((size_t)(nfiles ? nfiles : 1), sizeof(Doc));
    if (nfiles == 0) {
        if (isatty(0)) { fprintf(stderr, "patchouli-pager: missing filename (\"patchouli-pager -h\" for help)\n"); return 1; }
        doc_init(&g_docs[g_ndocs++], "(standard input)", 0, 1);
    }
    for (int k = 0; k < nfiles; k++) {
        if (!strcmp(files[k], "-")) {
            if (isatty(0)) { fprintf(stderr, "patchouli-pager: standard input is the terminal\n"); g_failed = 1; continue; }
            doc_init(&g_docs[g_ndocs++], "(standard input)", 0, 1);
            continue;
        }
        struct stat st;
        int fd = open(files[k], O_RDONLY);
        if (fd < 0 || fstat(fd, &st) < 0) {
            fprintf(stderr, "patchouli-pager: %s: %s\n", files[k], strerror(errno));
            if (fd >= 0) close(fd);
            g_failed = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            fprintf(stderr, "patchouli-pager: %s: Is a directory\n", files[k]);
            close(fd);
            g_failed = 1;
            continue;
        }
        doc_init(&g_docs[g_ndocs++], files[k], fd, 0);
    }
    if (g_ndocs == 0) return 1;

    if (!out_tty) {
        for (int k = 0; k < g_ndocs; k++) cat_doc(&g_docs[k]);
        return g_failed ? 1 : 0;
    }
    if (pt_term_open(&term) < 0) {
        fprintf(stderr, "patchouli-pager: no terminal to read keys from\n");
        for (int k = 0; k < g_ndocs; k++) cat_doc(&g_docs[k]);
        return 1;
    }
    pt_term_size(&term);
    if (opt.quit_one && g_ndocs == 1 && fits_one_screen()) {
        cat_doc(cur());
        return g_failed ? 1 : 0;
    }
    g_cur = 0;
    interactive();
    return g_failed ? 1 : 0;
}
