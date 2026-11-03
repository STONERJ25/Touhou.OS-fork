/*
 * The painter: turns the engine's display list (WebDisplayList, user/lib/web/include/web.h) into pixels of a BrSurface.
 *
 *   BrPainter *p = brp_new(fonts);
 *   const WebDisplayList *dl = web_doc_paint(doc, scroll_x, scroll_y, view_w, view_h);
 *   BrPaintOpts o = { .scale = 1, .fill_background = true };
 *   brp_render(p, &surface, dl, &o);
 *
 * What it draws: rectangles (rounded corners with anti-aliased edges), borders (solid, dotted, dashed, double, groove, ridge,
 * inset and outset, with the diagonal joins between sides of different colours, rounded corners when the radii are set),
 * text (fonts.h), images (copied, enlarged bilinearly, or shrunk with an area average kept in a small cache), clips (rectangles;
 * a rounded clip clips to its rectangle), opacity groups (every item inside is drawn with the group's alpha: exact unless
 * children overlap), scroll bars over scroll containers, ellipses, shadows (stacked translucent layers), and the form controls
 * the engine leaves to the UI (text fields and areas, buttons, checkboxes, radio buttons, selects, list boxes, sliders, colour
 * swatches, file pickers, progress bars and meters), drawn from their item data in the colours of the desktop.
 *
 * Written from the display list's own documentation; the reference rasteriser in tests/host/webtool.c is much cruder.
 * Everything is integer or single precision arithmetic; no maths library.
 */
#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H

#include <web.h>
#include "browser.h"
#include "fonts.h"

typedef struct BrPainter BrPainter;

typedef struct BrPaintOpts {
    BrRect clip;                 /* the part of the surface that may be touched; all zero: the whole surface */
    int ox, oy;                  /* where the display list's origin lies on the surface (device pixels) */
    int scale;                   /* device pixels per CSS pixel the list was made for (WebConfig.scale); 0 = 1 */
    bool fill_background;        /* first fill the clip with the list's canvas colour */
    /* the form control that has the keyboard focus: its caret and selection are drawn (byte offsets into its value) */
    WebNode *focus;
    size_t caret, anchor;
    bool caret_on;               /* the blink phase */
    int focus_scroll;            /* how far the focused control's text is scrolled (device px): horizontally for a field, vertically for a text area */
} BrPaintOpts;

BrPainter *brp_new(BrFonts *fonts);
void brp_free(BrPainter *p);
void brp_reset_images(BrPainter *p);        /* forget the scaled copies: call when the document that owned the pictures goes away */
void brp_render(BrPainter *p, BrSurface *dst, const WebDisplayList *dl, const BrPaintOpts *opts);

/* Where the text of a form control (a CONTROL item of the display list) sits, for placing a caret from a click and keeping it
 * visible. (ox, oy) is where the list's origin lies in the coordinates the result should have. */
typedef struct BrControlGeom {
    BrRect content;              /* the content box: inside border and padding */
    int text_x;                  /* where the first character starts (device px, before any scroll) */
    int baseline;                /* the baseline of the first line */
    int line_h;                  /* the line height */
    WebFontHandle font;
} BrControlGeom;
bool brp_control_geom(BrPainter *p, const WebDisplayItem *item, int ox, int oy, int scale, BrControlGeom *out);

/* Word wrapping of a text area's value for a width in device pixels: fills out[] with the byte range of each visual line (the
 * newline is not in it) and returns how many lines the text has (at least 1; out may be NULL or short, like snprintf). */
typedef struct BrLine { uint32_t start, end; } BrLine;
size_t brp_wrap(BrPainter *p, WebFontHandle font, int scale, int width, const char *text, size_t len, BrLine *out, size_t max);

/* Colours the page code shares with the painter. */
#define BR_SELECTION_BG 0x803390FFu

#endif
