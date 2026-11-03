/*
 * web.h: Hatate, the TouhouOS web engine core (user/lib/web).
 *
 * Hatate Himekaidou is the tengu who reports on the world through what her camera shows her; this library is what the
 * Bunbunmaru Browser looks at pages with. Given bytes of HTML and CSS it builds a DOM (WHATWG HTML parsing algorithm),
 * computes styles (CSS cascade), lays out boxes for a viewport (block, inline, floats, positioning, tables, flexbox, grid)
 * and emits a display list in painting order. It has no operating-system dependency: everything outside, fonts, network,
 * image decoding and scripts, comes in through the small interfaces declared here, which other parts of TouhouOS implement.
 *
 *      WebConfig cfg = { .fonts = &my_fonts, .loader = &my_loader, .images = web_image_libimg() };
 *      WebDoc *doc = web_doc_new(&cfg, "https://example.org/");
 *      web_doc_parse(doc, bytes, len, NULL);              // charset from the transport layer, or NULL
 *      web_doc_layout(doc, 800, 600);                      // the viewport in CSS pixels
 *      const WebDisplayList *dl = web_doc_paint(doc, 0, scroll_y, 800, 600);
 *      for (int i = 0; i < dl->n; i++) draw(&dl->items[i]);
 *      WebHit hit;
 *      if (web_doc_hit_test(doc, mx, my + scroll_y, &hit) && hit.href) navigate(hit.href);
 *      web_doc_free(doc);
 *
 * Memory: a document owns all its memory through an allocator the library controls (arenas with size-class recycling,
 * docs/WEB.md "Memory model"); web_doc_free releases a page at once. A per-document limit (WebConfig.memory_limit) caps
 * the total, so hostile input cannot grow without bound: when it is reached, parsing stops where it is and layout gives
 * up gracefully (web_doc_status reports WEB_E_NOMEM).
 *
 * Units: CSS pixels. Layout works in WebUnit, 1/64 of a CSS pixel (like a 26.6 fixed-point number): font metrics and text
 * widths returned by a WebFontProvider are in these units too. The display list has whole device pixels for boxes (the
 * device pixel is the CSS pixel times WebConfig.scale) and sub-pixel positions for text.
 *
 * Threads: a document must be used from one thread at a time. Documents are independent of each other.
 */
#ifndef GAP_WEB_H
#define GAP_WEB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================================ basic types */

typedef int32_t WebUnit;                         /* 1/64 CSS px */
#define WEB_UNIT_SHIFT  6
#define WEB_UNIT_ONE    64
#define WEB_PX(n)       ((WebUnit)((n) * 64))
#define WEB_UNIT_TO_PX(u) ((u) >> 6)             /* floor */

typedef struct WebRect { WebUnit x, y, w, h; } WebRect;   /* layout units */
typedef struct WebIRect { int x, y, w, h; } WebIRect;     /* device pixels */

typedef uint32_t WebColor;                       /* 0xAARRGGBB, straight alpha: 0xFF000000 is opaque black, 0 is transparent */

typedef struct WebDoc WebDoc;
typedef struct WebNode WebNode;

/* return codes */
#define WEB_OK            0
#define WEB_E_NOMEM     (-1)     /* the memory limit was reached */
#define WEB_E_ARG       (-2)
#define WEB_E_STATE     (-3)     /* the call does not fit the document's state (e.g. writing after the parse finished) */
#define WEB_PAUSED        1      /* web_doc_parse*: the parser waits for a script (WebScriptHost.run_script returned 1) */

/* ================================================================================================ interfaces */

/* Memory hooks. All memory a document uses goes through these (NULL: the C library's malloc/free). */
typedef struct WebAllocator {
    void *(*alloc)(void *user, size_t n);
    void  (*free)(void *user, void *p);
    void *user;
} WebAllocator;

/*
 * WebFont: text measurement, implemented by the embedder: first with the 8x16 bitmap font of the toolkit, later with the
 * TrueType rasteriser (user/lib/ttf). The engine asks for a font matching a description once per distinct style and keeps the
 * handle; it measures words and the spaces between them; the display list names the same handle for every text run, so the
 * painter draws exactly what was measured.
 */
