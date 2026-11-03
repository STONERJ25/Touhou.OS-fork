/*
 * Documents and the HTML parser's driver: the input byte stream (HTML 13.2.3: the encoding decision, decoding,
 * "changing the encoding while parsing"), feeding the tokenizer, parser-inserted scripts and document.write, the end of
 * parsing (13.2.7), and fragment parsing for innerHTML (13.4).
 *
 * The encoding: the transport layer's charset if it names a known encoding (certain); else a byte order mark (certain);
 * else the <meta charset> pre-scan of the first 1024 bytes (tentative); else, for pages that declare nothing, UTF-8 when
 * the first bytes are valid UTF-8 with at least one multi-byte character, windows-1252 otherwise (tentative). A later
 * <meta charset> found by the tree builder switches the decoder in place when everything so far was ASCII, or parses the
 * document again from its bytes with the declared encoding.
 */
#include "webint.h"

#include <stdlib.h>

/* ================================================================================================ documents */

WebDoc *web_doc_new(const WebConfig *cfg, const char *url) {
    WebConfig c;
    if (cfg) c = *cfg;
    else memset(&c, 0, sizeof c);
    WebAllocator al = c.alloc ? *c.alloc : (WebAllocator){ NULL, NULL, NULL };
    WebDoc *d = al.alloc ? al.alloc(al.user, sizeof *d) : malloc(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    d->cfg = c;
    d->budget.al = al;
    d->budget.limit = c.memory_limit ? c.memory_limit : (size_t)512 << 20;
    d->budget.used = sizeof *d;
    wa_init(&d->arena, &d->budget);
    wa_init(&d->larena, &d->budget);
    wa_init(&d->parena, &d->budget);
    if (d->cfg.scale <= 0) d->cfg.scale = 1;
    if (!atoms_init(&d->atoms, &d->arena)) goto fail;
    d->root = dom_new_node(d, WEB_NODE_DOCUMENT);
    if (!d->root) goto fail;
    d->url = wa_strdup(&d->arena, url && *url ? url : "about:blank");
    if (!d->url) goto fail;
    d->base_url = d->url;
    d->scripting = c.scripts != NULL;
    d->charset = "UTF-8";
    d->style_dirty = d->layout_dirty = true;
    d->vw = 800;
    d->vh = 600;
    return d;
fail:
    wa_destroy(&d->arena);
    if (al.free) al.free(al.user, d);
    else free(d);
    return NULL;
}

void res_free_all(WebDoc *d);          /* resource.c */
void style_free(WebDoc *d);            /* style.c */
void image_free_all(WebDoc *d);        /* image.c */

void web_doc_free(WebDoc *d) {
    if (!d) return;
    const WebScriptHost *h = d->cfg.scripts;
    if (h && h->document_freed) h->document_freed(h->user, d);
    res_free_all(d);
    image_free_all(d);
    style_free(d);
    if (d->parser) wa_destroy(&d->parser->a);
    wa_destroy(&d->parena);
    wa_destroy(&d->larena);
    wa_destroy(&d->arena);
    WebAllocator al = d->budget.al;
    if (al.free) al.free(al.user, d);
    else free(d);
}

int web_doc_status(const WebDoc *d) { return d->budget.exceeded ? WEB_E_NOMEM : d->status; }
const char *web_doc_url(const WebDoc *d) { return d->url; }
const char *web_doc_base_url(const WebDoc *d) { return d->base_url; }
const char *web_doc_charset(const WebDoc *d) { return d->charset; }
int web_doc_quirks(const WebDoc *d) { return d->quirks; }
WebNode *web_doc_root(WebDoc *d) { return d->root; }

WebNode *web_doc_element(WebDoc *d) {
    for (WebNode *k = d->root->first; k; k = k->next)
        if (k->type == WEB_NODE_ELEMENT) return k;
    return NULL;
}

static WebNode *html_child(WebDoc *d, WebAtom name) {
    WebNode *h = web_doc_element(d);
    if (!is_html(h, AT_html)) return NULL;
    for (WebNode *k = h->first; k; k = k->next)
        if (is_html(k, name)) return k;
    return NULL;
}

WebNode *web_doc_head(WebDoc *d) { return html_child(d, AT_head); }

WebNode *web_doc_body(WebDoc *d) {
    WebNode *b = html_child(d, AT_body);
    return b ? b : html_child(d, AT_frameset);
}

const char *web_doc_title(WebDoc *d) {
    WebNode *t = NULL;
    for (WebNode *n = d->root->first; n; n = dom_next(n, d->root))
        if (is_html(n, AT_title) || (n->type == WEB_NODE_ELEMENT && n->ns == WEB_NS_SVG && n->name == AT_title && n->parent &&
                                     n->parent->ns == WEB_NS_SVG && n->parent->name == AT_svg && !n->parent->parent)) {
            t = n;
            break;
        }
    if (!t) return "";
    WebBuf b, o;
    buf_init(&b, &d->arena);
    buf_init(&o, &d->arena);
    dom_text_of(t, &b);
    bool sp = false;                                         /* strip and collapse ASCII white space */
    for (uint32_t i = 0; i < b.n; i++) {
        char c = b.p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') { sp = o.n > 0; continue; }
        if (sp) buf_putc(&o, ' ');
        sp = false;
        buf_putc(&o, c);
    }
    buf_free(&b);
    if (d->title_cache) wa_free(&d->arena, d->title_cache, strlen(d->title_cache) + 1);
    d->title_cache = wa_strndup(&d->arena, o.p ? o.p : "", o.n);
    buf_free(&o);
    return d->title_cache ? d->title_cache : "";
}

/* ================================================================================================ parse errors */

void doc_error(WebDoc *d, int code, size_t offset) {
    d->nerrors++;
    if (d->errors.n >= 256) return;
    WebParseError e = { (uint16_t)code, 0, 0 };
    HtmlParser *p = d->parser;
    if (p && p->text.p) {
        /* line and column (1-based, in code points) of the offset in the decoded input still held */
        size_t off = offset < p->text.n ? offset : p->text.n;
        int line = 1, col = 1;
        for (size_t i = 0; i < off; i++) {
            unsigned char c = (unsigned char)p->text.p[i];
            if (c == '\n') { line++; col = 1; }
            else if ((c & 0xC0) != 0x80) col++;
        }
        e.line = line;
        e.col = col;
    }
    vec_push(&d->arena, d->errors, e);
}

int web_doc_parse_error_count(const WebDoc *d) { return d->nerrors; }

bool web_doc_parse_error(const WebDoc *d, int i, const char **code, int *line, int *col) {
    if (i < 0 || (uint32_t)i >= d->errors.n) return false;
    const WebParseError *e = &d->errors.v[i];
    if (code) *code = e->code < PE_COUNT ? web_parse_error_names[e->code] : "?";
    if (line) *line = e->line;
    if (col) *col = e->col;
    return true;
}

static void tok_error_cb(void *user, int code, size_t offset) {
    HtmlTreeBuilder *tb = user;
    doc_error(tb->doc, code, offset);
}

typedef struct { WebDoc *d; size_t base; } InputErr;

static void input_error_cb(void *user, int code, size_t offset) {
    InputErr *e = user;
    doc_error(e->d, code, e->base + offset);
}

/* decodes bytes into the parser's text and reports the input stream's parse errors for what was added */
static void decode(WebDoc *d, HtmlParser *p, const void *bytes, size_t len, bool last) {
    uint32_t before = p->text.n;
    dec_run(&p->dec, bytes, len, last, &p->text);
    InputErr e = { d, before };
    if (p->text.n > before) enc_input_errors(p->text.p + before, p->text.n - before, input_error_cb, &e);
}

/* ================================================================================================ the parser */

static HtmlParser *parser_new(WebDoc *d) {
    HtmlParser *p = wa_alloc(&d->arena, sizeof *p);
    if (!p) return NULL;
    p->doc = d;
    p->restart_enc = -1;
    wa_init(&p->a, &d->budget);
    buf_init(&p->raw, &p->a);
    buf_init(&p->text, &p->a);
    tok_init(&p->tok, d, &p->a);
    tb_init(&p->tb, d, &p->tok, &p->a);
    p->tok.sink = tb_token;
    p->tok.sink_user = &p->tb;
    p->tok.cdata_allowed = tb_cdata_allowed;
    p->tok.error = tok_error_cb;
    return p;
}

static bool valid_utf8_with_multibyte(const unsigned char *s, size_t n) {
    bool multi = false;
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        int need = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : -1;
        if (need < 0) return false;
        if (i + (size_t)need >= n) return multi;              /* cut at the end of what we have: undecided, accept */
        for (int k = 1; k <= need; k++)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        multi = true;
        i += (size_t)need + 1;
    }
    return multi;
}

