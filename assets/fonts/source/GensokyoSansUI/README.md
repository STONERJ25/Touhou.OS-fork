# Gensokyo Sans Regular and Bold, and Gensokyo Mono Regular 0.6

The everyday Touhou.OS face for menus, window titles, and reading. It
extends the original lowercase design with uppercase, complete printable
ASCII, nonbreaking space (264 units wide since 0.5), selected UI
punctuation and arrows, and since 0.6 all of Latin-1 Supplement and Latin
Extended-A (the languages of western and central Europe), the currency and
mathematical signs, the typographic spaces, dashes and quotes, and the Slavic
Cyrillic and modern Greek.
The round diamond dots on i and j and the lifted right end of the t
crossbar are the only decorative alternates.

The face uses unhinted quadratic outlines, a 1000-unit em, tabular
digits and kerning (98 pairs of the plain letters, the 61 legacy pairs of
0.3 and 37 added in 0.4, which the accented letters inherit: 2110 pairs in
0.6). Uppercase I has top and bottom
bars, lowercase l has a curved foot, 1 has a flag and foot, and zero
is narrower than uppercase O.

## Rebuild

Run these commands from this directory with Python 3.10 or later.
Building and checking require fontTools. Specimen generation requires
Pillow with FreeType and the sibling GensokyoDisplayLab build.

```
python src/build.py
python tools/check.py
python tools/install.py          (copies build/*.ttf to assets/fonts and rewrites SHA256SUMS)
python tools/specimen.py
```

The fonts go to build/GensokyoSans-Regular.ttf, build/GensokyoSans-Bold.ttf and build/GensokyoMono-Regular.ttf. The checker validates
the 591 codepoints (748 for the Mono) (the list is written out in tools/check.py, not taken
from the generator), table checksums, metrics, tabular figures, kerning,
that the 107 original glyphs are unchanged (a digest), and the topology
of every outline, composites decomposed: no contour crosses another, no
segment is shorter than 6 units, no curve has an off-curve extreme.
tests/host/test_ttf.py (OwnerFonts) reads the installed file with libttf
and tests/host/test_fontgen.py runs the generator again and compares.
The specimen uses FreeType.

The non-Latin alphabets of SPEC.md and the Mono's Bold and ligatures are outside this build. License: SIL Open Font License 1.1, no Reserved Font Name (`LICENSE`).

## The Bold (0.6)

