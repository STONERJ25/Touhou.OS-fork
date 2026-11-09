"""The glyph set of Gensokyo Sans: the 107 original glyphs untouched, and everything built on them (accented letters as composites, fused and barred
letters as single outlines made with geom.union). A Face holds the set for one design (Regular now; Bold and Mono are made from it in other modules).

Accented letters follow the font's own letters: each is the base letter (the dotless i and j where the dot would collide) with an accent mark placed by an
offset, so that the stems, the spacing and the kerning of the base are the letter's own. The marks that must touch the letter (cedilla on c C s S t T,
ogonek, the strokes of the barred letters) are unioned into one outline instead, so that no contour overlaps another (the font specification, outline rule 1).
"""
import unicodedata

import geom
from geom import make_shape, union, translate, scale, mirror_x, mirror_y, rotate180, rnd, ink_bbox, transform, flatten
import latin_outlines as L
from outlines import DIGITS, LOWERCASE, NOTDEF
from ui_outlines import LOWER_ALTERNATES, PUNCT, UPPERCASE, rect, polygon, dot

# the vertical placement of marks above a letter (y of the mark's lowest edge), by the class of the base
Y_ABOVE = {"lower": 615, "upper": 780, "tall": 800}
ASCENDERS = set("bdfhklt")


def glyph_name(cp):
    ch = chr(cp)
    return ch if ch.isascii() and ch.isalnum() else "uni%04X" % cp


class Face:
    def __init__(self, style):
        self.style = style
        self.names = []                  # glyph order
        self.adv = {}
        self.shape = {}                  # every simple glyph (legacy ones too, for the geometry)
        self.legacy = {}                 # name -> (outline tuple, mirrored): emitted through the original code path, byte for byte
        self.out = {}                    # simple glyphs whose shape is already final (integer points): name -> shape
        self.comps = {}                  # composite glyphs: name -> [(component, dx, dy)]
        self.parts = {}                  # glyphs made by a union: name -> (the parts, the pen of each part: "letter" or "mark"), so that the Bold can unite them after the pen
        self.cmap = {}
        self.base_of = {}                # name -> the ASCII letter whose kerning a variant inherits

    def _new(self, name, adv):
        if name in self.adv:
            raise KeyError("glyph %s twice" % name)
        self.names.append(name)
        self.adv[name] = adv

    def add_legacy(self, name, outline, mirrored=False):
        self._new(name, outline[0])
        self.legacy[name] = (outline, mirrored)
        shape = make_shape(*outline[1:])
        self.shape[name] = geom.mirror_x(shape, outline[0] / 2) if mirrored else shape

    def add_shape(self, name, adv, shape, cp=None):
        self._new(name, adv)
        self.shape[name] = shape
        if cp is not None:
            self.cmap[cp] = name

    def add_comp(self, name, adv, parts, cp=None):
        self._new(name, adv)
        self.comps[name] = parts
        if cp is not None:
            self.cmap[cp] = name

    def fuse(self, name, cp, letter, base, mark, x):
        """The letter `base` with its cedilla or ogonek (mark: 'cedilla' or 'ogonek') attached at x, as one outline: the top edge of the mark is put well inside the
        stroke of the letter, at its lowest point there."""
        base_shape = self.glyph_shape(base)
        mk = self.shape["mk." + mark]
        low, thick = lowest_ink(base_shape, x)
        top = low + min(36.0, 0.45 * thick)
        parts = [base_shape, translate(mk, x, top - 30)]
        self.add_shape(name, self.adv[base], rnd(union(*parts)), cp)
        self.base_of[name] = letter
        self.parts[name] = (parts, ["letter", "mark"])

    def glyph_shape(self, name):
        """The ink of a glyph in its own coordinates (a composite is the union of its parts' contours; they do not overlap)."""
        if name in self.shape:
            return self.shape[name]
        out = []
        for part, dx, dy in self.comps[name]:
            out.extend(translate(self.glyph_shape(part), dx, dy))
        return out

    def bbox(self, name):
        return ink_bbox(self.glyph_shape(name))


