/*
 * The HTML tokenizer: the state machine of the WHATWG HTML standard, section 13.2.5 (all 84 states, including the
 * processing instruction states added in 2025), emitting tokens to a sink (the tree builder, tree.c, or a test recorder).
 *
 * Input is UTF-8 that the decoder (encoding.c) has already normalized (CR LF and lone CR became LF). The machine looks at
 * one byte at a time: every character with a special meaning is ASCII, and the bytes of a multi-byte character always fall
 * into "anything else", so copying them one by one rebuilds the same character; this is exactly the code-point machine of
 * the specification, without decoding. Runs of ordinary text are passed on as one character token (a pointer into the
 * input), which the tree builder appends to a Text node in one copy.
 *
 * The tokenizer can stop at the end of the available input in any state and continue when more arrives (incremental
 * parsing, document.write): the states that look ahead several characters (markup declarations, DOCTYPE keywords, named
 * character references) wait until enough input is there or the end of the stream is known.
 */
#include "webint.h"

#define EOF_C (-1)

const char *const web_parse_error_names[] = {
    "",
#define E(id, s) s,
#include "errlist.h"
#undef E
};

static inline bool is_alpha(int c) { return c >= 0 && (unsigned)((c | 0x20) - 'a') < 26; }
static inline bool is_upper(int c) { return (unsigned)(c - 'A') < 26; }
static inline bool is_digit(int c) { return (unsigned)(c - '0') < 10; }
static inline bool is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static inline bool is_hex(int c) { return is_digit(c) || (c >= 0 && (unsigned)((c | 0x20) - 'a') < 6); }
static inline bool is_ws(int c) { return c == '\t' || c == '\n' || c == '\f' || c == ' '; }
static inline char lower(int c) { return (char)(is_upper(c) ? c + 32 : c); }

static const char FFFD[] = "\xEF\xBF\xBD";

static void tok_err(HtmlTokenizer *t, int code) {
    if (t->error) t->error(t->sink_user, code, t->pos ? t->pos - 1 : 0);
}

void tok_init(HtmlTokenizer *t, WebDoc *d, WebArena *a) {
    memset(t, 0, sizeof *t);
    t->doc = d;
    t->a = a;
    t->state = TS_DATA;
    buf_init(&t->tagname, a);
    buf_init(&t->name, a);
    buf_init(&t->val, a);
    buf_init(&t->data, a);
    buf_init(&t->tmp, a);
    buf_init(&t->dtpub, a);
    buf_init(&t->dtsys, a);
}

void tok_set_input(HtmlTokenizer *t, const char *in, size_t n, bool eof) {
    t->in = in;
    t->n = n;
    t->eof = eof;
}

void tok_set_last_start(HtmlTokenizer *t, const char *name, size_t n) {
    if (n >= sizeof t->last_start) n = sizeof t->last_start - 1;
    if (n) memcpy(t->last_start, name, n);
    t->last_start[n] = 0;
    t->last_start_len = (uint32_t)n;
}

/* ------------------------------------------------------------------------------------------------ emitting */

static void emit_chars(HtmlTokenizer *t, const char *p, size_t n) {
    if (!n) return;
    HtmlToken k;
    memset(&k, 0, sizeof k);
    k.type = HT_CHARS;
    k.data = p;
    k.len = (uint32_t)n;
    k.offset = t->pos;
    t->sink(t->sink_user, &k);
}

static void emit_c(HtmlTokenizer *t, int c) {
    char ch = (char)c;
    emit_chars(t, &ch, 1);
}

static void emit_fffd(HtmlTokenizer *t) {
    emit_chars(t, FFFD, 3);
}

static void emit_eof(HtmlTokenizer *t) {
    HtmlToken k;
    memset(&k, 0, sizeof k);
    k.type = HT_EOF;
    k.offset = t->pos;
    t->stop = true;
    t->sink(t->sink_user, &k);
}

static void new_tag(HtmlTokenizer *t, bool end) {
    buf_clear(&t->tagname);
    buf_clear(&t->name);
    buf_clear(&t->val);
    t->attrs.n = 0;
    t->drop_attr = false;
    t->is_end = end;
    t->tok.self_closing = false;
    t->tok_start = t->pos ? t->pos - 1 : 0;
}

/* Leaving the attribute name state: the name is complete, and a second attribute of the same name is dropped (with the
 * value that follows it). Names are never empty: the state that starts an attribute always appends its first character. */
static void attr_name_done(HtmlTokenizer *t) {
    if (!t->attrs.n || t->drop_attr) return;
    HtmlAttr *a = &t->attrs.v[t->attrs.n - 1];
    if (a->name != AT_NONE) return;
    WebAtom name = atom_get(&t->doc->atoms, t->name.p ? t->name.p : "", t->name.n);
    for (uint32_t i = 0; i + 1 < t->attrs.n; i++)
        if (t->attrs.v[i].name == name) {
            tok_err(t, PE_DUPLICATE_ATTRIBUTE);
            t->drop_attr = true;
            return;
        }
    a->name = name;
}

static void attr_finish(HtmlTokenizer *t) {
    if (!t->attrs.n) return;
    attr_name_done(t);
    HtmlAttr *a = &t->attrs.v[t->attrs.n - 1];
    if (t->drop_attr || a->name == AT_NONE) {
        t->val.n = a->voff;
        t->attrs.n--;
        t->drop_attr = false;
        return;
    }
    a->vlen = t->val.n - a->voff;
}

static void new_attr(HtmlTokenizer *t) {
    attr_finish(t);
    HtmlAttr a = { AT_NONE, t->val.n, 0 };
    buf_clear(&t->name);
    if (!vec_push(t->a, t->attrs, a)) t->drop_attr = true;
}

