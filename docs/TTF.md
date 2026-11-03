# TrueType and OpenType fonts (libttf)

`user/lib/ttf` loads TrueType (`glyf`) and OpenType-CFF fonts, maps characters to glyphs, rasterises glyphs with anti-aliasing at any pixel size and
sub-pixel position, measures and lays out text, and caches rendered glyphs. It is the engine for proportional, scalable text: the desktop's interface text (since
2026-11-03: Gensokyo Sans through the toolkit, see "Toolkit integration" and "The desktop in Gensokyo Sans"), the browser, later Japanese text. The 8x16 bitmap font
(`shared/font8x16.c`) remains for the terminal, the text areas, the pixel-art programs and characters the interface font lacks.

Everything is written from the specifications (Microsoft's OpenType specification, Apple's TrueType Reference Manual, Adobe Technical Notes 5176/5177);
no code from other projects. The only fonts in the repository are the owner's own, `assets/fonts` (Gensokyo Sans and seven display faces; license undecided); the
tests generate their fonts (`tests/host/ttfmake.py`), read those eight, and use the owner's Windows fonts only in place, in tests that skip themselves when the files
are absent (docs/ASSETS.md).

| Where | What |
|---|---|
| `user/lib/ttf/include/ttf.h` | the whole public API, with the details in its comments |
| `user/lib/ttf/src/` | `font.c` (sfnt, names, metrics), `cmap.c`, `glyf.c`, `cff.c`, `otl.c` (kern, GPOS, GSUB), `outline.c`, `raster.c`, `text.c`, `cache.c`, `blend.c`, `ttfint.h` |
| `user/lib/kappa/include/kappa_ttf.h`, `src/ttftext.c` | text for `KCanvas`: the generic shim and the system UI font behind `kc_text` (see "Toolkit integration") |
| `tests/host/test_ttf.py`, `ttftool.c`, `ttfmake.py` | the tests, a command-line driver, the test-font generator |

`tools/build.py` picks the library up by itself (`build/lib/libttf.a`, linked into every program; an archive, so only programs that call it carry any of
it). About 46 KB of code for x86-64 at `-O2`. It needs `malloc`, `realloc`, `free`, `memcpy`, `memset` from the libc and **no maths library**.

## Quick start

```c
#include <ttf.h>

TtfFont font;
if (ttf_open(&font, bytes, size, 0) != TTF_OK) return;            // bytes: the file's contents; they must outlive `font` (nothing is copied)

TtfRaster *r = ttf_raster_new();                                  // scratch memory, reused from glyph to glyph
TtfRenderParams p = { .size_px = 16.0f, .mode = TTF_MODE_GRAY };
TtfBitmap bm;
ttf_render(r, &font, ttf_glyph_for(&font, 'A'), &p, &bm);         // bm.data: bm.width x bm.height coverage bytes, valid until the next call on r
// blend it at (pen_x + bm.left, baseline_y - bm.top)
```

Text: put one or more opened fonts in a `TtfFontSet` (first font that has the character wins), then

```c
TtfFontSet set; ttf_set_init(&set); ttf_set_add(&set, &body); ttf_set_add(&set, &cjk);
float w = ttf_text_width(&set, 14.0f, "Hello, 世界", len, TTF_LAYOUT_DEFAULT);          // pixels
TtfCache *cache = ttf_cache_new(1 << 20, 2048);                                          // 1 MiB, 2048 glyphs
TtfDrawStyle st = { .color = 0x1A1A24, .mode = TTF_MODE_GRAY, .layout_flags = TTF_LAYOUT_DEFAULT };
ttf_draw_text(cache, r, &set, 14.0f, text, len, x, baseline_y, &st, &surface);           // XRGB8888 pixels; fractional x is honoured
```

## What is supported

