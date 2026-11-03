#!/usr/bin/env python3
"""Host tests for the PC-98 display module (user/lib/pc98/src/pc98gfx.c), through tests/host/pc98gfxtool.c.

The expected values come from models written here from the hardware's description (docs/PC98_GFX.md), deliberately
not from the C code: the planes, the GRCG and the EGC as operations on single dots; the EGC's block copies as what a
copy means (each destination dot takes its source dot through the raster operation) rather than as a shifter; the
uPD7220's figures from their geometry (closed forms, not the chip's incremental error terms); the text layer from the
attribute definitions; the picture from a straightforward per-pixel renderer. Nothing from a real PC-98 is used
(docs/ASSETS.md): the fonts are synthetic or the repository's own, and the one test that reads the owner's local
FONT.ROM skips itself when the file is absent and checks only structure (no glyph data in this file).

    python tools/hosttest.py discover -s tests/host -p "test_pc98gfx.py" -v
"""
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402

HERE = Path(__file__).resolve().parent
PLANES = (0xA8000, 0xB0000, 0xB8000, 0xE0000)          # B R G E
EGC_ON = ["out 7c 80", "out 6a 7", "out 6a 5", "out 6a 6"]
DIRS = [(0, 1), (1, 1), (1, 0), (1, -1), (0, -1), (-1, -1), (-1, 0), (-1, 1)]   # GDC directions: 0 down, 2 right
FONT_ROM_PLACES = [
    os.environ.get("TOUHOU_FONT_ROM", ""),
    "<checkout>/assets/local/(TH01-05)/DOSBox-X/FONT.ROM",
    r"<checkout>\assets\local\(TH01-05)\DOSBox-X\FONT.ROM",
    str(x86ref.REPO / "assets" / "local" / "(TH01-05)" / "DOSBox-X" / "FONT.ROM"),
]


def tool():
    return x86ref.build("pc98gfxtool", [HERE / "pc98gfxtool.c", x86ref.REPO / "shared" / "font8x16.c"])


def bench_tool():
    return x86ref.build("pc98gfxbench", [HERE / "pc98gfxtool.c", x86ref.REPO / "shared" / "font8x16.c"], sanitize=False)


def run(lines, exe=None):
    r = subprocess.run([str(exe or tool())], input="\n".join(lines) + "\n", capture_output=True, text=True, errors="replace",
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        raise RuntimeError("pc98gfxtool failed (%d): %s" % (r.returncode, r.stderr[-3000:]))
    out = r.stdout.splitlines()
    bad = [l for l in out if l.startswith("E ")]
    if bad:
        raise RuntimeError("pc98gfxtool refused: %s" % bad)
    return out


def answers(out, tag):
    return [l[len(tag) + 1:] for l in out if l.startswith(tag + " ")]


def ints(out, tag):
    return [int(a.split()[0], 16) for a in answers(out, tag)]


def state(out):
    z = [l for l in out if l.startswith("Z ")][-1]
    return dict(kv.split("=", 1) for kv in z[2:].split())


def planes_of(out):
    return [bytes(int(x, 16) for x in l.split()[1:]) for l in out if l.startswith("P")]


def setplane(page, plane, off, data, chunk=2048):
    """Raw plane writes, split so that no line is longer than the tool takes."""
    data = bytes(data)
    return ["setplane %x %x %x %s" % (page, plane, off + i, hexs(data[i:i + chunk])) for i in range(0, len(data), chunk)]


def same(test, got, want, msg=""):
    """assertEqual for long data: names the first difference (unittest's own diff of long sequences is quadratic)."""
    if isinstance(want, list) and want and isinstance(want[0], (bytes, bytearray)):
        test.assertEqual(len(got), len(want), msg)
        for k, (g, w) in enumerate(zip(got, want)):
            same(test, g, w, "%s [%d]" % (msg, k))
        return
    if len(got) != len(want):
        test.fail("%s: lengths %d and %d" % (msg, len(got), len(want)))
    bad = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), None)
    if bad is not None:
        test.fail("%s: first difference at %d (%#x): %r, expected %r" % (msg, bad, bad, got[bad], want[bad]))


def dump_planes(page=0, n=0x8000):
    return ["plane %x %x 0 %x" % (page, p, n) for p in range(4)]


def hexs(data):
    return " ".join("%x" % b for b in data)


def grb_rgb(c):
    """The eight fixed colours: GRB code bit 2 green, bit 1 red, bit 0 blue, each fully on or off."""
    return (0xFF0000 if c & 2 else 0) | (0x00FF00 if c & 4 else 0) | (0x0000FF if c & 1 else 0)


def lit(pb):
    """The set dots of a plane image (80 bytes per line, bit 7 = left dot)."""
    s = set()
    for i, b in enumerate(pb):
        if b:
            y, xb = divmod(i, 80)
            for k in range(8):
                if b & (0x80 >> k):
                    s.add((xb * 8 + k, y))
    return s