static void emit_tag(HtmlTokenizer *t) {
    attr_finish(t);
    HtmlToken *k = &t->tok;
    bool sc = k->self_closing;
    memset(k, 0, sizeof *k);
    k->type = t->is_end ? HT_END : HT_START;
    k->self_closing = sc;
    k->name = atom_get(&t->doc->atoms, t->tagname.p ? t->tagname.p : "", t->tagname.n);
    k->attrs = t->attrs.v;
    k->nattrs = (int)t->attrs.n;
    k->vals = t->val.p ? t->val.p : "";
    k->offset = t->tok_start;
    if (k->type == HT_START) {
        tok_set_last_start(t, t->tagname.p, t->tagname.n);
    } else {
        if (k->nattrs) tok_err(t, PE_END_TAG_WITH_ATTRIBUTES);
        if (sc) tok_err(t, PE_END_TAG_WITH_TRAILING_SOLIDUS);
    }
    t->sink(t->sink_user, k);
    if (k->type == HT_START && sc && !k->ack) tok_err(t, PE_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS);
}

static void new_comment(HtmlTokenizer *t) {
    buf_clear(&t->data);
    t->tok_start = t->pos;
}

static void emit_comment(HtmlTokenizer *t) {
    HtmlToken *k = &t->tok;
    memset(k, 0, sizeof *k);
    k->type = HT_COMMENT;
    k->data = t->data.p ? t->data.p : "";
    k->len = t->data.n;
    k->offset = t->tok_start;
    t->sink(t->sink_user, k);
}

static void new_doctype(HtmlTokenizer *t) {
    buf_clear(&t->data);
    buf_clear(&t->dtpub);
    buf_clear(&t->dtsys);
    memset(&t->tok, 0, sizeof t->tok);
    t->tok.dt_name_missing = t->tok.dt_pub_missing = t->tok.dt_sys_missing = true;
    t->tok_start = t->pos;
}

static void emit_doctype(HtmlTokenizer *t) {
    HtmlToken *k = &t->tok;
    k->type = HT_DOCTYPE;
    k->data = t->data.p ? t->data.p : "";
    k->len = t->data.n;
    k->dt_pub = t->dtpub.p ? t->dtpub.p : "";
    k->dt_pub_len = t->dtpub.n;
    k->dt_sys = t->dtsys.p ? t->dtsys.p : "";
    k->dt_sys_len = t->dtsys.n;
    k->offset = t->tok_start;
    t->sink(t->sink_user, k);
}

static void emit_pi(HtmlTokenizer *t) {
    HtmlToken *k = &t->tok;
    memset(k, 0, sizeof *k);
    k->type = HT_PI;
    k->name = atom_get(&t->doc->atoms, t->tmp.p ? t->tmp.p : "", t->tmp.n);
    k->data = t->data.p ? t->data.p : "";
    k->len = t->data.n;
    k->offset = t->tok_start;
    t->sink(t->sink_user, k);
}

/* "Convert the temporary buffer to a comment": "?" followed by the buffer. */
static void tmp_to_comment(HtmlTokenizer *t) {
    buf_clear(&t->data);
    buf_putc(&t->data, '?');
    if (t->tmp.n) buf_put(&t->data, t->tmp.p, t->tmp.n);
}

static bool appropriate_end(const HtmlTokenizer *t) {
    return t->last_start_len && t->tagname.n == t->last_start_len && memcmp(t->tagname.p, t->last_start, t->last_start_len) == 0;
}

static bool in_attr(TokState s) {
    return s == TS_ATTR_VALUE_DQ || s == TS_ATTR_VALUE_SQ || s == TS_ATTR_VALUE_UQ;
}

static void flush_charref(HtmlTokenizer *t) {
    if (!t->tmp.n) return;
    if (in_attr(t->ret_state)) buf_put(&t->val, t->tmp.p, t->tmp.n);
    else emit_chars(t, t->tmp.p, t->tmp.n);
}

/* Lookahead: does the input at pos start with s (case-insensitively for ASCII letters when ci)? */
static bool looking_at(const HtmlTokenizer *t, const char *s, bool ci) {
    size_t n = strlen(s);
    if (t->n - t->pos < n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = t->in[t->pos + i];
        if (ci ? lower((unsigned char)c) != s[i] : c != s[i]) return false;
    }
    return true;
}

/* The longest named character reference at the input position: candidates are narrowed one character at a time with two
 * binary searches over the sorted table (all candidates share the characters consumed so far, so the k-th characters of the
 * candidates are in order, with the name that ends there first). Returns 1 and the match, 0 for none, -1 when the input
 * ends while names are still possible and more input may come. */
static int key_at(int i, size_t k) {
    const WebEntity *e = &web_entities[i];
    return e->len > k ? (unsigned char)web_entity_names[e->off + k] : -1;
}

static int match_entity(const HtmlTokenizer *t, size_t *mlen, int *mi) {
    int lo = 0, hi = web_entity_count, best = -1;
    size_t k = 0, best_len = 0;
    for (;;) {
        if (t->pos + k >= t->n) {
            if (!t->eof) return -1;
            break;
        }
        int c = (unsigned char)t->in[t->pos + k];
        int l = lo, h = hi;
        while (l < h) { int m = (l + h) >> 1; if (key_at(m, k) < c) l = m + 1; else h = m; }
        int first = l;
        h = hi;
        while (l < h) { int m = (l + h) >> 1; if (key_at(m, k) <= c) l = m + 1; else h = m; }
        if (first == l) break;
        lo = first;
        hi = l;
        k++;
        if (web_entities[lo].len == k) { best = lo; best_len = k; }
    }
    if (best < 0) return 0;
    *mlen = best_len;
    *mi = best;
    return 1;
}

