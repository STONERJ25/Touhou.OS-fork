/*
 * Internal declarations of the style system: CSS tokens (csstok.c), stylesheets, rules and declarations (css.c),
 * selectors (selector.c), the cascade and computed styles (style.c).
 */
#ifndef WEB_CSS_H
#define WEB_CSS_H

#include "webint.h"

/* ================================================================================================ tokens (CSS Syntax 3) */

typedef enum {
    CT_EOF = 0, CT_IDENT, CT_FUNCTION, CT_AT, CT_HASH, CT_STRING, CT_BAD_STRING, CT_URL, CT_BAD_URL, CT_DELIM,
    CT_NUMBER, CT_PERCENT, CT_DIMENSION, CT_WS, CT_CDO, CT_CDC, CT_COLON, CT_SEMI, CT_COMMA, CT_LBRACK, CT_RBRACK,
    CT_LPAREN, CT_RPAREN, CT_LBRACE, CT_RBRACE,
} CssTokType;

#define CTF_INT 1                   /* the number had no fraction and no exponent */
#define CTF_ID  2                   /* a hash token whose value is an identifier */

typedef struct CssTok {
    uint8_t type, flags;
    uint16_t delim;                 /* CT_DELIM: the character (ASCII; others as 0xFFFF) */
    uint32_t vs, vn;                /* the value in the pool: name, string, url, hash; a dimension's unit */
    uint32_t pos, end;              /* the source range */
    uint32_t close;                 /* function and block openers: the index of the closing token (or of the EOF token) */
    double num;
} CssTok;

typedef struct CssTokens {
    CssTok *v;
    uint32_t n;                     /* not counting the final CT_EOF, which is always there */
    uint32_t cap;
    WebBuf pool;                    /* token values, NUL-separated */
    const char *src;
} CssTokens;

bool css_tokenize(WebArena *a, const char *src, size_t n, CssTokens *out);
void css_tokens_free(WebArena *a, CssTokens *t);
static inline const char *tok_val(const CssTokens *t, const CssTok *k) { return t->pool.p + k->vs; }
bool tok_is_ident(const CssTokens *t, const CssTok *k, const char *lower);     /* ASCII case-insensitive */

/* ================================================================================================ keywords, properties */

enum {
#define K(id, s) id,
#include "csskw.h"
#undef K
    K_COUNT
};
int css_keyword(const char *s, size_t n);           /* K_* or -1 */
const char *css_keyword_name(int k);

enum {
#define P(id, s, inh) id,
#include "cssprops.h"
#undef P
    CP_COUNT,
    CP_CUSTOM = 0x7FFF,             /* --name */
};
int css_property(const char *s, size_t n);          /* CP_* or -1 */
const char *css_property_name(int p);
extern const uint8_t css_prop_inherited[CP_COUNT];

/* ================================================================================================ values */

enum {                              /* units of specified lengths */
    SU_NONE = 0, SU_PX, SU_EM, SU_REM, SU_EX, SU_CH, SU_VW, SU_VH, SU_VMIN, SU_VMAX, SU_PCT, SU_FR, SU_LH,
};

typedef struct CssCalc CssCalc;     /* a calc() expression; see cssval.c */
typedef struct CalcEnv { float em, rem, vw, vh, ex, ch; } CalcEnv;
bool calc_linear(const CssCalc *n, const CalcEnv *e, float *px, float *pct);   /* px + pct%, when that form exists */
float calc_eval(const CssCalc *n, const CalcEnv *e, float basis);
CssCalc *calc_resolve(WebArena *a, const CssCalc *n, const CalcEnv *e);        /* a copy with lengths in px */

enum { SV_NONE = 0, SV_KW, SV_LEN, SV_NUM, SV_COLOR, SV_STR, SV_URL, SV_PTR, SV_CALC };