| | Supported | Not supported |
|---|---|---|
| Containers | `.ttf` (`0x00010000`, `true`), `.otf` (`OTTO`), `.ttc`/`.otc` collections (any face) | WOFF, WOFF2 (unpack first), bitmap-only fonts, `CFF2` |
| Tables | `head hhea maxp hmtx loca glyf cmap name OS/2 post kern GPOS GSUB` and `CFF ` (`post` only for the underline position and thickness and the fixed-pitch flag) | `post` glyph names, `vhea/vmtx` (vertical text), `COLR/CPAL`, `sbix`, `CBDT`, `SVG ` (colour glyphs: the plain outline is drawn), `fvar/gvar/HVAR` (variable fonts: the default instance), `MATH`, `JSTF` |
| `glyf` | simple and composite glyphs: offsets, scales, 2x2 matrices, point matching, scaled/unscaled offset flags, nesting to 8 levels | the instructions (no hinting: they are skipped) |
| CFF | Type 2 charstrings: every path operator and its odd-operand forms, `flex` family, hints and masks (skipped), subroutines (all three biases), arithmetic and storage operators, CID-keyed fonts (FDSelect formats 0 and 3) | the deprecated four-argument `endchar` (seac), `random` is a constant |
| `cmap` | formats 0, 4, 6, 12, 13; Windows symbol maps (`U+F0xx`); Macintosh Roman restricted to ASCII; the best subtable is chosen by platform/encoding | format 14 (skipped cleanly, variation selectors are ignored in layout), formats 2 and 10 |
| Kerning | GPOS `kern` feature, PairPos formats 1 and 2, through Extension lookups, all value formats (XAdvance is used); `kern` table format 0 with the Microsoft and Apple headers, several subtables, override | class kerning in `kern` format 2, contextual and cursive positioning, mark attachment |
| Ligatures | GSUB `liga`, LigatureSubst through Extension lookups | other features (`calt`, `ccmp`, `locl`, `vert`, ...) and every other lookup type |
| Rendering | anti-aliased 8-bit coverage, any size 1 to 1024 px, quarter-pixel positions (any fraction in the rasteriser), oblique shear, LCD stripes | hinting, emboldening, outlines/strokes |
| Layout | one line; kerning, ligatures, rounding, per-character fallback over a font set, UTF-8 | bidirectional text, Arabic/Indic/Thai shaping, vertical text, line breaking, justification |

When a font has both a GPOS `kern` feature and a `kern` table, GPOS wins (as in HarfBuzz). The script for GPOS/GSUB is picked once: `latn`, else `DFLT`,
else the first one; a script without that feature falls back to every feature of that name.

## The API in one page

All of it is in `ttf.h`; error codes are `TTF_E_*` (0 is `TTF_OK`), `ttf_strerror` names them.

- **Open:** `ttf_face_count`, `ttf_open(&font, data, len, face)`. `TtfFont` is a plain struct (680 bytes) the caller owns; nothing is allocated. Read-only
  fields: `units_per_em`, `num_glyphs`, `is_cff`, the metrics in font units, the table records.
- **Names and metrics:** `ttf_name(font, TTF_NAME_FAMILY, buf, cap)` gives UTF-8 (English entry preferred); `ttf_metrics(font, size, &m)` gives ascent, descent,
  line gap, line height, underline and strike-through position and thickness, x-height and cap height in pixels. Ascent and descent come from OS/2's typographic
  values when the font sets `USE_TYPO_METRICS`, else from `hhea`, else from the Windows values, else from the head box.
