/*
 * Internal declarations of the web engine (user/lib/web): memory, atoms, the DOM, the HTML parser. The CSS, layout and
 * paint parts have their own internal headers (css.h, layout.h) that build on this one.
 */
#ifndef WEB_INT_H
#define WEB_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "web.h"

#define WEB_UNUSED __attribute__((unused))
#define WEB_LIKELY(x) __builtin_expect(!!(x), 1)
#define WEB_UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline int web_min(int a, int b) { return a < b ? a : b; }
static inline int web_max(int a, int b) { return a > b ? a : b; }
static inline WebUnit u_min(WebUnit a, WebUnit b) { return a < b ? a : b; }
static inline WebUnit u_max(WebUnit a, WebUnit b) { return a > b ? a : b; }

/* ================================================================================================ memory (mem.c) */

/* What a document may spend: every arena of the document draws on the same budget. */
typedef struct WebBudget {
    WebAllocator al;
    size_t used, limit;
    bool exceeded;
} WebBudget;

/* An arena: bump allocation from 64 KiB chunks, with free lists per size class (16 .. 4096 bytes) so that memory freed while
 * the page lives (old computed styles, replaced strings) is used again; bigger blocks are allocated one by one and can be
 * freed. wa_reset/wa_destroy release everything at once. Memory comes back zeroed. */
#define WA_CLASSES 9
typedef struct WebArena {
    WebBudget *budget;
    struct WaChunk *chunks;
    struct WaBig *big;
    char *cur, *end;
    void *free_list[WA_CLASSES];
    size_t total;               /* bytes taken from the budget */
} WebArena;

void  wa_init(WebArena *a, WebBudget *b);
void *wa_alloc(WebArena *a, size_t n);
void  wa_free(WebArena *a, void *p, size_t n);
void *wa_realloc(WebArena *a, void *p, size_t old, size_t n);
char *wa_strndup(WebArena *a, const char *s, size_t n);
char *wa_strdup(WebArena *a, const char *s);
void  wa_reset(WebArena *a);
void  wa_destroy(WebArena *a);

/* Plain allocations outside an arena (results handed to the embedder, freed with web_free) */
void *web_raw_alloc(WebBudget *b, size_t n);
void  web_raw_free(WebBudget *b, void *p);

/* A growable byte buffer in an arena */
typedef struct WebBuf {
    char *p;
    uint32_t n, cap;
    WebArena *a;
} WebBuf;

void buf_init(WebBuf *b, WebArena *a);
bool buf_reserve(WebBuf *b, size_t extra);
bool buf_put(WebBuf *b, const void *s, size_t n);
bool buf_putc(WebBuf *b, char c);
bool buf_puts(WebBuf *b, const char *s);
bool buf_put_cp(WebBuf *b, uint32_t cp);           /* UTF-8 encode (surrogates as WTF-8) */
bool buf_put_int(WebBuf *b, long v);
void buf_free(WebBuf *b);
static inline void buf_clear(WebBuf *b) { b->n = 0; }

/* Growable arrays: VEC(T) v; vec_push(arena, v, x) */
#define VEC(T) struct { T *v; uint32_t n, cap; }
bool vec_grow_(WebArena *a, void **v, uint32_t *cap, uint32_t need, size_t esz);
#define vec_reserve(a, vec, need) ((need) <= (vec).cap || vec_grow_((a), (void **)&(vec).v, &(vec).cap, (need), sizeof(*(vec).v)))
#define vec_push(a, vec, x) (vec_reserve(a, vec, (vec).n + 1) ? ((vec).v[(vec).n++] = (x), true) : false)
#define vec_free(a, vec) do { if ((vec).v) wa_free((a), (vec).v, (size_t)(vec).cap * sizeof(*(vec).v)); (vec).v = NULL; (vec).n = (vec).cap = 0; } while (0)

/* UTF-8 helpers */
uint32_t utf8_decode(const char *s, size_t n, size_t *len);   /* one code point; *len = bytes used (>= 1); bad: U+FFFD */
int utf8_encode(uint32_t cp, char out[4]);

