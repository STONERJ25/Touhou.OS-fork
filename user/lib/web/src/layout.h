/*
 * Internal declarations of layout: the box tree (boxes.c), block and inline layout (layout.c, inline.c), flexbox
 * (flex.c), grid (grid.c), tables (table.c), and what painting and hit testing read (paint.c, interact.c).
 *
 * Coordinates are layout units (1/64 px). A box's (x, y) is the top-left corner of its border box relative to the
 * border box of its reference box `ref`: the parent box for boxes in block flow, the block container of the inline
 * formatting context for boxes laid out on lines (atomic inlines), the parent box again for positioned boxes (their
 * offsets are converted after the containing block has been laid out). Absolute positions are found by adding up the
 * chain of reference boxes, so a subtree moves by changing one number.
 */
#ifndef WEB_LAYOUT_H
#define WEB_LAYOUT_H

#include "css.h"

typedef enum {
    BK_BLOCK = 0,        /* a block container (block, list item, inline-block's inside, cell, caption, flex/grid item) */
    BK_INLINE,           /* an inline box: its contents are laid out on the lines of the enclosing block container */
    BK_TEXT,             /* a run of text (from a Text node or generated content) */
    BK_REPLACED,         /* an image, an embedded object, a form control */
    BK_BR,               /* a forced line break */
    BK_FLEX,             /* a flex container */
    BK_GRID,             /* a grid container */
    BK_TABLE_WRAPPER,    /* the table wrapper: margins, position, captions + the table */
    BK_TABLE,            /* the table grid box: border, padding, background */
    BK_ROW_GROUP, BK_ROW, BK_CELL, BK_COLGROUP, BK_COL,
    BK_VIEWPORT,         /* the initial containing block */
} BoxKind;

#define BF_INLINE_LEVEL  0x0001   /* participates in an inline formatting context */
#define BF_BFC           0x0002   /* establishes a new block formatting context */
#define BF_FLOAT         0x0004
#define BF_ABS           0x0008   /* absolutely positioned (absolute or fixed) */
#define BF_ANON          0x0010   /* anonymous box (node is the generating element or NULL) */
#define BF_INLINE_CONTENT 0x0020  /* block container whose children are inline-level: it has lines */
#define BF_LAID_OUT      0x0040
#define BF_EMPTY_COLLAPSE 0x0080  /* its top and bottom margins collapse through it */
#define BF_SCROLL        0x0100   /* a scroll container (overflow auto/scroll/hidden): clips, may scroll */
#define BF_MARKER        0x0200   /* a list marker */
#define BF_GENERATED     0x0400   /* ::before / ::after */
#define BF_CONTROL       0x0800   /* a form control (BK_REPLACED) */
#define BF_IMAGE_TEXT    0x1000   /* an image shown as its alt text */
#define BF_STACKING      0x2000   /* creates a stacking context */
#define BF_FIXED         0x4000
#define BF_TEXT_READY    0x8000   /* text: white space processed (text, tlen, tmap are the rendered text) */
#define BF_MARKER_DONE   0x10000  /* list item: its outside marker was put on a line */
#define BF_KEEP_EDGES    0x20000  /* borders set by table layout (collapsing borders): not recomputed from the style */

typedef struct LBox LBox;

enum { FR_TEXT = 1, FR_BOX, FR_ATOMIC, FR_MARKER };
#define FRF_FIRST 1                /* the first fragment of an inline box (draw its left border) */
#define FRF_LAST  2                /* the last fragment (its right border) */
#define FRF_HYPHEN 4               /* a hyphen is drawn after the text */

typedef struct Frag {
    uint8_t kind, flags;
    uint16_t line;
    LBox *box;                     /* the text box, inline box, atomic box or list item (marker) */
    WebUnit x, y, w, h;            /* relative to the block container's border box */
    WebUnit baseline;              /* text: the baseline (same space) */
    uint32_t start, len;           /* text: bytes of box->text */
} Frag;

typedef struct Line {
    WebUnit y, h, baseline;        /* relative to the block container's border box */
    WebUnit left, width;           /* the space the line had (floats excluded) */
    uint32_t first, n;             /* frags */
} Line;

typedef struct Float { WebUnit x, y, w, h; uint8_t side; } Float;   /* margin box, in BFC content coordinates */

