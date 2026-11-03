/*
 * libttf: TrueType and OpenType fonts. Parse a font file, map characters to glyphs, rasterise glyphs with anti-aliasing at any
 * pixel size and sub-pixel position, and measure and lay out text.
 *
 * Specifications followed (no code from other projects): Microsoft's OpenType specification (sfnt directory, head, hhea, maxp,
 * hmtx, loca, glyf, cmap, name, OS/2, post, kern, GPOS pair adjustment, GSUB ligatures), Apple's TrueType Reference Manual
 * (composite glyph flags, the 'true' signature, the 'kern' header variant), Adobe Technical Notes 5176 (CFF) and 5177 (Type 2
 * charstrings). The rasteriser is the signed-area accumulation method: every outline edge adds the exact area it covers to a
 * cell grid, and a running sum along each row turns the accumulated areas into 256 levels of coverage without any
 * super-sampling.
 *
 *      TtfFont font;
 *      if (ttf_open(&font, file_bytes, file_size, 0) == TTF_OK) {          // the bytes must stay valid while font is used
 *          TtfRaster *r = ttf_raster_new();
 *          TtfRenderParams p = { .size_px = 16.0f, .mode = TTF_MODE_GRAY };
 *          TtfBitmap bm;
 *          ttf_render(r, &font, ttf_glyph_for(&font, 'A'), &p, &bm);       // bm.data: bm.width x bm.height coverage bytes
 *          // draw bm at (pen_x + bm.left, baseline_y - bm.top)
 *      }
 *
 * What is supported and what is not (the full list is in docs/TTF.md):
 *   + TrueType outlines (glyf: simple and composite glyphs) and OpenType-CFF (Type 2 charstrings, subroutines, CID-keyed fonts)
 *   + .ttf, .otf and .ttc/.otc collections; cmap formats 0, 4, 6, 12 and 13 (format 14 is skipped cleanly)
 *   + kerning from GPOS pair adjustment (formats 1 and 2) or the 'kern' table (format 0); 'liga' ligatures from GSUB
 *   - no hinting (glyph programs are not run; CFF hints are ignored), no variable fonts (the default instance is used), no
 *     colour or bitmap glyphs, no complex-script shaping and no bidirectional text. WOFF/WOFF2 files must be unpacked first.
 *
 * Everything that parses a font is bounds-checked: a hostile file produces an error code or an empty glyph, never an over-read,
 * an unbounded allocation or an endless loop (tests/host/test_ttf.py fuzzes this under AddressSanitizer). Work and memory per
 * glyph are capped by the TTF_MAX_* limits below.
 *
 * Threads: a TtfFont is read-only after ttf_open and may be shared. A TtfRaster and a TtfCache belong to one thread at a time.
 * The library uses malloc/realloc/free and no floating point library.
 */
#ifndef GAP_TTF_H
#define GAP_TTF_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- return codes (0 or negative) */

#define TTF_OK            0
#define TTF_E_FORMAT    (-1)        /* not an sfnt font (or a collection index that does not exist) */
#define TTF_E_TRUNC     (-2)        /* the data ends before a table does */
#define TTF_E_CORRUPT   (-3)        /* the tables contradict themselves */
#define TTF_E_UNSUPPORTED (-4)      /* valid, but a kind of font this library does not do (CFF2, bitmap-only, ...) */
#define TTF_E_NOMEM     (-5)
#define TTF_E_TOOBIG    (-6)        /* a glyph or bitmap beyond the TTF_MAX_* limits */
#define TTF_E_ARG       (-7)
#define TTF_E_GLYPH     (-8)        /* glyph index out of range, or a glyph description that cannot be decoded */

const char *ttf_strerror(int rc);

/* ---------------------------------------------------------------- limits (a font that needs more is refused or the glyph is blank) */

#define TTF_MAX_POINTS      65536u          /* outline points in one glyph, composites included */
#define TTF_MAX_OPS         (1u << 17)      /* path operations in one glyph */
#define TTF_MAX_COMPONENTS  256u            /* components visited while building one composite glyph */
#define TTF_MAX_DEPTH       8               /* composite nesting */
#define TTF_MAX_DIM         2048            /* bitmap width or height in pixels */
#define TTF_MAX_CELLS       (1u << 21)      /* accumulator cells (width * height, three times the width for LCD output): 8 MB */
#define TTF_MAX_SIZE_PX     1024.0f         /* the largest pixel size ttf_render accepts */
#define TTF_MAX_FACES       65536

