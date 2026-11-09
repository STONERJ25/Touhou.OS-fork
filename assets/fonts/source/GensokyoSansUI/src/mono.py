"""Gensokyo Mono Regular: the fixed-width cut, drawn from the Regular of Gensokyo Sans.

The cell is 615 units, which is 8.0 pixels at the toolkit's UI size of 13 px: the cell of the 8x16 bitmap font, so a terminal can lay out Gensokyo Mono in whole
pixels (the brief of this face: the project's internal briefs). Heights are the Regular's (x-height 537, capitals 700, ascender 740, descender -210), the vertical metrics
and the stems (96 units: 1.25 px at 13 px) too, so that text in the two faces sits on one line and has one colour.

How a letter becomes a mono letter. The Regular's letters are 400 to 500 units of ink in a 500 to 600 unit advance, so most of them already fit a cell of 615 and
are only centred. The few that are wider than 500 units of ink (m w M W and the round capitals) are narrowed without thinning their strokes: the outline is eroded
sideways by a pen of a few units, its x coordinates scaled, and it is dilated sideways again (`condense`: an exact horizontal erosion and dilation, because the
pen has no height), so that a stem of 96 units comes out at 90 to 100, not at 60. The letters that are narrower than 300 units of ink and would leave a hole in a line of
mono text (i j l I r f J) are drawn with serifs and feet, as monospaced faces do, as single outlines. The zero has a dot in it, the one a flag and a foot, the lower case
l a curved foot and the capital I top and bottom bars, so that 0 O 1 l I | are six different pictures at 13 px.

Box drawing and block elements are generated (box_shape): they fill the cell exactly, 615 units wide and from -230 to 1000 (the 16 pixel row of the toolkit at 13 px, with
the baseline 13 px down), so that frames join without a seam at that size; strokes are 77 units (one pixel at 13 px), 154 (heavy) and two of 77 with 77 between (double).
"""
import unicodedata

import geom
from geom import embolden, scale, translate, ink_bbox, rnd, make_shape, clip_box

CELL = 615
MAX_INK = 500                         # a glyph with more ink than this is condensed to it
PEN = 36                              # the sideways pen of the condensing (units): stems of 96 come out at 60 + 36 f
LINE_TOP, LINE_BOTTOM = 1000, -230    # the 16 px row of the toolkit at 13 px: 13 px above the baseline and 3 below


def condense(shape, f, pen=PEN):
    """The shape narrowed to f times its width (f < 1) or widened (f > 1) with its vertical strokes kept near their width: erode by `pen` sideways, scale x, dilate."""
    if abs(f - 1.0) < 1e-6:
        return shape
    thin = embolden(geom.rnd(shape, min_len=10), -pen, 0)
    box = ink_bbox(thin)
    cx = (box[0] + box[2]) / 2
    out = embolden(scale(thin, f, 1.0, cx, 0), pen, 0)
    try:
        return geom.clean(out)
    except ValueError:
        return out


def centred(shape):
    """The shape moved sideways so that the ink is in the middle of the cell."""
    box = ink_bbox(shape)
    return translate(shape, (CELL - (box[0] + box[2])) / 2, 0)


# ---------------------------------------------------------------------------------------------------------------------------------------- the letters