def word_dots(w):
    """A word as the CPU writes it (low byte = left 8 dots, bit 7 of each byte first) as a list of 16 dots."""
    return [((w >> (8 * (j // 8))) >> (7 - j % 8)) & 1 for j in range(16)]


def dots_word(d):
    w = 0
    for j, bit in enumerate(d):
        if bit:
            w |= 1 << (8 * (j // 8) + 7 - j % 8)
    return w


class Picture:
    def __init__(self, data):
        self.px = struct.unpack("<%dI" % (640 * 400), data)

    def __getitem__(self, xy):
        return self.px[xy[1] * 640 + xy[0]]


def render(lines, phases=None):
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "pic.bin"
        cmd = "render %s" % f if phases is None else "renderp %s %x %x" % (f, phases[0], phases[1])
        out = run(lines + [cmd])
        return Picture(f.read_bytes()), out


# ------------------------------------------------------------------ GDC command helpers

def gdc(cmd_port, cmd, params=()):
    par = {0x62: 0x60, 0xA2: 0xA0}[cmd_port]
    return ["out %x %x" % (cmd_port, cmd)] + ["out %x %x" % (par, p & 0xFF) for p in params]


def g_csrw(x, y, plane=0, pitch=40):
    ead = plane * 0x4000 + y * pitch + (x >> 4)
    return gdc(0xA2, 0x49, [ead & 0xFF, (ead >> 8) & 0xFF, ((ead >> 16) & 3) | ((x & 15) << 4)])


def g_figs(direction, flags=0, dc=0, *more):
    p = [flags | direction, dc & 0xFF, (dc >> 8) & 0x3F]
    for v in more:
        v &= 0x3FFF
        p += [v & 0xFF, v >> 8]
    return gdc(0xA2, 0x4C, p)


FIGD = gdc(0xA2, 0x6C)
G_START = gdc(0xA2, 0x6B)
TEXT_OFF = gdc(0x62, 0x0C)


def octant_of(dx, dy):
    """Directions are numbered every 45 degrees from 'down' towards 'right' (y grows downwards)."""
    return int((math.degrees(math.atan2(dx, dy)) % 360.0) // 45.0) % 8


def octant_geometry(k):
    """The axis-parallel step of octant k and the step across the axis towards its diagonal."""
    a, b = DIRS[k], DIRS[(k + 1) % 8]
    axis = a if 0 in a else b
    diag = b if axis == a else a
    return axis, (diag[0] - axis[0], diag[1] - axis[1])


def line_params(x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    k = octant_of(dx, dy) if (dx or dy) else 0
    di, dd = max(abs(dx), abs(dy)), min(abs(dx), abs(dy))
    return k, di, dd


def line_cmds(x0, y0, x1, y1):
    k, di, dd = line_params(x0, y0, x1, y1)     # the data sheet's parameters, computed as a program would
    return g_csrw(x0, y0) + g_figs(k, 0x08, di, 2 * dd - di, 2 * (dd - di), 2 * dd) + FIGD


def line_model(x0, y0, x1, y1):
    """The dots of a digital line: along the major axis one dot per step, across it the nearest dot to the true line
    (half-way rounds away from the start)."""
    k, di, dd = line_params(x0, y0, x1, y1)
    axis, across = octant_geometry(k)
    pts = []
    for i in range(di + 1):
        m = (2 * i * dd + di) // (2 * di) if di else 0
        pts.append((x0 + i * axis[0] + m * across[0], y0 + i * axis[1] + m * across[1]))
    return pts


def arc_cmds(x0, y0, k, r, dc, dm):
    return g_csrw(x0, y0) + g_figs(k, 0x20, dc, r - 1, 2 * (r - 1), -1, dm) + FIGD


def arc_model(x0, y0, k, r, dc, dm):
    axis, across = octant_geometry(k)
    pts = []
    for i in range(dc + 1):
        m = r - round(math.sqrt(r * r - i * i)) if i <= r else r
        if i >= dm:
            pts.append((x0 + i * axis[0] + m * across[0], y0 + i * axis[1] + m * across[1]))
    return pts


# ------------------------------------------------------------------ text helpers

def cell(col, row, code, attr=0xE1):
    a = 2 * (row * 80 + col)
    return ["w8 %x %x" % (0xA0000 + a, code & 0xFF), "w8 %x %x" % (0xA0001 + a, code >> 8),
            "w8 %x %x" % (0xA2000 + a, attr)]


def synth(code, y):
    return (code + y) & 0xFF                     # the tool's synthetic ANK font: row y of code c


def kanji_synth(jis):
    row, c = jis >> 8, jis & 0xFF
    if row == 0x7E:
        return [0] * 32
    return [(row ^ (c << 1) ^ (0x11 * i)) & 0xFF for i in range(32)]


def attr_bits(glyph_line, attr, y, h=16, blink=True, cursor=False, semi=None):
    """What a text cell shows on line y: the pattern, hidden when secret or blinked off, the underline on the last line,
    the vertical line on the left dot (or simple graphics), reverse, and the cursor inverting its lines."""
    bits = glyph_line if y < 16 else 0
    if semi is not None and attr & 0x10:
        band = y * 4 // h
        bits = (0xF0 if semi >> band & 1 else 0) | (0x0F if semi >> (4 + band) & 1 else 0)
    if not attr & 0x01 or (attr & 0x02 and not blink):
        bits = 0
    if attr & 0x08 and y == h - 1:
        bits = 0xFF
    if attr & 0x10 and semi is None:
        bits |= 0x80
    if attr & 0x04:
        bits ^= 0xFF
    if cursor:
        bits ^= 0xFF
    return bits


# ------------------------------------------------------------------ tests

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Memory(unittest.TestCase):
    def test_planes_pages_text_and_holes(self):
        out = run(["init",
                   "w8 a8000 11", "w8 b0000 22", "w8 b8000 33", "w8 e0000 44",
                   "out a6 1", "w8 a8001 55", "w8 e7fff 66", "r8 a8001", "out a6 0", "r8 a8001", "r8 a8000",
                   "w8 a0000 41", "w8 a2000 e5", "r8 a0000", "r8 a2000", "r8 a3fff",
                   "r8 a4000", "r8 c0000", "r8 e8000", "r8 a5000"]
                  + ["claims %x" % a for a in (0xA0000, 0xA4FFF, 0xA5000, 0xA7FFF, 0xA8000, 0xBFFFF, 0xC0000, 0xE0000,
                                                0xE7FFF, 0xE8000)]
                  + ["plane 0 0 0 2", "plane 0 1 0 1", "plane 0 2 0 1", "plane 0 3 0 1", "plane 1 0 0 2",
                     "plane 1 3 7fff 1"])
        self.assertEqual(ints(out, "M"), [0x55, 0, 0x11, 0x41, 0xE5, 0, 0xFF, 0xFF, 0xFF, 0xFF])
        self.assertEqual(ints(out, "K"), [1, 1, 0, 0, 1, 1, 0, 1, 1, 0])
        self.assertEqual(planes_of(out), [b"\x11\x00", b"\x22", b"\x33", b"\x44", b"\x00\x55", b"\x66"])

    def test_word_accesses(self):
        out = run(["init", "w16 a8010 1234", "r16 a8010", "w16 a8021 abcd", "w16 affff 5678", "w16 a0002 2142",
                   "plane 0 0 10 2", "plane 0 0 21 2", "plane 0 0 7fff 1", "plane 0 1 0 1", "rb a0002 2", "r16 affff"])
        self.assertEqual(ints(out, "M"), [0x1234, 0x5678], "a word across the B/R boundary is two bytes")
        self.assertEqual(planes_of(out), [b"\x34\x12", b"\xcd\xab", b"\x78", b"\x56"])
        self.assertEqual(answers(out, "B"), ["42 21"])

    def test_memory_switches(self):
        out = run(["init", "w8 a3fe2 12", "memsw 1", "out 68 d", "w8 a3fe2 34", "w8 a3fe6 56", "memsw 1", "memsw 2",
                   "out 68 c", "w8 a3fe2 99", "memsw 1", "w8 a3fe4 77", "r8 a3fe4", "setmemsw 8 ab", "memsw 8",
                   "r8 a3ffe", "reset", "memsw 1", "memsw 8", "r8 a3fe4", "memsw 9"])
        self.assertEqual(ints(out, "S"), [0, 0x34, 0x56, 0x34, 0xAB, 0x34, 0xAB, 0xFF])
        self.assertEqual(ints(out, "M"), [0x77, 0xAB, 0xE1])

    def test_power_on_state(self):
        out = run(["init", "state", "text", "rb a0000 2", "rb a2000 2", "colours", "in a8", "in aa", "in ac", "in ae",
                   "in 7c", "in 68", "in 41"])
        st = state(out)
        self.assertEqual((st["gdc0_on"], st["gdc1_on"], st["pitch0"], st["pitch1"]), ("1", "0", "50", "28"))
        self.assertEqual((st["mode1"], st["analog"], st["disp"], st["draw"], st["grcg"]), ("80", "0", "0", "0", "0"))
        self.assertEqual([l for l in out if l.startswith("|")], ["|" + " " * 80] * 25)
        self.assertEqual(answers(out, "B"), ["20 0", "e1 0"])
        self.assertEqual(answers(out, "C")[0].split(), ["%06x" % grb_rgb(i & 7) for i in range(16)])
        self.assertEqual(answers(out, "I"), ["37 1", "26 1", "15 1", "4 1", "0 0", "0 0", "0 0"],
                         "palette readable; 7Ch, 68h write-only; 41h not ours")


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Palette(unittest.TestCase):
    # Digital palette: colour n's GRB code sits in AEh, ACh, AAh, A8h for n = 0..3 (high nibble), 4..7 (low nibble).
    WHERE = {0: (0xAE, 4), 1: (0xAC, 4), 2: (0xAA, 4), 3: (0xA8, 4), 4: (0xAE, 0), 5: (0xAC, 0), 6: (0xAA, 0),
             7: (0xA8, 0)}

    def test_digital_layout(self):
        rnd = random.Random(7)
        for trial in range(4):
            regs = {p: rnd.getrandbits(8) for p in (0xA8, 0xAA, 0xAC, 0xAE)}
            out = run(["init"] + ["out %x %x" % kv for kv in regs.items()] + ["colours", "in a8", "in aa", "in ac", "in ae"])
            want = []
            for n in range(16):
                port, shift = self.WHERE[n & 7]
                want.append("%06x" % grb_rgb((regs[port] >> shift) & 7))
            self.assertEqual(answers(out, "C")[0].split(), want)
            self.assertEqual(ints(out, "I"), [regs[p] for p in (0xA8, 0xAA, 0xAC, 0xAE)])

    def test_analog(self):
        rnd = random.Random(16)
        pal = [(rnd.getrandbits(8), rnd.getrandbits(8), rnd.getrandbits(8)) for _ in range(16)]   # G R B, masked to 4 bits
        lines = ["init", "out 6a 1"]
        for i, (gg, r, b) in enumerate(pal):
            lines += ["out a8 %x" % i, "out aa %x" % gg, "out ac %x" % r, "out ae %x" % b]
        lines += ["colours", "out a8 5", "in a8", "in aa", "in ac", "in ae", "out 6a 0", "colours", "state"]
        out = run(lines)
        c16, c8 = answers(out, "C")
        want = ["%06x" % ((r & 15) * 17 << 16 | (gg & 15) * 17 << 8 | (b & 15) * 17) for gg, r, b in pal]
        self.assertEqual(c16.split(), want)
        self.assertEqual(ints(out, "I"), [5, pal[5][0] & 15, pal[5][1] & 15, pal[5][2] & 15])
        self.assertEqual(c8.split(), ["%06x" % grb_rgb(i & 7) for i in range(16)], "the digital palette is separate")
        self.assertEqual(state(out)["analog"], "0")

    def test_render_uses_the_palette(self):
        # byte i of line 0 shows colour index i (plane p set when bit p of i is set)
        lines = ["init"] + TEXT_OFF + G_START
        for p in range(4):
            lines.append("setplane 0 %x 0 %s" % (p, hexs([0xFF if (i >> p) & 1 else 0 for i in range(16)])))
        pic8, _ = render(lines + ["out ae 70"])                 # colour 0 -> GRB 7, colour 4 -> GRB 0
        for i in range(16):
            self.assertEqual(pic8[8 * i + 3, 0], grb_rgb({0: 7, 4: 0}.get(i & 7, i & 7)), i)
        pal = ["out 6a 1"] + sum((["out a8 %x" % i, "out aa %x" % (i ^ 5), "out ac %x" % (15 - i), "out ae %x" % i]
                                  for i in range(16)), [])
        pic16, _ = render(lines + pal)
        for i in range(16):
            self.assertEqual(pic16[8 * i, 0], (15 - i) * 17 << 16 | (i ^ 5) * 17 << 8 | i * 17, i)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Grcg(unittest.TestCase):
    def test_against_a_dot_model(self):
        rnd = random.Random(98)
        base = 0x100
        vram = {(pg, p): bytearray(rnd.getrandbits(8) for _ in range(64)) for pg in (0, 1) for p in range(4)}
        lines = ["init"] + ["setplane %x %x %x %s" % (pg, p, base, hexs(b)) for (pg, p), b in vram.items()]
        reads = []
        for step in range(400):
            op = rnd.choice(["tdw", "rmw", "tcr", "rmwread", "plain", "tdw16", "rmw16", "tcr16"])
            page = rnd.randrange(2)
            planes_off = rnd.getrandbits(4)
            mode = (0 if op == "plain" else 0x80) | (0x40 if op.startswith("rmw") else 0) | planes_off
            tiles = [rnd.getrandbits(8) for _ in range(4)]
            lines += ["out a6 %x" % page, "out 7c %x" % mode] + ["out 7e %x" % t for t in tiles]
            if rnd.random() < 0.3:                      # a fifth tile write wraps to B again
                tiles[0] = rnd.getrandbits(8)
                lines.append("out 7e %x" % tiles[0])
            width = 2 if op.endswith("16") else 1
            off = rnd.randrange(0, 64 - width + 1)
            ap = rnd.randrange(4)                       # the plane in the address: irrelevant while the GRCG is on
            addr = PLANES[ap] + base + off
            enabled = [p for p in range(4) if not planes_off >> p & 1]
            for k in range(width):
                o = off + k
                if op in ("plain", "tdw", "rmw", "tdw16", "rmw16"):
                    v = rnd.getrandbits(8)
                    if k == 0:
                        data = v
                    else:
                        data |= v << 8
                    if op == "plain":
                        vram[(page, ap)][o] = v
                    for p in (enabled if op != "plain" else []):
                        old = vram[(page, p)][o]
                        if op.startswith("tdw"):
                            new = tiles[p]                                  # the CPU's byte is ignored
                        else:
                            new = sum(((tiles[p] if v >> b & 1 else old) >> b & 1) << b for b in range(8))
                        vram[(page, p)][o] = new
                else:
                    if op == "rmwread":
                        r = vram[(page, ap)][o]
                    else:
                        r = sum((all((vram[(page, p)][o] >> b & 1) == (tiles[p] >> b & 1) for p in enabled)) << b
                                for b in range(8))
                    reads.append(r if width == 1 else None)
                    if width == 2:
                        if k == 0:
                            lo = r
                        else:
                            reads[-2:] = [lo | r << 8]
            if op in ("plain", "tdw", "rmw"):
                lines.append("w8 %x %x" % (addr, data))
            elif op in ("tdw16", "rmw16"):
                lines.append("w16 %x %x" % (addr, data))
            elif op == "tcr16":
                lines.append("r16 %x" % addr)
            else:
                lines.append("r8 %x" % addr)
        lines += ["out 7c 0"] + ["plane %x %x %x 40" % (pg, p, base) for pg in (0, 1) for p in range(4)]
        out = run(lines)
        self.assertEqual(ints(out, "M"), reads)
        same(self, planes_of(out), [bytes(vram[(pg, p)]) for pg in (0, 1) for p in range(4)], "planes")

    def test_tile_pointer_restarts_with_the_mode(self):
        out = run(["init", "out 7c 80", "out 7e 1", "out 7e 2", "out 7c 80", "out 7e 3", "state"])
        st = state(out)
        self.assertEqual((st["tiles"], st["tile_next"]), ("03020000", "1"))


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Egc(unittest.TestCase):
    def test_registers_answer_only_in_egc_mode(self):
        out = run(["init", "out 4a4 1234 2", "state", "out 6a 5", "state", "out 6a 7", "out 6a 5", "out 6a 6",
                   "out 4a4 29f0 2", "out 4a8 5aa5 2", "out 4ae 8 1", "out 4af 1 1", "state", "out 6a 4", "state",
                   "out 6a 7", "out 6a 4", "state", "out 6a 21", "state"])
        st = [state([l]) for l in out if l.startswith("Z ")]
        self.assertEqual(st[0]["egc"].split(",")[2], "0000", "ignored outside EGC mode")
        self.assertEqual(st[1]["egc_mode"], "0", "05h needs the permit (07h) first")
        self.assertEqual(st[2]["egc"].split(","), ["fff0", "00ff", "29f0", "0000", "5aa5", "0000", "0000", "0108"])
        self.assertEqual((st[2]["egc_mode"], st[2]["egc_permit"]), ("1", "0"))
        self.assertEqual(st[3]["egc_mode"], "1", "protected: 04h does nothing")
        self.assertEqual(st[4]["egc_mode"], "0")
        self.assertEqual(st[5]["ff2_unknown"], "1")

    def test_raster_operations(self):
        """Every ROP code on random source (CPU), destination and pattern (loaded by a VRAM read) words, then random
        masks and write-protected planes: each dot is ROP bit (S * 4 + D * 2 + P)."""
        self.check_rops(split=False)

    def test_raster_operations_from_a_byte_wide_bus(self):
        self.check_rops(split=True)

    def check_rops(self, split):
        rnd = random.Random(3)
        lines = ["init"] + EGC_ON + ["out 4a2 ff 2", "out 4ac 0 2", "out 4ae f 2"]
        n = 256 + 64
        D = [[rnd.getrandbits(16) for _ in range(4)] for _ in range(n)]
        P = [[rnd.getrandbits(16) for _ in range(4)] for _ in range(n)]
        for p in range(4):
            lines += setplane(0, p, 0, (b for k in range(n) for b in (D[k][p] & 0xFF, D[k][p] >> 8)))
            lines += setplane(0, p, 0x1000, (b for k in range(n) for b in (P[k][p] & 0xFF, P[k][p] >> 8)))
        want = [bytearray(2 * n) for _ in range(4)]
        for k in range(n):
            rop = k if k < 256 else rnd.getrandbits(8)
            s = rnd.getrandbits(16)
            protect = rnd.getrandbits(4) if k >= 256 else 0
            mask = rnd.getrandbits(16) if k >= 256 else 0xFFFF
            lines += ["out 4a0 %x 2" % (0xFFF0 | protect), "out 4a8 %x 2" % mask,
                      "out 4a4 %x 2" % (0x0D00 | rop)]          # pattern from reads, shifter fed by the CPU, ROP output
            pa, da = 0xA8000 + 0x1000 + 2 * k, 0xA8000 + 2 * k
            if split:
                lines += ["r8 %x" % pa, "r8 %x" % (pa + 1), "w8 %x %x" % (da, s & 0xFF), "w8 %x %x" % (da + 1, s >> 8)]
            else:
                lines += ["r16 %x" % pa, "w16 %x %x" % (da, s)]
            for p in range(4):
                old = word_dots(D[k][p])
                if protect >> p & 1:
                    new = old
                else:
                    sd, pd, md = word_dots(s), word_dots(P[k][p]), word_dots(mask)
                    new = [(rop >> (sd[j] * 4 + old[j] * 2 + pd[j])) & 1 if md[j] else old[j] for j in range(16)]
                w = dots_word(new)
                want[p][2 * k:2 * k + 2] = bytes((w & 0xFF, w >> 8))
        out = run(lines + ["plane 0 %x 0 %x" % (p, 2 * n) for p in range(4)] + ["state"])
        same(self, planes_of(out), [bytes(w) for w in want], "planes")
        self.assertEqual(state(out)["egc_under"], "0")

    def test_pattern_sources_compare_and_plane_reads(self):
        rnd = random.Random(5)
        V = [rnd.getrandbits(16) for _ in range(4)]
        lines = ["init"] + EGC_ON + ["setplane 0 %x 40 %x %x" % (p, V[p] & 0xFF, V[p] >> 8) for p in range(4)]
        # writes with the pattern operand as the write source: foreground, background colour
        lines += ["out 4ae f 2", "out 4a6 a 2", "out 4aa 5 2", "out 4a8 f00f 2", "out 4a2 40ff 2", "out 4a4 1400 2",
                  "w16 a8000 0", "out 4a2 20ff 2", "w16 a8002 0"]
        # compare reads against the foreground colour on chosen planes, then plain reads of each plane
        cmp_cases = [(rnd.getrandbits(4), rnd.getrandbits(4)) for _ in range(12)]
        for colour, sel in cmp_cases:
            lines += ["out 4a6 %x 2" % colour, "out 4a2 %x 2" % (0x4000 | sel), "out 4a4 2000 2", "r16 a8040"]
        for p in range(4):
            lines += ["out 4a2 %x 2" % (p << 8), "out 4a4 0 2", "r16 b8040"]
        out = run(lines + ["plane 0 %x 0 4" % p for p in range(4)])
        mask = word_dots(0xF00F)
        got = planes_of(out)
        for p in range(4):
            fg = [1 if (0xA >> p) & 1 else 0] * 16
            bg = [1 if (0x5 >> p) & 1 else 0] * 16
            w0, w1 = dots_word([fg[j] if mask[j] else 0 for j in range(16)]), dots_word([bg[j] if mask[j] else 0 for j in range(16)])
            self.assertEqual(got[p], bytes((w0 & 0xFF, w0 >> 8, w1 & 0xFF, w1 >> 8)), p)
        reads = ints(out, "M")
        for (colour, sel), r in zip(cmp_cases, reads):
            planes = [word_dots(V[p]) for p in range(4)]
            want = dots_word([int(all(planes[p][j] == (colour >> p & 1) for p in range(4) if sel >> p & 1))
                              for j in range(16)])
            self.assertEqual(r, want, (colour, sel))
        self.assertEqual(reads[len(cmp_cases):], V)

    @staticmethod
    def copy_cmds(src, dst, length, rows, desc=False, extra=True, src_page=0, dst_page=0, split=False):
        """The access sequence a program uses for an EGC block copy (ROP = source, shifter fed by VRAM reads): per
        row, one read of the next source word before each write, plus one extra read up front when the source bit
        address is ahead of the destination's (behind it, when descending). split: every word access arrives as two
        byte accesses, low byte first, the way a CPU with an 8-bit bus delivers it."""
        sx, dx = (src[0] + length - 1, dst[0] + length - 1) if desc else (src[0], dst[0])
        step = -1 if desc else 1
        sb, db = sx % 16, dx % 16
        lines = ["out 4ac %x 2" % (sb | db << 4 | (0x1000 if desc else 0)), "out 4ae %x 2" % (length - 1)]
        last = dx + step * (length - 1)
        n_dst = abs(last // 16 - dx // 16) + 1
        need_extra = (sb < db) if desc else (sb > db)
        rd = (lambda a: ["r8 %x" % a, "r8 %x" % (a + 1)]) if split else (lambda a: ["r16 %x" % a])
        wr = (lambda a: ["w8 %x 0" % a, "w8 %x 0" % (a + 1)]) if split else (lambda a: ["w16 %x 0" % a])
        for r in range(rows):
            su, du = sx // 16, dx // 16
            srow, drow = 0xA8000 + (src[1] + r) * 80, 0xA8000 + (dst[1] + r) * 80
            if need_extra and extra:
                lines += ["out a6 %x" % src_page] + rd(srow + 2 * su)
                su += step
            for k in range(n_dst):
                lines += ["out a6 %x" % src_page] + rd(srow + 2 * su) + ["out a6 %x" % dst_page] + wr(drow + 2 * du)
                su += step
                du += step
        return lines

    @staticmethod
    def copy_model(planes, src, dst, length, rows, src_planes=None):
        """What a copy means: every destination dot takes the original source dot (all four planes)."""
        source = [bytes(p) for p in (src_planes or planes)]
        out = [bytearray(p) for p in planes]
        for p in range(4):
            for r in range(rows):
                for j in range(length):
                    sx, sy, tx, ty = src[0] + j, src[1] + r, dst[0] + j, dst[1] + r
                    bit = (source[p][sy * 80 + sx // 8] >> (7 - sx % 8)) & 1
                    o = ty * 80 + tx // 8
                    out[p][o] = (out[p][o] & ~(0x80 >> (tx % 8))) | (bit << (7 - tx % 8))
        return out

    def run_copies(self, cases, desc=False, split=False):
        rnd = random.Random(len(cases) * 31 + desc + 2 * split)
        planes = [bytearray(rnd.getrandbits(8) for _ in range(0x8000)) for _ in range(4)]
        lines = ["init"] + sum((setplane(0, p, 0, planes[p][:80 * 64]) for p in range(4)), [])
        lines += EGC_ON + ["out 4a0 fff0 2", "out 4a2 ff 2", "out 4a8 ffff 2", "out 4a4 29f0 2"]
        model = [bytearray(p[:80 * 64]) + bytearray(0x8000 - 80 * 64) for p in planes]
        for src, dst, length, rows in cases:
            lines += self.copy_cmds(src, dst, length, rows, desc=desc, split=split)
            model = self.copy_model(model, src, dst, length, rows)
        out = run(lines + ["out 7c 0"] + ["plane 0 %x 0 %x" % (p, 80 * 64) for p in range(4)] + ["state"])
        got = planes_of(out)
        for p in range(4):
            same(self, got[p], bytes(model[p][:80 * 64]), "plane %d" % p)
        st = state(out)
        self.assertEqual((st["egc_under"], st["egc_qn"], st["egc_done"]), ("0", "0", "0"), "every row re-armed the shifter")

    def test_block_copies_ascending(self):
        rnd = random.Random(11)
        cases = []
        for _ in range(40):
            length = rnd.choice([1, 2, 7, 15, 16, 17, 31, 32, 33, rnd.randrange(1, 300)])
            sx, dx = rnd.randrange(0, 640 - length), rnd.randrange(0, 640 - length)
            sy = rnd.randrange(0, 30)
            dy = rnd.randrange(32, 60)
            cases.append(((sx, sy), (dx, dy), length, rnd.randrange(1, 4)))
        for sb in range(16):                    # every pair of bit addresses at least once
            for db in range(16):
                if (sb * 16 + db) % 5 == 0:
                    cases.append(((sb + 32, 2), (db + 80, 40), 40, 1))
        cases.append(((40, 3), (35, 3), 200, 2))        # overlapping, moving left: reads stay ahead of writes
        self.run_copies(cases)

    def test_block_copies_descending_overlap(self):
        rnd = random.Random(12)
        cases = [((sx, 10), (sx + shift, 10), length, 2)
                 for sx, shift, length in [(10, 1, 100), (3, 17, 64), (100, 5, 250), (0, 33, 300), (8, 16, 16)]]
        cases += [((rnd.randrange(0, 300), rnd.randrange(0, 20)), (rnd.randrange(0, 300), rnd.randrange(30, 50)),
                   rnd.randrange(1, 300), 1) for _ in range(20)]
        self.run_copies(cases, desc=True)

    def test_block_copies_from_a_byte_wide_bus(self):
        """The same word-oriented copies with every word access split into its two bytes: identical results, so the
        machine's CPU core (8-bit bus callbacks) can drive the EGC for rows that run left to right."""
        rnd = random.Random(13)
        cases = [((rnd.randrange(0, 300), rnd.randrange(0, 20)), (rnd.randrange(0, 300), rnd.randrange(30, 50)),
                  rnd.randrange(1, 200), rnd.randrange(1, 3)) for _ in range(30)]
        cases += [((sb + 64, 25), (db + 160, 52), 37, 1) for sb in (0, 3, 7, 8, 9, 15) for db in (0, 4, 8, 12, 15)]
        self.run_copies(cases, split=True)

    def test_cpu_fed_rows(self):
        """Rows drawn from CPU data (the shifter fed by writes, ROP = source) with one bit-address setup: each row's
        dots take the CPU's dots in order from the source bit address on. Rows of 19 dots from dot 5 end in the even
        byte of their second word, which matters when the words arrive split into bytes."""
        rnd = random.Random(21)
        rows = 6
        data = [[rnd.getrandbits(16) for _ in range(2)] for _ in range(rows)]
        for split in (False, True):
            lines = ["init"] + EGC_ON + ["out 4a4 cf0 2", "out 4ac 52 2", "out 4ae 12 2"]   # dst bit 5, src bit 2, 19 dots
            for r in range(rows):
                for k, w in enumerate(data[r]):
                    a = 0xA8000 + 80 * (10 + r) + 2 * k
                    lines += ["w8 %x %x" % (a, w & 0xFF), "w8 %x %x" % (a + 1, w >> 8)] if split else ["w16 %x %x" % (a, w)]
            out = run(lines + ["out 7c 0", "plane 0 0 0 %x" % (80 * 20), "state"])
            want = bytearray(80 * 20)
            for r in range(rows):
                stream = word_dots(data[r][0]) + word_dots(data[r][1])
                for j in range(19):
                    x, bit = 5 + j, stream[2 + j]
                    want[(10 + r) * 80 + x // 8] |= bit << (7 - x % 8)
            same(self, planes_of(out)[0], bytes(want), "split=%s" % split)
            self.assertEqual(state(out)["egc_under"], "0")

    def test_copy_between_pages(self):
        rnd = random.Random(14)
        p0 = [bytearray(rnd.getrandbits(8) for _ in range(80 * 20)) + bytearray(0x8000 - 1600) for _ in range(4)]
        p1 = [bytearray(rnd.getrandbits(8) for _ in range(80 * 20)) + bytearray(0x8000 - 1600) for _ in range(4)]
        lines = ["init"] + sum((setplane(0, p, 0, p0[p][:1600]) for p in range(4)), [])
        lines += sum((setplane(1, p, 0, p1[p][:1600]) for p in range(4)), [])
        lines += EGC_ON + ["out 4a4 29f0 2"] + self.copy_cmds((37, 2), (5, 9), 123, 5, src_page=1, dst_page=0)
        out = run(lines + ["out 7c 0"] + ["plane 0 %x 0 640" % p for p in range(4)])
        model = self.copy_model(p0, (37, 2), (5, 9), 123, 5, src_planes=p1)
        same(self, planes_of(out), [bytes(m[:1600]) for m in model], "planes")

    def test_missing_prefill_read_shifts_the_row(self):
        """Real machines write nothing on the first write when the source bit address is ahead of the destination's
        and the program did not read one extra unit: the row lands 16 dots to the right and the shifter is left
        unfinished."""
        rnd = random.Random(15)
        planes = [bytearray(rnd.getrandbits(8) for _ in range(800)) + bytearray(0x8000 - 800) for _ in range(4)]
        src, dst, length = (16 * 3 + 9, 1), (16 * 10 + 4, 6), 70        # bit addresses 9 > 4
        lines = ["init"] + sum((setplane(0, p, 0, planes[p][:800]) for p in range(4)), [])
        lines += EGC_ON + ["out 4a4 29f0 2"] + self.copy_cmds(src, dst, length, 1, extra=False) + ["out 7c 0"]
        out = run(lines + ["plane 0 %x 0 320" % p for p in range(4)] + ["state"])
        good = self.copy_model(planes, src, dst, length, 1)
        n_dst = (dst[0] + length - 1) // 16 - dst[0] // 16 + 1
        for p in range(4):
            want = bytearray(planes[p][:800])
            for k in range(n_dst - 1):                  # chunk k of the good copy shows up in destination word k + 1
                w0 = dst[0] // 16 + k
                for j in range(16):
                    x = 16 * w0 + j
                    if dst[0] <= x < dst[0] + length:
                        bit = (good[p][dst[1] * 80 + x // 8] >> (7 - x % 8)) & 1
                        tx = x + 16
                        o = dst[1] * 80 + tx // 8
                        want[o] = (want[o] & ~(0x80 >> (tx % 8))) | (bit << (7 - tx % 8))
            same(self, planes_of(out)[p], bytes(want[:800]), "plane %d" % p)
        st = state(out)
        self.assertEqual(st["egc_under"], "1")
        self.assertNotEqual(st["egc_done"], "0", "the bit length was never reached")


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Gdc(unittest.TestCase):
    def test_protocol_status_and_cursor(self):
        out = run(["init", "in 60", "in a0", "retrace 1 0", "in 60", "retrace 0 1", "in a0", "retrace 0 0",
                   "out 62 c", "state", "out 62 d", "out a2 6b", "state", "out a2 0", "state", "out a2 f", "state",
                   "out a2 e", "state",
                   "out 62 49", "out 60 34", "out 60 12", "out 60 1", "out 62 e0",
                   "in 60", "in 62", "in 62", "in 62", "in 62", "in 62", "in 60", "in 62",
                   "out a2 49", "out a0 78", "out a0 56", "out a0 a3", "out a2 e0", "in a2", "in a2", "in a2", "in a2", "in a2",
                   "out a2 47", "out a0 50", "out a2 46", "out a0 12", "out a2 4b", "out a0 1", "out a0 2", "out a0 3",
                   "out a2 c0", "in a2", "in a2", "in a2", "in a0",
                   "out a2 4a", "out a0 ff", "out a2 e0", "in a0", "out a2 41", "in a0",
                   "out a2 24", "out a2 a4", "out a2 2", "state"])
        ins = [int(a.split()[0], 16) for a in answers(out, "I")]
        self.assertEqual(ins[:4], [0x04, 0x04, 0x24, 0x44], "FIFO empty; VSYNC; HBLANK")
        st = [state([l]) for l in out if l.startswith("Z ")]
        self.assertEqual([s["gdc0_on"] for s in st[:2]], ["0", "1"], "BCTRL")
        self.assertEqual([s["gdc1_on"] for s in st[1:5]], ["1", "0", "1", "0"], "START, RESET, SYNC")
        self.assertEqual(ins[4:12], [0x05, 0x34, 0x12, 0x01, 0x00, 0x00, 0x04, 0xFF], "text CSRR: no dot address")
        self.assertEqual(ins[12:17], [0x78, 0x56, 0x03, 0x00, 0x04], "graphics CSRR: the dot address as a mask")
        self.assertEqual(ins[17:21], [0, 0, 0, 0x04], "LPRD answers three zeros")
        self.assertEqual(ins[21:23], [0x05, 0x04], "a new command drops unread answers")
        last = st[-1]
        self.assertEqual((last["pitch1"], last["zoom1"], last["ead1"], last["dad1"]), ("50", "12", "35678", "a"))
        self.assertEqual((last["dma1"], last["unk1"]), ("2", "2"), "DMAW and DMAR ignored; 41h and 02h unknown")

    def test_wdat_modes_types_fill_planes_and_pages(self):
        w = 0xC3A5
        lines = ["init"] + G_START + g_csrw(32, 10) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + g_figs(2) + \
            gdc(0xA2, 0x20, [w & 0xFF, w >> 8])                             # replace: dot j = bit j
        lines += g_csrw(48, 10) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + gdc(0xA2, 0x20, [0xFF, 0xFF])
        lines += g_csrw(48, 10) + gdc(0xA2, 0x4A, [0x0F, 0x00]) + gdc(0xA2, 0x22, [0xFF, 0xFF])   # reset dots 0-3
        lines += g_csrw(48, 10) + gdc(0xA2, 0x4A, [0xF0, 0xF0]) + gdc(0xA2, 0x21, [0xFF, 0xFF])   # complement 4-7, 12-15
        lines += g_csrw(64, 10) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + gdc(0xA2, 0x33, [0x81])         # low byte, set
        lines += g_csrw(64, 10) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + gdc(0xA2, 0x3B, [0x81])         # high byte, set
        lines += g_csrw(0, 20) + g_figs(2, 0, 9) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + gdc(0xA2, 0x20, [0x01, 0x80])
        lines += g_csrw(0, 30, plane=1) + g_figs(0, 0, 4) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + gdc(0xA2, 0x20, [0xFF, 0x00])
        lines += ["out a6 1"] + g_csrw(16, 5, plane=3) + g_figs(2) + gdc(0xA2, 0x4A, [0xFF, 0xFF]) + \
            gdc(0xA2, 0x20, [0x34, 0x12, 0x78, 0x56]) + ["out a6 0"]
        out = run(lines + dump_planes(0) + ["plane 1 3 0 8000", "state"])
        b, r = lit(planes_of(out)[0]), lit(planes_of(out)[1])
        want_b = {(32 + j, 10) for j in range(16) if w >> j & 1}
        word48 = [1] * 16
        for j in range(4):
            word48[j] = 0                                   # reset under mask 000F
        for j in list(range(4, 8)) + list(range(12, 16)):
            word48[j] ^= 1                                  # complement under mask F0F0
        want_b |= {(48 + j, 10) for j in range(16) if word48[j]}
        want_b |= {(64, 10), (71, 10), (72, 10), (79, 10)}  # 81h in the low byte (dots 0, 7) and the high (8, 15)
        want_b |= {(16 * k, 20) for k in range(10)} | {(16 * k + 15, 20) for k in range(10)}
        self.assertEqual(b, want_b)
        self.assertEqual(r, {(j, 30 + k) for j in range(8) for k in range(5)}, "plane R by the address, DC + 1 lines down")
        self.assertEqual(lit(planes_of(out)[4]), {(16 + j, 5) for j in range(16) if 0x1234 >> j & 1} |
                         {(32 + j, 5) for j in range(16) if 0x5678 >> j & 1}, "page 1, plane E, two words")
        self.assertEqual(state(out)["words1"], str(1 + 1 + 1 + 1 + 1 + 1 + 10 + 5 + 2))

    def test_rdat(self):
        lines = ["init", "setplane 0 0 50 12 34 56 78 9a bc"] + g_csrw(0x50 * 8, 0) + g_figs(2, 0, 2) + gdc(0xA2, 0xA0)
        lines += ["in a0"] + ["in a2"] * 6 + ["in a0"]
        lines += g_csrw(0x50 * 8, 0) + g_figs(2, 0, 1) + gdc(0xA2, 0xB8) + ["in a2", "in a2", "in a0", "state"]
        out = run(lines)
        ins = [int(a.split()[0], 16) for a in answers(out, "I")]
        rv = lambda b: int("{:08b}".format(b)[::-1], 2)    # the GDC's words have the dots in reverse bit order
        self.assertEqual(ins[0], 0x05)
        self.assertEqual(ins[1:7], [rv(0x12), rv(0x34), rv(0x56), rv(0x78), rv(0x9A), rv(0xBC)])
        self.assertEqual(ins[7], 0x04)
        self.assertEqual(ins[8:11], [rv(0x34), rv(0x78), 0x04], "high bytes only")
        self.assertEqual(state(out)["ead1"], "2a")

    def test_text_gdc_writes_text_vram(self):
        lines = ["init"] + gdc(0x62, 0x49, [0xA0, 0x00]) + gdc(0x62, 0x4C, [0x02, 0x02, 0x00]) + \
            gdc(0x62, 0x20, [0x41, 0x00])                                   # 'A' into cells 160-162
        lines += gdc(0x62, 0x49, [0xA0, 0x10]) + gdc(0x62, 0x4C, [0x02, 0x00, 0x00]) + gdc(0x62, 0x20, [0x45, 0x00])
        lines += gdc(0x62, 0x49, [0xF1, 0x1F]) + gdc(0x62, 0x20, [0x99, 0x88])           # memory switch 1: protected
        lines += gdc(0x62, 0x49, [0x10, 0x00]) + gdc(0x62, 0x6C) + gdc(0x62, 0x68)       # no figures in character mode
        out = run(lines + ["rb a0140 8", "rb a2140 4", "memsw 1", "r8 a3fe3", "text", "state"])
        self.assertEqual(answers(out, "B"), ["41 0 41 0 41 0 20 0", "45 0 e1 0"])
        self.assertEqual(ints(out, "S"), [0])
        self.assertEqual(ints(out, "M"), [0x88])
        self.assertEqual([l for l in out if l.startswith("|")][2][1:5], "AAA ")
        self.assertEqual(state(out)["unk0"], "2")

    def test_lines_hand_computed(self):
        out = run(["init", "out a2 20"] + line_cmds(100, 50, 104, 52) + line_cmds(200, 100, 198, 95) +
                  line_cmds(300, 300, 300, 300) + dump_planes(0, 0x8000)[:1] + ["state"])
        self.assertEqual(lit(planes_of(out)[0]),
                         {(100, 50), (101, 51), (102, 51), (103, 52), (104, 52),
                          (200, 100), (200, 99), (199, 98), (199, 97), (198, 96), (198, 95), (300, 300)})
        st = state(out)
        self.assertEqual((st["ead1"], st["dad1"]), ("%x" % (300 * 40 + 300 // 16), "%x" % (300 % 16)), "cursor on the last dot")

    def test_lines_in_every_octant_against_the_model(self):
        rnd = random.Random(42)
        lines, want = ["init", "out a2 23"], set()
        segs = [(320, 200, 320 + dx, 200 + dy) for dx, dy in [(50, 0), (0, 50), (-50, 0), (0, -50), (30, 30), (-30, 30),
                                                               (30, -30), (-30, -30), (40, 7), (7, 40), (-40, 7), (-7, 40),
                                                               (40, -7), (7, -40), (-40, -7), (-7, -40)]]
        segs += [(rnd.randrange(640), rnd.randrange(400), rnd.randrange(640), rnd.randrange(400)) for _ in range(60)]
        for x0, y0, x1, y1 in segs:
            pts = line_model(x0, y0, x1, y1)
            self.assertEqual(pts[-1], (x1, y1), "the model reaches the end point")
            lines += line_cmds(x0, y0, x1, y1)
            want |= set(pts)
        out = run(lines + dump_planes(0)[:1])
        self.assertEqual(lit(planes_of(out)[0]), want)

    def test_line_pattern_and_logic(self):
        lines = ["init"] + gdc(0xA2, 0x78, [0x55, 0x55]) + ["out a2 20"] + line_cmds(0, 0, 31, 0)   # every other dot
        lines += gdc(0xA2, 0x78, [0xFF, 0xFF]) + ["out a2 23"] + line_cmds(0, 2, 15, 2)
        lines += ["out a2 21"] + line_cmds(8, 2, 23, 2)                                                 # complement
        lines += ["out a2 22"] + line_cmds(20, 2, 21, 2)                                                # reset
        out = run(lines + dump_planes(0)[:1])
        want = {(x, 0) for x in range(0, 32, 2)} | {(x, 2) for x in list(range(0, 8)) + list(range(16, 20)) + [22, 23]}
        self.assertEqual(lit(planes_of(out)[0]), want)

    def test_rectangle(self):
        lines = ["init", "out a2 23"] + g_csrw(10, 20) + g_figs(0, 0x40, 3, 3, 5, -1, 3) + FIGD + ["state"]
        out = run(lines + dump_planes(0)[:1])
        want = {(x, y) for x in range(10, 16) for y in (20, 23)} | {(x, y) for x in (10, 15) for y in range(20, 24)}
        self.assertEqual(lit(planes_of(out)[0]), want)
        st = state(out)
        self.assertEqual((st["ead1"], st["dad1"], st["dots1"]), ("%x" % (20 * 40), "a", "16"), "back at the start")

    def test_arcs_against_the_model(self):
        lines, want = ["init", "out a2 23"], set()
        cases = [(320, 200, k, r, int(r / math.sqrt(2)), 0) for k in range(8) for r in (5, 17, 60)]
        cases += [(100, 100, 0, 40, 28, 10), (500, 300, 3, 25, 17, 5), (300, 100, 6, 1, 0, 0), (200, 300, 1, 30, 35, 0)]
        for x0, y0, k, r, dc, dm in cases:
            lines += arc_cmds(x0, y0, k, r, dc, dm)
            want |= set(arc_model(x0, y0, k, r, dc, dm))
        out = run(lines + dump_planes(0)[:1])
        self.assertEqual(lit(planes_of(out)[0]), want)

    def test_graphics_characters_and_dots(self):
        pat = [0x01, 0x82, 0x44, 0x28, 0x10, 0x28, 0x44, 0x83]
        lines = ["init", "out a2 23"] + gdc(0xA2, 0x78, pat)          # PRAM 8-15 (8-9 are also the line pattern)
        lines += g_csrw(40, 40) + g_figs(2, 0x10, 7, 8) + gdc(0xA2, 0x68)
        lines += gdc(0xA2, 0x46, [0x01]) + g_csrw(100, 40) + g_figs(2, 0x10, 7, 8) + gdc(0xA2, 0x68)
        lines += gdc(0xA2, 0x78, [0xFF, 0xFF]) + g_csrw(200, 40) + g_figs(1, 0, 4) + FIGD   # no flags: DC + 1 dots
        out = run(lines + dump_planes(0)[:1])
        want = {(40 + i, 40 + j) for j in range(8) for i in range(8) if pat[j] >> i & 1}
        want |= {(100 + 2 * i + a, 40 + 2 * j + b) for j in range(8) for i in range(8) if pat[j] >> i & 1
                 for a in (0, 1) for b in (0, 1)}
        want |= {(200 + i, 40 + i) for i in range(5)}
        self.assertEqual(lit(planes_of(out)[0]), want)


def parse_font8x16():
    """The repository's own font, read from its C source: {code point: 16 rows} and the 'missing' box."""
    src = (x86ref.REPO / "shared" / "font8x16.c").read_text()
    glyphs = {int(m.group(1), 16): [int(v, 16) for v in m.group(2).split(",")]
              for m in re.finditer(r"\{ 0x([0-9A-F]+), \{ ([^}]*) \} \}", src)}
    notdef = [int(v, 16) for v in re.search(r"notdef\[16\] = \{ ([^}]*) \}", src).group(1).split(",")]
    return glyphs, notdef


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Fonts(unittest.TestCase):
    def test_ank_to_unicode(self):
        out = run(["ank %x" % c for c in (0x20, 0x41, 0x5C, 0x7E, 0x7F, 0xA0, 0xA1, 0xB1, 0xDF, 0xE0, 0x05, 0x80)])
        self.assertEqual(ints(out, "U"), [0x20, 0x41, 0xA5, 0x203E, 0, 0, 0xFF61, 0xFF71, 0xFF9F, 0, 0, 0])

    def test_ank_table_from_the_repository_font(self):
        glyphs, notdef = parse_font8x16()
        codes = [0x41, 0x7A, 0x5C, 0x7E, 0xB1, 0x80, 0x05, 0x20]
        out = run(["font repo"] + ["cg %x" % c for c in codes])
        got = {c: bytes.fromhex(a.split()[1])[:16] for c, a in zip(codes, answers(out, "G"))}
        self.assertEqual(list(got[0x41]), glyphs[0x41])
        self.assertEqual(list(got[0x7A]), glyphs[0x7A])
        self.assertEqual(list(got[0x5C]), glyphs[0xA5], "5Ch is the yen sign")
        self.assertEqual(list(got[0x7E]), glyphs[0xAF], "the overline is missing: the macron stands in")
        self.assertEqual(list(got[0xB1]), notdef, "no half-width katakana in that font: its box")
        self.assertEqual(got[0x80], bytes(16), "graphic characters: not mapped")
        self.assertEqual(got[0x05], bytes(16))

    def test_character_generator_ports_and_gaiji(self):
        lines = ["init", "font synth", "kanji synth"]
        lines += ["out a3 0", "out a1 41", "out a5 3", "in a9", "out a5 23", "in a9"]          # ANK 'A', line 3 (half ignored)
        lines += ["out a3 10", "out a1 21", "out a5 25", "in a9", "out a5 5", "in a9"]          # JIS 3021h line 5, left/right
        lines += ["out a3 5e", "out a1 7e", "out a5 20", "in a9"]                               # row 7Eh: no glyph
        lines += ["out a3 56", "out a1 3"] + sum((["out a5 %x" % (0x20 | y), "out a9 %x" % (y * 3), "out a5 %x" % y,
                                                    "out a9 %x" % (0xF0 - y)] for y in range(16)), [])
        lines += ["out a5 27", "in a9", "out a5 7", "in a9", "cg 356", "cg 3d6"]
        lines += ["out a3 10", "out a1 21", "out a5 20", "out a9 0", "in a9"]                   # ROM characters stay
        lines += ["out a3 57", "out a1 7f", "out a5 2f", "out a9 5a", "cg 7f57", "out a3 d7", "in a9"]
        out = run(lines)
        ins = [int(a.split()[0], 16) for a in answers(out, "I")]
        k = kanji_synth(0x3021)
        self.assertEqual(ins[:2], [synth(0x41, 3)] * 2)
        self.assertEqual(ins[2:4], [k[5], k[16 + 5]])
        self.assertEqual(ins[4], 0)
        self.assertEqual(ins[5:7], [21, 0xF0 - 7])
        gl = [bytes.fromhex(a.split()[1]) for a in answers(out, "G")]
        self.assertEqual(gl[0], bytes([y * 3 for y in range(16)] + [0xF0 - y for y in range(16)]))
        self.assertEqual(gl[0], gl[1], "the right-half mark (bit 7) does not change the character")
        self.assertEqual(ins[7], k[0])
        self.assertEqual(gl[2][15], 0x5A)
        self.assertEqual(ins[8], 0x5A, "bit 7 of A3h is ignored")

    @unittest.skipUnless(any(p and os.path.isfile(p) for p in FONT_ROM_PLACES), "the owner's FONT.ROM is not here")
    def test_owners_font_rom(self):
        path = next(p for p in FONT_ROM_PLACES if p and os.path.isfile(p))
        lines = ["fontrom %s" % path, "font rom %s" % path, "cg 41", "cg 20", "cg 2110", "cg 3110", "cg 2109", "cg 2156"]
        lines += cell(0, 0, 0x41) + cell(2, 0, 0x2110) + cell(3, 0, 0x2190)
        pic, out = render(lines)
        self.assertEqual(ints(out, "V"), [1])
        g = [bytes.fromhex(a.split()[1]) for a in answers(out, "G")]
        self.assertTrue(any(g[0][:16]) and not any(g[0][16:]), "ANK 'A': drawn, left half only")
        self.assertFalse(any(g[1]), "space")
        self.assertTrue(any(g[2][:16]) and any(g[2][16:]), "JIS 3021h: both halves")
        self.assertNotEqual(g[2], g[3], "a different kanji")
        self.assertTrue(any(g[4][:16]) and not any(g[4][16:]), "row 29h: a half-width character")
        self.assertFalse(any(g[5]), "the user-defined row comes from RAM: blank")
        self.assertGreater(sum(1 for x in range(32) for y in range(16) if pic[x, y]), 20, "text drawn")


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Text(unittest.TestCase):
    def assertCell(self, pic, col, row, lines_bits, colour, h=16, wide=False):
        w = 16 if wide else 8
        for y, bits in enumerate(lines_bits):
            for x in range(w):
                on = bits >> (7 - (x // 2 if wide else x)) & 1
                self.assertEqual(pic[col * w + x, row * h + y], colour if on else 0, (col, row, x, y))

    def test_attributes_and_colours(self):
        cases = [(0x41, 0xE1), (0x42, 0x21), (0x43, 0x41), (0x44, 0x81), (0x45, 0xC1), (0x46, 0x00), (0x47, 0xE3),
                 (0x48, 0xE5), (0x49, 0xE9), (0x4A, 0xF1), (0x4B, 0xED), (0x4C, 0x05), (0x4D, 0xA4), (0x4E, 0xFF)]
        lines = ["init", "font synth"]
        for i, (c, a) in enumerate(cases):
            lines += cell(2 * i, 3, c, a)
        for blink in (1, 0):
            pic, _ = render(lines, phases=(blink, 1))
            for i, (c, a) in enumerate(cases):
                bits = [attr_bits(synth(c, y), a, y, blink=bool(blink)) for y in range(16)]
                self.assertCell(pic, 2 * i, 3, bits, grb_rgb(a >> 5))
        # semigraphics mode: attribute bit 4 turns the code into 2 x 4 blocks
        pic, _ = render(lines + cell(0, 5, 0x96, 0xF1) + cell(1, 5, 0x5A, 0xE1) + ["out 68 1"], phases=(1, 1))
        self.assertCell(pic, 0, 5, [attr_bits(0, 0xF1, y, semi=0x96) for y in range(16)], 0xFFFFFF)
        self.assertCell(pic, 1, 5, [attr_bits(synth(0x5A, y), 0xE1, y, semi=0x5A) for y in range(16)], 0xFFFFFF)

    def test_spaces_and_blank_font(self):
        pic, _ = render(["init"] + cell(5, 5, 0x41, 0xED), phases=(1, 1))      # no font: only the decorations show
        self.assertCell(pic, 5, 5, [attr_bits(0, 0xED, y) for y in range(16)], 0xFFFFFF)

    def test_kanji_halves_and_missing_glyphs(self):
        lines = ["init", "kanji synth"] + cell(10, 2, 0x2110) + cell(11, 2, 0x2190) + cell(13, 2, 0x445E) + \
            cell(14, 2, 0x44DE, 0x45)
        pic, _ = render(lines, phases=(1, 1))
        k = kanji_synth(0x3021)
        self.assertCell(pic, 10, 2, k[:16], 0xFFFFFF)
        self.assertCell(pic, 11, 2, k[16:], 0xFFFFFF)
        self.assertCell(pic, 13, 2, [0] * 16, 0)
        self.assertCell(pic, 14, 2, [0xFF] * 16, 0xFF0000)      # missing glyph, reversed: a red block

    def test_right_half_is_marked_by_bit_7_of_either_byte(self):
        """NEC's DOS marks the right-hand cell of a two-cell character with bit 7 of the low byte (the JIS row byte: 2110h, 2190h);
        TH02's own writer for its user-defined characters marks it with bit 7 of the high byte (the character's second code byte:
        5603h, 5683h with the cell number 3 in the high byte: 0356h, 8356h). Both are the right half; read only as the low byte's
        bit the second cell was drawn as a second left half and every HUD text of TH02 to TH05 came out as repeated left halves."""
        gaiji = ["out a3 56", "out a1 3"] + sum((["out a5 %x" % (0x20 | y), "out a9 %x" % (y * 3 + 1), "out a5 %x" % y,
                                                    "out a9 %x" % (0xF0 - y)] for y in range(16)), [])
        lines = ["init", "kanji synth"] + gaiji
        lines += cell(10, 2, 0x2110) + cell(11, 2, 0x2190) + cell(12, 2, 0xA110)        # a kanji: left, right by the low byte, right by the high byte
        lines += cell(14, 2, 0x0356) + cell(15, 2, 0x03D6) + cell(16, 2, 0x8356)        # a user-defined character: the same three
        pic, _ = render(lines, phases=(1, 1))
        k = kanji_synth(0x3021)
        self.assertCell(pic, 10, 2, k[:16], 0xFFFFFF)
        self.assertCell(pic, 11, 2, k[16:], 0xFFFFFF)
        self.assertCell(pic, 12, 2, k[16:], 0xFFFFFF)
        left = [y * 3 + 1 for y in range(16)]
        right = [0xF0 - y for y in range(16)]
        self.assertCell(pic, 14, 2, left, 0xFFFFFF)
        self.assertCell(pic, 15, 2, right, 0xFFFFFF)
        self.assertCell(pic, 16, 2, right, 0xFFFFFF)

    def test_cursor(self):
        csr = gdc(0x62, 0x4B, [0x8F, 0x0E, 0x78 | 0x01])        # displayed, lines 14-15, blinking, rate 4
        lines = ["init", "font synth"] + cell(7, 1, 0x30) + csr + gdc(0x62, 0x49, [80 + 7, 0])
        on, _ = render(lines, phases=(1, 1))
        off, _ = render(lines, phases=(1, 0))
        self.assertCell(on, 7, 1, [synth(0x30, y) ^ (0xFF if y >= 14 else 0) for y in range(16)], 0xFFFFFF)
        self.assertCell(off, 7, 1, [synth(0x30, y) for y in range(16)], 0xFFFFFF)
        # the frame counter drives the phases: cursor every 4 frames, attribute blink every 32
        out = run(lines + ["hash 1 1", "hash 1 0", "hash", "vsync 4", "hash", "vsync 4", "hash", "state"])
        h = answers(out, "H")
        self.assertEqual(h[2], h[0])
        self.assertEqual(h[3], h[1])
        self.assertEqual(h[4], h[0])
        steady = run(lines + gdc(0x62, 0x4B, [0x8F, 0x2E, 0x79]) + ["vsync 4", "hash", "hash 1 1"])
        self.assertEqual(answers(steady, "H")[0], answers(steady, "H")[1], "steady cursor")

    def test_partitions_scroll_the_text(self):
        lines = ["init", "font synth"]
        for r in range(25):
            lines += cell(0, r, 0x41 + r)
        # partition 1: from row 5 for 10 rows (160 lines); partition 2: from row 0 for the rest
        sad1 = 5 * 80
        lines += gdc(0x62, 0x70, [sad1 & 0xFF, sad1 >> 8, (160 & 15) << 4, 160 >> 4, 0, 0, (240 & 15) << 4, 240 >> 4])
        pic, _ = render(lines, phases=(1, 1))
        shown = [sum(1 << (7 - x) for x in range(8) if pic[x, 16 * r]) for r in range(25)]   # line 0 of the glyph = its code
        self.assertEqual(shown, [0x41 + r for r in range(5, 15)] + [0x41 + r for r in range(15)])
        out = run(lines + ["text"])
        self.assertEqual([l[1] for l in out if l.startswith("|")], [chr(0x41 + r) for r in range(5, 25)] + [" "] * 5)

    def test_twenty_line_rows_and_forty_columns(self):
        lines = ["init", "font synth"] + cell(0, 1, 0x41, 0xE9) + cell(1, 1, 0x42) + gdc(0x62, 0x4B, [0x13, 0, 0x7B])
        pic, _ = render(lines, phases=(1, 1))
        self.assertCell(pic, 0, 1, [attr_bits(synth(0x41, y), 0xE9, y, h=20) for y in range(20)], 0xFFFFFF, h=20)
        pic, _ = render(lines + ["out 68 5"], phases=(1, 1))
        self.assertCell(pic, 1, 1, [synth(0x42, y) if y < 16 else 0 for y in range(20)], 0xFFFFFF, h=20, wide=True)

    def test_text_dump(self):
        lines = ["init"] + cell(0, 0, 0x48) + cell(1, 0, 0x69) + cell(2, 0, 0x2110) + cell(3, 0, 0x2190) + \
            cell(4, 0, 0x7F) + cell(5, 0, 0x07) + cell(6, 0, 0xB1) + cell(7, 0, 0x41, 0xE0) + cell(79, 24, 0x7E)
        out = run(lines + ["text"])
        scr = [l[1:] for l in out if l.startswith("|")]
        self.assertEqual(scr[0][:9], "Hi??? ?  ")
        self.assertEqual(scr[24][79], "~")

    def test_display_switches(self):
        lines = ["init", "font synth"] + cell(0, 0, 0x41) + G_START + ["setplane 0 0 50 ff"]   # under glyph line 1
        on, _ = render(lines, phases=(1, 1))
        self.assertEqual(on[0, 1], 0x0000FF)                # graphics colour 1 under the text
        self.assertEqual(on[1, 1], 0xFFFFFF)                # text dot
        off, _ = render(lines + ["out 68 e"], phases=(1, 1))
        self.assertEqual({off[x, y] for x in range(16) for y in range(16)}, {0}, "display disabled")
        notext, _ = render(lines + TEXT_OFF, phases=(1, 1))
        self.assertEqual(notext[1, 1], 0x0000FF)
        nogfx, _ = render(lines + gdc(0xA2, 0x0C), phases=(1, 1))
        self.assertEqual(nogfx[0, 1], 0)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Graphics(unittest.TestCase):
    def test_displayed_page(self):
        lines = ["init"] + TEXT_OFF + G_START + ["setplane 0 1 0 80", "setplane 1 2 0 80"]
        p0, _ = render(lines)
        p1, _ = render(lines + ["out a4 1", "out a6 0"])
        self.assertEqual((p0[0, 0], p1[0, 0]), (0xFF0000, 0x00FF00))

    def test_partitions_scroll_the_graphics(self):
        lines = ["init"] + TEXT_OFF + G_START
        lines += ["setplane 0 0 %x %x" % (y * 80, y & 0xFF) for y in range(400)]
        sad1 = 100 * 40
        lines += gdc(0xA2, 0x70, [sad1 & 0xFF, sad1 >> 8, (300 & 15) << 4, 300 >> 4, 0, 0, (100 & 15) << 4, 100 >> 4])
        pic, _ = render(lines)
        shown = [sum(1 << (7 - x) for x in range(8) if pic[x, y]) for y in range(400)]
        self.assertEqual(shown, [(y + 100) & 0xFF for y in range(300)] + [y for y in range(100)])

    def test_two_hundred_line_modes(self):
        lines = ["init"] + TEXT_OFF + G_START + ["setplane 0 0 %x %x" % (y * 80, y) for y in range(200)]
        lines += gdc(0xA2, 0x4B, [0x01, 0, 0])
        pic, _ = render(lines)
        shown = [sum(1 << (7 - x) for x in range(8) if pic[x, y]) for y in range(400)]
        self.assertEqual(shown, [y // 2 for y in range(400)])
        pic, _ = render(lines + ["out 68 9"])
        shown = [sum(1 << (7 - x) for x in range(8) if pic[x, y]) for y in range(400)]
        self.assertEqual(shown, [0 if y & 1 else y // 2 for y in range(400)])

    def test_whole_picture_against_a_model(self):
        rnd = random.Random(400)
        planes = [bytearray(rnd.getrandbits(8) if rnd.random() < 0.7 else 0 for _ in range(32000)) for _ in range(4)]
        pal = [(rnd.randrange(16), rnd.randrange(16), rnd.randrange(16)) for _ in range(16)]
        cells = [(rnd.choice([0x20, 0x20, rnd.randrange(0x21, 0x7F)]), rnd.choice([0xE1, 0x21, 0x45, 0x89, 0xC5, 0x00]))
                 for _ in range(2000)]
        lines = ["init", "font synth"] + G_START + sum((setplane(0, p, 0, planes[p]) for p in range(4)), [])
        lines += ["out 6a 1"] + sum((["out a8 %x" % i, "out aa %x" % g, "out ac %x" % r, "out ae %x" % b]
                                     for i, (g, r, b) in enumerate(pal)), [])
        for i, (c, a) in enumerate(cells):
            lines += ["w8 %x %x" % (0xA0000 + 2 * i, c), "w8 %x %x" % (0xA2000 + 2 * i, a)]
        pic, _ = render(lines, phases=(1, 1))
        rgb = [(r * 17) << 16 | (g * 17) << 8 | b * 17 for g, r, b in pal]
        want = [0] * (640 * 400)
        for y in range(400):
            for x in range(640):
                o, b = y * 80 + x // 8, 7 - x % 8
                want[y * 640 + x] = rgb[sum(((planes[p][o] >> b) & 1) << p for p in range(4))]
        for i, (c, a) in enumerate(cells):
            row, col = divmod(i, 80)
            for y in range(16):
                bits = attr_bits(synth(c, y), a, y)
                for x in range(8):
                    if bits >> (7 - x) & 1:
                        want[(row * 16 + y) * 640 + col * 8 + x] = grb_rgb(a >> 5)
        bad = next((i for i, (a, b) in enumerate(zip(pic.px, want)) if a != b), None)
        if bad is not None:
            self.fail("pixel (%d, %d): %06x, model %06x" % (bad % 640, bad // 640, pic.px[bad], want[bad]))

    # Regression guards: CRC-32 of the picture bytes of fixed scenes. Recorded from this implementation after the
    # model tests above passed; a change means the renderer's output changed (check the model tests first).
    SCENES = {
        "text_attributes": ["init", "font synth", "kanji synth"] + sum((cell(i, i % 25, 0x30 + i, (i * 37) & 0xFF)
                                                                      for i in range(80)), []) +
        cell(10, 10, 0x2110) + cell(11, 10, 0x2190) + gdc(0x62, 0x4B, [0x8F, 0x0C, 0x7B]) + gdc(0x62, 0x49, [10, 0]),
        "graphics_16": ["init"] + TEXT_OFF + G_START + ["out 6a 1"] +
        ["setplane 0 %x %x %s" % (p, 80 * y, hexs(((x * 7 + y * 3) >> p) & 0xFF for x in range(80)))
         for p in range(4) for y in range(0, 400, 7)],
        "gdc_figures": ["init", "out a2 23"] + G_START + TEXT_OFF + line_cmds(0, 0, 639, 399) + line_cmds(639, 0, 0, 399) +
        arc_cmds(320, 100, 2, 90, 63, 0) + arc_cmds(320, 100, 5, 90, 63, 0) + g_csrw(50, 300) +
        g_figs(2, 0x40, 3, 99, 49, -1, 99) + FIGD,
    }
    HASHES = {"text_attributes": "af8c6a3e", "graphics_16": "ebc63142", "gdc_figures": "16fe1f99"}

    def test_regression_hashes(self):
        got = {}
        for name, lines in self.SCENES.items():
            got[name] = answers(run(lines + ["hash 1 1"]), "H")[0]
        missing = [n for n, h in self.HASHES.items() if h is None]
        if missing:
            self.fail("record these hashes: %r" % got)
        self.assertEqual(got, self.HASHES)

    def test_ppm_round_trip(self):
        lines = self.SCENES["graphics_16"] + ["font synth"] + cell(3, 3, 0x41)
        with tempfile.TemporaryDirectory() as d:
            raw, ppm = Path(d) / "a.bin", Path(d) / "a.ppm"
            run(lines + ["renderp %s 1 1" % raw, "ppm %s 1 1" % ppm])
            px = struct.unpack("<%dI" % (640 * 400), raw.read_bytes())
            data = ppm.read_bytes()
        head = b"P6\n640 400\n255\n"
        self.assertEqual(data[:len(head)], head)
        body = data[len(head):]
        self.assertEqual(len(body), 640 * 400 * 3)
        self.assertEqual(body, b"".join(struct.pack(">I", v)[1:] for v in px))

    def test_size_and_speed(self):
        out = run(["size", "font synth", "kanji synth"] + self.SCENES["text_attributes"][3:] + G_START + ["bench 50"],
                  exe=bench_tool())
        size = int(answers(out, "Y")[0])
        self.assertTrue(280 * 1024 < size < 300 * 1024, size)
        t = [float(v) for v in answers(out, "T")[0].split()]
        print("\npc98gfx (-O2): struct %d bytes; render %.0f us/frame, GRCG fill %.0f us/32000 bytes, EGC copy %.0f us/16000 words"
              % (size, t[0], t[1], t[2]), file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