/* ================================================================================================ atoms (atoms.c) */

typedef uint32_t WebAtom;

enum {
#define X(id, s) id,
#include "atomlist.h"
#undef X
    AT_STATIC_COUNT
};

typedef struct AtomEntry { const char *s; uint32_t len, hash; } AtomEntry;
typedef struct AtomTable {
    AtomEntry *names;
    uint32_t count, cap;
    uint32_t *slots;            /* open addressing, slot = id + 1, 0 = empty */
    uint32_t nslots;
    WebArena *a;
} AtomTable;

bool        atoms_init(AtomTable *t, WebArena *a);
WebAtom     atom_get(AtomTable *t, const char *s, size_t n);        /* creates when absent; AT_NONE on memory failure */
WebAtom     atom_find(const AtomTable *t, const char *s, size_t n); /* AT_NONE when absent */
WebAtom     atom_get_lower(AtomTable *t, const char *s, size_t n);  /* ASCII-lowercased first */
const char *atom_str(const AtomTable *t, WebAtom a);
uint32_t    atom_len(const AtomTable *t, WebAtom a);
uint32_t    web_hash(const char *s, size_t n);

/* ================================================================================================ DOM (dom.c) */

typedef struct WebAttr {
    WebAtom name;               /* local name (lowercase for HTML attributes, adjusted case for foreign ones) */
    WebAtom prefix;             /* AT_xlink, AT_xml, AT_xmlns or AT_NONE */
    uint8_t ns;                 /* ATTR_NS_* */
    char *value;
    uint32_t len;
} WebAttr;

enum { ATTR_NS_NONE = 0, ATTR_NS_XLINK, ATTR_NS_XML, ATTR_NS_XMLNS };

/* node flags */
#define NF_PARSER_INSERTED  0x0001  /* script: inserted by the parser */
#define NF_STARTED          0x0002  /* script: "already started" */
#define NF_STYLE_DIRTY      0x0004  /* this element's style must be recomputed */
#define NF_CHILD_DIRTY      0x0008  /* some descendant has NF_STYLE_DIRTY */
#define NF_HOVER            0x0010
#define NF_ACTIVE           0x0020
#define NF_FOCUS            0x0040
#define NF_TARGET           0x0080
#define NF_FORM_INSERTED    0x0100  /* form-associated: the form owner was set by the parser */
#define NF_HTML_INTEGRATION 0x0200  /* annotation-xml with encoding text/html or application/xhtml+xml */
#define NF_CHECKED_DIRTY    0x0400  /* control state changed by the user: the attribute no longer decides */
#define NF_IN_DOC           0x0800  /* connected to the document (maintained lazily: see dom.c) */

struct WebNode {
    uint8_t type;               /* WebNodeType */
    uint8_t ns;                 /* WebNamespace of an element */
    uint16_t flags;
    WebAtom name;               /* element local name, PI target */
    WebNode *parent, *first, *last, *prev, *next;
    WebDoc *doc;
    void *user;
    struct LBox *box;           /* the principal layout box (valid after layout), or NULL */
    union {
        struct {                /* elements */
            WebAttr *attrs;
            uint16_t nattrs, cattrs;
            uint16_t nclasses, cclasses;
            WebAtom id;         /* the id attribute's value as an atom (AT_NONE when absent or empty) */
            WebAtom *classes;
            WebNode *content;   /* template contents (a fragment) */
            struct CStyle *style;            /* computed style (style.c), NULL when not rendered */
            struct CStyle *pstyle[4];        /* ::before, ::after, ::marker, ::placeholder */
            struct CssDecls *inline_decls;   /* parsed style attribute */
            struct WebControl *ctl;          /* form control state (forms.c) */
            struct WebRes *res;              /* the image this element shows (img, input type=image) */
            WebNode *form;                   /* form owner */
            int32_t scroll_x, scroll_y;      /* the scroll offset of a scroll container, in layout units */
        } el;
        struct {                /* text, comment, PI; doctype (data = name) */
            char *data;
            uint32_t len, cap;
            char *public_id, *system_id;     /* doctype: NULL = missing */
        } cd;
    } u;
};

