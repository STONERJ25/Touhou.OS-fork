"""Gensokyo Sans Bold: a heavier weight drawn from the Regular, glyph by glyph, with its spacing measured again.

How a glyph is made. Every edge of the Regular outline moves away from the ink by the reach of an elliptical pen (geom.embolden: EX = 29 units sideways, EY = 30
up and down), which thickens the vertical stems from 96 to 154 and the horizontal strokes from 82 to 142, the weights the font specification section 6 asks for (stems
150 to 160, horizontals 128 to 138). That moves the flat tops and bottoms too, so the heights are put back: a piecewise linear map of y takes the flat bottom
(-EY) to 0, the flat top of the x-height (537 + EY) to 537, the cap height (700 + EY) to 700, the ascender (740 + EY) to 740 and the descender (-210 - EY) to
-210, which leaves the overshoot of the round letters what it was and compresses the x-height and cap zones by 10% and 8% (the horizontals end up at 128 to 130).
The Bold therefore has the Regular's x-height, cap height, ascender and descender, and the toolkit's line box fits it as it fits the Regular.

Spacing: the sidebearings of every glyph are the Regular's times SB (0.9), measured on the ink of the glyph after the pen, not copied: the Bold is wider by the
extra weight of its stems and tighter in proportion (the specification asks for 45 to 65 units on straight stems, against 55 to 75 in the Regular). Digits stay
tabular. The kerning is measured again on the Bold's outlines (tools/kernscan.py's rule).
"""
import geom
from geom import embolden, translate, rnd, ink_bbox, scale

EX, EY = 29, 33                      # the pen of the letters (half-axes, units)
SB = 0.9                             # bold sidebearing = regular sidebearing x SB
MARK_PEN = 16                        # the pen of the accent marks (their strokes are 80 in the Regular)

LOWER = set("abcdefghijklmnopqrstuvwxyz")


def zone_map(kind):
    """The y map that puts the flat tops and bottoms of an emboldened glyph back where they were: kind 'lower' (x-height, ascender, descender) or 'upper' (caps)."""
    if kind == "lower":
        knots = [(-210 - EY, -210), (-EY, 0), (537 + EY, 537), (740 + EY, 740)]
    else:
        knots = [(-EY, 0), (700 + EY, 700)]

    def f(y):
        if y <= knots[0][0]:
            return knots[0][1] + (y - knots[0][0])
        for (a0, b0), (a1, b1) in zip(knots, knots[1:]):
            if y <= a1:
                return b0 + (y - a0) * (b1 - b0) / (a1 - a0)
        return knots[-1][1] + (y - knots[-1][0])
    return f


def remap_y(shape, f):
    return [[tuple((x, f(y)) for x, y in s) for s in contour] for contour in shape]


def bold_outline(shape, kind, name="?", parts=None, shifts=None):
    """The Bold outline of a Regular shape (not yet positioned): the pen, then the heights put back, then the small loops the pen leaves at the notches (and the
    parts it ran into each other) cleaned away; if the cleaning cannot close an outline the glyph is kept as the pen made it and the checks say so.
    parts = (shapes, pens): for a glyph that is a union of parts the pen goes on each part (the pen of a "mark" is the thin one of the accents) and the
    emboldened parts are united, which is cleaner than a pen on the finished union."""
    out = None
    if parts:
        shifts = shifts or {}
        pieces = [embolden(geom.rnd(translate(p, *shifts.get(k, (0, 0))), min_len=14), *((EX, EY) if pen == "letter" else (MARK_PEN, MARK_PEN)))
                  for k, (p, pen) in enumerate(zip(*parts))]
        try:
            out = remap_y(geom.union(*pieces), zone_map(kind))
        except ValueError as error:
            print("bold: %s: the emboldened parts do not unite (%s); the pen goes on the finished union" % (name, error))
    if out is None:
        out = remap_y(embolden(geom.rnd(shape, min_len=14), EX, EY), zone_map(kind))     # (steps of a few units, which a union leaves, would turn into loops)
    try:
        return geom.clean(out)
    except ValueError as error:
        print("bold: %s not cleaned (%s)" % (name, error))
        return out


