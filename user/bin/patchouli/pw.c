/* The .pw page format: parser, renderer and checker.
 *
 *   .page NAME SECTION One line description        header (first thing in the file)
 *   .alias NAME...                                 other names for the page (optional, before the first heading)
 *   ; comment
 *   # HEADING                                      section heading (SYNOPSIS, DESCRIPTION, OPTIONS, ...)
 *   ## Subheading
 *   paragraph text, joined and re-wrapped         a blank line ends a paragraph
 *       indented four or more spaces              verbatim example
 *   - bullet text                                  bullet item, continuation lines are indented
 *   @ term                                         definition item; its text follows, indented four spaces
 *       text more deeply indented (8)              example inside an item
 *   inline: *bold*  _underline_  `literal` (bold, no markup inside)  and \ escapes the next character
 *
 * In a SYNOPSIS section every line is kept as a line of its own.
 */
#include "patchouli.h"

/* ------------------------------------------------------------------ errors and blocks */

static void perr(Page *pg, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void perr(Page *pg, int line, const char *fmt, ...) {
    if (pg->nerr == pg->errcap) {
        pg->errcap = pg->errcap ? pg->errcap * 2 : 8;
        pg->err = realloc(pg->err, (size_t)pg->errcap * sizeof *pg->err);
        if (!pg->err) pt_oom();
    }
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pg->err[pg->nerr].line = line;
    pg->err[pg->nerr].msg = strdup(buf);
    pg->nerr++;
}

static Block *add_block(Page *pg, int kind, int line) {
    if (pg->nb == pg->cap) {
        pg->cap = pg->cap ? pg->cap * 2 : 32;
        pg->b = realloc(pg->b, (size_t)pg->cap * sizeof *pg->b);
        if (!pg->b) pt_oom();
    }
    Block *b = &pg->b[pg->nb++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->line = line;
    return b;
}

void pw_free(Page *pg) {
    free(pg->name);
    free(pg->sec);
    free(pg->desc);
    for (int i = 0; i < pg->naliases; i++) free(pg->aliases[i]);
    free(pg->aliases);
    for (int i = 0; i < pg->nb; i++) {
        Block *b = &pg->b[i];
        free(b->text);
        for (int k = 0; k < b->nlines; k++) free(b->lines[k]);
        free(b->lines);
    }
    free(pg->b);
    for (int i = 0; i < pg->nerr; i++) free(pg->err[i].msg);
    free(pg->err);
    memset(pg, 0, sizeof *pg);
}

static void text_append(Block *b, const char *s) {
    size_t old = b->text ? strlen(b->text) : 0, add = strlen(s);
    b->text = realloc(b->text, old + add + 2);
    if (!b->text) pt_oom();
    if (old) b->text[old++] = ' ';
    memcpy(b->text + old, s, add + 1);
}

static void verb_append(Block *b, const char *s) {
    b->lines = realloc(b->lines, (size_t)(b->nlines + 1) * sizeof(char *));
    if (!b->lines) pt_oom();
    b->lines[b->nlines++] = strdup(s);
}

/* ------------------------------------------------------------------ parser */

static char *trim_dup(const char *s) {
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    char *r = malloc(n + 1);
    if (!r) pt_oom();
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static void parse_header(Page *pg, const char *text, int line) {
    if (pg->name) { perr(pg, line, "second .page line"); return; }
    char *copy = strdup(text), *save;
    char *name = strtok_r(copy, " ", &save);
    char *sec = name ? strtok_r(NULL, " ", &save) : NULL;
    char *desc = sec ? save : NULL;
    if (!name || !sec) { perr(pg, line, ".page needs a name and a section number"); free(copy); return; }
    pg->name = strdup(name);
    pg->sec = strdup(sec);
    pg->desc = trim_dup(desc ? desc : "");
    pg->header_line = line;
    free(copy);
}

void pw_parse(Page *pg, const char *src, size_t len) {
    enum { NONE, PARA, VERB } open = NONE;
    Block *cur = NULL;
    int in_item = 0, pending_blank = 0, blank_seen = 0, seen_heading = 0, line = 0;
    char section[80] = "";
    const char *p = src, *end = src + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        line++;
        char *raw = malloc(ll + 1);
        if (!raw) pt_oom();
        memcpy(raw, p, ll);
        raw[ll] = 0;
        p = nl ? nl + 1 : end;
        while (ll && (raw[ll - 1] == '\r' || raw[ll - 1] == ' ')) raw[--ll] = 0;
        if (strchr(raw, '\t')) {
            perr(pg, line, "tab character in the source (use spaces)");
            for (char *t = raw; *t; t++) if (*t == '\t') *t = ' ';
        }
        if (!raw[0]) {                                      /* blank line */
            if (open == PARA) open = NONE;
            else if (open == VERB) pending_blank++;
            blank_seen = 1;
            free(raw);
            continue;
        }
        int indent = 0;
        while (raw[indent] == ' ') indent++;
        const char *text = raw + indent;

        if (indent == 0 && text[0] == ';') { free(raw); continue; }
        if (indent == 0 && text[0] == '.' && isalpha((unsigned char)text[1])) {
            open = NONE;
            if (!strncmp(text, ".page ", 6) || !strcmp(text, ".page")) {
                if (seen_heading) perr(pg, line, ".page after the first heading");
                parse_header(pg, text[5] ? text + 6 : "", line);
            } else if (!strncmp(text, ".alias ", 7)) {
                if (seen_heading) perr(pg, line, ".alias after the first heading");
                char *copy = strdup(text + 7), *save, *tok;
                for (tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
                    pg->aliases = realloc(pg->aliases, (size_t)(pg->naliases + 1) * sizeof(char *));
                    if (!pg->aliases) pt_oom();
                    pg->aliases[pg->naliases++] = strdup(tok);
                }
                free(copy);
            } else perr(pg, line, "unknown directive '%.*s'", (int)strcspn(text, " "), text);
            free(raw);
            continue;
        }
        if (indent == 0 && (text[0] == '#')) {
            int lvl = text[1] == '#' ? 2 : 1;
            const char *title = text + lvl;
            if (*title == ' ') title++;
            else if (*title) { perr(pg, line, "a heading needs a space after the #"); }
            if (!*title) perr(pg, line, "empty heading");
            open = NONE;
            in_item = 0;
            Block *b = add_block(pg, lvl == 1 ? BK_HEADING : BK_SUBHEADING, line);
            b->text = strdup(title);
            if (lvl == 1) { seen_heading = 1; snprintf(section, sizeof section, "%s", title); }
            blank_seen = 0;
            free(raw);
            continue;
        }
        if (indent == 0 && text[0] == '@' && (text[1] == ' ' || !text[1])) {
            open = NONE;
            in_item = 1;
            Block *b = add_block(pg, BK_ITEM, line);
            b->text = trim_dup(text + 1);
            if (!b->text[0]) perr(pg, line, "empty item term");
            blank_seen = 0;
            free(raw);
            continue;
        }

        int verbatim = in_item ? indent >= 8 : indent >= 4;
        if (in_item && indent == 0) in_item = 0;
        else if (in_item && indent < 4) perr(pg, line, "text inside an item must be indented four spaces");

        if (verbatim) {
            int rel = in_item ? indent - 8 : indent - 4;
            if (open != VERB) {
                cur = add_block(pg, BK_VERB, line);
                cur->in_item = in_item;
                open = VERB;
                pending_blank = 0;
            }
            while (pending_blank > 0) { verb_append(cur, ""); pending_blank--; }
            char *l = malloc((size_t)rel + strlen(text) + 1);
            if (!l) pt_oom();
            memset(l, ' ', (size_t)rel);
            strcpy(l + rel, text);
            verb_append(cur, l);
            free(l);
            blank_seen = 0;
            free(raw);
            continue;
        }
        pending_blank = 0;

        int bullet = indent == 0 && text[0] == '-' && text[1] == ' ';
        int in_syn = !strcmp(section, "SYNOPSIS");
        if (bullet) {
            cur = add_block(pg, BK_BULLET, line);
            cur->text = strdup("");
            text_append(cur, text + 2);
            open = PARA;
        } else if (open == PARA && !in_syn) {
            text_append(cur, text);
        } else {
            cur = add_block(pg, in_syn && !in_item ? BK_SYN : BK_PARA, line);
            cur->in_item = in_item;
            cur->text = strdup("");
            text_append(cur, text);
            open = in_syn ? NONE : PARA;
            if (in_syn) cur->indent = blank_seen;           /* blank line before: kept as a gap */
        }
        blank_seen = 0;
        free(raw);
    }
}

/* ------------------------------------------------------------------ inline markup */

typedef struct { uint32_t cp; unsigned char st; } Ch;
enum { S_BOLD = 1, S_ULINE = 2 };
typedef struct { Ch *c; size_t n, cap; } ChBuf;

static void ch_push(ChBuf *b, uint32_t cp, unsigned char st) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->c = realloc(b->c, b->cap * sizeof(Ch));
        if (!b->c) pt_oom();
    }
    b->c[b->n].cp = cp;
    b->c[b->n].st = st;
    b->n++;
}