/* ---------------------------------------------------------------- the font */

#define TTF_MAX_LOOKUPS     16              /* GPOS 'kern' / GSUB 'liga' lookups remembered per font */
#define TTF_MAX_KERN_TABLES 8
#define TTF_MAX_FDS         16              /* CFF font dictionaries (CID-keyed fonts) */

typedef struct TtfTable { uint32_t off, len; } TtfTable;           /* a table's place in the file (len is clamped to the file) */

typedef struct TtfCffSubrs { uint32_t off, count; uint8_t offsize; } TtfCffSubrs;       /* a subroutine INDEX */

typedef struct TtfFont {
    /* ---- read-only for callers ---- */
    const uint8_t *data;            /* the whole file (not copied: keep it alive) */
    size_t size;
    uint32_t id;                    /* unique per ttf_open in this process; the glyph cache keys on it */
    int face_index;
    int is_cff;                     /* outlines come from the 'CFF ' table, else from 'glyf' */
    int units_per_em;               /* 16..16384 */
    int num_glyphs;
    int bbox[4];                    /* head: xMin yMin xMax yMax, font units */
    /* metrics in font units, as stored (descents are negative as in the file) */
    int hhea_ascent, hhea_descent, hhea_line_gap;
    int typo_ascent, typo_descent, typo_line_gap;
    int win_ascent, win_descent;    /* OS/2 usWinAscent / usWinDescent (both positive) */
    int has_os2, use_typo_metrics;
    int x_height, cap_height;       /* OS/2 version 2+, else 0 */
    int underline_pos, underline_thick;         /* post: top of the stroke relative to the baseline (usually negative), thickness */
    int strike_pos, strike_thick;               /* OS/2 */
    int weight_class, width_class;              /* OS/2, 400 / 5 when absent */
    int is_italic, is_bold, is_fixed_pitch;
    /* ---- private: tables and decoder state ---- */
    TtfTable head, hhea, hmtx, maxp, loca, glyf, cmap, name, os2, post, kern, gpos, gsub, cff;
    int loca_long;
    int num_hmetrics;
    uint32_t cmap_off;              /* offset of the chosen subtable inside the cmap table; 0 = no usable subtable */
    int cmap_format;                /* 0, 4, 6, 12 or 13 (meaningful when cmap_off is not 0) */
    int cmap_symbol;                /* 1: a (3,0) symbol map, characters below 0x100 are also tried at 0xF000 + c; 2: Macintosh Roman, ASCII only */
    uint32_t kern_off[TTF_MAX_KERN_TABLES];     /* format 0 subtables of 'kern' (offset of the subtable body inside the kern table) */
    uint8_t  kern_override[TTF_MAX_KERN_TABLES];
    int nkern;
    uint32_t gpos_kern[TTF_MAX_LOOKUPS];        /* offsets (inside the GPOS table) of the lookups of the 'kern' feature */
    int ngpos_kern;
    uint32_t gsub_liga[TTF_MAX_LOOKUPS];        /* same for GSUB 'liga' */
    int ngsub_liga;
    TtfCffSubrs cff_gsubrs;
    TtfCffSubrs cff_fd[TTF_MAX_FDS];            /* local subroutines: [0] for plain CFF, one per font dictionary when CID-keyed */
    int cff_nfd, cff_cid;
    uint32_t cff_charstrings_off, cff_charstrings_count;
    uint8_t  cff_charstrings_offsize;
    uint32_t cff_fdselect_off;                  /* 0 = none */
    int cff_fdselect_format;
} TtfFont;

/* Number of faces in a file: 1 for a plain font, N for a collection (.ttc), 0 when it is not a font at all. */
int  ttf_face_count(const void *data, size_t len);

/* Opens face `face_index` of the font file in memory. Nothing is allocated and nothing is copied. */
int  ttf_open(TtfFont *f, const void *data, size_t len, int face_index);

/* Strings from the 'name' table, converted to UTF-8 (NUL-terminated when cap > 0). Returns the length in bytes, or -1 when the
 * font has no such string. The English entry is preferred. */
