"""Validate the display prototypes' coverage, metrics and outlines."""
from pathlib import Path
from string import ascii_letters, ascii_uppercase, digits

from fontTools.ttLib import TTFont

from topology import check_outline

ROOT = Path(__file__).resolve().parents[1]


def check(path):
    with TTFont(path, checkChecksums=2) as font:
        tables = set(font.reader.keys())
        assert set("head hhea maxp hmtx cmap glyf loca name OS/2 post gasp".split()) <= tables
        assert not {"fpgm", "prep", "cvt "} & tables
        for name in tables:
            font.reader[name]
        order = font.getGlyphOrder()
        assert order[0] == ".notdef" and len(order) == font["maxp"].numGlyphs
        assert font["head"].unitsPerEm == 1000 and font["OS/2"].fsType == 0
        cmap = font.getBestCmap()
        assert set(map(ord, ascii_letters + digits + " \u00a0!?-:./,")) == set(cmap)
        assert all(cmap[ord(c)] == cmap[ord(c.lower())] for c in ascii_uppercase)
        for name in order:
            glyph = font["glyf"][name]
            if name == "space":
                assert glyph.numberOfContours == 0
                continue
            assert glyph.numberOfContours > 0 and not glyph.program.getBytecode()
            assert -260 <= glyph.yMin < glyph.yMax <= 940
            assert 0 <= glyph.xMin < glyph.xMax <= font["hmtx"][name][0]
            check_outline(name, glyph, font["glyf"])
        print(f"{path.name}: PASS ({len(cmap)} mapped codepoints, {len(order)} glyphs)")


if __name__ == "__main__":
    for face in ("Frost", "Kappa", "Mischief", "Seals"):
        check(ROOT / "build" / f"Gensokyo{face}-Regular.ttf")