- **Glyphs:** `ttf_glyph_for(font, codepoint)` (0 is the missing-glyph box), `ttf_advance_units`, `ttf_lsb_units`, `ttf_kern_units(left, right)`, `ttf_ligature`.
- **Outlines:** `ttf_glyph_outline(raster, font, glyph, &outline)`: move/line/quadratic/cubic/close operations in font units, y up. Single-point contours (hinting
  anchors, common in Microsoft's fonts) are left out, and so is a move that nothing follows.
- **Rendering:** `ttf_render(raster, font, glyph, &params, &bitmap)` (below). `TtfBitmap` has `left` and `top` relative to the integer pen position and the baseline, and the glyph's
  `advance` in pixels (also for glyphs without ink, such as a space).
- **Blending:** `ttf_coverage_lut`, `ttf_blit`, `ttf_draw_text` (below).
- **Text:** `ttf_utf8_next/encode`, `TtfFontSet`, `ttf_layout` (glyph runs with positions, advances, clusters, flags), `ttf_text_width`.
- **Cache:** `ttf_cache_new/free/clear/stats/get`.

A `TtfFont` may be shared between threads (it is read-only after `ttf_open`). A `TtfRaster` and a `TtfCache` belong to one thread at a time.

## How the rasteriser works, and how exact it is

The signed-area accumulation method, derived from scratch. The bitmap is a grid of cells; every edge of the (flattened) outline is cut at the cell
boundaries it crosses, and each piece adds to its cell the area that lies to its right inside that cell's row: `d * (1 - m)` in the cell and `d * m` in the next
(`d` the piece's signed height, `m` the fraction of the cell width at its midpoint). A running sum along the row then *is* the covered area of every pixel, to
float precision; there is no sampling and no super-sampling. Curves are flattened first, with the chord error kept under 0.02 px (the segment count follows
from the curve's second difference). Output is `min(|sum|, 1)` as 0..255.

- **Polygons are exact:** squares, triangles, thin slivers, at any fraction of a pixel, match exact rational polygon clipping to within one level (the tests).
- **Curves** deviate from the true curve by at most the flattening tolerance; against a 60-segment reference the worst pixel of a quadratic circle is within 6
  levels and the mean error is 0.6 level.
- **Winding:** non-zero. Holes (opposite direction) and nested same-direction contours (solid) are right. Where two contours of the *same* direction overlap, the sum
  counts the overlap twice and is clamped to 1: exact everywhere except the (one or two) pixels where the *edges* of the two overlapping contours meet. This is the
  usual property of the method; it matters little, since a pixel is only ever too dark there, never wrong in shape.
- **Against real fonts** (`test_ink_matches_an_unhinted_reference`): the total ink of 35 glyphs of seven of the owner's fonts and 25 glyphs of five freely licensed ones (DejaVu, STIX,
  Computer Modern), at 100 px, is within 0.52% of a high-resolution (8 x 8 samples per pixel) non-zero fill of fontTools' own outline (the reference's sampling error is systematic on
  vertical stems, so it is the limit of the comparison, not of the rasteriser), and within 1.4% of the ink on a per-pixel basis. MS Gothic's densely overlapping CJK strokes come
  out within 0.14% (hinted FreeType is 3.5% heavier there).
- **No hinting.** Outlines are scaled exactly. Small text therefore looks softer than text hinted onto the pixel grid; FreeType-with-hinting and libttf differ by
  grid-fitting (Arial `%` overlaps 0.74 at 48 px, 0.96 at 200 px, 0.975 at 400 px; the ink differs by 0.014% at 400 px). Sub-pixel positioning (`sub_x`, the cache's
  quarter-pixel steps) and the coverage curve below are the means to keep unhinted text even and readable.
- **LCD:** `TTF_MODE_LCD` renders at three times the horizontal resolution, filters the stripes with a triangle `[28 57 86 57 28] / 256` and gives three bytes per
  pixel (the stripes left to right; the bitmap has one extra pixel on each side for the filter). `TtfDrawStyle.lcd_bgr` says which colour is leftmost.
- **Oblique:** `skew` shears the outline (`x += skew * y`), for faces that have no italic.

Limits (compile-time constants in `ttf.h`): 65536 points and 131072 path operations per glyph, 256 components and 8 levels of composites, bitmaps up to 2048 x 2048
and 2 M accumulator cells (8 MB), sizes up to 1024 px, CFF call depth 10, 96 operands, 1 M operations per glyph. A glyph over a limit is refused with
`TTF_E_TOOBIG`, never truncated.

## Text layout

`ttf_layout` makes one pass over the code points: each is resolved to a (font, glyph) through the set; glyphs of one font gather in a short window so that a ligature
can look ahead; every glyph that leaves the window is held back one step so that the kerning against its successor can be added to its advance. Nothing is allocated;
the caller's array may be shorter than the result and the return value says how long it would have to be.

- Kerning and ligatures act inside runs of one font only.
- A character no font has becomes the first font's `.notdef` glyph, flagged `TTF_GP_NOTDEF`; space separators (no-break space, em/en/ideographic space, ...) that no
  font has use the plain space.
- Skipped, with no advance: C0 and C1 controls, soft hyphen, zero-width space and joiners, direction marks, word joiner and invisible operators, variation selectors, BOM.
- `TTF_LAYOUT_ROUND` rounds every advance (kerning included) to whole pixels, which keeps unhinted small text evenly spaced.
- Malformed UTF-8 gives U+FFFD per maximal invalid subpart (identical to Python's decoder on random byte strings, in the tests).
- Each `TtfGlyphPos` has `x`, `advance`, the glyph, the font index, flags (`NOTDEF`, `LIGATURE`) and `cluster`, the byte offset of the first code point it stands for:
  enough for caret placement and hit testing.

## Blending

Blending coverage straight into sRGB values makes dark-on-light text heavier and light-on-dark thinner than blending the light would. `ttf_coverage_lut(lut, fg, bg,
gamma, contrast)` returns the table that makes the cheap blend `bg + a * (fg - bg)` give the linear-light result (judged by luma; the more alike the colours the less it
is trusted). `gamma` and `contrast` are percentages: 0/0 is plain coverage; 100 is the physically exact curve, which makes dark text look light, so partial values are the
practical choice (the sample pictures use 50 and 30). `contrast` steepens the curve (smoothstep), which helps unhinted text. `ttf_blit` and `ttf_draw_text` take the table
in `TtfDrawStyle.lut`. Table values equal the formulas within 2.5 levels (`test_lut_matches_linear_light_blend`).

## The glyph cache

A hash table keyed by (font id, glyph, size in 1/64 px, sub-pixel step 0..3, gray/LCD) with an LRU list; memory is bounded twice (sum of entry sizes, number of entries) and
the least recently used bitmaps are dropped, never the one just returned. An entry costs 80 bytes plus its pixels. A bitmap larger than the whole budget is still handed
out (and goes at the next call); empty and undecodable glyphs are cached as empty bitmaps, so a broken glyph is tried once. The returned pointer is valid until the next call
on that cache. `ttf_draw_text` splits a fractional pen position into an integer part and a quarter-pixel step (0.13 px rounds to 1/4, 0.9 px carries into the next pixel).

## Memory and speed

- `TtfFont`: 680 bytes, no allocation. `TtfRaster`: 144 bytes when new, then grows to what the largest glyph needs: about 8 KB after a 16 px glyph, 13 KB at 48 px,
  167 KB at 200 px (the accumulator is 4 bytes per cell, grown by doubling). Cache entries: 80 bytes plus pixels (a 12 x 12 gray glyph is 224 bytes).
- Speed on this PC (WSL, one core, `-O2`, ASCII glyphs at quarter-pixel positions, no cache). The PC is shared with other jobs, so the figures are the range over several runs: 94
  synthetic glyphs of 12 to 20 points: 240,000 to 715,000 glyphs/s at 16 px, 135,000 to 280,000 at 48 px gray; LCD 150,000 to 335,000 and 46,000 to 81,000. Real fonts (Arial, Segoe UI,
  Yu Gothic, a CFF font): 320,000 to 600,000 glyphs/s at 16 px and 160,000 to 270,000 at 48 px. (`build/ttf-samples/speed.txt` after a test run.) These are rendering speeds; with the
  cache a repeated glyph is a hash lookup and a blit.
- Opening a font scans the table directory and the GPOS/GSUB feature lists and allocates nothing; the survey that loaded 22 faces of the owner's PC and rendered 6000 glyphs of each (under the
  sanitizers) took about a second per face.

## Hostile fonts

Every read goes through a bounds-checked span; counts are checked against the table's length before any loop; the work per glyph is capped as listed above.
Evidence (tests/host/test_ttf.py, `Robustness`, AddressSanitizer + UBSan):

- Mutation fuzzing (random bit flips, field extremes, truncation, block copies, aimed at one table of the directory half the time) of a TrueType font that has every table
  and glyph kind, the same with long `loca`, a collection, a CFF font with every operator and a CID-keyed CFF font: 120,000 mutated fonts in one run (the test runs 3,000
  per font by default; `TTF_FUZZ_ITERS`, `TTF_FUZZ_SEED`), no crash, hang or leak. The fuzzer reaches 100% of `glyf.c` and `raster.c`, 99% of `otl.c`, 98% of `cmap.c`,
  94% of `font.c`, 77% of `cff.c` (gcov, 16,000 iterations; the rest is covered by the unit tests). Peak memory 15 MB, largest single request 8 MB (the cell limit).
- Allocation failure injection: every one of the 22 allocation requests of a render/cache/draw scenario is failed once in turn; the answer is the right one or an
  out-of-memory error, and nothing is left allocated afterwards.
- A glyph that includes itself, a chain deeper than 8, 300 components, a subroutine that calls itself, 20^7 nested calls: all end in an error within the budget.

## Toolkit integration

Two layers, both in `user/lib/kappa` and both compiled only when `KAPPA_TTF` is 1 (the default of `kappa_ttf.h`; `-DKAPPA_TTF=0` gives the toolkit as it was, with
the bitmap font alone and no libttf in the programs).

**The generic shim** (`kappa_ttf.h`, `src/ttftext.c`): `kt_new`, `kt_add_file`/`kt_add_font`, `kt_text(canvas, t, size, x, y, utf8, fg)` (`y` is the top of the line box,
like `kc_text`), `kt_text_in` (centred, cut with "..."), `kt_text_width`, `kt_line_height`, `kt_ascent`, `kt_set_blend`, `kt_set_lcd`. Sizes are logical pixels; on a scaled
canvas (`fairy_scale()`) the glyphs are rendered at the physical size, so text on a hi-dpi window is sharp, not enlarged. The clip rectangle is honoured. The ground behind
the text is not known to a canvas, so the coverage curve assumes the opposite extreme of the text's brightness (black behind light text). Any font, any size.

**The system UI font** (`kt_ui_*` in the same files, called from `canvas.c`): the toolkit's own text functions (`kc_text`, `kc_text_n`, `kc_text_bold`, `kc_text_shadow`,
`kc_text_scaled` with scale 1, `kc_text_in`, `kc_text_width`, and the new `kc_text_width_n`, `kc_text_hit`, `kc_text_is_proportional`, `kc_text_use_bitmap`) draw and
measure with **Gensokyo Sans** instead of the 8x16 bitmap font, so every widget and every program that uses them changes at once. The rules:

