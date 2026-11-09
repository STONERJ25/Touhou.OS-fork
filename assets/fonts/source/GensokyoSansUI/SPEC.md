# An original font for TouhouOS: specification and starter file

**Working names:** *Gensokyo Sans* (proportional, for windows, menus and text) and *Gensokyo Mono* (fixed width, for the terminal and code). The owner may rename them.
**Who this is for:** an AI assistant (GPT, Codex, Claude) that is asked to design and build the fonts, working from this file alone. Everything it needs to know about the target is here; the owner reviews the result and copies the files into the operating system's repository.
**Status:** draft 0, written on 2026-11-03.

## 0. How to use this file (for the owner)

1. Create an empty folder for the work (for example `<repos>\GensokyoFont`) and put this file in it as `SPEC.md`. A separate repository is the cleanest; the finished `.ttf` files and their license go into the OS repository later (`assets/fonts/`).
2. Give the assistant section 12, "Starter prompt", and let it read the rest of the file itself.
3. Ask it to work in the stages of section 10 and to show you a specimen picture after every stage. Your eye is the final test: the numbers in section 9 only catch mistakes.

## 1. What the fonts are for

TouhouOS is the owner's own operating system (Touhou-themed, written from scratch). Its desktop draws text with a built-in 8x16 bitmap font today. The new fonts replace it for:
- window titles, menus, buttons and labels (mostly **11 to 16 px**, sometimes 20 to 48 px for headings);
- the terminal and the text editor (**monospace, 8x16 px cells at the default size**) and the source code of the owner's programming language, *ShrineC*;
- a web browser the OS is growing (any size, long paragraphs);
- Notebook and Sketchbook, the desktop's writing and drawing apps.

The owner wants **a font that is ours**: every outline drawn for this project, not derived from another font.

## 2. Ownership and license

- **Original work only.** Do not trace, copy, convert, "inspire from by tracing" or otherwise derive outlines from any existing font or glyph set, including fonts that are free to use. Study of general letterform principles is fine. If a published design was a reference for an idea (for instance, "a humanist sans with an open aperture"), name it in `NOTES.md`; no outlines may come from it.
- The owner holds the copyright. **License: the owner decides.** A good default is the SIL Open Font License 1.1 with no Reserved Font Name, or CC0. Until the owner chooses, put a `LICENSE` file that says "undecided; all rights reserved by the owner", and fill the `name` table's license fields (IDs 13 and 14) with the same words.
- **Decided on 2026-11-08: the SIL Open Font License 1.1 with no Reserved Font Name**, copyright holder STONERJ25. The fonts carry the notice (ID 0), a sentence about the license (ID 13) and its address (ID 14); `LICENSE` and `assets/fonts/OFL.txt` hold the text.
- The fonts must carry `OS/2.fsType = 0` (installable embedding) so that nothing prevents the OS and the browser from embedding them.

## 3. Design brief

**Gensokyo Sans** (proportional)
- A humanist sans-serif: calm, friendly, very legible at 11 to 16 px. Open apertures (c, e, s, a), a generous x-height (at least 0.52 em), large counters, sturdy joins. Round-ish terminals are welcome; do not make it childish.
- Two weights to start with: **Regular** and **Bold**. (Italic is a later stage; do not fake it by slanting.)
- Unambiguous shapes: `I l 1 |`, `0 O`, `5 S`, `2 Z`, `rn m`, `cl d` must be told apart at 12 px. A lowercase `l` with a small tail, `1` with a flag and a foot, a `0` narrower than `O`.
- Digits: lining figures, all the same width (so that columns of numbers line up in menus and tables).
- A few quiet touches of the setting are welcome where they cost nothing in legibility, for example the dots of `i` and `j` as small circles, or a subtle torii-like crossbar in the `t` and `f`. Keep them to a handful and list them in `NOTES.md`.

