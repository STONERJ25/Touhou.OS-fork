"""Original outlines for Latin-1 Supplement and Latin Extended-A: the accent marks, the special letters, the other symbols.

Same conventions as outlines.py: 1000 units to the em, y up, cap height 700, x-height 537, ascender 740, descender -210, paths with a leading "!" are counters.
Accent marks are drawn in "mark space": x = 0 is the centre of the mark and y = 0 its lowest edge, so that a recipe only has to say where the origin goes.
The marks follow the strokes of the existing letters: the grave is the ASCII grave of the face, the acute is its mirror, the circumflex is the apex of the A,
the dots are the dots of the colon, and the tails of the cedilla and the ogonek are the tail of the j. Strokes are 70 to 80 units, a little lighter than the
stems (96) and a little thinner than the horizontals (82), which is what accents want at 13 px.
"""
from geom import make_shape, mirror_x, mirror_y, scale, translate, transform
from ui_outlines import PUNCT, TICK, COMMA, dot, ring, rect, polygon

# ---------------------------------------------------------------------------------------------------------------------------------------- accent marks

GRAVE = "M-91 157 L11 157 L91 0 L12 0 Z"                                 # the ASCII grave, moved to mark space (157 high, 182 wide)
ACUTE = "M91 157 L-11 157 L-91 0 L-12 0 Z"
CIRCUMFLEX = "M-132 0 L-42 142 L42 142 L132 0 L44 0 L0 70 L-44 0 Z"
CARON = "M-132 142 L-44 142 L0 72 L44 142 L132 142 L42 0 L-42 0 Z"
BREVE = "M-125 105 Q-125 0 0 0 Q125 0 125 105 L55 105 Q55 70 0 70 Q-55 70 -55 105 Z"
MACRON = rect(-105, 0, 105, 70)
DOT_ABOVE = dot(0, 54, 54)
DIAERESIS = (dot(-88, 52, 52), dot(88, 52, 52))
DIAERESIS_NARROW = (dot(-68, 50, 50), dot(68, 50, 50))
RING_ABOVE = ring(0, 82, 82, 30)

# the tilde is the ASCII tilde squeezed to the width of an accent (x by 0.58): 266 wide, 145 high
_TILDE = ("M48 280 Q59 414 178 414 Q224 414 279 380 Q340 342 373 342 Q421 342 431 410 L507 404 Q496 269 373 269 Q326 269 271 304 Q212 342 179 342 Q132 342 124 274 Z")

# the tail of the j, shortened and scaled to 0.78: a stem 75 wide that bends to the left and ends in a flat cut (the cedilla), or its mirror image (the ogonek).
# Placed so that the top of the stem (the part that sits inside the letter) is at y = 30 and the centre of the stem at x = 0.
_J_TAIL = "M78 13 L174 13 L174 -57 Q174 -210 43 -210 Q6 -210 -25 -197 L-10 -118 Q9 -127 29 -127 Q78 -127 78 -58 Z"


def _tail():
    s = scale(make_shape(_J_TAIL), 0.78, 0.78, 126, 13)
    return translate(s, -126, 17)


def marks():
    """The accent marks as shapes in mark space, by name."""
    m = {}
    for name, paths in (("grave", GRAVE), ("acute", ACUTE), ("circumflex", CIRCUMFLEX), ("caron", CARON), ("breve", BREVE), ("macron", MACRON),
                        ("dotabove", DOT_ABOVE), ("ring", RING_ABOVE), ("diaeresis", DIAERESIS), ("diaeresis.narrow", DIAERESIS_NARROW)):
        m[name] = make_shape(*paths) if isinstance(paths, tuple) else make_shape(paths)
    tilde = make_shape(_TILDE)
    m["tilde"] = scale(translate(tilde, -277.5, -269), 0.58, 1.0)
    acute = m["acute"]
    m["doubleacute"] = [c for dx in (-68, 68) for c in translate(scale(acute, 0.88, 0.88, 0, 0), dx, 0)]
    m["cedilla"] = _tail()
    m["ogonek"] = mirror_x(m["cedilla"], 0)
    comma = make_shape(COMMA)                                              # the comma of the face, scaled to 0.62: the "comma below" of Latvian and Romanian type
    box = (57, -135, 180, 113)
    m["commabelow"] = translate(scale(comma, 0.62, 0.62, 118, 113), -118, -48 - 113)
    m["tick"] = translate(scale(make_shape(TICK), 1.0, 0.86, 114, 698), -114, -(698 - 0.86 * 200))   # the apostrophe of the face as the caron of d, l, t and L (172 high)
    quote_left = make_shape("M177 680 Q135 637 124 592 L169 592 L169 481 L54 481 L54 561 Q54 646 139 727 Z")
    m["turnedcomma"] = translate(scale(quote_left, 0.62, 0.62, 115, 481), -115, -481)           # above the g of the Latvian g with cedilla
    # the narrow variants for the dotless i and j (221 and 248 units wide)
    for name in ("circumflex", "caron", "tilde", "breve", "macron"):
        m[name + ".narrow"] = scale(m[name], 0.8, 0.9, 0, 0)
    return m


