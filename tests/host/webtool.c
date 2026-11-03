/*
 * webtool: drives the web engine (user/lib/web) for the host tests (tests/host/test_web.py) and for looking at pages.
 *
 * Batch mode (no arguments): commands on stdin, each a header line "NAME ARG... LEN" followed by LEN bytes of payload;
 * every answer goes to stdout as "LEN\n" followed by LEN bytes, so one process runs thousands of cases.
 *
 *   tokens STATE LAST        the tokenizer alone (STATE: data, plaintext, rcdata, rawtext, script, cdata; LAST: the last
 *                            start tag or "-"): a JSON array of tokens in the html5lib format, a newline, a JSON array of
 *                            errors
 *   tree OPTS                parse a document (OPTS: "-" or a comma list of "script" and "frag=CONTEXT", CONTEXT like
 *                            "td", "svg:path", "math:mi"): the DOM in the html5lib tree format
 *   spec, select, style      selectors' specificity, matching, computed values (see the commands below)
 *   layout OPTS              lay a page out (OPTS: w=, h=, root=DIR for subresources, limit=BYTES): one line per element
 *                            with an id, "#id X Y W H" (its border box in CSS px), and for block containers with lines
 *                            "#id lines N", "#id line K Y H BASELINE" and "#id frag "TEXT" X Y W"
 *   dump OPTS                web_dump_layout of the page (with text)
 *   paint OPTS               the display list as text, one item per line
 *   hit OPTS                 payload "X Y
HTML": the hit test at a page point
 *   find OPTS                payload "NEEDLE
HTML": the match count, the first match's rectangle, the selected text
 *   render OPTS              payload "PATH
HTML": paints the viewport into a PPM file (the 8x16 font for text)
 *   bench OPTS               parse, style, layout and paint times in milliseconds
 *
 * By hand:  webtool tree FILE  |  webtool render FILE.html OUT.ppm [W [H]]  |  webtool dump FILE.html [W]
 */
#include "../../user/lib/web/src/layout.h"

#include <img.h>
#include <time.h>
#include "font8x16.h"

WebUnit builtin_handle_size(WebFontHandle h);      /* fonts.c */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ output buffer */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const void *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) { fprintf(stderr, "out of memory\n"); exit(2); }
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) bput(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void writer(void *user, const char *s, size_t n) { bput(user, s, n); }

/* JSON string from WTF-8 (a lone surrogate in its three-byte form becomes \uD8xx) */
static void json_str(Buf *b, const char *s, size_t n) {
    bputs(b, "\"");
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; bput(b, e, 2); i++; continue; }
        if (c < 0x20 || c == 0x7F) { bprintf(b, "\\u%04x", c); i++; continue; }
        if (c < 0x80) { bput(b, &s[i], 1); i++; continue; }
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        if (cp >= 0xD800 && cp <= 0xDFFF) bprintf(b, "\\u%04x", cp);
        else if (cp >= 0x10000) bprintf(b, "\\u%04x\\u%04x", 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 0x3FF));
        else if (cp >= 0x80 && cp <= 0x9F) bprintf(b, "\\u%04x", cp);
        else bput(b, s + i, len);
        i += len;
    }
    bputs(b, "\"");
}

/* ------------------------------------------------------------------------------------------------ tokens */

typedef struct { Buf *out; Buf chars; bool any; Buf errs; bool anyerr; } Rec;

static void flush_chars(Rec *r) {
    if (!r->chars.n) return;
    bputs(r->out, r->any ? ",[\"Character\"," : "[\"Character\",");
    json_str(r->out, r->chars.p, r->chars.n);
    bputs(r->out, "]");
    r->any = true;
    r->chars.n = 0;
}

