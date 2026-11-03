/*
 * The browser core: everything between the window and the engine that does not need the window toolkit.
 *
 *   - navigation: addresses, history (back, forward, reload, stop), fragments, the page's lifecycle from the first byte to the last
 *   - loading: the main resource and the engine's subresource requests through the loader (loader.h), progressive parsing, the
 *     treatment of each kind of content (pages, plain text, pictures, things the browser cannot show), error pages (errpage.c)
 *   - rendering: layout for the viewport, the display list through the painter (paint.h), scrolling
 *   - interaction: pointer, wheel and keys in terms of plain numbers (input.c), text selection, copying, find in page, links, forms
 *     with text editing, check boxes, radio buttons and drop-downs (forms.c)
 *
 * The window code (main.c) turns toolkit events into calls here and shows what comes back. Because nothing in here includes the
 * toolkit, tests/host/browsertool.c can run a whole session (open a page, click, type, take a picture) on the host.
 *
 * This is also where scripts would run: BrCore.script_hook in page_start_document() is the single place that gives the engine
 * a WebScriptHost; phase 1 leaves it empty (WebConfig.scripts = NULL), so <noscript> content is shown and nothing runs.
 */
#ifndef BROWSER_CORE_H
#define BROWSER_CORE_H

#include <web.h>

#include "browser.h"
#include "fonts.h"
#include "loader.h"
#include "paint.h"

#define BR_HIST_MAX 100

typedef struct BrHost {
    void *user;
    void (*invalidate)(void *user);                              /* something changed that needs a frame */
    void (*clipboard)(void *user, const char *text, size_t len); /* copied text for the system clipboard */
} BrHost;

typedef enum { BR_NAV_LINK, BR_NAV_BACK, BR_NAV_FORWARD, BR_NAV_RELOAD, BR_NAV_REPLACE } BrNav;
typedef enum { BR_PG_EMPTY, BR_PG_LOADING, BR_PG_DONE, BR_PG_FAILED } BrPageState;
typedef enum { BR_PK_NONE, BR_PK_HTML, BR_PK_TEXT, BR_PK_IMAGE, BR_PK_GENERATED } BrPageKind;

typedef struct BrHist { char url[BR_MAX_URL]; int scroll_y; } BrHist;

typedef struct BrDropdown {
    WebNode *select;
    BrRect box;                      /* the control in viewport pixels (device) */
    int n, sel, hover;
    char **labels;
    WebFontHandle font;
} BrDropdown;

typedef struct BrPage {
    unsigned gen;                    /* the generation the loader labels its fetches with */
    BrPageState state;
    BrPageKind kind;
    char req_url[BR_MAX_URL];        /* what was asked for */
    char url[BR_MAX_URL];            /* the document's address (after redirects) */
    char title[256];
    WebDoc *doc;
    uint32_t main_id;                /* the loader's id of the main resource, 0 when finished or none */
    char charset[64];
    BrResult res;                    /* what is known about the main fetch */
    bool main_done;
    bool truncated;
    uint8_t *body;                   /* for pictures: the bytes, until the document that shows them exists */
    size_t body_len, body_cap;
    int nsub, sub_done;              /* subresource requests made and finished */
    uint64_t bytes;                  /* main resource bytes so far */
    /* the view */
    int scroll_x, scroll_y;          /* CSS px */
    char fragment[256];              /* scroll here when layout allows */
    bool want_fragment;
    int restore_y;                   /* scroll to this when loaded (history), -1 none */
    uint64_t last_paint_ms;
    /* pointer */
    WebNode *hover;
    int cursor;
    char hover_url[BR_MAX_URL];
    bool pressed;                    /* the left button went down in the page */
    int press_x, press_y;            /* viewport px */
    bool dragged;
    WebNode *press_node, *press_link, *press_control;
    WebNode *sel_anchor;
    int sel_anchor_off;
    /* forms */
    WebNode *focus;                  /* the text control being edited */
    size_t caret, anchor;
    int field_scroll;                /* device px */
    uint64_t caret_t0;
    BrDropdown dd;
    bool dd_open;
    /* find in page */
    char find_text[128];
    int find_count, find_index;
    /* the document that image pages show: the one resource we already have */
    uint8_t *prefetch;
    size_t prefetch_len;
    char prefetch_url[BR_MAX_URL];
    bool layout_dirty;
    bool mem_noted;                  /* the memory limit was reached and the status line said so */
} BrPage;

typedef struct BrCore {
    BrHost host;
    BrFonts *fonts;
    BrPainter *painter;
    BrLoader *loader;
    WebLoader wloader;
    int scale;
    int vw, vh;                      /* the viewport in CSS px */
    unsigned gen;
    BrPage *page;                    /* what is on screen */
    BrPage *next;                    /* the page being fetched; it replaces `page` when it has something to show */
    BrHist hist[BR_HIST_MAX];
    int nhist, hpos;
    char message[256];               /* a transient note for the status line */
    uint64_t message_until;
    bool dirty;                      /* the page's pixels are out of date */
    unsigned loads;                  /* pages that finished loading (successfully or as an error page), counted */
    uint64_t now_ms;
    char status[BR_MAX_URL + 64];
    char home[BR_MAX_URL];
    char cwd[BR_MAX_URL];
} BrCore;

/* ---- life ---- */
BrCore *brc_new(const BrHost *host, int scale);               /* scale: device pixels per CSS pixel (the window's ui scale) */
void brc_free(BrCore *c);
int brc_wake_fd(const BrCore *c);                             /* poll this; when readable call brc_pump */
void brc_pump(BrCore *c, uint64_t now_ms);                    /* handle what the loader reports; also call it now and then (timers) */
bool brc_busy(const BrCore *c);                               /* a page or resource is still loading */
unsigned brc_loads(const BrCore *c);                          /* how many pages have finished so far (the window code traces it) */

