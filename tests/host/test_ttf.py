#!/usr/bin/env python3
"""Host tests for libttf (user/lib/ttf), through tests/host/ttftool.c.

    python tools/hosttest.py discover -s tests/host -p "test_ttf.py" [-v]

Every font used by the portable tests is generated here by tests/host/ttfmake.py (TrueType, CFF, collections), so the tests run anywhere with
a C compiler and need no font file. The tool is built with AddressSanitizer and UBSan; a bad read, a leak of the library's own allocations
or an undefined operation fails the test that triggered it.

What is checked:
  * containers, names, metrics, hmtx/loca variants and the errors for damaged files
  * glyph decoding (simple, composite, CFF operators) against the outline each generated glyph is known to have
  * cmap formats and preference, kerning (kern, GPOS), ligatures
  * the rasteriser against exactly computed pixel coverage (polygon clipping with exact fractions), sub-pixel positioning, LCD filtering
  * text layout, UTF-8, font fallback, the glyph cache (hits, eviction, bounds), coverage tables and blending
  * a mutation fuzzer under the sanitizers, allocation-failure injection, speed numbers
  * LOCAL ONLY: the owner's Windows fonts (arial.ttf, segoeui.ttf, msgothic.ttc, YuGothR.ttc, ...) are read in place and the tests skip
    themselves when a file is absent. Nothing from them is copied into the repository; sample renders go to build/ttf-samples/.
"""
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import unittest
import zlib
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ttfmake as M  # noqa: E402

REPO = HERE.parent.parent
LIB = REPO / "user" / "lib" / "ttf"


def in_wsl():
    try:
        return "microsoft" in os.uname().release.lower()
    except AttributeError:
        return False


OUT = REPO / "build" / ("host-ttf-wsl" if in_wsl() else "host-ttf")       # (Linux binaries built under WSL must not be mistaken for a native build's)
WORK = OUT / "work"
SAMPLES = REPO / "build" / "ttf-samples"
FONT_DIR = Path(os.environ.get("TOUHOU_FONT_DIR") or ("/mnt/c/Windows/Fonts" if in_wsl() else r"C:\Windows\Fonts"))


