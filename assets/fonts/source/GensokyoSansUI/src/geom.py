"""Outline geometry for the Gensokyo Sans generators: parsing, transforms, a boolean union that keeps the quadratic curves, and an outline offset.

Written for this project from the textbook definitions (quadratic Bezier splitting by de Casteljau, winding numbers, offset curves by the tangent-line
construction); no code from other projects. fontTools stays the only dependency of the generators, and it is not needed in this module.

A shape is a list of contours. A contour is a list of segments that form a closed loop; a segment is a tuple of points, ((x0, y0), (x1, y1)) for a line and
((x0, y0), (cx, cy), (x1, y1)) for a quadratic curve with one control point. The ink lies on the **right** of the direction of travel (TrueType: outer
contours run clockwise, counters counter-clockwise), and "inside" always means a non-zero winding number, as in libttf.

union() needs no bookkeeping about holes: the boundary of A union B is the part of A's boundary outside B's ink together with the part of B's boundary outside
A's ink (a counter of A that lies in B's ink disappears, a bar of B that crosses A's counter stays). Both curves are cut where they cross, and the pieces are
kept or dropped by testing their midpoints. Two outlines that share an edge exactly (the same line, the same stretch) are not handled: the designs avoid it.
"""
import math
import re
from collections import defaultdict


# ------------------------------------------------------------------------------------------------------------------------------ points and segments

def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def _lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def _cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def _unit(v):
    n = math.hypot(v[0], v[1])
    return (v[0] / n, v[1] / n) if n > 1e-12 else None


def seg_pt(s, t):
    if len(s) == 2:
        return _lerp(s[0], s[1], t)
    (x0, y0), (cx, cy), (x1, y1) = s
    u = 1.0 - t
    return (u * u * x0 + 2 * u * t * cx + t * t * x1, u * u * y0 + 2 * u * t * cy + t * t * y1)


def seg_split(s, t):
    """The two segments that make up s, cut at parameter t (exact: de Casteljau)."""
    if len(s) == 2:
        m = _lerp(s[0], s[1], t)
        return (s[0], m), (m, s[1])
    p0, c, p1 = s
    a, b = _lerp(p0, c, t), _lerp(c, p1, t)
    m = _lerp(a, b, t)
    return (p0, a, m), (m, b, p1)


def seg_sub(s, t0, t1):
    """The part of s between the parameters t0 < t1."""
    if t0 > 0.0:
        s = seg_split(s, t0)[1]
        t1 = (t1 - t0) / (1.0 - t0)
    if t1 < 1.0:
        s = seg_split(s, t1)[0]
    return s


def seg_reverse(s):
    return tuple(reversed(s))


def seg_bbox(s):
    xs = [p[0] for p in s]
    ys = [p[1] for p in s]
    return min(xs), min(ys), max(xs), max(ys)


def seg_tangents(s):
    """Unit tangent at the start and at the end of a segment."""
    if len(s) == 2:
        t = _unit(_sub(s[1], s[0]))
        return t, t
    p0, c, p1 = s
    chord = _unit(_sub(p1, p0))
    return _unit(_sub(c, p0)) or chord, _unit(_sub(p1, c)) or chord


def flatten(contour, n=12):
    """The contour as a polygon: every curve cut into n straight pieces."""
    pts = []
    for s in contour:
        pts.append(s[0])
        if len(s) == 3:
            pts.extend(seg_pt(s, k / n) for k in range(1, n))
    return pts


# ------------------------------------------------------------------------------------------------------------------------------ paths and shapes

_TOKEN = re.compile(r"[MLQZ]|-?\d+(?:\.\d+)?")


def parse_contour(d):
    """'M x y L x y Q cx cy x y ... Z' (absolute, M L Q Z only, one contour) to a contour. The closing line is a segment of its own when needed."""
    tokens = _TOKEN.findall(d.removeprefix("!"))
    contour, cur, start, i, cmd = [], None, None, 0, None
    while i < len(tokens):
        if tokens[i] in "MLQZ":
            cmd = tokens[i]
            i += 1
            if cmd == "Z":
                if cur != start:
                    contour.append((cur, start))
                cur = start
            continue
        if cmd == "M":
            start = cur = (float(tokens[i]), float(tokens[i + 1]))
            i += 2
            cmd = "L"                                                  # further pairs after a move are lines
        elif cmd == "L":
            p = (float(tokens[i]), float(tokens[i + 1]))
            contour.append((cur, p))
            cur = p
            i += 2
        elif cmd == "Q":
            c = (float(tokens[i]), float(tokens[i + 1]))
            p = (float(tokens[i + 2]), float(tokens[i + 3]))
            contour.append((cur, c, p))
            cur = p
            i += 4
        else:
            raise ValueError("bad path: " + d)
    if cur != start:
        contour.append((cur, start))
    return contour


