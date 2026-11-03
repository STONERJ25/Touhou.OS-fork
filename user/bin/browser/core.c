/*
 * The browser core, part one: life, navigation and history, loading, rendering. Interaction is in input.c, forms in forms.c,
 * generated pages in errpage.c. See core.h for the picture.
 */
#include "core.h"
#include "urls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---------------------------------------------------------------- small helpers */

void core_invalidate(BrCore *c) {
    c->dirty = true;
    if (c->host.invalidate) c->host.invalidate(c->host.user);
}

static void set_status(BrCore *c, const char *s) { snprintf(c->status, sizeof c->status, "%s", s); }

void brc_note(BrCore *c, const char *text) {
    snprintf(c->message, sizeof c->message, "%s", text);
    c->message_until = c->now_ms + 6000;
    core_invalidate(c);
}

static bool page_busy(const BrPage *p) { return p && (p->state == BR_PG_LOADING || p->nsub > p->sub_done); }

bool brc_busy(const BrCore *c) { return c->next != NULL || page_busy(c->page); }
unsigned brc_loads(const BrCore *c) { return c->loads; }

static const char *error_kind(int e) {
    switch (e) {
    case BR_E_DNS: return "dns";
    case BR_E_CONNECT: return "connect";
    case BR_E_TIMEOUT: return "timeout";
    case BR_E_TLS: return "tls";
    case BR_E_NOTRUST: return "notrust";
    case BR_E_IO: return "io";
    case BR_E_TRUNCATED: return "truncated";
    case BR_E_PARSE: return "parse";
    case BR_E_REDIRECTS: return "redirects";
    case BR_E_TOO_BIG: return "toobig";
    case BR_E_NOT_FOUND: return "notfound";
    case BR_E_ACCESS: return "access";
    case BR_E_SCHEME: return "scheme";
    case BR_E_URL: return "url";
    default: return "io";
    }
}

/* ---------------------------------------------------------------- engine side: documents and their loader */

static uint32_t eng_fetch(void *user, WebDoc *doc, const char *url, WebResourceKind kind) {
    BrCore *c = user;
    BrPage *p = c->page && c->page->doc == doc ? c->page : (c->next && c->next->doc == doc ? c->next : NULL);
    if (!p) return 0;
    BrUrlKind k = br_url_kind(url);
    if (k == BR_URL_FILE && br_url_kind(p->url) != BR_URL_FILE && p->kind != BR_PK_GENERATED) return 0;     /* a web page may not read this computer's files */
    if (k != BR_URL_HTTP && k != BR_URL_HTTPS && k != BR_URL_FILE && k != BR_URL_DATA) return 0;
    if (p->prefetch && strcmp(url, p->prefetch_url) == 0) return 0x80000001u;                               /* the picture the page is about: answered from memory */
    if (p->nsub >= BR_MAX_SUBRES) return 0;
    BrFetchKind fk = kind == WEB_RES_STYLESHEET ? BR_F_STYLE : kind == WEB_RES_IMAGE ? BR_F_IMAGE : kind == WEB_RES_SCRIPT ? BR_F_SCRIPT : BR_F_OTHER;
    uint32_t id = brl_start(c->loader, url, fk, p->gen, BR_MAX_SUB_BYTES);
    if (id) p->nsub++;
    return id;
}

static void eng_cancel(void *user, WebDoc *doc, uint32_t id) { (void)user; (void)doc; (void)id; }

static bool start_document(BrCore *c, BrPage *p, const char *url) {
    WebConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fonts = brf_provider(c->fonts);
    cfg.loader = &c->wloader;
    cfg.images = web_image_libimg();
    cfg.scale = c->scale;
    const char *mem = getenv("BROWSER_MEMORY_LIMIT");                    /* a knob for the tests: a smaller limit */
    cfg.memory_limit = mem && atol(mem) > 0 ? (size_t)atol(mem) : BR_MEMORY_LIMIT;
    cfg.scripts = NULL;                     /* the one place scripts would be hooked in: a WebScriptHost on Kosuzu (docs/BROWSER.md "Scripts") */
    p->doc = web_doc_new(&cfg, url);
    p->layout_dirty = true;
    return p->doc != NULL;
}

