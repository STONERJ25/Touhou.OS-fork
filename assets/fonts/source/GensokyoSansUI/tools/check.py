"""Check coverage, font structure, metrics, kerning and outline topology of the built Gensokyo Sans faces (build/GensokyoSans-Regular.ttf, and the Bold and
the Mono when they have been built)."""
from pathlib import Path
import hashlib
import sys

from fontTools.pens.pointInsidePen import PointInsidePen
from fontTools.ttLib import TTFont

from topology import check_outline

ROOT = Path(__file__).resolve().parents[1]

# what the face must cover (written out here, not taken from the generator)
ASCII = set(range(0x20, 0x7F))
LEGACY_MARKS = set(map(ord, " –—•…→←−’‘”“"))                        # the 13 signs of 0.3 and 0.4
LATIN1 = set(range(0xA0, 0x100))
LATIN_EXT_A = set(range(0x100, 0x180))
ROMANIAN = {0x218, 0x219, 0x21A, 0x21B}                                         # S and T with a comma below
SPACES = {0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x202F, 0x205F, 0x3000}
EXTRA = set(map(ord, "ȷˆˇ˘˙˚˛˜˝†‡‰′″‹›€™№⁄‚‛„‟‒―‐‑↑↓↔↕✓▲▼▪▾▸◦◆◇●○★☆■□△▽≠≤≥≈∞⁰⁴⁵⁶⁷⁸⁹₀₁₂₃₄₅₆₇₈₉")) | SPACES
CYRILLIC = set(range(0x400, 0x460)) | {0x490, 0x491, 0x4C0}                  # the Slavic letters (Russian, Ukrainian, Belarusian, Serbian, Macedonian, Bulgarian) and the palochka
GREEK = set(range(0x384, 0x38B)) | {0x38C, 0x38E, 0x38F, 0x390, 0x37E} | (set(range(0x391, 0x3CF)) - {0x3A2})      # modern Greek: tonos, dialytika, the question mark
REGULAR_COVERAGE = ASCII | LEGACY_MARKS | LATIN1 | LATIN_EXT_A | ROMANIAN | EXTRA | CYRILLIC | GREEK
BOX_AND_BLOCKS = set(range(0x2500, 0x25A0))
MONO_COVERAGE = (ASCII | LATIN1 | LATIN_EXT_A | ROMANIAN | CYRILLIC | GREEK | BOX_AND_BLOCKS | set(map(ord, "–—•…→←−’‘”“↑↓✓▲▼■□●○★☆≠≤≥≈∞€™"))) - {0x132, 0x133, 0x149}      # (IJ, ij and n with an apostrophe are two letters)
MONO_CELL = 615                                        # 8 px at 13 px, the cell of the 8x16 bitmap font
# the digest of the 107 original glyphs (name, advance, bearing, points, flags, contours): the original design must not change by accident
LEGACY_DIGEST = "d293fa05800ad077ca924d33a6c7275ab6eb892714d398e93b6f9fa2da7347e7"


def legacy_digest(font):
    h = hashlib.sha256()
    for name in font.getGlyphOrder()[:107]:
        g = font["glyf"][name]
        simple = g.numberOfContours > 0
        h.update(repr((name, font["hmtx"][name], list(g.coordinates) if simple else [], list(g.flags) if simple else [],
                       list(g.endPtsOfContours) if simple else [])).encode())
    return h.hexdigest()


def ink_run(glyphset, name, point, direction, limit=400):
    """The length of the stretch of ink that starts at `point` and runs in `direction` ((1, 0) or (0, -1))."""
    n = 0
    while n < limit:
        pen = PointInsidePen(glyphset, (point[0] + direction[0] * n + 0.5, point[1] + direction[1] * n + 0.5))
        glyphset[name].draw(pen)
        if not pen.getResult():
            break
        n += 1
    return n


