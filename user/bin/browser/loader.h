/*
 * The loader: fetches pages and their subresources on worker threads so that the window stays alive, and hands the results to
 * the one thread that owns the documents (the engine wants a document used from one thread at a time).
 *
 *   BrLoader *l = brl_new();
 *   kapp_watch_fd(app, brl_wake_fd(l), on_wake, ...);                // or poll() it
 *   uint32_t id = brl_start(l, "https://example.org/", BR_F_MAIN, gen, BR_MAX_DOC_BYTES);
 *   ... in on_wake:  brl_drain(l);  BrEvent ev;  while (brl_next_event(l, &ev)) { ...brl_result / brl_take_data / brl_release... }
 *
 * Schemes: http and https (libhttp over libtls: certificates are checked against $SSL_CERT_FILE or /etc/ssl/cacert.pem, a
 * failure is an error with the library's own sentence and there is no way around it), file (a file, or a generated index of a
 * folder) and data. At most BR_FETCH_THREADS downloads run at once; main documents go first, then stylesheets, then the rest.
 * Every download is bounded: by size (the `max_bytes` of the request), by BR_TIMEOUT_MS of silence and by BR_DEADLINE_MS in all.
 *
 * Events: a MAIN fetch reports HEADERS (status, final URL and content type are known; the first body bytes are there or the
 * body is empty), DATA (more bytes: take them with brl_take_data and feed them to the parser as they come) and DONE. Other
 * fetches only report DONE, after which brl_take_data returns the whole body. Everything is plain data; no callback runs on a
 * worker thread outside this file.
 */
#ifndef BROWSER_LOADER_H
#define BROWSER_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "browser.h"

typedef struct BrLoader BrLoader;

typedef enum { BR_F_MAIN = 0, BR_F_STYLE, BR_F_IMAGE, BR_F_SCRIPT, BR_F_OTHER } BrFetchKind;

/* why a fetch failed (BrResult.error); 0 is success */
enum {
    BR_E_NONE = 0, BR_E_URL, BR_E_DNS, BR_E_CONNECT, BR_E_TLS, BR_E_NOTRUST, BR_E_IO, BR_E_TIMEOUT, BR_E_PARSE, BR_E_TRUNCATED, BR_E_REDIRECTS,
    BR_E_TOO_BIG, BR_E_CANCELLED, BR_E_NOT_FOUND, BR_E_ACCESS, BR_E_NOMEM, BR_E_SCHEME,
};

typedef struct BrResult {
    int status;                      /* the HTTP status (200 for files and data: URLs), 0 when none came */
    int error;                       /* BR_E_* */
    int tls_error;                   /* TLS_E_* when error is BR_E_TLS */
    char error_text[512];            /* a sentence that says exactly what went wrong */
    char url[BR_MAX_URL];            /* the final URL, after redirects */
    char content_type[160];          /* the Content-Type header, or a guess for files */
    char tls[320];                   /* "TLS 1.3, TLS_AES_128_GCM_SHA256; certificate verified" for https, else "" */
    int redirects;
    bool downgraded;                 /* a redirect led from https to http */
    bool truncated;                  /* the body was cut at the size limit */
    uint64_t bytes;                  /* body bytes received so far */
} BrResult;

typedef enum { BR_EV_HEADERS = 1, BR_EV_DATA, BR_EV_DONE } BrEventType;
typedef struct BrEvent { BrEventType type; uint32_t id; unsigned gen; BrFetchKind kind; } BrEvent;

BrLoader *brl_new(void);                            /* NULL when the wake-up pipe or memory is not to be had */
void brl_free(BrLoader *l);
int brl_wake_fd(const BrLoader *l);                 /* becomes readable when there are events; call brl_drain, then brl_next_event until false */
void brl_drain(BrLoader *l);

/* Starts a fetch (a worker thread when one is free, else it waits). Returns its id (> 0), or 0 when it cannot be started. `gen`
 * labels the page the fetch belongs to, for brl_cancel_gen. */
uint32_t brl_start(BrLoader *l, const char *url, BrFetchKind kind, unsigned gen, size_t max_bytes);
void brl_cancel_gen(BrLoader *l, unsigned gen);     /* stops every fetch of that generation; they report nothing more */
bool brl_next_event(BrLoader *l, BrEvent *ev);
bool brl_result(BrLoader *l, uint32_t id, BrResult *out);       /* a copy of what is known so far */
uint8_t *brl_take_data(BrLoader *l, uint32_t id, size_t *len);  /* the bytes that arrived since the last call (malloc'd, free it); NULL when none */
void brl_release(BrLoader *l, uint32_t id);         /* done with a fetch (after DONE; before, it is cancelled) */
int brl_active(BrLoader *l);                        /* fetches not yet released, queued or running */

/* What the loader makes of a file's name and first bytes, for tests and the page logic. */
const char *brl_guess_type(const char *path, const uint8_t *head, size_t n);

#endif