typedef struct SVal {
    uint8_t kind;
    uint8_t unit;                   /* SV_LEN: SU_* */
    uint16_t kw;                    /* SV_KW: K_*; SV_PTR: what the pointer is */
    float num;                      /* SV_LEN, SV_NUM */
    union { WebColor color; const char *str; void *ptr; CssCalc *calc; } u;
} SVal;

/* colours: 0xAARRGGBB; the special value COLOR_CURRENT stands for currentcolor until computed */
#define COLOR_CURRENT 0x00FFFFFEu   /* transparent "near white": never produced by a colour literal */
bool css_parse_color(const CssTokens *t, uint32_t *i, uint32_t end, WebColor *out);

typedef struct CssDecl {
    uint16_t prop;                  /* CP_* or CP_CUSTOM */
    uint8_t important;
    uint8_t wide;                   /* 0 or K_INHERIT, K_INITIAL, K_UNSET, K_REVERT */
    uint8_t pending;                /* the value contains var(): raw text in v0.u.str, to parse as `via` */
    uint16_t via;                   /* the property (or shorthand, SH_*) the pending text belongs to */
    WebAtom name;                   /* CP_CUSTOM: the atom of "--name" */
    SVal v0, v1;
} CssDecl;

typedef struct CssDecls {
    CssDecl *v;
    uint32_t n, cap;
} CssDecls;

/* Parses a declaration list (a style attribute, or the body of a rule) and appends to out. */
void css_parse_decls(WebDoc *d, const CssTokens *t, uint32_t start, uint32_t end, CssDecls *out);
CssDecls *css_parse_style_attr(WebDoc *d, const char *s, size_t n);
/* Parses one property's value from text (var() substitution, @supports); false when invalid. */
bool css_parse_value_text(WebDoc *d, int prop_or_sh, const char *s, size_t n, CssDecls *out, bool important);
int  css_shorthand(const char *s, size_t n);         /* SH_* (>= 0x4000) or -1 */

/* complex values */
typedef struct FontList { const char *const *names; int n; uint32_t hash; } FontList;
enum { CC_STRING, CC_ATTR, CC_COUNTER, CC_COUNTERS, CC_OPEN_QUOTE, CC_CLOSE_QUOTE, CC_NO_OPEN_QUOTE, CC_NO_CLOSE_QUOTE, CC_URL };
typedef struct ContentItem { uint8_t kind; uint8_t style; const char *s; WebAtom name; const char *sep; } ContentItem;
typedef struct CssContent { ContentItem *v; int n; bool none, normal; } CssContent;
typedef struct CounterOp { WebAtom name; int value; } CounterOp;
typedef struct CounterList { CounterOp *v; int n; } CounterList;
typedef struct Quotes { const char **v; int n; } Quotes;    /* pairs: open, close, open, close ... */
typedef struct Shadow { float x, y, blur, spread; WebColor color; bool inset; } Shadow;

/* grid tracks: a track is a sizing function (min, max) each a CLen-like value */
enum { TS_LEN = 0, TS_FR, TS_AUTO, TS_MIN_CONTENT, TS_MAX_CONTENT, TS_FIT_CONTENT };
typedef struct TrackSize { uint8_t kind; uint8_t unit; float v; } TrackSize;
typedef struct Track { TrackSize min, max; WebAtom *names; int nnames; } Track;       /* names: the lines before it */
typedef struct TrackList {
    Track *v; int n;
    int repeat_at, repeat_n;        /* auto-fill/auto-fit: tracks v[repeat_at .. repeat_at+repeat_n) repeat */
    bool auto_fit, has_auto_repeat;
    WebAtom *end_names; int nend;   /* names of the last line */
} TrackList;
typedef struct GridAreas { int rows, cols; WebAtom *cells; } GridAreas;              /* rows*cols atoms, AT_NONE for "." */
typedef struct GridLine { int16_t n; uint8_t span; WebAtom name; bool autoval; } GridLine;

/* ================================================================================================ selectors */