def area(contour):
    """Signed area (y up: positive means counter-clockwise)."""
    pts = flatten(contour, 24)
    return 0.5 * sum(_cross(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts)))


def reverse_contour(contour):
    return [seg_reverse(s) for s in reversed(contour)]


def make_shape(*paths):
    """Contours from path strings, with the generator's convention: a path starting with '!' is a counter (counter-clockwise), the others are outer
    contours (clockwise). Accepts a ready-made contour or shape as well."""
    shape = []
    for p in paths:
        if isinstance(p, str):
            contour = parse_contour(p)
            counter = p.startswith("!")
            if (area(contour) > 0) != counter:
                contour = reverse_contour(contour)
            shape.append(contour)
        elif p and isinstance(p[0], list):
            shape.extend(p)
        else:
            shape.append(p)
    return shape


def shape_bbox(shape):
    boxes = [seg_bbox(s) for c in shape for s in c]
    return (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))


def ink_bbox(shape):
    """Bounding box of the ink itself (the control points of a curve may lie outside the curve)."""
    pts = [p for c in shape for p in flatten(c, 24)]
    return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))


def transform(shape, m):
    """Affine map x' = a x + c y + e, y' = b x + d y + f with m = (a, b, c, d, e, f). A mirroring map keeps the ink on the right."""
    a, b, c, d, e, f = m
    mirrored = a * d - b * c < 0

    def tp(p):
        return (a * p[0] + c * p[1] + e, b * p[0] + d * p[1] + f)

    out = []
    for contour in shape:
        new = [tuple(tp(p) for p in s) for s in contour]
        out.append(reverse_contour(new) if mirrored else new)
    return out


def translate(shape, dx, dy):
    return transform(shape, (1, 0, 0, 1, dx, dy))


def scale(shape, sx, sy=None, cx=0.0, cy=0.0):
    sy = sx if sy is None else sy
    return transform(shape, (sx, 0, 0, sy, cx - sx * cx, cy - sy * cy))


def rotate180(shape, cx, cy):
    return transform(shape, (-1, 0, 0, -1, 2 * cx, 2 * cy))


def mirror_x(shape, cx):
    return transform(shape, (-1, 0, 0, 1, 2 * cx, 0))


def mirror_y(shape, cy):
    return transform(shape, (1, 0, 0, -1, 0, 2 * cy))


def shear(shape, k):
    """x' = x + k y: a slant (about the baseline)."""
    return transform(shape, (1, 0, k, 1, 0, 0))


def clip_box(shape, x0, y0, x1, y1):
    """The shape cut to a box (polygons only: a shape made of straight pieces, as the diagonals are)."""
    out = []
    for contour in shape:
        poly = [s[0] for s in contour]
        for axis, bound, keep_less in ((0, x0, False), (0, x1, True), (1, y0, False), (1, y1, True)):
            nxt = []
            for k in range(len(poly)):
                p, q = poly[k], poly[(k + 1) % len(poly)]
                pin = (p[axis] <= bound) if keep_less else (p[axis] >= bound)
                qin = (q[axis] <= bound) if keep_less else (q[axis] >= bound)
                if pin:
                    nxt.append(p)
                if pin != qin:
                    t = (bound - p[axis]) / (q[axis] - p[axis])
                    nxt.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
            poly = nxt
            if not poly:
                break
        if len(poly) >= 3:
            out.append([(poly[k], poly[(k + 1) % len(poly)]) for k in range(len(poly))])
    return out


def collapse_short(contour, min_len):
    """Remove the pieces (lines and curves) shorter than min_len units by joining their two ends in the middle (the near-coincident edges of a union leave such steps)."""
    contour = list(contour)
    while len(contour) > 3:
        for i, s in enumerate(contour):
            if sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(s, s[1:])) < min_len:        # (a line, or the control polygon of a curve)
                break
        else:
            return contour
        rot = contour[i + 1:] + contour[:i + 1]                       # the short piece is last now, the one after it first, the one before it second to last
        mid = (int(round((s[0][0] + s[1][0]) / 2)), int(round((s[0][1] + s[1][1]) / 2)))
        rot[0] = (mid,) + rot[0][1:]
        rot[-2] = rot[-2][:-1] + (mid,)
        contour = rot[:-1]
    return contour


