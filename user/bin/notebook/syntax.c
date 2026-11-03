/* Light syntax highlighting for C, shell scripts and Markdown. Each scanner looks at one line at a time; the only thing that
 * crosses a line break is a small state number (inside a block comment, inside a fenced code block). */
#include "notebook.h"
#include <string.h>

static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static void mark(uint8_t *cls, size_t a, size_t b, int c) {
    if (!cls || b <= a) return;
    for (size_t i = a; i < b; i++) cls[i] = (uint8_t)c;
}

static bool word_in(const char *const *list, const char *w, size_t n) {
    for (; *list; list++) {
        const char *x = *list;
        if (x[0] == w[0] && strlen(x) == n && !memcmp(x, w, n)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- C */

static const char *const c_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "restrict", "return", "sizeof", "static", "struct", "switch", "typedef", "union", "volatile", "while",
    "NULL", "true", "false", "nullptr", "class", "public", "private", "protected", "namespace", "template", "typename", "new",
    "delete", "this", "using", "virtual", "operator", "try", "catch", "throw", "constexpr", "static_assert", "alignas", "alignof", NULL
};
static const char *const c_types[] = {
    "int", "char", "short", "long", "float", "double", "void", "unsigned", "signed", "bool", "size_t", "ssize_t", "ptrdiff_t",
    "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE",
    "DIR", "off_t", "pid_t", "time_t", "mode_t", "uid_t", "gid_t", "wchar_t", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
    "_Bool", NULL
};

static SynState scan_c(const char *s, size_t n, SynState st, uint8_t *cls) {
    size_t i = 0;
    bool pre = false, include = false, pre_word = false;
    if (st == 0) {
        size_t j = 0;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        pre = j < n && s[j] == '#';
        pre_word = pre;
    }
    while (i < n) {
        char c = s[i];
        if (st == 1) {                                              /* inside a block comment */
            size_t j = i;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            if (j + 1 < n) { mark(cls, i, j + 2, SC_COMMENT); i = j + 2; st = 0; }
            else { mark(cls, i, n, SC_COMMENT); i = n; }
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { mark(cls, i, n, SC_COMMENT); return st; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { st = 1; mark(cls, i, i + 2, SC_COMMENT); i += 2; continue; }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
            if (j < n) j++;
            mark(cls, i, j, SC_STRING);
            i = j;
            continue;
        }
        if (pre && c == '#') { mark(cls, i, i + 1, SC_PREPROC); i++; continue; }
        if (include && c == '<') {
            size_t j = i + 1;
            while (j < n && s[j] != '>') j++;
            if (j < n) j++;
            mark(cls, i, j, SC_STRING);
            i = j;
            include = false;
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]))) {
            size_t j = i + 1;
            while (j < n && (is_alnum(s[j]) || s[j] == '.')) j++;
            mark(cls, i, j, SC_NUMBER);
            i = j;
            continue;
        }
        if (is_alpha(c)) {
            size_t j = i + 1;
            while (j < n && is_alnum(s[j])) j++;
            int k = SC_NORMAL;
            if (pre_word) { k = SC_PREPROC; pre_word = false; include = (j - i == 7 && !memcmp(s + i, "include", 7)); }
            else if (word_in(c_keywords, s + i, j - i)) k = SC_KEYWORD;
            else if (word_in(c_types, s + i, j - i)) k = SC_TYPE;
            if (k) mark(cls, i, j, k);
            i = j;
            continue;
        }
        i++;
    }
    return st;
}

/* ---------------------------------------------------------------- shell */

static const char *const sh_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in", "function", "select", "time",
    "return", "exit", "break", "continue", NULL
};
static const char *const sh_builtins[] = {
    "echo", "cd", "export", "set", "unset", "read", "test", "source", "alias", "unalias", "eval", "exec", "shift", "trap", "wait",
    "local", "readonly", "printf", "pwd", "true", "false", "kill", "jobs", "fg", "bg", "umask", "type", "command", "declare", NULL
};

static SynState scan_sh(const char *s, size_t n, SynState st, uint8_t *cls) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == ';' || s[i - 1] == '(')) {
            mark(cls, i, n, (i == 0 && n > 1 && s[1] == '!') ? SC_PREPROC : SC_COMMENT);
            return st;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && s[j] != c) { if (c == '"' && s[j] == '\\' && j + 1 < n) j++; j++; }
            if (j < n) j++;
            mark(cls, i, j, SC_STRING);
            i = j;
            continue;
        }
        if (c == '$') {
            size_t j = i + 1;
            if (j < n && s[j] == '{') { while (j < n && s[j] != '}') j++; if (j < n) j++; }
            else if (j < n && s[j] == '(') j++;
            else if (j < n && is_alpha(s[j])) { while (j < n && is_alnum(s[j])) j++; }
            else if (j < n) j++;                                    /* $1 $? $@ ... */
            mark(cls, i, j, SC_TYPE);
            i = j;
            continue;
        }
        if (is_alpha(c)) {
            size_t j = i + 1;
            while (j < n && is_alnum(s[j])) j++;
            int k = SC_NORMAL;
            if (word_in(sh_keywords, s + i, j - i)) k = SC_KEYWORD;
            else if (word_in(sh_builtins, s + i, j - i)) k = SC_PREPROC;
            else if (j < n && s[j] == '=' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) k = SC_TYPE;    /* VAR=value */
            if (k) mark(cls, i, j, k);
            i = j;
            continue;
        }
        if (is_digit(c)) {
            size_t j = i + 1;
            while (j < n && is_digit(s[j])) j++;
            if (j >= n || !is_alpha(s[j])) mark(cls, i, j, SC_NUMBER);
            i = j;
            continue;
        }
        i++;
    }
    return st;
}