static void rec_sink(void *user, WebDoc *d, const HtmlToken *t) {
    Rec *r = user;
    if (t->type == HT_CHARS) { bput(&r->chars, t->data, t->len); return; }
    flush_chars(r);
    if (t->type == HT_EOF) return;
    Buf *o = r->out;
    if (r->any) bputs(o, ",");
    r->any = true;
    switch (t->type) {
    case HT_START:
        bputs(o, "[\"StartTag\",");
        json_str(o, atom_str(&d->atoms, t->name), atom_len(&d->atoms, t->name));
        bputs(o, ",{");
        for (int i = 0; i < t->nattrs; i++) {
            if (i) bputs(o, ",");
            json_str(o, atom_str(&d->atoms, t->attrs[i].name), atom_len(&d->atoms, t->attrs[i].name));
            bputs(o, ":");
            json_str(o, t->vals + t->attrs[i].voff, t->attrs[i].vlen);
        }
        bputs(o, t->self_closing ? "},true]" : "}]");
        ((HtmlToken *)t)->ack = true;                         /* errors about trailing solidus are tree-builder business */
        break;
    case HT_END:
        bputs(o, "[\"EndTag\",");
        json_str(o, atom_str(&d->atoms, t->name), atom_len(&d->atoms, t->name));
        bputs(o, "]");
        break;
    case HT_COMMENT:
        bputs(o, "[\"Comment\",");
        json_str(o, t->data, t->len);
        bputs(o, "]");
        break;
    case HT_PI:
        bputs(o, "[\"ProcessingInstruction\",");
        json_str(o, atom_str(&d->atoms, t->name), atom_len(&d->atoms, t->name));
        bputs(o, ",");
        json_str(o, t->data, t->len);
        bputs(o, "]");
        break;
    case HT_DOCTYPE:
        bputs(o, "[\"DOCTYPE\",");
        if (t->dt_name_missing) bputs(o, "null"); else json_str(o, t->data, t->len);
        bputs(o, ",");
        if (t->dt_pub_missing) bputs(o, "null"); else json_str(o, t->dt_pub, t->dt_pub_len);
        bputs(o, ",");
        if (t->dt_sys_missing) bputs(o, "null"); else json_str(o, t->dt_sys, t->dt_sys_len);
        bputs(o, t->force_quirks ? ",false]" : ",true]");
        break;
    default:
        break;
    }
}

static void rec_err(void *user, int code, int line, int col) {
    Rec *r = user;
    bprintf(&r->errs, "%s{\"code\":\"%s\",\"line\":%d,\"col\":%d}", r->anyerr ? "," : "",
            code > 0 && code < PE_COUNT ? web_parse_error_names[code] : "?", line, col);
    r->anyerr = true;
}

static int state_of(const char *s) {
    if (!strcmp(s, "plaintext")) return TS_PLAINTEXT;
    if (!strcmp(s, "rcdata")) return TS_RCDATA;
    if (!strcmp(s, "rawtext")) return TS_RAWTEXT;
    if (!strcmp(s, "script")) return TS_SCRIPT_DATA;
    if (!strcmp(s, "cdata")) return TS_CDATA;
    return TS_DATA;
}

static void cmd_tokens(Buf *out, const char *state, const char *last, const char *in, size_t n) {
    Rec r;
    memset(&r, 0, sizeof r);
    r.out = out;
    bputs(out, "[");
    web_test_tokenize(in, n, state_of(state), strcmp(last, "-") ? last : NULL, rec_sink, &r, rec_err, &r);
    flush_chars(&r);
    bputs(out, "]\n[");
    if (r.errs.n) bput(out, r.errs.p, r.errs.n);
    bputs(out, "]");
    free(r.chars.p);
    free(r.errs.p);
}

/* ------------------------------------------------------------------------------------------------ tree */

static int dummy_run(void *user, WebDoc *d, WebNode *s, const char *src, size_t len) {
    (void)user; (void)d; (void)s; (void)src; (void)len;
    return 0;
}

static const WebScriptHost dummy_scripts = { NULL, dummy_run, NULL, NULL, NULL };

static void cmd_tree(Buf *out, const char *opts, const char *in, size_t n) {
    WebConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *frag = NULL;
    char fragbuf[64];
    if (strstr(opts, "script")) cfg.scripts = &dummy_scripts;
    const char *f = strstr(opts, "frag=");
    if (f) {
        f += 5;
        size_t k = strcspn(f, ",");
        if (k >= sizeof fragbuf) k = sizeof fragbuf - 1;
        memcpy(fragbuf, f, k);
        fragbuf[k] = 0;
        frag = fragbuf;
    }
    WebDoc *d = web_doc_new(&cfg, "http://example.org/");
    if (!d) { bputs(out, "#nomem"); return; }
    if (frag) {
        WebNamespace ns = WEB_NS_HTML;
        const char *name = frag;
        if (!strncmp(frag, "svg:", 4)) { ns = WEB_NS_SVG; name = frag + 4; }
        else if (!strncmp(frag, "math:", 5)) { ns = WEB_NS_MATHML; name = frag + 5; }
        WebNode *ctx = web_doc_create_element_ns(d, ns, name);
        WebNode *fragment = web_doc_create_fragment(d);
        parse_fragment(d, ctx, in, n, fragment);
        web_dump_dom(fragment, writer, out);
    } else {
        web_doc_parse(d, in, n, "utf-8");
        web_dump_dom(web_doc_root(d), writer, out);
    }
    web_doc_free(d);
}

/* ------------------------------------------------------------------------------------------------ selectors and styles */

/* the payload of spec/select/style: "line 1\nline 2\nrest" */
static const char *line_of(const char **p, const char *end, size_t *n) {
    const char *s = *p;
    const char *e = memchr(s, '\n', (size_t)(end - s));
    if (!e) e = end;
    *n = (size_t)(e - s);
    *p = e < end ? e + 1 : end;
    return s;
}

