#!/usr/bin/env python3
"""Host tests for the browser application's glue (user/bin/browser): the font provider, the painter, addresses, the loader and the
whole core (navigation, history, forms, selection, find). They run through tests/host/browsertool.c, built with gcc, AddressSanitizer
and UBSan, against the real engine (user/lib/web), the real rasteriser (user/lib/ttf), the real picture decoders (user/lib/img) and
the real network libraries (user/lib/http, user/lib/tls) talking to servers written here. Nothing needs QEMU; tests/case_browser.py
is the guest-side half.

    python tools/hosttest.py discover -s tests/host -p "test_browser*.py" -v           (on Linux: python3 -m unittest ...)

`python tools/hosttest.py tests.host.test_browser.Samples` also writes the sample pages as pictures to build/browser-pages/ to look at.
"""
import concurrent.futures
import gzip
import os
import re
import shutil
import socket
import socketserver
import ssl
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
APP = REPO / "user" / "bin" / "browser"
LIB = REPO / "user" / "lib"
FIXTURES = REPO / "tests" / "browser"
PAGES = REPO / "tests" / "web" / "pages"
FONTS = REPO / "assets" / "fonts"
sys.path.insert(0, str(HERE))


def in_wsl():
    try:
        return "microsoft" in os.uname().release.lower()
    except AttributeError:
        return False


OUT = REPO / "build" / ("host-browser-wsl" if in_wsl() else "host-browser")
SHOTS = REPO / "build" / "browser-pages"
FIX_OUT = REPO / "build" / "browser-fixtures"


def find_cc():
    return shutil.which("gcc") or shutil.which("clang")


# every source of the libraries the glue stands on: compiled once per change into objects (other agents' code: no -Werror)
LIBS = ("web", "ttf", "img", "z", "http", "tls", "sshcrypto")
GLUE = ["fonts.c", "paint.c", "urls.c", "loader.c", "errpage.c", "core.c", "input.c", "forms.c"]


