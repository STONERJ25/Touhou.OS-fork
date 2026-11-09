"""Symbols for Gensokyo Sans: the Latin-1 signs (currency, copyright, fractions ...), typographic spaces, dashes and quotes, arrows, the geometric marks the
desktop uses, the mathematical signs of the font specification P0. Drawn in the style of the existing signs (+ = < > arrows, % & ...): straight cuts, strokes of
70 to 90 units, round parts as quadratics with a point at every extreme.

Everything here is built on the glyphs already in the face (a letter scaled into a sign, the arrow turned round, the % extended), so the weight stays that of the
face; the strokes that cross a letter (the bar of the cent sign, the two bars of the yen) are unioned into one outline. Advances of the currency signs and of the
mathematical signs are 555, the digits' width, as in the face's existing + = < >: they line up in tables.
"""
import math

import geom
from geom import make_shape, union, translate, scale, rotate180, mirror_x, mirror_y, transform, rnd, ink_bbox
from ui_outlines import PUNCT, TICK, COMMA, dot, ring, rect, polygon
from glyphset import glyph_name, stroke, hbar

QUOTE_RIGHT = PUNCT["’"][1]
SPACES = {0x2000: 500, 0x2001: 1000, 0x2002: 500, 0x2003: 1000, 0x2004: 333, 0x2005: 250, 0x2006: 167, 0x2007: 555, 0x2008: 241, 0x2009: 200, 0x200A: 100,
          0x202F: 200, 0x205F: 222, 0x3000: 1000}                                   # em, en, three-, four- and six-per-em, figure (a digit), punctuation (a period), thin, hair ...


def circle(cx, cy, r):
    """A circle as eight quadratics, with an on-curve point at every 45 degrees (so at the four extremes): within 0.03% of round."""
    k = r / math.cos(math.pi / 8)
    pts = [(cx + r * math.cos(i * math.pi / 4), cy + r * math.sin(i * math.pi / 4)) for i in range(8)]
    ctl = [(cx + k * math.cos((i + 0.5) * math.pi / 4), cy + k * math.sin((i + 0.5) * math.pi / 4)) for i in range(8)]
    parts = ["M%d %d" % (round(pts[0][0]), round(pts[0][1]))]
    for i in range(8):
        e = pts[(i + 1) % 8]
        parts.append("Q%d %d %d %d" % (round(ctl[i][0]), round(ctl[i][1]), round(e[0]), round(e[1])))
    return " ".join(parts) + " Z"


def ring_round(cx, cy, outer, inner):
    return circle(cx, cy, outer), "!" + circle(cx, cy, inner)


def star(cx, cy, outer, inner, points=5):
    pts = []
    for k in range(points * 2):
        r = outer if k % 2 == 0 else inner
        a = math.pi / 2 + k * math.pi / points
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return polygon(*pts)