/* Decides the encoding when enough bytes are there (or at the end); returns false while it still waits. */
static bool decide_encoding(WebDoc *d, HtmlParser *p, const char *charset, bool final) {
    const unsigned char *b = (const unsigned char *)p->raw.p;
    size_t n = p->raw.n, bom = 0;
    if (n < 3 && !final) return false;
    int enc = enc_sniff_bom(b, n, &bom);
    bool certain = enc >= 0;
    if (enc < 0 && charset) {
        enc = enc_from_label(charset, strlen(charset));
        if (enc == ENC_X_USER_DEFINED) enc = ENC_WINDOWS_1252;
        certain = enc >= 0;
    }
    if (enc < 0) {
        if (n < 1024 && !final) return false;
        enc = enc_prescan(b, n);
    }
    if (enc < 0) enc = valid_utf8_with_multibyte(b, n < 65536 ? n : 65536) ? ENC_UTF8 : ENC_WINDOWS_1252;
    dec_init(&p->dec, enc);
    p->dec_ready = true;
    p->enc_certain = certain;
    d->charset = enc_name(enc);
    if (bom) {
        memmove(p->raw.p, p->raw.p + bom, n - bom);
        p->raw.n -= (uint32_t)bom;
    }
    return true;
}

static bool any_nonascii(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (b[i] >= 0x80) return true;
    return false;
}