static char *dupn(const char *s, size_t n) {
    char *r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static void describe(Buf *o, WebNode *el) {
    bputs(o, web_node_name(el));
    const char *id = web_element_id(el);
    if (*id) { bputs(o, "#"); bputs(o, id); }
    const char *cls = web_element_get_attr(el, "class");
    if (cls) {
        for (const char *p = cls; *p;) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *s = p;
            while (*p && *p != ' ') p++;
            bputs(o, ".");
            bput(o, s, (size_t)(p - s));
        }
    }
}

/* a loader for tests: http://example.org/PATH is the file DIR/PATH (answered from inside fetch) */
static char loader_root[512];
static uint32_t loader_next;

static uint32_t test_fetch(void *user, WebDoc *d, const char *url, WebResourceKind kind) {
    (void)user;
    (void)kind;
    const char *pre = "http://example.org/";
    if (!loader_root[0] || strncmp(url, pre, strlen(pre))) return 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", loader_root, url + strlen(pre));
    char *q = strpbrk(path + strlen(loader_root), "?#");
    if (q) *q = 0;
    uint32_t id = ++loader_next;
    FILE *f = fopen(path, "rb");
    if (!f) { web_doc_resource_done(d, id, -1, NULL, 0, NULL); return id; }
    Buf b = { 0 };
    char tmp[65536];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, k);
    fclose(f);
    web_doc_resource_done(d, id, 0, b.p ? b.p : "", b.n, NULL);
    free(b.p);
    return id;
}

static const WebLoader test_loader = { NULL, test_fetch, NULL };

static const char *opt_str(const char *opts, const char *key, char *out, size_t cap) {
    const char *p = strstr(opts, key);
    if (!p) return NULL;
    p += strlen(key);
    size_t k = strcspn(p, ",");
    if (k >= cap) k = cap - 1;
    memcpy(out, p, k);
    out[k] = 0;
    return out;
}

static int opt_int(const char *opts, const char *key, int dflt) {
    char tmp[32];
    return opt_str(opts, key, tmp, sizeof tmp) ? atoi(tmp) : dflt;
}

static WebDoc *doc_with(const char *opts, const char *html, size_t n) {
    WebConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    if (strstr(opts, "dark")) cfg.prefers_dark = true;
    if (strstr(opts, "print")) cfg.print = true;
    if (strstr(opts, "script")) cfg.scripts = &dummy_scripts;
    cfg.memory_limit = (size_t)opt_int(opts, "limit=", 0);
    cfg.scale = opt_int(opts, "scale=", 1);
    loader_root[0] = 0;
    if (opt_str(opts, "root=", loader_root, sizeof loader_root)) {
        cfg.loader = &test_loader;
        cfg.images = web_image_libimg();
    }
    WebDoc *d = web_doc_new(&cfg, loader_root[0] ? "http://example.org/page.html" : "http://example.org/dir/page.html");
    if (!d) return NULL;
    const char *w = strstr(opts, "w="), *h = strstr(opts, "h=");
    if (w) d->vw = atoi(w + 2);
    if (h) d->vh = atoi(h + 2);
    web_doc_parse(d, html, n, "utf-8");
    return d;
}

static void cmd_spec(Buf *out, const char *in, size_t n) {
    WebDoc *d = web_doc_new(NULL, NULL);
    SelList l;
    if (!sel_parse_text(d, &d->arena, in, n, &l)) bputs(out, "invalid");
    else
        for (uint32_t i = 0; i < l.n; i++) {
            uint32_t s = l.v[i].spec;
            bprintf(out, "%s%u,%u,%u", i ? " " : "", s >> 20, (s >> 10) & 1023, s & 1023);
        }
    web_doc_free(d);
}

static void cmd_select(Buf *out, const char *opts, const char *in, size_t n) {
    const char *p = in, *end = in + n;
    size_t sl;
    const char *sel = line_of(&p, end, &sl);
    char *s = dupn(sel, sl);
    WebDoc *d = doc_with(opts, p, (size_t)(end - p));
    int count = 0, err = 0;
    WebNode **v = web_query_selector_all(web_doc_root(d), s, &count, &err);
    if (err) bputs(out, "invalid");
    for (int i = 0; i < count; i++) {
        if (i) bputs(out, " ");
        describe(out, v[i]);
    }
    if (v) web_free(d, v);
    free(s);
    web_doc_free(d);
}