- The font file is `/ScarletDevilMansion/share/fonts/GensokyoSans-Regular.ttf` (in the initrd; `$KAPPA_UI_FONT` names another file, for tests), read on the first use in each
  process. A missing or unreadable file means the bitmap font, so a build without the fonts (a host test, a text-mode program) behaves as before.
- 13 px, the baseline 13 px below the top of a 16 px line box (where the bitmap font's baseline is). Gensokyo Sans has cap height 700 and x-height 537 units: at 13 px
  that is 9.1 and 6.98 pixels, nearly whole numbers, which keeps the horizontal edges of an unhinted font crisp (12 px gives 6.44 and 14 px 7.52), and the optical size equals
  the bitmap text. The tallest glyph reaches 9.8 px above the baseline and the lowest 2.7 below it, so all ink lies in rows 0 to 15 of the line box (a test guards it):
  menu rows, buttons, list rows and tooltips made for 16 px keep working.
- Plain alpha blending (gamma 0) with contrast 30. Measured (`ink per character`, sum over the pixels of how far each moved from the ground towards the text colour, for
  "Terminal Gensokyo Explorer Patchouli's Notebook File Edit View Go Help"): the libttf defaults (50, 30) put down 0.89 times the bitmap font's ink for dark text on paper and
  1.15 to 1.17 times for light text on red and on night blue; (0, 30) gives 1.02 to 1.03 times in all four colour pairs, so the desktop keeps the weight it had.
- Layout is in logical pixels without rounding (advances and kerning in fractions), rendering at the physical size: widths scale exactly (a scale-2 window lays out as the
  scale-1 one), glyphs are placed at quarter-pixel positions through the glyph cache. `kc_text_width` rounds up to whole pixels.
- Characters the font does not have (accented letters, box drawing, most arrows, CJK) are drawn as 8 px bitmap cells in the same pass (the cell sits at the pen position
  rounded to a pixel). A character neither font has is the font's .notdef box. Tabs are a space, newlines have no width.
- `kc_glyph` is always one bitmap cell (the terminal, the text areas of the Notebook and `k_textarea`); `kc_text_scaled` with scale 2 or more is the enlarged bitmap font
  (big clocks, the Control Panel banner, avatar initials); password fields keep 8 px bullets. A program that calls `kapp_set_hidpi(false)` (the games, the Sketchbook, the
  Gallery: pixel art at the logical resolution, enlarged by the server) keeps the bitmap font, and any program can say `kc_text_use_bitmap(true)` (Bunbunmaru News does: its
  layout counts 8 px cells).
- `k_textfield` measures in pixels: the caret, the selection (drawn in the selection's text colour, white on shrine red) and clicks use `kc_text_width_n` and `kc_text_hit`
  (the nearest character boundary); with the bitmap font it is the old cell code, pixel for pixel.
- Bold has no weight to switch to: `kc_text_bold` draws the text a second time half a pixel to the right and is one pixel wider, like the bitmap version.

Tests: `KappaShim` and `UiFont` (host, below) compare the toolkit's text with libttf's own drawing pixel for pixel at scales 1 to 3, including the bitmap fallback, bold, tabs,
clipping, truncation, hit testing and the switch-off paths. A font the OS draws with is now shipped (`assets/fonts`, docs/ASSETS.md: original work of the owner, license undecided).

## For the browser (the `WebFont` interface)

The web engine's `WebFont` needs a measure, ascent and descent, and glyph runs. They map directly:

| WebFont | libttf |
|---|---|
| measure a string | `ttf_text_width(set, size, text, len, flags)` |
| ascent, descent, line gap | `ttf_metrics(set.fonts[0], size, &m)` (first font of the set) |
| a glyph run with positions | `ttf_layout(...)` (positions, advances, clusters; call with `cap = 0` to size the buffer) |
| draw a run | `ttf_draw_text`, or `ttf_cache_get` plus `ttf_blit` per glyph |
| caret and hit testing | `cluster` and `x` of the glyph positions |
| synthetic italic | `TtfRenderParams.skew` (about 0.2) |
| text decoration | `underline_pos/thick`, `strike_pos/thick` from `ttf_metrics` |

For line breaking, measure words with `ttf_text_width`. Because layout is per font run, split text at script or font changes the way CSS does only if the same set is used for
all of it; a different `font-family` is a different `TtfFontSet`.

## Testing

```
python tools/hosttest.py discover -s tests/host -p "test_ttf.py" [-v]
```

(Windows: the tests run in WSL's Ubuntu through `tools/hosttest.py`; on Linux directly with `python3 -m unittest`.) 148 tests: about one minute without the real-font tests,
about four minutes with them (a loaded PC takes longer). Environment knobs: `TTF_FUZZ_ITERS`, `TTF_FUZZ_SEED`, `TTF_FULL_SWEEP=1` (render every glyph of the big CJK fonts,
not the first 6000), `TOUHOU_FONT_DIR` (default `C:\Windows\Fonts`, `/mnt/c/Windows/Fonts` under WSL).

| Class | What it proves |
|---|---|
| `Container` | directory, collections, names (preference, UTF-16 pairs, truncation), metrics policy, `loca`/`hmtx` variants, 14 kinds of damaged file |
| `GlyphDecoding` | every on/off-curve rule, encodings, composites with every transform and flag, limits, hostile glyph data |
| `CharMap` | formats 0, 4 (delta and array), 6, 12, 13, subtable preference, symbol and Mac maps, random maps against Python |
| `Kerning` | `kern` (both headers, override), GPOS formats 1 and 2, coverage and class formats, Extension lookups, script choice, GSUB ligatures |
| `Raster` | exact coverage, boxes, sub-pixel, oblique, curves, holes and winding, LCD filter, composites, limits, determinism, ink against area |
| `CffDecoding` | hand-computed outlines for every operator and odd operand count, subroutine biases and limits, arithmetic, numbers, CID fonts, malformed charstrings, random round trips; cross-checked with fontTools |
| `TextLayout`, `GlyphCache`, `Blend` | layout rules, UTF-8 against Python, cache semantics and bounds, coverage tables against the formulas, drawing, clipping, LCD order |
| `KappaShim` | the generic shim's text equals libttf's drawing at scales 1 to 3, clip, truncation, switch-off (`-DKAPPA_TTF=0` leaves the file empty), the toolkit compiles warning-free with the switch on and off |
| `UiFont` | canvas.c's text API with the system UI font (a font made in the test, `$KAPPA_UI_FONT`): pixel equality with libttf at 13 px with the baseline 13 down, at scales 2 and 3, bitmap cells for what the font lacks, bold, tabs and newlines, clipping, `kc_text_in` (cutting, the small-box rule), widths, prefix widths and `kc_text_hit`, the bitmap font when the file is missing, `kc_text_use_bitmap`, `kc_text_scaled` staying bitmap |
| `OwnerFonts` | the eight fonts of `assets/fonts` (they are in the repository, so these always run): `SHA256SUMS`, every glyph renders, outlines/advances/cmap/metrics equal fontTools', the 98 kern pairs of Gensokyo Sans read exactly and the specification's pairs are negative, 13 px gives whole-pixel heights, every glyph's ink lies inside the toolkit's 16 px line box at scales 1 and 2, what the font lacks, the display faces map lower case to capitals |
| `Robustness` | fuzzing, allocation failure injection, speed |
| `WindowsFonts` | LOCAL ONLY (skips itself): 22 faces of the owner's fonts: every glyph decodes and renders; geometry (11 fonts, up to 6000 glyphs each, thousands of composites), advances, bearings, character maps, kerning, names and metrics equal fontTools' reading; two real CFF fonts equal operator for operator; ink against an unhinted reference; shapes against FreeType; sample pictures |
| `FreeFonts` | the same comparisons on the freely licensed fonts of matplotlib (38 fonts, 58,877 glyphs render; DejaVu, STIX and Computer Modern equal fontTools' reading, the `kern` table path on a copy with GPOS removed, STIX's GSUB ligatures), so they run wherever matplotlib is installed |

`tests/host/ttfmake.py` builds the fonts (TrueType with any glyph encoding, composites, `cmap` formats, `kern`, GPOS, GSUB, `name`, `OS/2`, `post`, collections, CFF with a
charstring assembler, CID-keyed CFF); its output is itself checked against fontTools. `tests/host/ttftool.c` drives the library (info, outline, render to hex, layout, cache, draw to
PPM/PGM, fuzz, bench, area/box/dump checks, allocation failure injection); `python tests/host/ttfmake.py OUTDIR` writes sample fonts. fontTools, Pillow and numpy are optional
oracles: tests that need them skip themselves.

## Design notes

- **Zero-copy, caller-owned font.** `ttf_open` allocates nothing and keeps a pointer into the file; a font costs 680 bytes and can live in a static.
- **Floating point.** The rasteriser and the layout use `float` (user programs may; the kernel's `-mgeneral-regs-only` is not a concern here). There is no libm dependency:
  the rasteriser's one square root is an integer one, the sRGB transfer curve is a 256-entry table, and the CFF `sqrt` operator uses Newton's iteration.
- **Single-point contours and trailing moves are dropped** from outlines: they enclose nothing, and they would only stretch the bounding box (the header box of Calibri or Consolas
  glyphs includes them).
- **A moveto is lazy** in CFF: the contour's first point is written when something is drawn.
- **Quadratic and cubic segments stay as they are** in the outline; only the rasteriser flattens.

## The desktop in Gensokyo Sans (branch `agent/desktop-fonts`, 2026-11-03)

The owner's interface face now sets the desktop's text. `KAPPA_TTF` defaults to 1 (header `kappa_ttf.h`); the rules are under "Toolkit integration". What was found, in the order
it came up, with the screenshots (under `build/`, git-ignored: `bm-*.png` the bitmap desktop before, `t2-*.png` the same scenes in Gensokyo Sans, `h1-*.png` at 2560x1440,
`t3-greeter*.png` the login screen, `w1-*.png` text field and dialogs, `m1-*.png` Notebook menus and find bar):

1. **The shim was not wired in.** The brief said `canvas.c` had a path for it; it had none (`ttftext.c` was only an addition that nothing called), so the desktop could not
   have used it. Written: the UI font in `ttftext.c`, the dispatch in `canvas.c`, the text field in `widgets.c`. The OS builds without a warning with it on and off.
2. **Size.** 13 px (the bitmap font's capitals are 9 px, the baseline 13 px below the line top): see "Toolkit integration". At 12, 14 and 16 px the x-height falls at 6.4, 7.5 and 8.6
   pixels and the edges blur; the desktop's layouts were made for a 16 px line box and Gensokyo Sans at 13 px keeps all its ink inside it (the tallest glyph 9.8 px above the
   baseline, the lowest 2.7 below; `OwnerFonts.test_sans_ink_stays_inside_the_16_pixel_line_box_of_the_toolkit`), so tooltips, menu rows and buttons did not have to change.
3. **Weight.** libttf's default blend (gamma 50, contrast 30) made dark text on paper 11% lighter than the bitmap font and light text on red or night blue 15 to 17% heavier.
   Plain alpha with contrast 30 is within 3% in every pair of colours measured (table in "Toolkit integration"): the desktop keeps its tone. LCD (sub-pixel) rendering is
   supported by the shim (`kt_set_lcd`) and left off: the panel's stripe order is unknown and screenshots and VNC would show the fringes.
4. **Kerning.** The table (61 pairs) covered 32 of the 208 pairs of the classes `the font specification` asks for, so Tc, Ts, Tw, T, and T-, Fa Fe Fo Fc F, , W. W, Wa We, Ly Lv, r, rc..
   left visible holes in words at 13, 26 and 44 px. Measured with the outlines (`assets/fonts/source/GensokyoSansUI/tools/kernscan.py`: the mean gap between two outlines against
   the median of the class) and fixed in the generator: 37 pairs added, version 0.4 (outlines, advances, cmap and metrics unchanged byte for byte; `SHA256SUMS`, READMEs and
   `tools/check.py` updated). Pairs the scan would add for a stem against a stem (Lb, Lh) are left alone: the foot of an L is part of its shape. The first pass's values were
   not touched. The space (240 units) reads a little easier at 264 in running text at 13 px, but the specification says 240: left, the owner may want to decide.
5. **What the font lacks falls back to the bitmap font**, one 8 px cell per code point, in the same call: accented Latin-1 letters, box drawing, the arrows up and down, the
   multiplication and division signs, the superscript digits, the plus-minus sign, the check mark and the filled triangles (the calculator's keys, the sort marks of the Spell
   Manager, the keyboard diagram of the Control Panel: all of the interface's own non-ASCII strings were checked against both fonts, none shows a missing-glyph box). A bitmap
   cell in running text is legible but plainly another design (monospaced, 8 px wide); Latin-1 and Latin Extended-A composites are the biggest piece of the font still to do.
   Kana, kanji and emoji are in neither font and show the hollow box, as before.
6. **Text that counted cells.** Fixed in the toolkit: the text field (caret, selection, scroll, clicks: pixels now, the nearest boundary), `kc_text_in` (cuts by measured width; and a box too
   small for two letters and dots gets just the letters: the Control Panel's 20 px key caps said `H...` and `N...` where the bitmap font said `Ho` and `Nu`). The selection in a field was dark
   ink on shrine red (hard to read in either font): it is drawn in the selection's text colour now. In programs: 119 of the 5,043 string literals of the OS's programs are wider in Gensokyo
   Sans than in the bitmap font (all of them capitals, by 3 to 10 px; the mean string is 21% narrower; `assets/fonts/source/GensokyoSansUI/tools/uistrings.py` repeats the sweep), so overflow is
   rare, and `kc_text_in` cuts cleanly where a box is too small. Programs that **position by counting** (`strlen(s) * 8`) now centre and right-align with an error of about one pixel per eight characters. Looked at
   in QEMU and fine: the Calculator, the Pocket Watch (clock, timer, calendar), the Control Panel (all seven pages), the Spell Manager (all four tabs), Explorer (list, icons, menus, Delete and
   Properties dialogs; its dialogs wrap by a column count, so their lines end early), the greeter, the Kappa dialogs, the Notebook's menus and find bar. Not fine: **Bunbunmaru News**, whose
   layout engine justifies and wraps by cells (it left word gaps of 20 px), and the pixel-art programs, which place their hand-made text by counting. Both keep the bitmap font: the news by
   a one-line `kc_text_use_bitmap(true)` in its `main.c` (a file outside this branch's remit, for the reason given; a measured line breaker in `layout.c` would let the paper use the face
   the web engine was written for), the games, the Sketchbook and the Gallery because `kapp_set_hidpi(false)` implies it (the server enlarges them: an anti-aliased face would come
   out as blurred blocks). One sentence in the Control Panel's Appearance page said the text was a bitmap font drawn without smoothing: it names the face in use now.
7. **Hi-dpi.** Text is rendered at N times the size, widths scale exactly (no rounding), so a scale-2 window lays out as the scale-1 one: checked at 2560x1440 (`h1-*.png`: the start
   menu, Explorer, Control Panel, Spell Manager are sharp, with real 26 px glyphs, not enlarged ones) and by the pixel-equality tests at scales 2 and 3.
8. **Tests that depended on the bitmap font** and were updated, each for a reason that is the font's geometry and not a regression: `tests/case_hidpi.py` (the start button's label:
   63x14 px with 263 lit pixels in the bitmap font, 56x12 with 177 in Gensokyo Sans; `check_label` accepts either and, for Gensokyo Sans, asserts that at scale 2 the label is about
   twice as large and not made of 2x2 blocks) and `tests/case_notebook.py` (the menu titles are narrower, so the click positions for View and Help moved from 167 and 213 to 154 and 200:
   inside the title boxes of both layouts). No other test of the groups desktop, hidpi, notebook, sketchbook, explorer, login, media_gallery and lock, nor `apps_windows`,
   `games_selftests` and the VNC view tests, needed a change.

9. **Cost.** The programs that draw text now carry libttf (about 46 KB each): the initrd grew from 15,070 to 16,010 KiB (the whole kernel + initrd set is 16.6 MiB with `main`'s other changes; the ESP check
   says "fine" up to 20 MiB). On this PC (one core, `-O2`, glyph cache warm; `ttftool kui bench MS SCALE TEXT` in the kappa build of the test tool) `kc_text` of a 21-character label takes 3.8 us, against
   1.0 us for the bitmap font (6.7 against 1.9 at scale 2), a 43-character line 6.8 us: about 0.2 us per character, so a window with 500 characters costs 0.1 ms. Each process reads the 8.5 KB font file
   once and keeps up to 1 MiB of glyph bitmaps (a 13 px glyph takes about 200 bytes with its cache entry). `notebook_big_file` (100,000 lines, "no slow frames") and the other timing-sensitive GUI tests pass unchanged.

Not done, and what to look at next: hinting (still the biggest difference from a bitmap font at 13 px: stems of 1.2 px land at any phase), accented letters in the font, a bold weight
(the synthetic bold is a half-pixel double strike), `Gensokyo Mono` for the terminal and the Notebook (`kc_glyph` cells), Bunbunmaru's layout, `kc_text_scaled` for large headings (the
Control Panel banner, the greeter's title and the clocks are enlarged bitmap text: a deliberate retro look, but they could use the face at 26 px), the Gallery and Sketchbook UI in the
face (they would need their own scaling rule at hi-dpi).

## Not done (next steps)

1. **Hinting** (TrueType instructions, or an auto-hinter) for crisp text at 9 to 14 px; until then, rounded advances, sub-pixel positions and the coverage curve.
2. **Complex scripts and bidi:** Arabic joining, Indic reordering, Thai, mark positioning, `ccmp`/`locl`/`calt`, bidirectional reordering (UAX #9), vertical text (`vmtx`, `vert`).
3. **Variable fonts** (`fvar` + `gvar`), **colour glyphs** (`COLR` v0/v1, `sbix`, `CBDT`), **WOFF/WOFF2** (needs `user/lib/z` and a Brotli decoder), embedded bitmaps for tiny sizes.
4. Smaller things: a "fit this text into N pixels" helper, synthetic bold, caching of kerning pairs, a text-run cache keyed by string, a `TtfFont` for fonts loaded from a stream.

## Seen on the Dell (2026-11-03, build `8d2b588`, the desktop in Gensokyo Sans)

At 2560x1440 (ui scale 2, the physical frame 2560x1440) the interface text is sharp, proportional and kerned; pictures of the start menu and an Explorer window were taken over VNC (`tools/vncshot.py`; not committed). `/tmp/fairyd.stat` after a few minutes
with a menu and a window opened: 1218 us to compose and 368 us to flush a frame (about 12.6 GB/s to the screen), worst frame 8 ms: the TrueType text costs no visible time. The panel launcher's tooltip ("Explorer") is drawn whole above the strip (the bug the owner reported, fixed the same day).
Not looked at on the Dell: the login screen, the Control Panel pages, Spell Manager, the Notebook, hi-dpi tooltips with long text.