static int open_ctx(char prev) { return prev == 0 || isspace((unsigned char)prev) || strchr("([{\"'/=|,", prev) != NULL; }
static int close_ctx(char next) { return next == 0 || isspace((unsigned char)next) || strchr(".,;:)]}!?\"'/-|=", next) != NULL; }

static int closer_exists(const char *s, size_t from, char c) {
    for (size_t j = from; s[j]; j++) {
        if (s[j] == '\\' && s[j + 1]) { j++; continue; }
        if (s[j] == '`') {
            const char *e = strchr(s + j + 1, '`');
            if (e) { j = (size_t)(e - s); continue; }
        }
        if (s[j] == c && j > from && !isspace((unsigned char)s[j - 1]) && close_ctx(s[j + 1])) return 1;
    }
    return 0;
}

/* Turns marked-up text into styled characters. Returns the number of unmatched backticks (a mistake in the page). */
static int inline_parse(const char *s, ChBuf *out, int *trailing_backslash) {
    int bold = 0, uline = 0, bad = 0;
    char prev = 0;
    if (trailing_backslash) *trailing_backslash = 0;
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        unsigned char st = (unsigned char)((bold ? S_BOLD : 0) | (uline ? S_ULINE : 0));
        if (c == '\\') {
            if (!s[i + 1]) { if (trailing_backslash) *trailing_backslash = 1; i++; continue; }
            uint32_t cp;
            int l = pt_u8_decode((const unsigned char *)s + i + 1, strlen(s + i + 1), &cp);
            ch_push(out, cp, st);
            prev = s[i + 1];
            i += 1 + (size_t)l;
            continue;
        }
        if (c == '`') {
            const char *e = strchr(s + i + 1, '`');
            if (e) {
                size_t j = i + 1;
                while (s + j < e) {
                    uint32_t cp;
                    int l = pt_u8_decode((const unsigned char *)s + j, (size_t)(e - s) - j, &cp);
                    ch_push(out, cp, (unsigned char)(st | S_BOLD));
                    j += (size_t)l;
                }
                prev = '`';
                i = (size_t)(e - s) + 1;
                continue;
            }
            bad++;
        } else if (c == '*' || c == '_') {
            int *flag = c == '*' ? &bold : &uline;
            if (*flag) {
                if (prev && !isspace((unsigned char)prev) && close_ctx(s[i + 1])) { *flag = 0; prev = (char)c; i++; continue; }
            } else if (open_ctx(prev) && s[i + 1] && !isspace((unsigned char)s[i + 1]) && s[i + 1] != (char)c && closer_exists(s, i + 1, (char)c)) {
                *flag = 1;
                prev = (char)c;
                i++;
                continue;
            }
        }
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)s + i, strlen(s + i), &cp);
        ch_push(out, cp, st);
        prev = s[i];
        i += (size_t)l;
    }
    return bad;
}