/* style: line 1 a selector, line 2 the properties (space separated; "before:content" style for pseudo-elements) */
static void cmd_style(Buf *out, const char *opts, const char *in, size_t n) {
    const char *p = in, *end = in + n;
    size_t sl, pl;
    const char *sel = line_of(&p, end, &sl);
    const char *props = line_of(&p, end, &pl);
    char *s = dupn(sel, sl), *pr = dupn(props, pl);
    WebDoc *d = doc_with(opts, p, (size_t)(end - p));
    style_update(d);
    int count = 0, err = 0;
    WebNode **v = web_query_selector_all(web_doc_root(d), s, &count, &err);
    if (err) bputs(out, "invalid selector\n");
    for (int i = 0; i < count; i++) {
        describe(out, v[i]);
        char *save;
        char *copy = dupn(pr, strlen(pr));
        for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            const CStyle *st = v[i]->u.el.style;
            const char *name = tok;
            int pk = -1;
            if (!strncmp(tok, "before:", 7)) { pk = 0; name = tok + 7; }
            else if (!strncmp(tok, "after:", 6)) { pk = 1; name = tok + 6; }
            else if (!strncmp(tok, "marker:", 7)) { pk = 2; name = tok + 7; }
            if (pk >= 0) st = v[i]->u.el.pstyle[pk];
            bprintf(out, "\t%s=", tok);
            if (!st) { bputs(out, "-"); continue; }
            if (!strcmp(name, "content")) {
                const CssContent *ct = st->box->content;
                if (!ct || ct->normal) bputs(out, "normal");
                else if (ct->none) bputs(out, "none");
                else for (int k = 0; k < ct->n; k++) {
                    if (k) bputs(out, "+");
                    if (ct->v[k].kind == CC_STRING) { bputs(out, "'"); bputs(out, ct->v[k].s); bputs(out, "'"); }
                    else if (ct->v[k].kind == CC_ATTR) bprintf(out, "attr(%s)", atom_str(&d->atoms, ct->v[k].name));
                    else if (ct->v[k].kind == CC_COUNTER) bprintf(out, "counter(%s)", atom_str(&d->atoms, ct->v[k].name));
                    else bprintf(out, "q%d", ct->v[k].kind);
                }
                continue;
            }
            int prop = css_property(name, strlen(name));
            char *val = prop >= 0 ? style_serialize(d, st, prop) : NULL;
            bputs(out, val ? val : "?");
            if (val) web_free(d, val);
        }
        free(copy);
        bputs(out, "\n");
    }
    if (v) web_free(d, v);
    free(s);
    free(pr);
    web_doc_free(d);
}

/* ------------------------------------------------------------------------------------------------ layout */

void web_px_str(WebUnit u, char *out, size_t cap);      /* ldump.c */

static const char *px(WebUnit u, char *buf) { web_px_str(u, buf, 32); return buf; }

static void layout_of(WebDoc *d, const char *opts) {
    web_doc_layout(d, opt_int(opts, "w=", 800), opt_int(opts, "h=", 600));
}

static void cmd_layout(Buf *out, const char *opts, const char *in, size_t n) {
    WebDoc *d = doc_with(opts, in, n);
    if (!d) { bputs(out, "#nomem"); return; }
    layout_of(d, opts);
    if (web_doc_status(d) != WEB_OK) bputs(out, "#status nomem\n");
    char a[32], b[32], c[32], e[32];
    WebNode *root = web_doc_root(d);
    for (WebNode *el = root->first; el; el = dom_next(el, root)) {
        if (el->type != WEB_NODE_ELEMENT || !el->u.el.id) continue;
        const char *id = atom_str(&d->atoms, el->u.el.id);
        WebRect r;
        if (!web_element_rect(el, &r)) { bprintf(out, "#%s none\n", id); continue; }
        bprintf(out, "#%s %s %s %s %s\n", id, px(r.x, a), px(r.y, b), px(r.w, c), px(r.h, e));
        LBox *bx = el->box;
        if (!bx || !(bx->flags & BF_INLINE_CONTENT)) continue;
        WebUnit ax, ay;
        box_abs(bx, &ax, &ay);
        bprintf(out, "#%s lines %u\n", id, bx->nlines);
        for (uint32_t i = 0; i < bx->nlines; i++) {
            const Line *ln = &bx->lines[i];
            bprintf(out, "#%s line %u %s %s %s\n", id, i, px(ay + ln->y, a), px(ln->h, b), px(ay + ln->baseline, c));
        }
        for (uint32_t i = 0; i < bx->nfrags; i++) {
            const Frag *f = &bx->frags[i];
            if (f->kind != FR_TEXT || !f->len) continue;
            bprintf(out, "#%s frag \"", id);
            bput(out, f->box->text + f->start, f->len);
            bprintf(out, "\" %s %s %s\n", px(ax + f->x, a), px(ay + f->y, b), px(f->w, c));
        }
    }
    int cw, ch;
    web_doc_content_size(d, &cw, &ch);
    bprintf(out, "size %d %d\n", cw, ch);
    web_doc_free(d);
}