def add_symbols(face, S):
    adv = face.adv
    shp = face.glyph_shape
    sh = make_shape

    def add(cp, w, shape, base=None):
        face.add_shape(glyph_name(cp), w, rnd(shape), cp)

    def alias(cp, of):
        face.cmap[cp] = of if of in face.adv else glyph_name(of)

    def centred(shape, w):
        box = ink_bbox(shape)
        return translate(shape, (w - (box[0] + box[2])) / 2, 0)

    # ----- inverted marks: the ! and ? turned half way round, about the middle of the capitals
    add(0xA1, adv["uni0021"], rotate180(shp("uni0021"), adv["uni0021"] / 2, 350))
    add(0xBF, adv["uni003F"], rotate180(shp("uni003F"), adv["uni003F"] / 2, 350))

    # ----- currency
    c_big = scale(shp("c"), 1.1, 1.1, 250, 268)
    add(0xA2, 555, union(translate(c_big, 22.5, 0), hbar(239, 315, -125, 665)))
    pound = ("M55 0 L55 83 L145 83 L145 262 L45 262 L45 342 L145 342 L145 520 Q145 712 300 712 Q380 712 437 664 L388 603 Q350 630 307 630 "
             "Q241 630 241 540 L241 342 L360 342 L360 262 L241 262 L241 83 L500 83 L500 0 Z")
    add(0xA3, 555, sh(pound))
    ring_cur = sh(*ring_round(277, 350, 165, 85))
    ears = [stroke((277 + 100 * sx, 350 + 100 * sy), (277 + 215 * sx, 350 + 215 * sy), 70) for sx in (-1, 1) for sy in (-1, 1)]
    add(0xA4, 555, union(ring_cur, *ears))
    add(0xA5, adv["Y"], union(shp("Y"), hbar(150, 485, 255, 317), hbar(150, 485, 148, 210)))
    add(0x20AC, adv["C"], union(shp("C"), hbar(14, 400, 395, 465), hbar(14, 400, 235, 305)))
    add(0xA6, adv["uni007C"], sh(rect(78, -135, 163, 255), rect(78, 355, 163, 745)))

    # ----- the section sign: two s stacked and joined
    s0 = scale(shp("s"), 1.12, 0.84, 237, 0)                                  # (flattened a little so that two of them fit between -150 and 740)
    add(0xA7, 560, centred(union(translate(s0, 0, -140), translate(s0, 0, 279)), 560))

    # ----- copyright, registered, the ordinal indicators and the trademark: signs that carry a small letter (below), and the degree sign
    add_letter_signs(face)
    add(0xB0, 300, sh(*ring_round(150, 560, 128, 55)))

    # ----- guillemets: chevrons 300 high, centred on the middle of the x-height
    def chevron(x0, right=False):
        pts = [(x0 + 220, 458), (x0, 268), (x0 + 220, 78), (x0 + 220, 174), (x0 + 118, 268), (x0 + 220, 362)]
        return sh(polygon(*pts)) if not right else mirror_x(sh(polygon(*pts)), x0 + 110)
    add(0xAB, 540, chevron(40) + chevron(280))
    add(0xBB, 540, chevron(40, True) + chevron(280, True))
    add(0x2039, 300, chevron(40))
    add(0x203A, 300, chevron(40, True))

    # ----- the other signs
    add(0xAC, 555, sh(polygon((50, 300), (50, 382), (505, 382), (505, 130), (423, 130), (423, 300))))
    plus = shp("uni002B")
    add(0xB1, 555, translate(plus, 0, 95) + sh(rect(38, 35, 517, 117)))
    add(0xD7, 555, union(stroke((100, 170), (455, 530), 76), stroke((100, 530), (455, 170), 76)))
    add(0xF7, 555, sh(rect(50, 310, 505, 390), dot(277, 530, 52), dot(277, 170, 52)))
    add(0xB7, adv["uni002E"], sh(dot(120, 290, 57)))
    micro = ("M68 537 L68 -210 L164 -210 L164 -12 L238 -12 Q342 -12 407 71 L415 0 L500 0 L500 537 L404 537 L404 162 "
             "Q343 70 260 70 Q164 70 164 183 L164 537 Z")
    add(0xB5, adv["u"], sh(micro))
    pilcrow = ("M272 0 L272 336 L232 336 Q60 336 60 518 Q60 700 232 700 L512 700 L512 0 L428 0 L428 336 L356 336 L356 0 Z", )
    add(0xB6, 570, sh(*pilcrow))

    add_small_digits(face)
    add(0x2044, 320, stroke((25, -30), (295, 720), 62))

    # ----- typographic spaces, dashes, quotes, daggers, primes, the euro and the trademark
    for cp, w in SPACES.items():
        face.add_shape(glyph_name(cp), w, [], cp)
    face.cmap[0xAD] = glyph_name(0x2D)                                     # soft hyphen and the hyphens are the hyphen
    face.cmap[0x2010] = glyph_name(0x2D)
    face.cmap[0x2011] = glyph_name(0x2D)
    add(0x2012, 555, sh(rect(40, 262, 515, 344)))
    add(0x2015, 1000, sh(rect(26, 262, 974, 344)))
    face.cmap[0x201A] = glyph_name(0x2C)
    face.cmap[0x201B] = glyph_name(0x2018)
    face.cmap[0x201F] = glyph_name(0x201C)
    comma = sh(COMMA)
    add(0x201E, 424, translate(comma, 30, 0) + translate(comma, 224, 0))
    face.cmap[0x2032] = glyph_name(0x27)
    face.cmap[0x2033] = glyph_name(0x22)
    add(0x2020, 555, sh(polygon((232, -80), (232, 480), (52, 480), (52, 562), (232, 562), (232, 720), (322, 720), (322, 562), (502, 562), (502, 480),
                                (322, 480), (322, -80))))
    add(0x2021, 555, sh(polygon((232, -80), (232, 150), (52, 150), (52, 232), (232, 232), (232, 480), (52, 480), (52, 562), (232, 562), (232, 720),
                                (322, 720), (322, 562), (502, 562), (502, 480), (322, 480), (322, 232), (502, 232), (502, 150), (322, 150), (322, -80))))
    pc = shp("uni0025")
    add(0x2030, 1170, pc + sh(*ring(975, 156, 145, 70)))

    # ----- arrows: the face has the left and right ones; up and down are the right one turned, the double ones are drawn out
    right = shp("uni2192")
    up = transform(right, (0, 1, -1, 0, 277.5 + 350, -10))
    add(0x2191, 555, up)
    add(0x2193, 555, mirror_y(up, 350))
    both = ("M46 350 L266 572 L324 513 L202 390 L769 390 L647 513 L705 572 L925 350 L705 128 L647 187 L769 310 L202 310 L324 187 L266 128 Z",)
    add(0x2194, 971, sh(*both))
    both_short = ("M46 350 L266 572 L324 513 L202 390 L569 390 L447 513 L505 572 L725 350 L505 128 L447 187 L569 310 L202 310 L324 187 L266 128 Z",)
    add(0x2195, 555, transform(sh(*both_short), (0, 1, -1, 0, 627.5, -35.5)))

    # ----- marks the desktop programs use
    add(0x2713, 700, union(stroke((105, 330), (262, 150), 84), stroke((262, 150), (598, 640), 84)))
    add(0x25A0, 700, sh(rect(90, 90, 610, 610)))
    add(0x25A1, 700, sh(rect(90, 90, 610, 610), "!" + rect(160, 160, 540, 540)))
    add(0x25AA, 520, sh(rect(130, 220, 390, 480)))
    add(0x25B2, 700, sh(polygon((80, 110), (350, 600), (620, 110))))
    add(0x25B3, 700, sh(polygon((80, 110), (350, 600), (620, 110)), "!" + polygon((200, 178), (350, 450), (500, 178))))
    add(0x25BC, 700, sh(polygon((80, 590), (620, 590), (350, 100))))
    add(0x25BD, 700, sh(polygon((80, 590), (620, 590), (350, 100)), "!" + polygon((200, 522), (500, 522), (350, 250))))
    add(0x25B8, 520, sh(polygon((105, 120), (105, 580), (440, 350))))
    add(0x25BE, 520, sh(polygon((60, 560), (460, 560), (260, 215))))
    add(0x25C6, 700, sh(polygon((350, 680), (610, 350), (350, 20), (90, 350))))
    add(0x25C7, 700, sh(polygon((350, 680), (610, 350), (350, 20), (90, 350)), "!" + polygon((350, 570), (500, 350), (350, 130), (200, 350))))
    add(0x25CF, 700, sh(circle(350, 350, 290)))
    add(0x25CB, 700, sh(*ring_round(350, 350, 290, 220)))
    add(0x25E6, 400, sh(*ring_round(200, 300, 110, 55)))
    add(0x2605, 740, sh(star(370, 330, 345, 140)))
    add(0x2606, 740, sh(star(370, 330, 345, 140), "!" + star(370, 330, 165, 70)))

    # ----- mathematical signs
    add(0x2260, 555, union(sh(rect(50, 212, 505, 292), rect(50, 409, 505, 489)), stroke((150, 90), (405, 610), 76)))
    le = translate(scale(shp("uni003C"), 1.0, 0.8, 277, 350), 0, 50)
    add(0x2264, 555, le + sh(rect(47, 60, 508, 140)))
    add(0x2265, 555, mirror_x(le, 277.5) + sh(rect(47, 60, 508, 140)))
    add(0x2248, 555, translate(shp("uni007E"), 0, 118) + translate(shp("uni007E"), 0, -102))
    add(0x221E, 760, infinity())
    return face


