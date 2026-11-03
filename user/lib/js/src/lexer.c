/*
 * lexer.c: the tokenizer (ECMA-262 12, "ECMAScript Language: Lexical Grammar"; Annex B.1 for legacy octal literals
 * and escapes and HTML-like comments).
 *
 * Input is UTF-8 (WTF-8: encoded lone surrogates are accepted, so eval of any string round-trips).  The lexer reads
 * one token at a time on demand; the parser tells it when a '/' starts a regular expression (kz_lex_regexp) and when
 * a '}' continues a template (kz_lex_template_continue), the two places where the grammar, not the characters,
 * decides.  Columns are counted in UTF-16 units from 1, like other engines report them.
 */
#include "parser.h"
#include <stdarg.h>

static const struct { int kw; int atom; } kw_table[] = {
    { KW_await, KZ_ATOM_await }, { KW_break, KZ_ATOM_kw_break }, { KW_case, KZ_ATOM_kw_case },
    { KW_catch, KZ_ATOM_catch }, { KW_class, KZ_ATOM_kw_class }, { KW_const, KZ_ATOM_kw_const },
    { KW_continue, KZ_ATOM_kw_continue }, { KW_debugger, KZ_ATOM_kw_debugger }, { KW_default, KZ_ATOM_default },
    { KW_delete, KZ_ATOM_delete }, { KW_do, KZ_ATOM_kw_do }, { KW_else, KZ_ATOM_kw_else }, { KW_enum, KZ_ATOM_kw_enum },
    { KW_export, KZ_ATOM_kw_export }, { KW_extends, KZ_ATOM_kw_extends }, { KW_false, KZ_ATOM_false },
    { KW_finally, KZ_ATOM_finally }, { KW_for, KZ_ATOM_kw_for }, { KW_function, KZ_ATOM_function },
    { KW_if, KZ_ATOM_kw_if }, { KW_import, KZ_ATOM_kw_import }, { KW_in, KZ_ATOM_kw_in },
    { KW_instanceof, KZ_ATOM_kw_instanceof }, { KW_new, KZ_ATOM_kw_new }, { KW_null, KZ_ATOM_null },
    { KW_return, KZ_ATOM_return }, { KW_super, KZ_ATOM_kw_super }, { KW_switch, KZ_ATOM_kw_switch },
    { KW_this, KZ_ATOM_this }, { KW_throw, KZ_ATOM_throw }, { KW_true, KZ_ATOM_true }, { KW_try, KZ_ATOM_kw_try },
    { KW_typeof, KZ_ATOM_kw_typeof }, { KW_var, KZ_ATOM_kw_var }, { KW_void, KZ_ATOM_kw_void },
    { KW_while, KZ_ATOM_kw_while }, { KW_with, KZ_ATOM_kw_with }, { KW_yield, KZ_ATOM_yield },
    { KW_let, KZ_ATOM_let }, { KW_static, KZ_ATOM_static }, { KW_implements, KZ_ATOM_kw_implements },
    { KW_interface, KZ_ATOM_kw_interface }, { KW_package, KZ_ATOM_kw_package }, { KW_private, KZ_ATOM_kw_private },
    { KW_protected, KZ_ATOM_kw_protected }, { KW_public, KZ_ATOM_kw_public }, { KW_async, KZ_ATOM_async },
    { KW_of, KZ_ATOM_of }, { KW_get, KZ_ATOM_get }, { KW_set, KZ_ATOM_set }, { KW_target, KZ_ATOM_target },
    { KW_meta, KZ_ATOM_meta }, { KW_as, KZ_ATOM_as }, { KW_from, KZ_ATOM_from }, { KW_eval, KZ_ATOM_eval },
    { KW_arguments, KZ_ATOM_arguments },
};

/* Keyword atoms are permanent; their keyword number is kept in the (otherwise unused) depth field. */
void kz_keywords_init(JSRuntime *rt);
void kz_keywords_init(JSRuntime *rt) {
    for (size_t i = 0; i < KZ_COUNTOF(kw_table); i++) JV_STR(rt->atom[kw_table[i].atom])->depth = (uint32_t)kw_table[i].kw;
}

int kz_keyword_of(JSValue atom, JSRuntime *rt) {
    (void)rt;
    JSString *s = JV_STR(atom);
    return (s->h.flags & STR_PERM) ? (int)s->depth : KW_NONE;
}