static void page_free(BrCore *c, BrPage *p) {
    if (!p) return;
    brl_cancel_gen(c->loader, p->gen);
    dropdown_close(c, p);
    if (p->doc) web_doc_free(p->doc);
    p->doc = NULL;
    brp_reset_images(c->painter);
    free(p->body);
    free(p->prefetch);
    free(p);
}

static BrPage *page_new(BrCore *c, const char *url) {
    BrPage *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->gen = ++c->gen;
    p->restore_y = -1;
    snprintf(p->req_url, sizeof p->req_url, "%s", url);
    snprintf(p->url, sizeof p->url, "%s", url);
    return p;
}

/* ---------------------------------------------------------------- life */

BrCore *brc_new(const BrHost *host, int scale) {
    BrCore *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (host) c->host = *host;
    c->scale = scale < 1 ? 1 : scale;
    c->fonts = brf_new(NULL);
    c->painter = c->fonts ? brp_new(c->fonts) : NULL;
    c->loader = brl_new();
    if (!c->fonts || !c->painter || !c->loader) { brc_free(c); return NULL; }
    c->wloader.user = c;
    c->wloader.fetch = eng_fetch;
    c->wloader.cancel = eng_cancel;
    if (!getcwd(c->cwd, sizeof c->cwd)) c->cwd[0] = 0;
    const char *h = getenv("HOME");
    snprintf(c->home, sizeof c->home, "%s", h ? h : "");
    c->vw = 800;
    c->vh = 600;
    c->hpos = -1;
    return c;
}

void brc_free(BrCore *c) {
    if (!c) return;
    page_free(c, c->page);
    page_free(c, c->next);
    brl_free(c->loader);
    brp_free(c->painter);
    brf_free(c->fonts);
    free(c);
}

int brc_wake_fd(const BrCore *c) { return brl_wake_fd(c->loader); }

/* ---------------------------------------------------------------- showing documents */

void core_set_title(BrCore *c) {
    BrPage *p = c->page;
    if (!p) return;
    const char *t = p->doc ? web_doc_title(p->doc) : "";
    if (t && *t) snprintf(p->title, sizeof p->title, "%s", t);
    else br_url_display(p->url, p->title, sizeof p->title, 120);
}

static void after_parse(BrCore *c, BrPage *p) {
    if (p->state != BR_PG_LOADING) c->loads++;
    p->layout_dirty = true;
    if (p->fragment[0] && p->doc) p->want_fragment = true;
    core_set_title(c);
    core_invalidate(c);
}

/* the incoming page has something to show: it replaces the one on screen */
static void commit_next(BrCore *c, BrPage *p) {
    if (p != c->next) return;
    BrPage *old = c->page;
    c->page = p;
    c->next = NULL;
    c->message[0] = 0;
    page_free(c, old);
}

static void show_html(BrCore *c, BrPage *p, char *html, size_t len, BrPageState state) {
    if (p == c->next) commit_next(c, p);
    if (p->doc) { web_doc_free(p->doc); p->doc = NULL; brp_reset_images(c->painter); }
    if (start_document(c, p, p->url)) web_doc_parse(p->doc, html, len, "utf-8");
    free(html);
    p->kind = BR_PK_GENERATED;
    p->state = state;
    p->main_id = 0;
    after_parse(c, p);
}

static void show_info(BrCore *c, BrPage *p, const char *kind, int status, const char *detail, const char *extra) {
    BrPageInfo info = { kind, status, p->req_url, detail, extra };
    size_t len;
    char *html = br_error_page(&info, &len);
    if (html) show_html(c, p, html, len, BR_PG_FAILED);
}

static void show_blank(BrCore *c, BrPage *p) {
    size_t len;
    char *html = br_blank_page(&len);
    if (html) show_html(c, p, html, len, BR_PG_DONE);
}

static void show_about(BrCore *c, BrPage *p, const char *base) {
    size_t len;
    char *html = NULL;
    if (strcasecmp(base, "about:blank") == 0) { show_blank(c, p); return; }
    if (strcasecmp(base, "about:browser") == 0) html = br_about_page(&len, brf_file_count(c->fonts), BRF_DEFAULT_DIR);
    else if (strcasecmp(base, "about:home") == 0) html = br_home_page(&len, c->home);
    if (html) { show_html(c, p, html, len, BR_PG_DONE); return; }
    show_info(c, p, "notfound", 0, "there is no such about: page (about:home, about:browser and about:blank exist)", NULL);
}