def infinity():
    """Two loops side by side: a horizontal figure eight made of two rings that overlap in the middle."""
    left = sh_ring(215, 280, 190, 150)
    right = sh_ring(545, 280, 190, 150)
    return union(left, right)


def sh_ring(cx, cy, rx, ry):
    """A ring 80 thick with horizontal radius rx and vertical radius ry (inner counter 80 less each way), as one shape."""
    def ell(rx, ry):
        return ("M%d %d Q%d %d %d %d Q%d %d %d %d Q%d %d %d %d Q%d %d %d %d Z" %
                (cx, cy + ry, cx + rx, cy + ry, cx + rx, cy, cx + rx, cy - ry, cx, cy - ry, cx - rx, cy - ry, cx - rx, cy, cx - rx, cy + ry, cx, cy + ry))
    return make_shape(ell(rx, ry), "!" + ell(rx - 80, ry - 70))


SUP_Y, SUB_Y = 270, -75                                                   # the small digits sit with their top at the cap height, or hang 75 units below the baseline


def symbol_composites(face):
    """Superscripts, subscripts, the fractions and the numero sign: the small digits, the fraction slash and the letters placed from the face's measures."""
    adv = face.adv
    for cp, d in ((0xB9, "1"), (0xB2, "2"), (0xB3, "3"), (0x2070, "0")) + tuple((0x2070 + int(k), k) for k in "456789"):
        face.add_comp(glyph_name(cp), adv["sm." + d] + 56, [("sm." + d, 28, SUP_Y)], cp)
    for k in "0123456789":
        face.add_comp(glyph_name(0x2080 + int(k)), adv["sm." + k] + 56, [("sm." + k, 28, SUB_Y)], 0x2080 + int(k))
    slash = glyph_name(0x2044)
    for cp, n, dn in ((0xBC, "1", "4"), (0xBD, "1", "2"), (0xBE, "3", "4")):
        sx = adv["sm." + n] - 84
        dx = sx + adv[slash] - 100
        face.add_comp(glyph_name(cp), dx + adv["sm." + dn] + 56, [("sm." + n, 10, SUP_Y), (slash, sx, 0), ("sm." + dn, dx, 0)], cp)
    off = adv["N"] - 12
    face.add_comp(glyph_name(0x2116), off + adv[glyph_name(0xBA)] + 22, [("N", 0, 0), (glyph_name(0xBA), off, 0)], 0x2116)


