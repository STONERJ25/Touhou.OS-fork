/*
 * Subresources: the requests a document makes through the embedder's WebLoader (stylesheets, images, scripts) and what
 * happens when they arrive (web_doc_resource_done). Scripts: parser-blocking ones resume the parser, async ones run when
 * they arrive, deferred ones run in document order after the parse; then DOMContentLoaded and, once nothing is pending,
 * load are reported to the WebScriptHost.
 */
#include "webint.h"

typedef struct WebReq {
    uint32_t id;
    WebResourceKind kind;
    WebNode *owner;             /* the element that asked (script, link, img), or NULL (@import, background) */
    char *url;
    int mode;                   /* scripts: 0 blocking, 1 async, 2 defer */
    bool done, ok;
    char *data;                 /* scripts kept until they can run */
    size_t len;
    void *user;                 /* the subsystem's own record (stylesheet, image) */
} WebReq;

typedef struct ResTable {
    VEC(WebReq) reqs;
    uint32_t next_id;
    bool load_sent, dcl_sent;
} ResTable;

static ResTable *table(WebDoc *d) {
    if (!d->res) d->res = wa_alloc(&d->arena, sizeof(ResTable));
    return d->res;
}

static WebReq *find(WebDoc *d, uint32_t id) {
    ResTable *t = d->res;
    if (!t) return NULL;
    for (uint32_t i = 0; i < t->reqs.n; i++)
        if (t->reqs.v[i].id == id) return &t->reqs.v[i];
    return NULL;
}

/* Asks the loader; returns the request id or 0. The id is recorded before fetch is called, so a loader that answers from
 * inside fetch (a cache, a test) finds it. */
uint32_t res_request(WebDoc *d, WebResourceKind kind, const char *url, WebNode *owner, int mode, void *user) {
    const WebLoader *l = d->cfg.loader;
    ResTable *t = table(d);
    if (!l || !l->fetch || !t || !url) return 0;
    WebReq r;
    memset(&r, 0, sizeof r);
    r.kind = kind;
    r.owner = owner;
    r.url = wa_strdup(&d->arena, url);
    r.mode = mode;
    r.user = user;
    uint32_t slot = t->reqs.n;
    if (!r.url || !vec_push(&d->arena, t->reqs, r)) return 0;
    uint32_t id = l->fetch(l->user, d, url, kind);
    if (!id) { t->reqs.n = slot; return 0; }
    if (slot < t->reqs.n) t->reqs.v[slot].id = id;
    return id;
}

uint32_t res_fetch_script(WebDoc *d, WebNode *script, const char *url, int mode) {
    return res_request(d, WEB_RES_SCRIPT, url, script, mode, NULL);
}

void parser_script_loaded(WebDoc *d, WebNode *s, uint32_t id, const char *data, size_t len);   /* parser.c */
void style_resource_done(WebDoc *d, void *user, int status, const void *data, size_t len);     /* style.c */
void image_resource_done(WebDoc *d, void *user, WebNode *owner, int status, const void *data, size_t len, const char *type); /* image.c */

static void run_deferred(WebDoc *d);

static bool pending(WebDoc *d) {
    ResTable *t = d->res;
    if (!t) return false;
    for (uint32_t i = 0; i < t->reqs.n; i++)
        if (!t->reqs.v[i].done) return true;
    return false;
}

static void maybe_load(WebDoc *d) {
    ResTable *t = table(d);
    const WebScriptHost *h = d->cfg.scripts;
    if (!t || !d->parse_done || t->load_sent || pending(d)) return;
    t->load_sent = true;
    if (h && h->lifecycle) h->lifecycle(h->user, d, WEB_EVENT_LOAD);
}

int web_doc_resource_done(WebDoc *d, uint32_t id, int status, const void *data, size_t len, const char *type) {
    WebReq *r = find(d, id);
    if (!r || r->done) return WEB_E_ARG;
    r->done = true;
    r->ok = status >= 0 && data;
    switch (r->kind) {
    case WEB_RES_SCRIPT:
        if (r->mode == 0) {
            parser_script_loaded(d, r->owner, id, r->ok ? data : NULL, r->ok ? len : 0);
        } else if (r->mode == 1) {
            if (r->ok) parser_script_loaded(d, r->owner, id, data, len);
        } else if (r->ok) {
            r->data = wa_alloc(&d->arena, len + 1);
            if (r->data) { memcpy(r->data, data, len); r->len = len; }
            run_deferred(d);
        }
        break;
    case WEB_RES_STYLESHEET:
        style_resource_done(d, r->user, r->ok ? status : -1, data, len);
        break;
    case WEB_RES_IMAGE:
        image_resource_done(d, r->user, r->owner, r->ok ? status : -1, data, len, type);
        break;
    default:
        break;
    }
    maybe_load(d);
    return WEB_OK;
}

/* Deferred scripts run in document order once the parse has finished and each has arrived. */
static void run_deferred(WebDoc *d) {
    ResTable *t = d->res;
    if (!t || !d->parse_done) return;
    for (uint32_t i = 0; i < t->reqs.n; i++) {
        WebReq *r = &t->reqs.v[i];
        if (r->kind != WEB_RES_SCRIPT || r->mode != 2) continue;
        if (!r->done) return;                                 /* wait for this one before running later ones */
        if (r->data) {
            char *data = r->data;
            size_t len = r->len;
            r->data = NULL;
            parser_script_loaded(d, r->owner, r->id, data, len);
        }
    }
    if (!t->dcl_sent) {
        t->dcl_sent = true;
        const WebScriptHost *h = d->cfg.scripts;
        if (h && h->lifecycle) h->lifecycle(h->user, d, WEB_EVENT_DOMCONTENTLOADED);
    }
}

void res_parse_finished(WebDoc *d) {
    ResTable *t = table(d);
    if (!t) return;
    run_deferred(d);
    if (!t->dcl_sent) {
        bool waiting = false;
        for (uint32_t i = 0; i < t->reqs.n; i++)
            if (t->reqs.v[i].kind == WEB_RES_SCRIPT && t->reqs.v[i].mode == 2 && !t->reqs.v[i].done) waiting = true;
        if (!waiting) {
            t->dcl_sent = true;
            const WebScriptHost *h = d->cfg.scripts;
            if (h && h->lifecycle) h->lifecycle(h->user, d, WEB_EVENT_DOMCONTENTLOADED);
        }
    }
    maybe_load(d);
}

void res_free_all(WebDoc *d) {
    ResTable *t = d->res;
    const WebLoader *l = d->cfg.loader;
    if (!t) return;
    for (uint32_t i = 0; i < t->reqs.n; i++)
        if (!t->reqs.v[i].done && l && l->cancel) l->cancel(l->user, d, t->reqs.v[i].id);
}