/* ---------------------------------------------------------------- navigation and history */

static void save_scroll(BrCore *c) {
    if (c->page && c->hpos >= 0 && c->hpos < c->nhist) c->hist[c->hpos].scroll_y = c->page->scroll_y;
}

static void open_url(BrCore *c, const char *url, int restore_y) {
    BrPage *old_next = c->next;
    c->next = NULL;
    page_free(c, old_next);
    BrPage *p = page_new(c, url);
    if (!p) return;
    p->restore_y = restore_y;
    char base[BR_MAX_URL];
    const char *frag;
    br_url_split_fragment(url, base, sizeof base, &frag);
    if (frag && *frag) snprintf(p->fragment, sizeof p->fragment, "%s", frag);
    c->next = p;
    p->state = BR_PG_LOADING;
    c->message[0] = 0;
    BrUrlKind k = br_url_kind(base);
    if (k == BR_URL_ABOUT) { show_about(c, p, base); return; }
    if (k == BR_URL_OTHER || k == BR_URL_NONE) {
        show_info(c, p, "scheme", 0, "that kind of address cannot be fetched", NULL);
        return;
    }
    const char *limit = getenv("BROWSER_MAX_DOC_BYTES");                  /* a knob for the tests: a smaller page limit */
    size_t max_doc = limit && atol(limit) > 0 ? (size_t)atol(limit) : BR_MAX_DOC_BYTES;
    p->main_id = brl_start(c->loader, base, BR_F_MAIN, p->gen, max_doc);
    if (!p->main_id) show_info(c, p, "io", 0, "the browser could not start the download", NULL);
    core_invalidate(c);
}

static void push_history(BrCore *c, const char *url) {
    if (c->hpos + 1 < c->nhist) c->nhist = c->hpos + 1;                   /* a new page forgets the forward list */
    if (c->nhist == BR_HIST_MAX) { memmove(&c->hist[0], &c->hist[1], (BR_HIST_MAX - 1) * sizeof c->hist[0]); c->nhist--; }
    BrHist *h = &c->hist[c->nhist++];
    snprintf(h->url, sizeof h->url, "%s", url);
    h->scroll_y = 0;
    c->hpos = c->nhist - 1;
}

void brc_navigate(BrCore *c, const char *url, BrNav how) {
    if (!url || !*url) return;
    save_scroll(c);
    if (how == BR_NAV_RELOAD) {
        if (c->hpos >= 0) open_url(c, c->hist[c->hpos].url, c->page ? c->page->scroll_y : -1);
        else open_url(c, url, -1);
        return;
    }
    if ((how == BR_NAV_LINK || how == BR_NAV_REPLACE) && c->page && c->page->doc && !c->next && br_url_same_document(c->page->url, url) && strchr(url, '#')) {
        const char *frag = strchr(url, '#') + 1;                          /* the same page, another place in it: no fetch */
        if (how == BR_NAV_LINK) push_history(c, url);
        else if (c->hpos >= 0) snprintf(c->hist[c->hpos].url, sizeof c->hist[c->hpos].url, "%s", url);
        snprintf(c->page->fragment, sizeof c->page->fragment, "%s", frag);
        snprintf(c->page->url, sizeof c->page->url, "%s", url);
        c->page->want_fragment = true;
        core_invalidate(c);
        return;
    }
    if (how == BR_NAV_REPLACE && c->hpos >= 0) snprintf(c->hist[c->hpos].url, sizeof c->hist[c->hpos].url, "%s", url);
    else push_history(c, url);
    open_url(c, url, -1);
}

void brc_go(BrCore *c, const char *text) {
    char url[BR_MAX_URL];
    if (br_url_from_input(text, c->cwd, c->home, url, sizeof url)) { brc_navigate(c, url, BR_NAV_LINK); return; }
    BrPage *p = page_new(c, text);                                       /* text that cannot be an address: say so, in the page */
    if (!p) return;
    save_scroll(c);
    push_history(c, text);
    BrPage *old = c->next;
    c->next = p;
    page_free(c, old);
    show_info(c, p, "url", 0, text, NULL);
}

bool brc_can_back(const BrCore *c) { return c->hpos > 0; }
bool brc_can_forward(const BrCore *c) { return c->hpos + 1 < c->nhist; }