static void cmd_dump(Buf *out, const char *opts, const char *in, size_t n) {
    WebDoc *d = doc_with(opts, in, n);
    if (!d) { bputs(out, "#nomem"); return; }
    layout_of(d, opts);
    web_dump_layout(d, writer, out, WEB_DUMP_TEXT | (strstr(opts, "style") ? WEB_DUMP_STYLE : 0));
    web_doc_free(d);
}

static const char *const di_names[] = { "?", "rect", "border", "text", "image", "clip", "/clip", "opacity", "/opacity", "scroll",
                                        "/scroll", "control", "ellipse", "shadow" };

static void node_desc(Buf *o, WebNode *n) {
    if (!n) { bputs(o, "-"); return; }
    if (n->type == WEB_NODE_TEXT) { bputs(o, "#text"); return; }
    describe(o, n);
}

static void cmd_paint(Buf *out, const char *opts, const char *in, size_t n) {
    WebDoc *d = doc_with(opts, in, n);
    if (!d) { bputs(out, "#nomem"); return; }
    int w = opt_int(opts, "w=", 800), h = opt_int(opts, "h=", 600);
    web_doc_layout(d, w, h);
    const WebDisplayList *dl = web_doc_paint(d, opt_int(opts, "x=", 0), opt_int(opts, "y=", 0), w, h);
    bprintf(out, "background %08x\n", dl->background);
    for (int i = 0; i < dl->n; i++) {
        const WebDisplayItem *it = &dl->items[i];
        bprintf(out, "%s", it->kind < sizeof di_names / sizeof di_names[0] ? di_names[it->kind] : "?");
        if (it->kind == WEB_DI_CLIP_POP || it->kind == WEB_DI_OPACITY_POP || it->kind == WEB_DI_SCROLL_POP) { bputs(out, "\n"); continue; }
        bprintf(out, " %d %d %d %d", it->rect.x, it->rect.y, it->rect.w, it->rect.h);
        switch (it->kind) {
        case WEB_DI_RECT: case WEB_DI_ELLIPSE: case WEB_DI_SHADOW: bprintf(out, " %08x", it->color); break;
        case WEB_DI_TEXT:
            bprintf(out, " %08x \"", it->color);
            bput(out, it->text, it->len);
            bprintf(out, "\" @%g,%g", it->x64 / 64.0, it->y64 / 64.0);
            break;
        case WEB_DI_BORDER:
            bprintf(out, " w=%u,%u,%u,%u s=%u,%u,%u,%u c=%08x", it->widths[0], it->widths[1], it->widths[2], it->widths[3],
                    it->styles[0], it->styles[1], it->styles[2], it->styles[3], it->colors[0]);
            break;
        case WEB_DI_OPACITY_PUSH: bprintf(out, " a=%u", it->alpha); break;
        case WEB_DI_SCROLL_PUSH: bprintf(out, " s=%d,%d", it->sx, it->sy); break;
        case WEB_DI_CONTROL:
            bprintf(out, " k=%u st=%u \"", it->control, it->state);
            bput(out, it->text ? it->text : "", it->len);
            bputs(out, "\"");
            break;
        case WEB_DI_IMAGE: bprintf(out, " img=%s", it->image ? "yes" : "no"); break;
        default: break;
        }
        if (it->kind != WEB_DI_TEXT && it->node) { bputs(out, " "); node_desc(out, it->node); }
        bputs(out, "\n");
    }
    web_doc_free(d);
}

static void cmd_hit(Buf *out, const char *opts, const char *in, size_t n) {
    const char *p = in, *end = in + n;
    size_t pl;
    const char *pt = line_of(&p, end, &pl);
    int x = atoi(pt), y = 0;
    const char *sp = memchr(pt, ' ', pl);
    if (sp) y = atoi(sp + 1);
    WebDoc *d = doc_with(opts, p, (size_t)(end - p));
    if (!d) { bputs(out, "#nomem"); return; }
    layout_of(d, opts);
    if (opt_int(opts, "sy=", 0)) web_doc_set_scroll(d, 0, opt_int(opts, "sy=", 0));
    WebHit h;
    if (!web_doc_hit_test(d, x, y, &h)) bputs(out, "miss");
    else {
        bputs(out, "node=");
        node_desc(out, h.node);
        if (h.node && h.node->type == WEB_NODE_TEXT) bprintf(out, " offset=%d", h.offset);
        bputs(out, " element=");
        node_desc(out, h.element);
        if (h.link) bprintf(out, " href=%s", h.href ? h.href : "?");
        if (h.control) { bputs(out, " control="); node_desc(out, h.control); }
        bprintf(out, " cursor=%d", h.cursor);
        WebNode *sc = web_doc_scroller_at(d, x, y);
        if (sc) { bputs(out, " scroller="); node_desc(out, sc); }
    }
    web_doc_free(d);
}