typedef struct WebFontDesc {
    const char *const *families;     /* the font-family list, unquoted names, most preferred first; generic names as
                                        "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui" */
    int nfamilies;
    WebUnit size;                    /* computed font-size */
    int weight;                      /* 100..900 (400 normal, 700 bold) */
    int style;                       /* 0 normal, 1 italic, 2 oblique */
    bool small_caps;
} WebFontDesc;

typedef struct WebFontMetrics {
    WebUnit ascent, descent;         /* above and below the baseline, both positive */
    WebUnit line_gap;                /* line-height: normal = ascent + descent + line_gap */
    WebUnit x_height;                /* ex units, vertical-align: middle */
    WebUnit zero_advance;            /* the advance of "0": ch units */
    WebUnit space_advance;           /* the advance of U+0020 */
    WebUnit underline_offset;        /* below the baseline, positive */
    WebUnit underline_thickness;
} WebFontMetrics;

typedef uintptr_t WebFontHandle;     /* chosen by the provider; 0 is never a valid handle */

typedef struct WebFontProvider {
    void *user;
    WebFontHandle (*match)(void *user, const WebFontDesc *desc);              /* never fails: fall back to a default font */
    void    (*metrics)(void *user, WebFontHandle font, WebFontMetrics *out);
    WebUnit (*measure)(void *user, WebFontHandle font, const char *utf8, size_t len);   /* advance width of the text */
} WebFontProvider;

/*
 * WebLoader: fetching subresources (stylesheets, images, scripts) for a document. The engine resolves URLs (against the
 * document URL and <base href>) and asks for absolute ones. fetch returns a request id (non-zero), or 0 when the embedder
 * refuses the request (blocked scheme, offline...). When the bytes are there, the embedder calls web_doc_resource_done with
 * that id, from any later point of its event loop or even from inside fetch. The engine copies what it needs.
 */
typedef enum {
    WEB_RES_STYLESHEET = 1,          /* <link rel=stylesheet>, @import */
    WEB_RES_IMAGE,                   /* <img>, <input type=image>, background-image, list-style-image */
    WEB_RES_SCRIPT,                  /* <script src> (only requested when a WebScriptHost is set) */
    WEB_RES_FONT,                    /* reserved: @font-face is not implemented */
} WebResourceKind;

typedef struct WebLoader {
    void *user;
    uint32_t (*fetch)(void *user, WebDoc *doc, const char *url, WebResourceKind kind);
    void     (*cancel)(void *user, WebDoc *doc, uint32_t id);                 /* may be NULL */
} WebLoader;

/* status: 0 = success, negative = failed (network error, HTTP error); a failed stylesheet is skipped, a failed image shows
 * its alt text. content_type may be NULL. Returns WEB_OK or WEB_E_ARG for an unknown id. */
int web_doc_resource_done(WebDoc *doc, uint32_t id, int status, const void *data, size_t len, const char *content_type);

/*
 * WebImage: image decoding on demand. The engine calls probe when an image arrives (to know its intrinsic size for layout)
 * and decode the first time the image is painted; the display list carries the decoded handle. web_image_libimg() returns
 * a decoder built on user/lib/img (PNG, JPEG, GIF, BMP, ...); its handles are `const Image *` (img.h).
 */
typedef uintptr_t WebImageHandle;    /* 0 = none */

typedef struct WebImageDecoder {
    void *user;
    int  (*probe)(void *user, const void *data, size_t len, int *w, int *h);  /* 0 and the size, or negative: not an image */
    WebImageHandle (*decode)(void *user, const void *data, size_t len);       /* 0 on failure */
    void (*release)(void *user, WebImageHandle img);
} WebImageDecoder;

const WebImageDecoder *web_image_libimg(void);

/*
 * WebScriptHost: the DOM bridge a JavaScript engine binds to (docs/WEB.md "DOM bridge"). Without one, scripts are not run,
 * <noscript> content is parsed as markup and shown (scripting disabled), and nothing else changes.
 */
typedef enum {
    WEB_MUT_CHILD_LIST = 1,          /* children added or removed: target is the parent */
    WEB_MUT_ATTRIBUTE,               /* attr_name changed on target (old_value: the previous value or NULL) */
    WEB_MUT_CHARACTER_DATA,          /* text/comment data changed */
} WebMutationKind;

typedef struct WebMutation {
    WebMutationKind kind;
    WebNode *target;
    WebNode *added, *removed;        /* child list: the node added or removed (one record per node) */
    WebNode *prev_sibling, *next_sibling;
    const char *attr_name;           /* attribute: the qualified name */
    const char *old_value;           /* attribute or character data: the previous value, valid during the callback */
} WebMutation;