enum { CB_DESCENDANT = 0, CB_CHILD, CB_ADJACENT, CB_SIBLING };
enum { SA_EXISTS = 0, SA_EQ, SA_INCLUDES, SA_DASH, SA_PREFIX, SA_SUFFIX, SA_SUBSTR };
enum {
    PC_NONE = 0, PC_ROOT, PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD,
    PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE, PC_EMPTY,
    PC_LINK, PC_VISITED, PC_ANY_LINK, PC_HOVER, PC_ACTIVE, PC_FOCUS, PC_FOCUS_WITHIN, PC_FOCUS_VISIBLE, PC_TARGET,
    PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_REQUIRED, PC_OPTIONAL, PC_READ_ONLY, PC_READ_WRITE, PC_PLACEHOLDER_SHOWN,
    PC_DEFAULT, PC_INDETERMINATE, PC_LANG, PC_DIR, PC_NOT, PC_IS, PC_WHERE, PC_HAS, PC_SCOPE, PC_DEFINED,
    PC_OPEN, PC_NEVER,
};
enum { PSE_NONE = 0, PSE_BEFORE, PSE_AFTER, PSE_MARKER, PSE_PLACEHOLDER, PSE_FIRST_LINE, PSE_FIRST_LETTER,
       PSE_SELECTION, PSE_OTHER };

struct SelList;
typedef struct SelAttr { WebAtom name; uint8_t op; bool ci; const char *val; uint32_t vlen; } SelAttr;
typedef struct SelPseudo { uint8_t type; int a, b; struct SelList *list; const char *arg; } SelPseudo;
typedef struct SelCompound {
    WebAtom tag;                    /* AT_NONE: any element; the lowercased name */
    WebAtom tag_cs;                 /* the name as written, for SVG/MathML elements */
    WebAtom id;
    WebAtom *classes;
    SelAttr *attrs;
    SelPseudo *pseudos;
    uint8_t nclasses, nattrs, npseudos;
    uint8_t comb;                   /* the combinator between this compound and the next one to its left */
} SelCompound;
typedef struct Selector {
    SelCompound *c;                 /* right to left: c[0] is the subject */
    uint16_t n;
    uint8_t pseudo_el;              /* PSE_* of the subject */
    uint32_t spec;                  /* specificity: a << 20 | b << 10 | c */
} Selector;
typedef struct SelList { Selector *v; uint32_t n; } SelList;

bool sel_parse(WebDoc *d, WebArena *a, const CssTokens *t, uint32_t start, uint32_t end, SelList *out);
bool sel_parse_text(WebDoc *d, WebArena *a, const char *s, size_t n, SelList *out);
typedef struct MatchCtx {
    WebNode *scope;                 /* :scope (querySelector's root), else the root element */
    uint8_t pseudo;                 /* the pseudo-element being matched (PSE_*), PSE_NONE for elements */
    bool uses_hover;
    WebNode *anchor;                /* :has(): the element the relative selector is anchored to */
    uint8_t anchor_comb;            /* and the leading combinator relating the leftmost compound to it */
} MatchCtx;
bool sel_matches(const Selector *s, WebNode *el, MatchCtx *ctx);
bool sel_list_matches(const SelList *l, WebNode *el, MatchCtx *ctx);

/* ================================================================================================ stylesheets */

enum { OR_UA = 0, OR_USER, OR_AUTHOR };

typedef struct MExpr MExpr;                  /* a media condition (css.c) */
typedef struct MediaList { MExpr **q; int n; } MediaList;      /* queries joined by commas: any may match */
typedef struct MediaCond { const MediaList *m; const struct MediaCond *parent; } MediaCond;   /* nested @media */
bool media_matches(WebDoc *d, const MediaList *m);       /* NULL or empty matches */
bool media_cond_matches(WebDoc *d, const MediaCond *c);
MediaList *media_parse_text(WebDoc *d, const char *s, size_t n);

typedef struct CssRule {
    SelList sel;
    CssDecls decls;
    const MediaCond *cond;          /* the @media blocks around the rule, or NULL */
    uint32_t order;
} CssRule;