def regular_outlines(space_advance):
    """The (name, outline, mirrored) of the 107 glyphs in the order of the original build, so that their numbers stay the same."""
    outlines = DIGITS | LOWERCASE | LOWER_ALTERNATES | UPPERCASE | PUNCT
    out = [(".notdef", NOTDEF, False), ("space", (space_advance,), False)]
    for ch, outline in outlines.items():
        name = glyph_name(ord(ch))
        out.append((name, outline, name in "dq"))
    return out


# ---------------------------------------------------------------------------------------------------------------------------------------- helpers

def scan(shape, y):
    """The x ranges of the ink at height y (even-odd over the flattened contours, which is right for a clean outline)."""
    xs = []
    for contour in shape:
        pts = flatten(contour, 24)
        for i in range(len(pts)):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
    xs.sort()
    return [(xs[i], xs[i + 1]) for i in range(0, len(xs) - 1, 2)]


def lowest_ink(shape, x):
    """The lowest y of the ink at column x, and the thickness of the first stroke above it."""
    lows = []
    for contour in shape:
        pts = flatten(contour, 24)
        for i in range(len(pts)):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
            if (x0 <= x < x1) or (x1 <= x < x0):
                lows.append(y0 + (x - x0) * (y1 - y0) / (x1 - x0))
    lows.sort()
    return lows[0], (lows[1] - lows[0]) if len(lows) > 1 else 80


def center_x(face, name):
    box = face.bbox(name)
    return (box[0] + box[2]) / 2


# the letters whose accent belongs over the stem and not over the middle of the letter (the l and the J hang off to one side)
STEM_ANCHOR = {"l": 0.5, "L": 0.5, "uni0237": 0.5, "J": 0.5}


def anchor_x(face, name):
    if name in STEM_ANCHOR:
        box = face.bbox(name)
        y = 400 if name != "J" else 500
        runs = scan(face.glyph_shape(name), y)
        run = runs[-1] if name in ("J", "uni0237") else runs[0]
        return (run[0] + run[1]) / 2
    return center_x(face, name)


MARK_OF = {0x300: "grave", 0x301: "acute", 0x302: "circumflex", 0x303: "tilde", 0x304: "macron", 0x306: "breve", 0x307: "dotabove", 0x308: "diaeresis",
           0x30A: "ring", 0x30B: "doubleacute", 0x30C: "caron", 0x326: "commabelow", 0x327: "cedilla", 0x328: "ogonek"}
NARROW_BASES = {"uni0131", "uni0237"}                      # the dotless i and j are 221 and 248 wide
STANDALONE = {0xB4: "acute", 0xA8: "diaeresis", 0xAF: "macron", 0xB8: "cedilla", 0x2C6: "circumflex", 0x2C7: "caron", 0x2D8: "breve", 0x2D9: "dotabove",
              0x2DA: "ring", 0x2DB: "ogonek", 0x2DC: "tilde", 0x2DD: "doubleacute"}


def glyph_order(face):
    """The original glyphs first (their numbers do not change), then the helper glyphs the composites are made of, then the rest by code point."""
    cp_of = {name: cp for cp, name in face.cmap.items()}
    legacy = [n for n in face.names if n in face.legacy]
    helpers = [n for n in face.names if n.startswith(("mk.", "sm."))]
    rest = sorted((n for n in face.names if n not in face.legacy and not n.startswith(("mk.", "sm."))), key=lambda n: cp_of.get(n, 1 << 20))
    return legacy + helpers + rest