void kz_lex_init(Lexer *lx, JSContext *ctx, const uint8_t *src, uint32_t len) {
    memset(lx, 0, sizeof *lx);
    lx->ctx = ctx;
    lx->src = src;
    lx->len = len;
    lx->line = 1;
    lx->html_comments = true;
    lx->col_cache_line = 1;
}

/* column (from 1, in UTF-16 units) of byte offset pos on the current line */
uint32_t kz_lex_col(Lexer *lx, uint32_t pos, uint32_t *line) {
    *line = lx->line;
    uint32_t from = lx->line_start, col = 1;
    if (lx->col_cache_line == lx->line && lx->col_cache_pos >= lx->line_start && lx->col_cache_pos <= pos) {
        from = lx->col_cache_pos;
        col = lx->col_cache_col;
    }
    for (uint32_t i = from; i < pos && i < lx->len;) {
        uint8_t c = lx->src[i];
        if (c < 0x80) { i++; col++; continue; }
        size_t j = i;
        uint32_t cp = kz_utf8_decode(lx->src, lx->len, &j);
        i = (uint32_t)j;
        col += cp > 0xFFFF ? 2 : 1;
    }
    lx->col_cache_pos = pos;
    lx->col_cache_col = col;
    lx->col_cache_line = lx->line;
    return col;
}

void kz_lex_error(Lexer *lx, uint32_t pos, const char *fmt, ...) {
    Parser *p = lx->p;
    if (p && p->failed) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    extern int kz_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
    kz_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    uint32_t line, col;
    if (pos < lx->line_start) { line = lx->tok.line; col = lx->tok.col; }
    else col = kz_lex_col(lx, pos, &line);
    if (p) { p->failed = true; p->err_line = line; p->err_col = col; }
    kz_throw_syntax_at(lx->ctx, p ? p->filename : JS_UNDEFINED, line, col, "%s", msg);
}

/* ------------------------------------------------------------------ characters */

static inline uint32_t peek_cp(Lexer *lx, uint32_t pos, uint32_t *next) {
    if (pos >= lx->len) { *next = pos; return 0xFFFFFFFFu; }
    uint8_t c = lx->src[pos];
    if (c < 0x80) { *next = pos + 1; return c; }
    size_t j = pos;
    uint32_t cp = kz_utf8_decode(lx->src, lx->len, &j);
    *next = (uint32_t)j;
    return cp;
}

static inline bool is_line_term(uint32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

static bool is_ws(uint32_t c) {
    if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF) return true;
    return c > 0x7F && kz_unicode_is_space(c);
}

static bool id_start(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    return kz_unicode_is_id_start(c);
}

static bool id_continue(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '_';
    return c == 0x200C || c == 0x200D || kz_unicode_is_id_continue(c);
}

static void new_line(Lexer *lx, uint32_t after) {
    lx->line++;
    lx->line_start = after;
}