static inline bool is_el(const WebNode *n) { return n && n->type == WEB_NODE_ELEMENT; }
static inline bool is_html(const WebNode *n, WebAtom name) { return n && n->type == WEB_NODE_ELEMENT && n->ns == WEB_NS_HTML && n->name == name; }
static inline bool is_html_el(const WebNode *n) { return n && n->type == WEB_NODE_ELEMENT && n->ns == WEB_NS_HTML; }

WebNode *dom_new_node(WebDoc *d, int type);
WebNode *dom_new_element(WebDoc *d, int ns, WebAtom name);
WebNode *dom_new_text(WebDoc *d, const char *s, size_t n);
WebNode *dom_new_comment(WebDoc *d, const char *s, size_t n);
bool dom_text_append(WebNode *t, const char *s, size_t n);
bool dom_set_data(WebNode *t, const char *s, size_t n);
void dom_append(WebNode *parent, WebNode *child);                   /* raw: no checks, no notifications */
void dom_insert_before(WebNode *parent, WebNode *child, WebNode *ref);
void dom_remove(WebNode *child);
WebAttr *dom_attr(const WebNode *el, WebAtom name);                  /* attribute in no namespace */
const char *dom_attr_val(const WebNode *el, WebAtom name);           /* NULL when absent */
bool dom_add_attr(WebNode *el, WebAtom name, const char *v, size_t n, int ns, WebAtom prefix);   /* appends, no checks */
bool dom_set_attr(WebNode *el, WebAtom name, const char *v, size_t n);
void dom_attr_changed(WebNode *el, WebAtom name);                    /* keep id/class caches, mark dirty */
void dom_mark_style_dirty(WebNode *n);                               /* this element (and the path above) */
void dom_mark_subtree_dirty(WebNode *n);
void dom_notify(WebDoc *d, const WebMutation *m);
WebNode *dom_next(const WebNode *n, const WebNode *root);            /* pre-order successor within root */
WebNode *dom_next_skip(const WebNode *n, const WebNode *root);       /* successor that skips n's children */
void dom_text_of(const WebNode *n, WebBuf *out);                     /* textContent */
bool dom_attr_is(const WebNode *el, WebAtom name, const char *lowercase_value);   /* ASCII case-insensitive */
void dom_serialize(WebNode *n, WebBuf *out, bool outer);

/* ================================================================================================ the document */

typedef struct WebParseError { uint16_t code; int line, col; } WebParseError;

struct WebDoc {
    WebConfig cfg;
    WebBudget budget;
    WebArena arena;             /* the DOM, stylesheets, computed styles */
    WebArena larena;            /* layout boxes: reset by every layout */
    WebArena parena;            /* display lists: reset by every paint */
    AtomTable atoms;
    WebNode *root;              /* the Document node */
    char *url, *base_url;
    char *title_cache;
    int quirks;                 /* 0 no-quirks, 1 limited, 2 quirks */
    const char *charset;
    int status;
    struct HtmlParser *parser;
    bool parsing, parse_done, scripting;
    int nerrors;
    VEC(WebParseError) errors;
    /* interaction state */
    WebNode *hover, *active, *focus, *target;
    int scroll_x, scroll_y;
    int vw, vh;                 /* the viewport in CSS pixels (media queries, vw/vh units), 800 x 600 until a layout */
    /* subsystems */
    struct CssEngine *css;      /* stylesheets, rule index (style.c) */
    struct LayoutState *lay;    /* the box tree of the last layout (layout.c) */
    struct ResTable *res;       /* subresources (resource.c) */
    struct SelState *sel;       /* selection, find (interact.c) */
    struct FontCache *fonts;    /* fonts.c */
    struct ImgTable *images;    /* image.c */
    bool style_dirty, layout_dirty;
    bool in_mutation;           /* suppress notifications while the parser builds the initial tree */
    uint32_t dom_version;       /* incremented by every mutation */
    char qname[300];            /* scratch for qualified attribute names handed out by web_element_attr_at */
};

