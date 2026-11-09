"""Optical spacing scan of Gensokyo Sans: proposes kerning pairs from the outlines, the way the second pass in src/spacing.py was made.

    python tools/kernscan.py [--font build/GensokyoSans-Regular.ttf] [--pairs]      (needs fontTools)

What it measures. For two glyphs side by side (the left one's advance, plus the kerning, plus the right one's left edge), the horizontal gap between their
outlines is taken every 10 units of height from the baseline to the cap height (to the x-height for two lower-case letters); a row where only one of the two
has ink counts as a gap of 400 units (the open space of an A or a T is real, but not infinite). The mean of those gaps is the pair's "optical gap". Its median
over all upper/lower/mixed pairs of letters is the reference: a pair far above the reference leaves a hole, which kerning closes.

The rule. The first pass (the 61 legacy pairs, kept as they are) closed, on average, about 55% of the excess over the reference. The second pass gives the pairs
the spec asks for (the font specification section 6: A V W Y T P F L against A V W Y T O C G U c e o a d q s y v w , . - : ; ' " and r against , . - a c d e g o q)
and the pairs that appear in interface text the same treatment: kern = -55% of the excess, in steps of 2 units, nothing below 30 units of excess (a pair
that close is balanced), nothing beyond -100.
"""
import statistics
import sys
from pathlib import Path

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parents[1]
DEPTH = 400
STEP = 10
FRACTION = 0.55
MIN_EXCESS = 30
MAX_KERN = 100

LEFT = "AFLPTVWYrfk7"
RIGHT_LOWER = "acdegoqsvwyuirnmpxzjtfkblh"
RIGHT_UPPER = "ACGOQTUVWYSJ"
RIGHT_PUNCT = ",.-:;'\""


def flatten(gs, g):
    pen = DecomposingRecordingPen(gs)
    gs[g].draw(pen)
    polys, cur = [], []
    for op, args in pen.value:
        if op == "moveTo":
            cur = [args[0]]
        elif op == "lineTo":
            cur.append(args[0])
        elif op == "qCurveTo":
            pts = list(args)
            p0 = cur[-1]
            offs, end = pts[:-1], pts[-1]
            for i, c in enumerate(offs):
                e = ((c[0] + offs[i + 1][0]) / 2, (c[1] + offs[i + 1][1]) / 2) if i < len(offs) - 1 else end
                for t in range(1, 9):
                    u = t / 8
                    cur.append(((1 - u) ** 2 * p0[0] + 2 * (1 - u) * u * c[0] + u * u * e[0], (1 - u) ** 2 * p0[1] + 2 * (1 - u) * u * c[1] + u * u * e[1]))
                p0 = e
        elif op in ("closePath", "endPath"):
            if len(cur) > 1:
                polys.append(cur)
            cur = []
    return polys


def extents(polys, ys):
    out = []
    for y in ys:
        xs = []
        for poly in polys:
            for i in range(len(poly)):
                (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % len(poly)]
                if (y0 <= y < y1) or (y1 <= y < y0):
                    xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        out.append((min(xs), max(xs)) if xs else None)
    return out


class Scan:
    def __init__(self, path):
        self.font = TTFont(path)
        self.cmap = self.font.getBestCmap()
        gs = self.font.getGlyphSet()
        hm = self.font["hmtx"]
        self.xh, self.cap = self.font["OS/2"].sxHeight, self.font["OS/2"].sCapHeight
        self.ys = list(range(0, self.cap + 1, STEP))
        self.data = {}
        for c in self.cmap:
            if c < 128 and chr(c).isprintable() and chr(c) != " ":
                g = self.cmap[c]
                self.data[chr(c)] = (g, hm[g][0], extents(flatten(gs, g), self.ys))
        self.kern = {}
        if "kern" in self.font:
            for st in self.font["kern"].kernTables:
                self.kern.update(st.kernTable)

    def gap(self, a, b, kerned=True):
        ga, adva, ea = self.data[a]
        gb, _, eb = self.data[b]
        k = self.kern.get((ga, gb), 0) if kerned else 0
        top = self.xh if (a.islower() and b.islower()) else self.cap
        tot, n = 0.0, 0
        for i, y in enumerate(self.ys):
            if y > top:
                break
            if ea[i] is None and eb[i] is None:
                continue
            g = DEPTH if ea[i] is None or eb[i] is None else adva + k + eb[i][0] - ea[i][1]
            tot += min(g, DEPTH)
            n += 1
        return tot / n if n else None

    def reference(self):
        pairs = {}
        for a in self.data:
            for b in self.data:
                if a.isalpha() and b.isalpha():
                    pairs[a + b] = self.gap(a, b, False)
        def med(f):
            return statistics.median(v for p, v in pairs.items() if f(p))
        return {"ll": med(lambda p: p[0].islower() and p[1].islower()), "uu": med(lambda p: p[0].isupper() and p[1].isupper()),
                "ul": med(lambda p: p[0].isupper() and p[1].islower())}

    def klass(self, a, b):
        if b in RIGHT_PUNCT:
            return "ul" if a.isupper() else "ll"
        return "ll" if a.islower() and b.islower() else "uu" if a.isupper() and b.isupper() else "ul" if a.isupper() else "ll"

    def proposals(self):
        ref = self.reference()
        out = {}
        for a in LEFT:
            for b in RIGHT_LOWER + RIGHT_UPPER + RIGHT_PUNCT:
                if b not in self.data or a == b:
                    continue
                if (self.data[a][0], self.data[b][0]) in self.kern:
                    continue                                       # the designer's value stands
                s = self.gap(a, b, False)
                if s is None:
                    continue
                excess = s - ref[self.klass(a, b)]
                if excess < MIN_EXCESS:
                    continue
                k = -min(MAX_KERN, int(round(FRACTION * excess / 2.0)) * 2)
                if k:
                    out[a + b] = k
        return out, ref


if __name__ == "__main__":
    font = HERE / "build" / "GensokyoSans-Regular.ttf"
    if "--font" in sys.argv:
        font = Path(sys.argv[sys.argv.index("--font") + 1])
    scan = Scan(font)
    props, ref = scan.proposals()
    print("reference gaps: %s; %d proposed pairs" % ({k: round(v) for k, v in ref.items()}, len(props)))
    if "--pairs" in sys.argv:
        for p, k in sorted(props.items()):
            print("  %s %d   (gap %.0f)" % (p, k, scan.gap(p[0], p[1], False)))
    else:
        print('"' + ', "'.join("%s\": %d" % (p, k) for p, k in sorted(props.items())) + '"')