# ---------------------------------------------------------------------------------------------------------------------------------------- letters

DOTLESS_I = (221, "M62 0 L62 537 L158 537 L158 0 Z")
DOTLESS_J = (248, "M78 537 L174 537 L174 -57 Q174 -210 43 -210 Q6 -210 -25 -197 L-10 -118 Q9 -127 29 -127 Q78 -127 78 -58 Z")
KRA = (515, "M67 0 L67 537 L163 537 L163 302 L374 537 L487 537 L290 320 L504 0 L391 0 L227 250 L163 180 L163 0 Z")
LONG_S = (370, "M65 0 L65 578 Q65 752 232 752 Q283 752 324 730 L301 653 Q272 669 236 669 Q161 669 161 580 L161 0 Z")
ENG_LOWER = (570, "M67 0 L67 537 L151 537 L158 465 Q221 549 329 549 Q499 549 499 365 L499 -57 Q499 -210 368 -210 Q331 -210 300 -197 L315 -118 "
                  "Q334 -127 354 -127 Q403 -127 403 -58 L403 348 Q403 466 307 466 Q224 466 163 369 L163 0 Z")
ENG_UPPER = (702, "M73 0 L73 700 L172 700 L533 161 L533 700 L629 700 L629 -57 Q629 -210 498 -210 Q461 -210 430 -197 L445 -118 Q464 -127 484 -127 "
                  "Q529 -127 529 -58 L529 0 L169 538 L169 0 Z")
THORN_LOWER = (577, "M67 740 L163 740 L163 469 Q216 549 324 549 Q530 549 530 271 Q530 -12 315 -12 Q219 -12 163 62 L163 -210 L67 -210 Z",
               "!M163 254 Q163 70 295 70 Q431 70 431 271 Q431 467 299 467 Q163 467 163 280 Z")
THORN_UPPER = (607, "M73 0 L73 700 L169 700 L169 618 L326 618 Q569 618 569 410 Q569 196 322 196 L169 196 L169 0 Z",
               "!M169 278 L321 278 Q470 278 470 410 Q470 536 322 536 L169 536 Z")
SHARP_S = (600, "M67 0 L67 580 Q67 752 262 752 Q455 752 455 603 Q455 480 360 438 Q540 400 540 215 Q540 -12 315 -12 Q225 -12 165 70 L158 0 Z",
           "!M163 455 L270 455 Q358 455 358 562 Q358 670 262 670 Q163 670 163 585 Z",
           "!M163 250 Q163 70 295 70 Q440 70 440 221 Q440 373 322 373 L163 373 Z")
# the lower-case d is the mirror image of the b, and the eth is that d with its stem bent over to the left at the top (the crossbar is added by the recipe)
ETH_STEM = (577, "M426 0 L510 0 L510 670 Q510 752 430 752 L330 752 L330 668 L360 668 Q414 668 414 610 L414 469 Q361 549 253 549 Q47 549 47 271 Q47 -12 262 -12 Q360 -12 419 70 Z",
            "!M414 254 Q414 70 282 70 Q146 70 146 271 Q146 467 278 467 Q414 467 414 280 Z")