typedef struct CssSheet {
    struct CssSheet *next;          /* document order */
    VEC(CssRule *) rules;
    VEC(struct CssSheet *) imports;
    const MediaList *media;
    uint8_t origin;
    WebNode *owner;                 /* <style> or <link> */
    char *url;                      /* base for url() inside it */
    bool loading, failed, disabled;
    uint32_t req;                   /* resource id while loading */
    uint32_t version;
} CssSheet;

CssSheet *css_parse_sheet(WebDoc *d, const char *text, size_t n, uint8_t origin, const char *base_url, int depth);

/* ================================================================================================ computed styles */

enum { CL_PX = 0, CL_AUTO, CL_NONE, CL_MIN_CONTENT, CL_MAX_CONTENT, CL_FIT_CONTENT, CL_CONTENT, CL_CALC };
typedef struct CLen { float px, pct; uint8_t kind; CssCalc *calc; } CLen;   /* px + pct% of the basis */

enum { D_NONE = 0, D_INLINE, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID,
       D_TABLE, D_INLINE_TABLE, D_TABLE_ROW_GROUP, D_TABLE_HEADER_GROUP, D_TABLE_FOOTER_GROUP, D_TABLE_ROW,
       D_TABLE_CELL, D_TABLE_COLUMN_GROUP, D_TABLE_COLUMN, D_TABLE_CAPTION, D_CONTENTS, D_FLOW_ROOT };