typedef struct WebScriptHost {
    void *user;
    /* A parser-inserted <script> has been closed (inline: its text is the script; external: its resolved "src" was fetched
     * through the loader and is passed as source). Run it now, or return 1 to make the parser pause: then call
     * web_doc_parse_resume when the script has finished. Scripts may call web_doc_write (document.write). */
    int  (*run_script)(void *user, WebDoc *doc, WebNode *script, const char *source, size_t len);
    /* After every DOM change made through the API or by the parser after the initial parse (MutationObserver records). */
    void (*mutation)(void *user, WebDoc *doc, const WebMutation *m);
    /* Lifecycle: DOMContentLoaded (after the parse), load (after every subresource arrived). */
    void (*lifecycle)(void *user, WebDoc *doc, int event);    /* WEB_EVENT_DOMCONTENTLOADED, WEB_EVENT_LOAD */
    /* The document is about to be freed: drop every wrapper that points to its nodes. */
    void (*document_freed)(void *user, WebDoc *doc);
} WebScriptHost;

#define WEB_EVENT_DOMCONTENTLOADED 1
#define WEB_EVENT_LOAD             2

/* ================================================================================================ documents */

typedef struct WebConfig {
    const WebAllocator *alloc;               /* NULL: malloc/free */
    size_t memory_limit;                     /* bytes; 0 = 512 MiB */
    const WebFontProvider *fonts;            /* NULL: a built-in fixed-width font (8 x 16 px at 16 px), for tests and tools */
    const WebLoader *loader;                 /* NULL: no subresources are fetched */
    const WebImageDecoder *images;           /* NULL: images keep their declared sizes and show as placeholders */
    const WebScriptHost *scripts;            /* NULL: scripting disabled */
    int scale;                               /* device pixels per CSS pixel (1, 2, 3); 0 = 1 */
    bool prefers_dark;                       /* @media (prefers-color-scheme: dark) */
    bool print;                              /* @media print instead of screen */
    int max_depth;                           /* the parser keeps the tree at most this deep; 0 = 512 */
    const char *user_stylesheet;             /* optional CSS of the user origin (after the UA sheet, before the author's) */
} WebConfig;

WebDoc *web_doc_new(const WebConfig *cfg, const char *url);   /* NULL on allocation failure */
void    web_doc_free(WebDoc *doc);
int     web_doc_status(const WebDoc *doc);                     /* WEB_OK or WEB_E_NOMEM */
const char *web_doc_url(const WebDoc *doc);
const char *web_doc_base_url(const WebDoc *doc);               /* after <base href> */
const char *web_doc_title(WebDoc *doc);                        /* the <title> text, whitespace collapsed; "" when none */
const char *web_doc_charset(const WebDoc *doc);                /* the encoding used: "UTF-8", "windows-1252", "Shift_JIS"... */
int     web_doc_quirks(const WebDoc *doc);                     /* 0 no-quirks, 1 limited-quirks, 2 quirks */
WebNode *web_doc_root(WebDoc *doc);                            /* the Document node */
WebNode *web_doc_element(WebDoc *doc);                         /* the <html> element */
WebNode *web_doc_body(WebDoc *doc);
WebNode *web_doc_head(WebDoc *doc);

/* Parsing. web_doc_parse parses a whole document at once. For data that arrives in pieces, call web_doc_parse_bytes for each
 * piece and web_doc_parse_finish at the end (the tree grows as bytes arrive; layout may run in between). charset is the
 * transport layer's charset (Content-Type), or NULL: the engine then sniffs (BOM, <meta charset> pre-scan, default
 * windows-1252). Returns WEB_OK, WEB_PAUSED (a script asked to pause) or a negative error. */
int web_doc_parse(WebDoc *doc, const void *bytes, size_t len, const char *charset);
int web_doc_parse_bytes(WebDoc *doc, const void *bytes, size_t len, const char *charset);
int web_doc_parse_finish(WebDoc *doc);
int web_doc_parse_resume(WebDoc *doc);                         /* after WEB_PAUSED */
/* document.write: inserts UTF-8 text at the parser's insertion point (during a script run by the parser), or replaces the
 * document when the parse has finished (an implicit document.open, like browsers). */
int web_doc_write(WebDoc *doc, const char *utf8, size_t len);