/* Replacements for numeric references to the C1 range (windows-1252 compatibility), 0x80..0x9F; 0 = keep */
static const uint16_t c1_table[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static bool is_noncharacter(uint32_t c) {
    return (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE;
}

/* Fast paths: the length of a run of bytes none of which is in `stop` (a NUL-free set of ASCII bytes plus NUL itself). */
static size_t run_until(const char *p, size_t n, const char *stop) {
    size_t i = 0;
    while (i < n) {
        char c = p[i];
        if (c == 0 || strchr(stop, c)) break;
        i++;
    }
    return i;
}

/* ------------------------------------------------------------------------------------------------ the state machine */

#define GETC() do { if (t->pos < t->n) c = (unsigned char)t->in[t->pos++]; \
                    else if (t->eof) c = EOF_C; else return 0; } while (0)
#define RECON(s) do { if (c != EOF_C) t->pos--; t->state = (s); } while (0)
#define SWITCH(s) do { t->state = (s); } while (0)

/* The three "end tag name" states of RCDATA, RAWTEXT and script data (and the escaped one) share their rules. */
static void end_tag_name(HtmlTokenizer *t, int c, TokState text_state) {
    if (is_ws(c) && appropriate_end(t)) { t->state = TS_BEFORE_ATTR_NAME; return; }
    if (c == '/' && appropriate_end(t)) { t->state = TS_SELF_CLOSING; return; }
    if (c == '>' && appropriate_end(t)) { t->state = TS_DATA; emit_tag(t); return; }
    if (is_alpha(c)) {
        buf_putc(&t->tagname, lower(c));
        buf_putc(&t->tmp, (char)c);
        return;
    }
    emit_chars(t, "</", 2);
    emit_chars(t, t->tmp.p, t->tmp.n);
    if (c != EOF_C) t->pos--;
    t->state = text_state;
}

int tok_run(HtmlTokenizer *t) {
    int c;
    for (;;) {
        if (t->stop) return 0;
        if (t->paused) return 1;
        switch (t->state) {
        /* ---------------------------------------------------------------- text states */
        case TS_DATA: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "<&");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == '&') { t->ret_state = TS_DATA; SWITCH(TS_CHARREF); }
            else if (c == '<') SWITCH(TS_TAG_OPEN);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_c(t, 0); }
            else if (c == EOF_C) emit_eof(t);
            else emit_c(t, c);
            break;
        }
        case TS_RCDATA: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "<&");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == '&') { t->ret_state = TS_RCDATA; SWITCH(TS_CHARREF); }
            else if (c == '<') SWITCH(TS_RCDATA_LT);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_fffd(t); }
            else if (c == EOF_C) emit_eof(t);
            else emit_c(t, c);
            break;
        }
        case TS_RAWTEXT:
        case TS_SCRIPT_DATA: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "<");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == '<') SWITCH(t->state == TS_RAWTEXT ? TS_RAWTEXT_LT : TS_SCRIPT_LT);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_fffd(t); }
            else if (c == EOF_C) emit_eof(t);
            else emit_c(t, c);
            break;
        }
        case TS_PLAINTEXT: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_fffd(t); }
            else if (c == EOF_C) emit_eof(t);
            else emit_c(t, c);
            break;
        }
        /* ---------------------------------------------------------------- tags */
        case TS_TAG_OPEN:
            GETC();
            if (c == '!') SWITCH(TS_MARKUP_DECL);
            else if (c == '/') SWITCH(TS_END_TAG_OPEN);
            else if (is_alpha(c)) { new_tag(t, false); RECON(TS_TAG_NAME); }
            else if (c == '?') { buf_clear(&t->tmp); t->tok_start = t->pos - 2; SWITCH(TS_PI_OPEN); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_BEFORE_TAG_NAME); emit_c(t, '<'); emit_eof(t); }
            else { tok_err(t, PE_INVALID_FIRST_CHARACTER_OF_TAG_NAME); emit_c(t, '<'); RECON(TS_DATA); }
            break;
        case TS_END_TAG_OPEN:
            GETC();
            if (is_alpha(c)) { new_tag(t, true); RECON(TS_TAG_NAME); }
            else if (c == '>') { tok_err(t, PE_MISSING_END_TAG_NAME); SWITCH(TS_DATA); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_BEFORE_TAG_NAME); emit_chars(t, "</", 2); emit_eof(t); }
            else { tok_err(t, PE_INVALID_FIRST_CHARACTER_OF_TAG_NAME); new_comment(t); RECON(TS_BOGUS_COMMENT); }
            break;
        case TS_TAG_NAME:
            GETC();
            if (is_ws(c)) SWITCH(TS_BEFORE_ATTR_NAME);
            else if (c == '/') SWITCH(TS_SELF_CLOSING);
            else if (c == '>') { SWITCH(TS_DATA); emit_tag(t); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->tagname, FFFD, 3); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else buf_putc(&t->tagname, lower(c));
            break;
        /* ---------------------------------------------------------------- RCDATA, RAWTEXT, script end tags */
        case TS_RCDATA_LT:
            GETC();
            if (c == '/') { buf_clear(&t->tmp); SWITCH(TS_RCDATA_END_OPEN); }
            else { emit_c(t, '<'); RECON(TS_RCDATA); }
            break;
        case TS_RCDATA_END_OPEN:
            GETC();
            if (is_alpha(c)) { new_tag(t, true); RECON(TS_RCDATA_END_NAME); }
            else { emit_chars(t, "</", 2); RECON(TS_RCDATA); }
            break;
        case TS_RCDATA_END_NAME:
            GETC();
            end_tag_name(t, c, TS_RCDATA);
            break;
        case TS_RAWTEXT_LT:
            GETC();
            if (c == '/') { buf_clear(&t->tmp); SWITCH(TS_RAWTEXT_END_OPEN); }
            else { emit_c(t, '<'); RECON(TS_RAWTEXT); }
            break;
        case TS_RAWTEXT_END_OPEN:
            GETC();
            if (is_alpha(c)) { new_tag(t, true); RECON(TS_RAWTEXT_END_NAME); }
            else { emit_chars(t, "</", 2); RECON(TS_RAWTEXT); }
            break;
        case TS_RAWTEXT_END_NAME:
            GETC();
            end_tag_name(t, c, TS_RAWTEXT);
            break;
        case TS_SCRIPT_LT:
            GETC();
            if (c == '/') { buf_clear(&t->tmp); SWITCH(TS_SCRIPT_END_OPEN); }
            else if (c == '!') { SWITCH(TS_SCRIPT_ESC_START); emit_chars(t, "<!", 2); }
            else { emit_c(t, '<'); RECON(TS_SCRIPT_DATA); }
            break;
        case TS_SCRIPT_END_OPEN:
            GETC();
            if (is_alpha(c)) { new_tag(t, true); RECON(TS_SCRIPT_END_NAME); }
            else { emit_chars(t, "</", 2); RECON(TS_SCRIPT_DATA); }
            break;
        case TS_SCRIPT_END_NAME:
            GETC();
            end_tag_name(t, c, TS_SCRIPT_DATA);
            break;
        case TS_SCRIPT_ESC_START:
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_ESC_START_DASH); emit_c(t, '-'); }
            else RECON(TS_SCRIPT_DATA);
            break;
        case TS_SCRIPT_ESC_START_DASH:
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_ESC_DASH_DASH); emit_c(t, '-'); }
            else RECON(TS_SCRIPT_DATA);
            break;
        case TS_SCRIPT_ESC: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "-<");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_ESC_DASH); emit_c(t, '-'); }
            else if (c == '<') SWITCH(TS_SCRIPT_ESC_LT);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else emit_c(t, c);
            break;
        }
        case TS_SCRIPT_ESC_DASH:
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_ESC_DASH_DASH); emit_c(t, '-'); }
            else if (c == '<') SWITCH(TS_SCRIPT_ESC_LT);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); SWITCH(TS_SCRIPT_ESC); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else { SWITCH(TS_SCRIPT_ESC); emit_c(t, c); }
            break;
        case TS_SCRIPT_ESC_DASH_DASH:
            GETC();
            if (c == '-') emit_c(t, '-');
            else if (c == '<') SWITCH(TS_SCRIPT_ESC_LT);
            else if (c == '>') { SWITCH(TS_SCRIPT_DATA); emit_c(t, '>'); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); SWITCH(TS_SCRIPT_ESC); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else { SWITCH(TS_SCRIPT_ESC); emit_c(t, c); }
            break;
        case TS_SCRIPT_ESC_LT:
            GETC();
            if (c == '/') { buf_clear(&t->tmp); SWITCH(TS_SCRIPT_ESC_END_OPEN); }
            else if (is_alpha(c)) { buf_clear(&t->tmp); emit_c(t, '<'); RECON(TS_SCRIPT_DESC_START); }
            else { emit_c(t, '<'); RECON(TS_SCRIPT_ESC); }
            break;
        case TS_SCRIPT_ESC_END_OPEN:
            GETC();
            if (is_alpha(c)) { new_tag(t, true); RECON(TS_SCRIPT_ESC_END_NAME); }
            else { emit_chars(t, "</", 2); RECON(TS_SCRIPT_ESC); }
            break;
        case TS_SCRIPT_ESC_END_NAME:
            GETC();
            end_tag_name(t, c, TS_SCRIPT_ESC);
            break;
        case TS_SCRIPT_DESC_START:
        case TS_SCRIPT_DESC_END: {
            GETC();
            bool start = t->state == TS_SCRIPT_DESC_START;
            if (is_ws(c) || c == '/' || c == '>') {
                bool script = t->tmp.n == 6 && memcmp(t->tmp.p, "script", 6) == 0;
                if (start) SWITCH(script ? TS_SCRIPT_DESC : TS_SCRIPT_ESC);
                else SWITCH(script ? TS_SCRIPT_ESC : TS_SCRIPT_DESC);
                emit_c(t, c);
            } else if (is_alpha(c)) {
                buf_putc(&t->tmp, lower(c));
                emit_c(t, c);
            } else RECON(start ? TS_SCRIPT_ESC : TS_SCRIPT_DESC);
            break;
        }
        case TS_SCRIPT_DESC: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "-<");
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_DESC_DASH); emit_c(t, '-'); }
            else if (c == '<') { SWITCH(TS_SCRIPT_DESC_LT); emit_c(t, '<'); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else emit_c(t, c);
            break;
        }
        case TS_SCRIPT_DESC_DASH:
            GETC();
            if (c == '-') { SWITCH(TS_SCRIPT_DESC_DASH_DASH); emit_c(t, '-'); }
            else if (c == '<') { SWITCH(TS_SCRIPT_DESC_LT); emit_c(t, '<'); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); SWITCH(TS_SCRIPT_DESC); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else { SWITCH(TS_SCRIPT_DESC); emit_c(t, c); }
            break;
        case TS_SCRIPT_DESC_DASH_DASH:
            GETC();
            if (c == '-') emit_c(t, '-');
            else if (c == '<') { SWITCH(TS_SCRIPT_DESC_LT); emit_c(t, '<'); }
            else if (c == '>') { SWITCH(TS_SCRIPT_DATA); emit_c(t, '>'); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); SWITCH(TS_SCRIPT_DESC); emit_fffd(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_SCRIPT_HTML_COMMENT_LIKE_TEXT); emit_eof(t); }
            else { SWITCH(TS_SCRIPT_DESC); emit_c(t, c); }
            break;
        case TS_SCRIPT_DESC_LT:
            GETC();
            if (c == '/') { buf_clear(&t->tmp); SWITCH(TS_SCRIPT_DESC_END); emit_c(t, '/'); }
            else RECON(TS_SCRIPT_DESC);
            break;
        /* ---------------------------------------------------------------- attributes */
        case TS_BEFORE_ATTR_NAME:
            GETC();
            if (is_ws(c)) break;
            if (c == '/' || c == '>' || c == EOF_C) RECON(TS_AFTER_ATTR_NAME);
            else if (c == '=') {
                tok_err(t, PE_UNEXPECTED_EQUALS_SIGN_BEFORE_ATTRIBUTE_NAME);
                new_attr(t);
                buf_putc(&t->name, '=');
                SWITCH(TS_ATTR_NAME);
            } else { new_attr(t); RECON(TS_ATTR_NAME); }
            break;
        case TS_ATTR_NAME:
            GETC();
            if (is_ws(c) || c == '/' || c == '>' || c == EOF_C) { attr_name_done(t); RECON(TS_AFTER_ATTR_NAME); }
            else if (c == '=') { attr_name_done(t); SWITCH(TS_BEFORE_ATTR_VALUE); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->name, FFFD, 3); }
            else {
                if (c == '"' || c == '\'' || c == '<') tok_err(t, PE_UNEXPECTED_CHARACTER_IN_ATTRIBUTE_NAME);
                buf_putc(&t->name, lower(c));
            }
            break;
        case TS_AFTER_ATTR_NAME:
            GETC();
            if (is_ws(c)) break;
            if (c == '/') SWITCH(TS_SELF_CLOSING);
            else if (c == '=') SWITCH(TS_BEFORE_ATTR_VALUE);
            else if (c == '>') { SWITCH(TS_DATA); emit_tag(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else { new_attr(t); RECON(TS_ATTR_NAME); }
            break;
        case TS_BEFORE_ATTR_VALUE:
            GETC();
            if (is_ws(c)) break;
            if (c == '"') SWITCH(TS_ATTR_VALUE_DQ);
            else if (c == '\'') SWITCH(TS_ATTR_VALUE_SQ);
            else if (c == '>') { tok_err(t, PE_MISSING_ATTRIBUTE_VALUE); SWITCH(TS_DATA); emit_tag(t); }
            else RECON(TS_ATTR_VALUE_UQ);
            break;
        case TS_ATTR_VALUE_DQ:
        case TS_ATTR_VALUE_SQ: {
            char q = t->state == TS_ATTR_VALUE_DQ ? '"' : '\'';
            const char stop[3] = { q, '&', 0 };
            size_t r = run_until(t->in + t->pos, t->n - t->pos, stop);
            if (r) { buf_put(&t->val, t->in + t->pos, r); t->pos += r; }
            GETC();
            if (c == q) SWITCH(TS_AFTER_ATTR_VALUE_Q);
            else if (c == '&') { t->ret_state = t->state; SWITCH(TS_CHARREF); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->val, FFFD, 3); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else buf_putc(&t->val, (char)c);
            break;
        }
        case TS_ATTR_VALUE_UQ:
            GETC();
            if (is_ws(c)) SWITCH(TS_BEFORE_ATTR_NAME);
            else if (c == '&') { t->ret_state = TS_ATTR_VALUE_UQ; SWITCH(TS_CHARREF); }
            else if (c == '>') { SWITCH(TS_DATA); emit_tag(t); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->val, FFFD, 3); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else {
                if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') tok_err(t, PE_UNEXPECTED_CHARACTER_IN_UNQUOTED_ATTRIBUTE_VALUE);
                buf_putc(&t->val, (char)c);
            }
            break;
        case TS_AFTER_ATTR_VALUE_Q:
            GETC();
            if (is_ws(c)) SWITCH(TS_BEFORE_ATTR_NAME);
            else if (c == '/') SWITCH(TS_SELF_CLOSING);
            else if (c == '>') { SWITCH(TS_DATA); emit_tag(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else { tok_err(t, PE_MISSING_WHITESPACE_BETWEEN_ATTRIBUTES); RECON(TS_BEFORE_ATTR_NAME); }
            break;
        case TS_SELF_CLOSING:
            GETC();
            if (c == '>') { t->tok.self_closing = true; SWITCH(TS_DATA); emit_tag(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_TAG); emit_eof(t); }
            else { tok_err(t, PE_UNEXPECTED_SOLIDUS_IN_TAG); RECON(TS_BEFORE_ATTR_NAME); }
            break;
        /* ---------------------------------------------------------------- comments */
        case TS_BOGUS_COMMENT: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, ">");
            if (r) { buf_put(&t->data, t->in + t->pos, r); t->pos += r; }
            GETC();
            if (c == '>') { SWITCH(TS_DATA); emit_comment(t); }
            else if (c == EOF_C) { emit_comment(t); emit_eof(t); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->data, FFFD, 3); }
            else buf_putc(&t->data, (char)c);
            break;
        }
        case TS_MARKUP_DECL:
            if (t->n - t->pos < 7 && !t->eof) return 0;
            if (looking_at(t, "--", false)) {
                t->pos += 2;
                new_comment(t);
                SWITCH(TS_COMMENT_START);
            } else if (looking_at(t, "doctype", true)) {
                t->pos += 7;
                SWITCH(TS_DOCTYPE);
            } else if (looking_at(t, "[CDATA[", false)) {
                t->pos += 7;
                if (t->cdata_allowed && t->cdata_allowed(t->sink_user)) SWITCH(TS_CDATA);
                else {
                    tok_err(t, PE_CDATA_IN_HTML_CONTENT);
                    new_comment(t);
                    buf_puts(&t->data, "[CDATA[");
                    SWITCH(TS_BOGUS_COMMENT);
                }
            } else {
                tok_err(t, PE_INCORRECTLY_OPENED_COMMENT);
                new_comment(t);
                SWITCH(TS_BOGUS_COMMENT);
            }
            break;
        case TS_COMMENT_START:
            GETC();
            if (c == '-') SWITCH(TS_COMMENT_START_DASH);
            else if (c == '>') { tok_err(t, PE_ABRUPT_CLOSING_OF_EMPTY_COMMENT); SWITCH(TS_DATA); emit_comment(t); }
            else RECON(TS_COMMENT);
            break;
        case TS_COMMENT_START_DASH:
            GETC();
            if (c == '-') SWITCH(TS_COMMENT_END);
            else if (c == '>') { tok_err(t, PE_ABRUPT_CLOSING_OF_EMPTY_COMMENT); SWITCH(TS_DATA); emit_comment(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_COMMENT); emit_comment(t); emit_eof(t); }
            else { buf_putc(&t->data, '-'); RECON(TS_COMMENT); }
            break;
        case TS_COMMENT: {
            size_t r = run_until(t->in + t->pos, t->n - t->pos, "<-");
            if (r) { buf_put(&t->data, t->in + t->pos, r); t->pos += r; }
            GETC();
            if (c == '<') { buf_putc(&t->data, '<'); SWITCH(TS_COMMENT_LT); }
            else if (c == '-') SWITCH(TS_COMMENT_END_DASH);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->data, FFFD, 3); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_COMMENT); emit_comment(t); emit_eof(t); }
            else buf_putc(&t->data, (char)c);
            break;
        }
        case TS_COMMENT_LT:
            GETC();
            if (c == '!') { buf_putc(&t->data, '!'); SWITCH(TS_COMMENT_LT_BANG); }
            else if (c == '<') buf_putc(&t->data, '<');
            else RECON(TS_COMMENT);
            break;
        case TS_COMMENT_LT_BANG:
            GETC();
            if (c == '-') SWITCH(TS_COMMENT_LT_BANG_DASH);
            else RECON(TS_COMMENT);
            break;
        case TS_COMMENT_LT_BANG_DASH:
            GETC();
            if (c == '-') SWITCH(TS_COMMENT_LT_BANG_DASH_DASH);
            else RECON(TS_COMMENT_END_DASH);
            break;
        case TS_COMMENT_LT_BANG_DASH_DASH:
            GETC();
            if (c == '>' || c == EOF_C) RECON(TS_COMMENT_END);
            else { tok_err(t, PE_NESTED_COMMENT); RECON(TS_COMMENT_END); }
            break;
        case TS_COMMENT_END_DASH:
            GETC();
            if (c == '-') SWITCH(TS_COMMENT_END);
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_COMMENT); emit_comment(t); emit_eof(t); }
            else { buf_putc(&t->data, '-'); RECON(TS_COMMENT); }
            break;
        case TS_COMMENT_END:
            GETC();
            if (c == '>') { SWITCH(TS_DATA); emit_comment(t); }
            else if (c == '!') SWITCH(TS_COMMENT_END_BANG);
            else if (c == '-') buf_putc(&t->data, '-');
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_COMMENT); emit_comment(t); emit_eof(t); }
            else { buf_puts(&t->data, "--"); RECON(TS_COMMENT); }
            break;
        case TS_COMMENT_END_BANG:
            GETC();
            if (c == '-') { buf_puts(&t->data, "--!"); SWITCH(TS_COMMENT_END_DASH); }
            else if (c == '>') { tok_err(t, PE_INCORRECTLY_CLOSED_COMMENT); SWITCH(TS_DATA); emit_comment(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_COMMENT); emit_comment(t); emit_eof(t); }
            else { buf_puts(&t->data, "--!"); RECON(TS_COMMENT); }
            break;
        /* ---------------------------------------------------------------- DOCTYPE */
        case TS_DOCTYPE:
            GETC();
            if (is_ws(c)) SWITCH(TS_BEFORE_DOCTYPE_NAME);
            else if (c == '>') RECON(TS_BEFORE_DOCTYPE_NAME);
            else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                new_doctype(t);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else { tok_err(t, PE_MISSING_WHITESPACE_BEFORE_DOCTYPE_NAME); RECON(TS_BEFORE_DOCTYPE_NAME); }
            break;
        case TS_BEFORE_DOCTYPE_NAME:
            GETC();
            if (is_ws(c)) break;
            if (c == '>') {
                tok_err(t, PE_MISSING_DOCTYPE_NAME);
                new_doctype(t);
                t->tok.force_quirks = true;
                SWITCH(TS_DATA);
                emit_doctype(t);
            } else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                new_doctype(t);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                new_doctype(t);
                t->tok.dt_name_missing = false;
                if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->data, FFFD, 3); }
                else buf_putc(&t->data, lower(c));
                SWITCH(TS_DOCTYPE_NAME);
            }
            break;
        case TS_DOCTYPE_NAME:
            GETC();
            if (is_ws(c)) SWITCH(TS_AFTER_DOCTYPE_NAME);
            else if (c == '>') { SWITCH(TS_DATA); emit_doctype(t); }
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(&t->data, FFFD, 3); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_DOCTYPE); t->tok.force_quirks = true; emit_doctype(t); emit_eof(t); }
            else buf_putc(&t->data, lower(c));
            break;
        case TS_AFTER_DOCTYPE_NAME:
            if (t->pos >= t->n && !t->eof) return 0;
            if (t->pos >= t->n) { tok_err(t, PE_EOF_IN_DOCTYPE); t->tok.force_quirks = true; emit_doctype(t); emit_eof(t); break; }
            c = (unsigned char)t->in[t->pos];
            if (is_ws(c)) { t->pos++; break; }
            if (c == '>') { t->pos++; SWITCH(TS_DATA); emit_doctype(t); break; }
            if (t->n - t->pos < 6 && !t->eof) return 0;
            if (looking_at(t, "public", true)) { t->pos += 6; SWITCH(TS_AFTER_DOCTYPE_PUBLIC_KW); }
            else if (looking_at(t, "system", true)) { t->pos += 6; SWITCH(TS_AFTER_DOCTYPE_SYSTEM_KW); }
            else {
                tok_err(t, PE_INVALID_CHARACTER_SEQUENCE_AFTER_DOCTYPE_NAME);
                t->tok.force_quirks = true;
                SWITCH(TS_BOGUS_DOCTYPE);
            }
            break;
        case TS_AFTER_DOCTYPE_PUBLIC_KW:
        case TS_AFTER_DOCTYPE_SYSTEM_KW: {
            bool pub = t->state == TS_AFTER_DOCTYPE_PUBLIC_KW;
            GETC();
            if (is_ws(c)) SWITCH(pub ? TS_BEFORE_DOCTYPE_PUBLIC_ID : TS_BEFORE_DOCTYPE_SYSTEM_ID);
            else if (c == '"' || c == '\'') {
                tok_err(t, pub ? PE_MISSING_WHITESPACE_AFTER_DOCTYPE_PUBLIC_KEYWORD : PE_MISSING_WHITESPACE_AFTER_DOCTYPE_SYSTEM_KEYWORD);
                if (pub) { t->tok.dt_pub_missing = false; buf_clear(&t->dtpub); SWITCH(c == '"' ? TS_DOCTYPE_PUBLIC_ID_DQ : TS_DOCTYPE_PUBLIC_ID_SQ); }
                else { t->tok.dt_sys_missing = false; buf_clear(&t->dtsys); SWITCH(c == '"' ? TS_DOCTYPE_SYSTEM_ID_DQ : TS_DOCTYPE_SYSTEM_ID_SQ); }
            } else if (c == '>') {
                tok_err(t, pub ? PE_MISSING_DOCTYPE_PUBLIC_IDENTIFIER : PE_MISSING_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                SWITCH(TS_DATA);
                emit_doctype(t);
            } else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                tok_err(t, pub ? PE_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER : PE_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                RECON(TS_BOGUS_DOCTYPE);
            }
            break;
        }
        case TS_BEFORE_DOCTYPE_PUBLIC_ID:
        case TS_BEFORE_DOCTYPE_SYSTEM_ID: {
            bool pub = t->state == TS_BEFORE_DOCTYPE_PUBLIC_ID;
            GETC();
            if (is_ws(c)) break;
            if (c == '"' || c == '\'') {
                if (pub) { t->tok.dt_pub_missing = false; buf_clear(&t->dtpub); SWITCH(c == '"' ? TS_DOCTYPE_PUBLIC_ID_DQ : TS_DOCTYPE_PUBLIC_ID_SQ); }
                else { t->tok.dt_sys_missing = false; buf_clear(&t->dtsys); SWITCH(c == '"' ? TS_DOCTYPE_SYSTEM_ID_DQ : TS_DOCTYPE_SYSTEM_ID_SQ); }
            } else if (c == '>') {
                tok_err(t, pub ? PE_MISSING_DOCTYPE_PUBLIC_IDENTIFIER : PE_MISSING_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                SWITCH(TS_DATA);
                emit_doctype(t);
            } else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                tok_err(t, pub ? PE_MISSING_QUOTE_BEFORE_DOCTYPE_PUBLIC_IDENTIFIER : PE_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                RECON(TS_BOGUS_DOCTYPE);
            }
            break;
        }
        case TS_DOCTYPE_PUBLIC_ID_DQ:
        case TS_DOCTYPE_PUBLIC_ID_SQ:
        case TS_DOCTYPE_SYSTEM_ID_DQ:
        case TS_DOCTYPE_SYSTEM_ID_SQ: {
            bool pub = t->state == TS_DOCTYPE_PUBLIC_ID_DQ || t->state == TS_DOCTYPE_PUBLIC_ID_SQ;
            char q = (t->state == TS_DOCTYPE_PUBLIC_ID_DQ || t->state == TS_DOCTYPE_SYSTEM_ID_DQ) ? '"' : '\'';
            WebBuf *b = pub ? &t->dtpub : &t->dtsys;
            GETC();
            if (c == q) SWITCH(pub ? TS_AFTER_DOCTYPE_PUBLIC_ID : TS_AFTER_DOCTYPE_SYSTEM_ID);
            else if (c == 0) { tok_err(t, PE_UNEXPECTED_NULL_CHARACTER); buf_put(b, FFFD, 3); }
            else if (c == '>') {
                tok_err(t, pub ? PE_ABRUPT_DOCTYPE_PUBLIC_IDENTIFIER : PE_ABRUPT_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                SWITCH(TS_DATA);
                emit_doctype(t);
            } else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else buf_putc(b, (char)c);
            break;
        }
        case TS_AFTER_DOCTYPE_PUBLIC_ID:
        case TS_BETWEEN_DOCTYPE_IDS: {
            bool after = t->state == TS_AFTER_DOCTYPE_PUBLIC_ID;
            GETC();
            if (is_ws(c)) { if (after) SWITCH(TS_BETWEEN_DOCTYPE_IDS); }
            else if (c == '>') { SWITCH(TS_DATA); emit_doctype(t); }
            else if (c == '"' || c == '\'') {
                if (after) tok_err(t, PE_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS);
                t->tok.dt_sys_missing = false;
                buf_clear(&t->dtsys);
                SWITCH(c == '"' ? TS_DOCTYPE_SYSTEM_ID_DQ : TS_DOCTYPE_SYSTEM_ID_SQ);
            } else if (c == EOF_C) {
                tok_err(t, PE_EOF_IN_DOCTYPE);
                t->tok.force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                tok_err(t, PE_MISSING_QUOTE_BEFORE_DOCTYPE_SYSTEM_IDENTIFIER);
                t->tok.force_quirks = true;
                RECON(TS_BOGUS_DOCTYPE);
            }
            break;
        }
        case TS_AFTER_DOCTYPE_SYSTEM_ID:
            GETC();
            if (is_ws(c)) break;
            if (c == '>') { SWITCH(TS_DATA); emit_doctype(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_DOCTYPE); t->tok.force_quirks = true; emit_doctype(t); emit_eof(t); }
            else { tok_err(t, PE_UNEXPECTED_CHARACTER_AFTER_DOCTYPE_SYSTEM_IDENTIFIER); RECON(TS_BOGUS_DOCTYPE); }
            break;
        case TS_BOGUS_DOCTYPE:
            GETC();
            if (c == '>') { SWITCH(TS_DATA); emit_doctype(t); }
            else if (c == 0) tok_err(t, PE_UNEXPECTED_NULL_CHARACTER);
            else if (c == EOF_C) { emit_doctype(t); emit_eof(t); }
            break;
        /* ---------------------------------------------------------------- CDATA */
        case TS_CDATA: {
            size_t r = 0;
            while (t->pos + r < t->n && t->in[t->pos + r] != ']') r++;
            if (r) { t->pos += r; emit_chars(t, t->in + t->pos - r, r); if (t->paused || t->stop) continue; }
            GETC();
            if (c == ']') SWITCH(TS_CDATA_BRACKET);
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_CDATA); emit_eof(t); }
            else emit_c(t, c);
            break;
        }
        case TS_CDATA_BRACKET:
            GETC();
            if (c == ']') SWITCH(TS_CDATA_END);
            else { emit_c(t, ']'); RECON(TS_CDATA); }
            break;
        case TS_CDATA_END:
            GETC();
            if (c == ']') emit_c(t, ']');
            else if (c == '>') SWITCH(TS_DATA);
            else { emit_chars(t, "]]", 2); RECON(TS_CDATA); }
            break;
        /* ---------------------------------------------------------------- processing instructions */
        case TS_PI_OPEN:
            GETC();
            if (is_alpha(c) || c == '_') RECON(TS_PI_TARGET);
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_PROCESSING_INSTRUCTION); emit_eof(t); }
            else {
                tok_err(t, PE_INVALID_FIRST_CHARACTER_OF_PROCESSING_INSTRUCTION_TARGET);
                tmp_to_comment(t);
                RECON(TS_BOGUS_COMMENT);
            }
            break;
        case TS_PI_TARGET:
            GETC();
            if (is_ws(c) || c == '?' || c == '>') {
                bool xml = (t->tmp.n == 3 && strncasecmp(t->tmp.p, "xml", 3) == 0) ||
                           (t->tmp.n == 14 && strncasecmp(t->tmp.p, "xml-stylesheet", 14) == 0);
                if (xml) {
                    tok_err(t, PE_DISALLOWED_PROCESSING_INSTRUCTION_TARGET);
                    tmp_to_comment(t);
                    RECON(TS_BOGUS_COMMENT);
                } else {
                    buf_clear(&t->data);
                    RECON(TS_AFTER_PI_TARGET);
                }
            } else if (is_alnum(c) || c == '-' || c == '_') buf_putc(&t->tmp, (char)c);
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_PROCESSING_INSTRUCTION); emit_eof(t); }
            else {
                tok_err(t, PE_INVALID_PROCESSING_INSTRUCTION_TARGET);
                tmp_to_comment(t);
                RECON(TS_BOGUS_COMMENT);
            }
            break;
        case TS_AFTER_PI_TARGET:
            GETC();
            if (is_ws(c)) break;
            RECON(TS_PI_DATA);
            break;
        case TS_PI_DATA:
            GETC();
            if (c == '?') SWITCH(TS_PI_QUESTIONABLE);
            else if (c == '>') { SWITCH(TS_DATA); emit_pi(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_PROCESSING_INSTRUCTION); emit_eof(t); }
            else buf_putc(&t->data, (char)c);
            break;
        case TS_PI_QUESTIONABLE:
            GETC();
            if (c == '>') { SWITCH(TS_DATA); emit_pi(t); }
            else if (c == EOF_C) { tok_err(t, PE_EOF_IN_PROCESSING_INSTRUCTION); emit_eof(t); }
            else { buf_putc(&t->data, '?'); RECON(TS_PI_DATA); }
            break;
        /* ---------------------------------------------------------------- character references */
        case TS_CHARREF:
            buf_clear(&t->tmp);
            buf_putc(&t->tmp, '&');
            GETC();
            if (is_alnum(c)) RECON(TS_NAMED_CHARREF);
            else if (c == '#') { buf_putc(&t->tmp, '#'); SWITCH(TS_NUMERIC_CHARREF); }
            else { flush_charref(t); RECON(t->ret_state); }
            break;
        case TS_NAMED_CHARREF: {
            size_t mlen = 0;
            int mi = 0;
            int r = match_entity(t, &mlen, &mi);
            if (r < 0) return 0;
            if (r == 0) {
                flush_charref(t);
                SWITCH(TS_AMBIGUOUS_AMP);
                break;
            }
            const WebEntity *e = &web_entities[mi];
            const char *nm = web_entity_names + e->off;
            buf_put(&t->tmp, nm, mlen);
            t->pos += mlen;
            bool semi = nm[mlen - 1] == ';';
            int next = t->pos < t->n ? (unsigned char)t->in[t->pos] : EOF_C;
            if (in_attr(t->ret_state) && !semi && (next == '=' || is_alnum(next))) {
                flush_charref(t);
                SWITCH(t->ret_state);
                break;
            }
            if (!semi) tok_err(t, PE_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE);
            buf_clear(&t->tmp);
            buf_put_cp(&t->tmp, e->cp[0]);
            if (e->ncp > 1) buf_put_cp(&t->tmp, e->cp[1]);
            flush_charref(t);
            SWITCH(t->ret_state);
            break;
        }
        case TS_AMBIGUOUS_AMP:
            GETC();
            if (is_alnum(c)) {
                if (in_attr(t->ret_state)) buf_putc(&t->val, (char)c);
                else emit_c(t, c);
            } else if (c == ';') { tok_err(t, PE_UNKNOWN_NAMED_CHARACTER_REFERENCE); RECON(t->ret_state); }
            else RECON(t->ret_state);
            break;
        case TS_NUMERIC_CHARREF:
            t->charref_code = 0;
            GETC();
            if (c == 'x' || c == 'X') { buf_putc(&t->tmp, (char)c); SWITCH(TS_HEX_CHARREF_START); }
            else RECON(TS_DEC_CHARREF_START);
            break;
        case TS_HEX_CHARREF_START:
            GETC();
            if (is_hex(c)) RECON(TS_HEX_CHARREF);
            else { tok_err(t, PE_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE); flush_charref(t); RECON(t->ret_state); }
            break;
        case TS_DEC_CHARREF_START:
            GETC();
            if (is_digit(c)) RECON(TS_DEC_CHARREF);
            else { tok_err(t, PE_ABSENCE_OF_DIGITS_IN_NUMERIC_CHARACTER_REFERENCE); flush_charref(t); RECON(t->ret_state); }
            break;
        case TS_HEX_CHARREF:
        case TS_DEC_CHARREF: {
            bool hex = t->state == TS_HEX_CHARREF;
            GETC();
            if (hex ? is_hex(c) : is_digit(c)) {
                int v = is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
                uint32_t code = t->charref_code * (hex ? 16 : 10) + (uint32_t)v;
                t->charref_code = code > 0x110000 ? 0x110000 : code;     /* stays "outside the range" without overflowing */
            } else if (c == ';') SWITCH(TS_NUMERIC_CHARREF_END);
            else { tok_err(t, PE_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE); RECON(TS_NUMERIC_CHARREF_END); }
            break;
        }
        case TS_NUMERIC_CHARREF_END: {
            uint32_t code = t->charref_code;
            if (code == 0) { tok_err(t, PE_NULL_CHARACTER_REFERENCE); code = 0xFFFD; }
            else if (code > 0x10FFFF) { tok_err(t, PE_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE); code = 0xFFFD; }
            else if (code >= 0xD800 && code <= 0xDFFF) { tok_err(t, PE_SURROGATE_CHARACTER_REFERENCE); code = 0xFFFD; }
            else if (is_noncharacter(code)) tok_err(t, PE_NONCHARACTER_CHARACTER_REFERENCE);
            else if (code == 0x0D || ((code < 0x20 || (code >= 0x7F && code <= 0x9F)) && !is_ws((int)code))) {
                tok_err(t, PE_CONTROL_CHARACTER_REFERENCE);
                if (code >= 0x80 && code <= 0x9F && c1_table[code - 0x80]) code = c1_table[code - 0x80];
            }
            buf_clear(&t->tmp);
            buf_put_cp(&t->tmp, code);
            flush_charref(t);
            SWITCH(t->ret_state);
            break;
        }
        default:
            t->state = TS_DATA;
            break;
        }
    }
}