static int hexval(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

/* \u escapes in identifiers and strings: reads after the 'u'; returns the code point or -1 */
static int64_t read_unicode_escape(Lexer *lx, uint32_t *pos) {
    uint32_t p = *pos;
    if (p < lx->len && lx->src[p] == '{') {
        p++;
        uint32_t v = 0;
        int n = 0;
        while (p < lx->len && lx->src[p] != '}') {
            int h = hexval(lx->src[p]);
            if (h < 0) return -1;
            v = v * 16 + (uint32_t)h;
            if (v > 0x10FFFF) return -1;
            p++;
            n++;
        }
        if (p >= lx->len || n == 0) return -1;
        *pos = p + 1;
        return v;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (p >= lx->len) return -1;
        int h = hexval(lx->src[p]);
        if (h < 0) return -1;
        v = v * 16 + (uint32_t)h;
        p++;
    }
    *pos = p;
    return v;
}

/* ------------------------------------------------------------------ whitespace and comments */

/* Skips whitespace and comments.  Returns -1 on an unterminated comment; sets *nl when a line break was crossed. */
static int skip_space(Lexer *lx, bool *nl) {
    uint32_t pos = lx->pos;
    bool line_start_ok = lx->pos == 0 || *nl;      /* for --> (only at the start of a line) */
    for (;;) {
        uint32_t next, c = peek_cp(lx, pos, &next);
        if (c == 0xFFFFFFFFu) break;
        if (c == '\n' || c == 0x2028 || c == 0x2029) { pos = next; new_line(lx, pos); *nl = true; line_start_ok = true; continue; }
        if (c == '\r') {
            pos = next;
            if (pos < lx->len && lx->src[pos] == '\n') pos++;
            new_line(lx, pos);
            *nl = true;
            line_start_ok = true;
            continue;
        }
        if (is_ws(c)) { pos = next; continue; }
        if (c == '/' && pos + 1 < lx->len && lx->src[pos + 1] == '/') {
        line_comment:
            pos += 2;
            for (;;) {
                uint32_t n2, d = peek_cp(lx, pos, &n2);
                if (d == 0xFFFFFFFFu || is_line_term(d)) break;
                pos = n2;
            }
            continue;
        }
        if (c == '/' && pos + 1 < lx->len && lx->src[pos + 1] == '*') {
            uint32_t start = pos;
            pos += 2;
            bool closed = false;
            while (pos < lx->len) {
                uint32_t n2, d = peek_cp(lx, pos, &n2);
                if (d == '*' && n2 < lx->len && lx->src[n2] == '/') { pos = n2 + 1; closed = true; break; }
                if (d == '\r') {
                    pos = n2;
                    if (pos < lx->len && lx->src[pos] == '\n') pos++;
                    new_line(lx, pos);
                    *nl = true;
                    line_start_ok = true;
                    continue;
                }
                if (is_line_term(d)) { pos = n2; new_line(lx, pos); *nl = true; line_start_ok = true; continue; }
                pos = n2;
            }
            if (!closed) { lx->pos = pos; kz_lex_error(lx, start, "Unterminated comment"); return -1; }
            continue;
        }
        if (lx->html_comments) {
            if (c == '<' && pos + 3 < lx->len && !memcmp(lx->src + pos, "<!--", 4)) { pos += 2; goto line_comment; }
            if (c == '-' && line_start_ok && pos + 2 < lx->len && !memcmp(lx->src + pos, "-->", 3)) { pos += 1; goto line_comment; }
        }
        break;
    }
    lx->pos = pos;
    return 0;
}

/* ------------------------------------------------------------------ names */

static int lex_name(Lexer *lx, uint32_t start, bool private_name) {
    JSContext *ctx = lx->ctx;
    uint32_t pos = start;
    bool escaped = false, ascii = true;
    /* fast path: plain ASCII without escapes */
    while (pos < lx->len) {
        uint8_t c = lx->src[pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '_') { pos++; continue; }
        if (c == '\\' || c >= 0x80) { ascii = false; }
        break;
    }
    JSValue atom;
    if (ascii) {
        atom = kz_atom_len(ctx, (const char *)lx->src + start, pos - start);
    } else {
        KzStrBuf sb;
        kz_sb_init(&sb, ctx);
        pos = start;
        bool first = true;
        for (;;) {
            uint32_t next, c = peek_cp(lx, pos, &next);
            if (c == '\\') {
                if (next >= lx->len || lx->src[next] != 'u') { kz_sb_free(&sb); kz_lex_error(lx, pos, "Invalid or unexpected token"); return -1; }
                uint32_t q = next + 1;
                int64_t v = read_unicode_escape(lx, &q);
                if (v < 0 || !(first ? id_start((uint32_t)v) : id_continue((uint32_t)v))) {
                    kz_sb_free(&sb);
                    kz_lex_error(lx, pos, "Invalid Unicode escape sequence");
                    return -1;
                }
                escaped = true;
                kz_sb_put_cp(&sb, (uint32_t)v);
                pos = q;
            } else if (c != 0xFFFFFFFFu && (first ? id_start(c) : id_continue(c))) {
                kz_sb_put_cp(&sb, c);
                pos = next;
            } else break;
            first = false;
        }
        JSValue s = kz_sb_finish(&sb);
        if (s == JS_EXCEPTION) return -1;
        KZ_ROOT(ctx, rs);
        rs = s;
        atom = kz_atom_from_str(ctx, rs);
    }
    if (atom == JS_EXCEPTION) return -1;
    if (kz_parser_keep(lx->p, atom) == JS_EXCEPTION) return -1;
    Token *t = &lx->tok;
    t->type = private_name ? T_PRIVATE : T_NAME;
    t->val = atom;
    t->escaped = escaped;
    t->kw = private_name ? KW_NONE : kz_keyword_of(atom, ctx->rt);
    lx->pos = pos;
    return 0;
}

/* ------------------------------------------------------------------ numbers */

static int lex_number(Lexer *lx, uint32_t start) {
    const uint8_t *s = lx->src;
    uint32_t pos = start, n = lx->len;
    char buf[512];
    size_t o = 0;
    bool big = false, legacy = false;
    int radix = 10;
    Token *t = &lx->tok;
    JSContext *ctx = lx->ctx;
    char *heap = NULL;
    char *out = buf;
    size_t cap = sizeof buf;
#define PUT(ch) do { if (o + 2 > cap) { size_t nc = cap * 2; char *nb = kz_rt_malloc(ctx->rt, nc); if (!nb) goto oom; memcpy(nb, out, o); if (heap) kz_free(ctx->rt, heap); heap = out = nb; cap = nc; } out[o++] = (char)(ch); } while (0)
    /* digits with numeric separators: returns false on a misplaced '_' */
#define DIGITS(test) do { \
        bool any_ = false, sep_ = false; \
        while (pos < n) { \
            uint8_t ch_ = s[pos]; \
            if (ch_ == '_') { if (!any_ || sep_) goto bad_sep; sep_ = true; pos++; continue; } \
            if (!(test)) break; \
            PUT(ch_); any_ = true; sep_ = false; pos++; \
        } \
        if (sep_) goto bad_sep; \
    } while (0)
    if (s[pos] == '0' && pos + 1 < n && (s[pos + 1] | 0x20) == 'x') radix = 16;
    else if (s[pos] == '0' && pos + 1 < n && (s[pos + 1] | 0x20) == 'o') radix = 8;
    else if (s[pos] == '0' && pos + 1 < n && (s[pos + 1] | 0x20) == 'b') radix = 2;
    if (radix != 10) {
        pos += 2;
        uint32_t dstart = pos;
        if (radix == 16) DIGITS(hexval(ch_) >= 0);
        else if (radix == 8) DIGITS(ch_ >= '0' && ch_ <= '7');
        else DIGITS(ch_ == '0' || ch_ == '1');
        if (pos == dstart || o == 0) goto bad;
        if (pos < n && s[pos] == 'n') { big = true; pos++; }
        if (big) {
            t->type = T_BIGINT;
            t->raw = kz_str_from_latin1(ctx, (const uint8_t *)out, (uint32_t)o);
            if (t->raw == JS_EXCEPTION || kz_parser_keep(lx->p, t->raw) == JS_EXCEPTION) goto fail;
            t->num = radix;
        } else {
            t->type = T_NUMBER;
            t->num = kz_parse_radix_int(out, o, radix);
        }
        goto done;
    }
    if (s[pos] == '0' && pos + 1 < n && s[pos + 1] >= '0' && s[pos + 1] <= '9') {
        /* legacy octal 017, or a "non-octal decimal" 089 (both sloppy-only) */
        uint32_t q = pos + 1;
        bool octal = true;
        while (q < n && s[q] >= '0' && s[q] <= '9') { if (s[q] >= '8') octal = false; q++; }
        legacy = true;
        if (octal) {
            for (uint32_t i = pos + 1; i < q; i++) PUT(s[i]);
            pos = q;
            t->type = T_NUMBER;
            t->num = kz_parse_radix_int(out, o, 8);
            goto done;
        }
        for (uint32_t i = pos; i < q; i++) PUT(s[i]);
        pos = q;
        if (pos < n && s[pos] == '.') { PUT('.'); pos++; while (pos < n && s[pos] >= '0' && s[pos] <= '9') PUT(s[pos++]); }
        if (pos < n && (s[pos] | 0x20) == 'e') goto exponent;
        goto decimal_done;
    }
    if (s[pos] != '.') {
        if (s[pos] == '0' && pos + 1 < n && s[pos + 1] == '_') goto bad_sep;
        DIGITS(ch_ >= '0' && ch_ <= '9');
    }
    if (pos < n && s[pos] == 'n') {
        pos++;
        t->type = T_BIGINT;
        t->raw = kz_str_from_latin1(ctx, (const uint8_t *)out, (uint32_t)o);
        if (t->raw == JS_EXCEPTION || kz_parser_keep(lx->p, t->raw) == JS_EXCEPTION) goto fail;
        t->num = 10;
        goto done;
    }
    if (pos < n && s[pos] == '.') {
        PUT('.');
        pos++;
        if (pos < n && s[pos] == '_') goto bad_sep;
        DIGITS(ch_ >= '0' && ch_ <= '9');
    }
    if (pos < n && (s[pos] | 0x20) == 'e') {
    exponent:
        PUT('e');
        pos++;
        if (pos < n && (s[pos] == '+' || s[pos] == '-')) PUT(s[pos++]);
        if (pos >= n || !(s[pos] >= '0' && s[pos] <= '9')) goto bad;
        DIGITS(ch_ >= '0' && ch_ <= '9');
    }
decimal_done:
    t->type = T_NUMBER;
    t->num = kz_strtod(out, o);
done:
    /* the character after a number may not start an identifier or be a digit */
    if (pos < n) {
        uint32_t nx, c = peek_cp(lx, pos, &nx);
        if (c == '\\' || id_start(c) || (c >= '0' && c <= '9')) goto bad;
    }
    t->legacy_octal = legacy;
    lx->pos = pos;
    if (heap) kz_free(ctx->rt, heap);
    return 0;
bad_sep:
    if (heap) kz_free(ctx->rt, heap);
    kz_lex_error(lx, pos, "Numeric separators are not allowed here");
    return -1;
bad:
    if (heap) kz_free(ctx->rt, heap);
    kz_lex_error(lx, start, "Invalid or unexpected token");
    return -1;
oom:
    kz_throw_oom(ctx);
fail:
    if (heap) kz_free(ctx->rt, heap);
    if (lx->p) lx->p->failed = true;
    return -1;
#undef PUT
#undef DIGITS
}

