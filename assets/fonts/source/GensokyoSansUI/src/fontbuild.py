"""Writes a glyphset.Face as a TrueType font with fontTools: glyf (the original glyphs through the original code path, byte for byte; the new ones from
geom shapes; composites), cmap, hmtx, kern, name, OS/2, post, gasp. Shared by the Regular, Bold and Mono builds."""
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

import geom

# SIL Open Font License 1.1 without a Reserved Font Name (the owner's decision of 2026-11-08; assets/fonts/OFL.txt). The same three strings are in
# GensokyoDisplayLab/src/build.py and DesignLab/src/build.py, and tests/host/test_font_license.py fails when the fonts, the generators and the
# LICENSE files disagree.
COPYRIGHT = "Copyright (c) 2026 STONERJ25 (https://github.com/STONERJ25/Touhou.OS-fork)"
LICENSE = ("This Font Software is licensed under the SIL Open Font License, Version 1.1. "
           "This license is available with a FAQ at: https://openfontlicense.org")
LICENSE_URL = "https://openfontlicense.org"


def legacy_glyph(outline, mirror=False):
    """The original generator's glyph: an advance followed by path strings (a leading '!' marks a counter); the winding is normalised and, for d and q,
    the b and p are mirrored."""
    width, *paths = outline
    pen = TTGlyphPen(None)
    for path in paths:
        counter = path.startswith("!")
        recording = RecordingPen()
        target = TransformPen(recording, (-1, 0, 0, 1, width, 0)) if mirror else recording
        parse_path(path.removeprefix("!"), target)
        area = AreaPen()
        recording.replay(area)
        if (area.value > 0) != counter:
            recording.replay(ReverseContourPen(pen))
        else:
            recording.replay(pen)
    glyph = pen.glyph()
    glyph.recalcBounds(None)
    return glyph


def shape_glyph(shape):
    pen = TTGlyphPen(None)
    geom.draw(shape, pen)
    glyph = pen.glyph()
    glyph.recalcBounds(None)
    return glyph


def composite_glyph(parts, names):
    pen = TTGlyphPen(names)                                  # (the pen only checks that the components exist)
    for name, dx, dy in parts:
        pen.addComponent(name, (1, 0, 0, 1, dx, dy))
    return pen.glyph()


def kern_table(pairs):
    """pairs: {(left glyph name, right glyph name): value}. A format 0 subtable holds 10920 pairs at most."""
    if len(pairs) > 10920:
        raise ValueError("%d kern pairs do not fit a format 0 subtable" % len(pairs))
    kern = newTable("kern")
    kern.version = 0
    sub = KernTable_format_0()
    sub.coverage = 1
    sub.kernTable = dict(pairs)
    kern.kernTables = [sub]
    return kern


def make_font(face, *, family, style, version, kern=None, weight=400, mono=False, order=None):
    """face: a glyphset.Face. style: 'Regular' or 'Bold'. kern: {(left name, right name): value} or None."""
    bold = style == "Bold"
    order = order or face.names
    glyphs = {}
    for name in order:
        if name in face.legacy:
            outline, mirrored = face.legacy[name]
            glyphs[name] = legacy_glyph(outline, mirrored)
        elif name in face.comps:
            glyphs[name] = composite_glyph(face.comps[name], set(order))
        elif name in face.out:                              # shapes that are already rounded to integers
            glyphs[name] = shape_glyph(face.out[name])
        else:
            glyphs[name] = shape_glyph(geom.rnd(face.shape[name], min_len=6))
    ps = "%s-%s" % (family.replace(" ", ""), style)
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(list(order))
    builder.setupCharacterMap(face.cmap)
    builder.setupGlyf(glyphs)
    glyf = builder.font["glyf"]
    metrics = {name: (face.adv[name], getattr(glyf[name], "xMin", 0)) for name in order}
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=940, descent=-260, lineGap=0)
    builder.setupNameTable({
        "familyName": family,
        "styleName": style,
        "uniqueFontIdentifier": "%s;%s;ui" % (ps, version),
        "fullName": "%s %s" % (family, style),
        "version": "Version %s" % version,
        "psName": ps,
        "copyright": COPYRIGHT,
        "licenseDescription": LICENSE,
        "licenseInfoURL": LICENSE_URL,
    })
    builder.setupOS2(
        version=4, fsType=0, fsSelection=0xA0 if bold else 0xC0,
        usWeightClass=weight, usWidthClass=5,
        sTypoAscender=940, sTypoDescender=-260, sTypoLineGap=0,
        usWinAscent=940, usWinDescent=260, sxHeight=537, sCapHeight=700,
    )
    if mono:
        from fontTools.ttLib.tables.O_S_2f_2 import Panose
        builder.font["OS/2"].panose = Panose()
        builder.font["OS/2"].panose.bFamilyType = 2
        builder.font["OS/2"].panose.bProportion = 9                       # monospaced
    builder.setupPost(isFixedPitch=1 if mono else 0, keepGlyphNames=False)
    builder.font["gasp"] = newTable("gasp")
    builder.font["gasp"].gaspRange = {0xFFFF: 0x000A}
    head = builder.font["head"]
    head.created = head.modified = 3992112000
    if bold:
        head.macStyle = 1
    builder.font.recalcTimestamp = False
    if kern:
        builder.font["kern"] = kern_table(kern)
    return builder
