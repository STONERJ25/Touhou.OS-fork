"""Four new display treatments built from original Touhou.OS design sources."""
import math
from pathlib import Path
import sys

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.svgLib.path import parse_path

FAMILY = Path(__file__).resolve().parents[2]
sys.path.extend(str(FAMILY / package / "src") for package in ("GensokyoDisplayLab", "GensokyoSansUI"))
from designs import OFUDA, PATTERNS
from outlines import DIGITS
from ui_outlines import UPPERCASE


def poly(points):
    return "M" + " L".join(f"{round(x)} {round(y)}" for x, y in points) + " Z"


def transform(path, matrix):
    pen = SVGPathPen(None, ntos=lambda v: str(round(v)))
    parse_path(path.removeprefix("!"), TransformPen(pen, matrix))
    return ("!" if path.startswith("!") else "") + pen.getCommands()


def bounds(paths):
    pen = BoundsPen(None)
    for path in paths:
        parse_path(path.removeprefix("!"), pen)
    return pen.bounds


def mischief():
    result = {}
    rotations = (-9, 8, -5, 6, 3, -7, 7)
    for index, (char, (_, *paths)) in enumerate(OFUDA.items()):
        angle = math.radians(rotations[index % len(rotations)])
        cos, sin = math.cos(angle), math.sin(angle)
        scale_x = (0.86, 1.01, 0.90, 0.98)[index % 4]
        scale_y = (0.88, 0.98, 0.93)[index % 3]
        shifted = [transform(p, (cos * scale_x, sin * scale_x, -sin * scale_y,
                                 cos * scale_y, 0, 0)) for p in paths]
        left, bottom, right, _ = bounds(shifted)
        baseline = (-43, 48, 10, -19, 33)[index % 5]
        if not char.isalnum():
            baseline = bounds(paths)[1] * scale_y
        final = [transform(p, (1, 0, 0, 1, 26 - left, baseline - bottom)) for p in shifted]
        result[char] = (round(right - left + 52), *final)
    return result


def seals():
    result = {}
    tile = poly(((36, 28), (90, -26), (650, -26), (704, 28),
                 (704, 678), (650, 732), (90, 732), (36, 678)))
    for char, (_, *paths) in (UPPERCASE | DIGITS).items():
        left, bottom, right, top = bounds(paths)
        scale = min(487 / (right - left), 545 / (top - bottom))
        matrix = (scale, 0, 0, scale, 370 - scale * (right + left) / 2,
                  353 - scale * (top + bottom) / 2)
        cutouts = []
        for path in paths:
            shifted = transform(path, matrix)
            cutouts.append(shifted[1:] if shifted.startswith("!") else "!" + shifted)
        result[char] = (740, tile, *cutouts)
    result.update({char: outline for char, outline in OFUDA.items() if not char.isalnum()})
    return result


def crystal_paths(rows):
    # Cancel shared cell edges. Follow each perimeter with ink on its left.
    edges = set()
    for row, cells in enumerate(rows):
        y = 6 - row
        for x, cell in enumerate(cells):
            if cell == ".":
                continue
            corners = [(x, y), (x+1, y), (x+1, y+1), (x, y+1)]
            for a, b in zip(corners, corners[1:] + corners[:1]):
                if (b, a) in edges:
                    edges.remove((b, a))
                else:
                    edges.add((a, b))
    while edges:
        a, b = min(edges)
        start = a
        corners = [a]
        edges.remove((a, b))
        while b != start:
            dx, dy = b[0] - a[0], b[1] - a[1]
            for u, v in [(-dy, dx), (dx, dy), (dy, -dx)]:
                c = (b[0]+u, b[1]+v)
                if (b, c) in edges:
                    edges.remove((b, c))
                    if (u, v) != (dx, dy):
                        corners.append(b)
                    a, b = b, c
                    break
            else:
                raise ValueError("Open crystal perimeter")
        if ((corners[-1][0] == start[0] == corners[1][0])
                or (corners[-1][1] == start[1] == corners[1][1])):
            corners.pop(0)
        closed = corners[1:] + corners[:1]
        hole = sum(x*v - y*u for (x, y), (u, v) in zip(corners, closed)) < 0

        def clip(point, toward):
            x, y = point
            dx, dy = toward[0] - x, toward[1] - y
            return 45 + x * 98 + (20 if dx > 0 else -20 if dx < 0 else 0), (
                y * 100 + (24 if dy > 0 else -24 if dy < 0 else 0)
            )

        clipped = [p for i, corner in enumerate(corners)
                   for p in (clip(corner, corners[i-1]), clip(corner, closed[i]))]
        yield ("!" if hole else "") + poly(clipped)


