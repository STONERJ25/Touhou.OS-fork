/* Recursive-descent parser: lists, and-or chains, pipelines, simple and compound commands, functions. */
#include "gapsh.h"

static void perr(Lexer *lx, const char *what) {
    if (lx->error || lx->incomplete) return;
    lx->error = 1;
    snprintf(lx->errmsg, sizeof lx->errmsg, "syntax error near %s", what);
}

static Node *mk(NodeKind k) {
    Node *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = k;
    return n;
}

static int is_kw(Token *t, const char *kw) { return t->type == T_WORD && !strcmp(t->text, kw); }

static void skip_newlines(Lexer *lx) {
    while (lex_peek(lx)->type == T_NEWLINE) lex_next(lx);
}

static void eof_means_incomplete(Lexer *lx) {
    if (lex_peek(lx)->type == T_EOF && !lx->error) lx->incomplete = 1;
}

static int word_is_assignment(const char *w) {
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0;
    size_t i = 0;
    while (isalnum((unsigned char)w[i]) || w[i] == '_') i++;
    return w[i] == '=';
}

static char *strip_markers(const char *w, int *had_quotes) {
    char *out = xmalloc(strlen(w) + 1);
    size_t n = 0;
    *had_quotes = 0;
    for (; *w; w++) {
        if ((unsigned char)*w < 6) { *had_quotes = 1; if (*w == Q_ESC && w[1]) { out[n++] = *++w; } continue; }
        out[n++] = *w;
    }
    out[n] = 0;
    return out;
}

static int is_terminator(Lexer *lx) {
    Token *t = lex_peek(lx);
    if (t->type == T_EOF || t->type == T_RPAREN || t->type == T_DSEMI) return 1;
    if (t->type != T_WORD) return 0;
    static const char *const kws[] = { "then", "elif", "else", "fi", "do", "done", "esac", "}" };
    for (unsigned i = 0; i < sizeof kws / sizeof kws[0]; i++)
        if (!strcmp(t->text, kws[i])) return 1;
    return 0;
}

static Node *parse_and_or(Lexer *lx);
static Node *parse_command(Lexer *lx);

static Node *parse_list(Lexer *lx) {
    Node *head = NULL;
    for (;;) {
        while (lex_peek(lx)->type == T_NEWLINE || lex_peek(lx)->type == T_SEMI) lex_next(lx);
        if (is_terminator(lx)) break;
        Node *n = parse_and_or(lx);
        if (!n) return NULL;
        Token *t = lex_peek(lx);
        if (t->type == T_AMP) {
            lex_next(lx);
            Node *bg = mk(N_BG);
            bg->a = n;
            n = bg;
        } else if (t->type == T_SEMI || t->type == T_NEWLINE) {
            lex_next(lx);
        } else if (!is_terminator(lx)) {
            perr(lx, t->type == T_WORD ? t->text : "token");
            return NULL;
        }
        if (head) {
            Node *l = mk(N_LIST);
            l->a = head;
            l->b = n;
            head = l;
        } else {
            head = n;
        }
    }
    return head;
}

static Node *parse_pipeline(Lexer *lx) {
    int negate = 0;
    if (is_kw(lex_peek(lx), "!")) { lex_next(lx); negate = 1; }
    Node *stages[64];
    int ns = 0;
    stages[ns++] = parse_command(lx);
    if (!stages[0]) return NULL;
    while (lex_peek(lx)->type == T_PIPE) {
        lex_next(lx);
        skip_newlines(lx);
        Node *s = parse_command(lx);
        if (!s) { eof_means_incomplete(lx); if (!lx->incomplete) perr(lx, "|"); return NULL; }
        if (ns < 64) stages[ns++] = s;
    }
    Node *head = stages[ns - 1];                        /* a | (b | (c | d)): a = stage, b = the rest */
    for (int i = ns - 2; i >= 0; i--) {
        Node *p = mk(N_PIPE);
        p->a = stages[i];
        p->b = head;
        head = p;
    }
    if (negate) {
        Node *n = mk(N_NOT);
        n->a = head;
        return n;
    }
    return head;
}