/* ------------------------------------------------------------------ output helpers */

static void put_cp(PtBuf *o, uint32_t cp) {
    char u[4];
    pb_add(o, u, (size_t)pt_u8_encode(cp, u));
}

static void set_style(PtBuf *o, unsigned char *cur, unsigned char want, int ansi) {
    if (!ansi || *cur == want) return;
    pb_adds(o, "\x1b[0m");
    if (want & S_BOLD) pb_adds(o, "\x1b[1m");
    if (want & S_ULINE) pb_adds(o, "\x1b[4;36m");
    *cur = want;
}

static void spaces(PtBuf *o, int n) { while (n-- > 0) pb_addc(o, ' '); }

/* Writes chars with greedy word wrapping. The first line starts at column ind1 (nothing is indented when first_inline
 * is set: the caller has already written text up to that column), later lines at indn. Returns the lines written. */
static int emit_wrapped(PtBuf *o, const Ch *c, size_t n, int ind1, int indn, int width, int ansi, int first_inline) {
    int lines = 0, col = 0, started = 0;
    unsigned char cur = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && c[i].cp == ' ') i++;
        if (i >= n) break;
        size_t j = i;
        while (j < n && c[j].cp != ' ') j++;
        int wl = (int)(j - i);
        if (!started) {
            if (!first_inline) spaces(o, ind1);
            col = ind1;
            started = 1;
            lines = 1;
        } else if (col + 1 + wl > width && col > indn) {
            set_style(o, &cur, 0, ansi);
            pb_addc(o, '\n');
            spaces(o, indn);
            col = indn;
            lines++;
        } else {
            set_style(o, &cur, 0, ansi);
            pb_addc(o, ' ');
            col++;
        }
        for (size_t k = i; k < j; k++) {
            set_style(o, &cur, c[k].st, ansi);
            put_cp(o, c[k].cp);
        }
        col += wl;
        i = j;
    }
    set_style(o, &cur, 0, ansi);
    if (started) pb_addc(o, '\n');
    return lines;
}