enum { POS_STATIC = 0, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { LH_NORMAL = 0, LH_NUMBER, LH_PX };
enum { WS_NORMAL = 0, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE, WS_BREAK_SPACES };
enum { TA_START = 0, TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY, TA_END, TA_WEBKIT_CENTER };   /* the last also centers blocks */
enum { OV_VISIBLE = 0, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum { VA_BASELINE = 0, VA_SUB, VA_SUPER, VA_TEXT_TOP, VA_TEXT_BOTTOM, VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_LEN };
enum { AL_AUTO = 0, AL_NORMAL, AL_STRETCH, AL_START, AL_END, AL_CENTER, AL_FLEX_START, AL_FLEX_END, AL_BASELINE,
       AL_LAST_BASELINE, AL_SPACE_BETWEEN, AL_SPACE_AROUND, AL_SPACE_EVENLY, AL_LEFT, AL_RIGHT, AL_SELF_START,
       AL_SELF_END };
enum { LT_NONE = 0, LT_DISC, LT_CIRCLE, LT_SQUARE, LT_DECIMAL, LT_DECIMAL_LZ, LT_LOWER_ALPHA, LT_UPPER_ALPHA,
       LT_LOWER_ROMAN, LT_UPPER_ROMAN, LT_LOWER_GREEK, LT_HIRAGANA, LT_KATAKANA, LT_CJK_DECIMAL, LT_DISCLOSURE_OPEN,
       LT_DISCLOSURE_CLOSED, LT_STRING };
enum { BG_REPEAT = 0, BG_REPEAT_X, BG_REPEAT_Y, BG_NO_REPEAT, BG_SPACE, BG_ROUND };
enum { BGS_AUTO = 0, BGS_COVER, BGS_CONTAIN, BGS_LEN };
enum { CUR_AUTO = 0, CUR_DEFAULT, CUR_POINTER, CUR_TEXT, CUR_MOVE, CUR_WAIT, CUR_HELP, CUR_NOT_ALLOWED,
       CUR_CROSSHAIR, CUR_GRAB, CUR_NONE };
#define DECO_UNDERLINE 1
#define DECO_OVERLINE 2
#define DECO_LINE_THROUGH 4

struct FontRef;

typedef struct CInh {
    WebColor color;
    const FontList *family;
    float font_size;                /* px */
    uint16_t font_weight;
    uint8_t font_style, small_caps;
    uint8_t lh_kind;
    float lh;                       /* LH_NUMBER: the factor; LH_PX: pixels */
    uint8_t text_align, text_transform, white_space, word_break, overflow_wrap, direction;
    uint8_t list_type, list_pos, visibility, cursor, pointer_events;
    uint8_t border_collapse, caption_side, empty_cells, tab_size;
    CLen text_indent;
    float letter_spacing, word_spacing;
    float bs_h, bs_v;               /* border-spacing */
    const char *list_image;         /* resolved URL or NULL */
    const char *list_string;        /* list-style-type: "<string>" */
    const Quotes *quotes;
    struct FontRef *font;           /* cache: the font for family/size/weight/style (fonts.c) */
} CInh;

typedef struct CBox {
    uint8_t display, position, float_, clear, box_sizing, overflow_x, overflow_y;
    uint8_t va_kind, flex_dir, flex_wrap, table_layout, object_fit, auto_flow, text_overflow;
    uint8_t justify_content, align_items, align_self, align_content, justify_items, justify_self;
    bool z_auto;
    int32_t z_index, order;
    CLen top, right, bottom, left;
    CLen width, height, min_w, min_h, max_w, max_h;
    CLen margin[4], padding[4];     /* top, right, bottom, left */
    CLen va_len;
    float flex_grow, flex_shrink;
    CLen flex_basis;
    CLen row_gap, col_gap;
    float aspect;                   /* width / height, 0 = auto */
    const TrackList *tpl_cols, *tpl_rows, *auto_cols, *auto_rows;
    const GridAreas *areas;
    GridLine gc_start, gc_end, gr_start, gr_end;
    const CssContent *content;
    const CounterList *c_reset, *c_inc, *c_set;
} CBox;

typedef struct CVis {
    WebColor bg_color;
    const char *bg_image;           /* resolved URL, or NULL; gradients are approximated by their colours */
    WebColor bg_gradient[2];        /* a linear-gradient: first and last stop (both 0 when none) */
    bool bg_gradient_h;             /* horizontal gradient */
    CLen bg_pos_x, bg_pos_y, bg_w, bg_h;
    uint8_t bg_size, bg_repeat, bg_attach, bg_clip, bg_origin;
    uint8_t border_style[4], outline_style, deco_line, deco_style;
    float border_w[4];              /* computed: 0 when the style is none/hidden */
    WebColor border_color[4];
    CLen radius[4];                 /* top-left, top-right, bottom-right, bottom-left (circular) */
    float outline_w, outline_offset;
    WebColor outline_color, deco_color;
    float opacity;
    const Shadow *shadow;
} CVis;

typedef struct CustomProps {        /* --name: value text, inherited, copy on write */
    WebAtom *names;
    const char **vals;
    int n;
} CustomProps;

typedef struct CStyle {
    CInh *inh;
    CBox *box;
    CVis *vis;
    const CustomProps *vars;
    uint8_t display;                /* = box->display */
    uint8_t pseudo;                 /* PSE_* for pseudo-element styles */
    bool has_vw;                    /* depends on the viewport size */
} CStyle;

/* style.c */
typedef struct CssEngine CssEngine;
void style_update(WebDoc *d);                         /* recompute what is dirty (sheets, index, element styles) */
void style_invalidate_all(WebDoc *d);
void style_sheets_changed(WebDoc *d);
const CStyle *style_of(const WebNode *el);           /* NULL: not rendered (display: none or not computed yet) */
const CStyle *style_initial(WebDoc *d);
float clen_resolve(const CLen *l, float basis, bool *is_auto);
char *style_serialize(WebDoc *d, const CStyle *s, int prop);

/* fonts.c */
typedef struct FontRef {
    WebFontHandle h;
    WebFontMetrics m;
    float size;
    uint16_t weight;
    uint8_t style;
    const FontList *family;
    struct FontRef *next;
    WebUnit space_w;
} FontRef;
FontRef *font_get(WebDoc *d, const CInh *inh);
WebUnit font_measure(WebDoc *d, const FontRef *f, const char *s, size_t n);

#endif