void brc_back(BrCore *c) {
    if (!brc_can_back(c)) return;
    save_scroll(c);
    c->hpos--;
    open_url(c, c->hist[c->hpos].url, c->hist[c->hpos].scroll_y);
}

void brc_forward(BrCore *c) {
    if (!brc_can_forward(c)) return;
    save_scroll(c);
    c->hpos++;
    open_url(c, c->hist[c->hpos].url, c->hist[c->hpos].scroll_y);
}

void brc_reload(BrCore *c) {
    if (c->hpos >= 0) brc_navigate(c, c->hist[c->hpos].url, BR_NAV_RELOAD);
}

void brc_stop(BrCore *c) {
    if (c->next) {                                                       /* the page that was on screen stays */
        BrPage *n = c->next;
        c->next = NULL;
        page_free(c, n);
        if (c->hpos >= 0 && c->page) {                                   /* the history points at the page we are still on */
            BrHist *h = &c->hist[c->hpos];
            if (strcmp(h->url, c->page->req_url) != 0) {
                for (int i = 0; i < c->nhist; i++) if (strcmp(c->hist[i].url, c->page->req_url) == 0) c->hpos = i;
            }
        }
        brc_note(c, "Stopped");
        core_invalidate(c);
        return;
    }
    BrPage *p = c->page;
    if (!p || !page_busy(p)) return;
    brl_cancel_gen(c->loader, p->gen);
    if (p->doc && p->state == BR_PG_LOADING && (p->kind == BR_PK_HTML || p->kind == BR_PK_TEXT)) web_doc_parse_finish(p->doc);
    p->state = BR_PG_DONE;
    p->main_id = 0;
    p->sub_done = p->nsub;
    brc_note(c, "Stopped");
    after_parse(c, p);
}

const char *brc_url(const BrCore *c) {
    if (c->next) return c->next->req_url;
    return c->page ? c->page->url : "";
}

const char *brc_title(const BrCore *c) {
    if (c->next && !c->page) return c->next->req_url;
    return c->page ? c->page->title : "";
}

/* ---------------------------------------------------------------- loading */

static bool is_html_type(const char *t) { return !strncasecmp(t, "text/html", 9) || !strncasecmp(t, "application/xhtml+xml", 21); }

static bool is_text_type(const char *t) {
    return !strncasecmp(t, "text/", 5) || !strncasecmp(t, "application/json", 16) || !strncasecmp(t, "application/javascript", 22) ||
           !strncasecmp(t, "application/x-javascript", 24) || !strncasecmp(t, "application/xml", 15) || strstr(t, "+xml") != NULL || strstr(t, "+json") != NULL;
}

static void copy_charset(const char *ctype, char *out, size_t cap) {
    out[0] = 0;
    const char *cs = NULL;
    for (const char *s = ctype; *s; s++) if (!strncasecmp(s, "charset=", 8)) { cs = s + 8; break; }
    if (!cs) return;
    if (*cs == '"') cs++;
    size_t n = strcspn(cs, ";\" \t");
    if (n >= cap) n = cap - 1;
    memcpy(out, cs, n);
    out[n] = 0;
}

static void human_size(unsigned long long n, char *out, size_t cap) {
    if (n < 10240) snprintf(out, cap, "%llu bytes", n);
    else if (n < 10u << 20) snprintf(out, cap, "%llu KiB", n / 1024);
    else snprintf(out, cap, "%llu MiB", n >> 20);
}

static const char *text_prefix(void) {
    return "<!doctype html><html><head><meta charset=\"utf-8\"></head><body style=\"margin:8px;background:#fff\"><plaintext style=\"white-space:pre-wrap;word-wrap:break-word;"
           "font-family:monospace;margin:0\">";
}

static void feed(BrCore *c, BrPage *p, const uint8_t *d, size_t n) {
    p->bytes += n;
    switch (p->kind) {
    case BR_PK_HTML: case BR_PK_TEXT:
        if (p->doc) web_doc_parse_bytes(p->doc, d, n, p->charset[0] ? p->charset : NULL);
        break;
    case BR_PK_IMAGE:
        if (p->body_len + n > p->body_cap) {
            size_t cap = p->body_cap ? p->body_cap : 65536;
            while (cap < p->body_len + n) cap *= 2;
            uint8_t *nb = realloc(p->body, cap);
            if (!nb) return;
            p->body = nb;
            p->body_cap = cap;
        }
        memcpy(p->body + p->body_len, d, n);
        p->body_len += n;
        break;
    default: break;
    }
    (void)c;
}