/* ------------------------------------------------------------------ strings and templates */

/* One escape sequence after the backslash at s[pos] (pos points after '\').  Template mode: invalid escapes do not
   fail but mark the cooked value undefined (*bad = true).  Returns the new position or 0 on error. */
static uint32_t lex_escape(Lexer *lx, uint32_t pos, KzStrBuf *sb, bool tmpl, bool *bad, bool *legacy) {
    uint32_t next, c = peek_cp(lx, pos, &next);
    switch (c) {
    case 'n': kz_sb_putc(sb, '\n'); return next;
    case 't': kz_sb_putc(sb, '\t'); return next;
    case 'r': kz_sb_putc(sb, '\r'); return next;
    case 'b': kz_sb_putc(sb, '\b'); return next;
    case 'f': kz_sb_putc(sb, '\f'); return next;
    case 'v': kz_sb_putc(sb, '\v'); return next;
    case '\r':
        if (next < lx->len && lx->src[next] == '\n') next++;
        new_line(lx, next);
        return next;
    case '\n': case 0x2028: case 0x2029:
        new_line(lx, next);
        return next;
    case 'x': {
        int h1 = next < lx->len ? hexval(lx->src[next]) : -1;
        int h2 = next + 1 < lx->len ? hexval(lx->src[next + 1]) : -1;
        if (h1 < 0 || h2 < 0) {
            if (tmpl) { *bad = true; return next; }
            kz_lex_error(lx, pos - 1, "Invalid hexadecimal escape sequence");
            return 0;
        }
        kz_sb_putc(sb, (uint32_t)(h1 * 16 + h2));
        return next + 2;
    }
    case 'u': {
        uint32_t q = next;
        int64_t v = read_unicode_escape(lx, &q);
        if (v < 0) {
            if (tmpl) { *bad = true; return next; }
            kz_lex_error(lx, pos - 1, "Invalid Unicode escape sequence");
            return 0;
        }
        kz_sb_put_cp(sb, (uint32_t)v);
        return q;
    }
    case 0xFFFFFFFFu:
        return pos;
    default:
        if (c >= '0' && c <= '9') {
            if (c == '0' && !(next < lx->len && lx->src[next] >= '0' && lx->src[next] <= '9')) {
                kz_sb_putc(sb, 0);
                return next;
            }
            if (tmpl) { *bad = true; return next; }
            *legacy = true;
            if (c >= '8') { kz_sb_putc(sb, c); return next; }       /* \8 \9: the digit itself (sloppy) */
            /* legacy octal: up to three digits, value <= 0377 */
            uint32_t v = c - '0', q = next;
            if (q < lx->len && lx->src[q] >= '0' && lx->src[q] <= '7') {
                v = v * 8 + (lx->src[q++] - '0');
                if (c <= '3' && q < lx->len && lx->src[q] >= '0' && lx->src[q] <= '7') v = v * 8 + (lx->src[q++] - '0');
            }
            kz_sb_putc(sb, v);
            return q;
        }
        kz_sb_put_cp(sb, c);
        return next;
    }
}