void doc_error(WebDoc *d, int code, size_t offset);
const char *doc_resolve_url(WebDoc *d, const char *rel, size_t n, WebArena *a);   /* absolute URL or NULL */

/* ================================================================================================ the HTML parser */

/* parse error codes (WHATWG names in tokenizer.c's table) */
enum {
    PE_NONE = 0,
#define E(id, s) id,
#include "errlist.h"
#undef E
    PE_COUNT
};
extern const char *const web_parse_error_names[];

typedef enum {
    TS_DATA = 0, TS_RCDATA, TS_RAWTEXT, TS_SCRIPT_DATA, TS_PLAINTEXT, TS_TAG_OPEN, TS_END_TAG_OPEN, TS_TAG_NAME,
    TS_RCDATA_LT, TS_RCDATA_END_OPEN, TS_RCDATA_END_NAME, TS_RAWTEXT_LT, TS_RAWTEXT_END_OPEN, TS_RAWTEXT_END_NAME,
    TS_SCRIPT_LT, TS_SCRIPT_END_OPEN, TS_SCRIPT_END_NAME, TS_SCRIPT_ESC_START, TS_SCRIPT_ESC_START_DASH, TS_SCRIPT_ESC,
    TS_SCRIPT_ESC_DASH, TS_SCRIPT_ESC_DASH_DASH, TS_SCRIPT_ESC_LT, TS_SCRIPT_ESC_END_OPEN, TS_SCRIPT_ESC_END_NAME,
    TS_SCRIPT_DESC_START, TS_SCRIPT_DESC, TS_SCRIPT_DESC_DASH, TS_SCRIPT_DESC_DASH_DASH, TS_SCRIPT_DESC_LT,
    TS_SCRIPT_DESC_END, TS_BEFORE_ATTR_NAME, TS_ATTR_NAME, TS_AFTER_ATTR_NAME, TS_BEFORE_ATTR_VALUE, TS_ATTR_VALUE_DQ,
    TS_ATTR_VALUE_SQ, TS_ATTR_VALUE_UQ, TS_AFTER_ATTR_VALUE_Q, TS_SELF_CLOSING, TS_BOGUS_COMMENT, TS_MARKUP_DECL,
    TS_COMMENT_START, TS_COMMENT_START_DASH, TS_COMMENT, TS_COMMENT_LT, TS_COMMENT_LT_BANG, TS_COMMENT_LT_BANG_DASH,
    TS_COMMENT_LT_BANG_DASH_DASH, TS_COMMENT_END_DASH, TS_COMMENT_END, TS_COMMENT_END_BANG, TS_DOCTYPE,
    TS_BEFORE_DOCTYPE_NAME, TS_DOCTYPE_NAME, TS_AFTER_DOCTYPE_NAME, TS_AFTER_DOCTYPE_PUBLIC_KW,
    TS_BEFORE_DOCTYPE_PUBLIC_ID, TS_DOCTYPE_PUBLIC_ID_DQ, TS_DOCTYPE_PUBLIC_ID_SQ, TS_AFTER_DOCTYPE_PUBLIC_ID,
    TS_BETWEEN_DOCTYPE_IDS, TS_AFTER_DOCTYPE_SYSTEM_KW, TS_BEFORE_DOCTYPE_SYSTEM_ID, TS_DOCTYPE_SYSTEM_ID_DQ,
    TS_DOCTYPE_SYSTEM_ID_SQ, TS_AFTER_DOCTYPE_SYSTEM_ID, TS_BOGUS_DOCTYPE, TS_CDATA, TS_CDATA_BRACKET, TS_CDATA_END,
    TS_PI_OPEN, TS_PI_TARGET, TS_AFTER_PI_TARGET, TS_PI_DATA, TS_PI_QUESTIONABLE,
    TS_CHARREF, TS_NAMED_CHARREF, TS_AMBIGUOUS_AMP, TS_NUMERIC_CHARREF, TS_HEX_CHARREF_START, TS_DEC_CHARREF_START,
    TS_HEX_CHARREF, TS_DEC_CHARREF, TS_NUMERIC_CHARREF_END,
    TS_COUNT
} TokState;