/* The headers are known and the first bytes (if any) are in hand: decide what kind of thing this is. */
static void main_decide(BrCore *c, BrPage *p, const uint8_t *d, size_t n) {
    const BrResult *r = &p->res;
    char ctype[160];
    snprintf(ctype, sizeof ctype, "%s", r->content_type);
    if (!ctype[0] && n) snprintf(ctype, sizeof ctype, "%s", brl_guess_type("", d, n));
    copy_charset(ctype, p->charset, sizeof p->charset);
    if (r->url[0]) snprintf(p->url, sizeof p->url, "%s", r->url);
    if (p->fragment[0]) {                                                /* the fragment of the address belongs to the page that was reached */
        size_t l = strlen(p->url);
        if (!strchr(p->url, '#') && l + strlen(p->fragment) + 2 < sizeof p->url) { p->url[l] = '#'; snprintf(p->url + l + 1, sizeof p->url - l - 1, "%s", p->fragment); }
    }
    if (is_html_type(ctype)) {
        commit_next(c, p);
        p->kind = BR_PK_HTML;
        start_document(c, p, p->url);
        p->state = BR_PG_LOADING;
    } else if (is_text_type(ctype)) {
        commit_next(c, p);
        p->kind = BR_PK_TEXT;
        if (!p->charset[0]) snprintf(p->charset, sizeof p->charset, "utf-8");
        start_document(c, p, p->url);
        if (p->doc) web_doc_parse_bytes(p->doc, text_prefix(), strlen(text_prefix()), "utf-8");
        p->state = BR_PG_LOADING;
    } else if (!strncasecmp(ctype, "image/", 6)) {
        p->kind = BR_PK_IMAGE;                                           /* shown when the whole picture is here */
        p->state = BR_PG_LOADING;
    } else {
        char size[48] = "", extra[200];
        if (r->bytes) human_size(r->bytes, size, sizeof size);
        snprintf(extra, sizeof extra, "Content type: %.100s%s%s", ctype[0] ? ctype : "unknown", size[0] ? ", at least " : "", size);
        brl_cancel_gen(c->loader, p->gen);                               /* do not download what cannot be shown */
        p->main_id = 0;
        p->main_done = true;
        show_info(c, p, "unsupported", 0, p->url, extra);
        return;
    }
    if (n) feed(c, p, d, n);
    after_parse(c, p);
}

static void make_image_page(BrCore *c, BrPage *p) {
    char name[200];
    const char *slash = strrchr(p->url, '/');
    snprintf(name, sizeof name, "%s", slash && slash[1] ? slash + 1 : p->url);
    char *q = strpbrk(name, "?#");
    if (q) *q = 0;
    char esc_url[BR_MAX_URL * 2];
    size_t o = 0;
    for (const char *s = p->url; *s && o + 8 < sizeof esc_url; s++) {
        if (*s == '"') { memcpy(esc_url + o, "%22", 3); o += 3; }
        else if (*s == '&') { memcpy(esc_url + o, "&amp;", 5); o += 5; }
        else if (*s == '<' || *s == '>') { memcpy(esc_url + o, *s == '<' ? "%3C" : "%3E", 3); o += 3; }
        else esc_url[o++] = *s;
    }
    esc_url[o] = 0;
    char html[BR_MAX_URL * 2 + 600];
    int hl = snprintf(html, sizeof html,
                      "<!doctype html><html><head><meta charset=\"utf-8\"><title></title><style>body{margin:0;background:#14182c;text-align:center}"
                      "img{margin:12px auto;max-width:98%%}</style></head><body><img src=\"%s\"></body></html>", esc_url);
    p->prefetch = p->body;                                               /* the engine asks for this URL: it is answered from the bytes we already have */
    p->prefetch_len = p->body_len;
    snprintf(p->prefetch_url, sizeof p->prefetch_url, "%s", p->url);
    p->body = NULL;
    p->body_len = p->body_cap = 0;
    commit_next(c, p);
    if (start_document(c, p, p->url)) web_doc_parse(p->doc, html, (size_t)hl, "utf-8");
    p->state = BR_PG_DONE;
    after_parse(c, p);
}