static int lex_string(Lexer *lx, uint32_t start) {
    JSContext *ctx = lx->ctx;
    uint8_t quote = lx->src[start];
    uint32_t pos = start + 1;
    KzStrBuf sb;
    kz_sb_init(&sb, ctx);
    bool legacy = false, bad = false;
    for (;;) {
        if (pos >= lx->len) { kz_sb_free(&sb); kz_lex_error(lx, start, "Invalid or unexpected token"); return -1; }
        uint8_t b = lx->src[pos];
        if (b == quote) { pos++; break; }
        if (b == '\\') {
            uint32_t q = lex_escape(lx, pos + 1, &sb, false, &bad, &legacy);
            if (!q) { kz_sb_free(&sb); return -1; }
            pos = q;
            continue;
        }
        if (b == '\n' || b == '\r') { kz_sb_free(&sb); kz_lex_error(lx, start, "Invalid or unexpected token"); return -1; }
        if (b < 0x80) {
            /* a run of plain bytes */
            uint32_t q = pos;
            while (q < lx->len && lx->src[q] < 0x80 && lx->src[q] != quote && lx->src[q] != '\\' && lx->src[q] != '\n' && lx->src[q] != '\r') q++;
            kz_sb_put_latin1(&sb, lx->src + pos, q - pos);
            pos = q;
            continue;
        }
        uint32_t next, c = peek_cp(lx, pos, &next);
        kz_sb_put_cp(&sb, c);         /* LS and PS are allowed in strings */
        pos = next;
    }
    JSValue v = kz_sb_finish(&sb);
    if (v == JS_EXCEPTION || kz_parser_keep(lx->p, v) == JS_EXCEPTION) { if (lx->p) lx->p->failed = true; return -1; }
    lx->tok.type = T_STRING;
    lx->tok.val = v;
    lx->tok.legacy_octal = legacy;
    lx->pos = pos;
    return 0;
}