def _compile(cc, flags, src, obj):
    r = subprocess.run([cc, *flags, "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    return None if r.returncode == 0 else "build of %s failed:\n%s%s" % (src.name, r.stdout, r.stderr)


def build(name="browsertool", sanitize=True):
    """Builds browsertool and returns its path. Objects are kept in build/ and rebuilt when their source (or any header of the
    same library or of the browser) is newer."""
    OUT.mkdir(parents=True, exist_ok=True)
    cc = find_cc()
    tag = "" if sanitize else "-fast"
    base = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize else ["-O2", "-g", "-DNDEBUG"]
    inc = []
    for d in [LIB / "web" / "include", LIB / "ttf" / "include", LIB / "img" / "include", LIB / "z" / "include", LIB / "http" / "include",
              LIB / "tls" / "include", LIB / "tls" / "src", LIB / "sshcrypto" / "include", REPO / "shared", APP]:
        inc += ["-I", str(d)]
    jobs = []
    headers = {}
    for lib in LIBS:
        headers[lib] = list((LIB / lib).rglob("*.h"))
        for src in sorted((LIB / lib / "src").glob("*.c")):
            if lib == "sshcrypto" and src.stem not in ("curve25519", "sha512", "chachapoly"):     # libtls needs only Ed25519 from it; the rest wants the OS's libc headers
                continue
            flags = ["-std=gnu17", "-w"] + (["-fno-sanitize=shift-base"] if lib == "sshcrypto" and sanitize else [])    # curve25519's carries shift negative values
            jobs.append((src, OUT / ("lib-%s-%s%s.o" % (lib, src.stem, tag)), flags, headers[lib]))
    app_headers = list(APP.glob("*.h")) + headers["web"] + headers["ttf"] + headers["http"] + headers["tls"]
    strict = ["-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers", "-Wno-format-truncation"]
    srcs = [APP / g for g in GLUE] + [REPO / "shared" / "font8x16.c", HERE / "browsertool.c"]
    for src in srcs:
        stem = "%s-%s" % (src.parent.name, src.stem)
        jobs.append((src, OUT / ("%s%s.o" % (stem, tag)), strict if src.parent == APP or src.name == "browsertool.c" else ["-std=gnu17", "-w"], app_headers))
    todo = []
    for src, obj, flags, hdrs in jobs:
        t = max([src.stat().st_mtime] + [h.stat().st_mtime for h in hdrs])
        if not obj.exists() or obj.stat().st_mtime < t:
            todo.append((src, obj, flags))
    if todo:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) // 2)) as ex:
            errors = [e for e in ex.map(lambda j: _compile(cc, [*base, *j[2], *inc], j[0], j[1]), todo) if e]
        if errors:
            raise RuntimeError("\n".join(errors))
    exe = OUT / (name + tag)
    objs = [o for _, o, _, _ in jobs]
    if not exe.exists() or any(o.stat().st_mtime > exe.stat().st_mtime for o in objs):
        r = subprocess.run([cc, *base, *map(str, objs), "-lm", "-lpthread", "-o", str(exe)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("link of %s failed:\n%s%s" % (name, r.stdout, r.stderr))
    return exe


def env(**extra):
    e = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:allocator_may_return_null=1", BROWSER_FONT_DIR=str(FONTS))
    e.pop("SSL_CERT_FILE", None)
    e.update(extra)
    return e


def tool(*args, timeout=300, check=True, **extra_env):
    exe = build()
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=timeout, env=env(**extra_env))
    if check and r.returncode != 0:
        raise RuntimeError("browsertool %s failed (%d):\n%s%s" % (" ".join(map(str, args)), r.returncode, r.stdout, r.stderr[-3000:]))
    return r


def kv(line):
    out = {}
    for m in re.finditer(r"(\w+)=(\S+)", line):
        out[m.group(1)] = m.group(2)
    return out


def render(page, name, w=800, h=600, scale=1, scroll=0):
    """Renders a page file to build/browser-pages/NAME.png and returns the tool's key=value line as a dict."""
    SHOTS.mkdir(parents=True, exist_ok=True)
    r = tool("render", page, SHOTS / (name + ".png"), w, h, scale, scroll)
    return kv(r.stdout)


def make_png(w, h, rgb):
    """A solid-colour PNG (written with zlib, so no picture library is needed)."""
    import struct
    import zlib

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


RED_PNG = make_png(64, 48, (200, 16, 46))
BLUE_PNG = make_png(200, 100, (30, 60, 200))


def fixtures():
    """tests/browser/ copied to build/browser-fixtures/ with the pictures the pages name (generated, never committed)."""
    FIX_OUT.mkdir(parents=True, exist_ok=True)
    for f in FIXTURES.iterdir():
        shutil.copyfile(f, FIX_OUT / f.name)
    (FIX_OUT / "red.png").write_bytes(RED_PNG)
    (FIX_OUT / "blue.png").write_bytes(BLUE_PNG)
    return FIX_OUT


def session(script, timeout=300, **extra_env):
    """Runs a browsing session (browsertool session) and returns its answer lines. @REPO@ in the script is the repository's path,
    @OUT@ the folder for pictures, @FIX@ the fixture pages."""
    SHOTS.mkdir(parents=True, exist_ok=True)
    text = script.replace("@REPO@", str(REPO)).replace("@OUT@", str(SHOTS)).replace("@FIX@", str(fixtures()))
    exe = build()
    r = subprocess.run([str(exe), "session"], input=text, capture_output=True, text=True, timeout=timeout, env=env(**extra_env))
    if r.returncode != 0 or "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        raise RuntimeError("browsertool session failed (%d):\n%s\n%s" % (r.returncode, r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout.splitlines()


def answers(lines, cmd):
    """The answer lines of one kind of command, as the text after the command word."""
    return [l[len(cmd) + 1:] for l in lines if l.startswith(cmd + " ") or l == cmd]


def last(lines, cmd):
    a = answers(lines, cmd)
    return a[-1] if a else ""


def page_session(html, script, name="tmp", w=200, h=120, scale=1, files=None):
    """Writes html as a page of the fixture folder, opens it in a w x h window and runs the script after it loaded."""
    d = fixtures()
    (d / (name + ".html")).write_text(html, encoding="utf-8")
    for fn, data in (files or {}).items():
        (d / fn).write_bytes(data)
    return session("size %d %d %d\ngo @FIX@/%s.html\nwait 10\n%s" % (w, h, scale, name, script))


# ================================================================================================================ the samples

class Samples(unittest.TestCase):
    """The engine's own sample pages through the font provider and the painter: they must paint something, and the pictures are
    left in build/browser-pages/ for a person to look at."""

    def test_sample_pages_paint(self):
        for page in sorted(PAGES.glob("*.html")):
            res = render(page, page.stem)
            self.assertEqual(res["rc"], "0", page.name)
            self.assertGreater(int(res["items"]), 10, page.name)
            self.assertEqual(res["files"], "1" if (FONTS / "GensokyoSans-Regular.ttf").exists() else "0")

    def test_hidpi_pages_paint_at_twice_the_size(self):
        res = render(PAGES / "article.html", "article-2x", 400, 300, 2)
        self.assertEqual(res["size"], "800x600")
        self.assertEqual(res["rc"], "0")


# ================================================================================================================ addresses

class Urls(unittest.TestCase):
    def inp(self, text, cwd="/work", home="/home/reimu"):
        return tool("urlinput", cwd, home, text).stdout.strip()

    def test_typed_addresses(self):
        cases = [
            ("", "about:blank"),
            ("about:blank", "about:blank"),
            ("http://example.org/a b", "http://example.org/a%20b"),
            ("https://example.org", "https://example.org"),
            ("example.org", "https://example.org"),
            ("www.example.org/path?q=1", "https://www.example.org/path?q=1"),
            ("localhost", "http://localhost"),
            ("localhost:8080/x", "http://localhost:8080/x"),
            ("10.0.2.2:8000/index.html", "http://10.0.2.2:8000/index.html"),
            ("gensokyo", "http://gensokyo"),
            ("/tmp/page.html", "file:///tmp/page.html"),
            ("/tmp/a b.html", "file:///tmp/a%20b.html"),
            ("./up/x.html", "file:///work/up/x.html"),
            ("../x.html", "file:///x.html"),
            ("~/doc.html", "file:///home/reimu/doc.html"),
            ("file:///etc/hosts", "file:///etc/hosts"),
            ("file:/etc/hosts", "file:///etc/hosts"),
            ("mailto:a@b.c", "mailto:a@b.c"),
            ("javascript:alert(1)", "javascript:alert(1)"),
            ("two words", "FAIL"),
        ]
        for text, want in cases:
            self.assertEqual(self.inp(text), want, text)

    def test_file_urls_and_paths(self):
        self.assertEqual(tool("filepath", "file:///a/b%20c.html?x=1#f").stdout.strip(), "/a/b c.html")
        self.assertEqual(tool("filepath", "file://localhost/a/b").stdout.strip(), "/a/b")
        self.assertEqual(tool("filepath", "file://host.example/a/b").stdout.strip(), "FAIL")
        self.assertEqual(tool("filepath", "http://x/a").stdout.strip(), "FAIL")
        self.assertEqual(tool("fileurl", "/a/./b/../c d/\u00e9.txt").stdout.strip(), "file:///a/c%20d/%C3%A9.txt")

    def test_kinds_and_display(self):
        self.assertEqual(tool("kind", "HTTPS://x").stdout.strip(), "2")
        self.assertEqual(tool("kind", "/a/b").stdout.strip(), "0")
        self.assertEqual(tool("kind", "data:text/plain,hi").stdout.strip(), "5")
        self.assertEqual(tool("display", "https://example.org/", "40").stdout.strip(), "example.org")
        self.assertEqual(tool("display", "https://example.org/a/very/long/path/that/goes/on", "20").stdout.strip(), "example.org/a/ver...")
        self.assertEqual(tool("samedoc", "http://x/a#one", "http://x/a#two").stdout.strip(), "1")
        self.assertEqual(tool("samedoc", "http://x/a?q=1#one", "http://x/a#two").stdout.strip(), "0")


# ================================================================================================================ fonts

def libttf_width(text, size, flags=3):
    """The width libttf itself gives for text in Gensokyo Sans, through tests/host/ttftool.c (the independent half of the comparison)."""
    import test_ttf
    path = str(FONTS / "GensokyoSans-Regular.ttf")
    out = test_ttf.batch(["load a %s 0" % path, "set a", "width %s %d %s" % (size, flags, text)])
    return float(out[2][-1].split()[1])


class Fonts(unittest.TestCase):
    def measure(self, family, weight, style, size, text):
        return kv(tool("measure", family, weight, style, size, text).stdout)

    def test_measure_is_libttfs_own_width(self):
        for text in ("Hello,_world", "AV_To_Wa", "The_quick_brown_fox", "fi_ffl"):
            for size in (11, 16, 24.5):
                shown = text.replace("_", " ")
                m = self.measure("sans-serif", 400, 0, size, shown)
                self.assertEqual(m["kind"], "0", m)
                self.assertAlmostEqual(int(m["width"]) / 64.0, libttf_width("hex:" + shown.encode().hex(), size), delta=1 / 64.0 + 1e-6, msg="%s at %s" % (shown, size))

    def test_families_map_to_faces(self):
        for fam in ("sans-serif", "serif", "Arial", "system-ui", "Georgia", "cursive"):
            self.assertEqual(self.measure(fam, 400, 0, 16, "x")["file"], "GensokyoSans-Regular.ttf", fam)
        m = self.measure("monospace", 400, 0, 16, "abcdefgh")
        self.assertEqual((m["kind"], m["mono"], m["file"]), ("0", "0", "GensokyoMono-Regular.ttf"))      # Gensokyo Mono, since 0.6
        self.assertEqual(int(m["width"]), int(8 * 615 * 16 / 1000 * 64 + 0.5))                          # 8 cells of 0.615 em

    def test_monospace_is_gensokyo_mono_one_cell_to_a_character_at_every_size(self):
        for fam in ("monospace", "Courier New", "Consolas", "ui-monospace", "Gensokyo Mono"):
            for size in (13, 16, 20, 24, 33, 48):
                m = self.measure(fam, 400, 0, size, "0123456789 mmmm iiii")
                self.assertEqual(int(m["width"]), int(round(20 * 615 * size / 1000.0 * 64)), "%s at %s" % (fam, size))
        # at 13 px a cell is 8 pixels, the cell of the 8x16 bitmap font
        self.assertEqual(int(self.measure("monospace", 400, 0, 13, "0123456789")["width"]), int(round(10 * 615 * 13 / 1000.0 * 64)))
        self.assertAlmostEqual(int(self.measure("monospace", 400, 0, 13, "0123456789")["width"]) / 64.0, 80.0, delta=0.1)

    def test_monospace_without_the_mono_file_is_a_whole_number_of_cells_of_the_bitmap_font(self):
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            shutil.copy(FONTS / "GensokyoSans-Regular.ttf", d)
            for size, cell in ((13, 8), (16, 8), (20, 8), (24, 16), (33, 16), (48, 24)):
                m = kv(tool("measure", "Courier New", 400, 0, size, "0123456789", BROWSER_FONT_DIR=d).stdout)
                self.assertEqual(int(m["width"]), 10 * cell * 64, "size %s" % size)
                self.assertEqual((m["kind"], m["mono"]), ("1", "1"))

    def test_bold_is_the_bold_face_and_italic_is_made_up(self):
        """GensokyoSans-Bold.ttf exists since 0.6 (assets/fonts): font-weight bold is that file, not a made-up weight; there is no italic file, so italic is the
        Regular slanted (the width is the Regular's)."""
        reg = self.measure("sans-serif", 400, 0, 16, "Echoes")
        bold = self.measure("sans-serif", 700, 0, 16, "Echoes")
        ital = self.measure("sans-serif", 400, 1, 16, "Echoes")
        self.assertEqual((bold["bold"], bold["italic"], bold["file"]), ("0", "0", "GensokyoSans-Bold.ttf"))
        self.assertEqual((ital["bold"], ital["italic"]), ("0", "1"))
        self.assertGreater(int(bold["width"]), int(reg["width"]))
        self.assertEqual(ital["width"], reg["width"])

    def test_bold_is_made_up_when_the_file_lacks_it(self):
        """With the Regular alone in the font folder (a build without the Bold) the weight is made up: wider, flagged, set in the Regular file."""
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            shutil.copy(FONTS / "GensokyoSans-Regular.ttf", d)
            reg = kv(tool("measure", "sans-serif", 400, 0, 16, "Echoes", BROWSER_FONT_DIR=d).stdout)
            bold = kv(tool("measure", "sans-serif", 700, 0, 16, "Echoes", BROWSER_FONT_DIR=d).stdout)
        self.assertEqual((bold["bold"], bold["italic"], bold["file"]), ("1", "0", "GensokyoSans-Regular.ttf"))
        self.assertGreater(int(bold["width"]), int(reg["width"]))

    def test_metrics_are_sane(self):
        m = self.measure("sans-serif", 400, 0, 16, "x")
        self.assertAlmostEqual(int(m["ascent"]) / 64.0, 16 * 0.94, delta=0.2)         # Gensokyo Sans: ascent 940, descent 260 per 1000 units
        self.assertAlmostEqual(int(m["descent"]) / 64.0, 16 * 0.26, delta=0.2)
        self.assertGreater(int(m["uthick"]), 0)
        self.assertGreater(int(m["space"]), 0)

    def test_missing_characters_come_from_the_bitmap_font(self):
        w_acute = int(self.measure("sans-serif", 400, 0, 16, "\u00e9")["width"])
        self.assertGreater(w_acute, 0)
        self.assertLessEqual(w_acute, 16 * 64)
        self.assertEqual(int(self.measure("sans-serif", 400, 0, 16, "\u6771")["width"]), 16 * 64)      # an East Asian wide character is a whole em

    def test_what_is_painted_is_what_was_measured(self):
        for scale in (1, 2):
            v = kv(tool("text", SHOTS / ("text-%d.png" % scale), 16, scale, "sans-serif", "Reflections on the lake").stdout)
            ink = int(v["ink_x1"]) - int(v["ink_x0"])
            self.assertGreater(ink, int(v["width"]) * 0.9 - 4 * scale)
            self.assertLess(ink, int(v["width"]) + 3 * scale)


# ================================================================================================================ painting

class Painting(unittest.TestCase):
    BODY = "<!doctype html><body style='margin:0'>"

    def px(self, html, points, **kw):
        lines = page_session(html, "\n".join("pixel %d %d" % p for p in points), **kw)
        return [l.split()[3] for l in lines if l.startswith("pixel ")]

    def count(self, html, rect, colour, tol=24, **kw):
        lines = page_session(html, "count %d %d %d %d %s %d" % (rect + (colour, tol)), **kw)
        return int(last(lines, "count"))

    def test_a_rectangle_is_where_it_should_be(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:40px;height:20px;background:#ff0000'></div>"
        self.assertEqual(self.px(h, [(10, 10), (49, 29), (50, 29), (9, 10), (30, 30)]), ["ff0000", "ff0000", "ffffff", "ffffff", "ffffff"])

    def test_solid_borders(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:30px;height:30px;border:4px solid #0000ff'></div>"
        got = self.px(h, [(10, 10), (30, 11), (11, 30), (25, 25), (44, 44), (47, 47), (48, 48)])
        self.assertEqual(got, ["0000ff", "0000ff", "0000ff", "ffffff", "0000ff", "0000ff", "ffffff"])

    def test_the_sides_of_a_border_meet_on_a_diagonal(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:20px;height:20px;border:10px solid;border-color:#ff0000 #00ff00 #0000ff #ffff00'></div>"
        got = self.px(h, [(30, 12), (48, 30), (30, 48), (12, 30)])                       # the middle of the top, right, bottom and left bands
        self.assertEqual(got, ["ff0000", "00ff00", "0000ff", "ffff00"])
        # just inside the top-left corner square: the top band above the diagonal, the left band below it
        self.assertEqual(self.px(h, [(18, 11), (11, 18)]), ["ff0000", "ffff00"])

    def test_rounded_corners(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:40px;height:40px;border-radius:20px;background:#000'></div>"
        got = [int(c, 16) for c in self.px(h, [(10, 10), (30, 30), (30, 10), (30, 11)])]
        self.assertEqual(got[0], 0xFFFFFF)                                                # the corner is outside the circle
        self.assertEqual(got[1], 0)
        self.assertLess(got[2] & 255, 0xA0)                                               # the top of the circle is mostly covered
        self.assertEqual(got[3], 0)

    def test_dashed_and_dotted_borders_leave_gaps(self):
        base = self.BODY + "<div style='position:absolute;left:5px;top:5px;width:150px;height:0;border-top:3px %s #000'></div>"
        solid = self.count(base % "solid", (0, 0, 200, 20), "000000", 10)
        dashed = self.count(base % "dashed", (0, 0, 200, 20), "000000", 10)
        dotted = self.count(base % "dotted", (0, 0, 200, 20), "000000", 10)
        self.assertEqual(solid, 150 * 3)
        self.assertTrue(0.3 * solid < dashed < 0.8 * solid, (solid, dashed))
        self.assertTrue(0.2 * solid < dotted < 0.8 * solid, (solid, dotted))

    def test_outset_borders_have_a_light_side_and_a_dark_side(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:30px;height:30px;border:4px outset #808080'></div>"
        top, bottom = [int(c, 16) & 255 for c in self.px(h, [(25, 11), (25, 47)])]
        self.assertGreater(top, bottom)

    def test_opacity_mixes_with_the_ground(self):
        h = self.BODY + "<div style='position:absolute;left:0;top:0;width:50px;height:50px;background:#000;opacity:0.5'></div>"
        v = int(self.px(h, [(20, 20)])[0], 16)
        for shift in (16, 8, 0):
            self.assertAlmostEqual((v >> shift) & 255, 128, delta=3)

    def test_pictures_are_drawn_copied_shrunk_and_stretched(self):
        files = {"red.png": RED_PNG, "blue.png": BLUE_PNG}
        h = (self.BODY + "<img src='red.png' style='position:absolute;left:0;top:0'>"
             "<img src='blue.png' style='position:absolute;left:70px;top:0;width:100px;height:50px'>"
             "<img src='red.png' style='position:absolute;left:0;top:60px;width:128px;height:96px'>")
        got = self.px(h, [(32, 24), (120, 25), (60, 100)], files=files, h=200)
        self.assertEqual(got[0], "c8102e")                                               # copied
        self.assertEqual(got[1], "1e3cc8")                                               # shrunk with an area average of one colour
        self.assertEqual(got[2], "c8102e")                                               # stretched

    def test_text_is_drawn_in_its_colour(self):
        h = self.BODY + "<p style='margin:4px;color:#0000ff;font-size:20px'>Hello echoes</p>"
        self.assertGreater(self.count(h, (0, 0, 200, 40), "0000ff", 60), 80)

    def test_hidpi_scale_draws_at_device_pixels(self):
        h = self.BODY + "<div style='position:absolute;left:10px;top:10px;width:20px;height:10px;background:#ff0000'></div>"
        lines = page_session(h, "count 0 0 200 120 ff0000 10\npixel 20 20\npixel 19 20", w=100, h=60, scale=2)
        self.assertEqual(last(lines, "count"), str(40 * 20))
        self.assertEqual([l.split()[3] for l in lines if l.startswith("pixel ")], ["ff0000", "ffffff"])

    def test_box_shadow_darkens_the_ground_below(self):
        h = self.BODY + "<div style='position:absolute;left:20px;top:20px;width:60px;height:30px;background:#fff;box-shadow:0 6px 8px #000'></div>"
        v = int(self.px(h, [(50, 56)])[0], 16)
        self.assertLess(v & 255, 0xF0)

    def test_clipping_hides_the_overflow(self):
        h = self.BODY + "<div style='position:absolute;left:0;top:0;width:30px;height:30px;overflow:hidden'><div style='width:100px;height:100px;background:#f00'></div></div>"
        self.assertEqual(self.px(h, [(29, 29), (31, 15), (15, 31)]), ["ff0000", "ffffff", "ffffff"])


class Hostile(unittest.TestCase):
    """Pages that ask for absurd things must neither crash the painter (AddressSanitizer and UBSan are watching) nor make it work for
    hours: sizes a billion pixels wide, borders and radii and shadows to match, a font a mile high, a thousand nested groups."""

    EXTREME = """<!doctype html><body style="margin:0">
<div style="position:absolute;left:-1000000000px;top:0;width:2000000000px;height:20px;border:1px dotted #000;background:#f00"></div>
<div style="position:absolute;left:0;top:30px;width:1000000000px;height:1000000000px;border:100000px dashed red;border-radius:1000000000px;
 box-shadow:0 0 1000000000px 1000000000px #000;opacity:0.000000001"></div>
<p style="font-size:1000000000px">huge font</p>
<p style="font-size:0.0001px;letter-spacing:1000000000px;word-spacing:-1000000000px">tiny</p>
<p style="font-family:monospace;font-size:100000px">mono</p>
<img src="red.png" style="width:1000000000px;height:1000000000px"> <img src="red.png" style="width:0;height:0"> <img src="red.png" width="1" height="100000000">
<input style="width:1000000000px"> <textarea style="width:1000000000px;height:1000000000px">x</textarea> <select multiple style="height:1000000000px"><option>a</option></select>
<input type=range style="width:1000000000px"> <input type=checkbox style="width:1000000000px;height:1000000000px">
<table><tr><td rowspan=65535 colspan=65535>x</td></tr></table>
<div style="overflow:scroll;width:10px;height:10px"><div style="width:1000000000px;height:1000000000px"></div></div>
<div style="border:20px double blue;border-radius:30px 5px 100px 0;width:5px;height:5px"></div>
<div style="border:30px groove #c0c0c0;width:1px;height:1px"></div><div style="border:7px ridge #c0c0c0;border-radius:50%"></div>
</body>"""

    def test_absurd_sizes_and_values(self):
        files = {"red.png": RED_PNG}
        for scale in (1, 2, 3):
            lines = page_session(self.EXTREME, "pixel 5 5\nkey pgdn\nkey end\nshot @OUT@/hostile.png\nfind huge\nselall\ncopy\nclick 10 10\nclick 90 90\nwheel 20 20 -3\ntitle",
                                 name="hostile", w=160, h=100, scale=scale, files=files)
            self.assertTrue(any(l.startswith("pixel ") for l in lines), lines[-5:])
            self.assertTrue(any(l.startswith("title") for l in lines))

    def test_a_thousand_nested_groups(self):
        deep = "<div style='opacity:0.99;overflow:hidden;border-radius:3px;border:1px solid #888;padding:1px'>" * 1000 + "deep" + "</div>" * 1000
        lines = page_session("<!doctype html><body style='margin:0'>" + deep, "pixel 3 3\nshot @OUT@/deep.png\nselall\ncopy\ntitle", name="deep", w=200, h=120)
        self.assertTrue(any(l.startswith("pixel ") for l in lines))

    def test_the_memory_limit_of_a_document_stops_it_growing_and_says_so(self):
        html = "<!doctype html><title>Heavy</title><body>" + "<p>paragraph with some words in it</p>" * 30000
        d = fixtures()
        (d / "heavy.html").write_text(html, encoding="utf-8")
        lines = session("size 300 200\ngo @FIX@/heavy.html\nwait 30\nstatus\ntitle", BROWSER_MEMORY_LIMIT="3000000")
        self.assertIn("needs more memory", " ".join(answers(lines, "status")))
        self.assertEqual(last(lines, "title"), "Heavy")

    def test_a_long_unbroken_word_in_a_text_area_and_a_big_value(self):
        html = "<!doctype html><textarea id=t style='width:150px;height:60px'>%s</textarea><input id=i value='%s' style='width:150px'>" % ("x" * 200000, "y" * 100000)
        lines = page_session(html, "click @t\nkey end\nclick @i\nclick 100 40\nkey home\ntype z\nshot @OUT@/bigvalue.png\ntitle", name="bigvalue", w=300, h=150)
        self.assertTrue(any(l.startswith("title") for l in lines))


# ================================================================================================================ the core, on files

class CoreSession(unittest.TestCase):
    def test_a_page_with_a_stylesheet_and_pictures(self):
        lines = session("size 800 600\ngo @FIX@/index.html\nwait 10\ntitle\nstatus\ncount 0 0 800 600 c8102e 30\ncount 8 98 64 48 c8102e 8")
        self.assertEqual(last(lines, "title"), "Fixture index")
        self.assertEqual(last(lines, "status"), "Done")
        self.assertGreater(int(answers(lines, "count")[0]), 3000)                          # the h1 coloured by style.css, and the red picture
        self.assertEqual(answers(lines, "count")[-1], "3072")                              # the picture is 64 x 48 and exactly red

    def test_links_back_forward_and_history(self):
        lines = session("go @FIX@/index.html\nwait 10\nmove @l2\nstatus\nclick @l2\nwait 5\ntitle\nclick @back\nwait 5\ntitle\nback\nwait 5\ntitle\nback\nwait 5\ntitle\n"
                        "forward\nwait 5\ntitle\nforward\nwait 5\ntitle")
        self.assertTrue(answers(lines, "status")[0].endswith("/second.html"))              # the link under the pointer, in full, in the status line
        self.assertEqual(answers(lines, "title"), ["Second page", "Fixture index", "Second page", "Fixture index", "Second page", "Fixture index"])

    def test_fragments_scroll_without_fetching(self):
        lines = session("size 800 400\ngo @FIX@/index.html\nwait 10\nclick @la\nwait 3\nurl\nscrolly\nclick @lt\nwait 3\nurl\nscrolly\nback\nwait 3\nurl\nscrolly")
        urls, ys = answers(lines, "url"), answers(lines, "scrolly")
        self.assertTrue(urls[0].endswith("index.html#bottom"))
        self.assertGreater(int(ys[0]), 1000)
        self.assertTrue(urls[1].endswith("index.html#top"))
        self.assertEqual(ys[1], "0")
        self.assertTrue(urls[2].endswith("index.html#bottom"))                             # back to the earlier fragment
        self.assertGreater(int(ys[2]), 1000)

    def test_scrolling_by_wheel_and_keys(self):
        lines = session("size 800 400\ngo @FIX@/index.html\nwait 10\nwheel 400 200 -3\nscrolly\nkey pgdn\nscrolly\nkey end\nscrolly\nkey home\nscrolly\nkey down\nscrolly\nkey up\nscrolly")
        ys = [int(v) for v in answers(lines, "scrolly")]
        self.assertEqual(ys[0], 144)                                                       # 3 notches of 48
        self.assertEqual(ys[1], 144 + 360)                                                 # a page is the view less 40
        self.assertGreater(ys[2], 1400)
        self.assertEqual(ys[3:], [0, 48, 0])

    def test_the_scroll_position_comes_back_with_back(self):
        lines = session("size 800 400\ngo @FIX@/index.html\nwait 10\nscroll 500\ngo @FIX@/second.html\nwait 5\nback\nwait 8\nscrolly")
        self.assertEqual(int(answers(lines, "scrolly")[-1]), 500)

    def test_find_in_page(self):
        lines = session("size 800 400\ngo @FIX@/text.html\nwait 10\nfind needle\nfind needle 1\nfind needle 1\nfind needle 1\nfind needle -1\nfind absent\ntext\nfindend\ntext")
        f = answers(lines, "find")
        self.assertEqual([kv(x)["count"] for x in f], ["3", "3", "3", "3", "3", "0"])
        self.assertEqual([kv(x)["index"] for x in f], ["1", "2", "3", "1", "3", "0"])
        self.assertNotEqual(kv(f[2])["scrolly"], "0")                                      # the last match is below the first screen: the page scrolled
        self.assertEqual(answers(lines, "text")[-1], "")

    def test_selecting_and_copying_text(self):
        lines = session("size 800 400\ngo @FIX@/text.html\nwait 10\nselall\ncopy\nfindend\ndown 12 16\nmove 300 16 1\nup 300 16\ncopy\n"
                        "click 40 16 2\ncopy\nclick 40 16 3\ncopy")
        c = answers(lines, "copy")
        self.assertIn("The quick brown fox", c[0])
        self.assertIn("|Third paragraph", c[0])                                           # paragraphs are told apart by a line break
        self.assertTrue(c[1].startswith("ok=1 text=The quick"), c[1])                      # dragged from the start of the first line
        self.assertEqual(c[2], "ok=1 text=quick")                                          # a double click takes a word
        self.assertIn("The quick brown fox jumps over the lazy dog.", c[3])                # a triple click the paragraph

    def test_forms_typing_checkboxes_radios_selects_and_submitting(self):
        lines = session("size 800 600\ngo @FIX@/form.html\nwait 10\nclick @q\ntype Reimu Hakurei\nkey backspace\nvalue q\nkey left\nkey left\ntype X\nvalue q\n"
                        "click @chk\nvalue chk\nclick @chk\nvalue chk\nclick @r2\nvalue r1\nvalue r2\nclick @sel\nkey down\nkey enter\nvalue sel\n"
                        "click @pw\ntype secret\nvalue pw\nclick @ta\nkey end\ntype  and more\nkey enter\ntype next\nvalue ta\nclick @go\nwait 5\nurl")
        v = [l for l in lines if l.startswith("value ")]
        self.assertEqual(v[0], "value q=Reimu Hakure checked=0")
        self.assertEqual(v[1], "value q=Reimu HakuXre checked=0")
        self.assertIn("chk=yes checked=1", v[2])
        self.assertIn("chk=yes checked=0", v[3])
        self.assertIn("r1=one checked=0", v[4])
        self.assertIn("r2=two checked=1", v[5])
        self.assertIn("sel=b", v[6])
        self.assertIn("pw=secret", v[7])
        self.assertIn("ta=first line and more", v[8])
        self.assertIn("search.html?q=Reimu+HakuXre&pw=secret&r=two&s=b&t=first+line+and+more%0D%0Anext", last(lines, "url"))

    def test_tab_walks_the_controls_and_enter_submits(self):
        lines = session("size 800 600\ngo @FIX@/form.html\nwait 10\nkey tab\ntype one\nkey tab\ntype two\nkey tab\nkey space\nvalue chk\nkey tab shift\ntype X\nvalue pw\n"
                        "click @q\ntype hello\nkey enter\nwait 5\nurl")
        self.assertIn("pw=twoX", [l for l in lines if l.startswith("value pw")][0])
        self.assertIn("chk=yes checked=1", [l for l in lines if l.startswith("value chk")][0])
        self.assertIn("search.html?q=onehello", last(lines, "url"))

    def test_the_pointer_cursor_follows_what_is_under_it(self):
        lines = session("go @FIX@/index.html\nwait 10\nmove @l2\ncursor\nmove @intro\nmove 700 300\ncursor")
        c = [int(x) for x in answers(lines, "cursor")]
        self.assertEqual(c[0], 1)                                                          # over the link: a pointing hand
        self.assertEqual(c[1], 0)                                                          # over empty ground: the arrow

    def test_text_files_and_pictures_as_pages(self):
        lines = session("size 400 300\ngo @FIX@/plain.txt\nwait 10\nhtml\nshot @OUT@/c-plain.png\ngo @FIX@/red.png\nwait 10\ncount 0 0 400 300 c8102e 8")
        self.assertIn("Plain text, shown as it is.", last(lines, "html") if "Plain text" in last(lines, "html") else answers(lines, "html")[0])
        self.assertIn("a <tag> & an ampersand stay literal", answers(lines, "html")[0])
        self.assertEqual(int(last(lines, "count")), 64 * 48)

    def test_errors_become_pages_with_the_plain_fact_in_them(self):
        lines = session("size 600 300\ngo /no/such/file.html\nwait 5\ntitle\nhtml\ngo @FIX@/\nwait 5\ntitle\nhtml\ngo two words here\nwait 3\ntitle\ngo about:nothing\nwait 3\ntitle\n"
                        "go about:blank\nwait 3\ntitle\ngo @FIX@/index.html\nwait 8\ngo javascript:alert(1)\nwait 2\ntitle")
        t = answers(lines, "title")
        h = answers(lines, "html")
        self.assertEqual(t[0], "Nothing here but the wind")
        self.assertIn("/no/such/file.html: No such file or directory", h[0])               # the exact sentence of the system
        self.assertTrue(t[1].startswith("Index of "), t[1])                                # a folder is an index
        self.assertIn("index.html", h[1])
        self.assertEqual(t[2], "That is not an address")
        self.assertEqual(t[3], "Nothing here but the wind")
        self.assertEqual(t[4], "about:blank")
        self.assertEqual(t[5], "Kyouko does not speak that language")

    def test_data_urls_noscript_and_a_script_free_page(self):
        import base64
        html = ("<!doctype html><body style='margin:0'><img src='data:image/png;base64,%s' style='position:absolute;left:0;top:0'>"
                "<noscript><p id=ns>no scripts here</p></noscript>" % base64.b64encode(RED_PNG).decode())
        lines = page_session(html, "html\ncount 0 0 200 120 c8102e 8")
        self.assertEqual(int(last(lines, "count")), 64 * 48)
        self.assertIn("no scripts here", last(lines, "html"))

    def test_the_viewport_follows_the_window(self):
        lines = session("size 300 300\ngo @FIX@/index.html\nwait 10\nwhere @box\nsize 800 300\nwhere @box\ncontent")
        w = [int(x.split()[1]) for x in answers(lines, "where")]
        self.assertLess(w[1], w[0])                                                        # wider: the paragraph above has fewer lines, so the box moved up


# ================================================================================================================ the loader against servers

class Server:
    """A small raw HTTP/1.1 server (plain or TLS) whose routes can misbehave on purpose. Routes map a path to a function
    (handler) -> None that writes the whole answer to handler.wfile."""

    def __init__(self, routes, tls_context=None):
        outer = self

        class H(socketserver.StreamRequestHandler):
            def handle(self):
                try:
                    line = self.rfile.readline().decode("latin-1")
                    while True:
                        h = self.rfile.readline()
                        if h in (b"\r\n", b"\n", b""):
                            break
                    parts = line.split()
                    self.path = parts[1] if len(parts) > 1 else "/"
                    outer.requests.append(self.path)
                    fn = outer.routes.get(self.path.split("?")[0])
                    if fn is None:
                        self.reply(404, b"", "text/plain")
                    else:
                        fn(self)
                except (ConnectionError, ssl.SSLError, OSError):
                    pass

            def reply(self, status, body, ctype="text/html; charset=utf-8", extra=(), chunked=False):
                head = "HTTP/1.1 %d X\r\nContent-Type: %s\r\nConnection: close\r\n" % (status, ctype)
                for k, v in extra:
                    head += "%s: %s\r\n" % (k, v)
                if chunked:
                    head += "Transfer-Encoding: chunked\r\n\r\n"
                    self.wfile.write(head.encode())
                    for i in range(0, len(body), 40):
                        piece = body[i:i + 40]
                        self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    head += "Content-Length: %d\r\n\r\n" % len(body)
                    self.wfile.write(head.encode() + body)

        class S(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

            def get_request(self):
                sock, addr = super().get_request()
                if tls_context:
                    sock = tls_context.wrap_socket(sock, server_side=True)
                return sock, addr

        self.routes = routes
        self.requests = []
        self.srv = S(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def url(self, path, scheme="http"):
        return "%s://127.0.0.1:%d%s" % (scheme, self.port, path)

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


def html_page(title, body="", head=""):
    return ("<!doctype html><html><head><meta charset='utf-8'><title>%s</title>%s</head><body>%s</body></html>" % (title, head, body)).encode()


def web_routes():
    def index(h):
        h.reply(200, html_page("Served index", "<h1>Echo</h1><p><a id=l href='/second.html'>next</a></p><img id=pic src='/red.png' style='position:absolute;left:0;top:60px'>",
                               "<link rel=stylesheet href='/style.css'>"))

    def second(h):
        h.reply(200, html_page("Served second", "<p>second</p>"))

    def redir(to, code):
        return lambda h: h.reply(code, b"", extra=[("Location", to)])

    def chunked(h):
        h.reply(200, html_page("Chunked page", "<p>" + "chunk " * 60 + "</p>"), chunked=True)

    def gz(h):
        h.reply(200, gzip.compress(html_page("Gzip page", "<p>compressed</p>" * 20)), extra=[("Content-Encoding", "gzip")])

    def slow(h):
        body = html_page("Slow page", "<p id=a>first part</p>" + "<p>filler</p>" * 30 + "<p id=b>second part</p>")
        h.wfile.write(("HTTP/1.1 200 X\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % len(body)).encode())
        h.wfile.write(body[:300])
        h.wfile.flush()
        time.sleep(3.0)
        h.wfile.write(body[300:])

    def stall(h):
        time.sleep(8)

    def garbage(h):
        h.wfile.write(b"this is not http at all\r\n\r\nreally\r\n")

    def cut(h):
        h.wfile.write(b"HTTP/1.1 200 X\r\nContent-Type: text/html\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n<!doctype html><title>Cut page</title><p>only a few bytes")

    def big(h):
        h.reply(200, b"<!doctype html><title>Big page</title>" + b"<p>x</p>\n" * 100000)

    return {
        "/bigheader": lambda h: h.reply(200, html_page("Big header page", "<p>ok</p>"), extra=[("Content-Security-Policy", "default-src 'self' " + "x" * 6000)]),
        "/hugeheader": lambda h: h.reply(200, html_page("Huge header page", "<p>never seen</p>"), extra=[("X-Huge", "y" * 9000)]),
        "/index.html": index, "/second.html": second,
        "/style.css": lambda h: h.reply(200, b"h1{color:#c8102e} body{margin:8px}", "text/css"),
        "/red.png": lambda h: h.reply(200, RED_PNG, "image/png"),
        "/redir1": redir("/redir2", 301), "/redir2": redir("/second.html", 302), "/loop": redir("/loop", 302),
        "/chunked": chunked, "/gzip": gz, "/slow": slow, "/stall": stall, "/garbage": garbage, "/cut": cut, "/big": big,
        "/plain": lambda h: h.reply(200, b"plain & <simple>\nsecond line\n", "text/plain; charset=utf-8"),
        "/image": lambda h: h.reply(200, RED_PNG, "image/png"),
        "/octet": lambda h: h.reply(200, b"\x00" * 100, "application/octet-stream"),
        "/missing": lambda h: h.reply(404, b""),
        "/missing-body": lambda h: h.reply(404, html_page("Custom 404", "<p>my own not found page</p>")),
        "/latin1": lambda h: h.reply(200, b"<!doctype html><title>Latin one</title><p>caf\xe9</p>", "text/html; charset=iso-8859-1"),
    }


class Loader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = Server(web_routes())

    @classmethod
    def tearDownClass(cls):
        cls.srv.close()

    def go(self, path, extra="", **env_):
        return session("size 400 200\ngo %s\nwait 20\ntitle\nstatus\n%s" % (self.srv.url(path), extra), **env_)

    def test_a_page_with_its_stylesheet_and_picture(self):
        lines = self.go("/index.html", "count 0 0 400 200 c8102e 30\ncount 0 55 70 60 c8102e 8")
        self.assertEqual(last(lines, "title"), "Served index")
        self.assertEqual(last(lines, "status"), "Done")
        self.assertGreater(int(answers(lines, "count")[0]), 3000)
        self.assertIn("/style.css", self.srv.requests)
        self.assertIn("/red.png", self.srv.requests)

    def test_redirects_are_followed_and_links_resolve_against_the_final_address(self):
        lines = self.go("/redir1", "url")
        self.assertEqual(last(lines, "title"), "Served second")

    def test_a_redirect_loop_stops(self):
        lines = self.go("/loop")
        self.assertEqual(last(lines, "title"), "Lost in the bamboo forest")

    def test_chunked_and_gzip_bodies(self):
        self.assertEqual(last(self.go("/chunked"), "title"), "Chunked page")
        lines = self.go("/gzip", "html")
        self.assertEqual(last(lines, "title"), "Gzip page")
        self.assertIn("compressed", last(lines, "html"))

    def test_a_page_is_shown_while_it_arrives(self):
        lines = session("size 400 200\ngo %s\nwait 0.6\nhtml\nstatus\nwait 20\nhtml\ntitle" % self.srv.url("/slow"))
        h = answers(lines, "html")
        self.assertIn("first part", h[0])
        self.assertNotIn("second part", h[0])
        self.assertIn("second part", h[1])
        self.assertTrue(answers(lines, "status")[0].startswith("Loading"))

    def test_plain_text_pictures_and_other_things(self):
        lines = self.go("/plain", "html")
        self.assertIn("plain & <simple>", last(lines, "html"))
        lines = session("size 400 200\ngo %s\nwait 20\ncount 0 0 400 200 c8102e 8" % self.srv.url("/image"))
        self.assertEqual(int(last(lines, "count")), 64 * 48)
        lines = self.go("/octet")
        self.assertEqual(last(lines, "title"), "Kyouko can only echo pages")

    def test_error_statuses(self):
        self.assertEqual(last(self.go("/missing"), "title"), "No such shrine")
        lines = self.go("/missing-body", "html")
        self.assertEqual(last(lines, "title"), "Custom 404")
        self.assertIn("my own not found page", last(lines, "html"))

    def test_a_server_that_misbehaves(self):
        self.assertEqual(last(self.go("/garbage"), "title"), "The echo came back garbled")
        lines = self.go("/cut", "html")                                                      # a body that ends early keeps what arrived
        self.assertEqual(last(lines, "title"), "Cut page")
        self.assertIn("only a few bytes", last(lines, "html"))
        self.assertIn("cut short", last(lines, "status") + " ".join(answers(lines, "status")))

    def test_long_header_fields_as_real_servers_send_them(self):
        # Wikipedia's Content-Security-Policy is 4.5 KiB: libhttp's old limit for a field was 4 KiB (found by loading the real page)
        self.assertEqual(last(self.go("/bigheader"), "title"), "Big header page")
        self.assertEqual(last(self.go("/hugeheader"), "title"), "The echo came back garbled")             # a field longer than a line is still refused

    def test_a_server_that_never_answers_times_out(self):
        start = time.time()
        lines = self.go("/stall", BROWSER_TIMEOUT_MS="1500")
        self.assertEqual(last(lines, "title"), "The mountain is asleep")
        self.assertLess(time.time() - start, 7)

    def test_nothing_listens_there(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        lines = session("go http://127.0.0.1:%d/\nwait 10\ntitle\nhtml" % port)
        self.assertEqual(last(lines, "title"), "The echo never came back")

    def test_the_page_size_limit_cuts_and_says_so(self):
        lines = self.go("/big", "status\nhtml", BROWSER_MAX_DOC_BYTES="65536")
        self.assertEqual(last(lines, "title"), "Big page")
        self.assertIn("longer than", " ".join(answers(lines, "status")))

    def test_the_character_set_of_the_header_is_used(self):
        lines = self.go("/latin1", "html")
        self.assertIn("caf\u00e9", last(lines, "html"))

    def test_stopping_keeps_the_page_that_was_shown(self):
        lines = session("size 400 200\ngo @FIX@/second.html\nwait 5\ngo %s\nstop\nwait 2\ntitle\nurl" % self.srv.url("/stall"), BROWSER_TIMEOUT_MS="1500")
        self.assertEqual(last(lines, "title"), "Second page")


def make_certs(tmp, names, days=30, ca_name="Test CA"):
    """A CA and a server certificate for the IP address 127.0.0.1 (and the given DNS names), written to tmp. Returns (ca.pem, cert.pem, key.pem)."""
    import datetime
    import ipaddress
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_subj = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, ca_name)])
    ca = (x509.CertificateBuilder().subject_name(ca_subj).issuer_name(ca_subj).public_key(ca_key.public_key()).serial_number(1)
          .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=days))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
          .add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True).sign(ca_key, hashes.SHA256()))
    key = ec.generate_private_key(ec.SECP256R1())
    sans = [x509.IPAddress(ipaddress.ip_address(n)) if re.match(r"^[0-9.]+$", n) else x509.DNSName(n) for n in names]
    cert = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "echo.test")])).issuer_name(ca_subj)
            .public_key(key.public_key()).serial_number(2).not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=days))
            .add_extension(x509.SubjectAlternativeName(sans), critical=False).add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False).sign(ca_key, hashes.SHA256()))
    tmp = Path(tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    paths = (tmp / "ca.pem", tmp / "cert.pem", tmp / "key.pem")
    paths[0].write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    paths[1].write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    paths[2].write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return paths


class Https(unittest.TestCase):
    """HTTPS through libtls with certificates checked against a root file: good, unknown authority, wrong name."""

    @classmethod
    def setUpClass(cls):
        try:
            import cryptography  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("python3 cryptography is needed to make test certificates")
        cls.dir = REPO / "build" / "browser-tls"
        cls.good = make_certs(cls.dir / "good", ["127.0.0.1"])
        cls.other = make_certs(cls.dir / "other", ["127.0.0.1"], ca_name="Another CA")
        cls.wrong = make_certs(cls.dir / "wrong", ["192.0.2.7"])
        cls.servers = {}
        for name, paths in (("good", cls.good), ("wrong", cls.wrong)):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(paths[1], paths[2])
            cls.servers[name] = Server(web_routes(), ctx)

    @classmethod
    def tearDownClass(cls):
        for s in getattr(cls, "servers", {}).values():
            s.close()

    def go(self, server, ca, path="/index.html", extra=""):
        return session("size 400 200\ngo %s\nwait 30\ntitle\nhtml\nstatus\n%s" % (self.servers[server].url(path, "https"), extra), SSL_CERT_FILE=str(ca[0]))

    def test_a_verified_page_loads_with_its_subresources(self):
        lines = self.go("good", self.good, extra="count 0 0 400 200 c8102e 30\nlock")
        self.assertEqual(last(lines, "title"), "Served index")
        self.assertGreater(int(answers(lines, "count")[0]), 3000)
        self.assertIn("/style.css", self.servers["good"].requests)

    def test_an_unknown_authority_is_refused_with_the_librarys_sentence(self):
        lines = self.go("good", self.other)
        self.assertEqual(last(lines, "title"), "That voice is not who it claims to be")
        self.assertIn("certificate", last(lines, "html").lower())
        self.assertNotIn("Served index", " ".join(answers(lines, "html")))

    def test_a_certificate_for_another_name_is_refused(self):
        lines = self.go("wrong", self.good)
        self.assertEqual(last(lines, "title"), "That voice is not who it claims to be")
        self.assertIn("127.0.0.1", last(lines, "html"))

    def test_without_a_root_store_nothing_secure_is_shown(self):
        lines = session("go %s\nwait 20\ntitle\nhtml" % self.servers["good"].url("/index.html", "https"), SSL_CERT_FILE="/nonexistent/cacert.pem")
        self.assertEqual(last(lines, "title"), "The shrine keeps no list of trusted voices")
        self.assertIn("/nonexistent/cacert.pem", last(lines, "html"))


class Scratch(unittest.TestCase):
    """For looking around: runs the session script in build/browser-scratch.txt (if there is one) and prints the answers."""

    def test_script(self):
        path = REPO / "build" / "browser-scratch.txt"
        if not path.exists():
            self.skipTest("no build/browser-scratch.txt")
        text = path.read_text(encoding="utf-8")
        extra = dict(l[5:].split("=", 1) for l in text.splitlines() if l.startswith("#env "))      # "#env NAME=value" lines set the tool's environment
        for line in session(text, **extra):
            print(line)


if __name__ == "__main__":
    unittest.main()