static void the_end(WebDoc *d);
static int restart(WebDoc *d, int enc);

static int pump(WebDoc *d) {
    HtmlParser *p = d->parser;
    if (!p) return WEB_E_STATE;
    for (;;) {
        tok_set_input(&p->tok, p->text.p ? p->text.p : "", p->text.n, p->finished);
        p->tok.paused = false;
        int r = tok_run(&p->tok);
        if (p->restart_enc >= 0) return restart(d, p->restart_enc);
        if (d->budget.exceeded) d->status = WEB_E_NOMEM;
        if (r == 1) { p->paused = true; return WEB_PAUSED; }
        if (p->tok.stop || p->tb.done || d->status == WEB_E_NOMEM) {
            the_end(d);
            return web_doc_status(d);
        }
        /* drop what the tokenizer has consumed once it is large, so incremental input does not pile up */
        if (p->tok.pos > 65536) {
            size_t keep = p->text.n - p->tok.pos;
            memmove(p->text.p, p->text.p + p->tok.pos, keep);
            p->text.n = (uint32_t)keep;
            p->tok.pos = 0;
        }
        return WEB_OK;
    }
}

int web_doc_parse_bytes(WebDoc *d, const void *bytes, size_t len, const char *charset) {
    if (d->parse_done) return WEB_E_STATE;
    if (!d->parser) {
        d->parser = parser_new(d);
        if (!d->parser) return WEB_E_NOMEM;
        d->parsing = true;
        d->in_mutation = true;
    }
    HtmlParser *p = d->parser;
    if (!p->dec_ready) {
        if (!buf_put(&p->raw, bytes, len)) return WEB_E_NOMEM;
        if (charset && !p->charset_hint) p->charset_hint = wa_strdup(&p->a, charset);
        if (!decide_encoding(d, p, p->charset_hint, false)) return WEB_OK;
        p->nonascii = any_nonascii((const unsigned char *)p->raw.p, p->raw.n);
        decode(d, p, p->raw.p, p->raw.n, false);
        if (p->enc_certain) buf_free(&p->raw);
    } else {
        if (!p->enc_certain && !buf_put(&p->raw, bytes, len)) return WEB_E_NOMEM;
        if (!p->nonascii) p->nonascii = any_nonascii(bytes, len);
        decode(d, p, bytes, len, false);
    }
    if (p->paused) return WEB_PAUSED;
    return pump(d);
}