/* ---------------------------------------------------------------- Markdown */

static bool md_rule(const char *s, size_t n) {
    char ch = 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ') continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_') return false;
        if (ch && s[i] != ch) return false;
        ch = s[i];
        count++;
    }
    return count >= 3;
}

static SynState scan_md(const char *s, size_t n, SynState st, uint8_t *cls) {
    size_t j = 0;
    while (j < n && s[j] == ' ') j++;
    bool fence = n - j >= 3 && ((s[j] == '`' && s[j + 1] == '`' && s[j + 2] == '`') || (s[j] == '~' && s[j + 1] == '~' && s[j + 2] == '~'));
    if (st == 1) { mark(cls, 0, n, SC_CODE); return fence ? 0 : 1; }
    if (fence) { mark(cls, 0, n, SC_CODE); return 1; }
    if (n > 0 && s[0] == '#') {
        size_t k = 0;
        while (k < n && s[k] == '#') k++;
        if (k <= 6 && (k == n || s[k] == ' ')) { mark(cls, 0, n, SC_HEADING); return 0; }
    }
    if (j < n && s[j] == '>') { mark(cls, j, n, SC_COMMENT); return 0; }
    if (md_rule(s, n)) { mark(cls, 0, n, SC_MARKER); return 0; }
    size_t p = j;
    if (p + 1 < n && (s[p] == '-' || s[p] == '*' || s[p] == '+') && s[p + 1] == ' ') { mark(cls, p, p + 1, SC_MARKER); p += 2; }
    else {
        size_t q = p;
        while (q < n && is_digit(s[q])) q++;
        if (q > p && q + 1 < n && (s[q] == '.' || s[q] == ')') && s[q + 1] == ' ') { mark(cls, p, q + 1, SC_MARKER); p = q + 2; }
    }
    size_t i = p;
    while (i < n) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '`') {
            size_t k = i + 1;
            while (k < n && s[k] != '`') k++;
            if (k < n) { mark(cls, i, k + 1, SC_CODE); i = k + 1; continue; }
        } else if (c == '*' || c == '_') {
            size_t run = (i + 1 < n && s[i + 1] == c) ? 2 : 1;
            size_t k = i + run;
            if (k < n && s[k] != ' ') {
                while (k < n && !(s[k] == c && (run == 1 || (k + 1 < n && s[k + 1] == c)))) k++;
                if (k < n) { mark(cls, i, k + run, SC_EMPHASIS); i = k + run; continue; }
            }
        } else if (c == '[') {
            size_t k = i + 1;
            while (k < n && s[k] != ']') k++;
            if (k + 1 < n && s[k + 1] == '(') {
                size_t m = k + 2;
                while (m < n && s[m] != ')') m++;
                if (m < n) { mark(cls, i, m + 1, SC_LINK); i = m + 1; continue; }
            }
        }
        i++;
    }
    return 0;
}

/* ---------------------------------------------------------------- interface */

SynState syn_scan(Lang lang, const char *s, size_t len, SynState st, uint8_t *cls) {
    if (cls && len) memset(cls, SC_NORMAL, len);                /* the scanners only mark what they recognise */
    switch (lang) {
    case LANG_C: return scan_c(s, len, st, cls);
    case LANG_SHELL: return scan_sh(s, len, st, cls);
    case LANG_MARKDOWN: return scan_md(s, len, st, cls);
    default: return 0;
    }
}

const char *syn_name(Lang l) {
    switch (l) {
    case LANG_C: return "C";
    case LANG_SHELL: return "Shell";
    case LANG_MARKDOWN: return "Markdown";
    default: return "Plain Text";
    }
}

static bool ends_with(const char *s, const char *ext) {
    size_t a = strlen(s), b = strlen(ext);
    if (b > a) return false;
    return strcasecmp(s + a - b, ext) == 0;
}

Lang syn_detect(const char *path, const char *first_line, size_t len) {
    if (path && *path) {
        static const char *const c_ext[] = { ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".inc", NULL };
        static const char *const sh_ext[] = { ".sh", ".bash", ".zsh", ".ksh", NULL };
        static const char *const md_ext[] = { ".md", ".markdown", ".mkd", NULL };
        for (int i = 0; c_ext[i]; i++) if (ends_with(path, c_ext[i])) return LANG_C;
        for (int i = 0; sh_ext[i]; i++) if (ends_with(path, sh_ext[i])) return LANG_SHELL;
        for (int i = 0; md_ext[i]; i++) if (ends_with(path, md_ext[i])) return LANG_MARKDOWN;
    }
    if (first_line && len > 2 && first_line[0] == '#' && first_line[1] == '!') {
        for (size_t i = 2; i + 1 < len; i++)
            if (first_line[i] == 's' && first_line[i + 1] == 'h') return LANG_SHELL;
    }
    return LANG_PLAIN;
}

uint32_t syn_color(int cls, const KTheme *th) {
    switch (cls) {
    case SC_COMMENT: return 0x667A55;
    case SC_STRING: return 0x1F7A4D;
    case SC_NUMBER: return 0xB4570A;
    case SC_KEYWORD: return 0xA3122A;
    case SC_TYPE: return 0x2A55A8;
    case SC_PREPROC: return 0x7B3FA8;
    case SC_HEADING: return 0x8E0B21;
    case SC_EMPHASIS: return 0x2A55A8;
    case SC_CODE: return 0x1F7A4D;
    case SC_LINK: return 0x1E6AC2;
    case SC_MARKER: return 0xA87A12;
    default: return th->field_text;
    }
}