typedef enum { HT_DOCTYPE = 1, HT_START, HT_END, HT_COMMENT, HT_CHARS, HT_PI, HT_EOF } HtmlTokType;

typedef struct HtmlAttr {
    WebAtom name;               /* lowercased name; AT_NONE while the name is being read */
    uint32_t voff, vlen;        /* value in the tokenizer's attribute value buffer */
} HtmlAttr;

typedef struct HtmlToken {
    HtmlTokType type;
    bool self_closing, ack, force_quirks;
    WebAtom name;               /* tags; PI target */
    const char *data;           /* characters, comment, PI data; doctype name */
    uint32_t len;
    bool dt_name_missing, dt_pub_missing, dt_sys_missing;
    const char *dt_pub, *dt_sys;
    uint32_t dt_pub_len, dt_sys_len;
    HtmlAttr *attrs;
    int nattrs;
    const char *vals;           /* attribute values: vals + attrs[i].voff */
    size_t offset;              /* input offset where the token started (errors) */
} HtmlToken;

typedef void (*TokSink)(void *user, HtmlToken *t);

typedef struct HtmlTokenizer {
    WebDoc *doc;
    WebArena *a;
    const char *in;             /* normalized UTF-8 (LF line ends) */
    size_t n, pos;
    bool eof;                   /* no more input will come */
    TokState state, ret_state;
    TokSink sink;
    void *sink_user;
    bool (*cdata_allowed)(void *user);    /* adjusted current node is not in the HTML namespace */
    void (*error)(void *user, int code, size_t offset);
    /* the token being built */
    HtmlToken tok;
    WebBuf tagname, name, val, data, tmp, dtpub, dtsys;
    VEC(HtmlAttr) attrs;
    bool drop_attr;             /* duplicate attribute name: the attribute being built is discarded */
    bool is_end;
    char last_start[32];        /* tag name of the last start tag emitted (for appropriate end tags) */
    uint32_t last_start_len;
    uint32_t charref_code;
    bool charref_overflow;
    size_t tok_start;
    bool paused;                /* the sink asked to stop (script) */
    bool stop;                  /* stop after the current token (EOF emitted) */
} HtmlTokenizer;

void tok_init(HtmlTokenizer *t, WebDoc *d, WebArena *a);
void tok_set_input(HtmlTokenizer *t, const char *in, size_t n, bool eof);
int  tok_run(HtmlTokenizer *t);         /* 0: needs more input or done; 1: paused by the sink */
void tok_set_last_start(HtmlTokenizer *t, const char *name, size_t n);

/* named character references (entities.c) */
typedef struct WebEntity { uint16_t off; uint8_t len; uint8_t ncp; uint32_t cp[2]; } WebEntity;
extern const WebEntity web_entities[];
extern const int web_entity_count;
extern const char web_entity_names[];

typedef enum {
    IM_INITIAL = 0, IM_BEFORE_HTML, IM_BEFORE_HEAD, IM_IN_HEAD, IM_IN_HEAD_NOSCRIPT, IM_AFTER_HEAD, IM_IN_BODY, IM_TEXT,
    IM_IN_TABLE, IM_IN_TABLE_TEXT, IM_IN_CAPTION, IM_IN_COLUMN_GROUP, IM_IN_TABLE_BODY, IM_IN_ROW, IM_IN_CELL,
    IM_IN_TEMPLATE, IM_AFTER_BODY, IM_IN_FRAMESET, IM_AFTER_FRAMESET, IM_AFTER_AFTER_BODY, IM_AFTER_AFTER_FRAMESET,
} InsertionMode;

