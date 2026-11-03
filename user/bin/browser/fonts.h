/*
 * The browser's fonts: a WebFontProvider (docs/WEB.md) on libttf, and the drawing of the text runs the engine puts in its
 * display list. The engine measures with the provider and the painter draws with the same code and the same handles, so
 * what was measured is what is painted.
 *
 *   BrFonts *f = brf_new(NULL);                       // loads /ScarletDevilMansion/share/fonts/Gensokyo*.ttf that exist
 *   WebConfig cfg = { .fonts = brf_provider(f), ... };
 *   ...
 *   brf_draw_text(f, &surface, clip, item->font, scale, item->text, item->len, item->x64, item->y64, item->color, 255);
 *
 * Faces: Gensokyo Sans (Regular, and Bold / Italic / BoldItalic when the files exist) answers every family but the
 * monospace ones; Gensokyo Mono answers those when its files exist. What a file lacks is made up: bold by drawing a glyph twice
 * a fraction of a pixel apart (and widening the advance to match), italic by shearing the outline. A character the face has no
 * glyph for is drawn from the toolkit's 8x16 bitmap font scaled to the text size (so an accented letter or a bullet is never a
 * hollow box when the bitmap font has it), and a monospace request with no mono file at all is set entirely in that bitmap font
 * at a whole multiple of 8x16 cells, which gives the crisp fixed-pitch text a terminal has. With no font file at all every
 * request falls back to the bitmap font, scaled to the requested size.
 *
 * Sizes are CSS pixels; on a hi-dpi window the painter passes `scale` and glyphs are rendered at the device size (sharp, not
 * enlarged), with positions in 1/64 device pixel as in the display list.
 */
#ifndef BROWSER_FONTS_H
#define BROWSER_FONTS_H

#include <web.h>
#include "browser.h"

typedef struct BrFonts BrFonts;

#define BRF_DEFAULT_DIR "/ScarletDevilMansion/share/fonts"

/* dir: where the .ttf files are (NULL: $BROWSER_FONT_DIR, else BRF_DEFAULT_DIR). Always returns a usable object (NULL only when
 * memory runs out): files that are missing or broken are simply absent. */
BrFonts *brf_new(const char *dir);
void brf_free(BrFonts *f);
int brf_file_count(const BrFonts *f);                  /* TrueType files loaded */
const WebFontProvider *brf_provider(BrFonts *f);

/* What a handle stands for, for tests and the painter. kind: 0 TrueType, 1 bitmap font. */
typedef struct BrFaceInfo { int kind; float size; bool synth_bold, synth_italic, mono; char file[48]; } BrFaceInfo;
bool brf_face_info(BrFonts *f, WebFontHandle h, BrFaceInfo *out);

/* The advance width of UTF-8 text in the engine's units (1/64 CSS px): what the provider's measure returns. */
WebUnit brf_measure(BrFonts *f, WebFontHandle h, const char *utf8, size_t len);

/*
 * Draws a text run. (x64, y64) is the baseline origin in 1/64 device pixels, `scale` the device pixels per CSS pixel the
 * display list was made for, argb the colour with straight alpha (0xAARRGGBB), group_alpha 0..255 the opacity of the groups
 * around the run. Pixels outside `clip` (and the surface) are not touched.
 */
void brf_draw_text(BrFonts *f, BrSurface *dst, BrRect clip, WebFontHandle h, int scale, const char *utf8, size_t len,
                   int32_t x64, int32_t y64, uint32_t argb, unsigned group_alpha);

/* Where the caret goes: the x offset (device pixels, from the run's origin) of byte offset `at` inside the run. */
int brf_prefix_width(BrFonts *f, WebFontHandle h, int scale, const char *utf8, size_t len, size_t at);

#endif
