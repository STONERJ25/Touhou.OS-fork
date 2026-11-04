"""Tests of the font generator in assets/fonts/source/GensokyoSansUI: its geometry (path parsing, transforms, the boolean union that keeps curves, the outline
offset used for the Bold) and that the generator, run again, makes the fonts that are shipped in assets/fonts (the same glyphs, advances, code points and kerning).

    python tools/hosttest.py tests.host.test_fontgen -v          (Python only; the font checks need fontTools and skip themselves without it)
"""
import importlib.util
import math
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "assets" / "fonts" / "source" / "GensokyoSansUI"
sys.path.insert(0, str(SRC / "src"))

import geom as G  # noqa: E402

try:
    from fontTools.ttLib import TTFont
    HAVE_FONTTOOLS = True
except ImportError:                                                           # pragma: no cover
    HAVE_FONTTOOLS = False


def rect(l, b, r, t):
    return "M%d %d L%d %d L%d %d L%d %d Z" % (l, b, l, t, r, t, r, b)


def ink_area(shape, step=1.0):
    """The ink of a shape by counting sample points with a non-zero winding number (independent of the signed areas of the contours)."""
    polys = [G.flatten(c, 32) for c in shape]
    x0, y0, x1, y1 = G.ink_bbox(shape)
    n = 0
    y = y0 + step / 2
    while y < y1:
        x = x0 + step / 2
        while x < x1:
            if G.winding((x, y), polys) != 0:
                n += 1
            x += step
        y += step
    return n * step * step