# Letters that would leave a hole in a line of mono text, redrawn with the serifs and feet of monospaced faces. Coordinates of the Regular's letters (stems of 96,
# horizontals of 82); each is one outline, centred in the cell afterwards.
I_DOT = "M110 750 Q126 750 145 732 Q165 713 165 693 Q165 673 145 654 Q126 636 110 636 Q94 636 75 654 Q55 673 55 693 Q55 713 75 732 Q94 750 110 750 Z"
ZERO_DOT = "M277 475 Q319 475 319 350 Q319 225 277 225 Q235 225 235 350 Q235 475 277 475 Z"     # an oval 84 wide and 250 high in the counter of the zero
CUSTOM = {
    "i": ("M-70 0 L-70 82 L62 82 L62 457 L-20 457 L-20 537 L158 537 L158 82 L290 82 L290 0 Z", I_DOT, 0),
    "uni0131": ("M-70 0 L-70 82 L62 82 L62 457 L-20 457 L-20 537 L158 537 L158 82 L290 82 L290 0 Z", None, 0),
    "j": ("M78 457 L-4 457 L-4 537 L174 537 L174 -57 Q174 -210 43 -210 Q6 -210 -25 -197 L-10 -118 Q9 -127 29 -127 Q78 -127 78 -58 Z", I_DOT, 16),
    "uni0237": ("M78 457 L-4 457 L-4 537 L174 537 L174 -57 Q174 -210 43 -210 Q6 -210 -25 -197 L-10 -118 Q9 -127 29 -127 Q78 -127 78 -58 Z", None, 0),
    "l": ("M67 658 L-15 658 L-15 740 L163 740 L163 137 Q163 71 210 71 Q225 71 238 77 L252 3 Q220 -12 182 -12 Q67 -12 67 117 Z", None, 0),
    "r": ("M-5 0 L-5 82 L67 82 L67 537 L151 537 L160 438 Q212 549 302 549 Q332 549 357 540 L342 446 Q320 455 291 455 Q209 455 163 343 L163 82 "
          "L235 82 L235 0 Z", None, 0),
    "f": ("M30 0 L30 82 L110 82 L110 456 L27 456 L27 537 L110 537 L110 578 Q110 752 277 752 Q328 752 369 730 L346 653 Q317 669 281 669 Q206 669 206 580 "
          "L206 537 L336 537 L336 456 L206 456 L206 82 L286 82 L286 0 Z", None, 0),
    "J": ("M333 618 L190 618 L190 700 L429 700 L429 210 Q429 -12 228 -12 Q91 -12 24 87 L90 145 Q143 71 225 71 Q333 71 333 210 Z", None, 0),
    "I": ("M-21 0 L-21 79 L121 79 L121 621 L-21 621 L-21 700 L359 700 L359 621 L217 621 L217 79 L359 79 L359 0 Z", None, 0),
}
CONDENSE_BY = {"uni002D": 1.35}                  # the hyphen is a little longer in a mono face


def mono_shape(name, shape):
    """The Mono shape of a Regular glyph, centred in the cell (the cell is the advance of every glyph)."""
    if name in CUSTOM:
        path, dot, dot_dx = CUSTOM[name]
        sh = make_shape(path)
        if dot:
            sh = sh + translate(make_shape(dot), dot_dx, 0)
        return centred(sh)
    if name == "0":
        return centred(shape + make_shape(ZERO_DOT))        # (an island in the counter: ink again)
    box = ink_bbox(shape)
    w = box[2] - box[0]
    f = CONDENSE_BY.get(name) or (min(1.0, MAX_INK / w) if w > MAX_INK else 1.0)
    return centred(condense(shape, f))


# ---------------------------------------------------------------------------------------------------------------------------------------- box drawing

