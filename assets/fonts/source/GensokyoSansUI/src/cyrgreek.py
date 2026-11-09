"""Cyrillic and Greek for Gensokyo Sans (the Regular; the Bold and the Mono are made from it by bold.py and mono.py).

Drawn for the owner's face from its own letters and strokes, in the same measures: capitals 700 high, lower case 537 (round letters 12 units over), stems 96, horizontals
82 to 83, the flat terminals and the mitred corners of the Latin letters, quadratic curves with the control point at the corner of the box (the way the O is drawn).

What is shared with the Latin letters (the same glyph answers to several code points, so it has one outline and one set of kerning pairs): the capitals and small letters
that are the same picture in the three alphabets (A B E K M H O P C T X and a e o p c x in Cyrillic, A B E Z H I K M N O P T Y X and o in Greek), the Cyrillic
letters that are Latin ones (I J S and i j s, y for y, the kra for the small ka), the Greek gamma, pi, phi and tau that are the Cyrillic Г, П, Ф, ф and т, and the Greek eta,
iota, kappa, mu, nu and rho that are the Latin eng, dotless i, kra, micro sign, v and p. Ё ё Ї ї Ѐ ѐ are the Latin letters with a diaeresis or a grave.

Accents (tonos, dialytika, breve, acute, grave) are the face's own marks over the letter, as in the Latin blocks; the Greek capitals with a tonos carry it above the
letter and not in front of it (the usual form in text set in capitals and small letters, which this keeps simple).
"""
import unicodedata

import geom
from geom import make_shape, union, translate, scale, mirror_x, rnd, ink_bbox, transform, embolden, clip_box
from glyphset import glyph_name, stroke, MARK_OF, Y_ABOVE, anchor_x, NARROW_BASES
from ui_outlines import rect
import latin_outlines as L


# ---------------------------------------------------------------------------------------------------------------------------------------- helpers

def P(*paths):
    return make_shape(*paths)


def R(x0, y0, x1, y1):
    return make_shape(rect(x0, y0, x1, y1))