def add_small_digits(face):
    """The small digits for the superscripts, subscripts and fractions: 0.62 of the digits (which keeps the stems of the Regular at 59 units)."""
    for d in "0123456789":
        g = scale(face.glyph_shape(d), 0.62, 0.62, 0, 0)
        face.add_shape("sm." + d, int(round(face.adv[d] * 0.62)), g)


def add_letter_signs(face, bar=56, ring_inner=292, c_scale=0.64, r_scale=0.55):
    """The signs that carry a small copy of the face's own letters, so that they take the weight of the face: copyright and registered (a ring with a c or an
    R in it), the ordinal indicators (a small a or o with a bar under it) and the trademark (a small T and M). bar: thickness of the bar under the
    indicators; ring_inner: the inner radius of the ring (its outer radius is 345)."""
    shp = face.glyph_shape
    sh = make_shape

    def add(cp, w, shape):
        face.add_shape(glyph_name(cp), w, rnd(shape), cp)

    big_ring = sh(*ring_round(370, 350, 345, ring_inner))
    c_box = ink_bbox(shp("c"))
    cx, cy = (c_box[0] + c_box[2]) / 2, (c_box[1] + c_box[3]) / 2
    add(0xA9, 740, big_ring + translate(scale(shp("c"), c_scale, c_scale, cx, cy), 370 - cx, 350 - cy))
    r_box = ink_bbox(shp("R"))
    rx, ry = (r_box[0] + r_box[2]) / 2, (r_box[1] + r_box[3]) / 2
    add(0xAE, 740, big_ring + translate(scale(shp("R"), r_scale, r_scale, rx, ry), 370 - rx, 350 - ry))
    for cp, letter in ((0xAA, "a"), (0xBA, "o")):
        g = scale(shp(letter), 0.62, 0.62, 0, 0)
        box = ink_bbox(g)
        w = int(round(box[2] - box[0] + 50))
        g = translate(g, 25 - box[0], 365 - box[1])
        add(cp, w + 30, translate(g + hbar(10, w - 10, 338 - bar, 338), 15, 0))
    t, m = scale(shp("T"), 0.58, 0.58, 0, 0), scale(shp("M"), 0.58, 0.58, 0, 0)
    xm = 25 + int(face.adv["T"] * 0.58) + 25
    add(0x2122, xm + int(face.adv["M"] * 0.58) + 33, translate(t, 25, 294) + translate(m, xm, 294))