/* Parse errors (the parser recovers from all of them): count, and the first ones with their WHATWG codes and positions. */
int web_doc_parse_error_count(const WebDoc *doc);
bool web_doc_parse_error(const WebDoc *doc, int i, const char **code, int *line, int *col);

/* ================================================================================================ DOM */

typedef enum {
    WEB_NODE_ELEMENT = 1,
    WEB_NODE_TEXT = 3,
    WEB_NODE_CDATA = 4,              /* never produced by the HTML parser (CDATA in foreign content becomes Text) */
    WEB_NODE_PI = 7,                 /* processing instruction */
    WEB_NODE_COMMENT = 8,
    WEB_NODE_DOCUMENT = 9,
    WEB_NODE_DOCTYPE = 10,
    WEB_NODE_FRAGMENT = 11,          /* DocumentFragment: <template> contents, fragments being built */
} WebNodeType;                        /* the DOM's nodeType numbers */

typedef enum { WEB_NS_HTML = 0, WEB_NS_SVG, WEB_NS_MATHML, WEB_NS_NONE } WebNamespace;

WebNodeType web_node_type(const WebNode *n);
WebNamespace web_node_namespace(const WebNode *n);
const char *web_node_name(const WebNode *n);                   /* local name: "div", "foreignObject", "#text", "#comment",
                                                                  "#document", the doctype's name, the PI's target */
WebDoc  *web_node_doc(const WebNode *n);
WebNode *web_node_parent(const WebNode *n);
WebNode *web_node_first_child(const WebNode *n);
WebNode *web_node_last_child(const WebNode *n);
WebNode *web_node_next_sibling(const WebNode *n);
WebNode *web_node_prev_sibling(const WebNode *n);
WebNode *web_template_content(const WebNode *tmpl);             /* the DocumentFragment of a <template> */
void    *web_node_user(const WebNode *n);                       /* one pointer for the embedder (the JS wrapper object) */
void     web_node_set_user(WebNode *n, void *user);
bool     web_node_is(const WebNode *n, const char *html_tag);   /* an HTML element with that local name */

/* Character data (text, comment, PI): UTF-8, not NUL-terminated beyond len (but always followed by a NUL byte). */
const char *web_node_data(const WebNode *n, size_t *len);
int  web_node_set_data(WebNode *n, const char *utf8, size_t len);
/* textContent: the concatenated text of the subtree (a new string in a buffer the caller frees with web_free). */
char *web_node_text_content(WebNode *n, size_t *len);
int   web_node_set_text_content(WebNode *n, const char *utf8, size_t len);

/* Attributes. Names are matched ASCII case-insensitively on HTML elements (like getAttribute). */
int  web_element_attr_count(const WebNode *el);
bool web_element_attr_at(const WebNode *el, int i, const char **name, const char **value);   /* qualified name */
const char *web_element_get_attr(const WebNode *el, const char *name);       /* NULL when absent */
int  web_element_set_attr(WebNode *el, const char *name, const char *value);
int  web_element_remove_attr(WebNode *el, const char *name);
const char *web_element_id(const WebNode *el);                               /* "" when none */
bool web_element_has_class(const WebNode *el, const char *cls);

/* Creation and mutation (each call sends the mutation records and marks style and layout dirty). */
WebNode *web_doc_create_element(WebDoc *doc, const char *tag);              /* HTML namespace; tag is lowercased */
WebNode *web_doc_create_element_ns(WebDoc *doc, WebNamespace ns, const char *tag);
WebNode *web_doc_create_text(WebDoc *doc, const char *utf8, size_t len);
WebNode *web_doc_create_comment(WebDoc *doc, const char *utf8, size_t len);
WebNode *web_doc_create_fragment(WebDoc *doc);
int  web_node_append_child(WebNode *parent, WebNode *child);
int  web_node_insert_before(WebNode *parent, WebNode *child, WebNode *ref);   /* ref NULL: append */
int  web_node_remove_child(WebNode *parent, WebNode *child);
int  web_node_replace_child(WebNode *parent, WebNode *newc, WebNode *oldc);
WebNode *web_node_clone(WebNode *n, bool deep);
bool web_node_contains(const WebNode *a, const WebNode *b);                  /* b is a or inside a */
int  web_node_compare_position(const WebNode *a, const WebNode *b);          /* <0: a before b in tree order, 0 same, >0 after */