def ell(x0, y0, x1, y1):
    """The round outline of the face's letters (the O): four quadratic pieces, the control points at the corners of the box."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    return "M%g %g Q%g %g %g %g Q%g %g %g %g Q%g %g %g %g Q%g %g %g %g Z" % (cx, y0, x0, y0, x0, cy, x0, y1, cx, y1, x1, y1, x1, cy, x1, y0, cx, y0)


def ring(x0, y0, x1, y1, tx=100, ty=84):
    """An O-like ring: the outline and its counter (sides tx thick, top and bottom ty)."""
    return ell(x0, y0, x1, y1), "!" + ell(x0 + tx, y0 + ty, x1 - tx, y1 - ty)


def refit(shape, fx, fy, px, py):
    """The shape scaled by (fx, fy) with its strokes kept at their weight: eroded by the pen (px, py), scaled, dilated by the same pen again."""
    thin = embolden(rnd(shape, min_len=10), -px, -py)
    out = embolden(scale(thin, fx, fy, 0, 0), px, py)
    try:
        return geom.clean(out)
    except ValueError:
        return out


def fit_y(shape, y0, y1):
    """The shape stretched so that its ink spans exactly y0 to y1."""
    b = ink_bbox(shape)
    k = (y1 - y0) / (b[3] - b[1])
    return transform(shape, (1, 0, 0, k, 0, y0 - b[1] * k))


# ---------------------------------------------------------------------------------------------------------------------------------------- which code points share a glyph

CYR_ALIAS = {0x410: 0x41, 0x412: 0x42, 0x415: 0x45, 0x41A: 0x4B, 0x41C: 0x4D, 0x41D: 0x48, 0x41E: 0x4F, 0x420: 0x50, 0x421: 0x43, 0x422: 0x54, 0x425: 0x58,
             0x400: 0xC8, 0x401: 0xCB, 0x405: 0x53, 0x406: 0x49, 0x407: 0xCF, 0x408: 0x4A, 0x4C0: 0x49,
             0x430: 0x61, 0x435: 0x65, 0x43A: 0x138, 0x43E: 0x6F, 0x440: 0x70, 0x441: 0x63, 0x443: 0x79, 0x445: 0x78,
             0x450: 0xE8, 0x451: 0xEB, 0x455: 0x73, 0x456: 0x69, 0x457: 0xEF, 0x458: 0x6A, 0x45B: 0x127}
GREEK_ALIAS = {0x391: 0x41, 0x392: 0x42, 0x393: 0x413, 0x395: 0x45, 0x396: 0x5A, 0x397: 0x48, 0x399: 0x49, 0x39A: 0x4B, 0x39C: 0x4D, 0x39D: 0x4E, 0x39F: 0x4F,
               0x3A0: 0x41F, 0x3A1: 0x50, 0x3A4: 0x54, 0x3A5: 0x59, 0x3A6: 0x424, 0x3A7: 0x58,
               0x3B3: 0x79, 0x3B7: 0x14B, 0x3B9: 0x131, 0x3BA: 0x138, 0x3BC: 0xB5, 0x3BD: 0x76, 0x3BF: 0x6F, 0x3C1: 0x70, 0x3C4: 0x442, 0x3C6: 0x444,
               0x384: 0xB4, 0x37E: 0x3B, 0x387: 0xB7}


def alias(face, table):
    for cp, src in table.items():
        face.cmap[cp] = face.cmap[src]


def _adder(face):
    """new(cp, advance, shape, base, parts): a simple glyph for cp; base: the Latin letter whose kerning it inherits; parts: the pieces it is the union of."""
    def new(cp, a, shape, base=None, parts=None):
        face.add_shape(glyph_name(cp), a, rnd(shape, 6), cp)
        if base:
            face.base_of[glyph_name(cp)] = base
        if parts:
            face.parts[glyph_name(cp)] = (list(parts), ["letter"] * len(parts))

    def newu(cp, a, parts, base=None):
        new(cp, a, union(*parts), base, parts)

    def put(cp, a, *paths, base=None):
        new(cp, a, P(*paths), base)
    return new, newu, put


# ---------------------------------------------------------------------------------------------------------------------------------------- Cyrillic

def add_cyrillic(face):
    sh = face.glyph_shape
    adv = face.adv
    new, newu, put = _adder(face)
    alias(face, CYR_ALIAS)

    # --- capitals
    put(0x411, 630, "M73 0 L73 700 L531 700 L531 617 L169 617 L169 413 L335 413 Q590 413 590 205 Q590 0 335 0 Z",
        "!M169 83 L328 83 Q492 83 492 207 Q492 331 329 331 L169 331 Z")
    put(0x413, 546, "M73 0 L73 700 L523 700 L523 617 L169 617 L169 0 Z", base="F")
    put(0x490, 546, "M73 0 L73 700 L427 700 L427 790 L523 790 L523 617 L169 617 L169 0 Z")
    new(0x414, 704, translate(P("M0 -140 L96 -140 L96 0 L568 0 L568 -140 L664 -140 L664 83 L636 83 L636 700 L291 700 L59 83 L0 83 Z",
                                "!M165 83 L540 83 L540 617 L366 617 Z"), 20, 0))
    newu(0x404, adv["C"], [sh("C"), R(100, 309, 470, 391)], base="C")
    # Ж: the K and its mirror image share the stem; each arm overlaps the stem a little so that no two edges coincide
    arms = P("M149 331.2 L487 700 L608 700 L328 392 L626 0 L508 0 L264 324 L149 199.3 Z")
    newu(0x416, 1050, [translate(s, 404, 0) for s in (R(73, 0, 169, 700), arms, mirror_x(arms, 121))])
    new(0x417, 585, translate(sh("3"), 15, 0))
    new(0x418, adv["N"], mirror_x(sh("N"), adv["N"] / 2))
    put(0x41B, 698, "M17 0 L123 0 L355 617 L529 617 L529 0 L625 0 L625 700 L280 700 Z")
    put(0x41F, 700, "M73 0 L73 700 L627 700 L627 0 L531 0 L531 617 L169 617 L169 0 Z")
    # У: the long stroke runs from the upper right to the foot, the short one from the upper left meets it
    new(0x423, 650, translate(clip_box(union(stroke((570, 760), (130, -60), 96), stroke((21, 770), (355, 250), 96)), -100, 0, 800, 700), 20, 0), base="V")
    # Ф: a ring with the stem through it
    newu(0x424, 680, [P(*ring(60, 110, 620, 590)), R(292, 0, 388, 700)])
    put(0x426, 660, "M73 0 L73 700 L169 700 L169 83 L491 83 L491 700 L587 700 L587 -140 L491 -140 L491 0 Z")
    put(0x427, 700, "M73 700 L169 700 L169 458 Q169 372 261 372 L531 372 L531 700 L627 700 L627 0 L531 0 L531 290 L270 290 Q73 290 73 460 Z")
    put(0x428, 916, "M73 0 L73 700 L169 700 L169 83 L410 83 L410 700 L506 700 L506 83 L747 83 L747 700 L843 700 L843 0 Z")
    put(0x429, 916, "M73 0 L73 700 L169 700 L169 83 L410 83 L410 700 L506 700 L506 83 L747 83 L747 700 L843 700 L843 -140 L747 -140 L747 0 Z")
    put(0x42A, 690, "M22 700 L229 700 L229 413 L395 413 Q650 413 650 205 Q650 0 395 0 L133 0 L133 617 L22 617 Z",
        "!M229 83 L388 83 Q552 83 552 207 Q552 331 389 331 L229 331 Z")
    soft = ("M73 700 L169 700 L169 413 L335 413 Q590 413 590 205 Q590 0 335 0 L73 0 Z",
            "!M169 83 L328 83 Q492 83 492 207 Q492 331 329 331 L169 331 Z")
    put(0x42C, 630, *soft)
    new(0x42B, 860, P(*soft) + R(690, 0, 786, 700))
    # Э: a C turned the other way with the bar of the Є
    newu(0x42D, adv["C"], [mirror_x(sh("C"), adv["C"] / 2), R(240, 309, 560, 391)])
    # Ю: a stem, a bar, and an O
    newu(0x42E, 1043, [R(73, 0, 169, 700), R(140, 309, 360, 391), translate(sh("O"), 300, 0)])
    new(0x42F, adv["R"], mirror_x(sh("R"), adv["R"] / 2))
    # Ђ and Ћ: a bar over a stem and the arch of an n whose right leg ends at the baseline (Ћ) or goes below it with a hook (Ђ)
    put(0x40B, 650, "M22 700 L22 617 L73 617 L73 0 L169 0 L169 318 L300 318 Q460 318 460 190 L460 0 L556 0 L556 175 Q556 400 300 400 L169 400 L169 617 L330 617 L330 700 Z")
    put(0x402, 650, "M22 700 L22 617 L73 617 L73 0 L169 0 L169 318 L300 318 Q460 318 460 190 L460 -45 Q460 -127 394 -127 Q374 -127 355 -118 L340 -197 Q371 -210 408 -210 "
                    "Q556 -210 556 -57 L556 175 Q556 400 300 400 L169 400 L169 617 L330 617 L330 700 Z")
    put(0x409, 1086, "M17 0 L123 0 L355 617 L529 617 L529 0 L791 0 Q1046 0 1046 205 Q1046 413 791 413 L625 413 L625 700 L280 700 Z",
        "!M625 83 L784 83 Q948 83 948 207 Q948 331 785 331 L625 331 Z")
    put(0x40A, 1088, "M73 0 L73 700 L169 700 L169 405 L531 405 L531 700 L627 700 L627 413 L793 413 Q1048 413 1048 205 Q1048 0 793 0 L531 0 L531 322 L169 322 L169 0 Z",
        "!M627 83 L786 83 Q950 83 950 207 Q950 331 787 331 L627 331 Z")
    put(0x40F, 660, "M73 0 L282 0 L282 -140 L378 -140 L378 0 L587 0 L587 700 L491 700 L491 83 L169 83 L169 700 L73 700 Z")

    # --- small letters
    put(0x431, 577, "M67 740 L430 740 L430 658 L163 658 L163 469 Q216 549 324 549 Q530 549 530 271 Q530 -12 315 -12 Q217 -12 158 70 L151 0 L67 0 Z",
        "!M163 254 Q163 70 295 70 Q431 70 431 271 Q431 467 299 467 Q163 467 163 280 Z")
    put(0x432, 570, "M67 0 L67 537 L277 537 Q488 537 488 403 Q488 321 411 287 Q522 259 522 157 Q522 0 288 0 Z",
        "!M163 332 L262 332 Q388 332 388 394 Q388 455 262 455 L163 455 Z",
        "!M163 82 L275 82 Q424 82 424 160 Q424 244 275 244 L163 244 Z")
    put(0x433, 470, "M67 0 L67 537 L430 537 L430 455 L163 455 L163 0 Z")
    put(0x491, 470, "M67 0 L67 537 L334 537 L334 640 L430 640 L430 455 L163 455 L163 0 Z")
    new(0x434, 600, translate(P("M0 -125 L80 -125 L80 0 L461 0 L461 -125 L541 -125 L541 82 L513 82 L513 537 L237 537 L48 82 L0 82 Z",
                                "!M161 82 L417 82 L417 455 L316 455 Z"), 20, 0))
    newu(0x454, adv["c"], [sh("c"), R(90, 227, 400, 309)], base="c")
    zh_arms = P("M143 279.7 L374 537 L487 537 L290 320 L504 0 L391 0 L227 250 L143 158.1 Z")
    newu(0x436, 840, [translate(s, 300, 0) for s in (R(67, 0, 163, 537), zh_arms, mirror_x(zh_arms, 115))])
    new(0x437, 520, fit_y(translate(refit(sh("3"), 0.9, 0.761, 30, 30), 10, 0), -12, 549))
    put(0x438, 570, "M503 0 L503 537 L407 537 L163 165 L163 537 L67 537 L67 0 L163 0 L407 372 L407 0 Z")
    put(0x43B, 560, "M20 0 L120 0 L304 455 L417 455 L417 0 L513 0 L513 537 L237 537 Z")
    put(0x43C, 740, "M67 0 L67 537 L190 537 L370 210 L550 537 L673 537 L673 0 L577 0 L577 395 L412 100 L328 100 L163 395 L163 0 Z")
    put(0x43D, 570, "M67 0 L67 537 L163 537 L163 315 L407 315 L407 537 L503 537 L503 0 L407 0 L407 233 L163 233 L163 0 Z")
    put(0x43F, 570, "M67 0 L67 537 L503 537 L503 0 L407 0 L407 455 L163 455 L163 0 Z")
    put(0x442, 460, "M20 455 L20 537 L440 537 L440 455 L278 455 L278 0 L182 0 L182 455 Z")
    newu(0x444, 560, [P(*ring(40, -12, 520, 549, 100, 82)), R(232, -210, 328, 740)])
    put(0x446, 560, "M67 0 L67 537 L163 537 L163 82 L407 82 L407 537 L503 537 L503 -140 L407 -140 L407 0 Z")
    put(0x447, 570, "M67 537 L163 537 L163 360 Q163 289 245 289 L407 289 L407 537 L503 537 L503 0 L407 0 L407 207 L255 207 Q67 207 67 380 Z")
    put(0x448, 838, "M67 0 L67 537 L163 537 L163 82 L371 82 L371 537 L467 537 L467 82 L675 82 L675 537 L771 537 L771 0 Z")
    put(0x449, 838, "M67 0 L67 537 L163 537 L163 82 L371 82 L371 537 L467 537 L467 82 L675 82 L675 537 L771 537 L771 -140 L675 -140 L675 0 Z")
    put(0x44A, 590, "M22 537 L208 537 L208 330 L355 330 Q560 330 560 165 Q560 0 355 0 L112 0 L112 455 L22 455 Z",
        "!M208 82 L345 82 Q462 82 462 166 Q462 248 345 248 L208 248 Z")
    soft_small = ("M67 537 L163 537 L163 330 L310 330 Q515 330 515 165 Q515 0 310 0 L67 0 Z",
                  "!M163 82 L300 82 Q417 82 417 166 Q417 248 300 248 L163 248 Z")
    put(0x44C, 560, *soft_small)
    new(0x44B, 735, P(*soft_small) + R(590, 0, 686, 537))
    newu(0x44D, adv["c"], [mirror_x(sh("c"), adv["c"] / 2), R(105, 227, 420, 309)])
    newu(0x44E, 792, [R(67, 0, 163, 537), R(140, 227, 300, 309), translate(sh("o"), 235, 0)])
    put(0x44F, 560, "M503 0 L407 0 L407 195 L312 195 L156 0 L40 0 L205 213 Q70 250 70 369 Q70 537 260 537 L503 537 Z",
        "!M407 280 L275 280 Q165 280 165 372 Q165 455 275 455 L407 455 Z")
    eng = L.ENG_LOWER[1].replace("L67 537 L151 537 L158 465 Q221 549", "L67 740 L163 740 L163 466 Q228 549")
    assert eng != L.ENG_LOWER[1]
    newu(0x452, 570, [P(eng), R(18, 600, 330, 666)])
    put(0x459, 912, "M20 0 L120 0 L304 455 L417 455 L417 0 L660 0 Q865 0 865 165 Q865 330 660 330 L513 330 L513 537 L237 537 Z",
        "!M513 82 L650 82 Q767 82 767 166 Q767 248 650 248 L513 248 Z")
    put(0x45A, 902, "M67 0 L67 537 L163 537 L163 315 L407 315 L407 537 L503 537 L503 330 L650 330 Q855 330 855 165 Q855 0 650 0 L407 0 L407 233 L163 233 L163 0 Z",
        "!M503 82 L640 82 Q757 82 757 166 Q757 248 640 248 L503 248 Z")
    put(0x45F, 570, "M67 0 L237 0 L237 -140 L333 -140 L333 0 L503 0 L503 537 L407 537 L407 82 L163 82 L163 537 L67 537 Z")


# ---------------------------------------------------------------------------------------------------------------------------------------- Greek

def add_greek(face):
    sh = face.glyph_shape
    adv = face.adv
    new, newu, put = _adder(face)
    alias(face, GREEK_ALIAS)

    # --- capitals
    put(0x394, 663, "M17 0 L280 700 L386 700 L646 0 Z", "!M148 83 L515 83 L331 597 Z", base="A")
    newu(0x398, adv["O"], [sh("O"), R(120, 309, 623, 391)], base="O")
    put(0x39B, 663, "M17 0 L280 700 L386 700 L646 0 L542 0 L331 597 L118 0 Z", base="A")
    new(0x39E, 609, R(60, 617, 549, 700) + R(130, 322, 479, 404) + R(60, 0, 549, 83))
    put(0x3A3, 600, "M40 0 L40 66 L240 350 L40 634 L40 700 L548 700 L548 617 L168 617 L360 350 L168 83 L558 83 L558 0 Z")
    newu(0x3A8, adv["U"], [sh("U"), R(300, 0, 396, 700)])
    put(0x3A9, 760, "M14 0 L300 0 Q150 170 150 330 Q150 628 380 628 Q610 628 610 330 Q610 170 460 0 L746 0 L746 83 L620 83 Q700 200 700 350 Q700 712 380 712 Q60 712 60 350 Q60 200 140 83 L14 83 Z")

    # --- small letters
    newu(0x3B1, 540, [P(*ring(40, -12, 480, 549, 100, 82)), R(392, 0, 488, 537)])
    put(0x3B2, 600, L.SHARP_S[1].replace(" L158 0 Z", " L163 -210 L67 -210 Z"), *L.SHARP_S[2:])
    newu(0x3B4, 550, [P(*ring(50, -12, 500, 500, 100, 82)), P("M160 440 L160 740 L410 740 L410 658 L256 658 L256 440 Z")])
    new(0x3B5, 520, mirror_x(sh(glyph_name(0x437)), 260))
    newu(0x3B8, 540, [P(*ring(60, -12, 480, 740, 96, 84)), R(130, 323, 410, 405)])
    # λ: the long stroke from the upper left to the foot, the short leg from its middle to the lower left
    lam = clip_box(union(stroke((40, 760), (460, -20), 96), stroke((255, 410), (40, -20), 96)), -100, 0, 800, 740)
    new(0x3BB, 520, translate(lam, 25, 0))
    newu(0x3C0, 550, [R(30, 455, 520, 537), R(110, 0, 206, 500), R(344, 0, 440, 500)])
    newu(0x3C3, 590, [P(*ring(45, -12, 470, 549, 100, 82)), R(250, 468, 560, 550)])
    newu(0x3C2, 480, [sh("c"), R(90, 468, 470, 550), R(380, -120, 476, 110)])
    ups = "M68 537 L164 537 L164 190 Q164 70 284 70 Q404 70 404 190 L404 537 L500 537 L500 188 Q500 -12 284 -12 Q68 -12 68 188 Z"
    put(0x3C5, 568, ups)
    chi = union(clip_box(stroke((5, 560), (475, -230), 96), -100, -210, 800, 537), clip_box(stroke((505, 560), (35, -30), 96), -100, 0, 800, 537))
    new(0x3C7, 540, translate(chi, 20, 0))
    newu(0x3C8, 570, [P(ups), R(236, -210, 332, 740)])
    w1 = P("M40 537 L136 537 L136 190 Q136 70 232 70 Q328 70 328 190 L328 420 L424 420 L424 188 Q424 -12 232 -12 Q40 -12 40 188 Z")
    w2 = P("M316 421 L412 421 L412 190 Q412 70 514 70 Q616 70 616 190 L616 537 L712 537 L712 188 Q712 -12 514 -12 Q316 -12 316 188 Z")
    newu(0x3C9, 752, [w1, w2])
    # ζ and ξ: the 'c' of the face under a bar, with a tail that hooks back under it; the xi has a second, smaller bowl above the first
    c = sh("c")
    tail = P("M364 110 L364 -128 L250 -128 L250 -210 L460 -210 L460 110 Z")
    newu(0x3B6, 480, [c, R(90, 468, 470, 550), tail])
    newu(0x3BE, 480, [c, R(90, 468, 470, 550), tail, R(105, 227, 340, 309)])


# ---------------------------------------------------------------------------------------------------------------------------------------- accents

def composites(face):
    """The letters with a mark: Cyrillic й ў ѓ ќ ѝ and their capitals, the Greek tonos and dialytika letters (the three with both marks, ΐ ΰ and the standalone
    dialytika tonos, are stacked by hand). The base is looked up through the cmap, so a shared glyph (Greek ι is the dotless i) takes the mark of its own size."""
    adv = face.adv
    for cp in list(range(0x370, 0x400)) + list(range(0x400, 0x460)) + [0x490, 0x491]:
        if face.cmap.get(cp) in face.adv or cp == 0x385:        # (a glyph of its own or a shared one; the Bold copies the Regular's cmap before the composites are made)
            continue
        src = CYR_ALIAS.get(cp) or GREEK_ALIAS.get(cp)
        if src is not None and face.cmap.get(src) in face.adv:   # (Ё ё Ї ї Ѐ ѐ: the Latin letter with the mark, which the Mono made on its own letters just before)
            face.cmap[cp] = face.cmap[src]
            continue
        d = unicodedata.normalize("NFD", chr(cp))
        if len(d) == 3 and ord(d[1]) == 0x308 and ord(d[2]) == 0x301 and ord(d[0]) in face.cmap:                    # ΐ ΰ
            base = face.cmap[ord(d[0])]
            ax = round(anchor_x(face, base))
            dia = "diaeresis.narrow" if base in NARROW_BASES else "diaeresis"
            face.add_comp(glyph_name(cp), adv[base], [(base, 0, 0), ("mk." + dia, ax, Y_ABOVE["lower"]), ("mk.acute", ax, Y_ABOVE["lower"] + 128)], cp)
            continue
        if len(d) != 2 or ord(d[1]) not in MARK_OF or ord(d[0]) not in face.cmap:
            continue
        letter, mark = d[0], MARK_OF[ord(d[1])]
        base = face.cmap[ord(letter)]
        if base in ("i", "j"):                                  # і and ј (the Latin i and j) lose their dot under a mark, as in the Latin blocks
            base = face.cmap[0x131 if base == "i" else 0x237]
        mname = mark
        if base in NARROW_BASES:
            mname = "diaeresis.narrow" if mark == "diaeresis" else mark + ".narrow" if mark in ("circumflex", "caron", "tilde", "breve", "macron") else mark
        cls = "upper" if letter.isupper() else "lower"
        face.add_comp(glyph_name(cp), adv[base], [(base, 0, 0), ("mk." + mname, round(anchor_x(face, base)), Y_ABOVE[cls])], cp)
        root = base if len(base) == 1 else face.base_of.get(base)
        if root:
            face.base_of[glyph_name(cp)] = root
    # the standalone dialytika tonos: the diaeresis with the tonos on top
    if glyph_name(0x385) not in face.adv:
        dia, acute = face.shape["mk.diaeresis"], face.shape["mk.acute"]
        w = 270
        face.add_shape(glyph_name(0x385), w, rnd(translate(dia, w / 2, Y_ABOVE["lower"]) + translate(acute, w / 2, Y_ABOVE["lower"] + 128)), 0x385)


def add_all(face):
    add_cyrillic(face)
    add_greek(face)
    composites(face)
