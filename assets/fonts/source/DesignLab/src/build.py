"""Build four original, unhinted display TTFs."""
from pathlib import Path
from string import ascii_uppercase

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from fontTools.ttLib import newTable

from new_designs import frost, kappa, mischief, seals

ROOT = Path(__file__).resolve().parents[1]
# SIL Open Font License 1.1 without a Reserved Font Name (the owner's decision of 2026-11-08; assets/fonts/OFL.txt). The same three strings are in
# GensokyoSansUI/src/fontbuild.py and GensokyoDisplayLab/src/build.py, and tests/host/test_font_license.py fails when the fonts, the
# generators and the LICENSE files disagree.
COPYRIGHT = "Copyright (c) 2026 STONERJ25 (https://github.com/STONERJ25/Touhou.OS-fork)"
LICENSE = ("This Font Software is licensed under the SIL Open Font License, Version 1.1. "
           "This license is available with a FAQ at: https://openfontlicense.org")
LICENSE_URL = "https://openfontlicense.org"
NAMES = {"!": "exclam", "?": "question", "-": "hyphen", ":": "colon",
         ".": "period", "/": "slash", ",": "comma"}
NOTDEF = (540, "M55 0 L55 700 L485 700 L485 0 Z",
          "!M135 80 L405 80 L405 620 L135 620 Z")


def draw(paths):
    pen = TTGlyphPen(None)
    for path in paths:
        counter = path.startswith("!")
        recording = RecordingPen()
        parse_path(path.removeprefix("!"), recording)
        area = AreaPen()
        recording.replay(area)
        target = ReverseContourPen(pen) if (area.value > 0) != counter else pen
        recording.replay(target)
    glyph = pen.glyph()
    glyph.recalcBounds(None)
    return glyph


def build(style, designs):
    designs = {".notdef": NOTDEF, "space": (270,),
               **{NAMES.get(char, char): outline for char, outline in designs.items()}}
    glyphs = {}
    metrics = {}
    for name, (advance, *paths) in designs.items():
        glyphs[name] = draw(paths)
        metrics[name] = (advance, getattr(glyphs[name], "xMin", 0))
    cmap = {ord(name): name for name in designs if len(name) == 1}
    cmap.update({ord(char): name for char, name in NAMES.items()})
    cmap.update({ord(char.lower()): char for char in ascii_uppercase})
    cmap.update({0x20: "space", 0xA0: "space"})

    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(list(glyphs))
    builder.setupCharacterMap(cmap)
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=940, descent=-260, lineGap=0)
    family = f"Gensokyo {style}"
    ps_name = f"Gensokyo{style}-Regular"
    builder.setupNameTable({
        "familyName": family, "styleName": "Regular",
        "uniqueFontIdentifier": f"{ps_name};0.1;display",
        "fullName": family, "version": "Version 0.1", "psName": ps_name,
        "copyright": COPYRIGHT,
        "licenseDescription": LICENSE,
        "licenseInfoURL": LICENSE_URL,
    })
    builder.setupOS2(
        version=4, fsType=0, fsSelection=0xC0,
        usWeightClass=400, usWidthClass=5,
        sTypoAscender=940, sTypoDescender=-260, sTypoLineGap=0,
        usWinAscent=940, usWinDescent=260,
        sCapHeight=glyphs["H"].yMax,
    )
    builder.setupPost(isFixedPitch=0, keepGlyphNames=False)
    builder.font["gasp"] = newTable("gasp")
    builder.font["gasp"].gaspRange = {0xFFFF: 0x000A}
    builder.font["head"].created = builder.font["head"].modified = 3992112000
    builder.font.recalcTimestamp = False
    target = ROOT / "build" / f"{ps_name}.ttf"
    target.parent.mkdir(exist_ok=True)
    builder.save(target)
    print(target.name)


if __name__ == "__main__":
    for style, design in [("Frost", frost()), ("Kappa", kappa()), ("Mischief", mischief()), ("Seals", seals())]:
        build(style, design)
