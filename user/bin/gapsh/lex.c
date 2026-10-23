/* Tokenizer: words with quote markers, operators, redirections, heredocs. */
#include "gapsh.h"

char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "gapsh: out of memory (Yuyuko is hungry)\n"); _exit(2); }
    return p;
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "gapsh: out of memory (Yuyuko is hungry)\n"); _exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "gapsh: out of memory (Yuyuko is hungry)\n"); _exit(2); }
    return p;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, 1, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

void sv_push(StrVec *sv, char *s) {
    if (sv->n + 1 >= sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = xrealloc(sv->v, (size_t)sv->cap * sizeof(char *));
    }
    sv->v[sv->n++] = s;
    sv->v[sv->n] = NULL;
}

typedef struct { char *s; size_t n, cap; } Buf;

static void buf_putc(Buf *b, char c) {
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->s = xrealloc(b->s, b->cap); }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}

static void buf_puts(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) buf_putc(b, s[i]);
}

void lex_init(Lexer *lx, const char *src) {
    memset(lx, 0, sizeof *lx);
    lx->src = src;
}

static int is_meta(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>';
}

/* Copies a balanced region starting at src[*pos] (which holds the opening char) verbatim into b; returns 0 if unterminated. */
static int copy_balanced(Lexer *lx, Buf *b, char open, char close) {
    int depth = 0;
    const char *s = lx->src;
    while (s[lx->pos]) {
        char c = s[lx->pos];
        if (c == '\\' && s[lx->pos + 1]) { buf_putc(b, c); buf_putc(b, s[lx->pos + 1]); lx->pos += 2; continue; }
        if (c == '\'' && open != '\'') {                         /* quoted text inside $( ... ) */
            buf_putc(b, c); lx->pos++;
            while (s[lx->pos] && s[lx->pos] != '\'') buf_putc(b, s[lx->pos++]);
            if (!s[lx->pos]) return 0;
            buf_putc(b, s[lx->pos++]);
            continue;
        }
        if (c == '"' && open != '"') {
            buf_putc(b, c); lx->pos++;
            while (s[lx->pos] && s[lx->pos] != '"') {
                if (s[lx->pos] == '\\' && s[lx->pos + 1]) buf_putc(b, s[lx->pos++]);
                buf_putc(b, s[lx->pos++]);
            }
            if (!s[lx->pos]) return 0;
            buf_putc(b, s[lx->pos++]);
            continue;
        }
        if (c == open) depth++;
        else if (c == close) {
            depth--;
            buf_putc(b, c);
            lx->pos++;
            if (depth == 0) return 1;
            continue;
        }
        buf_putc(b, c);
        lx->pos++;
    }
    return 0;
}

static void read_heredocs(Lexer *lx) {
    for (int i = 0; i < lx->npending; i++) {
        Buf body = { 0 };
        buf_putc(&body, 0); body.n = 0; body.s[0] = 0;
        const char *s = lx->src;
        int found = 0;
        while (s[lx->pos]) {
            size_t ls = lx->pos;
            while (s[lx->pos] && s[lx->pos] != '\n') lx->pos++;
            size_t len = lx->pos - ls;
            if (s[lx->pos] == '\n') lx->pos++;
            if (len == strlen(lx->heredoc_delims[i]) && !strncmp(s + ls, lx->heredoc_delims[i], len)) { found = 1; break; }
            buf_puts(&body, s + ls, len);
            buf_putc(&body, '\n');
        }
        if (!found) lx->incomplete = 1;
        lx->pending_heredocs[i]->target = body.s ? body.s : xstrdup("");
    }
    lx->npending = 0;
}