/* a template chunk starting at pos (after ` or }) */
static int lex_template_chunk(Lexer *lx, uint32_t pos) {
    JSContext *ctx = lx->ctx;
    uint32_t start = pos;
    KzStrBuf cooked, raw;
    kz_sb_init(&cooked, ctx);
    kz_sb_init(&raw, ctx);
    bool bad = false, legacy = false, tail;
    for (;;) {
        if (pos >= lx->len) {
            kz_sb_free(&cooked);
            kz_sb_free(&raw);
            kz_lex_error(lx, start, "Unterminated template literal");
            return -1;
        }
        uint8_t b = lx->src[pos];
        if (b == '`') { pos++; tail = true; break; }
        if (b == '$' && pos + 1 < lx->len && lx->src[pos + 1] == '{') { pos += 2; tail = false; break; }
        if (b == '\\') {
            uint32_t q = lex_escape(lx, pos + 1, &cooked, true, &bad, &legacy);
            if (!q) { kz_sb_free(&cooked); kz_sb_free(&raw); return -1; }
            /* raw: the source text, with CR LF and CR as LF */
            for (uint32_t i = pos; i < q; i++) {
                uint8_t r = lx->src[i];
                if (r == '\r') { kz_sb_putc(&raw, '\n'); if (i + 1 < q && lx->src[i + 1] == '\n') i++; continue; }
                if (r < 0x80) kz_sb_putc(&raw, r);
                else {
                    size_t j = i;
                    uint32_t cp = kz_utf8_decode(lx->src, lx->len, &j);
                    kz_sb_put_cp(&raw, cp);
                    i = (uint32_t)j - 1;
                }
            }
            pos = q;
            continue;
        }
        if (b == '\r') {
            pos++;
            if (pos < lx->len && lx->src[pos] == '\n') pos++;
            new_line(lx, pos);
            kz_sb_putc(&cooked, '\n');
            kz_sb_putc(&raw, '\n');
            continue;
        }
        uint32_t next, c = peek_cp(lx, pos, &next);
        if (c == '\n' || c == 0x2028 || c == 0x2029) new_line(lx, next);
        kz_sb_put_cp(&cooked, c);
        kz_sb_put_cp(&raw, c);
        pos = next;
    }
    JSValue cv = kz_sb_finish(&cooked);
    if (cv == JS_EXCEPTION || kz_parser_keep(lx->p, cv) == JS_EXCEPTION) { kz_sb_free(&raw); if (lx->p) lx->p->failed = true; return -1; }
    JSValue rv = kz_sb_finish(&raw);
    if (rv == JS_EXCEPTION || kz_parser_keep(lx->p, rv) == JS_EXCEPTION) { if (lx->p) lx->p->failed = true; return -1; }
    lx->tok.type = T_TEMPLATE;
    lx->tok.val = bad ? JS_UNDEFINED : cv;
    lx->tok.raw = rv;
    lx->tok.tail = tail;
    lx->tok.legacy_octal = bad;
    lx->pos = pos;
    return 0;
}