static void deliver_prefetch(BrCore *c) {
    BrPage *p = c->page;
    if (!p || !p->doc || !p->prefetch || p->prefetch_len == 0) return;
    uint8_t *data = p->prefetch;
    size_t len = p->prefetch_len;
    p->prefetch = NULL;                                                  /* once: the engine keeps its own copy */
    p->prefetch_len = 0;
    web_doc_resource_done(p->doc, 0x80000001u, 0, data, len, "image/*");
    free(data);
    p->layout_dirty = true;
    core_invalidate(c);
}

static void main_event(BrCore *c, BrPage *p, const BrEvent *ev) {
    if (ev->type == BR_EV_HEADERS) {
        brl_result(c->loader, p->main_id, &p->res);
        if (p->res.error) return;                                         /* a failure before any answer: the DONE event shows it */
        size_t n = 0;
        uint8_t *d = brl_take_data(c->loader, p->main_id, &n);
        main_decide(c, p, d, n);
        free(d);
        return;
    }
    if (ev->type == BR_EV_DATA) {
        size_t n = 0;
        uint8_t *d = brl_take_data(c->loader, p->main_id, &n);
        if (d) { feed(c, p, d, n); free(d); }
        brl_result(c->loader, p->main_id, &p->res);
        if (p->kind == BR_PK_HTML || p->kind == BR_PK_TEXT) { p->layout_dirty = true; core_invalidate(c); }
        return;
    }
    /* BR_EV_DONE */
    size_t n = 0;
    uint8_t *d = brl_take_data(c->loader, p->main_id, &n);
    brl_result(c->loader, p->main_id, &p->res);
    p->main_done = true;
    BrResult *r = &p->res;
    if (r->url[0] && p->kind == BR_PK_NONE) snprintf(p->url, sizeof p->url, "%s", r->url);
    if (p->kind == BR_PK_NONE && !r->error && r->status < 400) main_decide(c, p, d, n);       /* an empty body, or one whose headers came together with its end */
    else if (d && p->kind != BR_PK_NONE) feed(c, p, d, n);
    free(d);
    uint32_t id = p->main_id;
    p->main_id = 0;
    p->truncated = r->truncated;
    if (r->error == BR_E_CANCELLED) { brl_release(c->loader, id); return; }
    if (r->error && !(p->doc && p->bytes > 0 && (r->error == BR_E_TRUNCATED || r->error == BR_E_IO || r->error == BR_E_TIMEOUT))) {
        show_info(c, p, error_kind(r->error), 0, r->error_text, r->tls[0] ? r->tls : NULL);
        brl_release(c->loader, id);
        return;
    }
    if (r->error) { char note[300]; snprintf(note, sizeof note, "The page was cut short: %s", r->error_text); brc_note(c, note); }
    if (r->status >= 400 && p->bytes == 0 && (p->kind == BR_PK_HTML || p->kind == BR_PK_TEXT)) p->kind = BR_PK_NONE;      /* an error status and an empty body */
    if (p->kind == BR_PK_NONE) {                                         /* no content at all */
        if (r->status >= 400) { snprintf(p->url, sizeof p->url, "%s", r->url[0] ? r->url : p->req_url); show_info(c, p, "http", r->status, r->error_text, NULL); }
        else { commit_next(c, p); show_blank(c, p); }
        brl_release(c->loader, id);
        return;
    }
    if (p->kind == BR_PK_IMAGE) {
        if (p->body_len == 0) show_info(c, p, "parse", 0, "the picture is empty", NULL);
        else make_image_page(c, p);
    } else if (p->doc && (p->kind == BR_PK_HTML || p->kind == BR_PK_TEXT)) {
        web_doc_parse_finish(p->doc);
    }
    if (p->kind != BR_PK_GENERATED) p->state = BR_PG_DONE;
    if (r->truncated) {
        char note[200];
        snprintf(note, sizeof note, "The page is longer than the %u MiB the browser reads: the rest is not shown", (unsigned)(BR_MAX_DOC_BYTES >> 20));
        brc_note(c, note);
    }
    brl_release(c->loader, id);
    after_parse(c, p);
}