def rect_region(adds, subs=()):
    """The outline of the union of axis-aligned rectangles (x0, y0, x1, y1) minus the union of `subs`, as a shape (ink on the right). Done on the grid of all the
    edges' coordinates: a cell is ink when an added rectangle covers it and no subtracted one does; the boundary edges are chained into loops."""
    rects = list(adds) + list(subs)
    if not rects:
        return []
    xs = sorted({v for r in rects for v in (r[0], r[2])})
    ys = sorted({v for r in rects for v in (r[1], r[3])})
    nx, ny = len(xs) - 1, len(ys) - 1

    def covered(i, j, rs):
        x0, x1, y0, y1 = xs[i], xs[i + 1], ys[j], ys[j + 1]
        return any(r[0] <= x0 and x1 <= r[2] and r[1] <= y0 and y1 <= r[3] for r in rs)

    fill = [[covered(i, j, adds) and not covered(i, j, subs) for j in range(ny)] for i in range(nx)]

    def at(i, j):
        return 0 <= i < nx and 0 <= j < ny and fill[i][j]

    edges = {}
    for i in range(nx):
        for j in range(ny):
            if not fill[i][j]:
                continue
            x0, x1, y0, y1 = xs[i], xs[i + 1], ys[j], ys[j + 1]
            if not at(i, j + 1):
                edges.setdefault((x0, y1), []).append((x1, y1))
            if not at(i + 1, j):
                edges.setdefault((x1, y1), []).append((x1, y0))
            if not at(i, j - 1):
                edges.setdefault((x1, y0), []).append((x0, y0))
            if not at(i - 1, j):
                edges.setdefault((x0, y0), []).append((x0, y1))
    shape = []
    while edges:
        start = next(iter(edges))
        pts = [start]
        cur = start
        while True:
            nxt = edges[cur].pop()
            if not edges[cur]:
                del edges[cur]
            if nxt == start:
                break
            pts.append(nxt)
            cur = nxt
        merged = []                                                      # drop the points in the middle of a straight run
        for k, p in enumerate(pts):
            a, b = pts[k - 1], pts[(k + 1) % len(pts)]
            if (a[0] == p[0] == b[0]) or (a[1] == p[1] == b[1]):
                continue
            merged.append(p)
        shape.append([(merged[k], merged[(k + 1) % len(merged)]) for k in range(len(merged))])
    return shape


# stroke positions (light, heavy, double: the bands of ink across a line, and the gap of a double one), 77 units to a pixel at 13 px
CX, CY = 307.5, 385
V = {1: [(269, 346)], 2: [(230, 385)], 3: [(192, 269), (346, 423)]}       # bands across a vertical line (x)
H = {1: [(347, 423)], 2: [(308, 462)], 3: [(270, 347), (423, 500)]}       # bands across a horizontal line (y)
V_OUT = {1: (269, 346), 2: (230, 385), 3: (192, 423)}
H_OUT = {1: (347, 423), 2: (308, 462), 3: (270, 500)}
V_GAP, H_GAP = (269, 346), (347, 423)
DIRS = ("l", "r", "u", "d")


def box_arms(cp):
    """{'l': w, 'r': w, 'u': w, 'd': w} (0 none, 1 light, 2 heavy, 3 double) for the line-drawing characters U+2500..257F that are made of arms, parsed from their
    names; None for the dashed, rounded, diagonal ones."""
    name = unicodedata.name(chr(cp), "")
    if not name.startswith("BOX DRAWINGS ") or any(w in name for w in ("DASH", "ARC", "DIAGONAL")):
        return None
    words = name[len("BOX DRAWINGS "):].split(" AND ")
    weights = {"LIGHT": 1, "SINGLE": 1, "HEAVY": 2, "DOUBLE": 3}
    arms = {d: 0 for d in DIRS}
    parts = [w.split() for w in words]
    default = next((weights[t] for p in parts for t in p if t in weights), 1)
    for toks in parts:
        weight = next((weights[t] for t in toks if t in weights), None)
        dirs = []
        for t in toks:
            dirs += {"LEFT": ["l"], "RIGHT": ["r"], "UP": ["u"], "DOWN": ["d"], "HORIZONTAL": ["l", "r"], "VERTICAL": ["u", "d"]}.get(t, [])
        for d in dirs:
            arms[d] = weight if weight is not None else default
    return arms


