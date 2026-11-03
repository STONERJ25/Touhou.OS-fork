/*
 * Images of a document: <img>, <input type=image>, background-image, list-style-image and content: url() share one record
 * per URL. The bytes are fetched once through the WebLoader; when they arrive the WebImageDecoder probes them for the
 * intrinsic size (layout needs it, so the document's layout becomes dirty) and keeps a copy; the pixels are decoded the
 * first time a display list needs them. Without a loader or a decoder an image never becomes ready: layout then uses the
 * width and height the page gives and the alt text.
 */
#include "layout.h"

struct WebRes {
    char *url;
    uint32_t req;
    int state;                     /* 0 waiting, 1 ready, -1 failed */
    int w, h;
    void *data;
    size_t len;
    WebImageHandle handle;
    bool decoded;
    struct WebRes *next;
};

typedef struct ImgTable { WebRes *head; uint32_t count; } ImgTable;

uint32_t res_request(WebDoc *d, WebResourceKind kind, const char *url, WebNode *owner, int mode, void *user);   /* resource.c */

static WebRes *find(WebDoc *d, const char *url, bool create, WebNode *owner) {
    ImgTable *t = d->images;
    if (!t) {
        if (!create) return NULL;
        t = d->images = wa_alloc(&d->arena, sizeof *t);
        if (!t) return NULL;
    }
    for (WebRes *r = t->head; r; r = r->next) if (!strcmp(r->url, url)) return r;
    if (!create || t->count >= 4096) return NULL;
    WebRes *r = wa_alloc(&d->arena, sizeof *r);
    if (!r) return NULL;
    r->url = wa_strdup(&d->arena, url);
    if (!r->url) return NULL;
    r->next = t->head;
    t->head = r;
    t->count++;
    if (!d->cfg.images) r->state = -1;                       /* nothing could decode it */
    else {
        r->req = res_request(d, WEB_RES_IMAGE, url, owner, 0, r);
        if (!r->req && r->state == 0) r->state = -1;
    }
    return r;
}

void image_resource_done(WebDoc *d, void *user, WebNode *owner, int status, const void *data, size_t len, const char *type) {
    WebRes *r = user;
    (void)owner;
    (void)type;
    if (!r || r->state != 0) return;
    const WebImageDecoder *dec = d->cfg.images;
    int w = 0, h = 0;
    if (status < 0 || !data || !dec || !dec->probe || dec->probe(dec->user, data, len, &w, &h) != 0 || w <= 0 || h <= 0) {
        r->state = -1;
    } else {
        r->data = wa_alloc(&d->arena, len ? len : 1);
        if (!r->data) r->state = -1;
        else {
            memcpy(r->data, data, len);
            r->len = len;
            r->w = w;
            r->h = h;
            r->state = 1;
        }
    }
    d->layout_dirty = true;
}

bool image_size(WebDoc *d, WebNode *el, const char *url, int *w, int *h, bool *failed) {
    *failed = false;
    if (!url || !*url) { *failed = true; return false; }
    WebRes *r = find(d, url, true, el);
    if (!r) { *failed = true; return false; }
    if (el && is_el(el)) el->u.el.res = r;
    if (r->state < 0) { *failed = true; return false; }
    if (r->state == 0) return false;
    *w = r->w;
    *h = r->h;
    return true;
}

void image_request(WebDoc *d, WebNode *el, const char *url) {
    if (url && *url) find(d, url, true, el);
}

WebImageHandle image_handle(WebDoc *d, const char *url) {
    WebRes *r = url ? find(d, url, false, NULL) : NULL;
    if (!r || r->state != 1) return 0;
    if (!r->decoded) {
        const WebImageDecoder *dec = d->cfg.images;
        r->decoded = true;
        if (dec && dec->decode) r->handle = dec->decode(dec->user, r->data, r->len);
    }
    return r->handle;
}

/* the intrinsic size of an image that is ready, for backgrounds and list images */
bool image_ready_size(WebDoc *d, const char *url, int *w, int *h) {
    WebRes *r = url ? find(d, url, true, NULL) : NULL;
    if (!r || r->state != 1) return false;
    *w = r->w;
    *h = r->h;
    return true;
}

void image_free_all(WebDoc *d) {
    ImgTable *t = d->images;
    const WebImageDecoder *dec = d->cfg.images;
    if (!t) return;
    for (WebRes *r = t->head; r; r = r->next)
        if (r->handle && dec && dec->release) { dec->release(dec->user, r->handle); r->handle = 0; }
}