int web_doc_parse_finish(WebDoc *d) {
    if (d->parse_done) return WEB_OK;
    if (!d->parser) {
        int rc = web_doc_parse_bytes(d, "", 0, NULL);
        if (rc < 0) return rc;
    }
    HtmlParser *p = d->parser;
    if (!p->dec_ready) {
        decide_encoding(d, p, p->charset_hint, true);
        p->nonascii = any_nonascii((const unsigned char *)p->raw.p, p->raw.n);
        decode(d, p, p->raw.p, p->raw.n, true);
    } else {
        decode(d, p, "", 0, true);
    }
    p->finished = true;
    if (p->paused) return WEB_PAUSED;
    return pump(d);
}

int web_doc_parse(WebDoc *d, const void *bytes, size_t len, const char *charset) {
    int rc = web_doc_parse_bytes(d, bytes, len, charset);
    if (rc < 0 || rc == WEB_PAUSED) {
        if (rc == WEB_PAUSED) d->parser->finish_pending = true;
        return rc;
    }
    return web_doc_parse_finish(d);
}

int web_doc_parse_resume(WebDoc *d) {
    HtmlParser *p = d->parser;
    if (!p || !p->paused) return WEB_E_STATE;
    if (p->script_wait) return WEB_PAUSED;                    /* still waiting for an external script */
    p->paused = false;
    int rc = pump(d);
    if (rc == WEB_OK && p == d->parser && p->finish_pending && !p->finished) {
        p->finish_pending = false;
        return web_doc_parse_finish(d);
    }
    return rc;
}

/* 13.2.3.4 "changing the encoding while parsing", called for <meta charset> and <meta http-equiv=content-type> */
void parser_meta_charset(WebDoc *d, WebNode *meta) {
    HtmlParser *p = d->parser;
    if (!p || p->enc_certain || p->tb.fragment) return;
    int enc = -1;
    const char *cs = dom_attr_val(meta, AT_charset);
    if (cs) enc = enc_from_label(cs, strlen(cs));
    else if (dom_attr_is(meta, AT_http_equiv, "content-type")) {
        const char *c = dom_attr_val(meta, AT_content);
        if (c) enc = enc_from_meta_content(c, strlen(c));
    }
    if (enc < 0) return;
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) enc = ENC_UTF8;
    if (enc == ENC_X_USER_DEFINED) enc = ENC_WINDOWS_1252;
    if (enc == p->dec.enc) { p->enc_certain = true; buf_free(&p->raw); return; }
    if (!p->nonascii) {
        /* every byte so far was ASCII, which both encodings decode alike: switch the decoder for what follows */
        dec_init(&p->dec, enc);
        p->enc_certain = true;
        d->charset = enc_name(enc);
        buf_free(&p->raw);
        return;
    }
    p->restart_enc = enc;                                     /* parse again from the bytes (pump does it) */
    p->tok.stop = true;
}

static int restart(WebDoc *d, int enc) {
    HtmlParser *old = d->parser;
    HtmlParser *p = parser_new(d);
    if (!p) return WEB_E_NOMEM;
    while (d->root->first) dom_remove(d->root->first);
    d->quirks = 0;
    d->nerrors = 0;
    d->errors.n = 0;
    p->restart_enc = -1;
    dec_init(&p->dec, enc);
    p->dec_ready = p->enc_certain = true;
    d->charset = enc_name(enc);
    bool fin = old->finished;
    dec_run(&p->dec, (const unsigned char *)old->raw.p, old->raw.n, fin, &p->text);
    p->finished = fin;
    p->finish_pending = old->finish_pending;
    d->parser = p;
    wa_destroy(&old->a);
    return pump(d);
}

/* 13.2.7 "the end", without the parts that belong to scripts and navigation (DOMContentLoaded and load are reported to
 * the script host; deferred scripts run in order when they have arrived, resource.c) */
void res_parse_finished(WebDoc *d);   /* resource.c */

static void the_end(WebDoc *d) {
    HtmlParser *p = d->parser;
    if (!p || d->parse_done) return;
    tb_finish(&p->tb);
    d->parse_done = true;
    d->parsing = false;
    d->in_mutation = false;
    d->style_dirty = d->layout_dirty = true;
    /* the parser's memory goes; the tree stays */
    d->parser = NULL;
    wa_destroy(&p->a);
    res_parse_finished(d);
}