def build_regular(space_advance=264):
    face = Face("Regular")
    for name, outline, mirrored in regular_outlines(space_advance):
        face.add_legacy(name, outline, mirrored)
    for ch in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz":
        face.cmap[ord(ch)] = ch
        face.base_of[ch] = ch
    for cp in list(range(0x20, 0x7F)) + [0xA0, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2192, 0x2212]:
        face.cmap[cp] = "space" if cp in (0x20, 0xA0) else glyph_name(cp)

    marks = L.marks()
    for mname, shape in marks.items():                        # helper glyphs, not in the cmap: the marks in mark space (see latin_outlines.py)
        face.add_shape("mk." + mname, 0, shape)

    # ----- letters that are drawn once and have no decomposition
    def simple(cp, adv, *paths, base=None):
        face.add_shape(glyph_name(cp), adv, make_shape(*paths), cp)
        if base:
            face.base_of[glyph_name(cp)] = base

    simple(0x131, *L.DOTLESS_I, base="i")
    simple(0x237, *L.DOTLESS_J, base="j")
    simple(0x138, *L.KRA)
    simple(0x17F, *L.LONG_S)
    simple(0x14B, *L.ENG_LOWER, base="n")
    simple(0x14A, *L.ENG_UPPER, base="N")
    simple(0xFE, *L.THORN_LOWER, base="p")
    simple(0xDE, *L.THORN_UPPER, base="P")
    simple(0xDF, *L.SHARP_S)
    latin_simple(face, STROKE_REGULAR)
    import symbols                                            # (symbols builds on glyphset, so it is imported here)
    symbols.add_symbols(face, STROKE_REGULAR)
    latin_composites(face)
    symbols.symbol_composites(face)
    import cyrgreek                                           # (cyrgreek builds on glyphset too)
    cyrgreek.add_all(face)
    return face


# ---------------------------------------------------------------------------------------------------------------------------------------- recipes

STROKE_REGULAR = {"bar": 70, "slash": 70, "bar_small": 66}          # thickness of the strokes through barred letters (Regular)


def stroke(p0, p1, w):
    """A straight bar of thickness w from p0 to p1 with flat ends square to its direction."""
    (x0, y0), (x1, y1) = p0, p1
    dx, dy = x1 - x0, y1 - y0
    n = (dx * dx + dy * dy) ** 0.5
    ux, uy = -dy / n * w / 2, dx / n * w / 2
    return make_shape(polygon((x0 + ux, y0 + uy), (x1 + ux, y1 + uy), (x1 - ux, y1 - uy), (x0 - ux, y0 - uy)))


def hbar(x0, x1, y0, y1):
    return make_shape(rect(x0, y0, x1, y1))


def place(shape, x, y):
    return translate(shape, x, y)