/* ---- navigation: `text` is what a person typed (an address, a path) ---- */
void brc_go(BrCore *c, const char *text);
void brc_navigate(BrCore *c, const char *url, BrNav how);     /* an absolute URL */
void brc_back(BrCore *c);
void brc_forward(BrCore *c);
void brc_reload(BrCore *c);
void brc_stop(BrCore *c);
bool brc_can_back(const BrCore *c);
bool brc_can_forward(const BrCore *c);
const char *brc_url(const BrCore *c);                         /* the address to show in the address bar */
const char *brc_title(const BrCore *c);                       /* the page title, or the address */
const char *brc_status(BrCore *c);                            /* the status line: the link under the pointer, progress, a note */
void brc_note(BrCore *c, const char *text);                   /* show a message in the status line for a few seconds */

/* ---- the view ---- */
void brc_set_viewport(BrCore *c, int w, int h);               /* CSS px */
bool brc_render(BrCore *c, BrSurface *dst, uint64_t now_ms);  /* paints the page into dst (0,0 is the viewport's corner); false when nothing was painted */
bool brc_take_dirty(BrCore *c);                               /* true once after anything changed on the page */
void brc_content_size(BrCore *c, int *w, int *h);             /* CSS px */
int brc_scroll_y(const BrCore *c);
void brc_scroll_to(BrCore *c, int y);
void brc_scroll_by(BrCore *c, int dy);

/* ---- input (viewport coordinates in CSS px) ---- */
int brc_pointer_move(BrCore *c, int x, int y, unsigned buttons);   /* returns a WebCursor */
void brc_pointer_down(BrCore *c, int x, int y, int button, int clicks);
void brc_pointer_up(BrCore *c, int x, int y, int button);
void brc_wheel(BrCore *c, int x, int y, int ticks);           /* ticks > 0: up */
bool brc_key(BrCore *c, uint8_t code, uint8_t mods, uint32_t ch);   /* true when the page took the key */
void brc_focus_out(BrCore *c);                                /* the window lost the keyboard focus */

/* ---- what the lock icon says about the current page ---- */
typedef enum { BR_SEC_NONE, BR_SEC_PLAIN, BR_SEC_SECURE } BrSecurity;
BrSecurity brc_security(BrCore *c, char *text, size_t cap);    /* SECURE: https with a verified certificate; PLAIN: http; text is a sentence for a tooltip */

/* ---- selection, copying, find ---- */
void brc_select_all(BrCore *c);
bool brc_copy(BrCore *c);                                     /* copies the selection (or the selected text of a field) to the clipboard */
void brc_paste(BrCore *c, const char *text, size_t len);      /* text for the focused field */
bool brc_has_field_focus(const BrCore *c);
void brc_blur_field(BrCore *c);                               /* the keyboard went somewhere else (the address field): leave the page's field */
int brc_find(BrCore *c, const char *text, int direction);     /* 0 first/again, 1 next, -1 previous; returns the number of matches */
int brc_find_index(const BrCore *c);                          /* the current match, 1-based, 0 when none */
int brc_find_count(const BrCore *c);
void brc_find_done(BrCore *c);

/* ---- helpers shared by core.c, input.c and forms.c ---- */
void core_invalidate(BrCore *c);
void core_set_title(BrCore *c);
void page_scroll_clamp(BrCore *c, BrPage *p);
void page_apply_scroll(BrCore *c, BrPage *p);
bool page_control_geom(BrCore *c, BrPage *p, WebNode *node, WebDisplayItem *item, BrControlGeom *g);    /* geometry in viewport device pixels */
void page_navigate_action(BrCore *c, BrPage *p, WebAction *a);          /* does what web_doc_activate asked for; frees the action's strings */
bool field_is_text(WebNode *n);                                          /* a text input or textarea */
void field_focus(BrCore *c, BrPage *p, WebNode *node, bool at_click, int x_dev, int y_dev);
void field_blur(BrCore *c, BrPage *p);
bool field_key(BrCore *c, BrPage *p, uint8_t code, uint8_t mods, uint32_t ch);
void field_insert(BrCore *c, BrPage *p, const char *utf8, size_t len);
bool field_selection_text(BrPage *p, char **text, size_t *len);
void dropdown_open(BrCore *c, BrPage *p, WebNode *select);
void dropdown_close(BrCore *c, BrPage *p);
bool dropdown_pointer(BrCore *c, BrPage *p, int x, int y, int what);   /* what: 0 move, 1 down; true when the pointer is the dropdown's */
bool dropdown_key(BrCore *c, BrPage *p, uint8_t code);
void dropdown_draw(BrCore *c, BrPage *p, BrSurface *dst);
bool focus_next(BrCore *c, BrPage *p, int dir);
void scroll_into_view(BrCore *c, BrPage *p, WebNode *el);

/* generated pages (errpage.c): returns a malloc'd UTF-8 document */
typedef struct BrPageInfo {
    const char *kind;                /* "dns", "connect", "tls", "http", "file", ... see errpage.c */
    int status;                      /* HTTP status for kind "http" */
    const char *url;
    const char *detail;              /* the plain technical sentence */
    const char *extra;               /* a second line (the content type, a size...) or NULL */
} BrPageInfo;
char *br_error_page(const BrPageInfo *info, size_t *len);
char *br_blank_page(size_t *len);
char *br_about_page(size_t *len, int font_files, const char *font_dir);
char *br_home_page(size_t *len, const char *home);

#endif