`src/bold.py` makes it from the Regular, one glyph at a time. The weight is drawn by an elliptical pen (`geom.embolden`: every edge of the outline moves away from the
ink by the pen's reach in the direction of its normal, 29 units sideways and 33 up and down; curves by the tangent-line construction, corners mitred), which takes the
stems from 96 to 154 and the horizontal strokes from 82 to 126-136 (the specification asks 150-160 and 128-138; `tools/check.py` measures them on l, E and z). The
pen moves the flat tops and bottoms too, so a piecewise linear map of y puts them back (flat bottom to 0, flat x-height to 537, cap height to 700, ascender to 740,
descender to -210; the overshoot of the round letters stays 12 units): the Bold has the Regular's heights and the toolkit's line box fits it. The counters that
remain are at least 80 units (the eye of the e is 80, its aperture 86).

- **Spacing is measured again:** the sidebearings of a glyph are 0.9 times the Regular's, taken on the ink after the pen (so the n has 60 and 64 units against 67 and 71 and is 614 wide against 570: the Bold is wider by the extra weight of its stems and tighter in proportion); the digits are tabular (one advance, each digit centred);
  the figure space and the punctuation space follow the digit and the period; the space stays 264.
- **Kerning is measured again** on the Bold's own outlines with `tools/kernscan.py`'s rule (a pair is kerned by 55% of the excess of its optical gap over the median of
  its class, in steps of 2 units, at most 100); the pairs are those of the Regular, the ones the Regular loosens (rn, cl ...) and the small designer pairs keep their
  values. The accented letters inherit as in the Regular: 2110 pairs.
- **Glyphs that are unions** (the cedilla and ogonek letters, the barred letters, the ligatures) have the pen put on each part and the parts united afterwards
  (`Face.parts`), which is cleaner than a pen on the finished union; the marks use a thinner pen (16 units: strokes of about 112) with their height held to within 12% of
  the Regular's; the dots of i and j are lifted by 26 units (the pen had left them 39 units from the stem); the small letters in (c), (R), the ordinal indicators and the
  trademark sign, and the small digits, are the Bold's own letters scaled, and the yen sign and the two signs with a bar are drawn after the pen with their gaps kept.
  Where the pen runs two parts into each other `geom.clean` takes the small loops out (the non-zero boundary of the contours).
- **Accents** are placed again from the Bold's own measures by the same code as the Regular's (`glyphset.latin_composites`).

## Latin-1 and Latin Extended-A in 0.6

Written for the owner's face by `src/glyphset.py` (the recipes), `src/latin_outlines.py` (the marks and the special letters),
`src/symbols.py` (the signs), `src/kerning.py` and `src/geom.py` (the geometry: parsing, transforms, a boolean union that keeps the quadratic curves,
an outline offset). The 107 original glyphs, their numbers, advances, bearings, code points and 98 kern pairs are untouched (`tools/check.py` has their digest).

- **Accented letters are the plain letter with a mark above it** (composite glyphs, 176 of the 439): the base is the original glyph at the origin, the mark a helper
  glyph (not in the cmap) placed by an offset. The marks are drawn in the strokes of the face: the grave is the face's ASCII grave and the acute its mirror, the
  circumflex and the caron are the apex of the A (and its flip), the tilde is the ASCII tilde squeezed to 0.58 of its width, the dots of the diaeresis and the dot above
  are the dots of the colon, the ring and the double acute are built from these, the breve and the macron are plain. Their strokes are 70 to 80 units, a little lighter
  than the stems (96) and the horizontals (82). Height: the mark's lowest edge is 615 units up over lower case (one 13 px row above the x-height), 780 over capitals,
  800 over the letters with ascenders; the highest ink is 957 units (the acute of the l), inside the toolkit's 16 px line box (1000). The dotless i (U+0131) and j (U+0237) carry the marks
  for i and j, with narrower variants of the circumflex, caron, tilde, breve, macron and the dots so that they stay inside the letter's 221 and 248 units.
  The mark is centred over the middle of the letter, or over its stem for l, L, J and the dotless j.
- **Marks that touch the letter are unioned into one outline** (no overlapping contours, rule 1 of the specification): the cedilla on c C s S t T and the ogonek on a A e E
  i I u U (the tail of the j, scaled to 0.78: a stem that bends to the left, and its mirror image), at the foot of the stem or at the lowest point of the bowl. The comma
  below of Latvian and Romanian type (the comma of the face at 0.62) is detached, under g G k K l L n N r R (g has the turned comma above); d' l' t' L' are drawn as in
  Czech and Slovak type, with an apostrophe beside the letter (the advance of d and l grows by 64 and 19 units).
- **The letters that need a design:** the bars of Ø ø Ł ł Đ Ð đ Ħ ħ Ŧ ŧ are unioned into the letters (70 and 66 units); æ œ are a and e, o and e, laid over each other's stroke
  and unioned; Æ is drawn as a polygon (the left leg of the A runs into the top bar of an E; the A keeps its counter) and Œ is an O whose right side is the stem of an E;
  ß is a b with an arched top and a larger lower bowl; ð is the d with its stem bent over and a bar; Þ and þ are the P and the p with the stem raised; ŋ Ŋ are the n and the N
  with the tail of the j; ĸ is the k without its ascender; ſ is the f without its bar; Ĳ ĳ ŉ Ŀ ŀ are made of two glyphs of the face.
- **Signs:** ¡ ¿ are the ! and ? turned half a turn; ¢ £ ¤ ¥ € § © ª « ¬ ® ° ± µ ¶ × ÷ and the fractions are drawn in the face's style (the currency signs are 555 units wide, as the
  digits: they line up in tables); superscripts and subscripts are the digits at 0.62 (stems of 59 units), ⁄ is the fraction slash; the spaces of U+2000 to U+200A, U+202F,
  U+205F and U+3000 are empty glyphs with the usual fractions of an em; the dashes, hyphens, quotes, daggers, primes, ‰ ™ № are added; the arrows up and down, the check
  mark, the filled and open squares, triangles, diamonds, circles and stars and the signs ≠ ≤ ≥ ≈ ∞ are what the desktop's programs and the specification ask for.
- **Kerning:** every variant takes the pairs of its base letter in both positions, so T before ó is kerned as T before o (2012 pairs added to the 98).
- **Romanian:** Ș ș Ț ț (U+0218 to U+021B) are the S and T with the comma below of the Latvian letters (`mk.commabelow`, detached), set by `latin_composites` like the others.
- **Not drawn:** Latin Extended-B and the additional Latin (Vietnamese), combining marks (box drawing is in the Mono only; Greek and Cyrillic are below).
- **Cost:** 35.7 KB with Latin-1 and Latin Extended-A (it was 8.6 KB); 47.8 KB with the Cyrillic and Greek below; the initrd grows by the same.

## The Mono (0.6)