def latin_simple(face, S):
    """The Latin-1 and Latin Extended-A glyphs that are outlines of their own (Regular only: the Bold is made from these by bold.py): the letters and
    marks that touch their letter (unioned into one outline), the barred and ligature letters, the standalone spacing accents. S: stroke thicknesses."""
    shape = face.glyph_shape
    adv = face.adv

    def fused(cp, letter, base, mark, x):
        """Base letter with a mark that touches it (cedilla, ogonek) as one outline."""
        face.fuse(glyph_name(cp), cp, letter, base, mark, x)

    FUSE_X = {  # where an ogonek is attached: the x of the middle of its stem (cedillas hang from the middle of the letter, see below)
        ("a", "ogonek"): 428, ("A", "ogonek"): 594, ("e", "ogonek"): 405, ("E", "ogonek"): 470, ("i", "ogonek"): 110, ("I", "ogonek"): 169,
        ("u", "ogonek"): 458, ("U", "ogonek"): 560,
    }
    for cp in list(range(0xC0, 0x100)) + list(range(0x100, 0x180)):
        d = unicodedata.normalize("NFD", chr(cp))
        if len(d) != 2 or ord(d[1]) not in MARK_OF:
            continue
        letter, mark = d[0], MARK_OF[ord(d[1])]
        base = glyph_name(ord(letter))
        if mark == "cedilla" and letter not in COMMA_BELOW:
            x = 232 if letter == "t" else anchor_x(face, base) + (20 if letter == "T" else 38)
            fused(cp, letter, base, "cedilla", x)
        elif mark == "ogonek":
            fused(cp, letter, base, "ogonek", FUSE_X[(letter, "ogonek")])

    # ---- the letters that have no decomposition, made from the face's own letters
    O, o, a, e, E, D, d_, Ls, ls, h, T, t = (shape(x) for x in ("O", "o", "a", "e", "E", "D", "d", "L", "l", "h", "T", "t"))
    bar, slash, small = S["bar"], S["slash"], S["bar_small"]

    def add(cp, adv_, sh, base=None):
        face.add_shape(glyph_name(cp), adv_, rnd(sh), cp)
        if base:
            face.base_of[glyph_name(cp)] = base

    def addu(cp, adv_, parts, base=None):
        """A glyph that is the union of its parts (the parts are kept: the Bold puts the pen on each of them, then unites them)."""
        add(cp, adv_, union(*parts), base)
        face.parts[glyph_name(cp)] = (list(parts), ["letter"] * len(parts))

    ob, lb = ink_bbox(O), ink_bbox(o)                                       # (the strokes of the slashed letters reach a little beyond the letter)
    addu(0xD8, adv["O"], [O, stroke((ob[0] - 24, ob[1] - 34), (ob[2] + 24, ob[3] + 34), slash)], "O")
    addu(0xF8, adv["o"], [o, stroke((lb[0] - 24, lb[1] - 34), (lb[2] + 24, lb[3] + 34), slash)], "o")
    addu(0x141, adv["L"], [Ls, stroke((14, 288), (262, 430), slash)], "L")
    addu(0x142, adv["l"], [ls, stroke((10, 262), (232, 392), small)], "l")
    addu(0x110, adv["D"] + 36, [translate(D, 36, 0), hbar(8, 330, 316, 316 + bar)])
    face.cmap[0xD0] = glyph_name(0x110)                                  # the eth capital Ð is the same letter as the Croatian Đ
    addu(0x111, adv["d"], [d_, hbar(350, 560, 586, 586 + small)], "d")
    addu(0x126, adv["H"], [shape("H"), hbar(18, 682, 508, 508 + bar)], "H")
    addu(0x127, adv["h"], [h, hbar(18, 222, 584, 584 + small)], "h")
    addu(0x166, adv["T"], [T, hbar(150, 420, 262, 262 + bar)], "T")
    addu(0x167, adv["t"], [t, hbar(45, 262, 222, 222 + small)], "t")
    addu(0xF0, adv["d"], [make_shape(*L.ETH_STEM[1:]), hbar(330, 570, 560, 560 + small)])
    # ligatures: the second letter's left stroke is laid over the first letter's right stroke
    addu(0xE6, 330 + adv["e"], [a, translate(e, 330, 0)])
    addu(0x153, 367 + adv["e"], [o, translate(e, 367, 0)])
    addu(0x152, 528 + adv["E"], [O, translate(E, 528, 0)])
    # Ae: the left leg of the A runs up into the top bar of an E whose stem stands 60 units further right than the E's, so that the A keeps a counter
    ae = ("M17 0 L280 700 L981 700 L981 617 L619 617 L619 404 L933 404 L933 322 L619 322 L619 83 L987 83 L987 0 L523 0 L523 322 L233 322 L118 0 Z",
          "!M262 404 L338 617 L523 617 L523 404 Z")
    add(0xC6, 1029, make_shape(*ae))
    # the standalone spacing accents
    for cp, mname in STANDALONE.items():
        sh = face.shape["mk." + mname]
        box = ink_bbox(sh)
        w = int(round(box[2] - box[0] + 90))
        dy = 0 if mname in ("cedilla", "ogonek") else Y_ABOVE["lower"]
        add(cp, w, translate(sh, w / 2, dy))
    face.add_shape("mk.dot", 0, make_shape(dot(0, 0, 57)))


COMMA_BELOW = set("gGkKlLnNrR")