def bold_glyph(shape, adv, kind, lsb_factor=SB, name="?", parts=None, shifts=None):
    """(shape, advance) of the Bold glyph: positioned so that its sidebearings are the Regular's times lsb_factor."""
    if not shape:
        return shape, adv
    r = ink_bbox(shape)
    lsb0, rsb0 = r[0], adv - r[2]
    out = bold_outline(shape, kind, name, parts, shifts)
    b = ink_bbox(out)
    lsb1, rsb1 = round(lsb0 * lsb_factor), round(rsb0 * lsb_factor)
    out = translate(out, lsb1 - b[0], 0)
    return out, int(round(lsb1 + (b[2] - b[0]) + rsb1))


def bold_mark(shape, below=False):
    """An accent mark in the Bold: a thinner pen than the letters' (the strokes of the marks are 80 units), the edge the mark is placed by kept where it was (the
    lowest edge of a mark above a letter, the top edge of one that hangs below it), and what the pen added to the height squeezed back to 12%."""
    b0 = ink_bbox(shape)
    out = embolden(shape, MARK_PEN, MARK_PEN)
    b1 = ink_bbox(out)
    h0, h1 = b0[3] - b0[1], b1[3] - b1[1]
    if h1 > h0 * 1.12:
        k = (h0 * 1.12) / h1
        out = scale(out, 1.0, k, 0, b1[3] if below else b1[1])
        b1 = ink_bbox(out)
    return translate(out, 0, (b0[3] - b1[3]) if below else (b0[1] - b1[1]))


# ---------------------------------------------------------------------------------------------------------------------------------------- the whole face

BELOW_MARKS = {"commabelow", "cedilla", "ogonek"}
DOT_LIFT = 26                         # the dot of i and j: the pen shrank its gap to the stem to 39 units; raise it so that it stays one pixel clear at 13 px
SKIP_HELPERS = {"mk.cedilla", "mk.ogonek"}                  # (only the Regular uses them: the cedilla and ogonek letters are unioned there and emboldened here)
LETTER_SIGNS = {0xAA, 0xBA, 0x2122, 0xA9, 0xAE}
CUSTOM = {0xA5, 0x2264, 0x2265}                              # the yen and the two signs with a bar: made below from the Bold's own letters, with room kept between the parts
PART_SHIFT = {0xF0: {1: (0, -40)}, 0x127: {1: (0, -16)}}      # eth: the bar under the bent stem; h with a bar: the bar clear of the arch (the pen shrinks gaps by 60)              # (drawn again from the Bold's own letters: symbols.add_letter_signs)


def lift_dots(shape, dy=DOT_LIFT):
    out = []
    for contour in shape:
        b = ink_bbox([contour])
        out.append(translate([contour], 0, dy)[0] if b[1] > 560 and b[3] - b[1] < 220 and b[2] - b[0] < 220 else contour)
    return out