static Token lex_token(Lexer *lx) {
    Token t = { T_EOF, NULL, -1, R_IN };
    const char *s = lx->src;

    for (;;) {                                   /* skip blanks, comments, continuations */
        while (s[lx->pos] == ' ' || s[lx->pos] == '\t') lx->pos++;
        if (s[lx->pos] == '\\' && s[lx->pos + 1] == '\n') { lx->pos += 2; continue; }
        if (s[lx->pos] == '#') { while (s[lx->pos] && s[lx->pos] != '\n') lx->pos++; continue; }
        break;
    }
    char c = s[lx->pos];
    if (!c) return t;
    if (c == '\n') {
        lx->pos++;
        t.type = T_NEWLINE;
        if (lx->npending) read_heredocs(lx);
        return t;
    }
    switch (c) {
    case ';': lx->pos++; if (s[lx->pos] == ';') { lx->pos++; t.type = T_DSEMI; } else t.type = T_SEMI; return t;
    case '(': lx->pos++; t.type = T_LPAREN; return t;
    case ')': lx->pos++; t.type = T_RPAREN; return t;
    case '|': lx->pos++; if (s[lx->pos] == '|') { lx->pos++; t.type = T_OR_IF; } else t.type = T_PIPE; return t;
    case '&':
        lx->pos++;
        if (s[lx->pos] == '&') { lx->pos++; t.type = T_AND_IF; return t; }
        if (s[lx->pos] == '>') {
            lx->pos++;
            t.type = T_REDIR;
            t.rkind = R_OUTERR;
            if (s[lx->pos] == '>') { lx->pos++; t.rkind = R_OUTERR_APPEND; }
            return t;
        }
        t.type = T_AMP;
        return t;
    default: break;
    }

    /* A word, possibly the fd number of a redirection. */
    Buf w = { 0 };
    buf_putc(&w, 0); w.n = 0; w.s[0] = 0;
    int all_digits = 1;
    while (s[lx->pos] && !is_meta((unsigned char)s[lx->pos])) {
        c = s[lx->pos];
        if (c == '\'') {
            all_digits = 0;
            lx->pos++;
            buf_putc(&w, Q_SQ_BEGIN);
            while (s[lx->pos] && s[lx->pos] != '\'') {
                if ((unsigned char)s[lx->pos] < 6) { lx->pos++; continue; }
                buf_putc(&w, s[lx->pos++]);
            }
            if (!s[lx->pos]) { lx->incomplete = 1; break; }
            lx->pos++;
            buf_putc(&w, Q_SQ_END);
        } else if (c == '"') {
            all_digits = 0;
            lx->pos++;
            buf_putc(&w, Q_DQ_BEGIN);
            while (s[lx->pos] && s[lx->pos] != '"') {
                if (s[lx->pos] == '\\' && s[lx->pos + 1]) {
                    char n = s[lx->pos + 1];
                    if (n == '$' || n == '"' || n == '\\' || n == '`') { buf_putc(&w, Q_ESC); buf_putc(&w, n); lx->pos += 2; continue; }
                    if (n == '\n') { lx->pos += 2; continue; }
                    buf_putc(&w, '\\');
                    lx->pos++;
                    continue;
                }
                if (s[lx->pos] == '$' && (s[lx->pos + 1] == '(' || s[lx->pos + 1] == '{')) {
                    char open = s[lx->pos + 1], close = open == '(' ? ')' : '}';
                    buf_putc(&w, '$');
                    lx->pos++;
                    if (!copy_balanced(lx, &w, open, close)) { lx->incomplete = 1; break; }
                    continue;
                }
                if (s[lx->pos] == '`') {
                    buf_putc(&w, '`');
                    lx->pos++;
                    while (s[lx->pos] && s[lx->pos] != '`') { if (s[lx->pos] == '\\' && s[lx->pos + 1]) buf_putc(&w, s[lx->pos++]); buf_putc(&w, s[lx->pos++]); }
                    if (!s[lx->pos]) { lx->incomplete = 1; break; }
                    buf_putc(&w, s[lx->pos++]);
                    continue;
                }
                if ((unsigned char)s[lx->pos] < 6) { lx->pos++; continue; }
                buf_putc(&w, s[lx->pos++]);
            }
            if (lx->incomplete) break;
            if (!s[lx->pos]) { lx->incomplete = 1; break; }
            lx->pos++;
            buf_putc(&w, Q_DQ_END);
        } else if (c == '\\') {
            all_digits = 0;
            if (!s[lx->pos + 1]) { lx->pos++; break; }
            if (s[lx->pos + 1] == '\n') { lx->pos += 2; continue; }
            buf_putc(&w, Q_ESC);
            buf_putc(&w, s[lx->pos + 1]);
            lx->pos += 2;
        } else if (c == '$' && (s[lx->pos + 1] == '(' || s[lx->pos + 1] == '{')) {
            all_digits = 0;
            char open = s[lx->pos + 1], close = open == '(' ? ')' : '}';
            buf_putc(&w, '$');
            lx->pos++;
            if (!copy_balanced(lx, &w, open, close)) { lx->incomplete = 1; break; }
        } else if (c == '`') {
            all_digits = 0;
            buf_putc(&w, '`');
            lx->pos++;
            while (s[lx->pos] && s[lx->pos] != '`') { if (s[lx->pos] == '\\' && s[lx->pos + 1]) buf_putc(&w, s[lx->pos++]); buf_putc(&w, s[lx->pos++]); }
            if (!s[lx->pos]) { lx->incomplete = 1; break; }
            buf_putc(&w, s[lx->pos++]);
        } else {
            if (!isdigit((unsigned char)c)) all_digits = 0;
            if ((unsigned char)c < 6) { lx->pos++; continue; }
            buf_putc(&w, c);
            lx->pos++;
        }
    }

    if ((s[lx->pos] == '<' || s[lx->pos] == '>') && all_digits && w.n) {
        t.fd = atoi(w.s);
        free(w.s);
        c = s[lx->pos];
    } else {
        t.type = T_WORD;
        t.text = w.s ? w.s : xstrdup("");
        return t;
    }

    /* redirection operator */
    c = s[lx->pos];
    t.type = T_REDIR;
    lx->pos++;
    if (c == '<') {
        if (s[lx->pos] == '<') { lx->pos++; t.rkind = R_HEREDOC; }
        else if (s[lx->pos] == '&') { lx->pos++; t.rkind = R_DUPIN; }
        else t.rkind = R_IN;
    } else {
        if (s[lx->pos] == '>') { lx->pos++; t.rkind = R_APPEND; }
        else if (s[lx->pos] == '&') { lx->pos++; t.rkind = R_DUPOUT; }
        else t.rkind = R_OUT;
    }
    return t;
}

Token *lex_peek(Lexer *lx) {
    if (!lx->have_tok) {
        const char *s = lx->src;
        /* redirections without a leading fd digit: the word branch above never sees them */
        size_t save = lx->pos;
        while (s[lx->pos] == ' ' || s[lx->pos] == '\t') lx->pos++;
        char c = s[lx->pos];
        if (c == '<' || c == '>') {
            Token t = { T_REDIR, NULL, -1, R_IN };
            lx->pos++;
            if (c == '<') {
                if (s[lx->pos] == '<') { lx->pos++; t.rkind = R_HEREDOC; }
                else if (s[lx->pos] == '&') { lx->pos++; t.rkind = R_DUPIN; }
                else t.rkind = R_IN;
            } else {
                if (s[lx->pos] == '>') { lx->pos++; t.rkind = R_APPEND; }
                else if (s[lx->pos] == '&') { lx->pos++; t.rkind = R_DUPOUT; }
                else t.rkind = R_OUT;
            }
            lx->tok = t;
        } else {
            lx->pos = save;
            lx->tok = lex_token(lx);
        }
        lx->have_tok = 1;
    }
    return &lx->tok;
}

void lex_next(Lexer *lx) {
    lex_peek(lx);
    lx->have_tok = 0;
}