def rnd(shape, min_len=0):
    """Round every point to the integer grid of the font, drop segments that collapsed, merge neighbouring straight pieces that run the same way; with
    min_len, also remove straight steps shorter than that."""
    out = []
    for contour in shape:
        new = []
        for s in contour:
            t = tuple((int(round(x)), int(round(y))) for x, y in s)
            if t[0] == t[-1] and (len(t) == 2 or t[1] == t[0]):
                continue                                               # collapsed to a point
            if len(t) == 3 and (t[1] == t[0] or t[1] == t[2]):
                t = (t[0], t[2])                                       # a curve with its control on an end is a line
            new.append(t)
        merged = []
        for s in new:
            if merged and len(s) == 2 and len(merged[-1]) == 2:
                a, b = merged[-1], s
                if _cross(_sub(a[1], a[0]), _sub(b[1], b[0])) == 0 and (a[1][0] - a[0][0]) * (b[1][0] - b[0][0]) + (a[1][1] - a[0][1]) * (b[1][1] - b[0][1]) > 0:
                    merged[-1] = (a[0], b[1])
                    continue
            merged.append(s)
        if len(merged) > 1 and len(merged[0]) == 2 and len(merged[-1]) == 2:      # the seam between last and first
            a, b = merged[-1], merged[0]
            if _cross(_sub(a[1], a[0]), _sub(b[1], b[0])) == 0 and (a[1][0] - a[0][0]) * (b[1][0] - b[0][0]) + (a[1][1] - a[0][1]) * (b[1][1] - b[0][1]) > 0:
                merged[0] = (a[0], b[1])
                merged.pop()
        if len(merged) >= 3 or (len(merged) == 2 and any(len(s) == 3 for s in merged)):
            out.append(collapse_short(merged, min_len) if min_len else merged)
    return out


def point_count(shape):
    return sum(len(c) + sum(1 for s in c if len(s) == 3) for c in shape)


# ------------------------------------------------------------------------------------------------------------------------------ intersections

def winding(pt, polys):
    """Winding number of the point pt with respect to a list of polygons (lists of points)."""
    x, y = pt
    wn = 0
    for poly in polys:
        n = len(poly)
        for i in range(n):
            x0, y0 = poly[i]
            x1, y1 = poly[(i + 1) % n]
            if y0 <= y:
                if y1 > y and (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) > 0:
                    wn += 1
            elif y1 <= y and (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) < 0:
                wn -= 1
    return wn


def _line_line(a, b):
    p, q = a[0], b[0]
    r, s = _sub(a[1], a[0]), _sub(b[1], b[0])
    rxs = _cross(r, s)
    if abs(rxs) < 1e-12:
        return []
    qp = _sub(q, p)
    t = _cross(qp, s) / rxs
    u = _cross(qp, r) / rxs
    eps = 1e-9
    return [(t, u)] if -eps <= t <= 1 + eps and -eps <= u <= 1 + eps else []


def _line_quad(line, quad, flip):
    """Crossings of a line segment with a quadratic; returns (t_line, t_quad) pairs, or (t_quad, t_line) when flip."""
    p, d = line[0], _sub(line[1], line[0])
    length2 = d[0] * d[0] + d[1] * d[1]
    if length2 < 1e-18:
        return []
    n = (-d[1], d[0])
    p0, c, p1 = quad
    qa = n[0] * (p0[0] - 2 * c[0] + p1[0]) + n[1] * (p0[1] - 2 * c[1] + p1[1])
    qb = 2 * (n[0] * (c[0] - p0[0]) + n[1] * (c[1] - p0[1]))
    qc = n[0] * (p0[0] - p[0]) + n[1] * (p0[1] - p[1])
    roots = []
    if abs(qa) < 1e-12:
        if abs(qb) > 1e-12:
            roots = [-qc / qb]
    else:
        disc = qb * qb - 4 * qa * qc
        if disc >= 0:
            sq = math.sqrt(disc)
            roots = [(-qb - sq) / (2 * qa), (-qb + sq) / (2 * qa)]
    out = []
    eps = 1e-9
    for t in roots:
        if -eps <= t <= 1 + eps:
            q = seg_pt(quad, min(1.0, max(0.0, t)))
            u = ((q[0] - p[0]) * d[0] + (q[1] - p[1]) * d[1]) / length2
            if -eps <= u <= 1 + eps:
                out.append((t, u) if flip else (u, t))
    return out