def build_bold(reg):
    """The Bold face (a glyphset.Face) made from the Regular face reg: every simple glyph by the pen, the small digits and the letter signs from the Bold's own
    letters, the composites placed again from the Bold's measures."""
    import glyphset
    import symbols
    from glyphset import Face, glyph_name, STANDALONE
    face = Face("Bold")
    cp_of = {}
    for cp, name in reg.cmap.items():
        cp_of.setdefault(name, cp)

    def kind_of(name):
        cp = cp_of.get(name)
        ch = chr(cp) if cp is not None else ""
        return "lower" if ch.isalpha() and ch.islower() else "upper"

    digits = {}
    for name in reg.names:
        if name in reg.comps or name.startswith("sm."):
            continue
        shape = reg.shape[name]
        cp = cp_of.get(name)
        if name == "space":
            face.add_shape(name, reg.adv[name], [])
        elif name.startswith("mk."):
            if name not in SKIP_HELPERS:
                face.add_shape(name, 0, rnd(bold_mark(shape, below=name[3:] in BELOW_MARKS), 6))
        elif cp in STANDALONE:                                 # the standalone spacing accents
            m = bold_mark(shape, below=STANDALONE[cp] in ("cedilla", "ogonek"))
            b = ink_bbox(m)
            w = int(round(b[2] - b[0] + 90))
            face.add_shape(name, w, rnd(translate(m, w / 2 - (b[0] + b[2]) / 2, 0), 6))
        elif cp in LETTER_SIGNS:
            continue
        else:
            if cp in CUSTOM:
                continue
            sh, adv = bold_glyph(shape, reg.adv[name], kind_of(name), name=name, parts=reg.parts.get(name), shifts=PART_SHIFT.get(cp))
            if name in ("i", "j", glyph_name(0x12F)):
                sh = lift_dots(sh)
            if name in "0123456789" and len(name) == 1:
                digits[name] = (sh, adv)
            else:
                face.add_shape(name, adv, rnd(sh, 6))
    width = max(adv for _, adv in digits.values())             # tabular digits: one advance, each digit centred in it
    for name, (sh, adv) in digits.items():
        b = ink_bbox(sh)
        face.add_shape(name, width, rnd(translate(sh, (width - (b[2] - b[0])) / 2 - b[0], 0), 6))
    face.adv[glyph_name(0x2007)] = width                       # the figure space is as wide as a digit, the punctuation space as a period
    face.adv[glyph_name(0x2008)] = face.adv[glyph_name(0x2E)]
    symbols.add_small_digits(face)
    symbols.add_letter_signs(face, bar=90, ring_inner=262, c_scale=0.6, r_scale=0.5)
    add_bar_signs(face)
    for cp, name in reg.cmap.items():
        face.cmap.setdefault(cp, name)
    face.base_of = dict(reg.base_of)
    glyphset.latin_composites(face)
    import cyrgreek
    cyrgreek.composites(face)                                  # (the letters of the two alphabets that are a letter with a mark; the others come with the pen)
    symbols.symbol_composites(face)
    return face


def measure_kerning(path, regular_kern):
    """The Bold's kerning, measured on its own outlines with tools/kernscan.py's rule (a pair is kerned by 55% of the excess of its optical gap over the median of
    its class, in steps of 2 units, at most 100). The pairs are those of the Regular (spacing.KERN); a pair the Regular loosens (rn, cl ...) keeps its value, and a
    pair whose gap is not far enough over the median for the rule keeps the Regular's value (the designer's small pairs: ii, ij, fa ...). path: a Bold font without kerning."""
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    import kernscan
    scan = kernscan.Scan(path)
    ref = scan.reference()
    out = {}
    for key, value in regular_kern.items():
        a, b = key[0], key[1]
        if value > 0:
            out[key] = value
            continue
        gap = scan.gap(a, b, False)
        excess = (gap - ref[scan.klass(a, b)]) if gap is not None else 0
        if excess >= kernscan.MIN_EXCESS:
            out[key] = -min(kernscan.MAX_KERN, int(round(kernscan.FRACTION * excess / 2.0)) * 2)
        else:
            out[key] = value
    return out


def add_bar_signs(face):
    """The yen sign and the less-than-or-equal and greater-than-or-equal signs of the Bold: their bars are drawn after the pen, with the room between the parts the pen
    would have eaten kept (the Regular's gaps are 45 and 63 units)."""
    from glyphset import glyph_name, stroke, hbar, scan
    Y = face.shape["Y"]
    run = scan(Y, 100)[0]
    face.add_shape(glyph_name(0xA5), face.adv["Y"], rnd(geom.union(Y, hbar(run[0] - 62, run[1] + 62, 250, 326), hbar(run[0] - 62, run[1] + 62, 108, 184)), 6), 0xA5)
    lt = face.shape["uni003C"]
    box = ink_bbox(lt)
    cx = (box[0] + box[2]) / 2
    le = translate(scale(lt, 1.0, 0.8, cx, 350), 0, 50)
    lbox = ink_bbox(le)
    bar = hbar(box[0], box[2], lbox[1] - 60 - 90, lbox[1] - 60)
    adv = face.adv["uni003C"]
    face.add_shape(glyph_name(0x2264), adv, rnd(le + bar, 6), 0x2264)
    face.add_shape(glyph_name(0x2265), adv, rnd(geom.mirror_x(le, adv / 2) + hbar(adv - box[2], adv - box[0], lbox[1] - 150, lbox[1] - 60), 6), 0x2265)