/* Serialization (the HTML fragment serialization algorithm) and fragment parsing (innerHTML). Strings are freed with web_free. */
char *web_node_inner_html(WebNode *n, size_t *len);
char *web_node_outer_html(WebNode *n, size_t *len);
int   web_node_set_inner_html(WebNode *el, const char *utf8, size_t len);

/* Lookup. Lists of nodes are returned in tree order in an array the caller frees with web_free. */
WebNode  *web_doc_get_element_by_id(WebDoc *doc, const char *id);
WebNode **web_get_elements_by_tag(WebNode *root, const char *tag, int *count);      /* "*" for all */
WebNode **web_get_elements_by_class(WebNode *root, const char *classes, int *count); /* space-separated, all must match */
/* Selectors (CSS Selectors level 3 and the common level 4 pieces, docs/WEB.md). A syntax error returns NULL and sets *error
 * to WEB_E_ARG (querySelector throws SyntaxError then). */
WebNode  *web_query_selector(WebNode *root, const char *selectors, int *error);
WebNode **web_query_selector_all(WebNode *root, const char *selectors, int *count, int *error);
bool      web_element_matches(WebNode *el, const char *selectors, int *error);
WebNode  *web_element_closest(WebNode *el, const char *selectors, int *error);

void web_free(WebDoc *doc, void *p);                           /* for strings and arrays the API returned */

/* ================================================================================================ styles */

/* Inline style (element.style): the declarations of the style attribute, as text; setting a property rewrites the attribute. */
char *web_element_style_get(WebNode *el, const char *property);              /* NULL when not set */
int   web_element_style_set(WebNode *el, const char *property, const char *value, bool important);   /* value NULL removes */
/* getComputedStyle: the computed value of a property, serialized ("16px", "rgb(0, 0, 0)", "block"); NULL for unknown
 * properties or when no style was computed yet (call web_doc_layout first). */
char *web_element_computed(WebNode *el, const char *property);

/* Interaction states the selectors :hover, :active, :focus, :target, :checked match. */
void web_doc_set_hover(WebDoc *doc, WebNode *n);               /* the element under the pointer (and its ancestors hover) */
void web_doc_set_active(WebDoc *doc, WebNode *n);
void web_doc_set_focus(WebDoc *doc, WebNode *n);               /* NULL blurs */
WebNode *web_doc_focus(WebDoc *doc);
void web_doc_set_target(WebDoc *doc, const char *fragment);    /* the URL fragment: :target, and the scroll position */

/* ================================================================================================ layout */

/* Recomputes what is dirty (styles, then layout) for a viewport of w x h CSS pixels. Cheap when nothing changed. */
int  web_doc_layout(WebDoc *doc, int viewport_w, int viewport_h);
void web_doc_content_size(WebDoc *doc, int *w, int *h);         /* the scrollable size of the page in CSS pixels */
bool web_element_rect(WebNode *el, WebRect *border_box);       /* getBoundingClientRect in document coordinates; false
                                                                  when the element has no box (display: none) */
/* Scroll positions of the page (the viewport) and of scroll containers (overflow: auto/scroll) inside it. */
void web_doc_set_scroll(WebDoc *doc, int x, int y);
void web_doc_scroll(WebDoc *doc, int *x, int *y);
WebNode *web_doc_scroller_at(WebDoc *doc, int x, int y);       /* the innermost scroll container at a document point */
bool web_element_scroll(WebNode *el, int *x, int *y, int *max_x, int *max_y);
void web_element_set_scroll(WebNode *el, int x, int y);
/* Where the page should scroll to show a fragment (#name) target, or an element. */
bool web_doc_fragment_position(WebDoc *doc, const char *fragment, int *x, int *y);

/* ================================================================================================ painting */