def frost():
    return {char: (90 + len(pattern.split("/")[0]) * 98, *crystal_paths(pattern.split("/")))
            for char, pattern in PATTERNS.items()}


# Nine endpoints form the envelope and split middle of the status display.
NODES = {
    "a": (65, 690), "b": (290, 690), "c": (515, 690),
    "d": (65, 350), "e": (290, 350), "f": (515, 350),
    "g": (65, 10), "h": (290, 10), "i": (515, 10),
}
SEGMENTS = ("ac", "ad", "cf", "de", "ef", "dg", "fi", "gi", "ae", "ce", "eg", "ei", "be", "eh", "dh", "fh")
CODES = {
    "A": "ac ad cf de ef dg fi", "B": "ac ad cf de ef dg fi gi",
    "C": "ac ad dg gi", "D": "ac cf fi gi be eh", "E": "ac ad de ef dg gi",
    "F": "ac ad de ef dg", "G": "ac ad dg gi fi ef", "H": "ad cf de ef dg fi",
    "I": "ac be eh gi", "J": "cf fi gi dg", "K": "ad dg ce ei", "L": "ad dg gi",
    "M": "ad dg cf fi ae ce", "N": "ad dg cf fi ae ei",
    "O": "ac ad cf dg fi gi", "P": "ac ad cf de ef dg", "Q": "ac ad cf dg fi gi ei",
    "R": "ac ad cf de ef dg ei", "S": "ac ad de ef fi gi", "T": "ac be eh",
    "U": "ad cf dg fi gi", "V": "ad cf dh fh", "W": "ad dg cf fi eg ei",
    "X": "ae ce eg ei", "Y": "ae ce eh", "Z": "ac ce eg gi",
    "0": "ac ad cf dg fi gi ce eg", "1": "cf fi", "2": "ac cf de ef dg gi",
    "3": "ac cf de ef fi gi", "4": "ad cf de ef fi", "5": "ac ae ef fi gi",
    "6": "ac ad de ef dg fi gi", "7": "ac cf fi", "8": "ac ad cf de ef dg fi gi ae",
    "9": "ac ad cf de ef fi gi", "-": "de ef", "/": "ce eg",
}


def setback(node, other, keys):
    x, y = NODES[node]
    u, v = NODES[other]
    angle = math.atan2(v - y, u - x)
    distances = [42]
    for key in keys:
        if node not in key or other in key:
            continue
        p, q = NODES[key.replace(node, "")]
        delta = math.atan2(q - y, p - x) - angle
        separation = abs(math.remainder(delta, math.tau))
        if separation < math.pi / 2:
            distances.append(math.ceil(31 / math.tan(separation / 2)) - 11)
    return max(distances)


def segment(key, keys):
    assert key in SEGMENTS
    x1, y1 = NODES[key[0]]
    x2, y2 = NODES[key[1]]
    length = math.hypot(x2-x1, y2-y1)
    dx, dy = (x2-x1)/length, (y2-y1)/length
    start, stop = setback(*key, keys), setback(*reversed(key), keys)
    a, b = (x1+dx*start, y1+dy*start), (x2-dx*stop, y2-dy*stop)
    ax, ay = a
    bx, by = b
    nx, ny = -dy*27, dx*27
    return poly((a, (ax+dx*14+nx, ay+dy*14+ny), (bx-dx*14+nx, by-dy*14+ny),
                 b, (bx-dx*14-nx, by-dy*14-ny), (ax+dx*14-nx, ay+dy*14-ny)))


def kappa():
    result = {}
    for char, code in CODES.items():
        keys = code.split()
        result[char] = (580, *(segment(key, keys) for key in keys))
    result.update({char: OFUDA[char] for char in "!?:.,"})
    return result