struct LBox {
    uint8_t kind;
    uint8_t control;               /* WebControlKind for controls */
    uint32_t flags;
    const CStyle *s;
    WebNode *node;
    LBox *parent, *first, *last, *next, *prev;
    LBox *ref;                     /* (x, y) are relative to ref's border box */
    WebUnit x, y, w, h;            /* border box */
    WebUnit m[4], b[4], p[4];      /* used margins, borders, paddings: top, right, bottom, left */
    WebUnit rel_x, rel_y;          /* position: relative offset (applied when painting) */
    WebUnit baseline;              /* the first-line baseline (or last for inline-blocks), from the border box top */
    bool has_baseline;
    /* block containers with inline content */
    Line *lines;
    Frag *frags;
    uint32_t nlines, nfrags, clines, cfrags;
    /* text */
    const char *text;              /* the text; after white space processing (BF_TEXT_READY) the rendered text */
    uint32_t tlen;
    const uint32_t *tmap;          /* rendered byte -> source byte (NULL: the same offsets) */
    /* replaced */
    WebUnit iw, ih;                /* intrinsic size (0: none) */
    float iratio;                  /* intrinsic ratio w/h (0: none) */
    struct WebRes *img;            /* the image resource */
    const char *label;             /* controls: the text shown (button label, select's option, input value) */
    /* scrolling */
    WebUnit cw, ch;                /* the scrollable overflow size of a scroll container's content */
    WebUnit sx, sy;                /* its scroll offset */
    /* tables */
    int col, row, colspan, rowspan;
    /* intrinsic size cache */
    WebUnit min_c, max_c;
    bool intr_valid;
    /* list markers */
    const char *marker;            /* marker text (decimal, alpha...), NULL for bullets */
    uint8_t marker_kind;           /* LT_* */
    int z;                         /* z-index for stacking (0 for auto) */
    uint32_t order;                /* document order of boxes (paint order ties) */
    WebUnit bm_pos, bm_neg;        /* the bottom margin strut leaving the box (its margin and collapsed children's) */
    WebUnit ax, ay;                /* painting: the border box in page coordinates (scrolling and fixed boxes applied) */
};

typedef struct LayoutState {
    LBox *root;                    /* BK_VIEWPORT */
    WebUnit vw, vh;
    WebUnit doc_w, doc_h;          /* the scrollable size of the page */
    uint32_t nboxes;
    WebColor canvas;               /* the canvas background */
    WebNode *canvas_from;
} LayoutState;

typedef struct PendingAbs { LBox *box, *cb; } PendingAbs;

typedef struct L {
    WebDoc *d;
    LayoutState *st;
    WebArena *a;
    int depth;
    uint32_t order;
    VEC(PendingAbs) abs;           /* positioned boxes waiting for their containing block to finish */
    LBox *marker;                  /* a list item whose outside marker waits for the first line box inside it */
} L;

LBox *abs_containing_block(const LBox *b);
void abs_defer(L *l, LBox *b);                               /* the static position is in b->x, b->y relative to b->ref */
void abs_run(L *l, LBox *cb);                                /* lays out the positioned boxes of cb */

/* boxes.c */
LBox *boxes_build(L *l);
LBox *box_new(L *l, int kind, WebNode *node, const CStyle *s);
void  box_append(LBox *parent, LBox *child);
bool  box_is_block_level(const LBox *b);

/* layout.c */
typedef struct BFC {
    VEC(Float) floats;
    WebArena *a;
} BFC;

typedef struct Space {
    WebUnit cb_w, cb_h;            /* the containing block's content size; cb_h < 0: indefinite */
    BFC *bfc;                      /* the formatting context the box is in (floats) */
    WebUnit bfc_x, bfc_y;          /* the box's border-box origin in BFC coordinates */
    WebUnit force_w, force_h;      /* >= 0: the border-box size is imposed (flex/grid items, cells, stretch) */
    bool shrink;                   /* width auto means shrink-to-fit (floats, inline-blocks, abspos, cells) */
    bool absorb_top;               /* the first child's top margin was collapsed into this box's position */
} Space;