/* ------------------------------------------------------------------ renderer */

#define BODY 7

static void centered_line(PtBuf *o, const char *left, const char *mid, const char *right, int width, int ansi) {
    int l = (int)strlen(left), m = (int)strlen(mid), r = (int)strlen(right);
    int gap1 = (width - l - m - r) / 2, gap2 = width - l - m - r - gap1;
    if (gap1 < 1) gap1 = 1;
    if (gap2 < 1) gap2 = 1;
    if (ansi) pb_adds(o, "\x1b[90m");
    pb_adds(o, left);
    spaces(o, gap1);
    pb_adds(o, mid);
    spaces(o, gap2);
    pb_adds(o, right);
    if (ansi) pb_adds(o, "\x1b[0m");
    pb_addc(o, '\n');
}

void pw_render(Page *pg, PtBuf *out, int width, int ansi) {
    char head[160];
    snprintf(head, sizeof head, "%s(%s)", pg->name ? pg->name : "?", pg->sec ? pg->sec : "?");
    centered_line(out, head, "Patchouli's Library", head, width, ansi);

    /* NAME */
    pb_addc(out, '\n');
    if (ansi) pb_adds(out, "\x1b[1;31m");
    pb_adds(out, "NAME");
    if (ansi) pb_adds(out, "\x1b[0m");
    pb_addc(out, '\n');
    spaces(out, BODY);
    if (ansi) pb_adds(out, "\x1b[1m");
    pb_adds(out, pg->name ? pg->name : "?");
    if (ansi) pb_adds(out, "\x1b[0m");
    pb_adds(out, " - ");
    pb_adds(out, pg->desc ? pg->desc : "");
    pb_addc(out, '\n');

    int prev = -1;                                          /* kind of the previous block, -1 at the start */
    for (int bi = 0; bi < pg->nb; bi++) {
        Block *b = &pg->b[bi];
        ChBuf chars = { 0 };
        switch (b->kind) {
        case BK_HEADING:
            pb_addc(out, '\n');
            if (ansi) pb_adds(out, "\x1b[1;31m");
            pb_adds(out, b->text);
            if (ansi) pb_adds(out, "\x1b[0m");
            pb_addc(out, '\n');
            break;
        case BK_SUBHEADING:
            pb_addc(out, '\n');
            spaces(out, 3);
            if (ansi) pb_adds(out, "\x1b[1;33m");
            pb_adds(out, b->text);
            if (ansi) pb_adds(out, "\x1b[0m");
            pb_addc(out, '\n');
            break;
        case BK_SYN: {
            inline_parse(b->text, &chars, NULL);
            if (prev != BK_HEADING && (prev != BK_SYN || b->indent)) pb_addc(out, '\n');
            emit_wrapped(out, chars.c, chars.n, BODY, BODY + 4, width, ansi, 0);
            break;
        }
        case BK_PARA:
        case BK_BULLET: {
            inline_parse(b->text, &chars, NULL);
            int ind = b->in_item ? BODY + 7 : BODY;
            int tight = prev == BK_HEADING || (b->kind == BK_BULLET && prev == BK_BULLET) || (b->in_item && prev == BK_ITEM);
            if (!tight) pb_addc(out, '\n');
            if (b->kind == BK_BULLET) {
                spaces(out, ind);
                pb_adds(out, "- ");
                emit_wrapped(out, chars.c, chars.n, ind + 2, ind + 2, width, ansi, 1);
            } else emit_wrapped(out, chars.c, chars.n, ind, ind, width, ansi, 0);
            break;
        }
        case BK_VERB: {
            int ind = (b->in_item ? BODY + 7 : BODY + 4);
            int tight = prev == BK_HEADING;
            if (!tight) pb_addc(out, '\n');
            for (int k = 0; k < b->nlines; k++) {
                if (b->lines[k][0]) { spaces(out, ind); pb_adds(out, b->lines[k]); }
                pb_addc(out, '\n');
            }
            break;
        }
        case BK_ITEM: {
            inline_parse(b->text, &chars, NULL);
            if (prev != BK_HEADING) pb_addc(out, '\n');
            int tw = (int)chars.n;
            Block *next = bi + 1 < pg->nb ? &pg->b[bi + 1] : NULL;
            int inline_desc = next && next->kind == BK_PARA && next->in_item && tw <= 6;
            /* the term, with its markup */
            spaces(out, BODY);
            unsigned char cur = 0;
            for (size_t k = 0; k < chars.n; k++) {
                set_style(out, &cur, chars.c[k].st, ansi);
                put_cp(out, chars.c[k].cp);
            }
            set_style(out, &cur, 0, ansi);
            if (inline_desc) {
                spaces(out, BODY + 7 - BODY - tw);
                ChBuf d = { 0 };
                inline_parse(next->text, &d, NULL);
                emit_wrapped(out, d.c, d.n, BODY + 7, BODY + 7, width, ansi, 1);
                free(d.c);
                bi++;
                prev = BK_PARA;
                free(chars.c);
                continue;
            }
            pb_addc(out, '\n');
            break;
        }
        }
        free(chars.c);
        prev = b->kind;
    }
    pb_addc(out, '\n');
    centered_line(out, "TouhouOS", "", head, width, ansi);
}