def _quad_quad(a, b, ta0, ta1, tb0, tb1, out, depth):
    boxa, boxb = seg_bbox(a), seg_bbox(b)
    if boxa[2] < boxb[0] or boxb[2] < boxa[0] or boxa[3] < boxb[1] or boxb[3] < boxa[1]:
        return
    if depth >= 30 or (boxa[2] - boxa[0] < 1e-4 and boxa[3] - boxa[1] < 1e-4 and boxb[2] - boxb[0] < 1e-4 and boxb[3] - boxb[1] < 1e-4):
        out.append(((ta0 + ta1) / 2, (tb0 + tb1) / 2))
        return
    am, bm = (ta0 + ta1) / 2, (tb0 + tb1) / 2
    a1, a2 = seg_split(a, 0.5)
    b1, b2 = seg_split(b, 0.5)
    _quad_quad(a1, b1, ta0, am, tb0, bm, out, depth + 1)
    _quad_quad(a1, b2, ta0, am, bm, tb1, out, depth + 1)
    _quad_quad(a2, b1, am, ta1, tb0, bm, out, depth + 1)
    _quad_quad(a2, b2, am, ta1, bm, tb1, out, depth + 1)


def seg_intersections(a, b):
    """Parameters (ta, tb) where segments a and b cross."""
    ba, bb = seg_bbox(a), seg_bbox(b)
    if ba[2] < bb[0] - 1e-9 or bb[2] < ba[0] - 1e-9 or ba[3] < bb[1] - 1e-9 or bb[3] < ba[1] - 1e-9:
        return []
    if len(a) == 2 and len(b) == 2:
        return _line_line(a, b)
    if len(a) == 2:
        return _line_quad(a, b, False)
    if len(b) == 2:
        return _line_quad(b, a, True)
    raw = []
    _quad_quad(a, b, 0.0, 1.0, 0.0, 1.0, raw, 0)
    raw.sort()
    out = []
    for t in raw:
        if not out or abs(t[0] - out[-1][0]) > 1e-3 or abs(t[1] - out[-1][1]) > 1e-3:
            out.append(t)
    return out


# ------------------------------------------------------------------------------------------------------------------------------ union

def _key(p):
    return (round(p[0], 5), round(p[1], 5))


def union(*shapes):
    """The union of shapes (see the module's note). Returns a shape with float coordinates; round it with rnd()."""
    result = shapes[0]
    for other in shapes[1:]:
        result = _union2(result, other)
    return result