static void sub_done_event(BrCore *c, BrPage *p, const BrEvent *ev) {
    BrResult r;
    size_t n = 0;
    brl_result(c->loader, ev->id, &r);
    uint8_t *d = brl_take_data(c->loader, ev->id, &n);
    int status = (r.error || r.status >= 400 || r.truncated) ? -1 : 0;
    if (p->doc) web_doc_resource_done(p->doc, ev->id, status, d, n, r.content_type);
    free(d);
    brl_release(c->loader, ev->id);
    p->sub_done++;
    p->layout_dirty = true;
    core_invalidate(c);
}

void brc_pump(BrCore *c, uint64_t now_ms) {
    c->now_ms = now_ms;
    brl_drain(c->loader);
    BrEvent ev;
    int guard = 0;
    while (guard++ < 10000 && brl_next_event(c->loader, &ev)) {
        BrPage *p = c->next && ev.gen == c->next->gen ? c->next : c->page && ev.gen == c->page->gen ? c->page : NULL;
        if (!p) { if (ev.type == BR_EV_DONE) brl_release(c->loader, ev.id); continue; }
        if (ev.kind == BR_F_MAIN) { if (ev.id == p->main_id) main_event(c, p, &ev); else if (ev.type == BR_EV_DONE) brl_release(c->loader, ev.id); }
        else if (ev.type == BR_EV_DONE) sub_done_event(c, p, &ev);
    }
    if (c->page && c->page->prefetch) deliver_prefetch(c);
    if (c->message[0] && now_ms > c->message_until) { c->message[0] = 0; core_invalidate(c); }
    if (c->page && page_busy(c->page)) core_invalidate(c);                /* progressive display: the throttle in brc_render decides how often */
}

/* ---------------------------------------------------------------- the view */

void brc_set_viewport(BrCore *c, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == c->vw && h == c->vh) return;
    c->vw = w;
    c->vh = h;
    if (c->page) c->page->layout_dirty = true;
    core_invalidate(c);
}

void page_scroll_clamp(BrCore *c, BrPage *p) {
    if (!p->doc) { p->scroll_x = p->scroll_y = 0; return; }
    web_doc_set_scroll(p->doc, p->scroll_x, p->scroll_y);
    web_doc_scroll(p->doc, &p->scroll_x, &p->scroll_y);
    (void)c;
}

void page_apply_scroll(BrCore *c, BrPage *p) {
    if (!p->doc) return;
    if (p->want_fragment) {
        int x, y;
        web_doc_set_target(p->doc, p->fragment);
        if (web_doc_fragment_position(p->doc, p->fragment, &x, &y)) { p->scroll_x = 0; p->scroll_y = y; p->want_fragment = false; p->restore_y = -1; }
        else if (p->state == BR_PG_DONE) p->want_fragment = false;
    } else if (p->restore_y >= 0 && p->state == BR_PG_DONE) {
        p->scroll_y = p->restore_y;
        p->restore_y = -1;
    }
    page_scroll_clamp(c, p);
}

bool brc_render(BrCore *c, BrSurface *dst, uint64_t now_ms) {
    c->now_ms = now_ms;
    BrPage *p = c->page;
    if (!dst || !dst->px) return false;
    if (c->page && c->page->prefetch) deliver_prefetch(c);
    if (!p || !p->doc) {
        for (int y = 0; y < dst->h; y++) for (int x = 0; x < dst->w; x++) dst->px[(size_t)y * (size_t)dst->stride + (size_t)x] = 0xFFFFFF;
        c->dirty = false;
        return true;
    }
    if (page_busy(p) && p->last_paint_ms && now_ms >= p->last_paint_ms && now_ms - p->last_paint_ms < 150) return false;      /* a page still arriving is laid out a few times a second, not on every byte */
    web_doc_layout(p->doc, c->vw, c->vh);
    if (!p->mem_noted && web_doc_status(p->doc) != WEB_OK) {                /* the document hit its memory limit: the engine stopped growing it */
        char note[220];
        p->mem_noted = true;
        snprintf(note, sizeof note, "This page needs more memory than the browser gives one document (%u MiB): the rest of it is not shown", (unsigned)(BR_MEMORY_LIMIT >> 20));
        brc_note(c, note);
    }
    page_apply_scroll(c, p);
    const WebDisplayList *dl = web_doc_paint(p->doc, p->scroll_x, p->scroll_y, c->vw, c->vh);
    BrPaintOpts o;
    memset(&o, 0, sizeof o);
    o.scale = c->scale;
    o.fill_background = true;
    o.focus = p->focus;
    o.caret = p->caret;
    o.anchor = p->anchor;
    o.caret_on = ((now_ms - p->caret_t0) / 500) % 2 == 0;
    o.focus_scroll = p->field_scroll;
    brp_render(c->painter, dst, dl, &o);
    if (p->dd_open) dropdown_draw(c, p, dst);
    c->dirty = false;
    p->layout_dirty = false;
    p->last_paint_ms = now_ms;
    core_set_title(c);
    return true;
}