typedef enum {
    WEB_DI_RECT = 1,                 /* rect filled with color; radii non-zero: rounded corners */
    WEB_DI_BORDER,                   /* rect = border box; widths[4] (top, right, bottom, left), colors[4], styles[4], radii */
    WEB_DI_TEXT,                     /* a text run: font, color, UTF-8 text, baseline origin (x64, y64) in 1/64 device px */
    WEB_DI_IMAGE,                    /* image: handle (may be 0 while loading), rect = destination; clip = visible part */
    WEB_DI_CLIP_PUSH,                /* intersect the clip with rect (radii: rounded clip); nested */
    WEB_DI_CLIP_POP,
    WEB_DI_OPACITY_PUSH,             /* paint what follows into a group, composite it with `alpha` (0..255) at the POP */
    WEB_DI_OPACITY_POP,
    WEB_DI_SCROLL_PUSH,              /* a scroll container's visible area (rect) and its scroll offset (sx, sy); the items
                                        inside already have the offset applied: this is for scroll bars and caching */
    WEB_DI_SCROLL_POP,
    WEB_DI_CONTROL,                  /* a form control the UI draws (control: kind, state, text); rect = its border box */
    WEB_DI_ELLIPSE,                  /* filled ellipse in rect (list bullets: disc; with `width` > 0 an outline: circle) */
    WEB_DI_SHADOW,                   /* box-shadow approximated: rect filled with color, blur radius in `width` */
} WebDisplayKind;

typedef enum {
    WEB_BORDER_NONE = 0, WEB_BORDER_SOLID, WEB_BORDER_DOTTED, WEB_BORDER_DASHED, WEB_BORDER_DOUBLE,
    WEB_BORDER_GROOVE, WEB_BORDER_RIDGE, WEB_BORDER_INSET, WEB_BORDER_OUTSET,
} WebBorderStyle;

typedef enum {
    WEB_CONTROL_TEXT = 1,            /* <input> text-like types, text in `text` (password: bullets), placeholder when empty */
    WEB_CONTROL_TEXTAREA,
    WEB_CONTROL_BUTTON,              /* <button> draws its content as normal boxes; this is the frame: <input type=submit...> label in text */
    WEB_CONTROL_CHECKBOX,
    WEB_CONTROL_RADIO,
    WEB_CONTROL_SELECT,              /* drop-down: the selected option's label in text */
    WEB_CONTROL_LISTBOX,             /* <select multiple> or size > 1 */
    WEB_CONTROL_RANGE, WEB_CONTROL_COLOR, WEB_CONTROL_FILE, WEB_CONTROL_PROGRESS, WEB_CONTROL_METER,
} WebControlKind;

#define WEB_CTL_CHECKED   0x01
#define WEB_CTL_DISABLED  0x02
#define WEB_CTL_FOCUSED   0x04
#define WEB_CTL_PLACEHOLDER 0x08     /* text shows the placeholder (draw it greyed) */
#define WEB_CTL_HOVER     0x10
#define WEB_CTL_ACTIVE    0x20

typedef struct WebDisplayItem {
    uint8_t kind;                    /* WebDisplayKind */
    uint8_t control;                 /* WebControlKind for WEB_DI_CONTROL */
    uint8_t alpha;                   /* OPACITY_PUSH */
    uint8_t styles[4];               /* BORDER: WebBorderStyle per side */
    uint16_t state;                  /* CONTROL: WEB_CTL_* */
    WebIRect rect;
    WebColor color;                  /* RECT, TEXT, ELLIPSE, SHADOW; CONTROL: the text colour */
    WebColor colors[4];              /* BORDER */
    uint16_t widths[4];              /* BORDER (device px) */
    uint16_t radii[4];               /* corner radii: top-left, top-right, bottom-right, bottom-left (device px) */
    int width;                       /* ELLIPSE outline width, SHADOW blur */
    int sx, sy;                      /* SCROLL_PUSH offset */
    int32_t x64, y64;                /* TEXT: baseline origin in 1/64 device px */
    WebFontHandle font;              /* TEXT, CONTROL */
    const char *text;                /* TEXT, CONTROL: UTF-8 (not NUL-terminated: use len) */
    uint32_t len;
    WebImageHandle image;            /* IMAGE */
    WebIRect src;                    /* IMAGE: the part of the image to draw (background tiles may cut it); w = 0: all */
    WebNode *node;                   /* the node this item paints, for debugging and accessibility (may be NULL) */
} WebDisplayItem;

typedef struct WebDisplayList {
    WebDisplayItem *items;
    int n;
    WebColor background;             /* the canvas colour (the root's or body's background, else white) */
} WebDisplayList;

/* The items intersecting the rectangle (x, y, w, h) of the page, in CSS pixels of document coordinates (the scroll position
 * and the viewport), in painting order (CSS 2.1 appendix E), with coordinates relative to the rectangle's top-left corner
 * in device pixels; position: fixed boxes stay put. The list lives until the next paint or layout call. */