def _union2(a, b):
    polys_a = [flatten(c, 16) for c in a]
    polys_b = [flatten(c, 16) for c in b]
    cuts_a, cuts_b = defaultdict(list), defaultdict(list)
    for ci, ca in enumerate(a):
        for si, sa in enumerate(ca):
            for cj, cb in enumerate(b):
                for sj, sb in enumerate(cb):
                    for ta, tb in seg_intersections(sa, sb):
                        pa, pb = seg_pt(sa, ta), seg_pt(sb, tb)
                        p = ((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2)
                        cuts_a[(ci, si)].append((ta, p))
                        cuts_b[(cj, sj)].append((tb, p))
    pieces = []
    for shape, cuts, others in ((a, cuts_a, polys_b), (b, cuts_b, polys_a)):
        for ci, contour in enumerate(shape):
            for si, s in enumerate(contour):
                found = sorted(cuts.get((ci, si), []))
                ts, ps = [0.0], [s[0]]
                for t, p in found:
                    if t <= 1e-7 or t >= 1 - 1e-7 or t - ts[-1] < 1e-7:
                        continue                                       # a crossing at an end of the segment: the end is already a vertex
                    ts.append(t)
                    ps.append(p)
                ts.append(1.0)
                ps.append(s[-1])
                for k in range(len(ts) - 1):
                    part = seg_sub(s, ts[k], ts[k + 1]) if (ts[k] > 0 or ts[k + 1] < 1) else s
                    part = (ps[k],) + part[1:-1] + (ps[k + 1],)
                    if winding(seg_pt(part, 0.5), others) == 0:
                        pieces.append(part)
    return _assemble(pieces)


def _assemble(pieces):
    by_start = defaultdict(list)
    for i, p in enumerate(pieces):
        by_start[_key(p[0])].append(i)
    used = [False] * len(pieces)
    shape = []
    for first in range(len(pieces)):
        if used[first]:
            continue
        contour = []
        i = first
        start = _key(pieces[first][0])
        while True:
            used[i] = True
            contour.append(pieces[i])
            end = _key(pieces[i][-1])
            if end == start:
                break
            cands = [j for j in by_start.get(end, []) if not used[j]]
            if not cands:
                raise ValueError("union: the outline does not close at %s (%d pieces)" % (end, len(pieces)))
            if len(cands) > 1:                                         # two ways on: keep the one that turns least to the right
                t_in = seg_tangents(pieces[i])[1]
                cands.sort(key=lambda j: -(_cross(t_in, seg_tangents(pieces[j])[0])))
            i = cands[0]
        shape.append(contour)
    return shape


def clean(shape, eps=0.8):
    """The boundary of the region with a non-zero winding number, for a shape whose contours cross themselves or each other (what an outline offset leaves at a sharp
    notch: a small loop, a contour that has run into its neighbour). Every segment is cut where it crosses another one; a piece is kept when exactly one of its two
    sides (eps units away) is ink, turned round when the ink is on its left, so the result runs the usual way (ink on the right) and nothing crosses."""
    segs = [(ci, si, s) for ci, c in enumerate(shape) for si, s in enumerate(c)]
    polys = [flatten(c, 16) for c in shape]
    cuts = defaultdict(list)
    for i, (ci, si, a) in enumerate(segs):
        for cj, sj, b in segs[i + 1:]:
            if ci == cj and (abs(si - sj) == 1 or abs(si - sj) == len(shape[ci]) - 1):
                continue                                               # neighbours share an end point: that is no crossing
            for ta, tb in seg_intersections(a, b):
                pa, pb = seg_pt(a, ta), seg_pt(b, tb)
                p = ((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2)
                cuts[(ci, si)].append((ta, p))
                cuts[(cj, sj)].append((tb, p))
    pieces = []
    for ci, si, s in segs:
        found = sorted(cuts.get((ci, si), []))
        ts, ps = [0.0], [s[0]]
        for t, p in found:
            if t <= 1e-7 or t >= 1 - 1e-7 or t - ts[-1] < 1e-7:
                continue
            ts.append(t)
            ps.append(p)
        ts.append(1.0)
        ps.append(s[-1])
        for k in range(len(ts) - 1):
            part = seg_sub(s, ts[k], ts[k + 1]) if (ts[k] > 0 or ts[k + 1] < 1) else s
            part = (ps[k],) + part[1:-1] + (ps[k + 1],)
            m = seg_pt(part, 0.5)
            d = _unit(_sub(part[-1], part[0]))
            if d is None:
                continue
            left = (m[0] - d[1] * eps, m[1] + d[0] * eps)
            right = (m[0] + d[1] * eps, m[1] - d[0] * eps)
            ink_left, ink_right = winding(left, polys) != 0, winding(right, polys) != 0
            if ink_right and not ink_left:
                pieces.append(part)
            elif ink_left and not ink_right:
                pieces.append(seg_reverse(part))
    return _assemble(pieces) if pieces else []


# ------------------------------------------------------------------------------------------------------------------------------ offsetting (emboldening)

def _miter(p, t_in, t_out, ex, ey, limit):
    """Where the vertex p goes when both edges that meet there move away from the ink by the pen's reach in their normal direction (an elliptical pen of
    half-axes ex and ey: the reach along the unit normal n is sqrt((ex nx)^2 + (ey ny)^2))."""
    n1, n2 = (-t_in[1], t_in[0]), (-t_out[1], t_out[0])
    sign = -1.0 if ex < 0 else 1.0                                      # (a negative pen shrinks the ink)
    d1 = sign * math.hypot(ex * n1[0], ey * n1[1])
    d2 = sign * math.hypot(ex * n2[0], ey * n2[1])
    det = _cross(n1, n2)
    if abs(det) < 1e-3:                                                 # a smooth join: straight out
        nx, ny = n1[0] + n2[0], n1[1] + n2[1]
        m = math.hypot(nx, ny) or 1.0
        d = (d1 + d2) / 2
        return (p[0] + nx / m * d, p[1] + ny / m * d)
    dx = (d1 * n2[1] - d2 * n1[1]) / det
    dy = (n1[0] * d2 - n2[0] * d1) / det
    reach = math.hypot(dx, dy)
    cap = limit * max(abs(d1), abs(d2))
    if reach > cap:                                                     # a very sharp corner: clip the spike
        dx, dy = dx * cap / reach, dy * cap / reach
    return (p[0] + dx, p[1] + dy)


def embolden(shape, ex, ey=None, limit=3.2):
    """The shape with every edge moved away from the ink: the ink grows by ex on the left and right of a vertical stem and by ey above and below a
    horizontal bar (by about the pen's reach on any slope between). Counters shrink by the same amount, so they must be big enough: the caller checks."""
    ey = ex if ey is None else ey
    out = []
    for contour in shape:
        contour = [s for s in contour if sum(abs(b[0] - a[0]) + abs(b[1] - a[1]) for a, b in zip(s, s[1:])) > 1e-6]      # (a piece with no length has no direction)
        if len(contour) < 2:
            continue
        n = len(contour)
        tans = [seg_tangents(s) for s in contour]
        verts = [_miter(contour[i][0], tans[i - 1][1], tans[i][0], ex, ey, limit) for i in range(n)]
        new = []
        for i, s in enumerate(contour):
            v0, v1 = verts[i], verts[(i + 1) % n]
            if len(s) == 2:
                new.append((v0, v1))
                continue
            t0, t1 = tans[i]
            det = _cross(t0, t1)
            ctrl = ((v0[0] + v1[0]) / 2, (v0[1] + v1[1]) / 2)
            if abs(det) > 1e-3:
                w = _sub(v1, v0)
                a = _cross(w, t1) / det                                # w = a t0 + b t1: the control is v0 + a t0, which is also v1 - b t1
                b = _cross(t0, w) / det
                if a > 0 and b > 0:                                   # (the control lies ahead of the start and behind the end)
                    ctrl = (v0[0] + a * t0[0], v0[1] + a * t0[1])
            new.append((v0, ctrl, v1))
        out.append(new)
    return out


# ------------------------------------------------------------------------------------------------------------------------------ checks and drawing

def self_intersections(shape, n=8):
    """Pairs of edges of the (flattened) shape that cross each other, not counting neighbours: an empty list means the outline is clean."""
    edges = []
    for ci, contour in enumerate(shape):
        pts = flatten(contour, n)
        for k in range(len(pts)):
            edges.append((ci, k, len(pts), pts[k], pts[(k + 1) % len(pts)]))
    bad = []
    for i in range(len(edges)):
        ci, k, m, a, b = edges[i]
        for j in range(i + 1, len(edges)):
            cj, l, mm, c, d = edges[j]
            if ci == cj and (l - k in (1, m - 1) or l == k):
                continue
            if max(a[0], b[0]) < min(c[0], d[0]) or max(c[0], d[0]) < min(a[0], b[0]) or max(a[1], b[1]) < min(c[1], d[1]) or max(c[1], d[1]) < min(a[1], b[1]):
                continue
            s1 = _cross(_sub(b, a), _sub(c, a)) * _cross(_sub(b, a), _sub(d, a))
            s2 = _cross(_sub(d, c), _sub(a, c)) * _cross(_sub(d, c), _sub(b, c))
            if s1 < 0 and s2 < 0:
                bad.append((a, b, c, d))
    return bad


def draw(shape, pen):
    """Send a shape (integer points) to a fontTools pen."""
    for contour in shape:
        pen.moveTo(contour[0][0])
        for s in contour:
            if len(s) == 2:
                pen.lineTo(s[1])
            else:
                pen.qCurveTo(s[1], s[2])
        pen.closePath()


def to_path(shape):
    """The shape as one path string per contour (for debugging and for the source files)."""
    out = []
    for contour in shape:
        parts = ["M%g %g" % contour[0][0]]
        for s in contour:
            if len(s) == 2:
                parts.append("L%g %g" % s[1])
            else:
                parts.append("Q%g %g %g %g" % (s[1] + s[2]))
        parts.append("Z")
        out.append(" ".join(parts))
    return out