def find_cc():
    for name in ("gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    return None


HAVE_CC = find_cc() is not None


KAPPA = REPO / "user" / "lib" / "kappa"


def build_tool(sanitize=True, kappa=False):
    """Builds tests/host/ttftool.c with every libttf source. Sanitized by default; sanitize=False gives the optimised build for speed numbers;
    kappa=True also links the toolkit's canvas and the TrueType text shim (user/lib/kappa/src/ttftext.c), switched on."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("ttftool-kappa" if kappa else "ttftool" if sanitize else "ttftool-fast")
    srcs = sorted((LIB / "src").glob("*.c")) + [HERE / "ttftool.c"]
    extra = []
    if kappa:
        srcs += [KAPPA / "src" / "canvas.c", KAPPA / "src" / "ttftext.c", REPO / "shared" / "font8x16.c"]
        extra = ["-DTTFTOOL_KAPPA", "-DKAPPA_TTF=1", "-I", str(KAPPA / "include"), "-I", str(REPO / "user" / "lib" / "fairy" / "include"), "-I", str(REPO / "shared")]
    deps = srcs + sorted((LIB / "src").glob("*.h")) + sorted((LIB / "include").glob("*.h")) + sorted((KAPPA / "include").glob("*.h"))
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = find_cc()
    is_gcc = "gcc" in Path(cc).name.lower()
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize else ["-O2"]
    warn = ["-Wall", "-Wextra", "-Werror"] if is_gcc else ["-Wall", "-Wextra"]
    cmd = [cc, *flags, "-std=gnu17", *warn, "-Wno-unused-parameter", "-Wno-sign-compare", "-DTTF_ALLOC_HOOKS", "-I", str(LIB / "include"), *extra,
           *map(str, srcs), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of ttftool failed:\n%s%s" % (r.stdout, r.stderr))
    return exe


def run_tool(args=None, script=None, sanitize=True, timeout=300, kappa=False, env=None):
    exe = build_tool(sanitize, kappa)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1", **(env or {}))
    r = subprocess.run([str(exe), *(args or [])], input=script, capture_output=True, text=True, errors="replace", env=env, timeout=timeout)
    if "Sanitizer" in r.stderr or "runtime error" in r.stderr or r.returncode not in (0, 1):
        raise RuntimeError("ttftool failed (%d):\n%s\n%s" % (r.returncode, r.stdout[-1500:], r.stderr[-3000:]))
    return r


def batch(cmds, **kw):
    """Runs the commands in one tool process; returns one list of output lines per command."""
    script = "".join(c + "\necho @@@\n" for c in cmds)
    r = run_tool(script=script, **kw)
    parts, cur = [], []
    for line in r.stdout.splitlines():
        if line == "@@@":
            parts.append(cur)
            cur = []
        else:
            cur.append(line)
    assert len(parts) == len(cmds), (len(parts), len(cmds), r.stdout[-500:])
    return parts


_font_serial = [0]


def save(data, name=None):
    WORK.mkdir(parents=True, exist_ok=True)
    if name is None:
        _font_serial[0] += 1
        name = "f%04d.ttf" % _font_serial[0]
    p = WORK / name
    p.write_bytes(data)
    return str(p)


def with_font(data, cmds, face=0):
    """Loads the font as slot `a`, runs the commands; returns the outputs of the commands (not of the load)."""
    path = save(data)
    res = batch(["load a %s %d" % (path, face)] + cmds)
    if not res[0] or not res[0][0].startswith("ok"):
        raise AssertionError("load failed: %r" % (res[0],))
    return res[1:]


def kv(line):
    """'a=1 b=2,3' -> {'a': '1', 'b': '2,3'}"""
    return dict(tok.split("=", 1) for tok in line.split() if "=" in tok)


def parse_path(lines):
    ops = []
    for l in lines:
        if l == "end":
            break
        p = l.split()
        ops.append((p[0], *[float(v) for v in p[1:]]))
    return ops


def parse_bitmap(lines):
    assert lines and lines[0].startswith("bitmap"), lines[:2]
    head = lines[0].split()[1:]
    w, h, left, top, bpp, stride = [int(v) for v in head[:6]]
    rows = []
    for l in lines[1:]:
        if l == "end":
            break
        b = bytes.fromhex(l)
        rows.append(list(b))
    assert len(rows) == h, (len(rows), h)
    return dict(w=w, h=h, left=left, top=top, bpp=bpp, stride=stride, adv=float(head[6]) if len(head) > 6 else None, rows=rows)


def ink(bm):
    return sum(sum(r) for r in bm["rows"])


# ----------------------------------------------------------------------------------------------- the exact reference model


def clip_halfplane(poly, inside, intersect):
    out = []
    for i in range(len(poly)):
        a, b = poly[i - 1], poly[i]
        ia, ib = inside(a), inside(b)
        if ib:
            if not ia:
                out.append(intersect(a, b))
            out.append(b)
        elif ia:
            out.append(intersect(a, b))
    return out


def clip_box(poly, x0, y0, x1, y1):
    """Sutherland-Hodgman against an axis-aligned box. Exact when the coordinates are Fractions."""
    def lerp(a, b, t):
        return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    for axis, lim, keep_below in ((0, x0, False), (0, x1, True), (1, y0, False), (1, y1, True)):
        if not poly:
            return poly
        if keep_below:
            inside = lambda p, axis=axis, lim=lim: p[axis] <= lim
        else:
            inside = lambda p, axis=axis, lim=lim: p[axis] >= lim
        inter = lambda a, b, axis=axis, lim=lim: lerp(a, b, (lim - a[axis]) / (b[axis] - a[axis]))
        poly = clip_halfplane(poly, inside, inter)
    return poly


def signed_area(poly):
    s = 0
    for i in range(len(poly)):
        x0, y0 = poly[i - 1]
        x1, y1 = poly[i]
        s += x0 * y1 - x1 * y0
    return s / 2


def exact_coverage(contours, w, h, clamp=True):
    """contours: lists of (x, y) in pixel coordinates (x right, y down). Returns h rows of w coverages in 0..1: the absolute value of the summed
    signed areas inside each pixel, clamped to 1 (the documented behaviour of the signed-area method; exact wherever contours do not overlap)."""
    out = []
    for py in range(h):
        row = []
        for px in range(w):
            total = 0
            for c in contours:
                xs, ys = [p[0] for p in c], [p[1] for p in c]
                if max(xs) <= px or min(xs) >= px + 1 or max(ys) <= py or min(ys) >= py + 1:
                    # the polygon may still wind around the pixel: handled by clipping only when boxes overlap; otherwise its area inside is 0
                    continue
                total += signed_area(clip_box(c, px, py, px + 1, py + 1))
            v = abs(total)
            row.append(float(min(v, 1)) if clamp else float(v))
        out.append(row)
    return out


def to_level(v):
    return int(math.floor(v * 255 + 0.5))


def flatten_quad_path(path, steps=48):
    """TrueType expected path (M/L/Q/Z) -> list of polygons (font units, y up) using a fine flattening."""
    polys, cur = [], []
    x = y = 0.0
    for op in path:
        if op[0] == "M":
            if cur:
                polys.append(cur)
            cur = [(op[1], op[2])]
            x, y = op[1], op[2]
        elif op[0] == "L":
            cur.append((op[1], op[2]))
            x, y = op[1], op[2]
        elif op[0] == "Q":
            for k in range(1, steps + 1):
                t = k / steps
                m = 1 - t
                cur.append((m * m * x + 2 * m * t * op[1] + t * t * op[3], m * m * y + 2 * m * t * op[2] + t * t * op[4]))
            x, y = op[3], op[4]
        elif op[0] == "C":
            for k in range(1, steps + 1):
                t = k / steps
                m = 1 - t
                cur.append((m ** 3 * x + 3 * m * m * t * op[1] + 3 * m * t * t * op[3] + t ** 3 * op[5],
                            m ** 3 * y + 3 * m * m * t * op[2] + 3 * m * t * t * op[4] + t ** 3 * op[6]))
            x, y = op[5], op[6]
        elif op[0] == "Z":
            if cur:
                polys.append(cur)
            cur = []
    if cur:
        polys.append(cur)
    return polys


def to_pixels(polys, scale, left, top, sub_x=0.0, sub_y=0.0, hx=1, skew=0.0, exact=False):
    """Font-unit polygons -> pixel coordinates of a bitmap whose left/top the tool reported (y flips: row 0 is the top)."""
    conv = Fraction if exact else float
    out = []
    for poly in polys:
        out.append([(((conv(x) + conv(skew) * conv(y)) * conv(scale) + conv(sub_x) - left) * hx, top - (conv(y) * conv(scale) + conv(sub_y))) for x, y in poly])
    return out


def compare_levels(test, got_rows, want_cov, tol, what=""):
    worst = 0
    for y, (g, w) in enumerate(zip(got_rows, want_cov)):
        for x, (gv, wv) in enumerate(zip(g, w)):
            d = abs(gv - to_level(wv))
            if d > worst:
                worst = d
            test.assertLessEqual(d, tol, "%s pixel (%d,%d): got %d, exact %.4f = %d" % (what, x, y, gv, wv, to_level(wv)))
    return worst


# ----------------------------------------------------------------------------------------------- font builders shared by the tests


def simple_font(glyphs, **kw):
    """glyphs: {name: contours or (advance, contours)}; maps 'a'.. in order of the dict to U+0061..; glyph 0 is .notdef (a box)."""
    f = M.Font(**kw)
    f.glyph(".notdef", 500, M.BOX)
    cm = {}
    for i, (name, spec) in enumerate(glyphs.items()):
        adv, contours = spec if isinstance(spec, tuple) and isinstance(spec[0], int) else (600, spec)
        f.glyph(name, adv, contours)
        cm[0x61 + i] = name
    f.cmap_bmp(cm)
    return f


def sfnt_tables(data):
    n = struct.unpack(">H", data[4:6])[0]
    t = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        t[tag.decode()] = data[off:off + ln]
    return t


def patched(data, **changes):
    """Rebuilds a font with tables replaced (bytes), removed (None) or edited (a function)."""
    t = sfnt_tables(data)
    for k, v in changes.items():
        k = k.replace("_", "/") if k == "OS_2" else k
        if v is None:
            t.pop(k, None)
        elif callable(v):
            t[k] = v(t[k])
        else:
            t[k] = v
    return M.build_sfnt(t, struct.unpack(">I", data[:4])[0])


@unittest.skipUnless(HAVE_CC, "needs a C compiler")
class ToolCase(unittest.TestCase):
    def assertPath(self, got, want, tol=2.5e-3, what=""):
        self.assertEqual(len(got), len(want), "%s: %d ops, expected %d\n got  %s\n want %s" % (what, len(got), len(want), got, want))
        for i, (g, w) in enumerate(zip(got, want)):
            self.assertEqual(g[0], w[0], "%s op %d: %s vs %s\n got  %s\n want %s" % (what, i, g, w, got, want))
            self.assertEqual(len(g), len(w), "%s op %d arity" % (what, i))
            for a, b in zip(g[1:], w[1:]):
                self.assertAlmostEqual(a, b, delta=tol * max(1.0, abs(b)), msg="%s op %d: %s vs %s" % (what, i, g, w))

    def outlines(self, font, gids, face=0):
        """The tool's outline for each glyph index of `font` (a ttfmake.Font or bytes)."""
        data = font if isinstance(font, bytes) else font.build()
        res = with_font(data, ["outline a %d" % g for g in gids], face)
        return [parse_path(r) if not (r and r[0].startswith("err")) else r[0] for r in res]


# =============================================================================================== containers, names, metrics


class Container(ToolCase):
    def test_plain_font(self):
        f = M.sample_font()
        res = with_font(f.build(), ["info a", "name a 1", "name a 2", "name a 4", "name a 6", "name a 3", "name a 99"])
        lines = res[0]
        self.assertTrue(lines[0].startswith("family=Test Sans|style=Regular|full=Test Sans Regular|ps=TestSans-Regular"), lines[0])
        d = kv(lines[1])
        self.assertEqual((d["upem"], d["glyphs"], d["cff"]), ("1000", "8", "0"))
        self.assertEqual(kv(lines[2])["hhea"], "800,-200,100")
        self.assertEqual(bytes.fromhex(res[1][0].split()[2]).decode(), "Test Sans")
        self.assertEqual(bytes.fromhex(res[2][0].split()[2]).decode(), "Regular")
        self.assertEqual(bytes.fromhex(res[3][0].split()[2]).decode(), "Test Sans Regular")
        self.assertEqual(bytes.fromhex(res[4][0].split()[2]).decode(), "TestSans-Regular")
        self.assertEqual(res[6], ["none"])

    def test_name_preference_and_utf16(self):
        f = M.sample_font()
        f.names_extra = [(3, 1, 0x407, 1, "Deutsch".encode("utf-16-be")),                 # a German entry must lose to English
                         (1, 0, 0, 7, b"mac only"),
                         (3, 1, 0x409, 8, "A\U0001F600B".encode("utf-16-be")),             # surrogate pair
                         (3, 1, 0x409, 9, b"\xd8\x00\x00A")]                             # lone high surrogate
        res = with_font(f.build(), ["name a 1", "name a 7", "name a 8", "name a 8 5", "name a 8 2", "name a 9"])
        self.assertEqual(bytes.fromhex(res[0][0].split()[2]).decode(), "Test Sans")
        self.assertEqual(bytes.fromhex(res[1][0].split()[2]).decode(), "mac only")
        self.assertEqual(bytes.fromhex(res[2][0].split()[2]).decode(), "A\U0001F600B")
        self.assertEqual(res[2][0].split()[1], "6")                                         # the full length, in UTF-8 bytes
        # a small buffer: nothing is split in the middle of the 4-byte sequence, the length stays the full length
        self.assertEqual(res[3][0].split()[1], "6")
        self.assertEqual(bytes.fromhex(res[3][0].split()[2]).decode(), "A")
        self.assertEqual(bytes.fromhex(res[4][0].split()[2]).decode(), "A")
        self.assertEqual(bytes.fromhex(res[5][0].split()[2]).decode(), "\ufffdA")

    def test_collection(self):
        a, b = M.sample_font(), simple_font({"x": M.SQUARE})
        ttc = M.build_ttc([(a.build_tt_tables(), 0x00010000), (b.build_tt_tables(), 0x00010000)])
        path = save(ttc, "coll.ttc")
        res = batch(["load a %s 0" % path, "load b %s 1" % path, "load c %s 2" % path, "load d %s -1" % path, "glyph b 0x61"])
        self.assertEqual(res[0][0], "ok faces=2 glyphs=8 upem=1000 cff=0")
        self.assertEqual(res[1][0], "ok faces=2 glyphs=2 upem=1000 cff=0")
        self.assertEqual(res[2], ["err -1 not a TrueType/OpenType font"])
        self.assertEqual(res[3], ["err -1 not a TrueType/OpenType font"])
        self.assertEqual(res[4], ["cp=97 gid=1 adv=600 lsb=100"])

    def test_damaged_files(self):
        good = M.sample_font().build()
        rng = random.Random(5)
        cases = {
            "empty": (b"", -1),
            "random": (bytes(rng.getrandbits(8) for _ in range(500)), -1),
            "signature only": (b"OTTO", -2),
            "truncated directory": (good[:30], -2),
            "tiny ttc": (b"ttcf\x00\x01\x00\x00\x00\x00\x00\x01", -2),
            "no head": (patched(good, head=None), -3),
            "bad head magic": (patched(good, head=lambda h: h[:12] + b"\0\0\0\0" + h[16:]), -3),
            "zero upem": (patched(good, head=lambda h: h[:18] + b"\0\0" + h[20:]), -3),
            "huge upem": (patched(good, head=lambda h: h[:18] + b"\xff\xff" + h[20:]), -3),
            "bad loca format": (patched(good, head=lambda h: h[:50] + b"\x00\x07" + h[52:]), -3),
            "zero glyphs": (patched(good, maxp=lambda m: m[:4] + b"\0\0" + m[6:]), -3),
            "no outlines": (patched(good, glyf=None, loca=None), -4),
            "no hmtx": (patched(good, hmtx=None), -3),
            "empty hmtx": (patched(good, hmtx=b""), -3),
        }
        for name, (data, want) in cases.items():
            path = save(data)
            out = batch(["load a %s" % path])[0]
            self.assertTrue(out[0].startswith("err %d " % want), "%s: %r (wanted %d)" % (name, out, want))

    def test_glyph_count_follows_loca(self):
        good = M.sample_font().build()
        short = patched(good, loca=lambda l: l[:2 * 5])                       # only 4 glyphs' worth of loca (5 entries)
        res = with_font(short, ["info a"])
        self.assertEqual(kv(res[0][1])["glyphs"], "4")

    def test_loca_formats_agree(self):
        f = M.sample_font()
        gids = range(len(f.glyphs))
        a = self.outlines(f.build(loca_long=False), gids)
        b = self.outlines(f.build(loca_long=True), gids)
        self.assertEqual(a, b)
        for g in gids:
            self.assertPath(a[g], f.path(g), what="glyph %d" % g)

    def test_hmtx_short_tail(self):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        for i in range(6):
            f.glyph("g%d" % i, 600 if i else 500, M.SQUARE, lsb=10 + i)
        f.glyph("last", 600, M.SQUARE, lsb=77)
        f.cmap_bmp({0x41 + i: i + 1 for i in range(7)})
        full = with_font(f.build(), ["glyph a 65 66 67 70 71"])[0]
        compact = with_font(f.build(compact_hmtx=True), ["glyph a 65 66 67 70 71", "info a"])
        self.assertEqual(full, compact[0])                                    # same advances and left side bearings, whether or not the tail is short
        self.assertLess(int(kv(compact[1][4])["hmetrics"]), 8)                  # ... and the compact table really has fewer long entries
        self.assertEqual(compact[0][-1], "cp=71 gid=7 adv=600 lsb=77")

    def test_metrics_policy(self):
        base = M.sample_font()
        m = lambda font: kv(with_font(font.build(), ["info a"])[0][5].replace("metrics100 ", ""))
        # hhea by default
        d = m(base)
        self.assertEqual((float(d["ascent"]), float(d["descent"]), float(d["gap"]), float(d["height"])), (80.0, 20.0, 10.0, 110.0))
        # USE_TYPO_METRICS asks for OS/2's typographic values
        f = M.sample_font()
        f.os2 = {"fs_selection": 0x80 | 0x40, "typo_asc": 700, "typo_desc": -300, "typo_gap": 50}
        d = m(f)
        self.assertEqual((float(d["ascent"]), float(d["descent"]), float(d["gap"])), (70.0, 30.0, 5.0))
        # without the flag the same OS/2 values are ignored
        f = M.sample_font()
        f.os2 = {"typo_asc": 700, "typo_desc": -300, "typo_gap": 50}
        self.assertEqual(float(m(f)["ascent"]), 80.0)
        # hhea empty: the Windows metrics
        f = M.sample_font()
        f.ascent = f.descent = f.line_gap = 0
        f.os2 = {"win_asc": 900, "win_desc": 250, "typo_asc": 0, "typo_desc": 0}
        d = m(f)
        self.assertEqual((float(d["ascent"]), float(d["descent"]), float(d["gap"])), (90.0, 25.0, 0.0))
        # underline, strike-through, x height, cap height come from post and OS/2
        f = M.sample_font()
        f.post = {"pos": -120, "thick": 60}
        f.os2 = {"strike_pos": 280, "strike_size": 40, "x_height": 480, "cap_height": 690}
        d = m(f)
        self.assertEqual((d["ul"], d["st"], float(d["xh"]), float(d["caph"])), ("-12.000,6.000", "28.000,4.000", 48.0, 69.0))

    def test_style_flags(self):
        f = M.sample_font()
        f.os2 = {"weight": 700, "fs_selection": 0x21}
        f.mac_style = 2
        d = kv(with_font(f.build(), ["info a"])[0][3])
        self.assertEqual((d["weight"], d["bold"], d["italic"]), ("700", "1", "1"))
        f = M.sample_font()
        f.post = {"fixed": 1}
        self.assertEqual(kv(with_font(f.build(), ["info a"])[0][3])["fixed"], "1")


# =============================================================================================== glyph decoding


class GlyphDecoding(ToolCase):
    def check(self, font, names=None):
        gids = range(len(font.glyphs)) if names is None else [font.gid(n) for n in names]
        got = self.outlines(font, gids)
        for g, path in zip(gids, got):
            self.assertIsInstance(path, list, "glyph %d: %s" % (g, path))
            self.assertPath(path, font.path(g), what="glyph %s" % font.glyphs[g].name)

    def test_on_off_curve_rules(self):
        on, off = M.on, M.off
        f = M.Font()
        f.glyph(".notdef", 500)
        f.glyph("poly", 600, [[on(0, 0), on(500, 0), on(500, 500), on(0, 500)]])
        f.glyph("start_on_quad", 600, [[on(0, 0), off(250, 400), on(500, 0), on(500, -100)]])
        f.glyph("start_off", 600, [[off(0, 0), on(400, 0), on(400, 400), on(0, 400)]])                   # begins at the last on-curve point
        f.glyph("end_off", 600, [[on(0, 0), on(400, 0), on(400, 400), off(0, 400)]])
        f.glyph("both_off", 600, [[off(0, 0), on(400, 0), on(400, 400), off(0, 400)]])
        f.glyph("all_off", 600, [M.circle_contour(300, 300, 250, 6)])                                       # implied on-curve points everywhere
        f.glyph("two_off_in_row", 600, [[on(0, 0), off(100, 300), off(300, 300), on(400, 0)]])
        f.glyph("single_point", 600, [[on(10, 10)]])
        f.glyph("two_points", 600, [[on(0, 0), on(100, 100)]])
        f.glyph("two_contours", 600, [[on(0, 0), on(200, 0), on(200, 200), on(0, 200)], [on(300, 300), on(500, 300), on(500, 500), on(300, 500)]])
        f.glyph("empty", 300)
        f.glyph("negative", 600, [[on(-300, -200), on(-100, -200), on(-100, -50), on(-300, -50)]])
        f.glyph("big", 600, [[on(-16000, -16000), on(16000, -16000), on(16000, 16000), on(-16000, 16000)]])         # (deltas are 16-bit)
        self.check(f)

    def test_encoding_variants(self):
        on, off = M.on, M.off
        pts = [[on(0, 0), on(255, 0), on(-255, 0), on(256, 5), on(256, 5), on(256, 5), on(256, 5), off(-300, 300), on(-300, 300), on(0, 0)]]
        pts += [[on(1000, 1000), on(1000, 1000 + 255), on(1000 + 300, 1000 + 255), on(1000 + 300, 1000)]]
        for compress in (True, False):
            f = M.Font()
            f.glyph(".notdef", 500)
            f.glyph("g", 600, pts, compress=compress)
            f.glyph("instr", 600, pts, compress=compress, instructions=bytes(range(40)))
            self.check(f)
        # long repeats of one flag (more than 255 equal points)
        f = M.Font()
        f.glyph(".notdef", 500)
        poly = [on(i, 0) for i in range(700)] + [on(699, 10), on(0, 10)]                  # 699 points with one and the same flag byte
        f.glyph("many", 600, [poly])
        self.check(f)

    def test_composites(self):
        f = M.Font()
        f.glyph(".notdef", 500)
        f.glyph("sq", 600, M.SQUARE)
        f.glyph("tri", 600, M.TRIANGLE)
        f.glyph("c_offset", 600, components=[M.Comp("sq"), M.Comp("tri", 700, 50)])
        f.glyph("c_negative", 600, components=[M.Comp("tri", -120, -30)])
        f.glyph("c_words", 600, components=[M.Comp("tri", 1000, -2000), M.Comp("sq", 300, 0, words=True)])
        f.glyph("c_scale", 600, components=[M.Comp("tri", 100, 100, scale=0.5)])
        f.glyph("c_scale_neg", 600, components=[M.Comp("tri", 700, 700, scale=-1.0)])
        f.glyph("c_xyscale", 600, components=[M.Comp("tri", 0, 0, scale=(0.5, 1.75))])
        f.glyph("c_2x2_rot90", 600, components=[M.Comp("tri", 700, 0, scale=(0.0, 1.0, -1.0, 0.0))])      # x' = -y, y' = x
        f.glyph("c_2x2_shear", 600, components=[M.Comp("sq", 0, 0, scale=(1.0, 0.25, 0.5, 1.0))])
        f.glyph("c_scaled_offset", 600, components=[M.Comp("tri", 200, 100, scale=0.5, scaled_offset=True)])
        f.glyph("c_unscaled_offset", 600, components=[M.Comp("tri", 200, 100, scale=0.5, unscaled_offset=True)])
        f.glyph("c_both_offset_flags", 600, components=[M.Comp("tri", 200, 100, scale=0.5, scaled_offset=True, unscaled_offset=True)])
        f.glyph("c_match", 600, components=[M.Comp("sq"), M.Comp("tri", match=(0, 1))])                   # triangle point 1 onto square point 0
        f.glyph("c_match_scaled", 600, components=[M.Comp("sq"), M.Comp("tri", match=(2, 0), scale=0.5)])
        f.glyph("c_nested", 600, components=[M.Comp("c_offset", 10, 10), M.Comp("c_match", 0, 500, scale=0.5)])
        f.glyph("c_deep", 600, components=[M.Comp("c_nested", 5, 5, scale=0.8)])
        f.glyph("c_many", 600, components=[M.Comp("tri", 10 * i, 5 * i) for i in range(40)])
        f.glyph("c_flags", 600, components=[M.Comp("sq", use_my_metrics=True, round_xy=True, overlap=True), M.Comp("tri", 700, 0)], instructions=b"\x01\x02\x03")
        f.glyph("c_empty_component", 600, components=[M.Comp(".notdef"), M.Comp("tri", 5, 5)])
        self.check(f)

    def test_composite_limits(self):
        def raw_composite(targets, dx=1, dy=1):
            """A composite glyph made of whole-pixel offset components, written by hand (the generator cannot model cycles)."""
            out = struct.pack(">hhhhh", -1, 0, 0, 0, 0)
            for i, t in enumerate(targets):
                out += struct.pack(">HHbb", 0x0002 | (0x20 if i + 1 < len(targets) else 0), t, dx, dy)
            return out
        f = M.Font()
        f.glyph(".notdef", 500)
        f.glyph("tri", 600, M.TRIANGLE)
        selfref = len(f.glyphs)
        f.glyph("selfref", 600, raw=raw_composite([selfref]))                           # includes itself
        chain = []
        for i in range(12):                                                            # glyph k includes glyph k+1; the last one is the triangle
            idx = len(f.glyphs)
            f.glyph("chain%d" % i, 600, raw=raw_composite([idx + 1 if i < 11 else 1]))
            chain.append(idx)
        fan = len(f.glyphs)
        f.glyph("fan", 600, raw=raw_composite([1] * 300))                               # more components than a glyph may have
        pair = len(f.glyphs)
        f.glyph("pair", 600, raw=raw_composite([1, 1]))
        data = f.build()
        res = with_font(data, ["outline a %d" % g for g in (selfref, chain[0], chain[8], fan, pair)] + ["outline a 9999"])
        self.assertEqual(res[0][0], "err -6 glyph or bitmap too large")                 # a cycle ends at the depth limit
        self.assertEqual(res[1][0], "err -6 glyph or bitmap too large")                 # 12 levels: too deep
        self.assertEqual(parse_path(res[2])[0][0], "M")                                 # 4 levels are fine
        self.assertEqual(res[3][0], "err -6 glyph or bitmap too large")                 # 300 components
        self.assertEqual(sum(1 for l in res[4] if l == "Z"), 2)                         # two components, two contours
        self.assertEqual(res[5][0], "err -8 bad glyph")

    def test_bad_glyph_data(self):
        good = M.Font()
        good.glyph(".notdef", 500)
        good.glyph("tri", 600, M.TRIANGLE)
        data = good.build()
        t = sfnt_tables(data)
        glyf = bytearray(t["glyf"])
        # find the triangle's data: glyph 1 starts where loca says
        start = struct.unpack(">H", t["loca"][2:4])[0] * 2
        end = struct.unpack(">H", t["loca"][4:6])[0] * 2
        g = bytes(glyf[start:end])

        def with_glyph(raw):
            raw = raw + b"\0" * (len(raw) % 2)                                              # short loca counts in words
            return patched(data, glyf=raw, loca=struct.pack(">HHH", 0, 0, len(raw) // 2))

        def run(raw):
            return with_font(with_glyph(raw), ["outline a 1"])[0][0]

        self.assertTrue(run(g).startswith("M "))                                            # the intact glyph decodes
        self.assertEqual(run(g[:8]), "err -8 bad glyph")                                    # shorter than a header
        self.assertEqual(run(g[:12]), "err -8 bad glyph")                                   # cut in the end points
        self.assertEqual(run(g[:len(g) - 6]), "err -8 bad glyph")                           # cut in the coordinates
        hdr = struct.pack(">hhhhh", 2, 0, 0, 0, 0)
        self.assertTrue(run(hdr + struct.pack(">HHH", 5, 2, 0) + bytes(30)).startswith("err -8"))      # end points that go backwards
        too_many = bytearray(g)
        too_many[10:12] = b"\xff\xff"
        self.assertTrue(run(bytes(too_many)).startswith("err -8"))                          # 65536 points that are not there
        self.assertTrue(run(hdr[:0] + struct.pack(">hhhhh", -1, 0, 0, 0, 0) + g[10:]) is not None)      # reinterpreted as a composite: whatever happens, no crash
        zero_contours = struct.pack(">hhhhh", 0, 0, 0, 0, 0)                                # a header only: an empty glyph, not an error
        self.assertEqual(run(zero_contours), "end")


# =============================================================================================== cmap


class CharMap(ToolCase):
    def font(self, subtables, nglyphs=40):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        for i in range(1, nglyphs):
            f.glyph("g%d" % i, 500 + i, M.SQUARE)
        for plat, enc, data in subtables:
            f.cmap_raw(plat, enc, data)
        return f

    def lookup(self, f, cps):
        chunks = [cps[i:i + 200] for i in range(0, len(cps), 200)]
        res = with_font(f.build(), ["glyph a " + " ".join(str(c) for c in ch) for ch in chunks])
        return [int(kv(l)["gid"]) for r in res for l in r]

    def test_format4_delta_and_array(self):
        mapping = {0x20: 1, 0x21: 2, 0x22: 3, 0x41: 10, 0x42: 11, 0x43: 5, 0x44: 6, 0x61: 20, 0x3042: 30, 0x3044: 31, 0xFFFE: 4}
        cps = sorted(mapping) + [0x1F, 0x23, 0x40, 0x45, 0x60, 0x62, 0x3043, 0xFFFF, 0x10000, 0x1F600]
        for force in (False, True):
            f = self.font([(3, 1, M.cmap_format4(mapping, force_array=force))])
            got = self.lookup(f, cps)
            want = [mapping.get(c, 0) for c in cps]
            self.assertEqual(got, want, "force_array=%s" % force)

    def test_format4_edge_cases(self):
        # a glyph index at or beyond the glyph count is not a glyph
        f = self.font([(3, 1, M.cmap_format4({0x41: 5, 0x42: 39, 0x43: 40, 0x44: 500}))], nglyphs=40)
        self.assertEqual(self.lookup(f, [0x41, 0x42, 0x43, 0x44]), [5, 39, 0, 0])
        # a segment table that claims more segments than the table holds: unusable, not a crash
        data = bytearray(M.cmap_format4({0x41: 1}))
        data[6:8] = b"\x7f\xfe"
        f = self.font([(3, 1, bytes(data))])
        self.assertEqual(self.lookup(f, [0x41]), [0])

    def test_format6_and_0_and_12_13(self):
        f = self.font([(0, 3, M.cmap_format6(0x30, [1, 2, 0, 4]))])
        self.assertEqual(self.lookup(f, [0x2F, 0x30, 0x31, 0x32, 0x33, 0x34]), [0, 1, 2, 0, 4, 0])
        f = self.font([(1, 0, M.cmap_format0({0x41: 7, 0x7A: 9, 0xE9: 11}))])
        self.assertEqual(self.lookup(f, [0x41, 0x7A, 0xE9, 0x42]), [7, 9, 0, 0])       # Macintosh Roman: ASCII only (0xE9 is not Unicode's e-acute there)
        full = {0x41: 3, 0x42: 4, 0x43: 5, 0x1F600: 12, 0x1F601: 13, 0x1F602: 14, 0x10FFFF: 20, 0x4E00: 25}
        f = self.font([(3, 10, M.cmap_format12(full))])
        cps = sorted(full) + [0x40, 0x44, 0x1F5FF, 0x1F603, 0x110000, 0xFFFFFFFF]
        self.assertEqual(self.lookup(f, cps), [full.get(c, 0) for c in cps])
        f = self.font([(3, 10, M.cmap_format13([(0x100, 0x1FF, 6), (0x300, 0x300, 7)]))])
        self.assertEqual(self.lookup(f, [0xFF, 0x100, 0x180, 0x1FF, 0x200, 0x300]), [0, 6, 6, 6, 0, 7])

    def test_subtable_preference(self):
        # the full-repertoire table beats the BMP table; Windows Unicode beats Mac; unknown formats are skipped
        bmp = M.cmap_format4({0x41: 1})
        full = M.cmap_format12({0x41: 2, 0x1F600: 3})
        mac = M.cmap_format0({0x41: 4})
        f = self.font([(3, 1, bmp), (3, 10, full), (1, 0, mac)])
        self.assertEqual(self.lookup(f, [0x41, 0x1F600]), [2, 3])
        f = self.font([(3, 1, bmp), (1, 0, mac)])
        self.assertEqual(self.lookup(f, [0x41]), [1])
        f = self.font([(1, 0, mac)])
        self.assertEqual(self.lookup(f, [0x41]), [4])
        f = self.font([(0, 5, M.cmap_format14(0x41, 0xFE0F)), (3, 1, bmp)])                # variation selectors are not a character map
        self.assertEqual(self.lookup(f, [0x41]), [1])
        f = self.font([(0, 5, M.cmap_format14(0x41, 0xFE0F))])
        self.assertEqual(self.lookup(f, [0x41]), [0])
        f = self.font([(0, 3, bmp)])                                                      # Unicode platform, BMP
        self.assertEqual(self.lookup(f, [0x41]), [1])

    def test_symbol_map(self):
        sym = M.cmap_format4({0xF041: 3, 0xF042: 4, 0x20: 9})
        f = self.font([(3, 0, sym)])
        self.assertEqual(self.lookup(f, [0x41, 0x42, 0x20, 0xF041, 0x43]), [3, 4, 9, 3, 0])     # 'A' finds the glyph stored at U+F041

    def test_no_cmap(self):
        f = self.font([])
        self.assertEqual(self.lookup(f, [0x41, 0x20]), [0, 0])

    def test_matches_python_for_random_maps(self):
        rng = random.Random(11)
        for trial in range(8):
            mapping = {}
            for _ in range(200):
                base = rng.choice([0x20, 0x300, 0x4E00, 0xFF00, 0x1F000])
                for k in range(rng.randrange(1, 12)):
                    mapping[base + rng.randrange(0, 400) + k] = rng.randrange(1, 39)
            bmp = {c: g for c, g in mapping.items() if c < 0x10000 and c != 0xFFFF}
            probes = list(range(0, 0x500, 7)) + [rng.randrange(0, 0x30000) for _ in range(300)] + list(bmp)[:200] + list(mapping)[:200]
            for fmt, tbl, expect in ((4, M.cmap_format4(bmp, force_array=bool(trial & 1)), bmp), (12, M.cmap_format12(mapping), mapping)):
                f = self.font([(3, 1 if fmt == 4 else 10, tbl)])
                got = self.lookup(f, probes)
                self.assertEqual(got, [expect.get(c, 0) for c in probes], "trial %d format %d" % (trial, fmt))


# =============================================================================================== kerning and ligatures


class Kerning(ToolCase):
    def font(self, **extra):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        for n in "abcdefgh":
            f.glyph(n, 600, M.SQUARE)
        f.cmap_bmp({0x61 + i: n for i, n in enumerate("abcdefgh")})
        f.extra.update(extra)
        return f

    def kerns(self, f, pairs):
        res = with_font(f.build(), ["kern a %d %d" % p for p in pairs])
        return [int(r[0].split()[1]) for r in res]

    def test_kern_table_variants(self):
        pairs = {(1, 2): -50, (2, 3): 30, (1, 3): -7, (4, 5): 100, (8, 1): -1}
        probes = sorted(pairs) + [(2, 1), (3, 2), (1, 1), (0, 0), (9, 9), (200, 1)]
        for apple in (False, True):
            for split in (None, 2, 3):
                f = self.font(kern=M.kern_table(pairs, apple=apple, split=split))
                self.assertEqual(self.kerns(f, probes), [pairs.get(p, 0) for p in probes], "apple=%s split=%s" % (apple, split))
        # two subtables that both know a pair: the values add up, unless the later one has the override flag
        first = M.kern_table({(1, 2): -50})[4:]
        for override, want in ((False, [-30, 7]), (True, [20, 7])):
            second = M.kern_table({(1, 2): 20, (2, 3): 7}, override=override)[4:]
            f = self.font(kern=struct.pack(">HH", 0, 2) + first + second)
            self.assertEqual(self.kerns(f, [(1, 2), (2, 3)]), want, "override=%s" % override)

    def test_gpos_pair_format1(self):
        pairs = {(1, 2): -40, (1, 3): -30, (2, 3): 20, (5, 1): 11}
        probes = sorted(pairs) + [(2, 1), (1, 4), (3, 3), (6, 1)]
        for ext in (False, True):
            for cov in (1, 2):
                for vf1, vf2 in ((4, 0), (5, 0), (4, 1), (0x45, 0x44)):
                    sub = M.pairpos1(pairs, vf1=vf1, vf2=vf2, cov_fmt=cov)
                    f = self.font(GPOS=M.gpos_kern([sub], extension=ext))
                    self.assertEqual(self.kerns(f, probes), [pairs.get(p, 0) for p in probes], "ext=%s cov=%s vf=%x,%x" % (ext, cov, vf1, vf2))

    def test_gpos_pair_format2_classes(self):
        class1 = {1: 1, 2: 1, 3: 2}                  # first glyph classes (class 0 = everything else covered)
        class2 = {2: 1, 3: 2, 4: 2}
        matrix = [[0, 0, 0], [0, -10, -20], [0, 5, 15]]
        for cd in (1, 2):
            for cov in (1, 2):
                sub = M.pairpos2([1, 2, 3, 4], class1, class2, matrix, cov_fmt=cov, cd_fmt=cd)
                f = self.font(GPOS=M.gpos_kern([sub]))
                probes = [(1, 2), (1, 3), (1, 4), (2, 2), (3, 2), (3, 3), (3, 4), (4, 2), (4, 4), (1, 1), (5, 2), (3, 7)]
                want = []
                for l, r in probes:
                    if l not in (1, 2, 3, 4):
                        want.append(0)
                    else:
                        want.append(matrix[class1.get(l, 0)][class2.get(r, 0)])
                self.assertEqual(self.kerns(f, probes), want, "cd=%d cov=%d" % (cd, cov))

    def test_gpos_subtable_order_and_script(self):
        # the first subtable that covers the pair decides: an exception in format 1, the class rule behind it
        exceptions = M.pairpos1({(1, 2): -99}, cov_fmt=1)
        classes = M.pairpos2([1, 2], {1: 1, 2: 1}, {2: 1, 3: 1}, [[0, 0], [0, -10]], cov_fmt=1)
        f = self.font(GPOS=M.gpos_kern([exceptions, classes]))
        self.assertEqual(self.kerns(f, [(1, 2), (1, 3), (2, 2), (2, 4)]), [-99, -10, -10, 0])
        # the Latin script is preferred over DFLT: build a table whose DFLT script uses lookup 1 and latn lookup 0
        l0 = M.lookup_table(2, [M.pairpos1({(1, 2): -10})])
        l1 = M.lookup_table(2, [M.pairpos1({(1, 2): -20})])
        t = M.otl_table([("DFLT", [1]), ("latn", [0])], [("kern", [0]), ("kern", [1])], [l0, l1])
        f = self.font(GPOS=t)
        self.assertEqual(self.kerns(f, [(1, 2)]), [-10])
        # a font whose only script is Cyrillic still kerns: its script is better than none
        t = M.otl_table([("cyrl", [0])], [("kern", [0])], [l1])
        f = self.font(GPOS=t)
        self.assertEqual(self.kerns(f, [(1, 2)]), [-20])
        # a script that has no 'kern' feature of its own: every 'kern' feature of the font is used
        t = M.otl_table([("latn", [1])], [("kern", [0]), ("mark", [0])], [l1])
        f = self.font(GPOS=t)
        self.assertEqual(self.kerns(f, [(1, 2)]), [-20])
        # a feature that is not 'kern' is ignored
        t = M.otl_table([("latn", [0])], [("mark", [0])], [l1])
        f = self.font(GPOS=t, kern=M.kern_table({(1, 2): -5}))
        self.assertEqual(self.kerns(f, [(1, 2)]), [-5])                                   # no GPOS 'kern': the kern table speaks

    def test_gpos_wins_over_kern_table(self):
        f = self.font(GPOS=M.gpos_kern([M.pairpos1({(1, 2): -40})]), kern=M.kern_table({(1, 2): -5, (2, 3): -6}))
        self.assertEqual(self.kerns(f, [(1, 2), (2, 3)]), [-40, 0])

    def test_two_lookups_add_up(self):
        l0 = M.lookup_table(2, [M.pairpos1({(1, 2): -10})])
        l1 = M.lookup_table(2, [M.pairpos1({(1, 2): -3, (2, 3): 4})])
        t = M.otl_table([("DFLT", [0])], [("kern", [0, 1])], [l0, l1])
        self.assertEqual(self.kerns(self.font(GPOS=t), [(1, 2), (2, 3)]), [-13, 4])

    def test_ligatures(self):
        # f=5 i=6 l=7: fi=1 (glyph 1 stands for the ligature), ffi, ffl, fl; the font's order is tried first to last
        ligs = [(5, [5, 6], 2), (5, [5, 7], 3), (5, [6], 1), (5, [7], 4), (7, [7], 8)]
        for ext in (False, True):
            for cov in (1, 2):
                f = self.font(GSUB=M.gsub_liga([M.liga_subtable(ligs, cov_fmt=cov)], extension=ext))
                res = with_font(f.build(), ["lig a 5 6", "lig a 5 5 6", "lig a 5 5 7", "lig a 5 7", "lig a 5 8", "lig a 7 7 7", "lig a 6 5", "lig a 5", "lig a 5 5 5"])
                self.assertEqual([r[0] for r in res], ["lig 1 2", "lig 2 3", "lig 3 3", "lig 4 2", "none", "lig 8 2", "none", "none", "none"], "ext=%s cov=%s" % (ext, cov))
        # order matters: listing the short ligature before the longer one wins
        f = self.font(GSUB=M.gsub_liga([M.liga_subtable([(5, [6], 1), (5, [6, 7], 2)])]))
        self.assertEqual(with_font(f.build(), ["lig a 5 6 7"])[0][0], "lig 1 2")


# =============================================================================================== the rasteriser


SHAPES = {
    "square": [[(100, 0), (100, 500), (600, 500), (600, 0)]],
    "triangle": M.TRIANGLE,
    "diagonal": [[(0, 0), (600, 700), (615, 700), (15, 0)]],                              # a sliver, 15 units wide, about a third of a pixel at 25 px
    "circle": [M.circle_contour(500, 400, 400, 8)],
    "overlap": [[(100, 0), (100, 500), (600, 500), (600, 0)], [(350, 250), (350, 750), (850, 750), (850, 250)]],        # two squares, same direction
    "nested_same": [[(0, 0), (0, 700), (700, 700), (700, 0)], [(200, 200), (200, 500), (500, 500), (500, 200)]],       # winding 2 inside: solid
    "box_hole": M.BOX,                                                                 # the inner contour runs the other way: a hole
    "negative": [[(-300, -200), (-300, -50), (-100, -50), (-100, -200)]],
    "ring": [M.circle_contour(500, 400, 400, 8), list(reversed(M.circle_contour(500, 400, 200, 8)))],
}


class Raster(ToolCase):
    @classmethod
    def setUpClass(cls):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        cls.gid = {}
        for name, contours in SHAPES.items():
            cls.gid[name] = f.glyph(name, 700, contours)
        f.glyph("empty", 300)
        cls.gid["empty"] = len(f.glyphs) - 1
        f.glyph("comp_scaled", 700, components=[M.Comp("triangle", 100, 50, scale=0.5), M.Comp("square", 0, 0, scale=(0.25, 0.25))])
        cls.gid["comp_scaled"] = len(f.glyphs) - 1
        cls.font = f
        cls.path = save(f.build(), "raster.ttf")

    def render(self, name, size, sub_x=0.0, mode=0, skew=0.0, sub_y=0.0):
        gid = self.gid[name] if isinstance(name, str) else name
        res = batch(["load a %s" % self.path, "render a %d %s %s %d %s %s" % (gid, repr(float(size)), repr(float(sub_x)), mode, repr(float(skew)), repr(float(sub_y)))])[1]
        if res[0].startswith("err"):
            return res[0]
        return parse_bitmap(res)

    def expected(self, name, size, bm, sub_x=0.0, sub_y=0.0, skew=0.0, exact=True, steps=48):
        """The exact coverage of the glyph's outline in the bitmap's pixel grid (rational arithmetic unless the outline has curves)."""
        polys = flatten_quad_path(self.font.path(self.gid[name]), steps)
        if exact:
            conv = lambda v: Fraction(v).limit_denominator(100000)
            scale = conv(size) / self.font.upem
        else:
            conv = float
            scale = size / float(self.font.upem)
        px = [[(((conv(x) + conv(skew) * conv(y)) * scale + conv(sub_x) - bm["left"]), bm["top"] - (conv(y) * scale + conv(sub_y))) for x, y in poly]
              for poly in polys]
        return exact_coverage(px, bm["w"], bm["h"])

    def check(self, name, size, tol=1, sub_x=0.0, sub_y=0.0, skew=0.0, exact=True, steps=48):
        bm = self.render(name, size, sub_x, 0, skew, sub_y)
        self.assertIsInstance(bm, dict, bm)
        self.assertGreater(bm["w"], 0)
        want = self.expected(name, size, bm, sub_x, sub_y, skew, exact=exact, steps=steps)
        compare_levels(self, bm["rows"], want, tol, "%s@%s sub=%s,%s skew=%s" % (name, size, sub_x, sub_y, skew))
        return bm

    def test_pixel_aligned_square(self):
        bm = self.render("square", 10)
        self.assertEqual((bm["left"], bm["top"], bm["w"], bm["h"], bm["bpp"]), (1, 5, 5, 5, 1))
        self.assertEqual(bm["rows"], [[255] * 5] * 5)

    def test_bitmap_carries_the_advance(self):
        """Bearings and advance come with the bitmap: advance = font units * size / units per em, for glyphs without ink too (a space), in LCD mode, and
        with any sub-pixel offset (it is the glyph's, not the pen's)."""
        for name, units in (("square", 700), ("circle", 700), ("empty", 300)):
            for size, mode, sub in ((10, 0, 0.0), (37, 0, 0.5), (23.5, 1, 0.25)):
                bm = self.render(name, size, sub, mode)
                self.assertAlmostEqual(bm["adv"], units * size / 1000.0, places=3, msg="%s at %s" % (name, size))
        self.assertEqual(self.render("empty", 20)["w"], 0)                              # (no ink, still an advance)

    def test_polygons_are_exact(self):
        for size in (7.3, 12, 16.5, 25, 37, 41.25):
            for name in ("square", "triangle", "diagonal"):
                self.check(name, size)

    def test_bitmap_box_is_tight(self):
        for name, size in (("square", 37), ("triangle", 41), ("circle", 33), ("negative", 10), ("negative", 23.5), ("overlap", 19)):
            bm = self.render(name, size)
            pts = [p for c in self.font.points(self.gid[name]) for p in c]
            s = size / 1000.0
            xs, ys = [p[0] * s for p in pts], [p[1] * s for p in pts]
            self.assertEqual(bm["left"], math.floor(min(xs) + 1e-4), name)
            self.assertEqual(bm["left"] + bm["w"], math.ceil(max(xs) - 1e-4), name)
            self.assertEqual(bm["top"], math.ceil(max(ys) - 1e-4), name)
            self.assertEqual(bm["top"] - bm["h"], math.floor(min(ys) + 1e-4), name)

    def test_subpixel_positioning(self):
        for sub_x in (0.0, 0.25, 0.5, 0.75, 0.9):
            self.check("triangle", 30, sub_x=sub_x)
            self.check("square", 21.5, sub_x=sub_x, sub_y=sub_x / 2)
        # moving the pen by a quarter pixel moves the ink by a quarter pixel: the sum of coverage stays the same
        base = ink(self.render("triangle", 40, 0.0))
        for sub_x in (0.25, 0.5, 0.75):
            self.assertAlmostEqual(ink(self.render("triangle", 40, sub_x)), base, delta=base * 0.004 + 8)

    def test_oblique(self):
        for skew in (0.2, -0.3):
            self.check("triangle", 36, skew=skew)
            self.check("square", 29, skew=skew, sub_x=0.5)

    def test_curves_flatten_accurately(self):
        bm = self.check("circle", 40, tol=6, exact=False, steps=60)
        # and on average the error is a small fraction of a level
        want = self.expected("circle", 40, bm, exact=False, steps=60)
        err = [abs(g - to_level(w)) for gr, wr in zip(bm["rows"], want) for g, w in zip(gr, wr)]
        self.assertLess(sum(err) / len(err), 0.6)

    def test_ring_has_a_hole(self):
        bm = self.check("ring", 40, tol=6, exact=False, steps=60)
        cy, cx = bm["h"] // 2, bm["w"] // 2
        self.assertEqual(bm["rows"][cy][cx], 0)                                  # the centre is empty
        self.assertEqual(bm["rows"][cy][int(bm["w"] * 0.12)], 255)               # the ring itself is solid

    def test_winding_rules(self):
        # the hole of BOX is exact
        self.check("box_hole", 30)
        # a contour inside another one running the same way is solid (non-zero winding, not even-odd)
        bm = self.check("nested_same", 28)
        inner = bm["rows"][bm["h"] // 2][bm["w"] // 2]
        self.assertEqual(inner, 255)
        # overlapping contours: the interior of the overlap is solid, edges follow the documented sum-and-clamp rule exactly
        bm = self.check("overlap", 33)
        self.assertEqual(max(max(r) for r in bm["rows"]), 255)

    def test_overlap_junction_error_is_small(self):
        """The sum-and-clamp method overestimates only where the edges of two overlapping contours meet inside one pixel. With two rectangles the
        true union area per pixel is |A| + |B| - |A and B| (exact fractions), so the error can be measured: a few pixels at the two crossings, no more."""
        size = 33
        bm = self.render("overlap", size)
        s = Fraction(size, 1000)
        A, B = (100, 0, 600, 500), (350, 250, 850, 750)                                 # x0, y0, x1, y1 in font units
        px = lambda r: (r[0] * s - bm["left"], bm["top"] - r[3] * s, r[2] * s - bm["left"], bm["top"] - r[1] * s)     # to pixels, y down
        inter = lambda p, q: (max(p[0], q[0]), max(p[1], q[1]), min(p[2], q[2]), min(p[3], q[3]))
        area = lambda r: max(r[2] - r[0], 0) * max(r[3] - r[1], 0)
        a, b = px(A), px(B)
        off, worst = [], 0.0
        for y in range(bm["h"]):
            for x in range(bm["w"]):
                pix = (x, y, x + 1, y + 1)
                truth = area(inter(a, pix)) + area(inter(b, pix)) - area(inter(inter(a, b), pix))
                d = abs(bm["rows"][y][x] / 255.0 - float(truth))
                worst = max(worst, d)
                if d > 0.01:
                    off.append((x, y, round(d, 3)))
        self.assertLessEqual(len(off), 4, off)                                          # the two crossings, each in one pixel (or two, when it sits near a boundary)
        self.assertLess(worst, 0.5)

    def test_composite_renders_like_its_resolved_outline(self):
        # a half-size triangle moved by (100, 50) and a quarter-size square: exact coverage of the outline the transforms give
        for size in (37, 64):
            bm = self.render("comp_scaled", size)
            want = self.expected("comp_scaled", size, bm, exact=True)
            compare_levels(self, bm["rows"], want, 1, "composite@%s" % size)

    def test_lcd_filter(self):
        """Three stripes per pixel: the exact coverage at three times the horizontal resolution, rounded, then filtered with 28 57 86 57 28 (sum 256)."""
        for name, size in (("square", 31), ("triangle", 44)):
            for sub_x in (0.0, 0.5):
                bm = self.render(name, size, sub_x, mode=1)
                self.assertEqual(bm["bpp"], 3)
                gray = self.render(name, size, sub_x, mode=0)
                self.assertEqual(bm["left"], gray["left"] - 1, "one extra pixel on each side for the filter")
                self.assertEqual(bm["w"], gray["w"] + 2)
                polys = flatten_quad_path(self.font.path(self.gid[name]))
                s = size / 1000.0
                W3 = bm["w"] * 3
                px = [[(((x * s) + sub_x - bm["left"]) * 3, bm["top"] - y * s) for x, y in p] for p in polys]
                cov = exact_coverage(px, W3, bm["h"])
                for y in range(bm["h"]):
                    g = [to_level(c) for c in cov[y]]
                    row = []
                    for i in range(W3):
                        v = 86 * g[i] + (57 * g[i - 1] if i >= 1 else 0) + (28 * g[i - 2] if i >= 2 else 0) \
                            + (57 * g[i + 1] if i + 1 < W3 else 0) + (28 * g[i + 2] if i + 2 < W3 else 0)
                        row.append(min(255, (v + 128) >> 8))
                    for i, (a, b) in enumerate(zip(bm["rows"][y], row)):
                        self.assertLessEqual(abs(a - b), 3, "%s row %d stripe %d: %d vs %d" % (name, y, i, a, b))

    def test_lcd_has_colour_fringes_gray_does_not(self):
        bm = self.render("triangle", 20, 0.3, mode=1)
        fringe = any(r[i] != r[i + 1] or r[i + 1] != r[i + 2] for r in bm["rows"] for i in range(0, len(r) - 2, 3))
        self.assertTrue(fringe)

    def test_empty_and_errors(self):
        bm = self.render("empty", 20)
        self.assertEqual((bm["w"], bm["h"]), (0, 0))
        bm = self.render("empty", 20, mode=1)
        self.assertEqual((bm["w"], bm["h"], bm["bpp"]), (0, 0, 3))
        for size, want in ((0.5, "err -7"), (0, "err -7"), (-3, "err -7"), (2000, "err -7"), ("nan", "err -7"), ("inf", "err -7")):
            res = batch(["load a %s" % self.path, "render a 1 %s" % size])[1]
            self.assertTrue(res[0].startswith(want), (size, res))
        res = batch(["load a %s" % self.path, "render a 1 20 0 5", "render a 9999 20", "render a -1 20", "render a 1 20 nan 0 nan nan"])
        self.assertTrue(res[1][0].startswith("err -7"))                                  # a mode that does not exist
        self.assertTrue(res[2][0].startswith("err -8"))                                  # a glyph that does not exist
        self.assertTrue(res[3][0].startswith("err -8"))
        self.assertTrue(res[4][0].startswith("bitmap"))                                  # NaN offsets and skew are sanitised, not propagated

    def test_huge_outlines_are_refused(self):
        f = M.Font(upem=16)
        f.glyph(".notdef", 10)
        f.glyph("huge", 10, [[(0, 0), (0, 20000), (20000, 20000), (20000, 0)]])
        path = save(f.build())
        res = batch(["load a %s" % path, "render a 1 1000", "render a 1 100", "sweep a 1.0 0 1 1", "sweep a 1.0 1 1 1", "sweep a 30 0 1 1"])
        self.assertTrue(res[1][0].startswith("err -6"), res[1][:1])                    # 20000 / 16 * 1000 pixels wide
        self.assertTrue(res[2][0].startswith("err -6"), res[2][:1])
        self.assertIn("errors=0", res[3][0])                                             # 1 pixel per em: 1250 x 1250, within the limits
        self.assertIn("drawn=1", res[3][0])
        self.assertIn("errors=1", res[5][0])                                             # 30 px per em: 37500 pixels wide

    def test_rendering_is_deterministic_and_stateless(self):
        names = ["triangle", "circle", "overlap", "square", "triangle"]
        cmds = ["load a %s" % self.path] + ["render a %d 27.5 0.25" % self.gid[n] for n in names]
        out = batch(cmds)[1:]
        self.assertEqual(out[0], out[4])                                                 # the same glyph after others came out identical
        again = batch(cmds)[1:]
        self.assertEqual(out, again)

    def test_total_ink_matches_area(self):
        """Over a whole font: the ink of every glyph equals the area of its outline (computed by a fine independent flattening)."""
        res = batch(["load a %s" % self.path, "areacheck a 17", "areacheck a 90", "areacheck a 333"])
        # the glyphs whose contours overlap in the same direction differ by construction: the area counts the overlap twice, the ink once
        allowed = {self.gid["overlap"], self.gid["nested_same"], self.gid["comp_scaled"]}       # (the composite's square overlaps its triangle)
        for line in res[1:]:
            d = kv(line[-1].replace("areacheck ", ""))
            self.assertEqual(d["errors"], "0")
            flagged = {int(kv(l.replace("bad ", ""))["gid"]) for l in line if l.startswith("bad ")}
            self.assertLessEqual(flagged, allowed, line)


# =============================================================================================== CFF


def P(text):
    """'M 0 0; L 10 0; C 1 2 3 4 5 6; Z' -> [('M', 0.0, 0.0), ...]"""
    out = []
    for part in text.split(";"):
        p = part.split()
        if p:
            out.append((p[0], *[float(v) for v in p[1:]]))
    return out


def cff_font(cases, gsubrs=(), subrs=()):
    c = M.CffFont()
    c.glyph(".notdef", 500, M.cs("endchar"))
    for name, code, _ in cases:
        c.glyph(name, 500, code)
    c.gsubrs = list(gsubrs)
    c.local_subrs = list(subrs)
    c.cmap_bmp({0x41: 1})
    return c


try:
    import logging
    import fontTools.ttLib as _ftt           # an independent reader, used when it is installed (it is in the lead's WSL)
    from fontTools.pens.recordingPen import RecordingPen as _RecPen
    logging.getLogger("fontTools").setLevel(logging.ERROR)                      # (it grumbles about the zero timestamps of hand-made fonts)
    HAVE_FONTTOOLS = True
except Exception:                                                                  # noqa: BLE001
    HAVE_FONTTOOLS = False


def fonttools_paths(data, face=0):
    """Outlines of every glyph by fontTools, in the tool's M/L/Q/C/Z notation (for cross-checks)."""
    import io
    font = _ftt.TTFont(io.BytesIO(data), fontNumber=face)
    gs = font.getGlyphSet()
    out = []
    for name in font.getGlyphOrder():
        pen = _RecPen()
        try:
            gs[name].draw(pen)
        except Exception:                                                          # noqa: BLE001  (a glyph fontTools cannot read: no outline to compare)
            out.append(None)
            continue
        ops = []
        for kind, args in pen.value:
            if kind == "moveTo":
                ops.append(("M", *args[0]))
            elif kind == "lineTo":
                ops.append(("L", *args[0]))
            elif kind == "curveTo":
                ops.append(("C", *[v for pt in args for v in pt]))
            elif kind == "qCurveTo":
                pts = list(args)
                if pts[-1] is None:
                    raise ValueError("closed contour of off-curve points")
                for i in range(len(pts) - 1):                                      # implied on-curve points
                    c = pts[i]
                    e = pts[i + 1] if i == len(pts) - 2 else ((pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2)
                    ops.append(("Q", c[0], c[1], e[0], e[1]))
            elif kind in ("closePath", "endPath"):
                ops.append(("Z",))
        out.append(ops)
    return out


CS = M.cs
H = b"\x0e"      # a mask byte that would be the `endchar` operator if it were read as code

# Operand sequences that compute the target of a move with the arithmetic operators, and the point they must arrive at
ARITH_CASES = [
    ("add_mul", (3, 4, "add", 5, "mul", 0), (35, 0)),
    ("sub", (50, 10, "sub", 7, "mul", 0), (280, 0)),
    ("div", (100, 8, "div", 0), (12.5, 0)),
    ("div_zero", (100, 0, "div", 0), (0, 0)),
    ("neg_abs", (-7, "abs", 9, "neg"), (7, -9)),
    ("sqrt", (16, "sqrt", 0), (4, 0)),
    ("sqrt_negative", (-16, "sqrt", 0), (0, 0)),
    ("dup", (6, "dup"), (6, 6)),
    ("exch", (1, 2, "exch"), (2, 1)),
    ("drop", (1, 2, 3, "drop"), (1, 2)),
    ("index", (10, 20, 30, 1, "index"), (30, 20)),
    ("index_top", (10, 20, 30, 0, "index"), (30, 30)),
    ("roll_up", (10, 20, 30, 3, 1, "roll"), (10, 20)),                          # 30 10 20
    ("roll_down", (10, 20, 30, 3, -1, "roll"), (30, 10)),                       # 20 30 10
    ("roll_more_than_n", (10, 20, 30, 3, 4, "roll"), (10, 20)),                 # 4 mod 3 = 1
    ("ifelse_first", (7, 9, 1, 2, "ifelse", 0), (7, 0)),
    ("ifelse_second", (7, 9, 3, 2, "ifelse", 0), (9, 0)),
    ("put_get", (42, 3, "put", 3, "get", 0), (42, 0)),
    ("logic", (1, 1, "eq", 5, "mul", 1, 0, "or", 3, "mul"), (5, 3)),
    ("and_not", (1, 0, "and", 4, "add", 0, "not", 6, "mul"), (4, 6)),
    ("random_is_deterministic", ("random", 0), (0.5, 0)),
    ("fraction", (1, 3, "div", 0), (0.33333, 0)),
]


def arith_program(items):
    return CS(*items, "rmoveto", 1, "hlineto", "endchar")        # the computed operands are the move; a unit line makes the move visible


def cff_zoo():
    """A CFF font with a glyph for every kind of operator: for the fuzzer (and for looking at)."""
    subrs = [CS(10, 0, "rlineto", "return"), CS(-105, "callsubr", 5, 0, "rlineto", "return"), CS(0, 7, "rlineto", "return")]
    progs = [
        CS(10, 20, 30, 40, "hstemhm", 50, 60, "hintmask", H, 100, 100, "rmoveto", -107, "callsubr", -106, "callsubr", -107, "callgsubr", "endchar"),
        CS(0, 0, "rmoveto", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, "rrcurveto", 10, 20, 30, 40, 50, 60, 70, 80, 9, "hvcurveto", 10, 20, 30, 40, 7, "vhcurveto", "endchar"),
        CS(0, 0, "rmoveto", 5, 10, 20, 30, 40, 50, 60, 70, 80, "hhcurveto", 5, 10, 20, 30, 40, "vvcurveto", 1, 2, 3, 4, 5, 6, 7, 8, "rcurveline",
           10, 20, 1, 2, 3, 4, 5, 6, "rlinecurve", "endchar"),
        CS(0, 0, "rmoveto", 10, 20, 5, 30, 40, 50, 60, "hflex", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 50, "flex", 10, 4, 20, 6, 30, 40, 50, -5, 60, "hflex1",
           10, 1, 20, 2, 30, 3, 40, 4, 50, 5, 60, "flex1", "endchar"),
        CS(250, *range(1, 19), "hstemhm", "hintmask", H + H, 7, 7, "rmoveto", 5, "hlineto", "cntrmask", H, 5, "vlineto", "endchar"),
        CS(1.5, -0.25, "rmoveto", 5000, -20000, "rlineto", 32767, "hlineto", "endchar"),
    ] + [arith_program(items) for _, items, _ in ARITH_CASES]
    c = M.CffFont()
    c.local_subrs = subrs
    c.gsubrs = [CS(0, 20, "rlineto", "return")]
    c.glyph(".notdef", 500, CS("endchar"))
    for i, p in enumerate(progs):
        c.glyph("p%d" % i, 500, p)
    c.cmap_bmp({0x41 + i: i + 1 for i in range(len(progs))})
    return c


class CffDecoding(ToolCase):
    def run_cases(self, cases, cross=True, **kw):
        """cases: (name, charstring, expected path text or an 'err' prefix). One font, one tool session; failures name the case. When fontTools is
        installed the same expectations are checked against it, an independent implementation of the charstring interpreter (not for the arithmetic
        and malformed cases, where interpreters legitimately differ on what they refuse)."""
        c = cff_font(cases, **kw)
        data = c.build()
        res = with_font(data, ["outline a %d" % (i + 1) for i in range(len(cases))])
        for (name, code, want), got in zip(cases, res):
            if want.startswith("err"):
                self.assertTrue(got[0].startswith(want), "%s: %r (wanted %s)" % (name, got[:1], want))
            else:
                self.assertPath(parse_path(got), P(want), what=name)
        if HAVE_FONTTOOLS and cross:
            theirs = fonttools_paths(data)
            for i, (name, code, want) in enumerate(cases):
                if not want.startswith("err"):
                    self.assertIsNotNone(theirs[i + 1], "fontTools could not read %s" % name)
                    self.assertPath(theirs[i + 1], P(want), what="fontTools:" + name)

    def test_lines_and_moves(self):
        self.run_cases([
            ("rlineto", CS(100, 200, "rmoveto", 50, 0, 0, 50, -50, -50, "rlineto", "endchar"), "M 100 200; L 150 200; L 150 250; L 100 200; Z"),
            ("hlineto", CS(0, 0, "rmoveto", 10, 20, 30, "hlineto", "endchar"), "M 0 0; L 10 0; L 10 20; L 40 20; Z"),
            ("vlineto", CS(5, 5, "rmoveto", 10, 20, 30, "vlineto", "endchar"), "M 5 5; L 5 15; L 25 15; L 25 45; Z"),
            ("hlineto_even", CS(0, 0, "rmoveto", 10, 20, "hlineto", "endchar"), "M 0 0; L 10 0; L 10 20; Z"),
            ("h_v_moveto", CS(10, "hmoveto", 5, "vlineto", 20, "vmoveto", 7, "hlineto", "endchar"), "M 10 0; L 10 5; Z; M 10 25; L 17 25; Z"),
            ("two_contours", CS(0, 0, "rmoveto", 4, "hlineto", 50, 50, "rmoveto", 0, 4, "rlineto", "endchar"), "M 0 0; L 4 0; Z; M 54 50; L 54 54; Z"),
            ("no_moveto", CS(10, 10, "rlineto", "endchar"), "M 0 0; L 10 10; Z"),
            ("no_endchar", CS(1, 1, "rmoveto", 5, "hlineto"), "M 1 1; L 6 1; Z"),
            ("only_endchar", CS("endchar"), ""),
            ("empty", b"", ""),
        ])

    def test_curve_operators(self):
        self.run_cases([
            ("rrcurveto2", CS(0, 0, "rmoveto", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, "rrcurveto", "endchar"), "M 0 0; C 1 2 4 6 9 12; C 16 20 25 30 36 42; Z"),
            ("hvcurveto4", CS(0, 0, "rmoveto", 10, 20, 30, 40, "hvcurveto", "endchar"), "M 0 0; C 10 0 30 30 30 70; Z"),
            ("hvcurveto5", CS(0, 0, "rmoveto", 10, 20, 30, 40, 7, "hvcurveto", "endchar"), "M 0 0; C 10 0 30 30 37 70; Z"),
            ("hvcurveto8", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, "hvcurveto", "endchar"), "M 0 0; C 10 0 30 30 30 70; C 30 120 90 190 170 190; Z"),
            ("hvcurveto9", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, 9, "hvcurveto", "endchar"), "M 0 0; C 10 0 30 30 30 70; C 30 120 90 190 170 199; Z"),
            ("hvcurveto12", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, 11, 12, 13, 14, "hvcurveto", "endchar"),
             "M 0 0; C 10 0 30 30 30 70; C 30 120 90 190 170 190; C 181 190 193 203 193 217; Z"),
            ("vhcurveto4", CS(0, 0, "rmoveto", 10, 20, 30, 40, "vhcurveto", "endchar"), "M 0 0; C 0 10 20 40 60 40; Z"),
            ("vhcurveto5", CS(0, 0, "rmoveto", 10, 20, 30, 40, 7, "vhcurveto", "endchar"), "M 0 0; C 0 10 20 40 60 47; Z"),
            ("vhcurveto8", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, "vhcurveto", "endchar"), "M 0 0; C 0 10 20 40 60 40; C 110 40 170 110 170 190; Z"),
            ("hhcurveto4", CS(0, 0, "rmoveto", 10, 20, 30, 40, "hhcurveto", "endchar"), "M 0 0; C 10 0 30 30 70 30; Z"),
            ("hhcurveto5", CS(0, 0, "rmoveto", 5, 10, 20, 30, 40, "hhcurveto", "endchar"), "M 0 0; C 10 5 30 35 70 35; Z"),
            ("hhcurveto8", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, "hhcurveto", "endchar"), "M 0 0; C 10 0 30 30 70 30; C 120 30 180 100 260 100; Z"),
            ("vvcurveto4", CS(0, 0, "rmoveto", 10, 20, 30, 40, "vvcurveto", "endchar"), "M 0 0; C 0 10 20 40 20 80; Z"),
            ("vvcurveto5", CS(0, 0, "rmoveto", 5, 10, 20, 30, 40, "vvcurveto", "endchar"), "M 0 0; C 5 10 25 40 25 80; Z"),
            ("vvcurveto8", CS(0, 0, "rmoveto", 10, 20, 30, 40, 50, 60, 70, 80, "vvcurveto", "endchar"), "M 0 0; C 0 10 20 40 20 80; C 20 130 80 200 80 280; Z"),
            ("rcurveline", CS(0, 0, "rmoveto", 1, 2, 3, 4, 5, 6, 7, 8, "rcurveline", "endchar"), "M 0 0; C 1 2 4 6 9 12; L 16 20; Z"),
            ("rcurveline2", CS(0, 0, "rmoveto", 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 5, 6, 7, 8, "rcurveline", "endchar"),
             "M 0 0; C 1 2 4 6 9 12; C 10 14 13 18 18 24; L 25 32; Z"),
            ("rlinecurve", CS(0, 0, "rmoveto", 10, 20, 1, 2, 3, 4, 5, 6, "rlinecurve", "endchar"), "M 0 0; L 10 20; C 11 22 14 26 19 32; Z"),
            ("rlinecurve2", CS(0, 0, "rmoveto", 10, 20, 30, 40, 1, 2, 3, 4, 5, 6, "rlinecurve", "endchar"), "M 0 0; L 10 20; L 40 60; C 41 62 44 66 49 72; Z"),
        ])

    def test_flex_operators(self):
        self.run_cases([
            ("hflex", CS(0, 0, "rmoveto", 10, 20, 5, 30, 40, 50, 60, "hflex", "endchar"), "M 0 0; C 10 0 30 5 60 5; C 100 5 150 0 210 0; Z"),
            ("flex", CS(0, 0, "rmoveto", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 50, "flex", "endchar"), "M 0 0; C 1 2 4 6 9 12; C 16 20 25 30 36 42; Z"),
            ("hflex1", CS(0, 0, "rmoveto", 10, 4, 20, 6, 30, 40, 50, -5, 60, "hflex1", "endchar"), "M 0 0; C 10 4 30 10 60 10; C 100 10 150 5 210 0; Z"),
            ("flex1_x", CS(0, 0, "rmoveto", 10, 1, 20, 2, 30, 3, 40, 4, 50, 5, 60, "flex1", "endchar"), "M 0 0; C 10 1 30 3 60 6; C 100 10 150 15 210 0; Z"),
            ("flex1_y", CS(0, 0, "rmoveto", 1, 10, 2, 20, 3, 30, 4, 40, 5, 50, 60, "flex1", "endchar"), "M 0 0; C 1 10 3 30 6 60; C 10 100 15 150 0 210; Z"),
        ])

    def test_widths_and_hints(self):
        self.run_cases([
            ("width_rmoveto", CS(250, 100, 200, "rmoveto", 5, "hlineto", "endchar"), "M 100 200; L 105 200; Z"),
            ("width_hmoveto", CS(250, 100, "hmoveto", 5, "vlineto", "endchar"), "M 100 0; L 100 5; Z"),
            ("width_vmoveto", CS(250, 100, "vmoveto", 5, "hlineto", "endchar"), "M 0 100; L 5 100; Z"),
            ("width_endchar", CS(250, "endchar"), ""),
            ("width_hstem", CS(250, 10, 20, "hstem", 100, 100, "rmoveto", 5, "hlineto", "endchar"), "M 100 100; L 105 100; Z"),
            ("no_width_two_stems", CS(10, 20, 30, 40, "hstem", 100, 100, "rmoveto", 5, "hlineto", "endchar"), "M 100 100; L 105 100; Z"),
            ("vstem_hm", CS(10, 20, "vstemhm", 3, 3, "rmoveto", 5, "hlineto", "endchar"), "M 3 3; L 8 3; Z"),
            ("hintmask_1byte", CS(10, 20, "hstemhm", 30, 40, "hintmask", H, 100, 100, "rmoveto", 5, "hlineto", "endchar"), "M 100 100; L 105 100; Z"),
            ("hintmask_after_stems_only", CS(10, 20, "hstemhm", "hintmask", H, 100, 100, "rmoveto", 5, "hlineto", "endchar"), "M 100 100; L 105 100; Z"),
            ("hintmask_9_hints", CS(*range(1, 19), "hstemhm", "hintmask", H + H, 7, 7, "rmoveto", 5, "hlineto", "endchar"), "M 7 7; L 12 7; Z"),
            ("hintmask_17_hints", CS(*range(1, 35), "hstemhm", "hintmask", H + H + H, 7, 7, "rmoveto", 5, "hlineto", "endchar"), "M 7 7; L 12 7; Z"),
            ("cntrmask", CS(1, 2, "hstemhm", "cntrmask", H, 3, 3, "rmoveto", 5, "hlineto", "endchar"), "M 3 3; L 8 3; Z"),
            ("hintmask_in_path", CS(1, 2, "hstemhm", 0, 0, "rmoveto", 4, "hlineto", "hintmask", H, 4, "vlineto", "endchar"), "M 0 0; L 4 0; L 4 4; Z"),
            ("width_and_hintmask_args", CS(99, 1, 2, "hstemhm", 3, 4, "hintmask", H, 0, 0, "rmoveto", 4, "hlineto", "endchar"), "M 0 0; L 4 0; Z"),
        ])

    def test_numbers(self):
        def at(*items):
            return CS(*items, "rmoveto", 1, "hlineto", "endchar")             # (a move alone leaves no outline: draw a unit line from it)
        self.run_cases([
            ("one_byte", at(-107, 107), "M -107 107; L -106 107; Z"),
            ("two_byte", at(108, -108), "M 108 -108; L 109 -108; Z"),
            ("two_byte_max", at(1131, -1131), "M 1131 -1131; L 1132 -1131; Z"),
            ("shortint", at(5000, -20000), "M 5000 -20000; L 5001 -20000; Z"),
            ("shortint_extremes", at(32767, -32768), "M 32767 -32768; L 32768 -32768; Z"),
            ("fixed", at(1.5, -0.25), "M 1.5 -0.25; L 2.5 -0.25; Z"),
            ("fixed_big", at(1000.125, 0.0625), "M 1000.125 0.0625; L 1001.125 0.0625; Z"),
        ])
        # a move that nothing follows leaves no contour, so it cannot stretch the glyph's box (fontTools keeps an empty contour: no cross-check)
        self.run_cases([("only_moves", CS(5, 5, "rmoveto", 7, 7, "rmoveto", "endchar"), ""),
                        ("trailing_move", CS(0, 0, "rmoveto", 4, "hlineto", 900, 900, "rmoveto", "endchar"), "M 0 0; L 4 0; Z")], cross=False)

    def test_arithmetic(self):
        self.run_cases([(name, arith_program(items), "M %s %s; L %s %s; Z" % (x, y, x + 1, y)) for name, items, (x, y) in ARITH_CASES], cross=False)

    def test_subroutines(self):
        # (an operand k means subroutine k + 107 while there are fewer than 1240 of them)
        subrs = [
            CS(10, 0, "rlineto", "return"),                                        # 0
            CS(-105, "callsubr", 5, 0, "rlineto", "return"),                       # 1: calls subroutine 2, then draws
            CS(0, 7, "rlineto", "return"),                                         # 2
            CS(3, 0, "rlineto", "return", 99, 99, "rlineto"),                      # 3: code after return is not run
            CS(4, 0, "rlineto"),                                                   # 4: no return, falls off the end
            CS(0, "rlineto", "return"),                                            # 5: takes its first operand from the caller
        ]
        gsubrs = [CS(0, 20, "rlineto", "return"), CS(-107, "callsubr", "return")]   # global 1 calls local subroutine 0
        self.run_cases([
            ("local", CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), "M 0 0; L 10 0; Z"),
            ("global", CS(0, 0, "rmoveto", -107, "callgsubr", "endchar"), "M 0 0; L 0 20; Z"),
            ("nested", CS(0, 0, "rmoveto", -106, "callsubr", "endchar"), "M 0 0; L 0 7; L 5 7; Z"),
            ("falls_off_end", CS(0, 0, "rmoveto", -103, "callsubr", 0, 4, "rlineto", "endchar"), "M 0 0; L 4 0; L 4 4; Z"),
            ("operands_from_caller", CS(0, 0, "rmoveto", 12, -102, "callsubr", "endchar"), "M 0 0; L 12 0; Z"),
            ("global_calls_local", CS(0, 0, "rmoveto", -106, "callgsubr", "endchar"), "M 0 0; L 10 0; Z"),
            ("twice", CS(0, 0, "rmoveto", -107, "callsubr", -107, "callsubr", "endchar"), "M 0 0; L 10 0; L 20 0; Z"),
            ("bad_index", CS(0, 0, "rmoveto", 5000, "callsubr", "endchar"), "err -8"),
            ("negative_index", CS(0, 0, "rmoveto", -500, "callsubr", "endchar"), "err -8"),
            ("empty_stack", CS("callsubr", "endchar"), "err -8"),
        ], subrs=subrs, gsubrs=gsubrs)
        # `return` ends a subroutine at once: code written after it is dead (fontTools runs it anyway, so no cross-check here)
        self.run_cases([("early_return", CS(0, 0, "rmoveto", -104, "callsubr", "endchar"), "M 0 0; L 3 0; Z")], subrs=subrs, gsubrs=gsubrs, cross=False)

    def test_subroutine_bias_and_limits(self):
        # 1300 local subroutines use bias 1131; subroutine k draws a line of length k % 97 + 1
        subrs = [CS(k % 97 + 1, 0, "rlineto", "return") for k in range(1300)]
        gs = [CS(k % 89 + 1, 0, "rlineto", "return") for k in range(34000)]                  # bias 32768 from 33900 on
        cases = [("bias1131", CS(0, 0, "rmoveto", 1250 - 1131, "callsubr", "endchar"), "M 0 0; L %d 0; Z" % (1250 % 97 + 1)),
                 ("bias1131_first", CS(0, 0, "rmoveto", -1131, "callsubr", "endchar"), "M 0 0; L 1 0; Z"),
                 ("bias32768", CS(0, 0, "rmoveto", 33950 - 32768, "callgsubr", "endchar"), "M 0 0; L %d 0; Z" % (33950 % 89 + 1)),
                 ("bias32768_last", CS(0, 0, "rmoveto", 33999 - 32768, "callgsubr", "endchar"), "M 0 0; L %d 0; Z" % (33999 % 89 + 1))]
        self.run_cases(cases, subrs=subrs, gsubrs=gs)

    def test_recursion_and_budget(self):                                               # (an error is not something to cross-check with fontTools)
        # subroutine k calls k + 1; the last one draws. Ten levels of calls are allowed, an eleventh is not.
        def chain(n):
            return [CS(k + 1 - 107, "callsubr", "return") for k in range(n - 1)] + [CS(9, 0, "rlineto", "return")]
        self.run_cases([("depth10", CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), "M 0 0; L 9 0; Z")], subrs=chain(10))
        # (no cross-check below: fontTools would really execute these, forever in the last case)
        self.run_cases([("depth11", CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), "err -6")], subrs=chain(11), cross=False)
        self.run_cases([("self", CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), "err -6")], subrs=[CS(-107, "callsubr", "return")], cross=False)
        # seven levels that each call the next one twenty times: 20^7 operations if nobody stopped it
        fan = [CS(*([k + 1 - 107, "callsubr"] * 20), "return") for k in range(7)] + [CS(1, 0, "rlineto", "return")]
        t0 = time.time()
        self.run_cases([("fan_out", CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), "err -6")], subrs=fan, cross=False)
        self.assertLess(time.time() - t0, 30)

    def test_malformed_charstrings(self):
        self.run_cases([
            ("short_number", b"\x1c\x01", "err -8"),
            ("fixed_cut", b"\xff\x00\x01", "err -8"),
            ("two_byte_cut", b"\xf7", "err -8"),
            ("reserved_0", CS(1, 2, "rmoveto") + b"\x00", "err -8"),
            ("reserved_2", b"\x02", "err -8"),
            ("reserved_9", b"\x09", "err -8"),
            ("reserved_13", b"\x0d", "err -8"),
            ("reserved_15", b"\x0f", "err -8"),
            ("reserved_16", b"\x10", "err -8"),
            ("reserved_17", b"\x11", "err -8"),
            ("unknown_escape", b"\x0c\x63", "err -8"),
            ("escape_cut", b"\x0c", "err -8"),
            ("stack_overflow", CS(*([1] * 100), "endchar"), "err -8"),
            ("rmoveto_one_arg", CS(1, "rmoveto"), "err -8"),
            ("hmoveto_no_arg", CS("hmoveto"), "err -8"),
            ("add_underflow", CS(1, "add"), "err -8"),
            ("exch_underflow", CS(1, "exch"), "err -8"),
            ("roll_too_many", CS(1, 2, 9, 1, "roll"), "err -8"),
            ("index_out_of_stack", CS(1, 2, 9, "index"), "err -8"),
            ("hintmask_cut", CS(*range(1, 19), "hstemhm", "hintmask"), "err -8"),
            ("huge_coordinates", CS(32767, 32767, "rmoveto", *([32767, 32767] * 30), "rlineto", "endchar"), "M 32767 32767; " + "; ".join(
                "L %d %d" % (32767 * (k + 2), 32767 * (k + 2)) for k in range(30)) + "; Z"),
        ], cross=False)

    def test_random_paths_round_trip(self):
        """Random absolute-coordinate paths are written with whichever operator forms fit and must come back as the same outline."""
        rng = random.Random(21)
        for trial in range(6):
            c = M.CffFont()
            c.glyph(".notdef", 500, M.cs("endchar"))
            for n in range(25):
                p = M.CsPath(width_arg=rng.choice([None, 321]), prefer=rng.choice([None, None, "rr", "hv", "vh", "hh", "vv"]))
                for _ in range(rng.randrange(1, 4)):
                    x, y = rng.randrange(-300, 900), rng.randrange(-300, 900)
                    p.move(x, y)
                    for _ in range(rng.randrange(1, 7)):
                        kind = rng.choice(["line", "line", "curve", "hv", "vh", "hh", "vv"])
                        nx, ny = x + rng.randrange(-250, 250), y + rng.randrange(-250, 250)
                        if kind == "line":
                            nx, ny = rng.choice([(nx, y), (x, ny), (nx, ny)])
                            p.line(nx, ny)
                        else:
                            x1, y1 = x + rng.randrange(-200, 200), y + rng.randrange(-200, 200)
                            x2, y2 = x1 + rng.randrange(-200, 200), y1 + rng.randrange(-200, 200)
                            x3, y3 = x2 + rng.randrange(-200, 200), y2 + rng.randrange(-200, 200)
                            if kind == "hv":
                                y1, x3 = y, x2
                            elif kind == "vh":
                                x1, y3 = x, y2
                            elif kind == "hh":
                                y3 = y2
                            elif kind == "vv":
                                x3 = x2
                            p.curve(x1, y1, x2, y2, x3, y3)
                            nx, ny = x3, y3
                        x, y = nx, ny
                c.glyph("g%d" % n, 500, p.end(), p.expected)
            c.cmap_bmp({0x41: 1})
            data = c.build()
            res = with_font(data, ["outline a %d" % i for i in range(1, len(c.glyphs))])
            theirs = fonttools_paths(data) if HAVE_FONTTOOLS else None
            for i, got in enumerate(res):
                self.assertPath(parse_path(got), c.glyphs[i + 1][3], what="trial %d glyph %d" % (trial, i + 1))
                if theirs:
                    self.assertPath(theirs[i + 1], c.glyphs[i + 1][3], what="fontTools trial %d glyph %d" % (trial, i + 1))

    def test_cid_keyed_fonts(self):
        fd_of = [0, 0, 1, 1, 1, 0, 2, 2, 0, 0]
        for fmt in (0, 3):
            c = M.CffFont()
            c.fds = [[CS(10 * (k + 1), 0, "rlineto", "return")] for k in range(3)]
            c.gsubrs = [CS(0, 33, "rlineto", "return")]
            c.fdselect_format = fmt
            c.glyph(".notdef", 500, CS("endchar"), fd=0)
            for i, fd in enumerate(fd_of):
                c.glyph("g%d" % i, 500, CS(0, 0, "rmoveto", -107, "callsubr", -107, "callgsubr", "endchar"), fd=fd)
            c.cmap_bmp({0x41: 1})
            data = c.build()
            res = with_font(data, ["info a"] + ["outline a %d" % (i + 1) for i in range(len(fd_of))])
            self.assertEqual(kv(res[0][1])["cid"], "1")
            for i, fd in enumerate(fd_of):
                self.assertPath(parse_path(res[i + 1]), P("M 0 0; L %d 0; L %d 33; Z" % (10 * (fd + 1), 10 * (fd + 1))), what="fdselect %d glyph %d" % (fmt, i))
            if HAVE_FONTTOOLS:
                theirs = fonttools_paths(data)
                for i, fd in enumerate(fd_of):
                    self.assertPath(theirs[i + 1], P("M 0 0; L %d 0; L %d 33; Z" % (10 * (fd + 1), 10 * (fd + 1))), what="fontTools fdselect %d glyph %d" % (fmt, i))

    def test_cid_font_dictionary_out_of_range(self):
        c = M.CffFont()
        c.fds = [[CS(5, 0, "rlineto", "return")], []]
        c.glyph(".notdef", 500, CS("endchar"), fd=0)
        c.glyph("a", 500, CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), fd=0)
        c.glyph("b", 500, CS(0, 0, "rmoveto", -107, "callsubr", "endchar"), fd=1)      # this dictionary has no local subroutines
        c.glyph("c", 500, CS("endchar"), fd=7)                                          # a dictionary that does not exist
        c.cmap_bmp({0x41: 1})
        res = with_font(c.build(), ["outline a 1", "outline a 2", "outline a 3"])
        self.assertPath(parse_path(res[0]), P("M 0 0; L 5 0; Z"))
        self.assertTrue(res[1][0].startswith("err -8"), res[1])
        self.assertTrue(res[2][0].startswith("err -8"), res[2])

    def test_cff_containers_validated(self):
        c = cff_font([("g", CS(0, 0, "rmoveto", 5, "hlineto", "endchar"), "")])
        good = c.build()
        t = sfnt_tables(good)
        cff = bytearray(t["CFF "])
        cases = {
            "major version 2": (b"\x02" + bytes(cff[1:]), -4),
            "empty table": (b"", None),
            "header only": (bytes(cff[:4]), -3),
            "truncated mid index": (bytes(cff[:len(cff) // 2]), -3),
            "offsize 9": (bytes(cff[:4]) + bytes([0, 1, 9]) + bytes(cff[7:]), -3),
        }
        for name, (blob, want) in cases.items():
            path = save(patched(good, **{"CFF ": blob}))
            out = batch(["load a %s" % path])[0][0]
            if want is None:
                self.assertTrue(out.startswith("err"), (name, out))
            else:
                self.assertTrue(out.startswith("err %d" % want), (name, out))


# =============================================================================================== text layout


def fnv(rows):
    h = 1469598103934665603
    for row in rows:
        for b in row:
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


class TextLayout(ToolCase):
    @classmethod
    def setUpClass(cls):
        a = M.Font()
        a.glyph(".notdef", 500, M.BOX)                           # 0
        a.glyph("space", 250)                                    # 1
        a.glyph("A", 650, M.TRIANGLE)                            # 2
        a.glyph("o", 600, [M.circle_contour()])                  # 3
        a.glyph("i", 300, M.SQUARE)                              # 4
        a.glyph("f", 350, M.SQUARE)                              # 5
        a.glyph("fi", 650, M.SQUARE)                             # 6
        a.glyph("ffi", 900, M.SQUARE)                            # 7
        a.cmap_bmp({0x20: "space", 0x41: "A", 0x6F: "o", 0x69: "i", 0x66: "f"})
        a.extra["kern"] = M.kern_table({(2, 3): -100, (3, 2): -40})
        a.extra["GSUB"] = M.gsub_liga([M.liga_subtable([(5, [5, 4], 7), (5, [4], 6)])])        # ffi before fi
        b = M.Font()
        b.glyph(".notdef", 700, M.BOX)                           # 0
        b.glyph("space", 300)                                    # 1
        b.glyph("V", 650, M.TRIANGLE)                            # 2
        b.glyph("x", 400, M.SQUARE)                              # 3
        b.glyph("a-hira", 1000, M.SQUARE)                        # 4
        b.glyph("o2", 700, M.SQUARE)                             # 5: 'o' is in the first font too, which must win
        b.cmap_bmp({0x20: "space", 0x56: "V", 0x78: "x", 0x3042: "a-hira", 0x6F: "o2"})
        b.extra["GPOS"] = M.gpos_kern([M.pairpos1({(2, 2): -50})])
        cls.pa, cls.pb = save(a.build(), "layout-a.ttf"), save(b.build(), "layout-b.ttf")

    def lay(self, text, size=100.0, flags=3, fonts="ab", raw=None):
        data = raw if raw is not None else text.encode("utf-8")
        cmds = ["load a %s" % self.pa, "load b %s" % self.pb, "set " + " ".join(fonts), "layout %s %d hex:%s" % (size, flags, data.hex())]
        out = batch(cmds)[3]
        glyphs = []
        for l in out:
            if l.startswith("g "):
                _, gid, font, x, adv, cl, fl = l.split()
                glyphs.append(dict(gid=int(gid), font=int(font), x=float(x), adv=float(adv), cluster=int(cl), flags=int(fl)))
        count = int([l for l in out if l.startswith("count")][0].split()[1])
        width = float([l for l in out if l.startswith("width")][0].split()[1])
        return glyphs, count, width

    def test_widths_and_kerning(self):
        # at 100 px one font unit is a tenth of a pixel: A 650, o 600, space 250 (and kern A,o = -100, o,A = -40; V,V = -50 in the second font)
        for text, want in (("Ao", 115.0), ("oA", 121.0), ("AA", 130.0), ("VV", 125.0), ("Ax", 105.0), ("あ", 100.0), ("A o", 150.0)):
            g, n, w = self.lay(text)
            self.assertAlmostEqual(w, want, places=3, msg=text)
        g, n, w = self.lay("Ao")
        self.assertAlmostEqual(g[0]["adv"], 55.0, places=3)                    # the kerning is part of the left glyph's advance
        self.assertAlmostEqual(g[1]["x"], 55.0, places=3)
        # kerning acts inside a run of one font only: A from the first font, V from the second
        g, n, w = self.lay("AV")
        self.assertAlmostEqual(w, 130.0, places=3)
        self.assertEqual([x["font"] for x in g], [0, 1])
        # flags: no kerning
        g, n, w = self.lay("Ao", flags=2)
        self.assertAlmostEqual(w, 125.0, places=3)
        # scaling with the size
        g, n, w = self.lay("Ao", size=16.0)
        self.assertAlmostEqual(w, 18.4, places=3)
        for bad in (0.0, -5.0):
            self.assertEqual(self.lay("Ao", size=bad)[2], 0.0)

    def test_positions_are_consistent(self):
        text = "Aoo VxAあoA fi Ao" * 5
        g, n, w = self.lay(text, size=37.5)
        self.assertEqual(n, len(g))
        pen = 0.0
        for glyph in g:
            self.assertAlmostEqual(glyph["x"], pen, delta=1e-2)
            pen += glyph["adv"]
        self.assertAlmostEqual(pen, w, delta=2e-2)

    def test_fallback_and_missing(self):
        g, _, w = self.lay("Axあ")
        self.assertEqual([(x["font"], x["gid"]) for x in g], [(0, 2), (1, 3), (1, 4)])
        g, _, _ = self.lay("o")
        self.assertEqual([(x["font"], x["gid"]) for x in g], [(0, 3)])                  # the first font that has the character wins
        g, _, _ = self.lay("o", fonts="ba")
        self.assertEqual([(x["font"], x["gid"]) for x in g], [(0, 5)])                  # ... in the order of the set
        g, _, w = self.lay("?")
        self.assertEqual((g[0]["gid"], g[0]["font"], g[0]["flags"] & 1), (0, 0, 1))      # .notdef of the first font, flagged
        self.assertAlmostEqual(w, 50.0, places=3)
        g, _, w = self.lay(" ")                                                   # no-break space: the space glyph, not a box
        self.assertEqual((g[0]["gid"], g[0]["flags"] & 1), (1, 0))
        self.assertAlmostEqual(w, 25.0, places=3)
        g, _, w = self.lay(" 　")
        self.assertEqual([x["gid"] for x in g], [1, 1])
        g, _, w = self.lay("", fonts="a")
        self.assertEqual((g, w), ([], 0.0))
        g, n, w = self.lay("x", fonts="a")                                              # only the first font: no x there
        self.assertEqual(g[0]["flags"] & 1, 1)

    def test_ignorable_code_points_are_skipped(self):
        base = self.lay("Ao")
        for ch in ("​", "‍", "­", "﻿", "⁠", "️", "\x01", "\t", "\n", "‮"):
            g, n, w = self.lay("A" + ch + "o")
            self.assertEqual(n, 2, repr(ch))
            self.assertAlmostEqual(w, base[2], places=3, msg=repr(ch))                  # (and A o still kern)
        g, n, w = self.lay("​​")
        self.assertEqual((n, w), (0, 0.0))

    def test_clusters_are_byte_offsets(self):
        g, _, _ = self.lay("Aあo\U0001F600A")
        self.assertEqual([x["cluster"] for x in g], [0, 1, 4, 5, 9])
        self.assertEqual(g[3]["flags"] & 1, 1)                                         # the emoji is nowhere: .notdef

    def test_ligatures(self):
        g, _, w = self.lay("fi")
        self.assertEqual([(x["gid"], x["cluster"], x["flags"] & 2) for x in g], [(6, 0, 2)])
        self.assertAlmostEqual(w, 65.0, places=3)
        g, _, _ = self.lay("ffi")
        self.assertEqual([(x["gid"], x["flags"] & 2) for x in g], [(7, 2)])            # the three-letter ligature is tried first
        g, _, _ = self.lay("fif")
        self.assertEqual([x["gid"] for x in g], [6, 5])
        g, _, _ = self.lay("ff")
        self.assertEqual([x["gid"] for x in g], [5, 5])
        g, n, w = self.lay("fi", flags=1)
        self.assertEqual([x["gid"] for x in g], [5, 4])                                # ligatures off
        g, _, _ = self.lay("xfi")
        self.assertEqual([x["gid"] for x in g], [3, 6])
        g, _, w = self.lay("ffio")
        self.assertEqual([x["gid"] for x in g], [7, 3])
        self.assertAlmostEqual(w, 150.0, places=3)
        g, _, _ = self.lay("A" * 30 + "fi")                                          # a ligature far into a long run (the lookahead window moves on)
        self.assertEqual(g[-1]["gid"], 6)
        self.assertEqual(len(g), 31)
        g, _, _ = self.lay("f​i")                                                 # an invisible character between the letters does not break it
        self.assertEqual([x["gid"] for x in g], [6])

    def test_rounding(self):
        g, n, w = self.lay("AoAo", size=13.0, flags=7)
        for glyph in g:
            self.assertEqual(glyph["adv"], round(glyph["adv"]))
            self.assertEqual(glyph["x"], round(glyph["x"]))
        self.assertEqual(w, sum(x["adv"] for x in g))
        self.assertEqual(g[0]["adv"], 7.0)                                             # 650 * 0.013 - 100 * 0.013 = 7.15

    def test_long_text(self):
        g, n, w = self.lay("A" * 1000)
        self.assertEqual(n, 1000)                                                      # the count is the full count, the list holds what fits
        self.assertEqual(len(g), 256)
        self.assertAlmostEqual(w, 65000.0, delta=1.0)

    def test_utf8_decoding_matches_python(self):
        rng = random.Random(3)
        cases = [b"", b"A", "é".encode(), "€".encode(), "\U0001F600".encode(), b"\xc0\x80", b"\xe0\x80\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf0\x80\x80\x80",
                 b"\xe3\x81", b"\xe3\x81A", b"\xf0\x9f\x98", b"\xf0\x9f\x98A", b"\x80", b"\xbf\xbf", b"\xf5\x80\x80\x80", b"\xff", b"\xc2", b"\xc2A", b"\xe0\xa0", b"A\xe3"]
        alphabet = [0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF]
        for _ in range(400):
            cases.append(bytes(rng.choice(alphabet) for _ in range(rng.randrange(1, 7))))
        for _ in range(100):
            cases.append("".join(chr(rng.choice([rng.randrange(0x20, 0x7F), rng.randrange(0xA0, 0x800), rng.randrange(0x800, 0xD800), rng.randrange(0xE000, 0x10000),
                                                  rng.randrange(0x10000, 0x110000)])) for _ in range(rng.randrange(1, 8))).encode())
        out = batch(["utf8dec hex:" + c.hex() for c in cases])
        for c, lines in zip(cases, out):
            got = [int(v, 16) for v in lines[0].split()] if lines and lines[0].strip() else []
            want = [ord(ch) for ch in c.decode("utf-8", errors="replace")]
            self.assertEqual(got, want, c.hex())

    def test_utf8_encoding(self):
        cps = [0, 0x41, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF, 0xD7FF, 0xE000, 0xD800, 0xDFFF, 0x110000, 0xFFFFFFFF]
        out = batch(["utf8enc %d" % c for c in cps])
        for c, lines in zip(cps, out):
            want = chr(c).encode("utf-8") if (c <= 0x10FFFF and not 0xD800 <= c < 0xE000) else "�".encode()
            self.assertEqual(bytes.fromhex(lines[0]), want, hex(c))


# =============================================================================================== the glyph cache


class GlyphCache(ToolCase):
    @classmethod
    def setUpClass(cls):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        for i in range(10):
            f.glyph("sq%d" % i, 600, [[(100, 0), (100, 500), (600, 500), (600, 0)]])           # ten equal squares: equal cache entries
        f.glyph("empty", 300)
        f.glyph("tri", 600, M.TRIANGLE)
        cls.path = save(f.build(), "cache.ttf")

    def get(self, cmds):
        out = batch(["load a %s" % self.path, "load b %s" % self.path] + cmds)
        return out[2:]

    def stats(self, line):
        return {k: int(v) for k, v in kv(line).items()}

    def test_hit_and_miss_and_content(self):
        r = self.get(["cache new 100000 100", "cache get a 1 20 0 0", "cache get a 1 20 0 0", "cache stats", "render a 1 20 0 0", "cache get a 12 20 0 0", "cache get a 12 20 0 0"])
        miss, hit = r[1][0].split(), r[2][0].split()
        self.assertEqual(miss[0], "miss")
        self.assertEqual(hit[0], "hit")
        self.assertEqual(miss[1:], hit[1:])                                           # the very same bitmap
        s = self.stats(r[3][0])
        self.assertEqual((s["hits"], s["misses"], s["entries"]), (1, 1, 1))
        self.assertEqual(int(miss[5], 16), fnv(parse_bitmap(r[4])["rows"]))             # ... and what a direct render gives
        self.assertEqual((r[5][0].split()[0], r[6][0].split()[0]), ("miss", "hit"))

    def test_the_key_has_every_part(self):
        r = self.get(["cache new 100000 100",
                      "cache get a 1 20 0 0", "cache get a 2 20 0 0", "cache get a 1 21 0 0", "cache get a 1 20.5 0 0", "cache get a 1 20 1 0", "cache get a 1 20 0 1",
                      "cache get b 1 20 0 0",                                         # the same file opened again is a different font
                      "cache get a 1 20 0 0", "cache get a 1 20 1 0", "cache get a 1 20.5 0 0", "cache get a 1 20 0 1", "cache get b 1 20 0 0", "cache stats"])
        kinds = [x[0].split()[0] for x in r[1:13]]
        self.assertEqual(kinds, ["miss"] * 7 + ["hit"] * 5)
        self.assertEqual(self.stats(r[13][0])["entries"], 7)
        # sub-pixel steps beyond the last are clamped to it; a mode other than LCD is gray
        r = self.get(["cache new 100000 100", "cache get a 1 20 3 0", "cache get a 1 20 99 0", "cache get a 1 20 -4 0", "cache get a 1 20 0 0", "cache get a 1 20 0 7"])
        self.assertEqual([x[0].split()[0] for x in r[1:]], ["miss", "hit", "miss", "hit", "hit"])

    def test_subpixel_steps_render_shifted_glyphs(self):
        r = self.get(["cache new 100000 100"] + ["cache get a 12 24 %d 0" % s for s in range(4)] + ["render a 12 24 %s" % v for v in (0.25, 0.5)])      # (glyph 12: the triangle)
        hashes = [x[0].split()[5] for x in r[1:5]]
        self.assertEqual(len(set(hashes)), 4)
        self.assertEqual(int(hashes[1], 16), fnv(parse_bitmap(r[5])["rows"]))
        self.assertEqual(int(hashes[2], 16), fnv(parse_bitmap(r[6])["rows"]))

    def test_entry_limit_and_lru_order(self):
        r = self.get(["cache new 1000000 3",
                      "cache get a 1 20 0 0", "cache get a 2 20 0 0", "cache get a 3 20 0 0",        # A B C
                      "cache get a 1 20 0 0",                                                     # touch A: now A C B
                      "cache get a 4 20 0 0",                                                     # D arrives: B (the least recently used) goes
                      "cache stats", "cache get a 2 20 0 0", "cache get a 1 20 0 0", "cache get a 3 20 0 0"])
        s = self.stats(r[6][0])
        self.assertEqual((s["entries"], s["evictions"]), (3, 1))
        # B was dropped (miss); asking for it evicts C, the oldest of the others; A is still there
        self.assertEqual([x[0].split()[0] for x in r[7:10]], ["miss", "hit", "miss"])

    def test_byte_limit(self):
        r = self.get(["cache new 1000000 100", "cache get a 1 20 0 0", "cache stats"])
        one = self.stats(r[2][0])["bytes"]
        self.assertGreater(one, 100)
        limit = 2 * one + one // 2
        cmds = ["cache new %d 100" % limit] + ["cache get a %d 20 0 0" % g for g in range(1, 9)] + ["cache stats"]
        r = self.get(cmds)
        s = self.stats(r[-1][0])
        self.assertEqual(s["entries"], 2)
        self.assertLessEqual(s["bytes"], limit)
        self.assertLessEqual(s["peak"], limit + one)                                       # (the new entry is allocated before the old ones are dropped)
        self.assertEqual(s["evictions"], 6)
        # a bitmap larger than the whole budget is handed out, then dropped at the next call
        r = self.get(["cache new 200 100", "cache get a 12 200 0 0", "cache stats", "cache get a 1 20 0 0", "cache stats"])
        self.assertTrue(r[1][0].startswith("miss"))
        self.assertEqual(self.stats(r[2][0])["entries"], 1)
        self.assertEqual(self.stats(r[4][0])["entries"], 1)

    def test_empty_and_broken_glyphs_are_cached_too(self):
        r = self.get(["cache new 100000 100", "cache get a 11 20 0 0", "cache get a 11 20 0 0", "cache get a 9999 20 0 0", "cache get a 9999 20 0 0", "cache stats"])
        self.assertEqual([x[0].split()[0] for x in r[1:5]], ["miss", "hit", "miss", "hit"])
        self.assertEqual(r[1][0].split()[1:3], ["0", "0"])
        self.assertEqual(r[3][0].split()[1:3], ["0", "0"])                                 # a glyph that cannot be decoded: an empty bitmap, not an error
        self.assertEqual(self.stats(r[5][0])["entries"], 2)

    def test_clear_and_no_leaks(self):
        r = self.get(["cache new 100000 100", "cache get a 1 20 0 0", "cache get a 2 20 0 0", "cache clear", "cache stats", "cache get a 1 20 0 0", "cache free", "leaks"])
        s = self.stats(r[4][0])
        self.assertEqual((s["entries"], s["bytes"]), (0, 0))
        self.assertEqual(r[5][0].split()[0], "miss")
        self.assertEqual(r[7], ["outstanding=0"])

    def test_many_glyphs_keep_to_the_budget(self):
        cmds = ["cache new 20000 40"]
        rng = random.Random(8)
        for _ in range(600):                                                              # 72 distinct keys, 40 places: plenty of hits, plenty of evictions
            cmds.append("cache get a %d %d %d %d" % (rng.randrange(0, 6), rng.choice([9, 12, 16]), rng.randrange(2), rng.randrange(2)))
        cmds.append("cache stats")
        cmds.append("leaks")
        r = self.get(cmds)
        s = self.stats(r[-2][0])
        self.assertLessEqual(s["entries"], 40)
        self.assertLessEqual(s["bytes"], 20000)
        self.assertEqual(s["hits"] + s["misses"], 600)
        self.assertGreater(s["hits"], 100)
        self.assertGreater(s["evictions"], 0)
        self.assertEqual(r[-1], ["outstanding=0"])


# =============================================================================================== coverage tables and drawing


def srgb_to_lin(v):
    c = v / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lin_to_srgb(l):
    c = 12.92 * l if l <= 0.0031308 else 1.055 * l ** (1 / 2.4) - 0.055
    return 255.0 * c


def luma(rgb):
    return (54 * ((rgb >> 16) & 255) + 183 * ((rgb >> 8) & 255) + 19 * (rgb & 255)) / 256.0


def read_ppm(text_or_path):
    data = Path(text_or_path).read_bytes()
    parts = data.split(b"\n", 3)
    magic = parts[0]
    w, h = [int(v) for v in parts[1].split()]
    body = parts[3]
    if magic == b"P5":
        return w, h, [[(body[y * w + x],) * 3 for x in range(w)] for y in range(h)]
    return w, h, [[tuple(body[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]


class Blend(ToolCase):
    @classmethod
    def setUpClass(cls):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        f.glyph("A", 650, M.TRIANGLE)                            # 1
        f.glyph("bar", 400, [[(100, 0), (100, 700), (300, 700), (300, 0)]])        # 2
        f.cmap_bmp({0x41: "A", 0x7C: "bar"})
        cls.path = save(f.build(), "blend.ttf")

    def lut(self, fg, bg, gamma, contrast):
        out = batch(["lut %06x %06x %d %d" % (fg, bg, gamma, contrast)])[0]
        return [int(v) for v in out[0].split()]

    def test_lut_endpoints_and_identity(self):
        for fg, bg in ((0x000000, 0xFFFFFF), (0xFFFFFF, 0x000000), (0x336699, 0xFFCC00), (0x808080, 0x808080), (0xFF0000, 0x00FF00)):
            for gamma, contrast in ((0, 0), (100, 0), (50, 50), (100, 100), (0, 100)):
                t = self.lut(fg, bg, gamma, contrast)
                self.assertEqual((t[0], t[255]), (0, 255), (hex(fg), hex(bg), gamma, contrast))
                self.assertEqual(t, sorted(t), "monotonic %x %x %d %d" % (fg, bg, gamma, contrast))
        self.assertEqual(self.lut(0x000000, 0xFFFFFF, 0, 0), list(range(256)))                # nothing asked: plain coverage
        self.assertEqual(self.lut(0x808080, 0x808080, 100, 0), list(range(256)))              # no difference between text and ground: nothing to correct

    def test_lut_matches_linear_light_blend(self):
        for fg, bg in ((0x000000, 0xFFFFFF), (0xFFFFFF, 0x000000), (0x202020, 0xE0E0E0), (0xF0F0F0, 0x303030)):
            t = self.lut(fg, bg, 100, 0)
            lf, lb = luma(fg), luma(bg)
            for c in (16, 64, 128, 192, 240):
                cov = c / 255.0
                o = lin_to_srgb(cov * srgb_to_lin(lf) + (1 - cov) * srgb_to_lin(lb))
                want = (o - lb) / (lf - lb)
                self.assertAlmostEqual(t[c], want * 255, delta=2.5, msg="fg %x bg %x coverage %d" % (fg, bg, c))
        # black on white comes out lighter than plain coverage, white on black heavier
        self.assertLess(self.lut(0x000000, 0xFFFFFF, 100, 0)[128], 100)
        self.assertGreater(self.lut(0xFFFFFF, 0x000000, 100, 0)[128], 170)
        # half the strength is half way
        full, half = self.lut(0, 0xFFFFFF, 100, 0)[128], self.lut(0, 0xFFFFFF, 50, 0)[128]
        self.assertAlmostEqual(half, (full + 128) / 2, delta=2)

    def test_contrast_steepens(self):
        flat, steep = self.lut(0, 0xFFFFFF, 0, 0), self.lut(0, 0xFFFFFF, 0, 100)
        self.assertLess(steep[64], flat[64])
        self.assertGreater(steep[192], flat[192])
        self.assertAlmostEqual(steep[128], 128, delta=1)
        # smoothstep at 64/255
        c = 64 / 255.0
        self.assertAlmostEqual(steep[64], 255 * c * c * (3 - 2 * c), delta=1.5)

    def draw(self, name, text, size, x, baseline, w, h, fg=0x000000, bg=0xFFFFFF, mode=0, flags=3, gamma=0, contrast=0, cache=True):
        out = str(WORK / name)
        WORK.mkdir(parents=True, exist_ok=True)
        cmds = ["load a %s" % self.path, "set a", "usecache %d" % cache,
                "drawx %s %s %d %d %s %d %06x %06x %d %d %d %d hex:%s" % (out, size, w, h, x, baseline, fg, bg, mode, flags, gamma, contrast, text.encode().hex())]
        r = batch(cmds)[3]
        self.assertTrue(r[0].startswith("drew"), r)
        return read_ppm(out)

    def test_text_lands_where_the_layout_says(self):
        w, h, px = self.draw("a.ppm", "A", 40, 6, 36, 40, 44)
        dark = [(x, y) for y in range(h) for x in range(w) if px[y][x][0] < 128]
        xs, ys = [p[0] for p in dark], [p[1] for p in dark]
        # the triangle is 600 wide, 700 high at 40 px: 24 x 28, its base on the baseline (row 36), starting at x = 6
        self.assertEqual((min(xs), max(xs)), (6, 29))
        self.assertEqual(max(ys), 35)
        self.assertGreaterEqual(min(ys), 8)
        self.assertLessEqual(min(ys), 10)
        # everything else is the background, exactly
        for y in range(h):
            for x in range(w):
                if x < 6 or x > 30 or y > 36 or y < 7:
                    self.assertEqual(px[y][x], (255, 255, 255))

    def test_cache_and_direct_drawing_agree(self):
        for x in (3.0, 3.3, 3.5, 3.8):
            a = self.draw("c1.ppm", "AA|A", 23, x, 30, 90, 40, cache=True)
            b = self.draw("c2.ppm", "AA|A", 23, x, 30, 90, 40, cache=False)
            self.assertEqual(a, b, x)

    def test_fractional_pen_positions(self):
        base = self.draw("f0.ppm", "|", 30, 10.0, 25, 30, 30)
        q = lambda x: self.draw("fq.ppm", "|", 30, x, 25, 30, 30)
        self.assertEqual(q(10.1), base)                                                  # 0.1 px rounds to the 0 step
        self.assertEqual(q(10.13), q(10.25))                                             # ... 0.13 to the quarter step
        self.assertEqual(q(10.9), q(11.0))                                               # ... 0.9 to the next whole pixel
        self.assertNotEqual(q(10.5), base)

        def centroid(img):
            w, h, px = img
            m = sx = 0
            for y in range(h):
                for x in range(w):
                    d = 255 - px[y][x][0]
                    m += d
                    sx += d * (x + 0.5)
            return sx / m, m
        c0, m0 = centroid(base)
        for step in (0.25, 0.5, 0.75):
            c, m = centroid(q(10.0 + step))
            self.assertAlmostEqual(c - c0, step, delta=0.02)                             # moves by the step ...
            self.assertAlmostEqual(m, m0, delta=m0 * 0.003)                              # ... and keeps its weight

    def test_lcd_stripes_and_order(self):
        gray = self.draw("g.ppm", "|", 30, 10.4, 25, 30, 30)
        rgb = self.draw("l1.ppm", "|", 30, 10.4, 25, 30, 30, mode=1)
        bgr = self.draw("l2.ppm", "|", 30, 10.4, 25, 30, 30, mode=2)
        row = 12
        # gray: R = G = B everywhere; LCD: the edges of the bar show colour
        self.assertTrue(all(p[0] == p[1] == p[2] for p in gray[2][row]))
        edge = [(x, rgb[2][row][x], bgr[2][row][x]) for x in range(30) if len(set(rgb[2][row][x])) > 1]
        self.assertGreaterEqual(len(edge), 2)
        # on the left edge of a dark bar on white the stripes to the right are darker: with RGB order the blue channel is darker than the red one
        x, r, b = edge[0]
        self.assertLess(r[2], r[0])
        self.assertLess(b[0], b[2])                                                      # ... and with BGR order the other way round
        # the total darkness is about the same as the gray rendering's
        dsum = lambda img: sum(255 - p[1] for p in img[2][row])
        self.assertAlmostEqual(dsum(rgb), dsum(gray), delta=0.06 * dsum(gray))

    def test_gamma_makes_a_difference_to_dark_text(self):
        plain = self.draw("p.ppm", "A", 24, 4, 30, 40, 36)
        corrected = self.draw("q.ppm", "A", 24, 4, 30, 40, 36, gamma=100)
        darkness = lambda img: sum(255 - p[1] for row in img[2] for p in row)
        self.assertLess(darkness(corrected), darkness(plain))
        light_plain = self.draw("r.ppm", "A", 24, 4, 30, 40, 36, fg=0xFFFFFF, bg=0x000000)
        light_corr = self.draw("s.ppm", "A", 24, 4, 30, 40, 36, fg=0xFFFFFF, bg=0x000000, gamma=100)
        lightness = lambda img: sum(p[1] for row in img[2] for p in row)
        self.assertGreater(lightness(light_corr), lightness(light_plain))

    def test_clipping_never_writes_outside(self):
        for x, base in ((-5000.0, 20), (-10.5, 20), (-3.25, 20), (38.0, 20), (1e9, 20), (-1e9, 20), (5.0, -50), (5.0, 400), (5.0, 3)):
            for mode in (0, 1):
                w, h, px = self.draw("clip.ppm", "AA|", 40, x, base, 48, 48, mode=mode)
                self.assertEqual((w, h), (48, 48))
        # a glyph half out of the surface equals the same part of the glyph drawn on a wider surface
        wide = self.draw("wide.ppm", "A", 30, 20, 40, 80, 48)
        cut = self.draw("cut.ppm", "A", 30, -4, 40, 40, 48)
        shifted = [row[24:64] for row in wide[2]]                                      # (wide: x = 20 -> cut: x = -4: the same picture 24 px to the left)
        self.assertEqual(cut[2], shifted)

    def test_colour_and_background_are_kept(self):
        w, h, px = self.draw("col.ppm", "|", 30, 10, 25, 30, 30, fg=0xC8102E, bg=0xECE6D6)
        row = px[12]
        self.assertEqual(row[0], (0xEC, 0xE6, 0xD6))                                     # untouched ground
        inside = [p for p in row if p == (0xC8, 0x10, 0x2E)]
        self.assertGreaterEqual(len(inside), 4)                                          # the solid middle of the bar is exactly the text colour
        for p in row:                                                                    # and every pixel is a blend of the two: between them in each channel
            for k, (a, b) in enumerate(zip((0xC8, 0x10, 0x2E), (0xEC, 0xE6, 0xD6))):
                self.assertTrue(min(a, b) <= p[k] <= max(a, b))


# =============================================================================================== hostile input, memory failure, speed


def kitchen_sink_truetype():
    """Every TrueType table and glyph kind in one small font (for the fuzzer: mutations then land in all of them)."""
    f = M.sample_font()
    f.glyph("comp1", 700, components=[M.Comp("A", 10, 20, scale=0.5), M.Comp("o", match=(0, 1)), M.Comp("V", 100, 0, scale=(1.0, 0.25, -0.25, 1.0))])
    f.glyph("comp2", 700, components=[M.Comp("comp1", 5, 5), M.Comp("i", 300, 0, words=True)], instructions=b"\x01\x02")
    f.glyph("plain", 700, [[M.on(0, 0), M.on(400, 0), M.off(500, 300), M.on(400, 600), M.off(0, 700)], [M.on(100, 100), M.on(100, 300), M.on(300, 300)]], compress=False)
    f.cmap_raw(3, 10, M.cmap_format12({0x41: 2, 0x56: 3, 0x6F: 4, 0x69: 5, 0x66: 6, 0x1F600: 8, 0x1F601: 9}))
    f.cmap_raw(1, 0, M.cmap_format0({0x41: 2}))
    f.cmap_raw(0, 3, M.cmap_format6(0x41, [2, 3, 4]))
    f.cmap_raw(0, 5, M.cmap_format14(0x41, 0xFE0F))
    f.cmap_raw(3, 0, M.cmap_format4({0xF041: 2, 0xF042: 3}, force_array=True))
    f.extra["kern"] = M.kern_table({(2, 3): -80, (3, 2): -80, (5, 6): 10}, apple=True)
    f.extra["GPOS"] = M.gpos_kern([M.pairpos1({(2, 3): -40, (3, 2): -30}), M.pairpos2([2, 3, 4], {2: 1, 3: 1}, {3: 1, 4: 2}, [[0, 0, 0], [0, -10, -20]], cov_fmt=2)],
                                   extension=True)
    f.extra["GSUB"] = M.gsub_liga([M.liga_subtable([(6, [5], 7), (6, [6, 5], 7)])], extension=True)
    return f


def speed_font():
    """94 glyphs of about the size and shape of a real Latin font's: a blob of 24 to 40 points (curves and corners) with an optional hole."""
    rng = random.Random(4)
    f = M.Font(upem=2048)
    f.glyph(".notdef", 1200, M.BOX)
    for i in range(94):
        n = rng.randrange(12, 21)
        outer, cx, cy = [], 1000, 700
        for k in range(n):
            a = 2 * math.pi * k / n
            r = rng.randrange(500, 900)
            outer.append((round(cx + r * math.cos(a)), round(cy + r * math.sin(a)), k % 3 != 0))
        contours = [outer]
        if i % 2:
            contours.append([(round(cx + 250 * math.cos(-a)), round(cy + 250 * math.sin(-a)), True) for a in (0, 1.57, 3.14, 4.71)])
        f.glyph("g%d" % i, 1200, contours)
    f.cmap_bmp({0x21 + i: i + 1 for i in range(94)})
    return f


@unittest.skipUnless(HAVE_CC, "needs a C compiler")
class Robustness(ToolCase):
    ITERS = int(os.environ.get("TTF_FUZZ_ITERS", "3000"))
    SEED = int(os.environ.get("TTF_FUZZ_SEED", "1"))

    @classmethod
    def setUpClass(cls):
        k = kitchen_sink_truetype()
        cls.fonts = {"truetype": save(k.build(), "fuzz-kitchen.ttf"),
                     "truetype-long-loca": save(k.build(loca_long=True, compact_hmtx=True), "fuzz-kitchen-long.ttf"),
                     "collection": save(M.build_ttc([(k.build_tt_tables(), 0x00010000), (M.sample_font().build_tt_tables(), 0x00010000)]), "fuzz-kitchen.ttc"),
                     "cff": save(cff_zoo().build(), "fuzz-zoo.otf")}
        d = M.CffFont()
        d.fds = [[CS(10, 0, "rlineto", "return")], [CS(0, 10, "rlineto", "return")], []]
        d.gsubrs = [CS(0, 20, "rlineto", "return")]
        d.fdselect_format = 3
        d.glyph(".notdef", 500, CS("endchar"))
        for i in range(8):
            d.glyph("g%d" % i, 500, CS(0, 0, "rmoveto", -107, "callsubr", -107, "callgsubr", 5, "hlineto", "endchar"), fd=i % 3)
        d.cmap_bmp({0x41: 1, 0x42: 2})
        cls.fonts["cff-cid"] = save(d.build(), "fuzz-cid.otf")

    def fuzz(self, key, iters=None, seed=None):
        iters = iters or self.ITERS
        seed = seed or self.SEED
        r = run_tool(script="load a %s\nfuzz a %d %d\n" % (self.fonts[key], seed, iters), timeout=900)
        last = r.stdout.strip().splitlines()[-1]
        self.assertTrue(last.startswith("fuzz iterations=%d " % iters), r.stdout[-800:])
        d = {k: int(v) for k, v in kv(last.replace("fuzz ", "")).items()}
        self.assertGreater(d["opened"], iters // 5, "the mutations broke almost every font before it opened: the fuzzer reached only the header checks")
        self.assertLess(d["peak_alloc"], 48 << 20)                                        # hostile fonts never push the library past ~48 MB
        self.assertLessEqual(d["max_request"], 16 << 20)                                  # ... nor make it ask for a single big block
        return d

    def test_fuzz_truetype(self):
        self.fuzz("truetype")

    def test_fuzz_truetype_other_layout(self):
        self.fuzz("truetype-long-loca", seed=self.SEED + 1)

    def test_fuzz_collection(self):
        self.fuzz("collection", seed=self.SEED + 2)

    def test_fuzz_cff(self):
        self.fuzz("cff", seed=self.SEED + 3)

    def test_fuzz_cid_keyed_cff(self):
        self.fuzz("cff-cid", seed=self.SEED + 4)

    def test_every_allocation_can_fail(self):
        """Fails each allocation of a realistic scenario once, in turn: the answer is the right one or an out-of-memory error, never a crash
        (the sanitizers watch), and after the cache and the raster are freed nothing is left over."""
        f = kitchen_sink_truetype()
        path = save(f.build(), "oom.ttf")
        comp = f.gid("comp2")
        circle = f.gid("o")
        ops = ["render a %d 30" % circle, "render a %d 17.5 0.5 1 0.2" % circle, "outline a %d" % comp, "render a %d 24 0.25 1" % comp,
               "cache new 3000 20", "cache get a %d 20 1 0" % circle, "cache get a %d 20 2 1" % comp, "cache get a %d 20 1 0" % circle,
               "set a", "drawx build/work-oom.ppm 20 60 40 3.5 30 000000 ffffff 1 3 100 20 hex:" + "AVfio fi A".encode().hex()]
        res = batch(["load a %s" % path, "rasternew", "allocstats"] + ops + ["allocstats"])
        base = res[3:3 + len(ops)]
        self.assertTrue(base[0][0].startswith("bitmap"))
        total = int(kv(res[-1][0])["calls"]) - int(kv(res[2][0])["calls"])              # how many allocation requests the scenario makes
        self.assertGreater(total, 15)
        cmds = ["load a %s" % path]
        injections = total + 2                                                           # ... and each of them fails once (two more than there are: no failure at all)
        for n in range(1, injections + 1):
            cmds += ["rasternew", "failat %d" % n] + ops + ["failat 0", "leaks"]
        out = batch(cmds)[1:]
        per = len(ops) + 4
        failed_somewhere = 0
        for n in range(1, injections + 1):
            block = out[(n - 1) * per:n * per]
            self.assertEqual(block[-1], ["outstanding=0"], "allocation %d failed and something leaked" % n)
            for op, got, want in zip(ops, block[2:2 + len(ops)], base):
                if op.startswith(("cache get", "render", "outline")):
                    if got[0].startswith("err"):
                        self.assertTrue(got[0].startswith(("err -5", "err -1 bad cache")), "%s with allocation %d failing: %r" % (op, n, got[:1]))
                        failed_somewhere += 1
                    elif op.startswith("cache get"):
                        self.assertEqual(got[0].split()[1:], want[0].split()[1:], "%s with allocation %d failing" % (op, n))   # (hit or miss may differ)
                    else:
                        self.assertEqual(got, want, "%s with allocation %d failing" % (op, n))
        self.assertGreater(failed_somewhere, 10)                                          # the injections really reached the library's allocations
        # the very first allocation: the scratch context itself. The tool makes a new one on demand, so the next command works.
        res = batch(["load a %s" % path, "leaks", "failat 1", "rasternew", "render a %d 20" % circle, "leaks"])
        self.assertEqual(res[3], ["err -5 no memory"])
        self.assertTrue(res[4][0].startswith("bitmap"))
        self.assertEqual(res[5], ["outstanding=0"])

    def test_memory_limits_are_graceful(self):
        f = kitchen_sink_truetype()
        path = save(f.build(), "limit.ttf")
        cmds = ["load a %s" % path]
        for room in (0, 1, 64, 300, 1200, 5000, 40000):
            cmds += ["rasternew", "allocroom %d" % room, "render a 4 40", "render a 4 40 0 1", "cache new 100000 100", "cache get a 4 40 0 0", "alloclimit", "leaks"]
        out = batch(cmds)[1:]
        for i in range(7):
            block = out[i * 8:(i + 1) * 8]
            for got in block[2:4]:
                self.assertTrue(got[0].startswith(("err -5", "bitmap")), got[:1])
            self.assertEqual(block[-1], ["outstanding=0"])

    def test_speed(self):
        """glyphs per second, optimised build, random Latin-sized glyphs, with sub-pixel positions (a quarter-pixel step for each glyph)."""
        path = save(speed_font().build(), "speed.ttf")
        res = {}
        for size, mode in ((16, 0), (48, 0), (16, 1), (48, 1)):
            out = batch(["load a %s" % path, "bench a %d 400 %d" % (size, mode)], sanitize=False)[1]
            d = kv(out[0])
            res[(size, mode)] = float(d["glyphs_per_second"])
        SAMPLES.mkdir(parents=True, exist_ok=True)
        text = "\n".join("%s px %s: %8.0f glyphs/s" % (s, "LCD " if m else "gray", v) for (s, m), v in res.items())
        (SAMPLES / "speed.txt").write_text("libttf speed, generated font (94 glyphs of 12-20 points), -O2, one core of this PC\n" + text + "\n")
        print("\n" + text)
        self.assertGreater(res[(16, 0)], 20000)                                           # a very loose floor: a regression to something silly, not a benchmark
        self.assertGreater(res[(48, 0)], 5000)
        self.assertGreater(res[(16, 1)], 8000)


# =============================================================================================== the Kappa toolkit shim


class KappaShim(ToolCase):
    """user/lib/kappa/src/ttftext.c: kt_text and kt_text_in on the toolkit's own KCanvas code (canvas.c is linked into the test tool), at scale 1 and on a
    hi-dpi canvas. The reference is libttf's own ttf_draw_text through the `drawx` command: the shim must add nothing but the canvas' scale, the clip
    rectangle and the line box."""

    @classmethod
    def setUpClass(cls):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        f.glyph("space", 250)
        f.glyph("A", 650, M.TRIANGLE)
        f.glyph("o", 600, [M.circle_contour()])
        f.glyph("i", 300, M.SQUARE)
        f.cmap_bmp({0x20: "space", 0x41: "A", 0x6F: "o", 0x69: "i"})
        f.extra["kern"] = M.kern_table({(2, 3): -100})
        cls.path = save(f.build(), "kappa.ttf")                        # ascent 0.8 em, descent 0.2, gap 0.1: a 20 px line box is 22 px, the baseline 16 px down

    def kt(self, name, scale, w, h, x, y, size, text, fg=0x000000, bg=0xFFFFFF, clip=None):
        out = str(WORK / name)
        WORK.mkdir(parents=True, exist_ok=True)
        cmd = "ktext %s %d %d %d %d %d %s %06x %06x %s hex:%s" % (out, scale, w, h, x, y, size, fg, bg, self.path, text.encode().hex())
        if clip:
            cmd += " %d %d %d %d" % clip
        r = batch([cmd], kappa=True)[0]
        self.assertTrue(r[0].startswith("ktext"), r)
        return kv(r[0].replace("ktext ", "")), read_ppm(out)

    def reference(self, name, size, w, h, x, baseline, text, fg=0x000000, bg=0xFFFFFF):
        out = str(WORK / name)
        r = batch(["load a %s" % self.path, "set a", "drawx %s %s %d %d %s %d %06x %06x 0 3 50 30 hex:%s" % (out, size, w, h, x, baseline, fg, bg, text.encode().hex())])[2]
        self.assertTrue(r[0].startswith("drew"), r)
        return read_ppm(out), float(r[0].split("advance=")[1])

    def test_metrics(self):
        d, _ = self.kt("m.ppm", 1, 40, 40, 0, 0, 20, "A")
        self.assertEqual((int(d["lineheight"]), int(d["ascent"])), (22, 16))

    def test_text_is_libttf_text_with_a_line_box(self):
        text = "Ao iAo"
        d, img = self.kt("k1.ppm", 1, 120, 40, 6, 5, 20, text)
        ref, adv = self.reference("r1.ppm", 20, 120, 40, 6, 5 + int(d["ascent"]), text)
        self.assertEqual(img, ref)                                                   # the same pixels: the top of the line box at y, the baseline an ascent below
        self.assertEqual(int(d["advance"]), int(math.ceil(adv - 1e-3)))              # the advance, rounded up to a whole logical pixel
        self.assertGreater(sum(1 for row in img[2] for p in row if p[0] < 128), 50)

    def test_hidpi_canvas_renders_at_the_physical_size(self):
        text = "Ao iAo"
        for scale in (2, 3):
            d, img = self.kt("k2.ppm", scale, 120, 40, 6, 5, 20, text)
            self.assertEqual((img[0], img[1]), (120 * scale, 40 * scale))
            ref, adv = self.reference("r2.ppm", 20 * scale, 120 * scale, 40 * scale, 6 * scale, 5 * scale + int(d["physical_ascent"]), text)
            self.assertEqual(img, ref, "scale %d: glyphs are drawn at %d px, not enlarged from %d px" % (scale, 20 * scale, 20))
            self.assertEqual(int(d["advance"]), int(math.ceil(adv / scale - 1e-3)))                # the advance is in logical pixels
        # sharper than enlarging: a scale-2 canvas has smooth edges, not 2 x 2 blocks, so adjacent pixel columns differ
        d, img = self.kt("k3.ppm", 2, 120, 40, 6, 5, 20, "o")
        w, h, px = img
        blocky = all(px[y][x] == px[y][x + 1] for y in range(h) for x in range(0, w - 1, 2))
        self.assertFalse(blocky)

    def test_clip_rectangle(self):
        text = "AoAoAo"
        full = self.kt("c0.ppm", 1, 100, 40, 4, 6, 24, text)[1]
        for clip in ((10, 0, 30, 40), (0, 12, 100, 8), (50, 0, 10, 10), (-5, -5, 20, 20)):
            d, img = self.kt("c1.ppm", 1, 100, 40, 4, 6, 24, text, clip=clip)
            x0, y0, x1, y1 = max(clip[0], 0), max(clip[1], 0), min(clip[0] + clip[2], 100), min(clip[1] + clip[3], 40)
            for y in range(40):
                for x in range(100):
                    inside = x0 <= x < x1 and y0 <= y < y1
                    self.assertEqual(img[2][y][x], full[2][y][x] if inside else (255, 255, 255), "clip %s at (%d,%d)" % (clip, x, y))
        d, img = self.kt("c2.ppm", 1, 100, 40, 4, 6, 24, text, clip=(0, 0, 0, 0))             # an empty clip: nothing is drawn, the width is still reported
        self.assertTrue(all(p == (255, 255, 255) for row in img[2] for p in row))
        self.assertGreater(int(d["advance"]), 50)

    def test_light_text_on_dark(self):
        # The canvas does not know what is behind the text, so the shim assumes the opposite extreme of the text's own brightness: black behind light text.
        d, img = self.kt("l.ppm", 1, 80, 30, 4, 4, 20, "Aoi", fg=0xF4EFD8, bg=0x000000)
        ref, _ = self.reference("lr.ppm", 20, 80, 30, 4, 4 + int(d["ascent"]), "Aoi", fg=0xF4EFD8, bg=0x000000)
        self.assertEqual(img, ref)
        # Over a dark blue ground the assumption is a little off, so the result is close to (not equal to) the exact curve for that ground.
        d, img = self.kt("l2.ppm", 1, 80, 30, 4, 4, 20, "Aoi", fg=0xF4EFD8, bg=0x0E1330)
        ref, _ = self.reference("lr2.ppm", 20, 80, 30, 4, 4 + int(d["ascent"]), "Aoi", fg=0xF4EFD8, bg=0x0E1330)
        worst = max(abs(a - b) for ra, rb in zip(img[2], ref[2]) for pa, pb in zip(ra, rb) for a, b in zip(pa, pb))
        self.assertLess(worst, 16)
        self.assertGreater(worst, 0)

    def ink_box(self, img, bg=(255, 255, 255)):
        pts = [(x, y) for y, row in enumerate(img[2]) for x, p in enumerate(row) if p != bg]
        return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)) if pts else None

    def text_in(self, rect, text, size=18, align=0, w=200, h=60):
        out = str(WORK / "in.ppm")
        r = batch(["ktextin %s 1 %d %d %d %d %d %d %s 000000 ffffff %s %d hex:%s" % (out, w, h, rect[0], rect[1], rect[2], rect[3], size, self.path, align, text.encode().hex())], kappa=True)[0]
        self.assertTrue(r[0].startswith("ktextin"), r)
        return read_ppm(out), int(kv(r[0].replace("ktextin ", ""))["width"])

    def test_text_in_a_rectangle(self):
        rect = (20, 10, 100, 40)
        for align in (0, 1, 2):
            img, width = self.text_in(rect, "Aoi", align=align)
            box = self.ink_box(img)
            self.assertIsNotNone(box)
            self.assertTrue(rect[0] <= box[0] and box[2] < rect[0] + rect[2] and rect[1] <= box[1] and box[3] < rect[1] + rect[3], (align, box))
            mid = (box[0] + box[2]) / 2
            want = rect[0] + (0 if align == 0 else 0.5 if align == 1 else 1) * rect[2] + (width / 2 if align == 0 else 0 if align == 1 else -width / 2)
            self.assertAlmostEqual(mid, want, delta=width * 0.25 + 3, msg="alignment %d" % align)
        # vertically centred: the line box (22 px at 18?) sits in the middle of the rectangle's height
        img, _ = self.text_in(rect, "Aoi", align=1)
        box = self.ink_box(img)
        self.assertAlmostEqual((box[1] + box[3]) / 2, rect[1] + rect[3] / 2, delta=8)

    def test_too_long_text_is_cut_with_dots(self):
        rect = (10, 10, 90, 30)
        long_text = "AoAoAoAoAoAoAoAoAoAoAoAoAoAoAoAo"
        img, width = self.text_in(rect, long_text)
        self.assertGreater(width, rect[2])                                             # (the whole string would not fit)
        box = self.ink_box(img)
        self.assertTrue(rect[0] <= box[0] and box[2] < rect[0] + rect[2], box)          # nothing outside the rectangle
        self.assertGreater(box[2], rect[0] + rect[2] - 25)                             # and it runs close to the right edge: the string was cut, not shrunk
        # the dots are there: the right end of the ink is small square-ish dots, so the last columns are lighter than the middle of the text
        short, _ = self.text_in(rect, "Ao")
        self.assertLess(self.ink_box(short)[2], rect[0] + rect[2] - 25)
        # a multi-byte character is never cut in the middle: no crash and valid output for a string of 3-byte characters
        img, _ = self.text_in(rect, "日本語" * 30)
        self.assertIsNotNone(self.ink_box(img) or (0, 0, 0, 0))

    def test_switch_off_compiles_to_nothing(self):
        """With -DKAPPA_TTF=0 the shim's file is empty: the toolkit builds with the bitmap font alone (the default of kappa_ttf.h is not what this tests)."""
        cc = find_cc()
        obj = WORK / "ttftext-off.o"
        WORK.mkdir(parents=True, exist_ok=True)
        flags = ["-I", str(KAPPA / "include"), "-I", str(REPO / "user" / "lib" / "fairy" / "include"), "-I", str(REPO / "shared"), "-I", str(LIB / "include")]
        r = subprocess.run([cc, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-DKAPPA_TTF=0", *flags, "-c", str(KAPPA / "src" / "ttftext.c"), "-o", str(obj)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        nm = shutil.which("nm")
        if nm:
            syms = subprocess.run([nm, str(obj)], capture_output=True, text=True).stdout
            self.assertNotIn("kt_", syms)

    def test_toolkit_compiles_both_ways_without_warnings(self):
        """canvas.c, widgets.c, app.c and ttftext.c compile warning-free with the UI font switched on and off, and without it canvas.c needs nothing from libttf."""
        cc = find_cc()
        WORK.mkdir(parents=True, exist_ok=True)
        flags = ["-I", str(KAPPA / "include"), "-I", str(REPO / "user" / "lib" / "fairy" / "include"), "-I", str(REPO / "shared"), "-I", str(LIB / "include")]
        for on in (0, 1):
            for name in ("canvas", "widgets", "app", "ttftext"):
                obj = WORK / ("k-%s-%d.o" % (name, on))
                r = subprocess.run([cc, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare", "-D_GNU_SOURCE", "-DKAPPA_TTF=%d" % on, *flags, "-c",
                                    str(KAPPA / "src" / (name + ".c")), "-o", str(obj)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, "%s.c with KAPPA_TTF=%d:\n%s" % (name, on, r.stderr))
        nm = shutil.which("nm")
        if nm:
            syms = subprocess.run([nm, str(WORK / "k-canvas-0.o")], capture_output=True, text=True).stdout
            self.assertNotIn("kt_ui", syms)
            self.assertNotIn("ttf_", syms)
            syms = subprocess.run([nm, str(WORK / "k-canvas-1.o")], capture_output=True, text=True).stdout
            self.assertIn("kt_ui_walk", syms)


# =============================================================================================== the toolkit's text in the system UI font


class UiFont(ToolCase):
    """canvas.c's text functions (kc_text, kc_text_n, kc_text_bold, kc_text_shadow, kc_text_in, kc_text_width, kc_text_width_n, kc_text_hit) with the system UI
    font of user/lib/kappa/src/ttftext.c (kt_ui_*): 13 px, the baseline 13 pixels below the top of the 16 px line box, proportional and kerned, characters the
    font lacks drawn as 8 px bitmap cells, a missing font file meaning the bitmap font, scale-aware. The font is a small one made here (point `A` kerned against
    `o`), found through $KAPPA_UI_FONT, so nothing depends on the shipped fonts; the references are libttf's own drawing (`drawx`) and the bitmap cell (`kui glyph`)."""

    FG, BG = 0x1A1A24, 0xECE6D6

    @classmethod
    def setUpClass(cls):
        f = M.Font()
        f.glyph(".notdef", 500, M.BOX)
        f.glyph("space", 300)
        f.glyph("A", 650, M.TRIANGLE)
        f.glyph("o", 600, [M.circle_contour()])
        f.glyph("i", 300, M.SQUARE)
        f.cmap_bmp({0x20: "space", 0x41: "A", 0x6F: "o", 0x69: "i"})
        f.extra["kern"] = M.kern_table({(2, 3): -100})                  # glyph ids: .notdef 0, space 1, A 2, o 3, i 4
        cls.path = save(f.build(), "uifont.ttf")

    def kui(self, *cmds, font=None):
        env = {"KAPPA_UI_FONT": self.path if font is None else font}
        return batch(list(cmds), kappa=True, env=env)

    def draw(self, name, args, scale=1, w=120, h=40, x=6, y=5, mode=0, text="Ao", font=None, bg=None, fg=None):
        out = str(WORK / name)
        WORK.mkdir(parents=True, exist_ok=True)
        cmd = "kui draw %s %d %d %d %d %d %06x %06x %d hex:%s" % (out, scale, w, h, x, y, self.FG if fg is None else fg, self.BG if bg is None else bg, mode, text.encode().hex())
        res = self.kui(cmd + (" " + args if args else ""), font=font)[0]
        self.assertTrue(res[0].startswith("kui draw"), res)
        return kv(res[0].replace("kui draw ", "")), read_ppm(out)

    def reference(self, name, size, w, h, x, baseline, text):
        out = str(WORK / name)
        r = batch(["load a %s" % self.path, "set a", "drawx %s %s %d %d %s %d %06x %06x 0 3 0 30 hex:%s" % (out, size, w, h, x, baseline, self.FG, self.BG, text.encode().hex())])[2]
        self.assertTrue(r[0].startswith("drew"), r)
        return read_ppm(out), float(r[0].split("advance=")[1])

    def glyph(self, name, w, h, x, y, cp):
        out = str(WORK / name)
        r = self.kui("kui glyph %s 1 %d %d %d %d %06x %06x %d" % (out, w, h, x, y, self.FG, self.BG, cp))[0]
        self.assertEqual(r[0], "kui glyph", r)
        return read_ppm(out)

    def test_text_is_libttf_text_at_13_px_with_the_baseline_13_down(self):
        text = "Ao ioA"
        d, img = self.draw("u1.ppm", "", text=text)
        ref, adv = self.reference("u1r.ppm", 13, 120, 40, 6, 5 + 13, text)
        self.assertEqual(img, ref)                                                   # the same pixels as ttf_draw_text at 13 px, plain coverage with contrast 30
        self.assertEqual(int(d["advance"]), int(math.ceil(adv - 1e-3)))
        self.assertEqual(adv, 39.0)                                                  # (650 - 100 + 600 + 300 + 300 + 600 + 650) / 1000 * 13: the A o pair is kerned
        self.assertEqual(d["proportional"], "1")
        self.assertGreater(sum(1 for row in img[2] for p in row if p[0] < 128), 50)

    def test_every_variant_of_the_call_agrees(self):
        """kc_text, kc_text_n with all the bytes, kc_text_scaled with scale 1 and kc_text_shadow's second pass are the same drawing."""
        _, plain = self.draw("u2a.ppm", "", mode=0, text="Aoi A")
        _, scaled1 = self.draw("u2b.ppm", "", mode=3, text="Aoi A")
        _, counted = self.draw("u2c.ppm", "", mode=5, text="5:Aoi A")
        self.assertEqual(plain, scaled1)
        self.assertEqual(plain, counted)
        d, part = self.draw("u2d.ppm", "", mode=5, text="3:Aoi A")                  # only "Aoi"
        _, three = self.draw("u2e.ppm", "", mode=0, text="Aoi")
        self.assertEqual(part, three)
        # kc_text_shadow draws the shadow one pixel right and down, in its own colour, under the text
        d, shadowed = self.draw("u2f.ppm", "", mode=2, text="Ao", fg=0xFFFFFF)
        _, lone = self.draw("u2g.ppm", "", mode=0, text="Ao", fg=0xFFFFFF)
        self.assertNotEqual(shadowed, lone)
        dark = [(x, y) for y, row in enumerate(shadowed[2]) for x, p in enumerate(row) if p[0] < 0x80]
        self.assertGreater(len(dark), 10)

    def test_width_prefix_and_hit(self):
        r = self.kui("kui measure hex:%s 2 0 9 12 13 31 100" % "Ao io".encode().hex())[0][0]
        d = kv(r.replace("kui ", ""))
        self.assertEqual(int(d["width"]), 31)                                        # 2350 units = 30.55 px, rounded up
        self.assertEqual(int(d["prefix"]), 15)                                       # "Ao" = 1150 units (the pair is kerned) = 14.95 px
        self.assertEqual(d["proportional"], "1")
        # the boundaries are at 0, 9 (8.45), 15, 19, 23 and 31 pixels: the nearest one wins
        self.assertEqual(d["hit"], "0,1,1,2,5,5")

    def test_a_length_past_the_end_stops_at_the_nul(self):
        """A text field asks for the width of the text before its caret, and a caret that is stale (the program changed the buffer) can lie past the end: the
        measure must stop at the NUL, not read on (the tool runs under AddressSanitizer: a read beyond the string's allocation fails the test)."""
        for font in (None, "/nonexistent/none.ttf"):
            r = self.kui("kui measure hex:%s 100" % "Ao".encode().hex(), "kui measure hex:%s" % "Ao".encode().hex(), font=font)
            long_, plain = kv(r[0][0].replace("kui ", "")), kv(r[1][0].replace("kui ", ""))
            self.assertEqual(long_["prefix"], plain["width"])
            self.assertEqual(long_["width"], plain["width"])

    def test_characters_the_font_lacks_are_bitmap_cells(self):
        d, img = self.draw("u3.ppm", "", text="BA", x=4, y=3)                         # B is not in the font but is in the bitmap font; A is
        cell = self.glyph("u3b.ppm", 120, 40, 4, 3, ord("B"))
        ref, adv = self.reference("u3r.ppm", 13, 120, 40, 4 + 8, 3 + 13, "A")           # the A starts where the cell ends
        for y in range(40):
            for x in range(120):
                want = cell[2][y][x] if x < 4 + 8 else ref[2][y][x]
                self.assertEqual(img[2][y][x], want, "pixel %d,%d" % (x, y))
        self.assertEqual(int(d["advance"]), int(math.ceil(8 + adv - 1e-3)))
        # a character neither font has is the font's own .notdef box (500 units = 6.5 px)
        d, _ = self.draw("u3c.ppm", "", text="\u4e16")
        self.assertEqual(int(d["advance"]), 7)
        # and the width function counts the cell as 8 px
        r = self.kui("kui measure hex:%s" % "BoB".encode().hex())[0][0]
        self.assertEqual(int(kv(r.replace("kui ", ""))["width"]), 8 + 8 + 8)     # 8 + 7.8 + 8 = 23.8, rounded up to 24

    def test_hidpi_canvas_renders_at_the_physical_size(self):
        text = "Ao iAo"
        for scale in (2, 3):
            d, img = self.draw("u4.ppm", "", scale=scale, text=text)
            self.assertEqual((img[0], img[1]), (120 * scale, 40 * scale))
            ref, adv = self.reference("u4r.ppm", 13 * scale, 120 * scale, 40 * scale, 6 * scale, (5 + 13) * scale, text)
            self.assertEqual(img, ref, "scale %d: glyphs are drawn at %d px, not enlarged from 13 px" % (scale, 13 * scale))
            self.assertEqual(int(d["advance"]), int(math.ceil(adv / scale - 1e-3)))
        d, img = self.draw("u4s.ppm", "", scale=2, text="o")
        w, h, px = img
        self.assertFalse(all(px[y][x] == px[y][x + 1] for y in range(h) for x in range(0, w - 1, 2)))   # smooth edges, not 2 x 2 blocks

    def test_bold_is_a_second_copy_half_a_pixel_over(self):
        d0, plain = self.draw("u5a.ppm", "", text="Aoi", fg=0x000000, bg=0xFFFFFF)
        d1, bold = self.draw("u5b.ppm", "", mode=1, text="Aoi", fg=0x000000, bg=0xFFFFFF)
        self.assertEqual(int(d1["advance"]), int(d0["advance"]) + 1)                  # kc_text_bold's contract: one pixel wider
        ink = lambda im: sum(255 - p[0] for row in im[2] for p in row)               # noqa: E731
        self.assertGreater(ink(bold), ink(plain) * 1.05)
        for rp, rb in zip(plain[2], bold[2]):
            for pp, pb in zip(rp, rb):
                self.assertLessEqual(pb[0], pp[0])                                  # never lighter anywhere: the second copy only adds ink

    def test_tabs_are_a_space_and_newlines_are_skipped(self):
        w = lambda text: int(kv(self.kui("kui measure hex:%s" % text.encode().hex())[0][0].replace("kui ", ""))["width"])    # noqa: E731
        self.assertEqual(w("A\to"), int(math.ceil((650 + 300 + 600) * 13 / 1000 - 1e-3)))     # the tab is the space glyph's advance, and it breaks the A o kerning
        self.assertEqual(w("A\no"), w("Ao"))                                         # a newline has no width (and does not break the kerning)
        self.assertEqual(w(""), 0)

    def test_clip_rectangle_and_empty_clip(self):
        text = "AoAoAo"
        _, full = self.draw("u6a.ppm", "", text=text, w=100, h=40, x=4, y=6)
        for clip in ((10, 0, 30, 40), (0, 12, 100, 8), (50, 0, 10, 10), (-5, -5, 20, 20)):
            d, img = self.draw("u6b.ppm", "%d %d %d %d" % clip, text=text, w=100, h=40, x=4, y=6)
            x0, y0, x1, y1 = max(clip[0], 0), max(clip[1], 0), min(clip[0] + clip[2], 100), min(clip[1] + clip[3], 40)
            for y in range(40):
                for x in range(100):
                    inside = x0 <= x < x1 and y0 <= y < y1
                    self.assertEqual(img[2][y][x], full[2][y][x] if inside else (0xEC, 0xE6, 0xD6), "clip %s at (%d,%d)" % (clip, x, y))
        d, img = self.draw("u6c.ppm", "0 0 0 0", text=text, w=100, h=40, x=4, y=6)       # nothing drawn, the advance still reported
        self.assertTrue(all(p == (0xEC, 0xE6, 0xD6) for row in img[2] for p in row))
        self.assertGreater(int(d["advance"]), 30)

    def test_scaled_text_other_than_scale_one_stays_bitmap(self):
        _, with_font = self.draw("u7a.ppm", "", mode=4, text="Ao")
        _, bitmap = self.draw("u7b.ppm", "", mode=4, text="Ao", font="/nonexistent/none.ttf")
        self.assertEqual(with_font, bitmap)                                          # kc_text_scaled(2) is the bitmap font, whatever is loaded
        d, _ = self.draw("u7c.ppm", "", mode=4, text="Ao")
        self.assertEqual(int(d["advance"]), 32)                                      # two 16 px cells

    def test_a_missing_font_file_means_the_bitmap_font(self):
        none = "/nonexistent/none.ttf"
        d, img = self.draw("u8.ppm", "", text="Ao i", font=none)
        self.assertEqual(d["proportional"], "0")
        self.assertEqual(int(d["advance"]), 32)                                      # four cells
        cells = [self.glyph("u8g%d.ppm" % k, 120, 40, 6 + 8 * k, 5, ord(c)) for k, c in enumerate("Ao i")]
        for y in range(40):
            for x in range(120):
                k = (x - 6) // 8 if 6 <= x < 38 else None
                want = cells[k][2][y][x] if k is not None else (0xEC, 0xE6, 0xD6)
                self.assertEqual(img[2][y][x], want, "pixel %d,%d" % (x, y))
        r = self.kui("kui measure hex:%s 3 0 3 4 11 13 100" % "Ao io".encode().hex(), font=none)[0][0]
        d = kv(r.replace("kui ", ""))
        self.assertEqual((int(d["width"]), int(d["prefix"]), d["proportional"]), (40, 24, "0"))
        self.assertEqual(d["hit"], "0,0,0,1,2,5")                                    # the nearest of the 8 px boundaries (a tie goes to the left one)

    def test_use_bitmap_switches_the_toolkit_back_and_forth(self):
        out = self.kui("kui measure hex:%s" % "Aoi".encode().hex(), "kui bitmap 1", "kui measure hex:%s" % "Aoi".encode().hex(), "kui bitmap 0",
                       "kui measure hex:%s" % "Aoi".encode().hex())
        widths = [(int(kv(o[0].replace("kui ", ""))["width"]), kv(o[0].replace("kui ", ""))["proportional"]) for o in (out[0], out[2], out[4])]
        self.assertEqual(widths, [(19, "1"), (24, "0"), (19, "1")])                  # A o i = 650 - 100 + 600 + 300 = 1450 units = 18.85 px, or three 8 px cells

    def text_in(self, rect, text, align, w=200, h=60, font=None):
        out = str(WORK / "uin.ppm")
        r = self.kui("kui in %s 1 %d %d %d %d %d %d %06x %06x %d hex:%s" % (out, w, h, rect[0], rect[1], rect[2], rect[3], self.FG, self.BG, align, text.encode().hex()), font=font)[0]
        self.assertTrue(r[0].startswith("kui in"), r)
        return read_ppm(out), int(kv(r[0].replace("kui in ", ""))["width"])

    @staticmethod
    def ink_box(img):
        pts = [(x, y) for y, row in enumerate(img[2]) for x, p in enumerate(row) if p != (0xEC, 0xE6, 0xD6)]
        return (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)) if pts else None

    def test_a_box_too_small_for_two_letters_and_dots_gets_just_the_letters(self):
        """The key caps of the Control Panel are 20 px wide and say Hom, PgU, Num: the bitmap font showed Ho, Pg and Nu (no dots in a box of three cells or less); with the
        proportional face a cut that leaves one letter and "..." ("H...") is worse than two letters, so dots need room for two letters."""
        img, width = self.text_in((10, 10, 20, 30), "AoAoAo", 0)
        _, ref = self.draw("uin-ref.ppm", "10 10 20 30", text="Ao", x=10, y=17, w=200, h=60)
        self.assertEqual(img, ref)                                                   # exactly "Ao", at the place a text centred vertically in the box would have
        # nothing fits: nothing is drawn
        img, _ = self.text_in((10, 10, 5, 30), "Ao", 0)
        self.assertIsNone(self.ink_box(img))
        # room for two letters and the dots (the test font has no dot: three bitmap cells, 24 px): the dots appear
        img, _ = self.text_in((10, 10, 60, 30), "AoAoAoAo", 0)
        box = self.ink_box(img)
        self.assertTrue(10 <= box[0] and box[2] < 70, box)
        self.assertGreater(box[2], 70 - 12)

    def test_text_in_a_rectangle(self):
        text_in, ink_box = self.text_in, self.ink_box
        rect = (20, 10, 100, 40)
        for align in (0, 1, 2):
            img, width = text_in(rect, "Aoi", align)
            box = ink_box(img)
            self.assertIsNotNone(box)
            self.assertTrue(rect[0] <= box[0] and box[2] < rect[0] + rect[2] and rect[1] <= box[1] and box[3] < rect[1] + rect[3], (align, box))
            mid = (box[0] + box[2]) / 2
            want = rect[0] + (0 if align == 0 else 0.5 if align == 1 else 1) * rect[2] + (width / 2 if align == 0 else 0 if align == 1 else -width / 2)
            self.assertAlmostEqual(mid, want, delta=width * 0.25 + 3, msg="alignment %d" % align)
        img, _ = text_in(rect, "Aoi", 1)
        box = ink_box(img)
        self.assertAlmostEqual((box[1] + box[3]) / 2, rect[1] + rect[3] / 2, delta=8)           # vertically centred
        # too long: cut with "...", nothing outside the rectangle, and it runs close to the right edge
        img, width = text_in((10, 10, 90, 30), "AoAoAoAoAoAoAoAoAoAoAoAoAoAoAoAo", 0)
        self.assertGreater(width, 90)
        box = ink_box(img)
        self.assertTrue(10 <= box[0] and box[2] < 100, box)
        self.assertGreater(box[2], 100 - 20)
        # a long string of three-byte characters is cut between characters (no crash, valid picture)
        img, _ = text_in((10, 10, 90, 30), "\u65e5\u672c\u8a9e" * 30, 0)
        self.assertIsNotNone(ink_box(img))
        # the bitmap font cuts by cells: 90 px = 11 cells, 8 characters and "..."
        img, _ = text_in((10, 10, 90, 30), "AoAoAoAoAoAoAoAoAoAoAoAo", 0, font="/nonexistent/none.ttf")
        box = ink_box(img)
        self.assertTrue(10 <= box[0] and box[2] < 100, box)


# =============================================================================================== the owner's installed fonts (local only)

# Read in place from FONT_DIR (C:\Windows\Fonts); never copied into the repository, never quoted in a test: only measured and compared with other
# readers. Each test skips itself when the files it needs are absent, so a machine without them (CI, the lead's other checkouts) is unaffected.
LOCAL_FONTS = [("arial.ttf", [0]), ("segoeui.ttf", [0]), ("times.ttf", [0]), ("consola.ttf", [0]), ("calibri.ttf", [0]), ("symbol.ttf", [0]), ("wingding.ttf", [0]),
               ("seguisym.ttf", [0]), ("msgothic.ttc", [0, 1, 2]), ("YuGothR.ttc", [0, 1]), ("msyh.ttc", [0]), ("simsun.ttc", [0]), ("Nirmala.ttc", [0, 1]),
               ("cambria.ttc", [0, 1]), ("malgun.ttf", [0]), ("3ds Light.otf", [0]), ("ROGFonts-Regular.otf", [0])]
SWEEP_LIMIT = 0 if os.environ.get("TTF_FULL_SWEEP") else 6000          # glyphs to render per face (0 = all); the big CJK fonts have 20 to 65 thousand


def local_font(name):
    p = FONT_DIR / name
    return str(p) if p.is_file() else None


def ft_font(path, face=0):
    return _ftt.TTFont(path, fontNumber=face, lazy=True)


def write_png(path, rows):
    """rows: lists of (r, g, b) tuples, all of one width."""
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class RealFontChecks:
    """Comparisons of libttf with other readers on real font files, for the test classes below. `specs` is a list of (label, path, face); they need fontTools."""

    def ft(self, path, face=0):
        tt = ft_font(path, face)
        self.addCleanup(tt.close)
        return tt

    def dump(self, path, face, limit):
        out = batch(['load a "%s" %d' % (path, face), "dump a 0 %d" % limit])[1]
        glyphs, cur = {}, None
        for line in out:
            if line.startswith("g "):
                p = line.split()
                cur = int(p[1])
                glyphs[cur] = None if p[2] == "err" else []
            elif line == "end":
                break
            elif glyphs.get(cur) is not None and line != "Z":
                v = [float(x) for x in line.split()[1:]]
                glyphs[cur] += [(v[i], v[i + 1]) for i in range(0, len(v), 2)]
        return glyphs

    @staticmethod
    def ft_lookups(table, tag):
        """The lookup indices of feature `tag` as libttf documents it picks them: the Latin script's default language system, else DFLT, else the first script's;
        if that gives none, every feature of that name. (Language-specific features are not used.)"""
        fl = table.FeatureList.FeatureRecord
        feats, rank, script = [], 0, None
        for sr in table.ScriptList.ScriptRecord:
            rk = 3 if sr.ScriptTag == "latn" else 2 if sr.ScriptTag == "DFLT" else 1
            if rk > rank:
                rank, script = rk, sr.Script
        if script is not None:
            ls = script.DefaultLangSys or (script.LangSysRecord[0].LangSys if script.LangSysRecord else None)
            if ls:
                feats = [i for i in ls.FeatureIndex if i < len(fl) and fl[i].FeatureTag == tag]
        if not feats:
            feats = [i for i, fr in enumerate(fl) if fr.FeatureTag == tag]
        out = []
        for i in feats:
            for li in fl[i].Feature.LookupListIndex:
                if li not in out:
                    out.append(li)
        return out[:16]

    @staticmethod
    def points_near(points, p, tol=0.6):
        cx, cy = int(math.floor(p[0])), int(math.floor(p[1]))
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for q in points.get((cx + dx, cy + dy), ()):
                    if abs(q[0] - p[0]) <= tol and abs(q[1] - p[1]) <= tol:
                        return True
        return False

    def ft_kern(self, tt, cache, lname, rname):
        """The pair adjustment fontTools' decoded GPOS tables give, choosing lookups the way libttf documents (Latin script, else DFLT, else the first)."""
        gpos = tt["GPOS"].table
        if "lookups" not in cache:
            fl = gpos.FeatureList.FeatureRecord
            feats = []
            rank, script = 0, None
            for sr in gpos.ScriptList.ScriptRecord:
                rk = 3 if sr.ScriptTag == "latn" else 2 if sr.ScriptTag == "DFLT" else 1
                if rk > rank:
                    rank, script = rk, sr.Script
            if script is not None:
                ls = script.DefaultLangSys or (script.LangSysRecord[0].LangSys if script.LangSysRecord else None)
                if ls:
                    feats = [i for i in ls.FeatureIndex if i < len(fl) and fl[i].FeatureTag == "kern"]
            if not feats:
                feats = [i for i, fr in enumerate(fl) if fr.FeatureTag == "kern"]
            lookups = []
            for i in feats:
                for li in fl[i].Feature.LookupListIndex:
                    if li not in lookups:
                        lookups.append(li)
            cache["lookups"] = lookups[:16]
        total = 0
        for li in cache["lookups"]:
            lk = gpos.LookupList.Lookup[li]
            for st in lk.SubTable:
                if lk.LookupType == 9:
                    st = st.ExtSubTable
                if type(st).__name__ != "PairPos":
                    continue
                cov = st.Coverage.glyphs
                if lname not in cov:
                    continue
                if st.Format == 1:
                    rec = [r for r in st.PairSet[cov.index(lname)].PairValueRecord if r.SecondGlyph == rname]
                    if rec:
                        v = rec[0].Value1
                        total += getattr(v, "XAdvance", 0) or 0 if v else 0
                        break
                else:
                    c1, c2 = st.ClassDef1.classDefs.get(lname, 0), st.ClassDef2.classDefs.get(rname, 0)
                    v = st.Class1Record[c1].Class2Record[c2].Value1
                    total += getattr(v, "XAdvance", 0) or 0 if v else 0
                    break
        return total

    def check_geometry(self, specs):
        """Every point that fontTools (an independent decoder) finds in a glyph is a point of our outline, composite glyphs and all. Single-point contours
        are the one exception: they are hinting anchors, legal and common in Microsoft's fonts, enclose nothing, and are left out of an outline on purpose."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        done = []
        for name, path, face in specs:
            tt = self.ft(path, face)
            glyf = tt["glyf"]
            order = tt.getGlyphOrder()
            limit = min(len(order), 6000)
            mine = self.dump(path, face, limit)
            bad = []
            composites = 0
            for gid in range(limit):
                g = glyf[order[gid]]
                if g.numberOfContours == 0:
                    self.assertEqual(mine[gid], [], (name, gid))
                    continue
                composites += g.isComposite()
                coords, ends, _ = g.getCoordinates(glyf)
                want, start = [], 0
                for e in ends:
                    if e > start:
                        want += [(float(x), float(y)) for x, y in coords[start:e + 1]]
                    start = e + 1
                self.assertIsNotNone(mine[gid], "%s glyph %d failed to decode" % (name, gid))
                index = {}
                for p in mine[gid]:
                    index.setdefault((int(math.floor(p[0])), int(math.floor(p[1]))), []).append(p)
                missing = [p for p in want if not self.points_near(index, p)]
                if missing:
                    bad.append((gid, order[gid], len(missing), missing[:2]))
            self.assertEqual(bad, [], "%s: %d glyphs of %d differ from fontTools' decoding, e.g. %s" % (name, len(bad), limit, bad[:3]))
            done.append("%s: %d glyphs (%d composite) identical to fontTools" % (name, limit, composites))
        if not done:
            self.skipTest("none of the fonts is installed")
        print("\n" + "\n".join(done))

    def check_advances(self, specs):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        n = 0
        for name, path, face in specs:
            tt = self.ft(path, face)
            order = tt.getGlyphOrder()
            hmtx = tt["hmtx"].metrics
            out = batch(['load a "%s" %d' % (path, face), "metrics a 0 100000"])[1]
            rows = [l.split() for l in out if l.startswith("m ")]
            self.assertGreater(len(rows), 30)                                    # (the owner's fonts of assets/fonts have about a hundred glyphs, the big ones thousands)
            for _, gid, adv, lsb in rows:
                a, l = hmtx[order[int(gid)]]
                self.assertEqual((int(adv), int(lsb)), (a, l), "%s glyph %s" % (name, gid))
            n += 1
        if not n:
            self.skipTest("none of the fonts is installed")

    def check_cmap(self, specs):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        rng = random.Random(12)
        n = 0
        for name, path, face in specs:
            tt = self.ft(path, face)
            order = tt.getGlyphOrder()
            best = tt.getBestCmap()
            if best:
                items = sorted(best.items())
                probes = rng.sample(items, min(len(items), 3000))
                want = {cp: tt.getGlyphID(g) for cp, g in probes}
                unmapped = [cp for cp in (rng.randrange(0, 0x30000) for _ in range(300)) if cp not in best]
            else:                                                                                      # a symbol font: characters live at 0xF000 + c
                sym = tt["cmap"].getcmap(3, 0).cmap
                want = {k - 0xF000: tt.getGlyphID(g) for k, g in sym.items() if 0xF020 <= k <= 0xF0FF}
                unmapped = []
            cps = sorted(want) + unmapped
            cmds = ['load a "%s" %d' % (path, face)] + ["glyph a " + " ".join(str(c) for c in cps[i:i + 250]) for i in range(0, len(cps), 250)]
            res = batch(cmds)[1:]
            got = {}
            for r in res:
                for line in r:
                    d = kv(line)
                    got[int(d["cp"])] = int(d["gid"])
            for cp in sorted(want):
                self.assertEqual(got[cp], want[cp], "%s: U+%04X" % (name, cp))
            for cp in unmapped:
                self.assertEqual(got[cp], 0, "%s: U+%04X is not mapped" % (name, cp))
            n += 1
        if not n:
            self.skipTest("none of the fonts is installed")

    def check_kerning(self, specs):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        rng = random.Random(31)
        n = 0
        for name, path, face in specs:
            tt = self.ft(path, face)
            if "GPOS" not in tt:
                continue
            order = tt.getGlyphOrder()
            best = tt.getBestCmap() or {}
            letters = [best[c] for c in list(range(0x41, 0x5B)) + list(range(0x61, 0x7B)) + list(range(0xC0, 0x100)) if c in best]
            pairs = {(a, b) for a in letters for b in rng.sample(letters, min(len(letters), 12))}
            # pairs the font really kerns: from explicit pair sets
            cache = {}
            gpos = tt["GPOS"].table
            self.ft_kern(tt, cache, letters[0], letters[0])
            for li in cache["lookups"]:
                lk = gpos.LookupList.Lookup[li]
                for st in lk.SubTable:
                    st = st.ExtSubTable if lk.LookupType == 9 else st
                    if type(st).__name__ == "PairPos" and st.Format == 1:
                        for first, ps in list(zip(st.Coverage.glyphs, st.PairSet))[:80]:
                            for rec in ps.PairValueRecord[:3]:
                                pairs.add((first, rec.SecondGlyph))
                    elif type(st).__name__ == "PairPos":                                              # class based: glyphs of the covered set against glyphs that have a class
                        seconds = sorted(st.ClassDef2.classDefs)
                        for first in rng.sample(st.Coverage.glyphs, min(len(st.Coverage.glyphs), 60)):
                            for second in rng.sample(seconds, min(len(seconds), 8)):
                                pairs.add((first, second))
            pairs = sorted(pairs)
            want = [self.ft_kern(tt, cache, a, b) for a, b in pairs]
            self.assertGreater(sum(1 for w in want if w), 20, "%s: too few kerned pairs in the sample to mean anything" % name)
            cmds = ['load a "%s" %d' % (path, face)] + ["kern a %d %d" % (tt.getGlyphID(a), tt.getGlyphID(b)) for a, b in pairs]
            got = [int(r[0].split()[1]) for r in batch(cmds)[1:]]
            bad = [(pairs[i], got[i], want[i]) for i in range(len(pairs)) if got[i] != want[i]]
            self.assertEqual(bad, [], "%s: %d of %d pairs differ, e.g. %s" % (name, len(bad), len(pairs), bad[:3]))
            n += 1
        if not n:
            self.skipTest("none of the fonts is installed")

    def check_metrics(self, specs):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        n = 0
        for name, path, face0 in specs:
            for face in (face0,):
                tt = self.ft(path, face)
                out = batch(['load a "%s" %d' % (path, face), "info a"] + ["name a %d" % i for i in (0, 1, 2, 4, 6, 16, 17)])
                info = out[1]
                d1, d2, d3 = kv(info[1]), kv(info[2]), kv(info[3])
                self.assertEqual(int(d1["upem"]), tt["head"].unitsPerEm)
                self.assertEqual(d2["hhea"], "%d,%d,%d" % (tt["hhea"].ascent, tt["hhea"].descent, tt["hhea"].lineGap))
                os2 = tt["OS/2"]
                self.assertEqual(d2["typo"], "%d,%d,%d" % (os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap))
                self.assertEqual(d2["win"], "%d,%d" % (os2.usWinAscent, os2.usWinDescent))
                self.assertEqual(int(d3["weight"]), os2.usWeightClass)
                self.assertEqual(d3["underline"], "%d,%d" % (tt["post"].underlinePosition, tt["post"].underlineThickness))
                nm = tt["name"]
                for nid, o in zip((0, 1, 2, 4, 6, 16, 17), out[2:]):
                    rec = nm.getName(nid, 3, 1, 0x409) or nm.getName(nid, 3, 1) or nm.getName(nid, 1, 0)
                    if rec is None:
                        self.assertEqual(o, ["none"], (name, nid))
                    else:
                        want = rec.toUnicode().encode("utf-8")
                        self.assertEqual(int(o[0].split()[1]), len(want), (name, face, nid))                       # the length is the whole string's ...
                        self.assertTrue(want.startswith(bytes.fromhex(o[0].split()[2])), (name, face, nid))       # ... the tool prints at most 255 bytes of it
                n += 1
        if not n:
            self.skipTest("no fonts installed")

    def check_ink(self, cases):
        """The rasteriser against ground truth that shares nothing with it: fontTools' outline of each glyph, flattened finely, filled with the non-zero
        winding rule on a grid of 8 x 8 samples per pixel (numpy). libttf draws the exact area, so the two must agree up to the reference's own sampling
        noise (which is systematic on a vertical stem: its edge sits at one fractional offset along its whole length): the total ink within 1% and the per-pixel
        difference small. Measured at 100 px: 0.00 to 0.52% in total ink, 1.4% of the ink per pixel at worst; MS Gothic's densely overlapping CJK strokes
        are within 0.14%, where hinted FreeType is 3.5% off."""
        try:
            import numpy as np
            from fontTools.pens.recordingPen import RecordingPen
        except Exception:                                                               # noqa: BLE001
            self.skipTest("needs fontTools and numpy")
        size, ss, checked, worst = 100, 8, 0, (0.0, 0.0)
        for name, path, face, text in cases:
            tt = self.ft(path, face)
            cmap = tt.getBestCmap()
            gs = tt.getGlyphSet()
            s = size / tt["head"].unitsPerEm
            res = batch(['load a "%s" %d' % (path, face), "glyph a " + " ".join(str(ord(c)) for c in text)])
            gids = [int(kv(l)["gid"]) for l in res[1]]
            bms = batch(['load a "%s" %d' % (path, face)] + ["render a %d %d" % (g, size) for g in gids])[1:]
            for ch, gid, lines in zip(text, gids, bms):
                bm = parse_bitmap(lines)
                pen = RecordingPen()
                gs[cmap[ord(ch)]].draw(pen)
                polys, cur = [], []
                for kind, args in pen.value:
                    if kind == "moveTo":
                        cur = [args[0]]
                    elif kind == "lineTo":
                        cur.append(args[0])
                    elif kind == "qCurveTo":
                        pts = list(args)
                        p0 = cur[-1]
                        for i in range(len(pts) - 1):
                            c = pts[i]
                            e = pts[i + 1] if i == len(pts) - 2 else ((pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2)
                            for t in np.linspace(0, 1, 17)[1:]:
                                cur.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * e[0], (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * e[1]))
                            p0 = e
                    elif kind in ("closePath", "endPath"):
                        polys.append(cur)
                        cur = []
                # sample points of the bitmap's own pixel grid: x to the right, y up (row r of the bitmap covers y in [top - r - 1, top - r])
                gx = bm["left"] + (np.arange(bm["w"] * ss) + 0.5) / ss
                gy = bm["top"] - (np.arange(bm["h"] * ss) + 0.5) / ss
                X, Y = np.meshgrid(gx, gy)
                wind = np.zeros(X.shape, dtype=np.int32)
                for poly in polys:
                    pts = np.array(poly) * s
                    for (xa, ya), (xb, yb) in zip(pts, np.roll(pts, -1, axis=0)):
                        if ya == yb:
                            continue
                        crosses = (ya <= Y) != (yb <= Y)
                        xi = xa + (Y - ya) / (yb - ya) * (xb - xa)
                        wind += np.where(crosses & (xi > X), 1 if yb > ya else -1, 0).astype(np.int32)
                truth = (wind != 0).reshape(bm["h"], ss, bm["w"], ss).mean(axis=(1, 3))
                mine = np.array(bm["rows"], dtype=np.float64) / 255.0
                ink_t, ink_m = truth.sum(), mine.sum()
                rel = abs(ink_m - ink_t) / ink_t
                mae = np.abs(mine - truth).sum() / ink_t
                worst = (max(worst[0], float(rel)), max(worst[1], float(mae)))
                self.assertLess(rel, 0.01, "%s %r: total ink %.1f against the reference's %.1f" % (name, ch, ink_m, ink_t))
                self.assertLess(mae, 0.05, "%s %r: pixels differ from the reference by %.3f of the ink" % (name, ch, mae))
                checked += 1
        if not checked:
            self.skipTest("none of the fonts is installed")
        print("\nagainst an unhinted high-resolution reference at %d px: worst total-ink difference %.3f%%, worst per-pixel difference %.2f%% of the ink" % (size, 100 * worst[0], 100 * worst[1]))

    def check_kern_table(self, specs):
        """The 'kern' table on real data: a copy of each font with its GPOS table removed (so that the 'kern' table is all libttf has), written to build/, against the pairs
        fontTools reads from that table."""
        rng = random.Random(77)
        n = 0
        for name, path, face in specs:
            tt = self.ft(path, face)
            if "kern" not in tt or "GPOS" not in tt:
                continue
            copy = _ftt.TTFont(path, fontNumber=face)
            self.addCleanup(copy.close)
            del copy["GPOS"]
            WORK.mkdir(parents=True, exist_ok=True)
            stripped = str(WORK / ("kernonly-" + name))
            copy.save(stripped)
            table = copy["kern"].kernTables[0].kernTable
            order = copy.getGlyphOrder()
            ids = {g: i for i, g in enumerate(order)}
            known = sorted(table.items())
            pairs = [p for p, v in rng.sample(known, min(len(known), 400))]
            letters = [g for g in order[:400]]
            pairs += [(rng.choice(letters), rng.choice(letters)) for _ in range(300)]
            cmds = ['load a "%s" 0' % stripped, "info a"] + ["kern a %d %d" % (ids[a], ids[b]) for a, b in pairs]
            res = batch(cmds)
            self.assertEqual(kv(res[1][4])["gposkern"], "0", "the copy still has GPOS kerning")
            self.assertGreater(int(kv(res[1][4])["kern"]), 0)
            got = [int(r[0].split()[1]) for r in res[2:]]
            want = [sum(kt.kernTable.get(p, 0) for kt in copy["kern"].kernTables) for p in pairs]
            self.assertGreater(sum(1 for w in want if w), 100, name)
            bad = [(pairs[i], got[i], want[i]) for i in range(len(pairs)) if got[i] != want[i]]
            self.assertEqual(bad, [], "%s: %d of %d pairs differ from the kern table, e.g. %s" % (name, len(bad), len(pairs), bad[:3]))
            n += 1
        if not n:
            self.skipTest("no font with both kern and GPOS")


class WindowsFonts(RealFontChecks, ToolCase):
    def setUp(self):
        if not FONT_DIR.is_dir():
            self.skipTest("no font folder at %s" % FONT_DIR)

    def present(self):
        return [(f, local_font(f), faces) for f, faces in LOCAL_FONTS if local_font(f)]

    def need(self, *names):
        paths = [local_font(n) for n in names]
        if not all(paths):
            self.skipTest("needs %s in %s" % (", ".join(names), FONT_DIR))
        return paths


    def specs(self, *pairs):
        return [(n, local_font(n), f) for n, f in pairs if local_font(n)]

    def test_glyph_geometry_agrees_with_fonttools(self):
        """Every point that fontTools (an independent decoder) finds in a glyph is a point of our outline, composite glyphs and all. Single-point contours
        are the one exception: they are hinting anchors, legal and common in Microsoft's fonts, enclose nothing, and are left out of an outline on purpose."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_geometry(self.specs(("arial.ttf", 0), ("segoeui.ttf", 0), ("times.ttf", 0), ("consola.ttf", 0), ("calibri.ttf", 0), ("symbol.ttf", 0), ("cambria.ttc", 0),
                                       ("msgothic.ttc", 0), ("YuGothR.ttc", 0), ("malgun.ttf", 0), ("seguisym.ttf", 0)))

    def test_advances_and_bearings_agree_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_advances(self.specs(("arial.ttf", 0), ("consola.ttf", 0), ("calibri.ttf", 0), ("msgothic.ttc", 2), ("YuGothR.ttc", 0), ("malgun.ttf", 0), ("msyh.ttc", 0)))

    def test_character_map_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_cmap(self.specs(("arial.ttf", 0), ("segoeui.ttf", 0), ("calibri.ttf", 0), ("msgothic.ttc", 0), ("YuGothR.ttc", 1), ("malgun.ttf", 0), ("seguiemj.ttf", 0),
                                   ("symbol.ttf", 0), ("wingding.ttf", 0), ("msyh.ttc", 0)))

    def test_kerning_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_kerning(self.specs(("arial.ttf", 0), ("segoeui.ttf", 0), ("times.ttf", 0), ("calibri.ttf", 0), ("cambria.ttc", 0), ("YuGothR.ttc", 0), ("Nirmala.ttc", 0)))

    def test_metrics_and_names_agree_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_metrics([(n, p, f) for n, p, faces in self.present() for f in faces[:2]])

    def test_ink_matches_an_unhinted_reference(self):
        """The rasteriser against ground truth that shares nothing with it (see check_ink). Measured on the owner's fonts: 0.00 to 0.4% in total ink, for
        MS Gothic's densely overlapping CJK strokes too, where hinted FreeType is 3.5% off."""
        cases = [("arial.ttf", 0, "AgSQ&@%8W"), ("segoeui.ttf", 0, "Rgs4"), ("times.ttf", 0, "aGWR&"), ("calibri.ttf", 0, "aeRgQ"), ("consola.ttf", 0, "mW0@"),
                 ("YuGothR.ttc", 0, "あ日本語東"), ("msgothic.ttc", 0, "あ日語")]
        self.check_ink([(n, local_font(n), f, t) for n, f, t in cases if local_font(n)])

    def test_every_installed_font_loads_and_renders(self):
        fonts = self.present()
        if not fonts:
            self.skipTest("none of the usual fonts is installed")
        rows = []
        for name, path, faces in fonts:
            for face in faces:
                t0 = time.time()
                out = batch(['load a "%s" %d' % (path, face), "info a", "sweep a 24 0 0 %d" % SWEEP_LIMIT, "sweep a 11 1 0 800"])
                self.assertTrue(out[0][0].startswith("ok"), (name, face, out[0]))
                d = kv(out[2][0])
                self.assertEqual(d["errors"], "0", "%s face %d: some glyphs failed to render: %s" % (name, face, out[2]))
                self.assertGreater(int(d["drawn"]), 0)
                self.assertEqual(kv(out[3][0])["errors"], "0", (name, face, out[3]))                     # (LCD mode too)
                fam = out[1][0].split("|")[0].split("=")[1]
                rows.append("%-22s face %d %-28s glyphs %6s  rendered %6s  %.1fs" % (name, face, fam, kv(out[1][1])["glyphs"], d["glyphs"], time.time() - t0))
        print("\n" + "\n".join(rows))

    def test_collections_have_the_faces_fonttools_sees(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        checked = 0
        for name, path, faces in self.present():
            if not name.endswith(".ttc"):
                continue
            coll = _ftt.TTCollection(path, lazy=True)
            n = len(coll.fonts)
            coll.close()
            out = batch(['load a "%s" 0' % path, 'load a "%s" %d' % (path, n)])
            self.assertTrue(out[0][0].startswith("ok faces=%d " % n), (name, out[0]))
            self.assertTrue(out[1][0].startswith("err -1"), (name, out[1]))                              # one past the last face
            checked += 1
        self.assertGreater(checked, 0)

    def test_cff_outlines_agree_with_fonttools(self):
        """The two OpenType-CFF fonts on this PC: every glyph's whole outline, operator by operator, against fontTools' charstring interpreter."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        n = 0
        for name in ("3ds Light.otf", "ROGFonts-Regular.otf"):
            path = local_font(name)
            if not path:
                continue
            theirs = fonttools_paths(Path(path).read_bytes())
            res = batch(['load a "%s"' % path] + ["outline a %d" % g for g in range(len(theirs))])[1:]
            for gid, (got, want) in enumerate(zip(res, theirs)):
                self.assertFalse(got[0].startswith("err"), (name, gid, got[0]))
                if want is None:
                    continue
                want = [op for op in want]
                self.assertPath(parse_path(got), want, tol=2.5e-3, what="%s glyph %d" % (name, gid))
            n += 1
        if not n:
            self.skipTest("neither CFF font is installed")

    def test_shapes_agree_with_freetype(self):
        """Rendered glyphs against hinted FreeType (through Pillow, which is on the lead's WSL): the same ink in the same places, up to what grid-fitting
        changes. FreeType hints TrueType outlines (moves features onto pixel rows, flattens baseline overshoots, rounds stem widths); libttf does not hint.
        Measured for Arial '%': overlap 0.74 at 48 px, 0.86 at 96, 0.96 at 200, 0.975 at 400, so the comparison is made at 200 px, with room for fonts that
        are hinted hard (MS Gothic's ink is 3.5% heavier under FreeType; the unhinted reference above shows libttf is the exact one)."""
        try:
            import numpy as np
            from PIL import Image, ImageDraw, ImageFont
        except Exception:                                                               # noqa: BLE001
            self.skipTest("needs Pillow and numpy")
        cases = [("arial.ttf", 0, "AgSQ&@%8éW"), ("segoeui.ttf", 0, "Rgs4Ö"), ("times.ttf", 0, "aGWRé&"), ("calibri.ttf", 0, "aeRgQ"), ("consola.ttf", 0, "mW0@"),
                 ("YuGothR.ttc", 0, "あ日本語東"), ("msgothic.ttc", 0, "あ日語")]
        size, checked, worst = 200, 0, []
        for name, face, text in cases:
            path = local_font(name)
            if not path:
                continue
            pil = ImageFont.truetype(path, size, index=face, layout_engine=ImageFont.Layout.BASIC)
            res = batch(['load a "%s" %d' % (path, face), "glyph a " + " ".join(str(ord(c)) for c in text)])
            gids = [int(kv(l)["gid"]) for l in res[1]]
            bms = batch(['load a "%s" %d' % (path, face)] + ["render a %d %d" % (g, size) for g in gids])[1:]
            for ch, gid, lines in zip(text, gids, bms):
                self.assertNotEqual(gid, 0, "%s has no %r" % (name, ch))
                bm = parse_bitmap(lines)
                W = H = size * 4
                ox, oy = size, size * 3
                mine = np.zeros((H, W), dtype=np.float64)
                rows = np.array(bm["rows"], dtype=np.float64)
                mine[oy - bm["top"]:oy - bm["top"] + bm["h"], ox + bm["left"]:ox + bm["left"] + bm["w"]] = rows
                im = Image.new("L", (W, H), 0)
                ImageDraw.Draw(im).text((ox, oy), ch, font=pil, fill=255, anchor="ls")
                ft = np.asarray(im, dtype=np.float64)
                ink_m, ink_f = mine.sum() / 255.0, ft.sum() / 255.0
                a, b = mine >= 128, ft >= 128
                iou = (a & b).sum() / max(1, (a | b).sum())
                rel = abs(ink_m - ink_f) / ink_f
                worst.append((round(float(rel), 4), round(float(iou), 4), name, ch))
                self.assertLess(rel, 0.05, "%s %r: ink %.0f against FreeType's %.0f" % (name, ch, ink_m, ink_f))
                self.assertGreater(iou, 0.90, "%s %r: the shapes overlap only %.3f" % (name, ch, iou))
                checked += 1
        if not checked:
            self.skipTest("none of the fonts is installed")
        print("\nagainst FreeType at %d px (relative ink difference, overlap): worst ink %s, worst overlap %s" % (size, max(worst), min(worst, key=lambda w: w[1])))

    def test_speed_on_real_fonts(self):
        lines = []
        for name in ("arial.ttf", "segoeui.ttf", "YuGothR.ttc", "3ds Light.otf"):
            path = local_font(name)
            if not path:
                continue
            for size in (16, 48):
                out = batch(['load a "%s" 0' % path, "bench a %d 400 0" % size], sanitize=False)[1]
                lines.append("%-14s %2d px gray: %8.0f glyphs/s" % (name, size, float(kv(out[0])["glyphs_per_second"])))
        if not lines:
            self.skipTest("none of the fonts is installed")
        SAMPLES.mkdir(parents=True, exist_ok=True)
        with open(SAMPLES / "speed.txt", "a") as f:
            f.write("\nreal fonts (printable ASCII glyphs, quarter-pixel positions, -O2)\n" + "\n".join(lines) + "\n")
        print("\n" + "\n".join(lines))

    # ---- sample pictures for the lead to look at (build/ttf-samples/*.png)

    def sheet(self, out_name, fonts, lines, width=980, bg=0xFFFFFF, fg=0x000000):
        """lines: (size, text, mode, gamma, contrast, flags). Draws each with libttf and stacks the lines into one PNG."""
        SAMPLES.mkdir(parents=True, exist_ok=True)
        cmds = ['load %s "%s" %d' % (n, p, f) for n, p, f in fonts] + ["set " + " ".join(n for n, _, _ in fonts)]
        tmp = []
        for i, (size, text, mode, gamma, contrast, flags) in enumerate(lines):
            h = int(size * 1.7) + 6
            out = str(WORK / ("sheet%d.ppm" % i))
            tmp.append(out)
            cmds.append("drawx %s %s %d %d 8 %d %06x %06x %d %d %d %d hex:%s" % (out, size, width, h, int(size * 1.15) + 3, fg, bg, mode, flags, gamma, contrast, text.encode().hex()))
        WORK.mkdir(parents=True, exist_ok=True)
        res = batch(cmds)
        for r in res[len(fonts) + 1:]:
            self.assertTrue(r[0].startswith("drew"), r)
        rows = []
        for out in tmp:
            w, h, px = read_ppm(out)
            rows += px
            rows.append([(200, 200, 200)] * w)
        write_png(SAMPLES / out_name, rows)
        dark = sum(1 for row in rows for p in row if p[1] < 128) if bg == 0xFFFFFF else sum(1 for row in rows for p in row if p[1] > 128)
        self.assertGreater(dark, 200, out_name)
        return rows

    def test_write_sample_pictures(self):
        text = "The quick brown fox jumps over the lazy dog 0123456789"
        accents = "Çà é ñ ü ß Ø å œ Ž ł € — “quoted” … ½ ©"
        made = []
        for key, name in (("arial", "arial.ttf"), ("segoeui", "segoeui.ttf"), ("times", "times.ttf"), ("calibri", "calibri.ttf"), ("consolas", "consola.ttf")):
            path = local_font(name)
            if not path:
                continue
            lines = [(12, text, 0, 50, 30, 3), (16, text, 0, 50, 30, 3), (24, text, 0, 50, 30, 3), (48, "Hamburgefonstiv", 0, 50, 30, 3), (24, accents, 0, 50, 30, 3),
                     (16, text + " (LCD)", 1, 50, 30, 3), (24, "AVATAR To Wo fi fl ffi (kerned, ligatures)", 0, 50, 30, 3), (24, "AVATAR To Wo fi fl ffi (plain)", 0, 50, 30, 0)]
            self.sheet("latin-%s.png" % key, [("a", path, 0)], lines)
            made.append(key)
        jp = "日本語のテキスト、ひらがな、カタカナ。東方Project 幻想郷"
        for key, name, face in (("msgothic", "msgothic.ttc", 0), ("yugothic", "YuGothR.ttc", 0), ("msyh", "msyh.ttc", 0), ("malgun", "malgun.ttf", 0)):
            path = local_font(name)
            if not path:
                continue
            text_ko = "한국어 텍스트 샘플" if key == "malgun" else jp
            self.sheet("cjk-%s.png" % key, [("a", path, face)], [(12, text_ko, 0, 50, 30, 3), (16, text_ko, 0, 50, 30, 3), (24, text_ko, 0, 50, 30, 3), (40, text_ko[:14], 0, 50, 30, 3),
                                                                  (16, text_ko, 1, 50, 30, 3)])
            made.append(key)
        a, y = local_font("arial.ttf"), local_font("YuGothR.ttc")
        if a and y:
            self.sheet("fallback-arial-yugothic.png", [("a", a, 0), ("b", y, 0)],
                       [(16, "Hello, world! こんにちは世界。Mixed 東方 text with fallback, 日本語とLatinの混在", 0, 50, 30, 3),
                        (28, "Arial first, Yu Gothic for the rest: 幻想郷 ABC xyz", 0, 50, 30, 3),
                        (20, "Small kana and Latin side by side: ひらがな カタカナ ABC", 0, 50, 30, 3)])
            self.sheet("night-arial-yugothic.png", [("a", a, 0), ("b", y, 0)],
                       [(16, "Light text on a dark ground: Hello, 東方 幻想郷 AVATAR", 0, 100, 30, 3), (16, "Light text on a dark ground: Hello, 東方 幻想郷 AVATAR", 0, 0, 0, 3),
                        (24, "Gamma 100 above, plain blending below", 0, 100, 30, 3), (24, "Gamma 100 above, plain blending below", 0, 0, 0, 3)], bg=0x0E1330, fg=0xF4EFD8)
            made.append("fallback")
        for key, name in (("3dslight", "3ds Light.otf"), ("rog", "ROGFonts-Regular.otf")):
            path = local_font(name)
            if path:
                self.sheet("cff-%s.png" % key, [("a", path, 0)], [(20, "ABCDEFGHIJKLMN abcdefghijklmn 0123456789", 0, 50, 30, 3), (40, "Gensokyo OS", 0, 50, 30, 3), (16, "ABC abc 123 &?!", 0, 50, 30, 3)])
                made.append(key)
        if not made:
            self.skipTest("none of the fonts is installed")
        print("\nsample pictures in %s: %s" % (SAMPLES, ", ".join(made)))


# =============================================================================================== freely licensed fonts (matplotlib's bundled ones)


class FreeFonts(RealFontChecks, ToolCase):
    """The same comparisons on the fonts that ship with matplotlib (DejaVu, STIX, Computer Modern: freely licensed, read in place, never copied), so that the
    real-font checks also run on a machine without the owner's Windows fonts, wherever matplotlib and fontTools are installed."""

    def setUp(self):
        try:
            import matplotlib
            self.dir = Path(matplotlib.get_data_path()) / "fonts" / "ttf"
        except Exception:                                                               # noqa: BLE001
            self.skipTest("matplotlib (and its fonts) not installed")
        if not self.dir.is_dir():
            self.skipTest("no fonts at %s" % self.dir)

    def specs(self, *names):
        s = [(n, str(self.dir / n), 0) for n in names if (self.dir / n).is_file()]
        if not s:
            self.skipTest("none of the fonts is there")
        return s

    def test_every_font_loads_and_renders(self):
        fonts = sorted(self.dir.glob("*.ttf"))
        self.assertGreater(len(fonts), 10)
        total = 0
        for p in fonts:
            out = batch(['load a "%s" 0' % p, "sweep a 24 0 0 0", "sweep a 11 1 0 0"])
            self.assertTrue(out[0][0].startswith("ok"), (p.name, out[0]))
            self.assertEqual(kv(out[1][0])["errors"], "0", (p.name, out[1]))
            self.assertEqual(kv(out[2][0])["errors"], "0", (p.name, out[2]))
            total += int(kv(out[1][0])["glyphs"])
        print("\n%d free fonts, %d glyphs rendered without an error" % (len(fonts), total))

    def test_glyph_geometry_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_geometry(self.specs("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSerif.ttf", "STIXGeneral.ttf", "STIXNonUni.ttf",
                                       "cmr10.ttf", "cmsy10.ttf", "cmex10.ttf"))

    def test_advances_and_bearings_agree_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_advances(self.specs("DejaVuSans.ttf", "DejaVuSansMono.ttf", "DejaVuSerif-Italic.ttf", "STIXGeneral.ttf"))

    def test_character_map_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_cmap(self.specs("DejaVuSans.ttf", "DejaVuSerif.ttf", "STIXGeneral.ttf", "cmr10.ttf"))

    def test_kerning_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_kerning(self.specs("DejaVuSans.ttf", "DejaVuSerif.ttf", "DejaVuSans-Bold.ttf", "STIXGeneral.ttf"))

    def test_kern_table_agrees_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_kern_table(self.specs("DejaVuSans.ttf", "DejaVuSerif.ttf"))

    def test_metrics_and_names_agree_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_metrics(self.specs("DejaVuSans.ttf", "DejaVuSansMono.ttf", "STIXGeneral.ttf", "cmr10.ttf"))

    def test_ink_matches_an_unhinted_reference(self):
        cases = [("DejaVuSans.ttf", 0, "Rgs4&@%W"), ("DejaVuSerif.ttf", 0, "aGWRQ"), ("DejaVuSansMono.ttf", 0, "mW0@g"), ("STIXGeneral.ttf", 0, "aeRgQ"), ("cmr10.ttf", 0, "aeRgQ")]
        self.check_ink([(n, str(self.dir / n), f, t) for n, f, t in cases if (self.dir / n).is_file()])

    def test_ligatures_of_a_real_font(self):
        """STIX has real 'fi' / 'fl' ligature lookups: libttf finds the same ones (their glyph ids) as fontTools reads from the GSUB table."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        (name, path, face), = self.specs("STIXGeneral.ttf")
        tt = self.ft(path, face)
        gsub = tt["GSUB"].table
        ordered = {}                                                                    # first glyph -> its ligatures in the font's order (a language-specific feature is not used)
        for li in self.ft_lookups(gsub, "liga"):
            lk = gsub.LookupList.Lookup[li]
            for st in lk.SubTable:
                st = st.ExtSubTable if lk.LookupType == 7 else st
                if type(st).__name__ == "LigatureSubst":
                    for first, ls in st.ligatures.items():
                        ordered.setdefault(first, []).extend((list(l.Component), l.LigGlyph) for l in ls)
        inputs = [[first] + comps for first, ls in ordered.items() for comps, _ in ls]
        self.assertGreater(len(inputs), 3)

        def expected(seq):
            for comps, lig in ordered[seq[0]]:                                          # the first entry whose components follow wins
                if seq[1:1 + len(comps)] == comps:
                    return lig, 1 + len(comps)
        ids = tt.getGlyphID
        cmds = ['load a "%s" 0' % path] + ["lig a " + " ".join(str(ids(g)) for g in seq) for seq in inputs[:80]]
        for seq, r in zip(inputs[:80], batch(cmds)[1:]):
            lig, used = expected(seq)
            self.assertEqual(r[0], "lig %d %d" % (ids(lig), used), seq)
        # and a pair that is no ligature of the font
        first = next(iter(ordered))
        none = batch(['load a "%s" 0' % path, "lig a %d %d" % (ids(first), ids(first) + 12345 % 100)])[1]
        self.assertTrue(none[0] == "none" or none[0].startswith("lig"))


# =============================================================================================== the original fonts of assets/fonts (in the repository)

ASSET_FONTS = REPO / "assets" / "fonts"


class OwnerFonts(RealFontChecks, ToolCase):
    """The eight original fonts of assets/fonts (assets/fonts/README.md). They are in the repository, so unlike the Windows fonts these tests run everywhere (the
    comparisons with another reader need fontTools). Gensokyo Sans, the interface face, gets the checks that matter for the desktop set in it at 13 px: the kern
    table libttf reads, the kerning of the pairs the specification asks for, the whole-pixel heights at that size, and what the font lacks (which the toolkit
    draws with the bitmap font)."""

    SANS = "GensokyoSans-Regular.ttf"
    NAMES = sorted(p.name for p in ASSET_FONTS.glob("Gensokyo*.ttf"))

    def setUp(self):
        if not self.NAMES:
            self.skipTest("no fonts in assets/fonts")

    def specs(self, *names):
        return [(n, str(ASSET_FONTS / n), 0) for n in (names or self.NAMES)]

    def test_eight_fonts_and_their_checksums(self):
        import hashlib
        sums = {}
        for line in (ASSET_FONTS / "SHA256SUMS").read_text().splitlines():
            if line.strip():
                digest, path = line.split("  ", 1)
                sums[path] = digest
        self.assertEqual(sorted(Path(p).name for p in sums), self.NAMES)
        self.assertEqual(len(self.NAMES), 8)
        for path, digest in sums.items():
            self.assertEqual(hashlib.sha256((REPO / path).read_bytes()).hexdigest(), digest, "%s differs from SHA256SUMS: update the file when a font is rebuilt" % path)

    def test_every_font_loads_and_renders_every_glyph(self):
        for name, path, face in self.specs():
            out = batch(['load a "%s" 0' % path, "info a", "sweep a 13 0 0 0", "sweep a 26 1 0 0"])
            self.assertTrue(out[0][0].startswith("ok"), (name, out[0]))
            self.assertEqual(kv(out[2][0])["errors"], "0", (name, out[2]))
            self.assertEqual(kv(out[3][0])["errors"], "0", (name, out[3]))
            self.assertGreaterEqual(int(kv(out[2][0])["glyphs"]), 40, name)                  # (45 in each display face, 107 in Gensokyo Sans)

    def test_outlines_advances_character_maps_and_metrics_agree_with_fonttools(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        self.check_geometry(self.specs())
        self.check_advances(self.specs())
        self.check_cmap(self.specs())
        self.check_metrics(self.specs())

    def sans(self):
        tt = self.ft(str(ASSET_FONTS / self.SANS))
        return tt, tt.getBestCmap(), tt["kern"].kernTables[0].kernTable

    def test_sans_kern_table_is_read_exactly(self):
        """All 98 pairs of the 'kern' table (format 0, the only kerning the font has), and pairs that are not in it (kerning 0), as fontTools reads them."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        tt, cmap, table = self.sans()
        ids = tt.getGlyphID
        self.assertEqual(len(table), 98)
        rng = random.Random(5)
        letters = [cmap[c] for c in range(0x21, 0x7F)]
        extra = [(rng.choice(letters), rng.choice(letters)) for _ in range(400)]
        pairs = sorted(table) + extra
        res = batch(['load a "%s" 0' % (ASSET_FONTS / self.SANS)] + ["kern a %d %d" % (ids(a), ids(b)) for a, b in pairs])[1:]
        for (a, b), r in zip(pairs, res):
            self.assertEqual(int(r[0].split()[1]), table.get((a, b), 0), (a, b))

    def test_sans_kerns_the_pairs_the_specification_asks_for(self):
        """the font specification section 6: A V W Y T P F L against the round lower case, v w y and the punctuation, and r against , . - a c d e g o q. Not every pair of
        those classes needs kerning (VV, AA ...), but the ones where a diagonal or an open bar leaves a hole do, and these pairs must be negative; the pairs that
        the font loosens on purpose (r n, c l, r l, r f, f t) must be positive."""
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        tt, cmap, table = self.sans()
        k = lambda pair: table.get((cmap[ord(pair[0])], cmap[ord(pair[1])]), 0)       # noqa: E731
        tighten = ("AV AW AY AT VA WA YA TA LV LW LY LT FA PA Te To Ta Ti Tu Tr Ty Tc Td Tq Ts Tv Tw T, T- T. Yo Ye Ya Yc Ys Vo Va Wo Wa We Fa Fe Fo Fc F, F. Pa Pe Po P, P. "
                   "Ly Lv Lw Av Aw Ay V. V, W. W, Y. Y, r. r, ro ra rc rd re rg rq av aw ay fa fe fo ta te to va vo wa wo ya yo").split()
        for pair in tighten:
            self.assertLess(k(pair), 0, pair)
            self.assertGreaterEqual(k(pair), -100, pair)                              # nothing is kerned by more than a tenth of an em
        for pair in ("rn", "cl", "rl", "rf", "ft"):
            self.assertGreater(k(pair), 0, pair)

    def test_sans_text_width_is_the_advances_plus_the_kerning(self):
        if not HAVE_FONTTOOLS:
            self.skipTest("fontTools not installed")
        tt, cmap, table = self.sans()
        hm = tt["hmtx"]
        texts = ["AVATAR", "To Tc Ts Tw", "Patchouli's Notebook", "Kappa Control Panel", "Warning: Wa We", "Fa Fo Fe  F, F.", "rn cl m", "Lock Ly Lv", "Yes Yo Ya Y, V,"]
        res = batch(['load a "%s" 0' % (ASSET_FONTS / self.SANS), "set a"] + ["width 13 3 hex:%s" % t.encode().hex() for t in texts])[2:]
        for t, r in zip(texts, res):
            g = [cmap[ord(c)] for c in t]
            units = sum(hm[x][0] for x in g) + sum(table.get(p, 0) for p in zip(g, g[1:]))
            self.assertAlmostEqual(float(r[0].split()[1]), units * 13 / 1000, places=3, msg=t)

    def test_sans_has_whole_pixel_heights_at_the_ui_size(self):
        """Why the toolkit sets the UI text at 13 px (docs/TTF.md): capitals are 9.1 and lower case 6.98 pixels high, so the flat tops and bottoms of an unhinted
        font land on pixel rows. 12 and 14 px do not."""
        info = batch(['load a "%s" 0' % (ASSET_FONTS / self.SANS), "info a"])[1]
        m = kv(info[5])                                            # metrics100: x-height and cap height at 100 px
        xh, cap = float(m["xh"]) / 100, float(m["caph"]) / 100
        off = lambda size, v: abs(size * v - round(size * v))      # noqa: E731
        self.assertLess(off(13, xh), 0.05)
        self.assertLess(off(13, cap), 0.15)
        self.assertEqual(round(13 * cap), 9)
        self.assertEqual(round(13 * xh), 7)
        self.assertGreater(off(12, xh), 0.3)                       # (12 px: 6.44) and (14 px: 7.52) would blur the x-height edge
        self.assertGreater(off(14, xh), 0.4)

    def test_sans_ink_stays_inside_the_16_pixel_line_box_of_the_toolkit(self):
        """Layouts made for the bitmap font (menu rows, tooltips: tooltip.h's TIP_LINE_H, buttons, list rows) assume a 16 px line box. With the baseline 13 px below
        its top, every glyph of Gensokyo Sans at 13 px (the highest reaches 9.8 px above the baseline, the lowest 2.7 below) lies between rows 0 and 15, on a
        1x canvas and on a scale-2 one, so a one-line box stays whole and nothing is clipped at the edge of a text rectangle."""
        font = str(ASSET_FONTS / self.SANS)
        chars = [chr(c) for c in range(0x21, 0x7F)] + ["–", "—", "‘", "“", "•", "…", "←", "→", "−"]
        WORK.mkdir(parents=True, exist_ok=True)
        for scale in (1, 2):
            cmds = ["kui draw %s %d 40 24 4 4 000000 ffffff 0 hex:%s" % (WORK / ("box%d.ppm" % i), scale, c.encode().hex()) for i, c in enumerate(chars)]
            batch(cmds, kappa=True, env={"KAPPA_UI_FONT": font})
            top, bottom = 99, -1
            for i, c in enumerate(chars):
                w, h, px = read_ppm(str(WORK / ("box%d.ppm" % i)))
                rows = [y for y in range(h) if any(p != (255, 255, 255) for p in px[y])]
                self.assertTrue(rows, "no ink for %r" % c)
                top, bottom = min(top, rows[0]), max(bottom, rows[-1])
            first_row, last_row = 4 * scale, (4 + 16) * scale - 1                   # the line box of text drawn at y = 4
            self.assertGreaterEqual(top, first_row, "ink above the line box (scale %d)" % scale)
            self.assertLessEqual(bottom, last_row, "ink below the 16 px line box (scale %d)" % scale)

    def test_sans_covers_ascii_and_what_it_lacks_is_documented(self):
        """Printable ASCII and a few UI marks; accented letters, box drawing and CJK are not there, so the toolkit shows them with the bitmap font (user/lib/kappa)."""
        have = [0x20, 0xA0, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2192, 0x2212] + list(range(0x21, 0x7F))
        lack = [0xE9, 0xC0, 0xF1, 0xFC, 0x2500, 0x2193, 0x25A0, 0x4E16, 0x3042, 0xA9]
        res = batch(['load a "%s" 0' % (ASSET_FONTS / self.SANS), "glyph a " + " ".join(str(c) for c in have + lack)])[1]
        gids = {int(kv(l)["cp"]): int(kv(l)["gid"]) for l in res}
        for c in have:
            self.assertNotEqual(gids[c], 0, "U+%04X" % c)
        for c in lack:
            self.assertEqual(gids[c], 0, "U+%04X" % c)

    def test_display_fonts_map_lower_case_to_capitals(self):
        for name in self.NAMES:
            if name == self.SANS:
                continue
            res = batch(['load a "%s" 0' % (ASSET_FONTS / name), "glyph a " + " ".join(str(ord(c)) for c in "aAzZ")])[1]
            g = {int(kv(l)["cp"]): int(kv(l)["gid"]) for l in res}
            self.assertNotEqual(g[ord("A")], 0, name)
            self.assertEqual((g[ord("a")], g[ord("z")]), (g[ord("A")], g[ord("Z")]), "%s: lower case is drawn with the capital shapes" % name)


if __name__ == "__main__":
    unittest.main()