typedef struct FmtEntry {
    WebNode *el;                /* NULL = marker */
    WebAtom name;
    WebAttr *attrs;             /* a copy of the token's attributes */
    int nattrs;
} FmtEntry;

typedef struct HtmlTreeBuilder {
    WebDoc *doc;
    HtmlTokenizer *tok;
    WebArena *a;
    InsertionMode mode, orig_mode;
    VEC(InsertionMode) tmpl_modes;
    VEC(WebNode *) open;        /* the stack of open elements */
    VEC(FmtEntry) fmt;          /* the list of active formatting elements */
    WebNode *head, *form;
    WebNode *context;           /* fragment parsing context element */
    WebNode *fragment_root;     /* fragment case: the html element whose children are the result */
    bool scripting, frameset_ok, foster, fragment, ignore_lf, done;
    WebBuf pending;             /* pending table character tokens */
    bool pending_nonspace;
    int max_depth;
    WebNode *pending_script;    /* a script whose end tag was just seen */
} HtmlTreeBuilder;

void tb_init(HtmlTreeBuilder *tb, WebDoc *d, HtmlTokenizer *t, WebArena *a);
void tb_token(void *user, HtmlToken *t);     /* TokSink */
bool tb_cdata_allowed(void *user);
void tb_init_fragment(HtmlTreeBuilder *tb, WebNode *context);

/* encoding (encoding.c) */
typedef enum { ENC_UTF8 = 0, ENC_WINDOWS_1252, ENC_UTF16LE, ENC_UTF16BE, ENC_SHIFT_JIS, ENC_EUC_JP, ENC_ISO_2022_JP,
               ENC_X_USER_DEFINED, ENC_REPLACEMENT, ENC_COUNT } WebEncoding;
int  enc_from_label(const char *label, size_t n);        /* -1 when unknown */
const char *enc_name(int enc);
int  enc_sniff_bom(const unsigned char *p, size_t n, size_t *bom_len);   /* -1 when none */
int  enc_prescan(const unsigned char *p, size_t n);      /* <meta charset> pre-scan; -1 when nothing found */
int  enc_from_meta_content(const char *v, size_t n);     /* content="text/html; charset=..." */
typedef struct WebDecoder { int enc; uint32_t state[4]; bool cr; } WebDecoder;
void dec_init(WebDecoder *d, int enc);
/* Decodes bytes into out (UTF-8, CR LF and CR normalized to LF). last: the stream ends with these bytes. */
bool dec_run(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out);
void enc_input_errors(const char *utf8, size_t n, void (*cb)(void *user, int code, size_t offset), void *user);

/* the parser (parser.c) */
typedef struct HtmlParser {
    WebDoc *doc;
    WebArena a;                 /* parser-only memory, released when the parse ends */
    WebDecoder dec;
    bool dec_ready, enc_certain;
    bool nonascii;              /* a byte >= 0x80 was seen (a late <meta charset> then needs a restart) */
    int restart_enc;            /* >= 0: parse again from the bytes with this encoding */
    char *charset_hint;         /* the transport layer's charset */
    WebBuf raw;                 /* bytes kept while the encoding is tentative */
    WebBuf text;                /* decoded input */
    HtmlTokenizer tok;
    HtmlTreeBuilder tb;
    int script_nesting;
    uint32_t script_wait;       /* the resource id of a parser-blocking script being fetched */
    bool paused, finished, finish_pending;
} HtmlParser;

int  parse_fragment(WebDoc *d, WebNode *context, const char *utf8, size_t len, WebNode *into);
void tb_finish(HtmlTreeBuilder *tb);

/* tests and tools (parser.c): run the tokenizer alone with a recording sink */
typedef void (*TokTestSink)(void *user, WebDoc *d, const HtmlToken *t);
int web_test_tokenize(const char *utf8, size_t len, int initial_state, const char *last_start_tag,
                      TokTestSink sink, void *user, void (*err)(void *user, int code, int line, int col), void *err_user);

#endif
