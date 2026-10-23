/*
 * Bunbunmaru News: the typesetting engine. Articles and other items are poured into balanced newspaper columns:
 * text is word wrapped into lines, lines are grouped so a headline never ends a column alone, the columns are balanced
 * (the shortest tallest column wins), and the result is a list of drawing operations the window replays.
 */
#ifndef BUN_LAYOUT_H
#define BUN_LAYOUT_H

#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Article {
    const char *kicker;                 /* small label above the headline: "GENSOKYO", may be NULL */
    const char *headline;
    const char *byline;                 /* may be NULL */
    const char *body;                   /* paragraphs separated by '\n' */
} Article;

typedef enum { OP_TEXT, OP_GLYPH, OP_HRULE, OP_VRULE, OP_BOX, OP_CUSTOM, OP_ICON } OpKind;

typedef void (*CustomDraw)(KUI *ui, KRect r, void *data);

typedef struct DrawOp {
    uint8_t kind, scale;
    bool bold, justify, dashed, center;
    int x, y, w, h;                     /* page coordinates (y counts from the top of the page) */
    uint32_t color, fill;
    const char *text;
    int len;
    uint32_t cp;                        /* OP_GLYPH: the code point; OP_ICON: the icon */
    CustomDraw draw;
    void *data;
} DrawOp;

typedef struct Layout {
    DrawOp *ops;
    int n, cap;
    int height;                         /* total height of the page content */
    int width;
} Layout;

void    lay_reset(Layout *L, int width);
void    lay_free(Layout *L);
DrawOp *lay_op(Layout *L, OpKind kind);

/* a single line of text at an exact place (mastheads, running heads) */
void    lay_text(Layout *L, int x, int y, const char *text, int scale, bool bold, uint32_t color);
void    lay_text_centered(Layout *L, int x, int y, int w, const char *text, int scale, bool bold, uint32_t color);
/* a headline wrapped over the full width; returns the y below it */
int     lay_banner(Layout *L, int x, int y, int w, const char *text, int scale, uint32_t color, bool center);
void    lay_icon(Layout *L, int x, int y, int size, int icon);
void    lay_hrule(Layout *L, int x, int y, int w, int thickness, uint32_t color);
void    lay_double_rule(Layout *L, int x, int y, int w, uint32_t color);

/* items poured into columns */
typedef struct Item {
    enum { IT_ARTICLE, IT_PARA, IT_CUSTOM, IT_GAP, IT_RULE } type;
    const Article *art;
    bool dropcap;
    int head_scale;                     /* 1 or 2 */
    const char *text;                   /* IT_PARA */
    uint32_t color;
    bool bold, justify, boxed, center;
    int h;                              /* IT_CUSTOM, IT_GAP: height in pixels */
    CustomDraw draw;
    void *data;
} Item;

typedef struct Flow { Item *it; int n, cap; } Flow;

void flow_free(Flow *f);
void flow_article(Flow *f, const Article *a, int head_scale, bool dropcap);
void flow_para(Flow *f, const char *text, uint32_t color, bool bold, bool justify);
void flow_boxed(Flow *f, const char *text, uint32_t color);                 /* kept together in a dashed box (classified ads) */
void flow_custom(Flow *f, int height, CustomDraw draw, void *data);
void flow_gap(Flow *f, int h);
void flow_rule(Flow *f);

/* Typesets the flow into ncols columns inside [x, x + w) starting at y. Returns the y below the tallest column. */
int     lay_flow(Layout *L, int x, int y, int w, int ncols, int gutter, const Flow *f);

/* Draws one operation at (ox, oy) = where the page's top-left lies on the canvas. */
void    lay_draw(KUI *ui, const Layout *L, int ox, int oy, KRect view);

#define BODY_LH 18

#endif