class Geometry(unittest.TestCase):
    def test_paths_parse_and_close(self):
        c = G.parse_contour("M0 0 L100 0 Q150 50 100 100 L0 100 Z")
        self.assertEqual([len(s) for s in c], [2, 3, 2, 2])
        self.assertEqual(c[-1], ((0.0, 100.0), (0.0, 0.0)))                       # the closing line is a segment of its own

    def test_outer_contours_run_clockwise_and_counters_counter_clockwise(self):
        shape = G.make_shape(rect(0, 0, 100, 100), "!" + rect(25, 25, 75, 75))
        self.assertLess(G.area(shape[0]), 0)
        self.assertGreater(G.area(shape[1]), 0)

    def test_quadratic_split_is_exact(self):
        s = ((0.0, 0.0), (60.0, 120.0), (200.0, 0.0))
        a, b = G.seg_split(s, 0.3)
        for t in (0.0, 0.25, 0.5, 1.0):
            pa, pb = G.seg_pt(a, t), G.seg_pt(s, 0.3 * t)
            self.assertAlmostEqual(pa[0], pb[0], places=9)
            self.assertAlmostEqual(pa[1], pb[1], places=9)
            pa, pb = G.seg_pt(b, t), G.seg_pt(s, 0.3 + 0.7 * t)
            self.assertAlmostEqual(pa[0], pb[0], places=9)
            self.assertAlmostEqual(pa[1], pb[1], places=9)

    def test_union_of_overlapping_rectangles(self):
        u = G.union(G.make_shape(rect(0, 0, 100, 100)), G.make_shape(rect(50, 50, 150, 150)))
        self.assertEqual(len(u), 1)
        self.assertEqual(sorted(G.to_path(G.rnd(u))), ["M0 0 L0 100 L50 100 L50 150 L150 150 L150 50 L100 50 L100 0 L0 0 Z"])
        self.assertAlmostEqual(abs(G.area(u[0])), 10000 + 10000 - 2500)

    def test_union_keeps_a_counter_that_a_bar_crosses_and_drops_one_it_fills(self):
        ring = G.make_shape("M0 100 Q100 100 100 0 Q100 -100 0 -100 Q-100 -100 -100 0 Q-100 100 0 100 Z",
                            "!M0 50 Q-50 50 -50 0 Q-50 -50 0 -50 Q50 -50 50 0 Q50 50 0 50 Z")
        bar = G.make_shape(rect(-20, -150, 20, 150))
        u = G.union(ring, bar)
        self.assertEqual(len(u), 3)                                               # the outer outline, and the counter cut in two by the bar
        self.assertEqual(G.self_intersections(u), [])
        fat = G.make_shape(rect(-70, -70, 70, 70))                                # a block that covers the whole counter
        self.assertEqual(len(G.union(ring, fat)), 1)

    def test_union_area_is_what_sampling_says(self):
        """The ink of the union equals the ink of the parts counted point by point, less what they share: tested on curves, with a counter."""
        a = G.make_shape("M0 100 Q100 100 100 0 Q100 -100 0 -100 Q-100 -100 -100 0 Q-100 100 0 100 Z",
                         "!M0 50 Q-50 50 -50 0 Q-50 -50 0 -50 Q50 -50 50 0 Q50 50 0 50 Z")
        b = G.translate(a, 90, 30)
        u = G.union(a, b)
        # the same grid of points in the parts and in the union: a point is ink in the union exactly when it is ink in one of the parts (the few points
        # within a flattening error of an edge may differ: the union's pieces are flattened piece by piece)
        pa, pb, pu = ([G.flatten(c, 32) for c in s] for s in (a, b, u))
        x0, y0, x1, y1 = -110, -110, 200, 140
        total = wrong = 0
        for iy in range(y0, y1):
            for ix in range(x0, x1):
                p = (ix + 0.5, iy + 0.5)
                total += 1
                wrong += (G.winding(p, pu) != 0) != (G.winding(p, pa) != 0 or G.winding(p, pb) != 0)
        self.assertLess(wrong, total // 2000, "%d of %d points disagree" % (wrong, total))
        self.assertEqual(G.self_intersections(u), [])

    def test_embolden_grows_a_square_by_the_pen(self):
        sq = G.make_shape(rect(100, 100, 300, 300))
        fat = G.embolden(sq, 20, 10)
        box = G.shape_bbox(fat)
        self.assertEqual(tuple(round(v) for v in box), (80, 90, 320, 310))
        thin = G.embolden(sq, -20, -10)                                           # a negative pen shrinks the ink
        self.assertEqual(tuple(round(v) for v in G.shape_bbox(thin)), (120, 110, 280, 290))

    def test_embolden_moves_the_counter_inwards_and_keeps_the_curves_clean(self):
        ring = G.make_shape("M0 100 Q100 100 100 0 Q100 -100 0 -100 Q-100 -100 -100 0 Q-100 100 0 100 Z",
                            "!M0 60 Q-60 60 -60 0 Q-60 -60 0 -60 Q60 -60 60 0 Q60 60 0 60 Z")
        fat = G.embolden(ring, 15)
        outer, inner = G.shape_bbox([fat[0]]), G.shape_bbox([fat[1]])
        self.assertEqual(tuple(round(v) for v in outer), (-115, -115, 115, 115))
        self.assertEqual(tuple(round(v) for v in inner), (-45, -45, 45, 45))
        self.assertEqual(G.self_intersections(fat), [])

    def test_rounding_removes_collapsed_and_short_pieces(self):
        c = [((0.0, 0.0), (100.2, 0.1)), ((100.2, 0.1), (100.4, 3.0)), ((100.4, 3.0), (100.0, 100.0)), ((100.0, 100.0), (0.0, 100.0)), ((0.0, 100.0), (0.0, 0.0))]
        out = G.rnd([c], min_len=6)
        self.assertEqual(len(out), 1)
        self.assertTrue(all(len(s) == 2 for s in out[0]))
        for s in out[0]:
            self.assertGreaterEqual(math.hypot(s[1][0] - s[0][0], s[1][1] - s[0][1]), 6)
        self.assertEqual(out[0][0][0], out[0][-1][1])                              # still a closed loop

    def test_clip_box_cuts_a_polygon_to_a_box_and_keeps_it_closed(self):
        tri = G.make_shape("M-50 -50 L150 -50 L50 150 Z")                           # a triangle sticking out of the box (0, 0, 100, 100) on three sides
        out = G.clip_box(tri, 0, 0, 100, 100)
        self.assertEqual(len(out), 1)
        box = G.shape_bbox(out)
        self.assertEqual(tuple(round(v) for v in box), (0, 0, 100, 100))
        self.assertTrue(all(0 <= v <= 100 for contour in out for seg in contour for p in seg for v in p))
        for contour in out:
            for a, b in zip(contour, contour[1:] + contour[:1]):
                self.assertEqual(a[-1], b[0])                                        # every segment starts where the one before it ended
        self.assertEqual(G.clip_box(G.make_shape(rect(200, 200, 300, 300)), 0, 0, 100, 100), [])   # nothing inside: nothing left
        inside = G.make_shape(rect(10, 10, 20, 20))
        self.assertAlmostEqual(abs(G.area(G.clip_box(inside, 0, 0, 100, 100)[0])), 100)

    def test_transforms_keep_the_ink_on_the_right(self):
        c = G.make_shape("M0 0 L0 100 L50 100 L50 0 Z")
        for m in ((-1, 0, 0, 1, 50, 0), (1, 0, 0, -1, 0, 100), (-1, 0, 0, -1, 50, 100)):
            self.assertLess(G.area(G.transform(c, m)[0]), 0)


@unittest.skipUnless(HAVE_FONTTOOLS, "fontTools not installed")
class GeneratorMakesTheShippedFonts(unittest.TestCase):
    """src/build.py, run again, makes the glyphs, advances, code points and kerning of the Gensokyo Sans files in assets/fonts (what is shipped is what the source
    makes; byte-for-byte equality is not asked, since fontTools versions order a few table fields differently)."""

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("gensokyo_build", SRC / "src" / "build.py")
        cls.build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.build)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.paths = cls.build.main(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_each_built_face_equals_the_shipped_one(self):
        for built in self.paths:
            shipped = REPO / "assets" / "fonts" / built.name
            self.assertTrue(shipped.exists(), built.name)
            a, b = TTFont(built), TTFont(shipped)
            self.assertEqual(a.getGlyphOrder(), b.getGlyphOrder(), built.name)
            self.assertEqual(a.getBestCmap(), b.getBestCmap(), built.name)
            self.assertEqual(dict(a["hmtx"].metrics), dict(b["hmtx"].metrics), built.name)
            if built.name.startswith("GensokyoMono"):
                self.assertNotIn("kern", a.reader.keys())                               # (a fixed-width face has no kerning)
                self.assertNotIn("kern", b.reader.keys())
            else:
                self.assertEqual(a["kern"].kernTables[0].kernTable, b["kern"].kernTables[0].kernTable, built.name)
            for name in a.getGlyphOrder():
                ga, gb = a["glyf"][name], b["glyf"][name]
                self.assertEqual(ga.numberOfContours, gb.numberOfContours, name)
                if ga.numberOfContours > 0:
                    self.assertEqual((list(ga.coordinates), list(ga.flags), list(ga.endPtsOfContours)), (list(gb.coordinates), list(gb.flags), list(gb.endPtsOfContours)), name)
                elif ga.numberOfContours < 0:
                    self.assertEqual([(c.glyphName, c.x, c.y) for c in ga.components], [(c.glyphName, c.x, c.y) for c in gb.components], name)

    def test_the_checks_of_the_source_pass(self):
        spec = importlib.util.spec_from_file_location("gensokyo_check", SRC / "tools" / "check.py")
        sys.path.insert(0, str(SRC / "tools"))
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        errors = []
        for built in self.paths:
            check.check_face(built, built.stem.split("-")[1], errors, mono=built.name.startswith("GensokyoMono"))
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
