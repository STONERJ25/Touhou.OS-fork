/*
 * The CSS tokenizer: CSS Syntax Module Level 3, section 4 ("Tokenization"), over UTF-8 text.
 *
 * The whole input becomes an array of tokens; token values (identifiers, strings, URLs, hash names, units) are unescaped
 * into one pool, and every token keeps its source range (var() substitution works on source text). A last pass pairs
 * each function and block opener with its closing token the way "consume a simple block" and "consume a function" do:
 * a closer that does not match the innermost opener is an ordinary token, and openers left open at the end are closed
 * by the end of the input.
 */
#include "css.h"

typedef struct {
    const unsigned char *s;
    size_t n, i;
    CssTokens *out;
    WebArena *a;
    bool fail;
} Lex;

static int at(Lex *L, size_t k) {           /* the code unit k bytes ahead, -1 at the end; CR and FF read as LF */
    if (L->i + k >= L->n) return -1;
    int c = L->s[L->i + k];
    if (c == '\r' || c == '\f') return '\n';
    return c;
}

static bool is_ws(int c) { return c == ' ' || c == '\t' || c == '\n'; }
static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_hex(int c) { return is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
static bool name_start(int c) { return c >= 0x80 || c == '_' || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'); }
static bool name_char(int c) { return name_start(c) || is_digit(c) || c == '-'; }
static bool valid_escape(int a, int b) { return a == '\\' && b != '\n' && b != -1; }

static bool starts_ident(int a, int b, int c) {
    if (a == '-') return name_start(b) || b == '-' || valid_escape(b, c);
    if (name_start(a)) return true;
    if (a == '\\') return valid_escape(a, b);
    return false;
}

static bool starts_number(int a, int b, int c) {
    if (a == '+' || a == '-') return is_digit(b) || (b == '.' && is_digit(c));
    if (a == '.') return is_digit(b);
    return is_digit(a);
}

/* ------------------------------------------------------------------------------------------------ the pool */

static WebBuf *pool(Lex *L) { return &L->out->pool; }

static void put_byte(WebBuf *b, int c) { buf_putc(b, (char)c); }

static void put_cp(WebBuf *b, uint32_t cp) {
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    buf_put_cp(b, cp);
}

/* consumes an escape (the backslash is already consumed) into b */
static void consume_escape(Lex *L, WebBuf *b) {
    int c = at(L, 0);
    if (c < 0) { put_cp(b, 0xFFFD); return; }
    if (is_hex(c)) {
        uint32_t v = 0;
        int k = 0;
        while (k < 6 && is_hex(at(L, 0))) {
            int h = at(L, 0);
            v = v * 16 + (uint32_t)(is_digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
            L->i++;
            k++;
        }
        if (is_ws(at(L, 0))) {
            if (L->s[L->i] == '\r' && L->i + 1 < L->n && L->s[L->i + 1] == '\n') L->i++;
            L->i++;
        }
        put_cp(b, v);
        return;
    }
    /* any other code point stands for itself: copy its UTF-8 bytes */
    size_t len;
    uint32_t cp = utf8_decode((const char *)L->s + L->i, L->n - L->i, &len);
    L->i += len;
    put_cp(b, cp);
}

static void consume_ident_into(Lex *L, WebBuf *b) {
    for (;;) {
        int c = at(L, 0);
        if (c >= 0 && name_char(c)) {
            if (c == 0) { put_cp(b, 0xFFFD); L->i++; continue; }
            put_byte(b, L->s[L->i]);
            L->i++;
        } else if (valid_escape(c, at(L, 1))) {
            L->i++;
            consume_escape(L, b);
        } else break;
    }
}

/* ------------------------------------------------------------------------------------------------ tokens */

static CssTok *push_tok(Lex *L, int type, size_t pos) {
    CssTokens *t = L->out;
    if (t->n + 1 >= t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 256;
        CssTok *v = wa_realloc(L->a, t->v, sizeof(CssTok) * t->cap, sizeof(CssTok) * nc);
        if (!v) { L->fail = true; return NULL; }
        t->v = v;
        t->cap = nc;
    }
    CssTok *k = &t->v[t->n++];
    memset(k, 0, sizeof *k);
    k->type = (uint8_t)type;
    k->pos = (uint32_t)pos;
    return k;
}

static double consume_number(Lex *L, bool *is_int) {
    double sign = 1;
    *is_int = true;
    int c = at(L, 0);
    if (c == '+' || c == '-') { if (c == '-') sign = -1; L->i++; }
    double ip = 0;
    while (is_digit(at(L, 0))) { ip = ip * 10 + (at(L, 0) - '0'); L->i++; }
    double fp = 0, scale = 1;
    if (at(L, 0) == '.' && is_digit(at(L, 1))) {
        *is_int = false;
        L->i++;
        while (is_digit(at(L, 0))) { fp = fp * 10 + (at(L, 0) - '0'); scale *= 10; L->i++; }
    }
    int e = 0, es = 1;
    c = at(L, 0);
    if ((c == 'e' || c == 'E') && (is_digit(at(L, 1)) || ((at(L, 1) == '+' || at(L, 1) == '-') && is_digit(at(L, 2))))) {
        *is_int = false;
        L->i++;
        if (at(L, 0) == '+' || at(L, 0) == '-') { if (at(L, 0) == '-') es = -1; L->i++; }
        while (is_digit(at(L, 0))) { if (e < 1000) e = e * 10 + (at(L, 0) - '0'); L->i++; }
    }
    double v = ip + fp / scale;
    for (int k = 0; k < e && v != 0; k++) v = es > 0 ? v * 10 : v / 10;
    if (v > 1e30) v = 1e30;
    return sign * v;
}

static void consume_numeric(Lex *L, size_t pos) {
    WebBuf *pb = pool(L);
    bool is_int;
    double v = consume_number(L, &is_int);
    if (starts_ident(at(L, 0), at(L, 1), at(L, 2))) {
        CssTok *k = push_tok(L, CT_DIMENSION, pos);
        if (!k) return;
        k->num = v;
        k->flags = is_int ? CTF_INT : 0;
        uint32_t s = pb->n;
        consume_ident_into(L, pb);
        k = &L->out->v[L->out->n - 1];
        k->vs = s;
        k->vn = pb->n - s;
        buf_putc(pb, 0);
    } else if (at(L, 0) == '%') {
        L->i++;
        CssTok *k = push_tok(L, CT_PERCENT, pos);
        if (k) k->num = v;
    } else {
        CssTok *k = push_tok(L, CT_NUMBER, pos);
        if (k) { k->num = v; k->flags = is_int ? CTF_INT : 0; }
    }
}

static void consume_bad_url(Lex *L) {
    for (;;) {
        int c = at(L, 0);
        if (c < 0) return;
        if (c == ')') { L->i++; return; }
        if (valid_escape(c, at(L, 1))) { L->i++; WebBuf tmp; buf_init(&tmp, L->a); consume_escape(L, &tmp); buf_free(&tmp); continue; }
        L->i++;
    }
}

static void consume_url(Lex *L, size_t pos) {
    WebBuf *pb = pool(L);
    uint32_t s = pb->n;
    while (is_ws(at(L, 0))) L->i++;
    for (;;) {
        int c = at(L, 0);
        if (c == ')' ) { L->i++; break; }
        if (c < 0) break;
        if (is_ws(c)) {
            while (is_ws(at(L, 0))) L->i++;
            if (at(L, 0) == ')') { L->i++; break; }
            if (at(L, 0) < 0) break;
            consume_bad_url(L);
            pb->n = s;
            push_tok(L, CT_BAD_URL, pos);
            return;
        }
        if (c == '"' || c == '\'' || c == '(' || c == 0x7F || (c < 0x20 && c != '\t' && c != '\n')) {
            consume_bad_url(L);
            pb->n = s;
            push_tok(L, CT_BAD_URL, pos);
            return;
        }
        if (c == '\\') {
            if (valid_escape(c, at(L, 1))) { L->i++; consume_escape(L, pb); continue; }
            consume_bad_url(L);
            pb->n = s;
            push_tok(L, CT_BAD_URL, pos);
            return;
        }
        put_byte(pb, L->s[L->i]);
        L->i++;
    }
    CssTok *k = push_tok(L, CT_URL, pos);
    if (!k) return;
    k->vs = s;
    k->vn = pb->n - s;
    buf_putc(pb, 0);
}

static void consume_ident_like(Lex *L, size_t pos) {
    WebBuf *pb = pool(L);
    uint32_t s = pb->n;
    consume_ident_into(L, pb);
    uint32_t n = pb->n - s;
    buf_putc(pb, 0);
    if (n == 3 && strncasecmp(pb->p + s, "url", 3) == 0 && at(L, 0) == '(') {
        L->i++;
        size_t save = L->i;
        while (is_ws(at(L, 0)) && is_ws(at(L, 1))) L->i++;
        int c = at(L, 0), c2 = at(L, 1);
        if (c == '"' || c == '\'' || (is_ws(c) && (c2 == '"' || c2 == '\''))) {
            L->i = save;
            CssTok *k = push_tok(L, CT_FUNCTION, pos);
            if (k) { k->vs = s; k->vn = n; }
            return;
        }
        L->i = save;
        consume_url(L, pos);
        return;
    }
    if (at(L, 0) == '(') {
        L->i++;
        CssTok *k = push_tok(L, CT_FUNCTION, pos);
        if (k) { k->vs = s; k->vn = n; }
        return;
    }
    CssTok *k = push_tok(L, CT_IDENT, pos);
    if (k) { k->vs = s; k->vn = n; }
}

static void consume_string(Lex *L, int q, size_t pos) {
    WebBuf *pb = pool(L);
    uint32_t s = pb->n;
    for (;;) {
        int c = at(L, 0);
        if (c < 0) break;                                /* EOF: parse error, the string is returned */
        if (c == q) { L->i++; break; }
        if (c == '\n') {                                 /* newline: a bad string, the newline is not consumed */
            pb->n = s;
            push_tok(L, CT_BAD_STRING, pos);
            return;
        }
        if (c == '\\') {
            int d = at(L, 1);
            if (d < 0) { L->i++; continue; }
            if (d == '\n') {
                L->i++;
                if (L->s[L->i] == '\r' && L->i + 1 < L->n && L->s[L->i + 1] == '\n') L->i++;
                L->i++;
                continue;
            }
            L->i++;
            consume_escape(L, pb);
            continue;
        }
        if (c == 0) { put_cp(pb, 0xFFFD); L->i++; continue; }
        put_byte(pb, L->s[L->i]);
        L->i++;
    }
    CssTok *k = push_tok(L, CT_STRING, pos);
    if (!k) return;
    k->vs = s;
    k->vn = pb->n - s;
    buf_putc(pb, 0);
}

static void next_token(Lex *L) {
    /* comments */
    while (at(L, 0) == '/' && at(L, 1) == '*') {
        L->i += 2;
        while (L->i < L->n && !(at(L, 0) == '*' && at(L, 1) == '/')) L->i++;
        L->i = L->i + 2 <= L->n ? L->i + 2 : L->n;
    }
    size_t pos = L->i;
    int c = at(L, 0);
    if (c < 0) return;
    if (is_ws(c)) {
        while (is_ws(at(L, 0))) L->i++;
        push_tok(L, CT_WS, pos);
        return;
    }
    switch (c) {
    case '"': case '\'':
        L->i++;
        consume_string(L, c, pos);
        return;
    case '#':
        if (name_char(at(L, 1)) || valid_escape(at(L, 1), at(L, 2))) {
            L->i++;
            bool id = starts_ident(at(L, 0), at(L, 1), at(L, 2));
            WebBuf *pb = pool(L);
            uint32_t s = pb->n;
            consume_ident_into(L, pb);
            CssTok *k = push_tok(L, CT_HASH, pos);
            if (k) { k->vs = s; k->vn = pb->n - s; k->flags = id ? CTF_ID : 0; }
            buf_putc(pb, 0);
            return;
        }
        break;
    case '(': L->i++; push_tok(L, CT_LPAREN, pos); return;
    case ')': L->i++; push_tok(L, CT_RPAREN, pos); return;
    case '[': L->i++; push_tok(L, CT_LBRACK, pos); return;
    case ']': L->i++; push_tok(L, CT_RBRACK, pos); return;
    case '{': L->i++; push_tok(L, CT_LBRACE, pos); return;
    case '}': L->i++; push_tok(L, CT_RBRACE, pos); return;
    case ',': L->i++; push_tok(L, CT_COMMA, pos); return;
    case ':': L->i++; push_tok(L, CT_COLON, pos); return;
    case ';': L->i++; push_tok(L, CT_SEMI, pos); return;
    case '+': case '.':
        if (starts_number(c, at(L, 1), at(L, 2))) { consume_numeric(L, pos); return; }
        break;
    case '-':
        if (starts_number(c, at(L, 1), at(L, 2))) { consume_numeric(L, pos); return; }
        if (at(L, 1) == '-' && at(L, 2) == '>') { L->i += 3; push_tok(L, CT_CDC, pos); return; }
        if (starts_ident(c, at(L, 1), at(L, 2))) { consume_ident_like(L, pos); return; }
        break;
    case '<':
        if (at(L, 1) == '!' && at(L, 2) == '-' && at(L, 3) == '-') { L->i += 4; push_tok(L, CT_CDO, pos); return; }
        break;
    case '@':
        if (starts_ident(at(L, 1), at(L, 2), at(L, 3))) {
            L->i++;
            WebBuf *pb = pool(L);
            uint32_t s = pb->n;
            consume_ident_into(L, pb);
            CssTok *k = push_tok(L, CT_AT, pos);
            if (k) { k->vs = s; k->vn = pb->n - s; }
            buf_putc(pb, 0);
            return;
        }
        break;
    case '\\':
        if (valid_escape(c, at(L, 1))) { consume_ident_like(L, pos); return; }
        break;
    default:
        if (is_digit(c)) { consume_numeric(L, pos); return; }
        if (name_start(c)) { consume_ident_like(L, pos); return; }
        break;
    }
    /* a delimiter: one code point */
    size_t len;
    uint32_t cp = utf8_decode((const char *)L->s + L->i, L->n - L->i, &len);
    L->i += len;
    CssTok *k = push_tok(L, CT_DELIM, pos);
    if (k) k->delim = (uint16_t)(cp < 0x80 ? cp : 0xFFFF);
}

bool css_tokenize(WebArena *a, const char *src, size_t n, CssTokens *out) {
    memset(out, 0, sizeof *out);
    out->src = src;
    Lex L = { (const unsigned char *)src, n, 0, out, a, false };
    WebBuf *pb = pool(&L);
    buf_init(pb, a);
    buf_putc(pb, 0);                                     /* offset 0 is the empty string */
    while (L.i < L.n && !L.fail) {
        size_t before = L.i;
        uint32_t ntok = out->n;
        next_token(&L);
        if (out->n > ntok) out->v[out->n - 1].end = (uint32_t)L.i;
        if (L.i == before && out->n == ntok) L.i++;      /* never stall */
    }
    CssTok *eof = push_tok(&L, CT_EOF, n);
    if (!eof || L.fail || a->budget->exceeded) return false;
    eof->end = (uint32_t)n;
    out->n--;                                            /* the EOF token stays at v[n] */
    /* pair openers and closers */
    uint32_t stack[256];
    int sp = 0;
    for (uint32_t i = 0; i < out->n; i++) {
        CssTok *k = &out->v[i];
        int ty = k->type;
        if (ty == CT_FUNCTION || ty == CT_LPAREN || ty == CT_LBRACK || ty == CT_LBRACE) {
            if (sp < 256) stack[sp++] = i;
            k->close = out->n;
            continue;
        }
        if (sp && (ty == CT_RPAREN || ty == CT_RBRACK || ty == CT_RBRACE)) {
            CssTok *o = &out->v[stack[sp - 1]];
            int want = o->type == CT_LBRACK ? CT_RBRACK : o->type == CT_LBRACE ? CT_RBRACE : CT_RPAREN;
            if (ty == want) {
                o->close = i;
                sp--;
            }
        }
    }
    return true;
}

void css_tokens_free(WebArena *a, CssTokens *t) {
    if (t->v) wa_free(a, t->v, sizeof(CssTok) * t->cap);
    buf_free(&t->pool);
    t->v = NULL;
}

bool tok_is_ident(const CssTokens *t, const CssTok *k, const char *lower) {
    if (k->type != CT_IDENT) return false;
    size_t n = strlen(lower);
    return k->vn == n && strncasecmp(t->pool.p + k->vs, lower, n) == 0;
}