/* ------------------------------------------------------------------ checker */

static int report(FILE *f, const char *path, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int report(FILE *f, const char *path, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%s:%d: ", path, line);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    return 1;
}

static int has_heading(Page *pg, const char *title) {
    for (int i = 0; i < pg->nb; i++) if (pg->b[i].kind == BK_HEADING && !strcmp(pg->b[i].text, title)) return 1;
    return 0;
}

/* Looks for name(N) cross references in a block of text and verifies that each page exists. */
static int check_refs(const char *text, int line, const char *path, Index *ix, FILE *rep) {
    int problems = 0;
    for (const char *p = text; *p; p++) {
        if (*p != '(' || p == text || p[1] < '1' || p[1] > '9' || p[2] != ')') continue;
        const char *e = p;
        while (e > text && (isalnum((unsigned char)e[-1]) || strchr("_.+-[:", e[-1]))) e--;
        if (e == p) continue;
        char name[128];
        snprintf(name, sizeof name, "%.*s", (int)(p - e), e);
        char sec[2] = { p[1], 0 };
        if (!index_find(ix, sec, name)) problems += report(rep, path, line, "unknown reference %s(%s)", name, sec);
    }
    return problems;
}

/* Returns the number of problems found. */
int pw_check(Page *pg, const char *path, const char *dir_section, const char *file_base, Index *ix, FILE *rep) {
    int problems = 0;
    for (int i = 0; i < pg->nerr; i++) problems += report(rep, path, pg->err[i].line, "%s", pg->err[i].msg);
    if (!pg->name) return problems + report(rep, path, 1, "no .page header line");
    if (strcmp(pg->name, file_base)) problems += report(rep, path, pg->header_line, "the page says it is '%s' but the file is called '%s.pw'", pg->name, file_base);
    if (dir_section && strcmp(pg->sec, dir_section)) problems += report(rep, path, pg->header_line, "the page says section %s but it lives in directory %s", pg->sec, dir_section);
    if (strlen(pg->sec) != 1 || pg->sec[0] < '1' || pg->sec[0] > '9') problems += report(rep, path, pg->header_line, "bad section '%s'", pg->sec);
    if (!pg->desc[0]) problems += report(rep, path, pg->header_line, "the description is empty");
    else if (strlen(pg->desc) > 72) problems += report(rep, path, pg->header_line, "the description is longer than 72 characters");
    int cmd = pg->sec[0] == '1' || pg->sec[0] == '8';
    if (cmd && !has_heading(pg, "SYNOPSIS")) problems += report(rep, path, 1, "missing SYNOPSIS section");
    if (!has_heading(pg, "DESCRIPTION")) problems += report(rep, path, 1, "missing DESCRIPTION section");
    char section[80] = "";
    for (int i = 0; i < pg->nb; i++) {
        Block *b = &pg->b[i];
        if (b->kind == BK_HEADING) {
            snprintf(section, sizeof section, "%s", b->text);
            if (i + 1 >= pg->nb || pg->b[i + 1].kind == BK_HEADING) problems += report(rep, path, b->line, "section %s is empty", b->text);
        }
        if (b->kind == BK_ITEM) {
            int has = 0;
            for (int k = i + 1; k < pg->nb && pg->b[k].in_item; k++) has = 1;
            if (!has) problems += report(rep, path, b->line, "item '%s' has no description", b->text);
        }
        if (b->text && b->kind != BK_HEADING && b->kind != BK_SUBHEADING) {
            ChBuf tmp = { 0 };
            int tb = 0;
            int bad = inline_parse(b->text, &tmp, &tb);
            free(tmp.c);
            if (bad) problems += report(rep, path, b->line, "unmatched ` in the text");
            if (tb) problems += report(rep, path, b->line, "a backslash ends the line");
            if (!strcmp(section, "SEE ALSO")) problems += check_refs(b->text, b->line, path, ix, rep);
        }
    }
    return problems;
}