`src/mono.py` makes `GensokyoMono-Regular.ttf` (`build_mono(reg)`: a `glyphset.Face` made from the Regular's face, written by the same `fontbuild.make_font` with
`mono=True`: no `kern` table, `post.isFixedPitch`, Panose proportion 9). The brief wanted the cell of the 8x16 terminal font; the toolkit sets text at 13 px, where 8 px
are 615 units (7.995 px), so **the advance of every character is 615** (the em spaces two cells) and not the 500 of the specification, which is 8 px only at 16 px (the
deviation is recorded in the font specification). The vertical metrics, the heights (x-height 537, capitals 700, ascender 740, descender -210) and the strokes (96 and 82) are the Regular's,
so text in the two faces sits on one line and has one colour.

- **Letters:** the Regular's outlines, centred in the cell. Those with more than 500 units of ink (m w M W and the round capitals) are narrowed without thinning their strokes
  (`condense`: erode sideways by 36, scale x, dilate by 36: a stem of 96 stays at 90 to 100, not 60). The ones that would leave a hole in a line of mono text (i j l r f I J, and
  the dotless i and j) are drawn again with the serifs and feet of monospaced faces, as single outlines (`CUSTOM`); the zero gets a dot in its counter, the one keeps its flag and
  foot, so that `0 O 1 l I |` are six different pictures at 13 px (a test). The hyphen is 1.35 times longer, the underscore and the em dash fill the cell, so that rules join.
- **Accents** come from the same recipes as the Sans' (`glyphset.latin_composites` run on the Mono's letters: the marks are centred over each mono letter). IJ, ij and
  n preceded by an apostrophe are two letters and have no place in a cell: they are left out (572 code points, 596 glyphs, 149 of them composites). What the Regular builds from
  several glyphs and is not a letter with a mark (fractions, superscripts, the numero sign) becomes one condensed shape.
- **Box drawing and block elements, all of U+2500 to U+259F,** are generated (`box_glyphs`): the light, heavy and double lines and their corners, tees and crosses from
  rectangles united on a grid (`rect_region`), the dashed lines, the rounded corners, the diagonals, the halves, eighths and quadrants, and the shades as stripes of one pixel row (1 in 4, 1 in 2, 3 in 4: the pattern repeats across cells). Every one fills the cell exactly: 615 wide
  and from -230 to 1000, which at 13 px with the baseline 13 px down is the toolkit's 16 pixel row, so frames and blocks join without a seam. The light stroke is 77 units (one
  pixel at 13 px), the heavy 154, the double two of 77 with 77 between. Quadrants that meet at a corner (the two diagonal ones) are one unit apart so that the outlines do not touch.
- **Checks** (`tools/check.py`, `check_mono`): one advance everywhere, the flags, six different pictures for `0 O 1 l I |`, the dotted zero (three contours), the edges of the
  box-drawing glyphs, and the topology rules as for the Sans. `tests/host/test_ttf.py` (`OwnerFonts`) tests the Mono with libttf: the width of a line of 80 characters is 639.6
  px at 13 px, the coverage, that box drawing fills 8 x 16 pixels exactly and joins (pictures), that the ink of every character lies in the 16 pixel rows.
- **Not done:** the terminal and the Notebook stay on the 8x16 bitmap font. At 13 px, unhinted, a stem of 96 units is 1.25 px wide and falls between pixel rows: the same four
  lines of text have 4943 pixels with ink in the Mono against 3044 in the bitmap font and only a quarter of them (nearly) full, though the total ink is about the same (1.04 times). A Mono
  drawn for exactly 13 px (stems of 77 units on pixel boundaries) is the way to equal it. No Bold, no ligatures (`liga`), no kana.

## Cyrillic and Greek (0.6)