def box_shape(arms):
    """The shape of a line-drawing character from its arms: every arm runs from the edge of the cell to the far face of the strokes that cross it; a double arm is the
    outline of a thick stroke (its core is cut out), which gives the corners, tees and crosses of double lines their right shapes."""
    X0, X1, Y0, Y1 = 0, CELL, LINE_BOTTOM, LINE_TOP
    hv = [arms["u"], arms["d"]]
    hh = [arms["l"], arms["r"]]
    v_out = V_OUT[max(hv)] if max(hv) else None                       # the vertical strokes that cross the horizontal arms
    h_out = H_OUT[max(hh)] if max(hh) else None
    adds, subs = [], []

    def horizontal(side):
        w = arms[side]
        if not w:
            return
        lo, hi = H_OUT[w]
        if side == "r":
            x0 = v_out[0] if v_out else (X0 if arms["l"] == w else int(CX))
            adds.append((x0, lo, X1, hi))
        else:
            x1 = v_out[1] if v_out else (X1 if arms["r"] == w else int(CX) + 1)
            adds.append((X0, lo, x1, hi))
        if w == 3:                                                    # the core: the gap between the two bars, open to the edge
            if side == "r":
                start = V_GAP[0] if max(hv) == 3 else (V_OUT[max(hv)][1] if max(hv) else (X0 - 50 if arms["l"] == 3 else int(CX)))
                subs.append((start, H_GAP[0], X1 + 50, H_GAP[1]))
            else:
                end = V_GAP[1] if max(hv) == 3 else (V_OUT[max(hv)][0] if max(hv) else (X1 + 50 if arms["r"] == 3 else int(CX)))
                subs.append((X0 - 50, H_GAP[0], end, H_GAP[1]))

    def vertical(side):
        w = arms[side]
        if not w:
            return
        lo, hi = V_OUT[w]
        if side == "u":
            y0 = h_out[0] if h_out else (Y0 if arms["d"] == w else int(CY))
            adds.append((lo, y0, hi, Y1))
        else:
            y1 = h_out[1] if h_out else (Y1 if arms["u"] == w else int(CY) + 1)
            adds.append((lo, Y0, hi, y1))
        if w == 3:
            if side == "u":
                start = H_GAP[0] if max(hh) == 3 else (H_OUT[max(hh)][1] if max(hh) else (Y0 - 50 if arms["d"] == 3 else int(CY)))
                subs.append((V_GAP[0], start, V_GAP[1], Y1 + 50))
            else:
                end = H_GAP[1] if max(hh) == 3 else (H_OUT[max(hh)][0] if max(hh) else (Y1 + 50 if arms["u"] == 3 else int(CY)))
                subs.append((V_GAP[0], Y0 - 50, V_GAP[1], end))

    for side in DIRS:
        (horizontal if side in "lr" else vertical)(side)
    # arms of the same weight on both sides make one bar through the middle: the extra half-rectangles overlap, which the region merges
    return rect_region(adds, subs)


def dashed_shape(cp):
    """The dashed lines U+2504..250B and U+254C..254F: 2, 3 or 4 dashes across the cell (a dash is 60% of its period, so that the pattern repeats across cells)."""
    name = unicodedata.name(chr(cp))
    n = 2 if "DOUBLE DASH" in name else 3 if "TRIPLE DASH" in name else 4
    heavy = "HEAVY" in name
    horizontal = "HORIZONTAL" in name
    span = CELL if horizontal else LINE_TOP - LINE_BOTTOM
    start = 0 if horizontal else LINE_BOTTOM
    period = span / n
    dash = period * 0.6
    band = (H if horizontal else V)[2 if heavy else 1][0]
    rects = []
    for k in range(n):
        a = start + k * period + (period - dash) / 2
        rects.append((int(round(a)), band[0], int(round(a + dash)), band[1]) if horizontal else (band[0], int(round(a)), band[1], int(round(a + dash))))
    return rect_region(rects)