/* ================================================================================================ scripts and document.write */

static bool js_type(const WebNode *s) {
    const char *t = dom_attr_val(s, AT_type);
    if (!t) {
        const char *lang = dom_attr_val(s, AT_lang);
        return !lang || !*lang || strncasecmp(lang, "javascript", 10) == 0;
    }
    while (*t == ' ' || *t == '\t' || *t == '\n') t++;
    if (!*t) return true;
    static const char *const ok[] = { "application/ecmascript", "application/javascript", "application/x-ecmascript",
        "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0", "text/javascript1.1",
        "text/javascript1.2", "text/javascript1.3", "text/javascript1.4", "text/javascript1.5", "text/jscript",
        "text/livescript", "text/x-ecmascript", "text/x-javascript", "module" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (strcasecmp(t, ok[i]) == 0) return true;
    return false;
}

uint32_t res_fetch_script(WebDoc *d, WebNode *script, const char *url, int mode);   /* resource.c; mode 0 blocking, 1 async, 2 defer */

void parser_script_end(HtmlTreeBuilder *tb, WebNode *s) {
    WebDoc *d = tb->doc;
    const WebScriptHost *h = d->cfg.scripts;
    HtmlParser *p = d->parser;
    if (!h || !h->run_script || !p || (s->flags & NF_STARTED)) return;
    s->flags |= NF_STARTED;
    if (!js_type(s) || dom_attr(s, AT_nomodule)) return;
    const char *src = dom_attr_val(s, AT_src);
    if (src) {
        const char *url = doc_resolve_url(d, src, strlen(src), &d->arena);
        if (!url) return;
        bool module = dom_attr_is(s, AT_type, "module");
        int mode = dom_attr(s, AT_async) ? 1 : (dom_attr(s, AT_defer) || module) ? 2 : 0;
        uint32_t id = res_fetch_script(d, s, url, mode);
        if (id && mode == 0) {
            p->script_wait = id;                              /* parser-blocking: wait for it */
            p->tok.paused = true;
        }
        return;
    }
    WebBuf b;
    buf_init(&b, &p->a);
    dom_text_of(s, &b);
    p->script_nesting++;
    int r = h->run_script(h->user, d, s, b.p ? b.p : "", b.n);
    p->script_nesting--;
    buf_free(&b);
    if (r == 1) p->tok.paused = true;
}

/* resource.c: a parser-blocking script arrived (or failed: data NULL) */
void parser_script_loaded(WebDoc *d, WebNode *s, uint32_t id, const char *data, size_t len) {
    HtmlParser *p = d->parser;
    const WebScriptHost *h = d->cfg.scripts;
    if (data && h && h->run_script) {
        if (p) p->script_nesting++;
        h->run_script(h->user, d, s, data, len);
        if (p) p->script_nesting--;
    }
    if (p && p->script_wait == id) {
        p->script_wait = 0;
        web_doc_parse_resume(d);
    }
}

int web_doc_write(WebDoc *d, const char *s, size_t n) {
    HtmlParser *p = d->parser;
    if (p && p->script_nesting > 0) {
        /* insert at the insertion point: just before the next input character of the tokenizer */
        WebBuf tmp;
        buf_init(&tmp, &p->a);
        WebDecoder dec;
        dec_init(&dec, ENC_UTF8);
        dec_run(&dec, (const unsigned char *)s, n, true, &tmp);
        size_t at = p->tok.pos;
        if (!buf_reserve(&p->text, tmp.n)) { buf_free(&tmp); return WEB_E_NOMEM; }
        memmove(p->text.p + at + tmp.n, p->text.p + at, p->text.n - at);
        memcpy(p->text.p + at, tmp.p, tmp.n);
        p->text.n += tmp.n;
        p->text.p[p->text.n] = 0;
        tok_set_input(&p->tok, p->text.p, p->text.n, p->finished);
        buf_free(&tmp);
        return WEB_OK;
    }
    if (p && !d->parse_done) {                                /* a script that is not run by the parser: append */
        return web_doc_parse_bytes(d, s, n, "utf-8");
    }
    /* after the parse: document.open() is implied, the document starts again */
    while (d->root->first) dom_remove(d->root->first);
    d->parse_done = false;
    d->quirks = 0;
    d->parser = parser_new(d);
    if (!d->parser) return WEB_E_NOMEM;
    d->parser->restart_enc = -1;
    dec_init(&d->parser->dec, ENC_UTF8);
    d->parser->dec_ready = d->parser->enc_certain = true;
    d->parsing = true;
    return web_doc_parse_bytes(d, s, n, "utf-8");
}

/* ================================================================================================ fragments (13.4) */

int parse_fragment(WebDoc *d, WebNode *context, const char *utf8, size_t len, WebNode *into) {
    HtmlParser *p = parser_new(d);
    if (!p) return WEB_E_NOMEM;
    HtmlParser *outer = d->parser;
    d->parser = p;                                            /* errors and meta handling see the fragment parser */
    p->restart_enc = -1;
    dec_init(&p->dec, ENC_UTF8);
    p->dec_ready = p->enc_certain = true;
    dec_run(&p->dec, (const unsigned char *)utf8, len, true, &p->text);
    p->finished = true;
    bool was = d->in_mutation;
    d->in_mutation = true;
    tb_init_fragment(&p->tb, context);
    tok_set_input(&p->tok, p->text.p ? p->text.p : "", p->text.n, true);
    while (tok_run(&p->tok) == 1) p->tok.paused = false;     /* scripts do not run in fragments */
    WebNode *root = p->tb.fragment_root;
    if (root)
        while (root->first) {
            WebNode *k = root->first;
            dom_remove(k);
            dom_append(into, k);
        }
    d->in_mutation = was;
    d->parser = outer;
    wa_destroy(&p->a);
    return d->budget.exceeded ? WEB_E_NOMEM : WEB_OK;
}

/* ================================================================================================ tests: the tokenizer alone */

typedef struct { TokTestSink sink; void *user; } TestSinkCtx;
typedef struct {
    WebDoc *d;
    HtmlTokenizer *t;
    void (*err)(void *user, int code, int line, int col);
    void *err_user;
    TestSinkCtx s;
} TestCtx;

static void test_sink(void *user, HtmlToken *t) {
    TestCtx *c = user;
    c->s.sink(c->s.user, c->d, t);
}

static void test_err(void *user, int code, size_t offset) {
    TestCtx *c = user;
    if (!c->err) return;
    int line = 1, col = 1;
    for (size_t i = 0; i < offset && i < c->t->n; i++) {
        unsigned char ch = (unsigned char)c->t->in[i];
        if (ch == '\n') { line++; col = 1; }
        else if ((ch & 0xC0) != 0x80) col++;
    }
    c->err(c->err_user, code, line, col);
}

int web_test_tokenize(const char *utf8, size_t len, int initial_state, const char *last_start_tag,
                      TokTestSink sink, void *user, void (*err)(void *user, int code, int line, int col), void *err_user) {
    WebDoc *d = web_doc_new(NULL, NULL);
    if (!d) return WEB_E_NOMEM;
    WebArena a;
    wa_init(&a, &d->budget);
    WebBuf text;
    buf_init(&text, &a);
    for (size_t i = 0; i < len; i++) {                       /* the input stream preprocessing: CR LF and CR -> LF */
        if (utf8[i] == '\r') {
            buf_putc(&text, '\n');
            if (i + 1 < len && utf8[i + 1] == '\n') i++;
        } else buf_putc(&text, utf8[i]);
    }
    HtmlTokenizer t;
    tok_init(&t, d, &a);
    TestCtx c = { d, &t, err, err_user, { sink, user } };
    t.sink = test_sink;
    t.sink_user = &c;
    t.error = test_err;
    t.state = (TokState)initial_state;
    if (last_start_tag) tok_set_last_start(&t, last_start_tag, strlen(last_start_tag));
    tok_set_input(&t, text.p ? text.p : "", text.n, true);
    enc_input_errors(text.p ? text.p : "", text.n, test_err, &c);
    tok_run(&t);
    wa_destroy(&a);
    web_doc_free(d);
    return WEB_OK;
}