def check_weight(font, style):
    """The stems and horizontal strokes of the font specification section 6: Regular 92 to 98 and 78 to 84 units, Bold 150 to 160 and 128 to 138 (the horizontals of the
    lower case of the Bold may be two units under, where the x-height zone is squeezed most). Measured on the letters l (stem), E (top bar) and z (top bar)."""
    gs, cmap = font.getGlyphSet(), font.getBestCmap()
    left = 0
    while True:                                                              # the left edge of the stem of the l, at the height 300
        pen = PointInsidePen(gs, (left + 0.5, 300.5))
        gs[cmap[ord("l")]].draw(pen)
        if pen.getResult() or left > 300:
            break
        left += 1
    stem = ink_run(gs, cmap[ord("l")], (left, 300), (1, 0))
    cap_bar = ink_run(gs, cmap[ord("E")], (350, 699), (0, -1))
    low_bar = ink_run(gs, cmap[ord("z")], (250, 536), (0, -1))
    if style == "Bold":
        assert 150 <= stem <= 160, f"Bold stem {stem}"
        assert 128 <= cap_bar <= 138, f"Bold capital bar {cap_bar}"
        assert 126 <= low_bar <= 138, f"Bold lower-case bar {low_bar}"
    else:
        assert 92 <= stem <= 98, f"Regular stem {stem}"
        assert 78 <= cap_bar <= 84 and 78 <= low_bar <= 84, f"Regular bars {cap_bar} {low_bar}"
    return stem, cap_bar, low_bar


def check_mono(font, cmap, order, errors, name):
    """What makes the Mono a fixed-width face: one advance for every character (two cells for the em spaces), the flags of a monospaced face, 0 O 1 l I | all different,
    and box drawing that fills the cell exactly (615 wide, from -230 to 1000: the 16 pixel row of the toolkit at 13 px) so that frames join."""
    glyf, hmtx = font["glyf"], font["hmtx"]
    assert font["post"].isFixedPitch == 1 and font["OS/2"].panose.bProportion == 9, "monospaced flags"
    for code, glyph_name in cmap.items():
        assert hmtx[glyph_name][0] in (MONO_CELL, 2 * MONO_CELL), "U+%04X: advance %d" % (code, hmtx[glyph_name][0])
    assert {hmtx[cmap[c]][0] for c in range(0x20, 0x7F)} == {MONO_CELL}
    shapes = {}
    for ch in "0O1lI|":
        coords, ends, flags = glyf[cmap[ord(ch)]].getCoordinates(glyf)
        shapes[ch] = (list(coords), list(ends))
    assert len({repr(v) for v in shapes.values()}) == 6, "0 O 1 l I | must be six different pictures"
    assert len(shapes["0"][1]) == 3, "the zero has a dot in its counter"
    for code, (x0, y0, x1, y1) in {0x2500: (0, 347, 615, 423), 0x2502: (269, -230, 346, 1000), 0x2588: (0, -230, 615, 1000), 0x253C: (0, -230, 615, 1000),
                                    0x2501: (0, 308, 615, 462), 0x2503: (230, -230, 385, 1000), 0x2550: (0, 270, 615, 500)}.items():
        g = glyf[cmap[code]]
        assert (g.xMin, g.yMin, g.xMax, g.yMax) == (x0, y0, x1, y1), "U+%04X: %s" % (code, (g.xMin, g.yMin, g.xMax, g.yMax))
    for code in range(0x2500, 0x25A0):
        g = glyf[cmap[code]]
        assert g.xMin >= 0 and g.xMax <= 615 and g.yMin >= -230 and g.yMax <= 1000, "U+%04X leaves the cell" % code
    return len(glyf[cmap[0x2500]].getCoordinates(glyf)[0])


