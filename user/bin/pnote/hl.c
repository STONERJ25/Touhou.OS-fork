/* A modest syntax highlighter: C, shell and markdown. It colours one line at a time; the only state carried from one
 * line to the next is "inside a block comment" (C) and "inside a code fence" (markdown). Line start states are cached
 * in Line.hl and recomputed lazily after edits. */
#include "pnote.h"

enum { ST_NONE = 0, ST_KEYWORD, ST_TYPE, ST_STRING, ST_COMMENT, ST_NUMBER, ST_PREPROC, ST_VAR, ST_HEADING, ST_BOLD,
       ST_EMPH, ST_CODE, ST_LINK, ST_QUOTE, ST_BULLET, ST_COUNT };

const char *hl_style_sgr(int style) {
    static const char *const sgr[ST_COUNT] = {
        "", "1;33", "1;32", "32", "36", "35", "1;34", "1;36", "1;31", "1", "1;35", "32", "36", "35", "1;33",
    };
    return style >= 0 && style < ST_COUNT ? sgr[style] : "";
}

const char *hl_lang_name(int lang) {
    switch (lang) {
    case LANG_C: return "C";
    case LANG_SH: return "shell";
    case LANG_MD: return "markdown";
    default: return "plain";
    }
}

static int ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

int hl_detect(const char *path, const char *first_line, size_t n) {
    if (path) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (ends_with(base, ".c") || ends_with(base, ".h") || ends_with(base, ".cc") || ends_with(base, ".cpp") ||
            ends_with(base, ".hpp") || ends_with(base, ".cxx")) return LANG_C;
        if (ends_with(base, ".sh") || ends_with(base, ".bash") || ends_with(base, ".gap") || !strcmp(base, "profile") ||
            !strcmp(base, ".profile") || !strcmp(base, ".gaprc") || !strcmp(base, ".bashrc") || !strcmp(base, ".gapshrc")) return LANG_SH;
        if (ends_with(base, ".md") || ends_with(base, ".markdown")) return LANG_MD;
    }
    if (first_line && n > 2 && first_line[0] == '#' && first_line[1] == '!') {
        if (memmem(first_line, n, "sh", 2) || memmem(first_line, n, "gapsh", 5)) return LANG_SH;
    }
    return LANG_NONE;
}

static void paint(unsigned char *st, size_t from, size_t to, int style) {
    for (size_t i = from; i < to; i++) st[i] = (unsigned char)style;
}

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident(int c) { return isalnum(c) || c == '_'; }

static int word_in(const char *w, size_t n, const char *const *list) {
    for (; *list; list++) if (strlen(*list) == n && !memcmp(*list, w, n)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ C */

static const char *const c_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto", "sizeof",
    "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline", "register", "restrict",
    "class", "namespace", "template", "public", "private", "protected", "new", "delete", "this", "using", "typename", NULL,
};
static const char *const c_types[] = {
    "int", "char", "short", "long", "float", "double", "void", "unsigned", "signed", "bool", "_Bool", "size_t", "ssize_t",
    "FILE", "NULL", "true", "false", "auto", "wchar_t", NULL,
};

static int hl_c(const char *s, size_t n, int state, unsigned char *st) {
    size_t i = 0;
    if (state == 1) {
        size_t j = 0;
        while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
        if (j + 1 >= n) { paint(st, 0, n, ST_COMMENT); return 1; }
        paint(st, 0, j + 2, ST_COMMENT);
        i = j + 2;
    }
    size_t first = i;
    while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { paint(st, i, n, ST_COMMENT); return 0; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 >= n) { paint(st, i, n, ST_COMMENT); return 1; }
            paint(st, i, j + 2, ST_COMMENT);
            i = j + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
            size_t e = j < n ? j + 1 : n;
            paint(st, i, e, ST_STRING);
            i = e;
            continue;
        }
        if (c == '#' && i == first) {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            while (j < n && is_ident(s[j])) j++;
            paint(st, i, j, ST_PREPROC);
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < n && s[k] == '<') {
                size_t e = k;
                while (e < n && s[e] != '>') e++;
                if (e < n) e++;
                paint(st, k, e, ST_STRING);
                i = e;
            } else i = j;
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            size_t j = i + 1;
            while (j < n && (is_ident(s[j]) || s[j] == '.')) j++;
            paint(st, i, j, ST_NUMBER);
            i = j;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i + 1;
            while (j < n && is_ident(s[j])) j++;
            if (word_in(s + i, j - i, c_keywords)) paint(st, i, j, ST_KEYWORD);
            else if (word_in(s + i, j - i, c_types) || (j - i > 2 && s[j - 2] == '_' && s[j - 1] == 't')) paint(st, i, j, ST_TYPE);
            i = j;
            continue;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------ shell */

static const char *const sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in", "function", "select",
    "return", "exit", "break", "continue", "time", NULL,
};
static const char *const sh_builtins[] = {
    "cd", "echo", "printf", "export", "local", "readonly", "shift", "set", "unset", "alias", "unalias", "source", "eval",
    "exec", "read", "test", "trap", "wait", "kill", "pwd", "type", "command", "umask", "getopts", "true", "false", NULL,
};