**Gensokyo Mono** (fixed width)
- The same family feeling, tuned for code: advance width exactly **615 units per 1000** (8.0 px at 13 px, the toolkit's size and the 8 px cell of the terminal; this specification first said 500, which is 8 px only at 16 px: see "Metrics for the fixed-width font"), a slashed or dotted zero, `1 l I |` and `O 0` plainly different, clear `{ } [ ] ( ) < >`, `' ` ,`.`, ';' ':'.
- Regular and Bold.
- **Ligatures** (GSUB `liga`, which is all the renderer reads) for the operators of ShrineC and common code: `->  =>  ::  ..  ..=  ==  !=  <=  >=  &&  ||  <<  >>  //`. Each ligature is optional per glyph pair but must keep the two-cell width (1230 units: two cells of 615).
- The box-drawing and block characters must join exactly (no gaps or overlaps at cell edges) because text-mode programs draw frames with them.

## 4. Character set

Priority 0 is required for the first usable release; later priorities come in their own stages.

| Priority | Coverage |
|----------|----------|
| **P0** | Basic Latin U+0020-007E; Latin-1 Supplement U+00A0-00FF; Latin Extended-A U+0100-017F; General Punctuation (quotes U+2018-201F, dashes U+2010-2015, bullet U+2022, ellipsis U+2026, U+2030, U+2032-2033, U+20AC and the other currency signs); Letterlike and number forms that appear in UI (U+2116, U+2122); Arrows U+2190-2195; Mathematical operators used in documents (U+2212 minus, U+00D7, U+00F7, U+2260, U+2264-2265, U+2248, U+221E, U+221A); Geometric Shapes subset (U+25A0-25A1, U+25B2-25B3, U+25BC-25BD, U+25C6-25C7, U+25CB, U+25CF, U+2605-2606); **U+262F (yin yang) and U+2468 (circled digit nine, Cirno's number)**; U+FFFD (replacement character, shown as a box with a question mark); `.notdef` as a visible empty box. Mono only: Box Drawing U+2500-257F and Block Elements U+2580-259F in full. |
| **P1** | Greek and Coptic U+0370-03FF and Cyrillic U+0400-04FF (basic letters at least); Latin Extended-B subset for Romanian, Vietnamese letters with marks (U+01A0-01B0, U+1EA0-1EF9 can be composites); Superscripts and Subscripts U+2070-209F; Currency Symbols U+20A0-20CF. |
| **P2** | **Hiragana U+3040-309F, Katakana U+30A0-30FF**, CJK Symbols and Punctuation U+3000-303F (the ideographic space, brackets, the middle dot), Halfwidth and Fullwidth Forms U+FF00-FFEF (Mono: the cell is 615 units, so halfwidth katakana are one cell, 615, and fullwidth forms two cells, 1230; the owner confirmed 615 on 2026-11-08). Japanese text matters for this project: draw kana in the same design language (monoline strokes with the same terminals as the Latin). |
| **P3 (stretch)** | The few hundred kanji most used in UI and in Touhou names (博麗霊夢, 霧雨魔理沙, 十六夜咲夜, ...). Kanji are not required: the OS falls back to another CJK font for any character a font lacks, so a font without kanji still works. |

Not needed: emoji, symbols for other scripts, Hangul, Arabic, Indic scripts, mathematical alphanumerics.

## 5. What the OS's renderer reads (hard requirements)

The OS draws text with its own library, *libttf* (written for this project). It is an **exact-area, unhinted** rasteriser: antialiased coverage computed from the real outline, no hinting instructions are executed, no grid fitting. Consequences for the design:
- Stems do not snap to pixels. Choose stem widths so that the font looks even without snapping (section 9 has numbers), avoid hairlines, and give every glyph a clear, open structure that survives being softened a little at 11 px.
- Supported file features (what libttf reads): TrueType outlines (`glyf`/`loca`), simple and composite glyphs (composites with x/y offsets and with point matching), CFF outlines (OpenType-CFF), `cmap` formats 0, 4, 6, 12 and 13, `name`, `OS/2`, `post`, `kern` (format 0), GPOS pair adjustment (`kern` feature), GSUB `liga` (ligatures). **Not read:** hinting programs, `post` glyph names, variable fonts, colour glyphs (COLR/SVG/bitmaps), WOFF/WOFF2, contextual alternates (`calt`), mark positioning (`mark`/`mkmk`), complex shaping.
- So: **build plain `.ttf` files with quadratic (TrueType) outlines**, one face per file, no `.ttc`. Use composites for accented letters, but give composites that are used with combining marks real base glyphs; do not rely on mark positioning.

## 6. Technical specification

**File names:** `GensokyoSans-Regular.ttf`, `GensokyoSans-Bold.ttf`, `GensokyoMono-Regular.ttf`, `GensokyoMono-Bold.ttf`.

**Tables required:** `head`, `hhea`, `maxp` (version 1.0), `hmtx`, `cmap`, `glyf`, `loca`, `name`, `OS/2` (version 4 or 5), `post` (format 3 is fine). Recommended: `kern` or `GPOS` (Sans: pair kerning), `GSUB` with `liga` (Mono), `gasp` (one range up to 0xFFFF with gridfitting off and smoothing on: flags 0x000A). Do not include `fpgm`, `prep`, `cvt ` or glyph instructions.

**Units:** `unitsPerEm = 1000`. All coordinates are integers.

**Vertical metrics (1000 units/em):** cap height 700, x-height 530 to 540 (Sans) / 520 (Mono), ascender (b d h k l) 740, descender (g j p q y) -210; `hhea.ascent = 940`, `hhea.descent = -260`, `lineGap = 0` (so the default line is 1.2 em); `OS/2.sTypoAscender/Descender/LineGap` equal to `hhea`, `usWinAscent = 940`, `usWinDescent = 260`, `fsSelection` bit 7 (USE_TYPO_METRICS) set; `OS/2.sxHeight` and `sCapHeight` set to the real values. Overshoot of round letters: 10 to 14 units above and below the flat heights.

**Metrics for the fixed-width font:** every glyph of the basic set has `advanceWidth = 615` (the first draft of this specification said 500: that is 8 px at 16 px, but the toolkit sets text at 13 px, where a cell of 500 is 6.5 px, not the 8 px of the terminal's 8x16 cell; 615 units at 13 px are 7.995 px and the vertical metrics of the Sans fit the 16 px row; decided with version 0.6 and confirmed by the owner on 2026-11-08: a whole-pixel cell is what the brief asked for); two cells (`1230`) for the em spaces, and for fullwidth forms and the CJK punctuation once they are drawn. `post.isFixedPitch = 1`, `OS/2.panose.bProportion = 9` (monospaced). Ligatures (not made yet) replace a pair of cells by two glyphs of 615 each, or one glyph of 1230, never anything else.

**Stems and details for an unhinted renderer at 11 to 16 px:**
| Feature | Regular | Bold |
|---------|---------|------|
| Vertical stem (l, i, |) | 92 to 98 | 150 to 160 |
| Horizontal stem (e crossbar, t, f) | 78 to 84 | 128 to 138 |
| Smallest counter or gap (inside e, a, s) | at least 90 | at least 80 |
| Letter-spacing (sidebearings) | 55 to 75 on straight stems | 45 to 65 |
At 16 px one pixel is 62.5 units: a Regular stem is therefore about 1.5 px, a Bold stem about 2.5 px, which is what the eye wants without hinting. Never use a stem under 60 units.

**Outline rules (they decide whether the glyphs render cleanly):**
1. **Remove overlaps.** Each glyph's outline is a set of non-overlapping contours (union the strokes). libttf fills by non-zero winding and darkens the one or two pixels where same-direction contours overlap. Counters are contours of the opposite direction.
2. **Direction:** outer contours clockwise, inner (counter) contours counter-clockwise (TrueType convention).
3. **Start points and extrema:** put an on-curve point at every horizontal and vertical extreme of a curve; no point inside a straight stretch; no zero-length and no tiny segments (under 6 units) except where a design needs them; at most about 100 points per ordinary glyph.
4. **Curves:** quadratic splines; where two curves meet smoothly, the off-curve points on either side are collinear with the on-curve point between them (or use implied on-curve points between consecutive off-curve points).
5. Composites: accents are separate glyphs placed by offsets; every composite component must itself be a simple glyph; no more than two levels.
6. `.notdef` is glyph 0: a clearly visible box (and for Mono, 615 wide); the space glyph and the no-break space have no contours and the right advance (Sans: 264, the owner's decision of 2026-11-03: this specification first said 240; Mono: 615).
7. Glyph names are free (they are not stored: `post` format 3); keep them sensible in the source anyway (`uni00E9`, `eacute`, ...).

**Kerning (Sans):** a pair table (`kern` format 0, or GPOS PairPos) with at least these classes of pairs, tuned by eye on the specimen: `A V W Y T P F L` against `A V W Y T O C G U c e o a d q s y v w , . - : ; ' "` and `r` followed by `, . - a c d e g o q`. Zero for Mono.

**Names (`name` table, platform 3, encoding 1, language 0x409; and the same strings on platform 1 if you like):** ID 1 family ("Gensokyo Sans"), 2 subfamily ("Regular"/"Bold"), 3 unique id, 4 full name, 5 version ("Version 0.1"), 6 PostScript name ("GensokyoSans-Regular", no spaces), 0 copyright ("Copyright (c) 2026 <the owner>"), 13 license description, 14 license URL (if any). `head.macStyle` and `OS/2.fsSelection` bits agree with the subfamily; `OS/2.usWeightClass` 400 / 700; `usWidthClass` 5.

**Size budget:** each face under 300 KB with P0 and P1; under 600 KB with kana. Fewer points is better.

## 7. Source of truth and tools

The fonts are **generated from source**, not hand-edited binaries, so that every number above can be changed and the build repeated:
- Preferred: a Python generator using **fontTools** (installed on the owner's PC: `python -c "import fontTools"`). Glyph designs as code: parametric skeletons (stems, bowls, joins) with width, weight and spacing as parameters, turned into outlines, unioned and written with `fontTools.fontBuilder.FontBuilder` and `TTGlyphPen`. Remove overlaps with a path-union library if one is available (`skia-pathops` or `booleanOperations`); if none is installed **do not install anything without asking the owner**: design shapes so that they do not overlap by construction (draw each letter as one outline), or ask.
- Alternative: draw in a font editor of the assistant's choice and export UFO or SFD source; whatever is used, the repository must contain the source files and a script or documented command that rebuilds the `.ttf` files.
- Never commit generated files that cannot be rebuilt, and never check in fonts that came from elsewhere.

## 8. Layout of the deliverable

```
SPEC.md              this file
README.md            what the fonts are, how to rebuild, how to preview
NOTES.md             design decisions, the handful of touches, references (names only), known issues
LICENSE              the owner's chosen license, or the "undecided" placeholder
src/                 generator scripts or font sources
build/               the four .ttf files (generated)
specimen/            PNG specimen sheets (section 9) at 9, 11, 12, 16, 24, 48 px
tools/check.py       the automatic checks of section 9
```

## 9. Acceptance: how the result is judged

Automatic (run by `tools/check.py`, which exits non-zero on a failure):
1. Each `.ttf` opens with `fontTools.ttLib.TTFont`; the required tables are present, `maxp`/`hhea`/`hmtx` agree, no table checksum is wrong, `head.magicNumber` correct.
2. `cmap` covers every code point of the priorities delivered so far (the script prints the missing ones); `.notdef` is glyph 0; `U+0020` and `U+00A0` map to contour-less glyphs.
3. For Mono: every glyph in the basic set has advance 615 (two cells, 1230, for the em spaces); the box-drawing glyphs touch the cell edges (x = 0 and x = 615, y = -230 and 1000, the 16 pixel row of the toolkit at 13 px) exactly.
4. Outline hygiene: integer coordinates; no overlapping contours within a glyph (compute with a path union or an area test); contour directions as specified; no segment shorter than 6 units unless allow-listed; point count per glyph reported (flag > 120); extrema have on-curve points.
5. Metrics: ascent/descent/lineGap agree between `hhea` and `OS/2` typo values; `fsType = 0`; `usWeightClass` 400/700; `isFixedPitch` right.
6. Kerning: the pair table loads (`kern` or `GPOS`) and the test pairs (`AV`, `To`, `Yo`, `r.`) have negative values.

By eye (the owner and the assistant, together): specimen sheets rendered at 9, 11, 12, 16, 24 and 48 px with the pangram, the alphabet in both cases, the digits, punctuation, the `Il1|` and `0O` rows, ligature rows (Mono), a paragraph of English, a paragraph of kana (once drawn), a box-drawing frame, and a "shrine" sample (a short ShrineC program). Even colour (no letter noticeably darker or lighter), even spacing (no pair that looks too tight or loose), shapes that stay recognisable at 11 px.

**With the OS's own renderer** (optional, if the repository `Touhou.OS-fork` is at hand): `python tools/hosttest.py discover -s tests/host -p "test_ttf.py"` builds a small command `ttftool` (in `build/host-x86-wsl/`) that draws text with the very rasteriser the OS uses: `ttftool draw OUT.ppm FONT.ttf SIZE "TEXT"`. Previewing with it shows exactly how the OS will draw the font. `docs/TTF.md` there describes the library and the tool.

## 10. Stages (show a specimen after each; the owner can stop at any stage)

1. **Skeleton:** the generator, the metrics, `.notdef`, space, digits `0-9` and Latin lowercase, Regular, Sans. Specimen. Fix the stem and spacing rules here: everything later follows them.
2. **Latin uppercase, punctuation, the P0 symbols** (Sans Regular). Specimen, kerning pass 1.
3. **Bold** (a weight parameter, not an automatic embolden; compare with Regular at each size).
4. **Accents:** Latin-1, Latin Extended-A as composites; check heights at 11 px.
5. **Mono** Regular and Bold: reuse the skeleton with fixed advance, redraw the letters that need monospace help (i l j f t r m w), the zero, the ligatures, box drawing and blocks.
6. **P1** alphabets (Greek, Cyrillic) if wanted.
7. **Kana** (P2) and fullwidth forms; the decision whether to try kanji (P3).
8. **Packaging:** checks, README, NOTES, license.

## 11. What the assistant hands back

The folder of section 8, plus a short message: what was built, what is missing from the priority list, any deliberate deviation from this file (with the reason), and anything the owner must decide (license, names). Please do not claim that a check passed unless it was run.

## 12. Starter prompt (paste this to the assistant)

> You are helping me, the owner of TouhouOS, make two original fonts for my operating system: **Gensokyo Sans** (proportional) and **Gensokyo Mono** (fixed width), each in Regular and Bold, as TrueType `.ttf` files. Read `SPEC.md` in this folder completely before doing anything; it is the specification. Rules that matter most: every outline must be original (no tracing, copying or converting any existing font); the fonts are generated from source files that you commit, so that the build can be repeated; outlines must have no overlapping contours; the renderer is unhinted, so stem widths and spacing follow section 6; do not install software without asking me.
> Work in the stages of section 10. After each stage, build the fonts, render a specimen sheet to `specimen/` as PNG, look at it yourself, tell me what you see that is wrong, fix it, and only then show me. Write `tools/check.py` early (section 9) and run it after every build. Keep `NOTES.md` up to date with your design decisions. Start with stage 1 now, and ask me a question only if a decision is really mine to make (license, names, style direction).
