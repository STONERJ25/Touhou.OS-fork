"""Validate the tables, metrics, and outlines delivered in Stage 1."""
import math

from fontTools.pens.basePen import BasePen



class Outline(BasePen):
    """Record endpoints and sample curves for contour topology checks."""

    def __init__(self):
        super().__init__(None)
        self.rings = []
        self.short = []
        self.extrema = []

    def _moveTo(self, point):
        self.ring = [point]

    def _lineTo(self, point):
        length = math.dist(self.ring[-1], point)
        if length < 6:
            self.short.append((self.ring[-1], point))
        self.ring.append(point)

    def _qCurveToOne(self, control, end):
        start = self.ring[-1]
        if math.dist(start, control) + math.dist(control, end) < 6:
            self.short.append((start, end))
        for a, b, c in zip(start, control, end):
            if b < min(a, c) or b > max(a, c):
                self.extrema.append((start, control, end))
        for step in range(1, 17):
            t = step / 16
            self.ring.append(tuple(
                (1 - t)**2 * a + 2*t*(1 - t)*b + t*t*c
                for a, b, c in zip(start, control, end)
            ))

    def _closePath(self):
        if self.ring[-1] != self.ring[0]:
            self._lineTo(self.ring[0])
        self.rings.append(self.ring)


def side(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])


def intersects(a, b, c, d):
    for axis in range(2):
        if max(a[axis], b[axis]) < min(c[axis], d[axis]):
            return False
        if max(c[axis], d[axis]) < min(a[axis], b[axis]):
            return False
    return side(a, b, c) * side(a, b, d) <= 0 and side(c, d, a) * side(c, d, b) <= 0


def inside(point, ring):
    x, y = point
    crossings = sum(
        (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax
        for (ax, ay), (bx, by) in zip(ring, ring[1:])
    )
    return crossings % 2


def check_outline(name, glyph, glyf):
    outline = Outline()
    glyph.draw(outline, glyf)
    edges = []
    for i, ring in enumerate(outline.rings):
        area = sum(ax * by - bx * ay for (ax, ay), (bx, by) in zip(ring, ring[1:]))
        depth = sum(inside(ring[0], other) for j, other in enumerate(outline.rings) if i != j)
        assert depth <= 1 and area * (1 if depth else -1) > 0, f"{name}: contour winding"
        edges.extend((i, j, a, b) for j, (a, b) in enumerate(zip(ring, ring[1:])))
    for k, (i, j, a, b) in enumerate(edges):
        for i2, j2, c, d in edges[k + 1:]:
            if i == i2 and j2 - j in (1, len(outline.rings[i]) - 2):
                continue
            assert not intersects(a, b, c, d), f"{name}: intersecting edges"
    assert not outline.short, f"{name}: short segments {outline.short}"
    assert not outline.extrema, f"{name}: off-curve extrema {outline.extrema}"