static int hl_sh(const char *s, size_t n, int state, unsigned char *st) {
    (void)state;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '#' && (i == 0 || strchr(" \t;|&(", s[i - 1]))) { paint(st, i, n, ST_COMMENT); return 0; }
        if (c == '\'') {
            size_t j = i + 1;
            while (j < n && s[j] != '\'') j++;
            size_t e = j < n ? j + 1 : n;
            paint(st, i, e, ST_STRING);
            i = e;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            paint(st, i, i + 1, ST_STRING);
            while (j < n && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < n) { paint(st, j, j + 2, ST_STRING); j += 2; continue; }
                if (s[j] == '$' && j + 1 < n) {
                    size_t k = j + 1;
                    if (s[k] == '{') { while (k < n && s[k] != '}') k++; if (k < n) k++; }
                    else if (s[k] == '(') k++;
                    else if (is_ident_start((unsigned char)s[k])) { while (k < n && is_ident(s[k])) k++; }
                    else k++;
                    paint(st, j, k, ST_VAR);
                    j = k;
                    continue;
                }
                paint(st, j, j + 1, ST_STRING);
                j++;
            }
            if (j < n) paint(st, j, j + 1, ST_STRING);
            i = j < n ? j + 1 : n;
            continue;
        }
        if (c == '$' && i + 1 < n) {
            size_t k = i + 1;
            if (s[k] == '{') { while (k < n && s[k] != '}') k++; if (k < n) k++; }
            else if (s[k] == '(') { k++; if (k < n && s[k] == '(') k++; }
            else if (is_ident_start((unsigned char)s[k])) { while (k < n && is_ident(s[k])) k++; }
            else if (isdigit((unsigned char)s[k]) || strchr("@*#?$!-", s[k])) k++;
            else k = i + 1;
            paint(st, i, k, ST_VAR);
            i = k;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i + 1;
            while (j < n && (is_ident(s[j]) || s[j] == '-')) j++;
            int at_word_start = i == 0 || strchr(" \t;|&(", s[i - 1]) != NULL;
            if (at_word_start && j < n && s[j] == '=') paint(st, i, j, ST_TYPE);          /* NAME=value */
            else if (at_word_start && word_in(s + i, j - i, sh_keywords)) paint(st, i, j, ST_KEYWORD);
            else if (at_word_start && word_in(s + i, j - i, sh_builtins)) paint(st, i, j, ST_TYPE);
            i = j;
            continue;
        }
        if (isdigit(c)) {
            size_t j = i + 1;
            while (j < n && isdigit((unsigned char)s[j])) j++;
            if (i == 0 || strchr(" \t=", s[i - 1])) paint(st, i, j, ST_NUMBER);
            i = j;
            continue;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------ markdown */

static void md_inline(const char *s, size_t n, size_t from, unsigned char *st) {
    size_t i = from;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n) { i += 2; continue; }
        if (c == '`') {
            size_t j = i + 1;
            while (j < n && s[j] != '`') j++;
            if (j < n) { paint(st, i, j + 1, ST_CODE); i = j + 1; continue; }
        }
        if (c == '*' || c == '_') {
            int dbl = i + 1 < n && s[i + 1] == c;
            size_t open = dbl ? 2 : 1, j = i + open;
            if (j < n && s[j] != ' ') {
                while (j < n && !(s[j] == c && (!dbl || (j + 1 < n && s[j + 1] == c)))) j++;
                if (j < n) {
                    size_t e = j + open;
                    paint(st, i, e, dbl ? ST_BOLD : ST_EMPH);
                    i = e;
                    continue;
                }
            }
        }
        if (c == '[') {
            size_t j = i + 1;
            while (j < n && s[j] != ']') j++;
            if (j + 1 < n && s[j + 1] == '(') {
                size_t e = j + 2;
                while (e < n && s[e] != ')') e++;
                if (e < n) { paint(st, i, e + 1, ST_LINK); i = e + 1; continue; }
            }
        }
        i++;
    }
}