#define TTF_NAME_COPYRIGHT   0
#define TTF_NAME_FAMILY      1
#define TTF_NAME_SUBFAMILY   2          /* "Regular", "Bold Italic", ... */
#define TTF_NAME_UNIQUE      3
#define TTF_NAME_FULL        4
#define TTF_NAME_VERSION     5
#define TTF_NAME_POSTSCRIPT  6
#define TTF_NAME_TYPO_FAMILY 16
#define TTF_NAME_TYPO_SUBFAMILY 17
int  ttf_name(const TtfFont *f, int name_id, char *buf, size_t cap);

/* ---------------------------------------------------------------- metrics */

typedef struct TtfMetrics {
    float ascent;                   /* pixels above the baseline, positive */
    float descent;                  /* pixels below the baseline, positive */
    float line_gap;                 /* extra leading between two lines */
    float line_height;              /* ascent + descent + line_gap */
    float underline_pos, underline_thick;       /* top edge of the underline relative to the baseline (positive up, usually negative) */
    float strike_pos, strike_thick;             /* the same for a strike-through */
    float x_height, cap_height;     /* 0 when the font does not say */
} TtfMetrics;

/* Vertical metrics at a pixel size. The ascent and descent come from the typographic metrics of OS/2 when the font asks for
 * them (USE_TYPO_METRICS), else from hhea, else from the Windows metrics, else from the head box. Values are not rounded. */
void ttf_metrics(const TtfFont *f, float size_px, TtfMetrics *m);

/* ---------------------------------------------------------------- characters and glyphs */

/* Glyph index for a Unicode code point; 0 (the .notdef glyph) when the font has no glyph for it. */
uint32_t ttf_glyph_for(const TtfFont *f, uint32_t cp);

/* Advance width and left side bearing of a glyph in font units; 0 for a glyph index out of range. */
int  ttf_advance_units(const TtfFont *f, uint32_t glyph);
int  ttf_lsb_units(const TtfFont *f, uint32_t glyph);

/* Horizontal kerning between two glyphs in font units (add it to the advance of the left glyph): GPOS pair adjustment if the
 * font has any, else the 'kern' table. 0 when the pair is not kerned. */
int  ttf_kern_units(const TtfFont *f, uint32_t left, uint32_t right);

/* Ligature: if the glyphs[0..n) start with a sequence the font's GSUB 'liga' feature replaces, stores the ligature glyph in
 * *out, and returns how many input glyphs it replaces (at least 2). Returns 0 when there is none. */
int  ttf_ligature(const TtfFont *f, const uint32_t *glyphs, size_t n, uint32_t *out);

/* Pixels per font unit at a size: size_px / units_per_em. */
static inline float ttf_scale(const TtfFont *f, float size_px) { return size_px / (float)f->units_per_em; }

/* ---------------------------------------------------------------- outlines */

#define TTF_OP_MOVE   0                 /* 1 point: starts a contour */
#define TTF_OP_LINE   1                 /* 1 point */
#define TTF_OP_QUAD   2                 /* 2 points: control, end (TrueType curves) */
#define TTF_OP_CUBIC  3                 /* 3 points: control, control, end (CFF curves) */
#define TTF_OP_CLOSE  4                 /* 0 points: back to the start of the contour */

/* A glyph outline in font units, y up. ops[i] says how many (x, y) pairs it takes from pts, in order. Contours are filled
 * with the non-zero winding rule. The arrays belong to the TtfRaster that produced it. */
typedef struct TtfOutline {
    uint8_t *ops;
    float *pts;
    uint32_t nops, npts;                /* npts counts floats (two per point) */
    uint32_t cap_ops, cap_pts;
    float xmin, ymin, xmax, ymax;       /* bounds of all points (control points included); zeros when empty */
} TtfOutline;

typedef struct TtfRaster TtfRaster;     /* scratch memory for outlines and bitmaps, reused from glyph to glyph */
TtfRaster *ttf_raster_new(void);
void       ttf_raster_free(TtfRaster *r);

/* Decodes a glyph's outline. *out points into r and is valid until the next call that uses r. A glyph without contours (a
 * space) gives an empty outline and TTF_OK. */
int  ttf_glyph_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph, const TtfOutline **out);

/* ---------------------------------------------------------------- rasterising */

#define TTF_MODE_GRAY 0                 /* 1 byte per pixel: coverage 0..255 */
#define TTF_MODE_LCD  1                 /* 3 bytes per pixel: coverage of the three stripes of the pixel, left to right (filtered) */