bool brc_take_dirty(BrCore *c) {
    bool d = c->dirty;
    c->dirty = false;
    return d;
}

void brc_content_size(BrCore *c, int *w, int *h) {
    *w = *h = 0;
    if (!c->page || !c->page->doc) return;
    web_doc_layout(c->page->doc, c->vw, c->vh);
    web_doc_content_size(c->page->doc, w, h);
}

int brc_scroll_y(const BrCore *c) { return c->page ? c->page->scroll_y : 0; }

void brc_scroll_to(BrCore *c, int y) {
    BrPage *p = c->page;
    if (!p || !p->doc) return;
    p->scroll_y = y;
    page_scroll_clamp(c, p);
    p->restore_y = -1;
    p->want_fragment = false;
    core_invalidate(c);
}

void brc_scroll_by(BrCore *c, int dy) {
    if (c->page) brc_scroll_to(c, c->page->scroll_y + dy);
}

const char *brc_status(BrCore *c) {
    BrPage *p = c->page;
    BrPage *n = c->next;
    if (p && p->hover_url[0]) { set_status(c, p->hover_url); return c->status; }
    if (c->message[0]) { set_status(c, c->message); return c->status; }
    if (n) {
        char sz[48] = "";
        if (n->bytes) { human_size(n->bytes, sz, sizeof sz); }
        char disp[160];
        br_url_display(n->req_url, disp, sizeof disp, 80);
        snprintf(c->status, sizeof c->status, "Loading %s...%s%s", disp, sz[0] ? " " : "", sz);
        return c->status;
    }
    if (p && page_busy(p)) {
        char disp[160], sz[48] = "";
        br_url_display(p->url, disp, sizeof disp, 80);
        if (p->bytes) human_size(p->bytes, sz, sizeof sz);
        if (p->nsub > p->sub_done) snprintf(c->status, sizeof c->status, "Loading %s... %s%s%d of %d resources", disp, sz, sz[0] ? ", " : "", p->sub_done, p->nsub);
        else snprintf(c->status, sizeof c->status, "Loading %s...%s%s", disp, sz[0] ? " " : "", sz);
        return c->status;
    }
    if (p && p->state == BR_PG_DONE && p->res.status >= 400) {
        snprintf(c->status, sizeof c->status, "%d %s", p->res.status, p->res.status == 404 ? "not found" : "the server reported an error");
        return c->status;
    }
    c->status[0] = 0;
    if (p && p->state != BR_PG_EMPTY) snprintf(c->status, sizeof c->status, "Done");
    return c->status;
}

/* ---------------------------------------------------------------- small accessors */

void brc_blur_field(BrCore *c) {
    if (!c->page) return;
    if (c->page->focus) field_blur(c, c->page);
    if (c->page->dd_open) dropdown_close(c, c->page);
}

int brc_find_count(const BrCore *c) { return c->page ? c->page->find_count : 0; }

BrSecurity brc_security(BrCore *c, char *text, size_t cap) {
    BrPage *p = c->page;
    text[0] = 0;
    if (!p) return BR_SEC_NONE;
    BrUrlKind k = br_url_kind(p->url);
    if (k == BR_URL_HTTPS && p->res.tls[0] && strstr(p->res.tls, "certificate verified")) {
        snprintf(text, cap, "Secure connection: %.150s", p->res.tls);
        return BR_SEC_SECURE;
    }
    if (k == BR_URL_HTTP && p->kind != BR_PK_GENERATED) {
        snprintf(text, cap, "Not secure: this page came over plain http");
        return BR_SEC_PLAIN;
    }
    return BR_SEC_NONE;
}