static void cmd_find(Buf *out, const char *opts, const char *in, size_t n) {
    const char *p = in, *end = in + n;
    size_t nl;
    const char *nd = line_of(&p, end, &nl);
    char *needle = dupn(nd, nl);
    WebDoc *d = doc_with(opts, p, (size_t)(end - p));
    if (!d) { free(needle); bputs(out, "#nomem"); return; }
    layout_of(d, opts);
    WebRect r;
    int count = web_doc_find(d, needle, opt_int(opts, "which=", 0), &r);
    char a[32], b[32], c[32], e[32];
    bprintf(out, "count=%d rect=%s,%s,%s,%s", count, px(r.x, a), px(r.y, b), px(r.w, c), px(r.h, e));
    size_t sl;
    char *sel = web_doc_selection_text(d, &sl);
    if (sel) { bputs(out, " selected=\""); bput(out, sel, sl); bputs(out, "\""); web_free(d, sel); }
    free(needle);
    web_doc_free(d);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void cmd_bench(Buf *out, const char *opts, const char *in, size_t n) {
    WebConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    double t0 = now_ms();
    WebDoc *d = web_doc_new(&cfg, "http://example.org/");
    web_doc_parse(d, in, n, "utf-8");
    double t1 = now_ms();
    style_update(d);
    double t2 = now_ms();
    web_doc_layout(d, opt_int(opts, "w=", 1024), opt_int(opts, "h=", 768));
    double t3 = now_ms();
    const WebDisplayList *dl = web_doc_paint(d, 0, 0, 1024, 768);
    double t4 = now_ms();
    int cw, ch;
    web_doc_content_size(d, &cw, &ch);
    bprintf(out, "bytes=%zu parse=%.1f style=%.1f layout=%.1f paint=%.1f boxes=%u items=%d height=%d memory=%zu", n, t1 - t0, t2 - t1,
            t3 - t2, t4 - t3, d->lay ? d->lay->nboxes : 0, dl->n, ch, d->budget.used);
    web_doc_free(d);
}

/* ------------------------------------------------------------------------------------------------ a reference rasteriser */

typedef struct Canvas {
    int w, h;
    uint32_t *px;
    int cx0[64], cy0[64], cx1[64], cy1[64];
    int nclip;
    uint8_t alpha[64];
    int nalpha;
} Canvas;

static void clip_now(Canvas *c, int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = c->w; *y1 = c->h;
    for (int i = 0; i < c->nclip && i < 64; i++) {
        if (c->cx0[i] > *x0) *x0 = c->cx0[i];
        if (c->cy0[i] > *y0) *y0 = c->cy0[i];
        if (c->cx1[i] < *x1) *x1 = c->cx1[i];
        if (c->cy1[i] < *y1) *y1 = c->cy1[i];
    }
}

static unsigned group_alpha(const Canvas *c) {
    unsigned a = 255;
    for (int i = 0; i < c->nalpha && i < 64; i++) a = a * c->alpha[i] / 255;
    return a;
}

static void blend(Canvas *c, int x, int y, uint32_t col) {
    int x0, y0, x1, y1;
    clip_now(c, &x0, &y0, &x1, &y1);
    if (x < x0 || y < y0 || x >= x1 || y >= y1) return;
    unsigned a = (col >> 24) * group_alpha(c) / 255;
    if (!a) return;
    uint32_t *d = &c->px[y * c->w + x];
    unsigned r = ((col >> 16) & 255) * a + ((*d >> 16) & 255) * (255 - a);
    unsigned g = ((col >> 8) & 255) * a + ((*d >> 8) & 255) * (255 - a);
    unsigned b = (col & 255) * a + (*d & 255) * (255 - a);
    *d = 0xFF000000u | (r / 255) << 16 | (g / 255) << 8 | (b / 255);
}

static void fill(Canvas *c, int x, int y, int w, int h, uint32_t col) {
    int x0, y0, x1, y1;
    clip_now(c, &x0, &y0, &x1, &y1);
    for (int j = y < y0 ? y0 : y; j < y + h && j < y1; j++)
        for (int i = x < x0 ? x0 : x; i < x + w && i < x1; i++) blend(c, i, j, col);
}

static void glyph(Canvas *c, int x, int y, int size, unsigned cp, uint32_t col) {
    const unsigned char *g = font8x16_glyph(cp);
    int gw = cp >= 0x1100 && size > 0 ? size : size / 2, gh = size;
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            int sy = j * 16 / gh, sx = i * 8 / (gw > 0 ? gw : 1);
            if (sx < 8 && (g[sy] & (0x80 >> sx))) blend(c, x + i, y + j, col);
        }
}