typedef struct TtfRenderParams {
    float size_px;                      /* the em square, in pixels (1..TTF_MAX_SIZE_PX) */
    float sub_x, sub_y;                 /* where the pen is inside its pixel, 0 <= v < 1: the glyph moves right and up by that much */
    float skew;                         /* synthetic oblique: x += skew * y (0.2 is about 11 degrees); 0 = upright */
    int mode;                           /* TTF_MODE_* */
} TtfRenderParams;

typedef struct TtfBitmap {
    int width, height;                  /* in pixels; 0 x 0 for glyphs without ink */
    int left;                           /* the bitmap's left edge, in pixels right of the integer pen position */
    int top;                            /* the bitmap's top edge, in pixels above the baseline */
    int stride;                         /* bytes per row */
    int bpp;                            /* bytes per pixel: 1 (gray) or 3 (LCD) */
    float advance;                      /* the glyph's advance width in pixels, unrounded, kerning not included (set for glyphs without ink too) */
    uint8_t *data;
} TtfBitmap;

/* Renders one glyph. The bitmap lives in r until the next call that uses r (copy it to keep it). With a non-zero `top`/`left`
 * the glyph is drawn at (pen_x + left, baseline_y - top). No hinting is applied: outlines are scaled exactly, so small text looks
 * softer than hinted text; sub-pixel positioning (sub_x) keeps the spacing even. */
int  ttf_render(TtfRaster *r, const TtfFont *f, uint32_t glyph, const TtfRenderParams *p, TtfBitmap *out);

/* ---------------------------------------------------------------- blending and drawing into 0x00RRGGBB pixels */

/* A table that turns coverage into the alpha to blend with. Blending coverage straight into sRGB values (what a plain alpha blend
 * does) makes dark text on a light background heavier, and light text on a dark background thinner, than blending the light itself
 * would. `gamma` (0..100 percent) is how much of that correction to apply: 0 returns plain coverage, 100 makes the sRGB blend
 * reproduce a linear-light blend of the text colour over the background colour (both 0x00RRGGBB, judged by their luma; the more
 * alike they are the less the correction is trusted). The best value is a matter of taste on a given display: full correction
 * makes dark-on-light text look light, so a partial value is the usual choice. `contrast` (0..100) then steepens the curve
 * (coverage below one half gets smaller, above it larger), which helps unhinted small text. lut[0] is always 0, lut[255] always 255. */
void ttf_coverage_lut(uint8_t lut[256], uint32_t fg, uint32_t bg, int gamma, int contrast);

typedef struct TtfSurface {
    uint32_t *px;                       /* 0x00RRGGBB pixels */
    int width, height;
    int stride;                         /* in pixels */
} TtfSurface;

typedef struct TtfDrawStyle {
    uint32_t color;                     /* 0x00RRGGBB */
    int mode;                           /* TTF_MODE_GRAY or TTF_MODE_LCD */
    int lcd_bgr;                        /* LCD stripes run blue-green-red from the left */
    const uint8_t *lut;                 /* from ttf_coverage_lut, or NULL for plain coverage */
    unsigned layout_flags;              /* TTF_LAYOUT_* */
    int clip_x0, clip_y0, clip_x1, clip_y1;     /* pixels outside are not touched; all zero means the whole surface */
} TtfDrawStyle;

/* Blends a rendered bitmap with its pen position at (x, y) = (integer pen x, baseline y). Clipped to the surface. */
void ttf_blit(TtfSurface *dst, int x, int y, const TtfBitmap *bm, const TtfDrawStyle *style);

/* ---------------------------------------------------------------- UTF-8 */

/* Decodes one code point at *p (p < end) and advances *p. Malformed, truncated, overlong and surrogate sequences give U+FFFD
 * and consume at least one byte, so a loop always ends. */
uint32_t ttf_utf8_next(const char **p, const char *end);
int      ttf_utf8_encode(uint32_t cp, char out[4]);        /* returns 1..4; code points above 10FFFF and surrogates become U+FFFD */

/* ---------------------------------------------------------------- font sets and text layout */

#define TTF_SET_MAX 8

/* An ordered list of fonts: each code point is shown with the first font that has a glyph for it. */
typedef struct TtfFontSet {
    const TtfFont *fonts[TTF_SET_MAX];
    int count;
} TtfFontSet;