def arc_shape(cp):
    """The rounded corners U+256D..2570: a light stroke along a quarter of an ellipse that joins the middle of two edges of the cell."""
    name = unicodedata.name(chr(cp))
    down, right = "DOWN" in name, "RIGHT" in name
    cx0, cx1 = V[1][0]
    t0, t1 = H[1][0]
    # the horizontal arm lies at y in (t0, t1) and the vertical one at x in (cx0, cx1); each quarter ellipse has its control point at the corner of the L that the
    # arc cuts, which is how the face draws all its round parts
    if down and right:        # arc from the right edge to the bottom edge, bulging up and to the left
        outer = [((CELL, t1), (cx0, t1), (cx0, LINE_BOTTOM)), ((cx1, LINE_BOTTOM), (cx1, t0), (CELL, t0))]
    elif down:                # down and left
        outer = [((0, t1), (cx1, t1), (cx1, LINE_BOTTOM)), ((cx0, LINE_BOTTOM), (cx0, t0), (0, t0))]
    elif right:               # up and right
        outer = [((CELL, t0), (cx0, t0), (cx0, LINE_TOP)), ((cx1, LINE_TOP), (cx1, t1), (CELL, t1))]
    else:                     # up and left
        outer = [((0, t0), (cx1, t0), (cx1, LINE_TOP)), ((cx0, LINE_TOP), (cx0, t1), (0, t1))]
    (a0, a1, a2), (b0, b1, b2) = outer
    contour = [(a0, a1, a2), (a2, b0), (b0, b1, b2), (b2, a0)]
    contour = [tuple((float(x), float(y)) for x, y in seg) for seg in contour]
    return [geom.reverse_contour(contour) if geom.area(contour) > 0 else contour]


def diagonal_shape(cp):
    """U+2571..2573: a stroke of 77 units from corner to corner of the cell, clipped to it."""
    from glyphset import stroke
    forward = cp in (0x2571, 0x2573)                               # from the lower left to the upper right
    backward = cp in (0x2572, 0x2573)
    parts = []
    if forward:
        parts.append(stroke((-60, LINE_BOTTOM - 100), (CELL + 60, LINE_TOP + 100), 77))
    if backward:
        parts.append(stroke((-60, LINE_TOP + 100), (CELL + 60, LINE_BOTTOM - 100), 77))
    shape = geom.union(*parts) if len(parts) > 1 else parts[0]
    return clip_box(shape, 0, LINE_BOTTOM, CELL, LINE_TOP)


def block_shape(cp):
    """The block elements U+2580..259F: halves, eighths, quadrants, shades (stripes of one pixel, so that the pattern repeats across cells)."""
    H_ = LINE_TOP - LINE_BOTTOM
    row = H_ / 16.0                                                # 16 pixel rows of the cell
    col = CELL / 8.0

    def r(x0, y0, x1, y1):
        return (int(round(x0)), int(round(LINE_BOTTOM + y0 * row)), int(round(x1)), int(round(LINE_BOTTOM + y1 * row)))

    if cp == 0x2580:
        return rect_region([r(0, 8, CELL, 16)])
    if 0x2581 <= cp <= 0x2588:                                     # lower one eighth ... full block
        return rect_region([r(0, 0, CELL, 2 * (cp - 0x2580))])
    if 0x2589 <= cp <= 0x258F:                                     # left seven eighths ... one eighth
        return rect_region([r(0, 0, col * (0x2590 - cp), 16)])
    if cp == 0x2590:
        return rect_region([r(col * 4, 0, CELL, 16)])
    if cp in (0x2591, 0x2592, 0x2593):                             # light, medium, dark shade: one pixel row in 4, in 2, three in 4
        rows = [k for k in range(16) if (k % 4 == 0 if cp == 0x2591 else k % 2 == 0 if cp == 0x2592 else k % 4 != 3)]
        return rect_region([r(0, k, CELL, k + 1) for k in rows])
    if cp == 0x2594:
        return rect_region([r(0, 15, CELL, 16)])
    if cp == 0x2595:
        return rect_region([r(col * 7, 0, CELL, 16)])
    quadrants = {0x2596: "ll", 0x2597: "lr", 0x2598: "ul", 0x2599: "ul ll lr", 0x259A: "ul lr", 0x259B: "ul ur ll", 0x259C: "ul ur lr", 0x259D: "ur", 0x259E: "ur ll",
                 0x259F: "ur ll lr"}
    rects = []
    for q in quadrants[cp].split():
        x0, x1 = (0, CELL / 2) if q[1] == "l" else (CELL / 2, CELL)
        y0, y1 = (0, 8) if q[0] == "l" else (8, 16)
        rects.append(r(x0, y0, x1, y1))
    if cp in (0x259A, 0x259E):                                     # two quadrants that only touch at a corner: a unit of gap there keeps the outline simple
        a = rects[1]
        rects[1] = (a[0] + 1, a[1], a[2], a[3] - 1) if cp == 0x259A else (a[0], a[1], a[2] - 1, a[3] - 1)
    return rect_region(rects)