static Node *parse_and_or(Lexer *lx) {
    Node *left = parse_pipeline(lx);
    if (!left) return NULL;
    for (;;) {
        Token *t = lex_peek(lx);
        if (t->type != T_AND_IF && t->type != T_OR_IF) break;
        NodeKind k = t->type == T_AND_IF ? N_AND : N_OR;
        lex_next(lx);
        skip_newlines(lx);
        Node *right = parse_pipeline(lx);
        if (!right) { eof_means_incomplete(lx); if (!lx->incomplete) perr(lx, "&&"); return NULL; }
        Node *n = mk(k);
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static Redir *parse_redirect(Lexer *lx) {
    Token t = *lex_peek(lx);
    lex_next(lx);
    Token *w = lex_peek(lx);
    if (w->type != T_WORD) { eof_means_incomplete(lx); perr(lx, "redirection"); return NULL; }
    Redir *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->kind = t.rkind;
    r->fd = t.fd >= 0 ? t.fd : ((t.rkind == R_IN || t.rkind == R_DUPIN || t.rkind == R_HEREDOC) ? 0 : 1);
    if (t.rkind == R_OUTERR || t.rkind == R_OUTERR_APPEND) r->fd = 1;
    if (t.rkind == R_HEREDOC) {
        int quoted;
        char *delim = strip_markers(w->text, &quoted);
        r->heredoc_quoted = quoted;
        if (lx->npending < 8) { lx->pending_heredocs[lx->npending] = r; lx->heredoc_delims[lx->npending++] = delim; }
        r->target = xstrdup("");
    } else {
        r->target = w->text;
    }
    lex_next(lx);
    return r;
}

static void append_redir(Node *n, Redir *r) {
    if (!n->redirs) { n->redirs = r; return; }
    Redir *p = n->redirs;
    while (p->next) p = p->next;
    p->next = r;
}

static void parse_trailing_redirs(Lexer *lx, Node *n) {
    while (lex_peek(lx)->type == T_REDIR) {
        Redir *r = parse_redirect(lx);
        if (!r) return;
        append_redir(n, r);
    }
}

static int expect_kw(Lexer *lx, const char *kw) {
    skip_newlines(lx);
    Token *t = lex_peek(lx);
    if (is_kw(t, kw)) { lex_next(lx); return 1; }
    eof_means_incomplete(lx);
    if (!lx->incomplete) perr(lx, t->type == T_WORD ? t->text : kw);
    return 0;
}

static Node *parse_if(Lexer *lx) {
    Node *n = mk(N_IF);
    lex_next(lx);                                       /* if / elif */
    n->a = parse_list(lx);
    if (!n->a || !expect_kw(lx, "then")) { eof_means_incomplete(lx); return NULL; }
    n->b = parse_list(lx);
    if (!n->b) { eof_means_incomplete(lx); return NULL; }
    skip_newlines(lx);
    Token *t = lex_peek(lx);
    if (is_kw(t, "elif")) {
        n->c = parse_if(lx);
        return n->c ? n : NULL;                         /* nested if consumed the final fi */
    }
    if (is_kw(t, "else")) {
        lex_next(lx);
        n->c = parse_list(lx);
        if (!n->c) { eof_means_incomplete(lx); return NULL; }
    }
    return expect_kw(lx, "fi") ? n : NULL;
}

static Node *parse_while(Lexer *lx, NodeKind kind) {
    Node *n = mk(kind);
    lex_next(lx);
    n->a = parse_list(lx);
    if (!n->a || !expect_kw(lx, "do")) { eof_means_incomplete(lx); return NULL; }
    n->b = parse_list(lx);
    if (!n->b) { eof_means_incomplete(lx); return NULL; }
    return expect_kw(lx, "done") ? n : NULL;
}

static Node *parse_for(Lexer *lx) {
    Node *n = mk(N_FOR);
    lex_next(lx);
    Token *t = lex_peek(lx);
    if (t->type != T_WORD) { eof_means_incomplete(lx); perr(lx, "for"); return NULL; }
    n->name = t->text;
    lex_next(lx);
    n->nwords = -1;                                     /* no "in": iterate over "$@" */
    skip_newlines(lx);
    if (is_kw(lex_peek(lx), "in")) {
        lex_next(lx);
        StrVec sv = { 0 };
        while (lex_peek(lx)->type == T_WORD) { sv_push(&sv, lex_peek(lx)->text); lex_next(lx); }
        n->words = sv.v;
        n->nwords = sv.n;
    }
    t = lex_peek(lx);
    if (t->type == T_SEMI || t->type == T_NEWLINE) lex_next(lx);
    if (!expect_kw(lx, "do")) return NULL;
    n->a = parse_list(lx);
    if (!n->a) { eof_means_incomplete(lx); return NULL; }
    return expect_kw(lx, "done") ? n : NULL;
}

static Node *parse_case(Lexer *lx) {
    Node *n = mk(N_CASE);
    lex_next(lx);
    Token *t = lex_peek(lx);
    if (t->type != T_WORD) { eof_means_incomplete(lx); perr(lx, "case"); return NULL; }
    n->words = xmalloc(sizeof(char *));
    n->words[0] = t->text;
    n->nwords = 1;
    lex_next(lx);
    skip_newlines(lx);
    if (!is_kw(lex_peek(lx), "in")) { eof_means_incomplete(lx); perr(lx, "case"); return NULL; }
    lex_next(lx);
    CaseItem **tail = &n->items;
    for (;;) {
        skip_newlines(lx);
        t = lex_peek(lx);
        if (is_kw(t, "esac")) { lex_next(lx); return n; }
        if (t->type == T_EOF) { lx->incomplete = 1; return NULL; }
        if (t->type == T_LPAREN) { lex_next(lx); t = lex_peek(lx); }
        CaseItem *it = xmalloc(sizeof *it);
        memset(it, 0, sizeof *it);
        StrVec pats = { 0 };
        for (;;) {
            t = lex_peek(lx);
            if (t->type != T_WORD) { perr(lx, "case pattern"); return NULL; }
            sv_push(&pats, t->text);
            lex_next(lx);
            if (lex_peek(lx)->type == T_PIPE) { lex_next(lx); continue; }
            break;
        }
        it->patterns = pats.v;
        it->npat = pats.n;
        if (lex_peek(lx)->type != T_RPAREN) { eof_means_incomplete(lx); perr(lx, "case"); return NULL; }
        lex_next(lx);
        it->body = parse_list(lx);
        if (lx->error || lx->incomplete) return NULL;
        if (lex_peek(lx)->type == T_DSEMI) lex_next(lx);
        *tail = it;
        tail = &it->next;
    }
}

static Node *parse_function_body(Lexer *lx, char *name) {
    skip_newlines(lx);
    Node *body = parse_command(lx);
    if (!body) { eof_means_incomplete(lx); if (!lx->incomplete) perr(lx, "function"); return NULL; }
    Node *f = mk(N_FUNC);
    f->name = name;
    f->a = body;
    return f;
}

static Node *parse_simple(Lexer *lx) {
    Node *n = mk(N_SIMPLE);
    StrVec words = { 0 };
    Assign **atail = &n->assigns;
    for (;;) {
        Token *t = lex_peek(lx);
        if (t->type == T_REDIR) {
            Redir *r = parse_redirect(lx);
            if (!r) return NULL;
            append_redir(n, r);
            continue;
        }
        if (t->type != T_WORD) break;
        if (words.n == 0 && word_is_assignment(t->text)) {
            Assign *a = xmalloc(sizeof *a);
            char *eq = strchr(t->text, '=');
            a->name = xmalloc((size_t)(eq - t->text) + 1);
            memcpy(a->name, t->text, (size_t)(eq - t->text));
            a->name[eq - t->text] = 0;
            a->value = xstrdup(eq + 1);
            a->next = NULL;
            *atail = a;
            atail = &a->next;
            lex_next(lx);
            continue;
        }
        /* name() { ... } */
        if (words.n == 0 && !n->assigns) {
            Lexer save = *lx;
            char *name = t->text;
            lex_next(lx);
            if (lex_peek(lx)->type == T_LPAREN) {
                lex_next(lx);
                if (lex_peek(lx)->type == T_RPAREN) {
                    lex_next(lx);
                    return parse_function_body(lx, name);
                }
                perr(lx, "(");
                return NULL;
            }
            *lx = save;
            lex_peek(lx);
        }
        sv_push(&words, t->text);
        lex_next(lx);
    }
    n->words = words.v;
    n->nwords = words.n;
    if (!n->words && !n->assigns && !n->redirs) return NULL;
    return n;
}

static Node *parse_command(Lexer *lx) {
    Token *t = lex_peek(lx);
    Node *n = NULL;
    if (t->type == T_LPAREN) {
        lex_next(lx);
        n = mk(N_SUBSHELL);
        n->a = parse_list(lx);
        if (lx->error || lx->incomplete) return NULL;
        if (lex_peek(lx)->type != T_RPAREN) { eof_means_incomplete(lx); perr(lx, "("); return NULL; }
        lex_next(lx);
    } else if (is_kw(t, "{")) {
        lex_next(lx);
        n = mk(N_GROUP);
        n->a = parse_list(lx);
        if (lx->error || lx->incomplete) return NULL;
        if (!is_kw(lex_peek(lx), "}")) { eof_means_incomplete(lx); perr(lx, "{"); return NULL; }
        lex_next(lx);
    } else if (is_kw(t, "if")) {
        n = parse_if(lx);
    } else if (is_kw(t, "while")) {
        n = parse_while(lx, N_WHILE);
    } else if (is_kw(t, "until")) {
        n = parse_while(lx, N_UNTIL);
    } else if (is_kw(t, "for")) {
        n = parse_for(lx);
    } else if (is_kw(t, "case")) {
        n = parse_case(lx);
    } else if (is_kw(t, "function")) {
        lex_next(lx);
        Token *nt = lex_peek(lx);
        if (nt->type != T_WORD) { perr(lx, "function"); return NULL; }
        char *name = nt->text;
        lex_next(lx);
        if (lex_peek(lx)->type == T_LPAREN) { lex_next(lx); if (lex_peek(lx)->type == T_RPAREN) lex_next(lx); }
        return parse_function_body(lx, name);
    } else if (t->type == T_WORD && (!strcmp(t->text, "then") || !strcmp(t->text, "fi") || !strcmp(t->text, "done") ||
                                     !strcmp(t->text, "do") || !strcmp(t->text, "esac") || !strcmp(t->text, "else") ||
                                     !strcmp(t->text, "elif") || !strcmp(t->text, "}"))) {
        perr(lx, t->text);
        return NULL;
    } else {
        n = parse_simple(lx);
        if (!n && !lx->error && !lx->incomplete) perr(lx, "command");
        return n;
    }
    if (!n) return NULL;
    if (n->kind != N_FUNC) parse_trailing_redirs(lx, n);
    return n;
}

Node *parse_program(Lexer *lx) {
    Node *head = NULL;
    for (;;) {
        while (lex_peek(lx)->type == T_NEWLINE || lex_peek(lx)->type == T_SEMI) lex_next(lx);
        if (lex_peek(lx)->type == T_EOF) break;
        if (is_terminator(lx)) { perr(lx, lex_peek(lx)->type == T_WORD ? lex_peek(lx)->text : "token"); return NULL; }
        Node *n = parse_and_or(lx);
        if (!n) return NULL;
        Token *t = lex_peek(lx);
        if (t->type == T_AMP) {
            lex_next(lx);
            Node *bg = mk(N_BG);
            bg->a = n;
            n = bg;
        } else if (t->type == T_SEMI || t->type == T_NEWLINE) {
            lex_next(lx);
        } else if (t->type != T_EOF) {
            perr(lx, t->type == T_WORD ? t->text : "token");
            return NULL;
        }
        if (head) {
            Node *l = mk(N_LIST);
            l->a = head;
            l->b = n;
            head = l;
        } else {
            head = n;
        }
    }
    if (lx->incomplete) return NULL;
    return head;
}