void ttf_set_init(TtfFontSet *s);
int  ttf_set_add(TtfFontSet *s, const TtfFont *f);                    /* 0, or TTF_E_TOOBIG when full */
/* The font (index into the set, or -1 when none has the character) and glyph that show cp. With -1, *glyph is the .notdef glyph of
 * the first font. */
int  ttf_set_lookup(const TtfFontSet *s, uint32_t cp, uint32_t *glyph);

#define TTF_LAYOUT_KERN   0x1u          /* apply kerning */
#define TTF_LAYOUT_LIGA   0x2u          /* apply the font's 'liga' ligatures */
#define TTF_LAYOUT_ROUND  0x4u          /* round every advance to whole pixels (steadier spacing for unhinted small text) */
#define TTF_LAYOUT_DEFAULT (TTF_LAYOUT_KERN | TTF_LAYOUT_LIGA)

#define TTF_GP_NOTDEF     0x1u          /* no font in the set has this character: the glyph is a .notdef box */
#define TTF_GP_LIGATURE   0x2u          /* stands for more than one code point */

typedef struct TtfGlyphPos {
    uint32_t glyph;                     /* glyph index in fonts[font] */
    uint16_t font;                      /* index into the TtfFontSet */
    uint16_t flags;                     /* TTF_GP_* */
    float x;                            /* pen position of the glyph's origin, pixels from the start of the string */
    float advance;                      /* pixels, kerning included */
    uint32_t cluster;                   /* byte offset in the input of the first code point this glyph stands for */
} TtfGlyphPos;

/* Lays out UTF-8 text on one line. Fills out[0..min(count, cap)) and returns the number of glyphs the text needs (like snprintf,
 * so a caller can size a buffer by calling once with cap = 0); out may be NULL. *width receives the total advance in pixels.
 * Control characters, zero-width and variation-selector code points are skipped. Kerning and ligatures act inside runs of one
 * font. Simple on purpose: no bidirectional reordering and no complex-script shaping. */
size_t ttf_layout(const TtfFontSet *set, float size_px, const char *utf8, size_t len, unsigned flags,
                  TtfGlyphPos *out, size_t cap, float *width);

/* Width in pixels of UTF-8 text laid out as above. */
float ttf_text_width(const TtfFontSet *set, float size_px, const char *utf8, size_t len, unsigned flags);

/* True for the code points ttf_layout skips (C0 controls, soft hyphen, zero-width and bidi marks, variation selectors, BOM). */
int  ttf_is_ignorable(uint32_t cp);

/* ---------------------------------------------------------------- glyph cache */

#define TTF_SUBPIXEL_STEPS 4            /* horizontal sub-pixel positions that are cached separately: 0, 1/4, 1/2, 3/4 */

typedef struct TtfCacheStats {
    uint64_t hits, misses, evictions;
    size_t bytes, entries;              /* held now */
    size_t peak_bytes;
} TtfCacheStats;

typedef struct TtfCache TtfCache;

/* A bounded cache of rendered bitmaps keyed by (font, glyph, size, sub-pixel step, mode). When it holds more than max_bytes or
 * max_entries the least recently used bitmaps are dropped. (A single bitmap larger than max_bytes is still returned, and dropped
 * at the next call.) */
TtfCache *ttf_cache_new(size_t max_bytes, size_t max_entries);
void      ttf_cache_free(TtfCache *c);
void      ttf_cache_clear(TtfCache *c);
void      ttf_cache_stats(const TtfCache *c, TtfCacheStats *st);

/* Returns the bitmap of a glyph for the sub-pixel step 0..TTF_SUBPIXEL_STEPS-1 (the glyph shifted right by step/STEPS pixel),
 * rendering it with `r` on a miss. NULL only when memory runs out. A glyph that cannot be decoded comes back as an empty bitmap
 * (and is remembered as such). The pointer is valid until the next call on this cache. */
const TtfBitmap *ttf_cache_get(TtfCache *c, TtfRaster *r, const TtfFont *f, uint32_t glyph, float size_px, int subpixel, int mode);

/* Lays out and draws UTF-8 text with its baseline at y and its start at x (pixels, fractional x allowed), using the cache. Returns
 * the advance in pixels. */
float ttf_draw_text(TtfCache *c, TtfRaster *r, const TtfFontSet *set, float size_px, const char *utf8, size_t len,
                    float x, int baseline_y, const TtfDrawStyle *style, TtfSurface *dst);

#endif
