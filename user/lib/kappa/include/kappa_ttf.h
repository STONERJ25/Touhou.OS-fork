/*
 * Kappa: proportional, scalable TrueType/OpenType text for KCanvas, on top of libttf (user/lib/ttf).
 *
 * Two things live here, both compiled only when KAPPA_TTF is 1 (the default below since 2026-11-03; -DKAPPA_TTF=0 builds the toolkit with the 8x16 bitmap
 * font alone, no libttf in the programs, exactly as it was before):
 *   - the generic shim (kt_new, kt_text, ...): TrueType text in any font at any size, for programs that want their own face;
 *   - the system UI font (kt_ui_*, at the end): what makes kc_text, kc_text_in, kc_text_width and every widget use Gensokyo Sans. docs/TTF.md has the rules.
 *
 *      KTtf *t = kt_new();
 *      kt_add_file(t, "/usr/share/fonts/body.ttf", 0);          // the first font wins; later ones fill in the characters it lacks
 *      kt_add_file(t, "/usr/share/fonts/cjk.ttc", 0);
 *      ...
 *      int adv = kt_text(canvas, t, 14.0f, x, y, "Hello, \xE4\xB8\x96\xE7\x95\x8C", KC_INK);      // y is the top of the line box
 *
 * Sizes are em sizes in LOGICAL pixels, like every coordinate of a KCanvas. On a canvas with scale N (fairy_scale(): a window on a hi-dpi
 * screen) the glyphs are rendered at N times the size, not enlarged: text on such a window is sharp instead of blocky.
 * Text is anti-aliased and, by default, blended so that dark and light text look equally heavy (ttf_coverage_lut); see kt_set_blend.
 */
#ifndef KAPPA_TTF_H
#define KAPPA_TTF_H

#ifndef KAPPA_TTF
#define KAPPA_TTF 1
#endif

#if KAPPA_TTF

#include <stddef.h>
#include <stdint.h>
#include <kappa.h>
#include <ttf.h>

typedef struct KTtf KTtf;

KTtf *kt_new(void);                                                 /* NULL when memory runs out */
void  kt_free(KTtf *t);

/* Fonts, in order of preference (at most TTF_SET_MAX). Each call returns 0 or a TTF_E_* code. kt_add_font keeps the pointer: the bytes must
 * stay valid until kt_free. kt_add_file reads the whole file and owns the copy. `face` picks a font of a collection (.ttc). */
int   kt_add_font(KTtf *t, const void *data, size_t len, int face);
int   kt_add_file(KTtf *t, const char *path, int face);

/* How coverage becomes colour. gamma and contrast are percentages as in ttf_coverage_lut: 0 and 0 blend coverage as plain alpha, the default
 * (50, 30) makes text look even on light and dark grounds. `lcd`: 0 gray anti-aliasing, 1 or 2 sub-pixel stripes (RGB or BGR order of the display). */
void  kt_set_blend(KTtf *t, int gamma, int contrast);
void  kt_set_lcd(KTtf *t, int lcd);

/* Measurements in logical pixels at an em size: line height (ascent + descent + line gap of the first font), its ascent, and the advance of a
 * UTF-8 string (kerning and ligatures included, rounded up). */
int   kt_line_height(KTtf *t, float size);
int   kt_ascent(KTtf *t, float size);
int   kt_text_width(KTtf *t, float size, const char *utf8);

/* Draws UTF-8 text with the top of its line box at y (so that it can replace kc_text), clipped to the canvas clip. Returns the advance in logical
 * pixels. */
int   kt_text(KCanvas *c, KTtf *t, float size, int x, int y, const char *utf8, uint32_t fg);
/* The same inside a rectangle, vertically centred, cut with "..." when too wide (like kc_text_in). */
void  kt_text_in(KCanvas *c, KTtf *t, float size, KRect r, const char *utf8, uint32_t fg, KAlign align);

/* ---------------------------------------------------------------- the system UI font
 *
 * With KAPPA_TTF on, the toolkit's own text (kc_text, kc_text_n, kc_text_bold, kc_text_shadow, kc_text_in, kc_text_width and every widget built on
 * them) is set in Gensokyo Sans instead of the 8x16 bitmap font. The font is read from KT_UI_FONT_PATH the first time it is needed (every process
 * has its own copy; it is 8 KB); when the file is missing, or the program asked for the bitmap font (kc_text_use_bitmap), nothing changes.
 *
 * Size and line box: the bitmap font's capitals are 9 pixels high in a 16 pixel cell with the baseline 13 pixels below the cell's top. Gensokyo Sans has
 * cap height 0.700 and x-height 0.537 em, so 13 px gives capitals 9.1 and lower case 6.98 pixels high: nearly whole numbers, which is what an unhinted
 * font needs to keep its horizontal edges crisp, and the same optical size as the bitmap text. So the UI text is 13 px with its baseline at y + 13,
 * and every layout that was made for a 16 px line box keeps working.
 *
 * What the font lacks (Latin-1 letters with accents, box drawing, arrows other than left and right ...) is drawn with the bitmap font, one 8 px cell
 * per code point, in the same pass, so a string never shows a missing-glyph box that the bitmap font could have drawn.
 */
#define KT_UI_FONT_PATH "/ScarletDevilMansion/share/fonts/GensokyoSans-Regular.ttf"
#define KT_UI_SIZE      13.0f           /* em size in logical pixels */
#define KT_UI_BASELINE  13              /* pixels from the top of the 16 px line box to the baseline */

/* 1 when the UI font is loaded (the first call loads it, once per process: a missing file is not retried). canvas.c asks this, and whether the
 * program called kc_text_use_bitmap, before it sends text this way. */
int   kt_ui_ready(void);
/* One walk over UTF-8 text (bytes long, (size_t)-1 = up to the NUL): draws it with its pen starting at logical x (fractional allowed) and the top of
 * the line box at y, when c is not NULL, and returns the advance in logical pixels (not rounded). `bold` draws a second, half a pixel shifted copy
 * and adds a pixel of advance. */
float kt_ui_walk(KCanvas *c, float x, int y, const char *s, size_t bytes, uint32_t fg, int bold);

#endif /* KAPPA_TTF */
#endif