static void draw_text(Canvas *c, const char *s, size_t n, int x, int baseline, int size, uint32_t col) {
    int top = baseline - size * 3 / 4;
    for (size_t i = 0; i < n;) {
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        i += len;
        if (cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x300 && cp <= 0x36F)) continue;
        glyph(c, x, top, size, cp, col);
        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                    (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x20000 && cp <= 0x3FFFD);
        x += wide ? size : size / 2;
    }
}

static void render_list(Canvas *c, const WebDisplayList *dl) {
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = dl->background | 0xFF000000u;
    for (int k = 0; k < dl->n; k++) {
        const WebDisplayItem *it = &dl->items[k];
        const WebIRect *r = &it->rect;
        switch (it->kind) {
        case WEB_DI_RECT: case WEB_DI_SHADOW: fill(c, r->x, r->y, r->w, r->h, it->color); break;
        case WEB_DI_ELLIPSE:
            for (int j = 0; j < r->h; j++)
                for (int i = 0; i < r->w; i++) {
                    double dx = (i + 0.5) / r->w * 2 - 1, dy = (j + 0.5) / r->h * 2 - 1, q = dx * dx + dy * dy;
                    if (q <= 1 && (!it->width || q >= 0.45)) blend(c, r->x + i, r->y + j, it->color);
                }
            break;
        case WEB_DI_BORDER: {
            const int *w = NULL;
            (void)w;
            int t = it->widths[0], rr = it->widths[1], b = it->widths[2], l = it->widths[3];
            if (t && it->styles[0]) fill(c, r->x, r->y, r->w, t, it->colors[0]);
            if (b && it->styles[2]) fill(c, r->x, r->y + r->h - b, r->w, b, it->colors[2]);
            if (l && it->styles[3]) fill(c, r->x, r->y, l, r->h, it->colors[3]);
            if (rr && it->styles[1]) fill(c, r->x + r->w - rr, r->y, rr, r->h, it->colors[1]);
            break;
        }
        case WEB_DI_TEXT: {
            int size = (int)(builtin_handle_size(it->font) / 64);
            draw_text(c, it->text, it->len, it->x64 / 64, it->y64 / 64, size > 0 ? size : 16, it->color);
            break;
        }
        case WEB_DI_IMAGE: {
            const Image *im = (const Image *)it->image;
            if (!im) { fill(c, r->x, r->y, r->w, r->h, 0x30808080u); break; }
            for (int j = 0; j < r->h; j++)
                for (int i = 0; i < r->w; i++) {
                    int sx = (int)((int64_t)i * im->w / (r->w ? r->w : 1)), sy = (int)((int64_t)j * im->h / (r->h ? r->h : 1));
                    blend(c, r->x + i, r->y + j, im->px[sy * im->w + sx]);
                }
            break;
        }
        case WEB_DI_CONTROL: {
            int size = (int)(builtin_handle_size(it->font) / 64);
            if (size <= 0) size = 16;
            if (it->control == WEB_CONTROL_CHECKBOX || it->control == WEB_CONTROL_RADIO) {
                fill(c, r->x, r->y, r->w, r->h, 0xFF606060u);
                fill(c, r->x + 1, r->y + 1, r->w - 2, r->h - 2, 0xFFFFFFFFu);
                if (it->state & WEB_CTL_CHECKED) fill(c, r->x + 3, r->y + 3, r->w - 6, r->h - 6, 0xFF2060C0u);
            } else if (it->text && it->len) {
                uint32_t col = (it->state & WEB_CTL_PLACEHOLDER) ? 0xFF909090u : it->color;
                draw_text(c, it->text, it->len, r->x + 4, r->y + (r->h - size) / 2 + size * 3 / 4, size, col);
            }
            break;
        }
        case WEB_DI_CLIP_PUSH:
            if (c->nclip < 64) { c->cx0[c->nclip] = r->x; c->cy0[c->nclip] = r->y; c->cx1[c->nclip] = r->x + r->w; c->cy1[c->nclip] = r->y + r->h; }
            c->nclip++;
            break;
        case WEB_DI_CLIP_POP: if (c->nclip) c->nclip--; break;
        case WEB_DI_OPACITY_PUSH: if (c->nalpha < 64) c->alpha[c->nalpha] = it->alpha; c->nalpha++; break;
        case WEB_DI_OPACITY_POP: if (c->nalpha) c->nalpha--; break;
        default: break;
        }
    }
}