def box_glyphs(face):
    """Adds U+2500..257F and U+2580..259F to the face (all the code points of the two blocks)."""
    from glyphset import glyph_name
    for cp in range(0x2500, 0x25A0):
        if cp < 0x2580:
            name = unicodedata.name(chr(cp), "")
            if "DASH" in name:
                shape = dashed_shape(cp)
            elif "ARC" in name:
                shape = arc_shape(cp)
            elif "DIAGONAL" in name:
                shape = diagonal_shape(cp)
            else:
                shape = box_shape(box_arms(cp))
        else:
            shape = block_shape(cp)
        face.add_shape(glyph_name(cp), CELL, rnd(shape), cp)


# ---------------------------------------------------------------------------------------------------------------------------------------- the face

NO_MONO = {0x132, 0x133, 0x149}                         # IJ, ij and n preceded by an apostrophe are two letters: they have no place in a cell
EM_SPACES = {0x2001, 0x2003, 0x3000}                    # these are two cells wide


def build_mono(reg):
    """The Mono face (a glyphset.Face) made from the Regular face reg."""
    import glyphset
    import symbols
    from glyphset import Face, glyph_name
    face = Face("Mono")
    cp_of = {}
    for cp, name in reg.cmap.items():
        cp_of.setdefault(name, cp)
    for name in reg.names:
        if name in reg.comps:
            continue
        cp = cp_of.get(name)
        shape = reg.shape[name]
        if name.startswith("mk.") or name.startswith("sm."):
            face.add_shape(name, reg.adv[name], shape)
        elif name == "space" or (cp is not None and not shape):
            face.add_shape(name, CELL * (2 if cp in EM_SPACES else 1), [])
        elif cp in NO_MONO:
            continue
        else:
            face.add_shape(name, CELL, rnd(mono_shape(name, shape), 6))
    for cp, name in reg.cmap.items():
        if name in face.adv and cp not in NO_MONO:
            face.cmap[cp] = name
    face.base_of = {}
    # the underscore and the dashes fill the cell, so that rules join across cells
    face.shape["uni005F"] = make_shape("M0 -148 L0 -72 L615 -72 L615 -148 Z")
    face.shape["uni2014"] = make_shape("M0 262 L0 344 L615 344 L615 262 Z")
    face.shape["uni2013"] = make_shape("M62 262 L62 344 L553 344 L553 262 Z")
    # the ellipsis: three dots in one cell
    from ui_outlines import dot
    face.shape["uni2026"] = centred(make_shape(dot(60, 57, 57), dot(280, 57, 57), dot(500, 57, 57)))
    glyphset.latin_composites(face)
    for cp in NO_MONO:                                                 # (latin_composites made them: they are two letters and do not fit a cell)
        name = glyph_name(cp)
        if name in face.comps:
            del face.comps[name]
            face.names.remove(name)
            del face.adv[name]
            face.cmap.pop(cp, None)
    import cyrgreek
    cyrgreek.composites(face)                                          # (й ў ѓ ќ ѝ and the Greek letters with a tonos or dialytika, placed over the mono letters)
    for name in list(face.comps):
        face.adv[name] = CELL
    # what the Regular builds from several glyphs and is not a letter with a mark (fractions, superscripts, the numero sign) becomes one condensed shape
    for name in reg.comps:
        cp = cp_of.get(name)
        if name in face.adv or cp is None or cp in NO_MONO:
            continue
        face.add_shape(name, CELL, rnd(mono_shape(name, reg.glyph_shape(name)), 6), cp)
    box_glyphs(face)
    return face