int kz_lex_template_continue(Lexer *lx) {
    /* the current token is the '}' that closes a substitution */
    Token *t = &lx->tok;
    uint32_t start = t->pos;
    t->nl_before = false;
    int r = lex_template_chunk(lx, start + 1);
    t->pos = start;
    t->end = lx->pos;
    return r;
}

/* ------------------------------------------------------------------ regular expressions */

int kz_lex_regexp(Lexer *lx) {
    JSContext *ctx = lx->ctx;
    Token *t = &lx->tok;
    uint32_t start = t->pos, pos = start + 1;
    bool in_class = false;
    for (;;) {
        if (pos >= lx->len) { kz_lex_error(lx, start, "Invalid regular expression: missing /"); return -1; }
        uint32_t next, c = peek_cp(lx, pos, &next);
        if (is_line_term(c)) { kz_lex_error(lx, start, "Invalid regular expression: missing /"); return -1; }
        if (c == '\\') {
            uint32_t n2, d = peek_cp(lx, next, &n2);
            if (d == 0xFFFFFFFFu || is_line_term(d)) { kz_lex_error(lx, start, "Invalid regular expression: missing /"); return -1; }
            pos = n2;
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) break;
        pos = next;
    }
    JSValue body = kz_str_from_utf8(ctx, (const char *)lx->src + start + 1, pos - start - 1);
    if (body == JS_EXCEPTION || kz_parser_keep(lx->p, body) == JS_EXCEPTION) { if (lx->p) lx->p->failed = true; return -1; }
    pos++;
    uint32_t fstart = pos;
    for (;;) {
        uint32_t next, c = peek_cp(lx, pos, &next);
        if (c == '\\') { kz_lex_error(lx, pos, "Invalid regular expression flags"); return -1; }
        if (c == 0xFFFFFFFFu || !id_continue(c)) break;
        pos = next;
    }
    JSValue flags = kz_str_from_utf8(ctx, (const char *)lx->src + fstart, pos - fstart);
    if (flags == JS_EXCEPTION || kz_parser_keep(lx->p, flags) == JS_EXCEPTION) { if (lx->p) lx->p->failed = true; return -1; }
    t->type = T_REGEXP;
    t->val = body;
    t->raw = flags;
    t->end = pos;
    lx->pos = pos;
    return 0;
}

/* ------------------------------------------------------------------ the main entry */