static int render_file(const char *path, const char *opts, const char *html, size_t n, Buf *out) {
    WebDoc *d = doc_with(opts, html, n);
    if (!d) return -1;
    int w = opt_int(opts, "w=", 800), h = opt_int(opts, "h=", 600);
    web_doc_layout(d, w, h);
    int cw, ch;
    web_doc_content_size(d, &cw, &ch);
    if (strstr(opts, "full")) h = ch > 8000 ? 8000 : ch;
    const WebDisplayList *dl = web_doc_paint(d, 0, 0, w, h);
    Canvas c;
    memset(&c, 0, sizeof c);
    c.w = w;
    c.h = h;
    c.px = malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
    if (!c.px) { web_doc_free(d); return -1; }
    render_list(&c, dl);
    Image im = { w, h, c.px, 0 };
    int rc = img_save_ppm(path, &im, 0xFFFFFF);
    uint64_t sum = 0;
    for (int i = 0; i < w * h; i++) sum = sum * 31 + (c.px[i] & 0xFFFFFF);
    if (out) bprintf(out, "ppm=%s rc=%d size=%dx%d items=%d content=%dx%d checksum=%016llx", path, rc, w, h, dl->n, cw, ch,
                     (unsigned long long)sum);
    free(c.px);
    web_doc_free(d);
    return rc;
}

static void cmd_render(Buf *out, const char *opts, const char *in, size_t n) {
    const char *p = in, *end = in + n;
    size_t pl;
    const char *pt = line_of(&p, end, &pl);
    char *path = dupn(pt, pl);
    render_file(path, opts, p, (size_t)(end - p), out);
    free(path);
}

/* ------------------------------------------------------------------------------------------------ main */

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    Buf b = { 0 };
    char tmp[65536];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, k);
    fclose(f);
    *n = b.n;
    if (!b.p) bput(&b, "", 0);
    return b.p;
}

static void answer(Buf *out) {
    printf("%zu\n", out->n);
    fwrite(out->p ? out->p : "", 1, out->n, stdout);
    fflush(stdout);
    out->n = 0;
}

static int batch(void) {
    char line[4096];
    Buf out = { 0 };
    while (fgets(line, sizeof line, stdin)) {
        char *argv[16];
        int argc = 0;
        for (char *p = strtok(line, " \n"); p && argc < 16; p = strtok(NULL, " \n")) argv[argc++] = p;
        if (argc < 2) continue;
        size_t len = strtoul(argv[argc - 1], NULL, 10);
        char *payload = malloc(len + 1);
        if (!payload || fread(payload, 1, len, stdin) != len) { fprintf(stderr, "short payload\n"); return 1; }
        payload[len] = 0;
        if (!strcmp(argv[0], "tokens") && argc >= 4) cmd_tokens(&out, argv[1], argv[2], payload, len);
        else if (!strcmp(argv[0], "tree") && argc >= 3) cmd_tree(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "spec")) cmd_spec(&out, payload, len);
        else if (!strcmp(argv[0], "select") && argc >= 3) cmd_select(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "style") && argc >= 3) cmd_style(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "layout") && argc >= 3) cmd_layout(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "dump") && argc >= 3) cmd_dump(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "paint") && argc >= 3) cmd_paint(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "hit") && argc >= 3) cmd_hit(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "find") && argc >= 3) cmd_find(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "render") && argc >= 3) cmd_render(&out, argv[1], payload, len);
        else if (!strcmp(argv[0], "bench") && argc >= 3) cmd_bench(&out, argv[1], payload, len);
        else bprintf(&out, "#unknown command %s", argv[0]);
        free(payload);
        answer(&out);
    }
    free(out.p);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return batch();
    Buf out = { 0 };
    if (!strcmp(argv[1], "tree") && argc >= 3) {
        size_t n;
        char *in = read_file(argv[2], &n);
        cmd_tree(&out, argc >= 4 ? argv[3] : "-", in, n);
        fwrite(out.p, 1, out.n, stdout);
        free(in);
    } else if (!strcmp(argv[1], "render") && argc >= 4) {
        size_t n;
        char *in = read_file(argv[2], &n);
        char opts[128];
        snprintf(opts, sizeof opts, "w=%s,h=%s,full", argc >= 5 ? argv[4] : "800", argc >= 6 ? argv[5] : "600");
        render_file(argv[3], opts, in, n, &out);
        fwrite(out.p, 1, out.n, stdout);
        putchar('\n');
        free(in);
    } else if (!strcmp(argv[1], "dump") && argc >= 3) {
        size_t n;
        char *in = read_file(argv[2], &n);
        char opts[64];
        snprintf(opts, sizeof opts, "w=%s", argc >= 4 ? argv[3] : "800");
        cmd_dump(&out, opts, in, n);
        fwrite(out.p, 1, out.n, stdout);
        free(in);
    } else {
        fprintf(stderr, "usage: webtool (batch mode on stdin) | webtool tree FILE [OPTS] | webtool render FILE OUT.ppm [W [H]] | "
                        "webtool dump FILE [W]\n");
        return 2;
    }
    free(out.p);
    return 0;
}