const WebDisplayList *web_doc_paint(WebDoc *doc, int x, int y, int w, int h);

/* ================================================================================================ interaction */

typedef enum { WEB_CURSOR_DEFAULT = 0, WEB_CURSOR_POINTER, WEB_CURSOR_TEXT, WEB_CURSOR_MOVE, WEB_CURSOR_WAIT,
               WEB_CURSOR_HELP, WEB_CURSOR_NOT_ALLOWED, WEB_CURSOR_CROSSHAIR, WEB_CURSOR_GRAB } WebCursor;

typedef struct WebHit {
    WebNode *node;                   /* the innermost node under the point: a text node over text, else an element */
    int offset;                      /* in a text node: the byte offset of the caret position nearest to the point */
    WebNode *element;                /* the innermost element (node's parent when node is text) */
    WebNode *link;                   /* the nearest <a href>/<area href> ancestor, or NULL */
    const char *href;                /* its resolved URL (valid until the next layout), or NULL */
    WebNode *control;                /* the form control (input, button, select, textarea, label's control), or NULL */
    WebCursor cursor;                /* the cursor property (auto resolved: pointer over links, text over text) */
} WebHit;

/* Document coordinates in CSS pixels (scroll position added). False when the point is outside the page. */
bool web_doc_hit_test(WebDoc *doc, int x, int y, WebHit *out);

/* Text selection: from (anchor, offset) to (focus, offset), offsets as in WebHit. Painted as highlight rectangles under the
 * text (WEB_DI_RECT with the selection colour). web_doc_selection_text returns the selected text (web_free). */
void  web_doc_select(WebDoc *doc, WebNode *anchor, int anchor_offset, WebNode *focus, int focus_offset);
void  web_doc_select_all(WebDoc *doc);
void  web_doc_select_none(WebDoc *doc);
char *web_doc_selection_text(WebDoc *doc, size_t *len);

/* Find in page: the matches of a string in the rendered text (case-insensitive for ASCII); returns the count and selects the
 * match number `which` (0 = first), with its rectangle in *rect so the browser can scroll to it. */
int web_doc_find(WebDoc *doc, const char *needle, int which, WebRect *rect);

/* Activation: what the browser should do when the user clicks (or presses Enter on) a node, unless a script prevented it.
 * Handles links, form submission, checkboxes and radio buttons (their state changes here), labels, <details>. */
typedef enum { WEB_ACT_NONE = 0, WEB_ACT_NAVIGATE, WEB_ACT_SUBMIT, WEB_ACT_TOGGLED, WEB_ACT_FOCUS, WEB_ACT_OPEN_SELECT } WebActionKind;
typedef struct WebAction {
    WebActionKind kind;
    char *url;                       /* NAVIGATE/SUBMIT: the absolute URL (GET: with the query string); web_free */
    char *body;                      /* SUBMIT with method=post: application/x-www-form-urlencoded body; web_free */
    size_t body_len;
    bool post;
    const char *target;              /* the target attribute ("_blank"...) or NULL */
    WebNode *node;                   /* FOCUS/OPEN_SELECT/TOGGLED: the control */
} WebAction;
WebAction web_doc_activate(WebDoc *doc, WebNode *node);

/* Form control values (what the user typed): text inputs and textareas get the UTF-8 value; selects the selected index. */
const char *web_control_value(WebNode *control);
int  web_control_set_value(WebNode *control, const char *utf8);
bool web_control_checked(WebNode *control);
int  web_control_set_checked(WebNode *control, bool on);
int  web_select_index(WebNode *select);
int  web_select_set_index(WebNode *select, int index);
int  web_select_options(WebNode *select, const char **labels, int max);   /* the option labels; returns the count */

/* ================================================================================================ debugging and tests */

/* Text dumps used by the tests (tests/host/webtool.c): the DOM in the html5lib test format, and the box tree with positions.
 * The writer receives pieces of output. */
typedef void (*WebWriter)(void *user, const char *s, size_t n);
void web_dump_dom(WebNode *root, WebWriter w, void *user);
void web_dump_layout(WebDoc *doc, WebWriter w, void *user, int flags);    /* WEB_DUMP_* */
#define WEB_DUMP_TEXT     0x01       /* include text fragments */
#define WEB_DUMP_STYLE    0x02       /* include a few computed values per box */

#ifdef __cplusplus
}
#endif
#endif