int kz_lex_next(Lexer *lx) {
    Token *t = &lx->tok;
    lx->prev_end = t->end;
    lx->prev_line = lx->line;
    bool nl = false;
    if (lx->pos == 0 && lx->len >= 2 && lx->src[0] == '#' && lx->src[1] == '!') {
        /* a hashbang comment (ES2023) */
        uint32_t pos = 2;
        while (pos < lx->len) {
            uint32_t next, c = peek_cp(lx, pos, &next);
            if (is_line_term(c)) break;
            pos = next;
        }
        lx->pos = pos;
    }
    if (skip_space(lx, &nl)) return -1;
    t->nl_before = nl;
    t->escaped = false;
    t->legacy_octal = false;
    t->kw = KW_NONE;
    t->val = JS_UNDEFINED;
    t->raw = JS_UNDEFINED;
    uint32_t pos = lx->pos;
    t->pos = pos;
    t->col = kz_lex_col(lx, pos, &t->line);
    if (pos >= lx->len) { t->type = T_EOF; t->end = pos; return 0; }
    const uint8_t *s = lx->src;
    uint8_t c = s[pos];
    uint32_t n = lx->len;
#define AT(k) (pos + (k) < n ? s[pos + (k)] : 0)
#define TOK(ty, w) do { t->type = (ty); lx->pos = pos + (w); goto out; } while (0)
    switch (c) {
    case '{': TOK(T_LBRACE, 1);
    case '}': TOK(T_RBRACE, 1);
    case '(': TOK(T_LPAREN, 1);
    case ')': TOK(T_RPAREN, 1);
    case '[': TOK(T_LBRACKET, 1);
    case ']': TOK(T_RBRACKET, 1);
    case ';': TOK(T_SEMI, 1);
    case ',': TOK(T_COMMA, 1);
    case ':': TOK(T_COLON, 1);
    case '~': TOK(T_TILDE, 1);
    case '@': TOK(T_AT, 1);
    case '.':
        if (AT(1) >= '0' && AT(1) <= '9') { if (lex_number(lx, pos)) return -1; goto out_num; }
        if (AT(1) == '.' && AT(2) == '.') TOK(T_ELLIPSIS, 3);
        TOK(T_DOT, 1);
    case '<':
        if (AT(1) == '<') { if (AT(2) == '=') TOK(T_SHL_ASSIGN, 3); TOK(T_SHL, 2); }
        if (AT(1) == '=') TOK(T_LE, 2);
        TOK(T_LT, 1);
    case '>':
        if (AT(1) == '>') {
            if (AT(2) == '>') { if (AT(3) == '=') TOK(T_SHR_ASSIGN, 4); TOK(T_SHR, 3); }
            if (AT(2) == '=') TOK(T_SAR_ASSIGN, 3);
            TOK(T_SAR, 2);
        }
        if (AT(1) == '=') TOK(T_GE, 2);
        TOK(T_GT, 1);
    case '=':
        if (AT(1) == '=') { if (AT(2) == '=') TOK(T_SEQ, 3); TOK(T_EQ, 2); }
        if (AT(1) == '>') TOK(T_ARROW, 2);
        TOK(T_ASSIGN, 1);
    case '!':
        if (AT(1) == '=') { if (AT(2) == '=') TOK(T_SNE, 3); TOK(T_NE, 2); }
        TOK(T_BANG, 1);
    case '+':
        if (AT(1) == '+') TOK(T_INC, 2);
        if (AT(1) == '=') TOK(T_PLUS_ASSIGN, 2);
        TOK(T_PLUS, 1);
    case '-':
        if (AT(1) == '-') TOK(T_DEC, 2);
        if (AT(1) == '=') TOK(T_MINUS_ASSIGN, 2);
        TOK(T_MINUS, 1);
    case '*':
        if (AT(1) == '*') { if (AT(2) == '=') TOK(T_STARSTAR_ASSIGN, 3); TOK(T_STARSTAR, 2); }
        if (AT(1) == '=') TOK(T_STAR_ASSIGN, 2);
        TOK(T_STAR, 1);
    case '/':
        if (AT(1) == '=') TOK(T_SLASH_ASSIGN, 2);
        TOK(T_SLASH, 1);
    case '%':
        if (AT(1) == '=') TOK(T_PERCENT_ASSIGN, 2);
        TOK(T_PERCENT, 1);
    case '&':
        if (AT(1) == '&') { if (AT(2) == '=') TOK(T_ANDAND_ASSIGN, 3); TOK(T_ANDAND, 2); }
        if (AT(1) == '=') TOK(T_AMP_ASSIGN, 2);
        TOK(T_AMP, 1);
    case '|':
        if (AT(1) == '|') { if (AT(2) == '=') TOK(T_OROR_ASSIGN, 3); TOK(T_OROR, 2); }
        if (AT(1) == '=') TOK(T_PIPE_ASSIGN, 2);
        TOK(T_PIPE, 1);
    case '^':
        if (AT(1) == '=') TOK(T_CARET_ASSIGN, 2);
        TOK(T_CARET, 1);
    case '?':
        if (AT(1) == '?') { if (AT(2) == '=') TOK(T_QQ_ASSIGN, 3); TOK(T_QQ, 2); }
        if (AT(1) == '.' && !(AT(2) >= '0' && AT(2) <= '9')) TOK(T_QDOT, 2);
        TOK(T_QUESTION, 1);
    case '\'': case '"':
        if (lex_string(lx, pos)) return -1;
        goto out_set;
    case '`':
        if (lex_template_chunk(lx, pos + 1)) return -1;
        goto out_set;
    case '#': {
        uint32_t nx, d = peek_cp(lx, pos + 1, &nx);
        if (d == '\\' || (d != 0xFFFFFFFFu && id_start(d))) {
            if (lex_name(lx, pos + 1, true)) return -1;
            goto out_set;
        }
        kz_lex_error(lx, pos, "Invalid or unexpected token");
        return -1;
    }
    default:
        if (c >= '0' && c <= '9') { if (lex_number(lx, pos)) return -1; goto out_num; }
        {
            uint32_t nx, cp = peek_cp(lx, pos, &nx);
            if (cp == '\\' || id_start(cp)) {
                if (lex_name(lx, pos, false)) return -1;
                goto out_set;
            }
        }
        kz_lex_error(lx, pos, "Invalid or unexpected token");
        return -1;
    }
#undef AT
#undef TOK
out_num:
out_set:
out:
    t->end = lx->pos;
    return 0;
}