def latin_composites(face):
    """Everything in the Latin-1 and Latin Extended-A blocks that is a letter with something beside it: the accents above, the comma below, the apostrophe
    of d' l' t' L', the dot of the l with a middle dot, the ligatures made of two letters. Every number is measured on the face's own glyphs (the stem of the
    l, the right edge of the d), so the same code places them in the Regular and in the Bold."""
    adv = face.adv
    shape = face.glyph_shape
    tick = ink_bbox(face.shape["mk.tick"])
    dot_box = ink_bbox(face.shape["mk.dot"])

    def comp(cp, base, mark, letter):
        """Base letter with an accent mark above it."""
        mname = mark
        if base in NARROW_BASES:
            mname = "diaeresis.narrow" if mark == "diaeresis" else mark + ".narrow" if mark in ("circumflex", "caron", "tilde", "breve", "macron") else mark
        cls = "upper" if letter.isupper() else "tall" if letter in ASCENDERS else "lower"
        face.add_comp(glyph_name(cp), adv[base], [(base, 0, 0), ("mk." + mname, round(anchor_x(face, base)), Y_ABOVE[cls])], cp)
        face.base_of[glyph_name(cp)] = letter

    for cp in list(range(0xC0, 0x100)) + list(range(0x100, 0x180)) + list(range(0x218, 0x21C)):             # (U+0218 to U+021B: the Romanian s and t with a comma below)
        d = unicodedata.normalize("NFD", chr(cp))
        if len(d) != 2 or ord(d[1]) not in MARK_OF:
            continue
        letter, mark = d[0], MARK_OF[ord(d[1])]
        base = glyph_name(ord(letter))
        if mark == "ogonek" or (mark == "cedilla" and letter not in COMMA_BELOW):
            continue                                                     # (one outline with the letter: latin_simple)
        if mark in ("cedilla", "commabelow"):
            if letter == "g":                                            # g with a cedilla is drawn with a turned comma above
                parts = [(base, 0, 0), ("mk.turnedcomma", round(anchor_x(face, base)), 615)]
            else:
                parts = [(base, 0, 0), ("mk.commabelow", round(center_x(face, base)), 0)]
            face.add_comp(glyph_name(cp), adv[base], parts, cp)
            face.base_of[glyph_name(cp)] = letter
            continue
        if mark == "caron" and letter in "dlLt":                         # the caron of these four is drawn as an apostrophe beside the letter
            y_scan, gap, pad = {"d": (650, 5, 30), "l": (650, 5, 36), "L": (600, 8, 30), "t": (600, 8, 30)}[letter]
            right = scan(shape(base), y_scan)[-1][1]                     # the right edge of the stem
            x = round(right + gap - tick[0])
            y = {"d": 740, "l": 740, "L": 700}.get(letter, 0) - tick[3] if letter != "t" else 575
            face.add_comp(glyph_name(cp), max(adv[base], round(x + tick[2] + pad)), [(base, 0, 0), ("mk.tick", x, y)], cp)
            face.base_of[glyph_name(cp)] = letter
            continue
        if letter == "i":
            base = "uni0131"
        elif letter == "j":
            base = "uni0237"
        comp(cp, base, mark, letter)

    # ligatures made of two letters side by side, with the apostrophe and the middle dot
    face.add_comp(glyph_name(0x132), adv["I"] + adv["J"] - 30, [("I", 0, 0), ("J", adv["I"] - 30, 0)], 0x132)
    face.add_comp(glyph_name(0x133), adv["i"] + adv["j"] - 6, [("i", 0, 0), ("j", adv["i"] - 6, 0)], 0x133)
    n_off = adv["uni2019"] - 44
    face.add_comp(glyph_name(0x149), n_off + adv["n"], [("uni2019", 0, 0), ("n", n_off, 0)], 0x149)
    xl = scan(shape("L"), 400)[0][1] + 166
    face.add_comp(glyph_name(0x13F), max(adv["L"], round(xl + dot_box[2] + 30)), [("L", 0, 0), ("mk.dot", round(xl), 350)], 0x13F)
    xs = scan(shape("l"), 400)[0][1] + 87
    face.add_comp(glyph_name(0x140), max(adv["l"], round(xs + dot_box[2] + 30)), [("l", 0, 0), ("mk.dot", round(xs), 268)], 0x140)