`src/cyrgreek.py` (`add_all`, called at the end of `glyphset.build_regular`; `composites` is called again by `bold.py` and `mono.py`, which make the new letters from the Regular's like all the others).
Written for this face from its own measures (capitals 700, small letters 537, stems 96, horizontals 82, quadratic curves with the control point at the corner of the box), not copied from another font.

- **Letters that are the same picture share the Latin glyph** (`CYR_ALIAS`, `GREEK_ALIAS`: one glyph answers to several code points), which gives them one outline and one set of kerning pairs for free: А В Е К М Н О Р С Т Х а е о р с х у і ј ѕ І Ј Ѕ, and Ё ё Ї ї Ѐ ѐ as the
  Latin letters with the mark; Α Β Ε Ζ Η Ι Κ Μ Ν Ο Ρ Τ Υ Χ ο ν ρ γ; the Greek η ι κ μ are the eng, the dotless i, the kra and the micro sign; Γ Π Φ φ τ are the Cyrillic Г П Ф ф т; the Greek question mark and raised dot, and the tonos, are the
  semicolon, the middle dot and the acute. A test (`SHARED` in `tests/host/test_ttf.py`) pins every one of them.
- **The others are drawn:** simple outlines by path (Б Г Ґ Д З И Л П Ч Ш Щ Ъ Ы Ь Я Ђ Ћ Љ Њ Џ, the small ones likewise, Δ Λ Σ Ω, υ); unions of stems, bars, rings and strokes for the rest, with the parts kept for the Bold
  (`Face.parts`: Ж is the К with its mirror image on the same stem, the arms overlapping the stem so that no two edges coincide, which the union cannot take; У is two diagonal strokes cut to the cap height by `geom.clip_box`; Ф ф Ю ю
  Θ θ Ψ ψ α π σ ς are rings with bars and stems). The wide letters of the capitals are not squeezed: Ж is 1050 units, Ш and Щ 916, Ю 1043, which the Mono condenses to its cell. З is the digit 3, з the same with the
  strokes kept at their weight and the height of a small letter (`refit`: erode, scale, dilate), ε is the з turned round.
- **Accents** are the composites of the Latin blocks again (`composites`, NFD decomposition, the mark placed over the middle of the base found through the cmap, the dotless i and j under a mark): й ў ѓ ќ ѝ, ά έ ή ί ό ύ ώ ϊ ϋ, and
  the capitals; ΐ ΰ and the standalone dialytika tonos stack the acute over the dots; the Greek capitals with a tonos carry it above the letter rather than in front of it.
- **Rough:** ζ ξ ς are the face's c under a bar with a hooked tail (angular); Ђ Ћ ђ are a bar over an n arch; the Ω is an arch with feet and straight legs; Љ Њ љ њ are the л or н and the soft sign sharing a stem. Not drawn: the Cyrillic of
  Central Asia (Ә Ө Ү Қ Ғ Ң Һ ...), polytonic Greek, Coptic, the variant letters ϑ ϕ ϖ ϵ.
- **Checks:** `tools/check.py` has the code points of both alphabets as `CYRILLIC` and `GREEK` (99 and 73) in the coverage of the Sans and the Mono and draws the topology of every glyph; `tests/host/test_ttf.py` (`OwnerFonts`) tests coverage in the three faces,
  the shared glyphs, accents, text widths with inherited kerning and the heights of the capitals and small letters.

## Space width in 0.5 (the owner's decision, 2026-11-03)

The specification gave the space (and the no-break space) 240 units. Set at 13 px in running text (docs/TTF.md) 240 left the words of a line a little close, and 264 read
easier; the owner was asked and **decided on 264**. `SPACE_ADVANCE` in `src/build.py` is the one constant (`tools/check.py` asserts it); at 13 px the space is 3.43 px
instead of 3.12. Nothing else changed: outlines, the other advances, the cmap, the 98 kern pairs and every other metric equal 0.4's (`glyf`, `cmap`, `kern`, `loca`,
`maxp`, `OS/2`, `hhea`, `post`, `gasp` are byte for byte the same; `hmtx` differs in the one entry, `name` in the version strings, `head` in its checksum adjustment).
The seven display faces keep their own space (270 units, in their own generators).

## Spacing revisions in 0.4 (the desktop switch, 2026-11-03)

Found while the desktop was set in this font at 13 px (docs/TTF.md): the kerning table covered 32 of the 208 pairs of the classes the font specification asks for
(A V W Y T P F L against the round lower case, v w y and the punctuation, and r against , . - a c d e g o q), so Tc, Ts, Tw, T, and T- , Fa, Fe, Fo,
Fc, F, , W. and W, , Wa, We, Ly, Lv, r, and rc rd re rg rq left visible holes (looked at in rendered words at 13, 26 and 44 px).

1. 37 pairs added in `SECOND_PASS` of src/spacing.py (the 61 legacy pairs, `FIRST_PASS`, are untouched). Each value is a little under half of the excess that
   `tools/kernscan.py` measures for the pair (the mean gap between the two outlines against the median of its class), which is how far the first pass went on
   average. Pairs the scan would add for stems against stems (Lb, Lh ...) are left alone on purpose.
2. `tools/check.py` asserts every pair of both passes (98 in all).
3. Version 0.4 in the name table. Outlines, advances, the cmap and every other metric are unchanged (`hmtx`, `glyf`, `cmap` equal 0.3's byte for byte).

Not changed in 0.4, on purpose: the space (240 units then; the owner chose 264 for 0.5, see above) and the missing accented
letters (Latin-1 composites are the biggest piece of the font still to do; the toolkit draws them with the bitmap font in the meantime).

## Spacing revisions in 0.3

1. Shortened the capital T bar from 570 to 518 units, kept its 700-unit
   cap height, and centered its stem. Reduced the lowercase i advance
   from 237 to 221 units and centered its dot on the stem.
2. Refined the T pairs Te, To, Ta and Ti, and added small adjustments
   for ii, ij and ji after viewing the first pass at 12, 16 and 42 px.