static int hl_md(const char *s, size_t n, int state, unsigned char *st) {
    size_t first = 0;
    while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;
    int fence = n - first >= 3 && (!memcmp(s + first, "```", 3) || !memcmp(s + first, "~~~", 3));
    if (fence) { paint(st, 0, n, ST_CODE); return !state; }
    if (state) { paint(st, 0, n, ST_CODE); return 1; }
    if (first < n && s[first] == '#') { paint(st, 0, n, ST_HEADING); return 0; }
    if (first < n && s[first] == '>') { paint(st, 0, n, ST_QUOTE); return 0; }
    size_t body = first;
    if (first < n && (s[first] == '-' || s[first] == '*' || s[first] == '+') && first + 1 < n && s[first + 1] == ' ') {
        paint(st, first, first + 1, ST_BULLET);
        body = first + 2;
    } else if (first < n && isdigit((unsigned char)s[first])) {
        size_t j = first;
        while (j < n && isdigit((unsigned char)s[j])) j++;
        if (j + 1 < n && s[j] == '.' && s[j + 1] == ' ') { paint(st, first, j + 1, ST_BULLET); body = j + 2; }
    } else if (n - first >= 3) {
        int hr = 1;
        for (size_t j = first; j < n; j++) if (s[j] != '-' && s[j] != '*' && s[j] != '_' && s[j] != ' ') hr = 0;
        if (hr && (s[first] == '-' || s[first] == '*' || s[first] == '_')) { paint(st, first, n, ST_BULLET); return 0; }
    }
    md_inline(s, n, body, st);
    return 0;
}

/* ------------------------------------------------------------------ interface */

int hl_line(int lang, const char *s, size_t n, int state_in, unsigned char *styles) {
    memset(styles, 0, n);
    switch (lang) {
    case LANG_C: return hl_c(s, n, state_in, styles);
    case LANG_SH: return hl_sh(s, n, state_in, styles);
    case LANG_MD: return hl_md(s, n, state_in, styles);
    default: return 0;
    }
}

void hl_invalidate(long from_line) {
    if (from_line < 0) from_line = 0;
    if (E.hl_valid > from_line) E.hl_valid = from_line;
}

/* Makes the cached start state valid for every line up to `upto`. */
void hl_prepare(long upto) {
    if (!E.highlight || E.lang == LANG_NONE) return;
    if (upto >= E.n) upto = E.n - 1;
    if (E.hl_valid >= upto) return;
    if (upto - E.hl_valid > 40000) {                    /* too far away to be worth scanning: guess "normal" */
        LN(upto)->hl = 0;
        E.hl_valid = upto;
        return;
    }
    static unsigned char tmp[2048];
    for (long i = E.hl_valid; i < upto; i++) {
        Line *L = LN(i);
        int st = L->len <= sizeof tmp ? hl_line(E.lang, L->s, L->len, L->hl, tmp) : (int)L->hl;
        LN(i + 1)->hl = (unsigned)st & 15u;
    }
    E.hl_valid = upto;
}