void layout_box(L *l, LBox *b, const Space *sp);           /* sets w/h and lays out the inside (x, y set by the caller) */
void layout_block_inside(L *l, LBox *b, const Space *sp);
void layout_compute_box_edges(L *l, LBox *b, WebUnit cb_w);   /* m, b, p from the style */
WebUnit resolve_len(const CLen *c, WebUnit basis, bool *is_auto);
WebUnit resolve_len_or(const CLen *c, WebUnit basis, WebUnit dflt);
void intrinsic_widths(L *l, LBox *b, WebUnit *min, WebUnit *max);   /* of the content box */
WebUnit shrink_to_fit(L *l, LBox *b, WebUnit avail);         /* border-box width */
void float_avail(const BFC *bfc, WebUnit y, WebUnit h, WebUnit x0, WebUnit x1, WebUnit *left, WebUnit *right);
WebUnit float_clear_y(const BFC *bfc, int side);
void place_float(L *l, LBox *f, const Space *sp, WebUnit y, WebUnit line_left, WebUnit line_right);
void float_layout(L *l, LBox *f, const Space *sp, WebUnit cb_w);                     /* the float's size (shrink-to-fit) */
void float_position(L *l, LBox *f, const Space *sp, WebUnit y, WebUnit x0, WebUnit x1); /* after float_layout */
WebUnit float_next_bottom(const BFC *bfc, WebUnit y, WebUnit h);                     /* INT32_MAX when none */
void box_contribution(L *l, LBox *c, WebUnit *min, WebUnit *max);   /* margin-box min/max-content contributions */
void layout_relative(LBox *b, WebUnit cb_w, WebUnit cb_h);          /* position: relative offsets */
bool box_spec_size(L *l, const LBox *b, const CLen *c, WebUnit basis, bool horiz, WebUnit *out);  /* border-box size */
WebUnit box_clamp(L *l, const LBox *b, WebUnit v, WebUnit basis, bool horiz);       /* min/max-width or -height */
WebUnit box_content_w(const LBox *b);
WebUnit box_content_h(const LBox *b);
void layout_replaced_size(L *l, LBox *b, WebUnit cb_w, WebUnit cb_h);
void layout_positioned(L *l, LBox *cb, LBox *b, WebUnit sx, WebUnit sy);
WebUnit line_height_of(L *l, const CStyle *s);
FontRef *box_font(L *l, const CStyle *s);
void box_abs(const LBox *b, WebUnit *x, WebUnit *y);       /* border box position in page coordinates */

/* inline.c */
void layout_inline(L *l, LBox *b, const Space *sp);         /* lines of a BF_INLINE_CONTENT block container */
void inline_intrinsic(L *l, LBox *b, WebUnit *min, WebUnit *max);

/* flex.c, grid.c, table.c */
void layout_flex(L *l, LBox *b, const Space *sp);
void flex_intrinsic(L *l, LBox *b, WebUnit *min, WebUnit *max);
void layout_grid(L *l, LBox *b, const Space *sp);
void grid_intrinsic(L *l, LBox *b, WebUnit *min, WebUnit *max);
void layout_table_wrapper(L *l, LBox *b, const Space *sp);
void table_intrinsic(L *l, LBox *b, WebUnit *min, WebUnit *max);

/* images (image.c) */
typedef struct WebRes WebRes;
bool image_size(WebDoc *d, WebNode *el, const char *url, int *w, int *h, bool *failed);
WebImageHandle image_handle(WebDoc *d, const char *url);
void image_request(WebDoc *d, WebNode *el, const char *url);

bool image_ready_size(WebDoc *d, const char *url, int *w, int *h);

/* painting (paint.c) and what hit testing reads from it */
typedef struct PaintHit { LBox *box; const Frag *frag; LBox *container; } PaintHit;
typedef struct PaintOut { WebDisplayItem *items; PaintHit *hits; int n; } PaintOut;
/* The items of the rectangle (layout units, page coordinates) in painting order; fixed boxes are moved by (fx, fy). With
 * hit, every box also gives an invisible item, and hits[] says which box and fragment each item paints. */
void paint_collect(WebDoc *d, WebArena *a, WebRect r, WebUnit fx, WebUnit fy, bool hit, PaintOut *out);
const char *control_text(WebDoc *d, const LBox *b, uint16_t *state, uint32_t *len);   /* what a form control shows */
/* interact.c: the selected part of a text box, in rendered bytes */
bool sel_text_range(WebDoc *d, const LBox *t, uint32_t *start, uint32_t *end);
uint32_t text_src_offset(const LBox *t, uint32_t rendered);     /* rendered offset -> DOM text offset */
uint32_t text_rendered_offset(const LBox *t, uint32_t src);     /* and back */

/* the layout of the document (layout.c) */
LayoutState *layout_state(WebDoc *d);
/* CSS pixels to layout units, clamped to +-2^28 units (4.2 million pixels): a page that asks for a billion pixels gets a very large box
 * instead of an overflow, and several such lengths can still be added together in 32 bits. (Found by the browser's hostile page test.) */
static inline WebUnit to_unit(float px) {
    float v = px * 64;
    if (!(v < 268435456.0f)) v = 268435456.0f;                  /* also catches NaN and infinity */
    if (v < -268435456.0f) v = -268435456.0f;
    return (WebUnit)(v + (v >= 0 ? 0.5f : -0.5f));
}

#endif