def check_face(path, style, errors, mono=False):
    with TTFont(path, checkChecksums=2) as font:
        tables = set(font.reader.keys())
        assert set("head hhea maxp hmtx cmap glyf loca name OS/2 post gasp".split()) <= tables
        assert mono or "kern" in tables
        assert not {"fpgm", "prep", "cvt "} & tables
        for tag in tables:
            font.reader[tag]
        order = font.getGlyphOrder()
        cmap = font.getBestCmap()
        missing = sorted((MONO_COVERAGE if mono else REGULAR_COVERAGE) - set(cmap))
        assert not missing, "missing code points: " + " ".join("U+%04X" % c for c in missing)
        assert order[0] == ".notdef"
        assert len(order) == font["maxp"].numGlyphs == len(font["hmtx"].metrics)
        assert len(font["loca"].locations) == len(order) + 1
        head, hhea, os2, hmtx, glyf = font["head"], font["hhea"], font["OS/2"], font["hmtx"], font["glyf"]
        assert (head.unitsPerEm, head.magicNumber) == (1000, 0x5F0F3CF5)
        assert (hhea.ascent, hhea.descent, hhea.lineGap) == (940, -260, 0)
        assert (os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap) == (940, -260, 0)
        assert (os2.usWinAscent, os2.usWinDescent) == (940, 260)
        assert (os2.fsType, os2.sCapHeight, os2.sxHeight) == (0, 700, 537)
        assert os2.usWeightClass == (700 if style == "Bold" else 400)
        assert bool(head.macStyle & 1) == (style == "Bold") and bool(os2.fsSelection & 0x20) == (style == "Bold")
        assert os2.fsSelection & 0x80, "USE_TYPO_METRICS"
        assert {hmtx[cmap[code]][0] for code in range(48, 58)} == {MONO_CELL if mono else 555 if style != "Bold" else hmtx[cmap[48]][0]}, "tabular figures"
        space_adv = MONO_CELL if mono else 264 if style != "Bold" else hmtx[cmap[32]][0]
        assert cmap[32] == cmap[160] and hmtx[cmap[32]][0] == space_adv        # 264: the owner's decision (2026-11-03); the specification's first value was 240
        kern_pairs = 0
        if not mono:
            table = font["kern"].kernTables[0]
            assert table.format == 0 and table.coverage == 1
            for pair in ("AV", "To", "Yo", "r.", "Ti", "ii", "ij", "ji"):
                assert table.kernTable[tuple(cmap[ord(c)] for c in pair)] < 0
            sys.path.insert(0, str(ROOT / "src"))
            from spacing import FIRST_PASS, SECOND_PASS            # noqa: E402  (the 61 legacy pairs of 0.3 and the 37 added in 0.4)
            if style == "Regular":
                for (left, right), value in {**FIRST_PASS, **SECOND_PASS}.items():
                    assert table.kernTable[cmap[ord(left)], cmap[ord(right)]] == value, f"kern pair {left}{right}"
                assert legacy_digest(font) == LEGACY_DIGEST, "the 107 original glyphs changed"
            assert len(table.kernTable) <= 10920
            kern_pairs = len(table.kernTable)
        else:
            assert "kern" not in tables, "a fixed-width face has no kerning"
            check_mono(font, cmap, order, errors, path.name)
        composites = 0
        glyphset = font.getGlyphSet()
        for name in order:
            glyph = glyf[name]
            if glyph.numberOfContours == 0:
                continue
            assert not glyph.program.getBytecode() if glyph.numberOfContours > 0 else True
            coords, ends, _ = glyph.getCoordinates(glyf)
            assert all(isinstance(value, int) for point in coords for value in point)
            assert glyph.yMin >= -260 and glyph.yMax <= 1000, f"{name}: ink outside the line box ({glyph.yMin}..{glyph.yMax})"
            if glyph.numberOfContours > 0:
                assert len(glyph.coordinates) <= 120, f"{name}: {len(glyph.coordinates)} points"
            else:
                composites += 1
                for comp in glyph.components:
                    assert comp.glyphName in glyf.glyphs and glyf[comp.glyphName].numberOfContours > 0, f"{name}: component {comp.glyphName}"
                    assert -600 <= comp.x <= 1200 and -600 <= comp.y <= 1200
            assert hmtx[name][1] == glyph.xMin, f"{name}: left bearing {hmtx[name][1]} is not xMin {glyph.xMin}"
            try:
                check_outline(name, glyph, glyf, glyphset)
            except AssertionError as error:
                errors.append(f"{path.name}: {error}")
        stem, cap_bar, low_bar = check_weight(font, style)
        print(f"{path.name}: {len(cmap)} codepoints, {len(order)} glyphs ({composites} composite), {kern_pairs} kern pairs; "
              f"stem {stem}, bars {cap_bar} and {low_bar}")


def main():
    errors = []
    built = [(ROOT / "build" / f"GensokyoSans-{style}.ttf", style, False) for style in ("Regular", "Bold")] + [(ROOT / "build" / "GensokyoMono-Regular.ttf", "Regular", True)]
    for path, style, mono in built:
        if path.exists():
            check_face(path, style, errors, mono)
    if errors:
        sys.exit("\n".join(errors))
    print("PASS")


if __name__ == "__main__":
    main()
