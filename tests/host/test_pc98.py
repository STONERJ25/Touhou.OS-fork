#!/usr/bin/env python3
"""Host tests for the PC-98 machine skeleton and the disk image readers (user/lib/pc98), through tests/host/pc98tool.c.

Every disk image is synthesized here (D88, FDI, HDI, raw); every program is written from scratch (a hand-assembled boot
sector, and tests/host/pc98boot/hello.asm when nasm is installed). Nothing from a real PC-98 is used: no ROM, no font,
no game data (docs/ASSETS.md, docs/PC98.md). Needs gcc (the tool is built with AddressSanitizer and UBSan).

    python3 -m unittest discover -s tests/host -p "test_pc98.py" -v
"""
import os
import random
import shutil
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
OUT = x86ref.REPO / "build" / "host-pc98"

# tests/host/pc98boot/tiny.asm, assembled by hand:
TINY_BOOT = bytes([
    0x0E,                   # push cs
    0x1F,                   # pop ds
    0xB8, 0x00, 0xA0,       # mov ax, 0A000h
    0x8E, 0xC0,             # mov es, ax
    0xBE, 0x19, 0x00,       # mov si, msg (offset 19h)
    0x31, 0xFF,             # xor di, di
    0xFC,                   # cld
    0x30, 0xE4,             # xor ah, ah
    0xAC,                   # next: lodsb
    0x08, 0xC0,             # or al, al
    0x74, 0x03,             # jz done
    0xAB,                   # stosw
    0xEB, 0xF8,             # jmp next
    0xFA,                   # done: cli
    0xF4,                   # hlt
]) + b"TOUHOU OS: SHRINE BOOT OK\x00"


def tool():
    return x86ref.build("pc98tool", [HERE / "pc98tool.c"])


def run(lines):
    r = subprocess.run([str(tool())], input="\n".join(lines) + "\n", capture_output=True, text=True, errors="replace",
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        raise RuntimeError("pc98tool failed (%d): %s" % (r.returncode, r.stderr[-3000:]))
    return r.stdout.splitlines()


def state(line):
    assert line.startswith("Z "), line
    return {k: v for k, v in (kv.split("=") for kv in line[2:].split())}


def screen(lines):
    return [l[1:] for l in lines if l.startswith("|")]


# ------------------------------------------------------------------ image builders

def d88(tracks, name=b"TOUHOU TEST", media=0x20, wp=False, ntable=164, trailing=b""):
    """tracks: {track index: [(c, h, r, n, data, deleted, status, density)]}."""
    header = bytearray(0x20 + 4 * ntable)
    header[:len(name)] = name
    header[0x1A] = 0x10 if wp else 0
    header[0x1B] = media
    body = bytearray()
    for t in sorted(tracks):
        struct.pack_into("<I", header, 0x20 + 4 * t, len(header) + len(body))
        secs = tracks[t]
        for c, h, r, n, data, deleted, status, density in secs:
            body += struct.pack("<BBBBHBBB5xH", c, h, r, n, len(secs), density, deleted, status, len(data)) + data
    struct.pack_into("<I", header, 0x1C, len(header) + len(body))
    return bytes(header + body) + trailing


def pattern(seed, n):
    rnd = random.Random(seed)
    return bytes(rnd.getrandbits(8) for _ in range(n))


def standard_d88(boot, cyls=2, wp=False):
    """A 2HD disk: 8 sectors of 1024 bytes per track, the boot program in C0 H0 R1."""
    tracks = {}
    for c in range(cyls):
        for h in range(2):
            secs = []
            for r in range(1, 9):
                data = pattern(c * 100 + h * 10 + r, 1024)
                if (c, h, r) == (0, 0, 1):
                    data = (boot + bytes(1024))[:1024]
                secs.append((c, h, r, 3, data, 0, 0, 0))
            tracks[c * 2 + h] = secs
    return d88(tracks, wp=wp)


def anex(kind, cyls, heads, spt, ssz, first=b"", hdr=4096, ftype=None):
    """FDI or HDI: eight 32-bit words, a header of `hdr` bytes, then the sectors in C/H/R order."""
    n = cyls * heads * spt
    data = bytearray()
    for i in range(n):
        data += pattern(i * 7 + 3, ssz)
    data[:len(first)] = first
    t = ftype if ftype is not None else (0x90 if kind == "fdi" else 0)
    head = struct.pack("<8I", 0, t, hdr, len(data), ssz, spt, heads, cyls)
    return head + bytes(hdr - len(head)) + bytes(data)


class Images:
    def __init__(self):
        self.dir = Path(tempfile.mkdtemp(prefix="pc98test-"))

    def put(self, name, data):
        p = self.dir / name
        p.write_bytes(data)
        return p

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class DiskImages(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def test_d88_layout(self):
        tracks = {
            0: [(0, 0, 1, 3, pattern(1, 1024), 0, 0, 0), (0, 0, 2, 3, pattern(2, 1024), 0, 0, 0)],
            1: [(0, 1, 1, 1, pattern(3, 256), 0, 0, 0x40), (0, 1, 2, 2, pattern(4, 512), 1, 0, 0),   # FM, deleted
                (0, 1, 3, 0, pattern(5, 128), 0, 0xB0, 0)],                                          # CRC error status
            # track 2 absent
            3: [(1, 1, 9, 3, pattern(6, 1024), 0, 0, 0), (7, 7, 1, 3, pattern(7, 1024), 0, 0, 0)],  # odd ID on the track
        }
        p = self.img.put("t.d88", d88(tracks, name=b"REIMU", wp=True))
        out = run(["disk %s none" % p, "sectors", "find 0 1 2", "find 1 1 1", "find 1 1 9", "find 0 0 5", "find 1 0 1"])
        f = out[0].split()
        self.assertEqual(f[1:4], ["0", "D88", "2"], out[0])          # rc, format, cylinders
        self.assertEqual(f[4:9], ["2", "2", "400", "20", "1"], out[0])  # heads, spt, ssz, media, wp
        self.assertEqual(f[9], "REIMU")
        want = []
        for t, secs in sorted(tracks.items()):
            for i, (c, h, r, n, data, dele, st, dens) in enumerate(secs):
                want.append("S %x %x %x %x %x %x %x %x %x %x %08x" % (t // 2, t % 2, i, c, h, r, n, len(data), dele, st, zlib.crc32(data)))
        got = out[1:out.index("END")]
        self.assertEqual(got, want)
        rest = out[out.index("END") + 1:]
        self.assertTrue(rest[0].startswith("S 0 1 0 0 1 2 2 200 1 0 "), rest[0])     # deleted mark reported
        self.assertTrue(rest[1].startswith("S 1 1 0 7 7 1 3 400"), rest[1])           # R=1 only under another ID
        self.assertTrue(rest[2].startswith("S 1 1 0 1 1 9 3 400"), rest[2])
        self.assertEqual(rest[3], "N -3")                                              # no such sector
        self.assertEqual(rest[4], "N -3")                                              # absent track

    def test_d88_short_table_and_multi_disk(self):
        tracks = {0: [(0, 0, 1, 3, pattern(9, 1024), 0, 0, 0)]}
        img = d88(tracks, ntable=160, trailing=b"SECOND DISK" * 10)     # 160-entry table, a second image after it
        p = self.img.put("s.d88", img)
        out = run(["disk %s d88" % p, "sectors"])
        self.assertTrue(out[0].startswith("F 0 D88 1 1 1 400"), out[0])
        self.assertEqual(out[1], "S 0 0 0 0 0 1 3 400 0 0 %08x" % zlib.crc32(pattern(9, 1024)))

    def test_fdi_hdi_raw(self):
        fdi = anex("fdi", 77, 2, 8, 1024, first=b"FDI!")
        hdi = anex("hdi", 20, 4, 17, 512, first=b"HDI!", hdr=4096)
        raw = b"".join(pattern(i, 1024) for i in range(77 * 2 * 8))
        pf, ph, pr = self.img.put("a.fdi", fdi), self.img.put("b.hdi", hdi), self.img.put("c.hdm", raw)
        out = run(["disk %s none" % pf, "lba 0", "lba 1", "lba 9", "lba 4d0", "find 4c 1 8",
                   "disk %s hdi" % ph, "lba 0", "lba 54f", "lba 550",
                   "disk %s none" % pr, "lba 10"])
        self.assertEqual(out[0], "F 0 FDI 4d 2 8 400 90 0 -")
        sec = lambda img, hdr, ssz, i: img[hdr + i * ssz: hdr + (i + 1) * ssz]
        self.assertEqual(out[1], "L 0 0 0 1 400 %08x" % zlib.crc32(sec(fdi, 4096, 1024, 0)))
        self.assertEqual(out[2], "L 1 0 0 2 400 %08x" % zlib.crc32(sec(fdi, 4096, 1024, 1)))
        self.assertEqual(out[3], "L 9 0 1 2 400 %08x" % zlib.crc32(sec(fdi, 4096, 1024, 9)))
        self.assertEqual(out[4], "L 4d0 -3")                               # 1232 sectors: 0..4CF
        self.assertTrue(out[5].endswith("%08x" % zlib.crc32(sec(fdi, 4096, 1024, 77 * 16 - 1))), out[5])
        self.assertEqual(out[6], "F 0 HDI 14 4 11 200 0 0 -")
        self.assertEqual(out[7], "L 0 0 0 1 200 %08x" % zlib.crc32(sec(hdi, 4096, 512, 0)))
        self.assertEqual(out[8], "L 54f 13 3 11 200 %08x" % zlib.crc32(sec(hdi, 4096, 512, 0x54F)))
        self.assertEqual(out[9], "L 550 -3")
        self.assertEqual(out[10], "F 0 raw 4d 2 8 400 0 0 -")
        self.assertEqual(out[11], "L 10 1 0 1 400 %08x" % zlib.crc32(sec(raw, 0, 1024, 16)))

    def test_detection_without_hint(self):
        hdi = anex("hdi", 300, 8, 17, 512, hdr=4096)       # many heads and cylinders: a hard disk
        out = run(["disk %s none" % self.img.put("x.img", hdi)])
        self.assertTrue(out[0].startswith("F 0 HDI"), out[0])
        out = run(["disk %s none" % self.img.put("junk.bin", b"\x01" * 5000)])
        self.assertEqual(out[0].split()[1:3], ["-1", "none"])

    def test_damaged_images_are_refused_not_crashed(self):
        rnd = random.Random(2024)
        good = [standard_d88(TINY_BOOT, cyls=2), anex("fdi", 4, 2, 8, 1024), anex("hdi", 4, 2, 17, 512)]
        lines = []
        for i in range(400):
            b = bytearray(rnd.choice(good))
            mode = rnd.choice(("header", "flip", "trunc", "offsets"))
            if mode == "header":
                for _ in range(rnd.randint(1, 6)):
                    b[rnd.randrange(0, 0x40)] = rnd.getrandbits(8)
            elif mode == "flip":
                for _ in range(rnd.randint(1, 20)):
                    b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
            elif mode == "trunc":
                del b[rnd.randrange(len(b)):]
            else:
                for _ in range(rnd.randint(1, 4)):
                    o = 0x20 + 4 * rnd.randrange(8)
                    if o + 4 <= len(b):
                        struct.pack_into("<I", b, o, rnd.getrandbits(32) if rnd.random() < 0.5 else rnd.randrange(len(b) + 64))
                    # sector sizes inside the first track
                    p = 0x2B0 + 14
                    if p + 2 <= len(b):
                        struct.pack_into("<H", b, p, rnd.getrandbits(16))
            p = self.img.put("f%d.img" % i, bytes(b))
            hint = rnd.choice(("none", "d88", "fdi", "hdi"))
            lines += ["disk %s %s" % (p, hint), "sectors", "lba 0", "lba 7", "find 0 0 1", "find 1 1 8", "boot 90", "run 2000"]
        out = run(lines)       # AddressSanitizer aborts the tool on any out-of-bounds read
        self.assertEqual(sum(1 for l in out if l.startswith("F ")), 400)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Devices(unittest.TestCase):
    PIC_INIT = ["out 0 11", "out 2 8", "out 2 80", "out 2 1d", "out 2 0",
                "out 8 11", "out a 10", "out a 7", "out a 9", "out a 0"]

    def test_pic(self):
        out = run(["init v30 10"] + self.PIC_INIT + [
            "irq 0 1", "ack",                      # IR0 -> vector 08h
            "irq 1 1", "ack",                      # IR1 waits: IR0 is in service
            "out 0 20", "ack",                     # non-specific EOI, now IR1
            "out 0 61", "ack",                     # specific EOI for IR1; nothing left
            "irq 0 0", "irq 0 1", "irq 3 1", "ack", "ack",   # two at once: IR0 first, IR3 waits until IR0 ends
            "out 0 60", "ack", "out 0 20",
            "out 2 4", "irq 2 0", "irq 2 1", "ack",          # IR2 masked
            "out 0 a", "in 0", "out 0 b", "in 0", "in 2",    # read IRR, ISR, IMR
            "out 2 0", "ack", "out 0 20",
            "irq 9 1", "ack", "state",                       # slave IR1 through master IR7 -> vector 11h
        ])
        acks = [l for l in out if l.startswith("A ")]
        self.assertEqual(acks, ["A 8", "A -1", "A 9", "A -1", "A 8", "A -1", "A b", "A -1", "A a", "A 11"])
        ins = [l for l in out if l.startswith("I ")]
        self.assertEqual(ins, ["I 4", "I 0", "I 4"])
        st = state(out[-1])
        self.assertEqual((st["isr0"], st["isr1"]), ("80", "2"))

    def test_pic_level_and_auto_eoi(self):
        out = run(["init v30 10", "out 0 1b", "out 2 8", "out 2 3", "ack",       # single, level, AEOI, no slave
                   "irq 4 1", "ack", "ack", "irq 4 0", "ack", "state"])
        self.assertEqual([l for l in out if l.startswith("A ")], ["A -1", "A c", "A c", "A -1"])
        self.assertEqual(state(out[-1])["isr0"], "0")

    def test_pit(self):
        cpu, pit = 10_000_000, 2_457_600

        def cycles_for(ticks):
            return -(-ticks * cpu // pit)
        out = run(["init v30 10"] + Devices.PIC_INIT + [
            "out 77 34", "out 71 e8", "out 71 3",               # counter 0: mode 2, 1000
            "adv %d" % cycles_for(999), "out 0 a", "in 0",      # 999 ticks: no IRQ yet
            "out 77 0", "in 71", "in 71",                       # latch: count 1
            "adv %d" % (cycles_for(1000) - cycles_for(999)), "out 0 a", "in 0", "ack", "out 0 20",
            "adv %d" % (cycles_for(3500) - cycles_for(1000)), "ack", "out 0 20",   # periodic: more edges
            "out 77 b0", "out 75 10", "out 75 0",               # counter 2: mode 0, 16
            "out 77 80", "in 75", "in 75",
            "out 77 50", "out 73 64",                           # counter 1: LSB only, mode 0, 100
            "out 77 40", "in 73", "in 73",
            "out 77 35", "out 71 0", "out 71 10",               # counter 0: BCD 1000 -> 1000 ticks
            "out 77 0", "in 71", "in 71", "state",
        ])
        ins = [l.split()[1] for l in out if l.startswith("I ")]
        self.assertEqual(ins[0], "0", "IRR before the terminal count")
        self.assertEqual(ins[1:3], ["1", "0"], "latched count 1")
        self.assertEqual(ins[3], "1", "IR0 requested")
        self.assertEqual([l for l in out if l.startswith("A ")], ["A 8", "A 8"])
        self.assertEqual(ins[4:6], ["10", "0"], "mode 0 count 16, nothing elapsed")
        self.assertEqual(ins[6:8], ["64", "64"], "LSB-only access reads the low byte twice")
        self.assertEqual(ins[8:10], ["e8", "3"], "BCD 1000 counts in binary here")
        self.assertEqual(state(out[-1])["pit_hz"], "2457600")
        out = run(["init v30 8", "state"])
        self.assertEqual(state(out[-1])["pit_hz"], "1996800", "8 MHz machines clock the timer at 1.9968 MHz")

    def test_gdc_and_vsync(self):
        frame = 10_000_000 * 100 // 5642
        vs = frame // 11
        out = run(["init v30 10"] + Devices.PIC_INIT + [
            "in 60", "adv %d" % vs, "in 60", "state",
            "out 62 c", "state", "out 62 d",
            "out 62 49", "out 60 34", "out 60 12", "out 60 1", "out 62 e0",
            "in 60", "in 62", "in 62", "in 62", "in 62", "in 62", "in 60",
            "out a2 6b", "out a2 47", "out a0 28", "out a2 20", "out a0 ff", "state",
            "out 64 0", "adv %d" % (frame - vs), "out 0 a", "in 0", "ack", "out 0 20",
            "adv %d" % frame, "out 0 a", "in 0", "state",
        ])
        ins = [l.split()[1] for l in out if l.startswith("I ")]
        self.assertEqual(ins[0], "24", "FIFO empty and VSYNC at power-on")
        self.assertEqual(ins[1], "4", "VSYNC over")
        sts = [state(l) for l in out if l.startswith("Z ")]
        self.assertEqual(sts[0]["gdc0_on"], "1")
        self.assertEqual(sts[1]["gdc0_on"], "0", "BCTRL 0Ch blanks")
        self.assertEqual(ins[2], "5", "CSRR answer ready")
        self.assertEqual(ins[3:8], ["34", "12", "1", "0", "0"])
        self.assertEqual(ins[8], "4")
        self.assertEqual((sts[2]["gdc1_on"], sts[2]["pitch1"], sts[2]["gdc1_ignored"], sts[2]["ead0"]), ("1", "28", "1", "11234"))
        self.assertEqual(ins[9], "4", "the armed VSYNC interrupt (IR2)")
        self.assertIn("A a", out)
        self.assertEqual(ins[10], "0", "not armed again: no second interrupt")
        self.assertEqual(sts[3]["frames"], "2")

    def test_keyboard(self):
        out = run(["init v30 10"] + Devices.PIC_INIT + [
            "in 43", "key 1c 1", "key 1c 0", "key 70 1", "in 43", "ack",
            "in 41", "in 43", "out 0 20", "adv 10", "ack", "in 41", "out 0 20", "adv 10", "in 41", "in 43",
            "out 43 0", "out 43 40", "out 43 4e", "out 43 37", "out 41 9d", "state",
        ])
        ins = [l.split()[1] for l in out if l.startswith("I ")]
        self.assertEqual(ins[0], "85", "nothing received")
        self.assertEqual(ins[1], "87", "RxRDY")
        self.assertEqual(ins[2:4], ["1c", "87"], "make code; the next byte is already waiting")
        self.assertEqual(ins[4:7], ["9c", "70", "85"], "break code = make | 80h, then the next key")
        self.assertEqual([l for l in out if l.startswith("A ")], ["A 9", "A 9"])
        self.assertEqual(state(out[-1])["to_kbd"], "1")

    def test_beeper_a20_pages_ports(self):
        out = run(["init 286 10", "state", "out 37 6", "state", "out 37 7",
                   "out 3fdf 76", "out 3fdb cd", "out 3fdb 4", "out 35 0", "state", "out 35 8",
                   "out f2 0", "in f2", "state", "out f6 3", "in f2", "state",
                   "out a6 1", "wr a8000 5a", "rd a8000", "out a6 0", "rd a8000", "wr e0010 33", "rd e0010",
                   "rd e8000", "rd ffff0", "rd c0000", "rd a4000", "wr 9ffff 77", "rd 9ffff", "rd a0000", "rd a2000",
                   "in 7c", "out 7e 1", "out 5f 0", "out 68 f", "out 6a 1", "out a4 1", "state"])
        sts = [state(l) for l in out if l.startswith("Z ")]
        self.assertEqual(sts[0]["beep_on"], "0")
        self.assertEqual(sts[1]["beep_on"], "1", "port C bit 3 low: buzzer on")
        self.assertEqual((sts[2]["beep_on"], sts[2]["beep_hz"], sts[2]["beep_changes"]), ("1", str(2457600 // 0x4CD), "3"))
        self.assertEqual((sts[3]["a20"], sts[3]["addr_mask"]), ("1", "ffffff"))
        self.assertEqual((sts[4]["a20"], sts[4]["addr_mask"]), ("0", "fffff"))
        ins = [l.split()[1] for l in out if l.startswith("I ")]
        self.assertEqual(ins[:2], ["0", "1"])
        mems = [l.split()[1] for l in out if l.startswith("M ")]
        self.assertEqual(mems, ["5a", "0", "33", "ff", "ff", "ff", "ff", "77", "20", "e1"])
        st = sts[5]
        self.assertEqual((st["unhandled"], st["last_unhandled"]), ("2", "7e"))
        self.assertEqual((st["mode_ff1"], st["analog"], st["disp_page"], st["draw_page"]), ("80", "1", "1", "0"))

    def test_render(self):
        with tempfile.TemporaryDirectory() as d:
            pic = Path(d) / "pic.bin"
            out = run(["init v30 10", "out a2 d",
                       "wr a8000 80", "wr b0000 80", "wr b8000 80",     # pixel (0,0): B+R+G = 7, white
                       "wr b0000 c0",                                   # pixel (1,0): R only = 2, red (B, G planes clear)
                       "wr a8001 1", "wr b8001 1",                      # pixel (15,0): B+G = 5, cyan
                       "wr a2000 0", "wr a2002 0",                      # hide the text cells over those pixels
                       "wr a00a0 41", "wr a20a0 41",                    # text cell (0,1): 'A', red, visible
                       "render %s" % pic,
                       "out 6a 1", "out a8 5", "out aa 3", "out ac 9", "out ae f",   # analog: colour 5 = R9 G3 BF
                       "render %s.2" % pic])
            px = struct.unpack("<%dI" % (640 * 400), pic.read_bytes())
            self.assertEqual(px[0], 0xFFFFFF)
            self.assertEqual(px[1], 0xFF0000)
            self.assertEqual(px[15], 0x00FFFF)
            self.assertEqual(px[2], 0)
            # the synthetic glyph: row y of 'A' is 41h + y; bit 7 is the left pixel
            for y in range(16):
                bits = (0x41 + y) & 0xFF
                for x in range(8):
                    want = 0xFF0000 if bits & (0x80 >> x) else 0
                    self.assertEqual(px[(16 + y) * 640 + x], want, (x, y))
            px2 = struct.unpack("<%dI" % (640 * 400), Path(str(pic) + ".2").read_bytes())
            self.assertEqual(px2[15], 0x9933FF)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Boot(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def boot(self, path, hint, daua, model="v30", mhz=10, cycles=50_000_000):
        return run(["init %s %d" % (model, mhz), "disk %s %s" % (path, hint), "boot %x" % daua,
                    "run %d" % cycles, "text", "state", "regs"])

    def test_tiny_boot_sector_from_every_format(self):
        images = [
            ("tiny.d88", standard_d88(TINY_BOOT), "d88", 0x90),
            ("tiny.fdi", anex("fdi", 77, 2, 8, 1024, first=TINY_BOOT), "fdi", 0x90),
            ("tiny.hdi", anex("hdi", 20, 4, 17, 512, first=TINY_BOOT), "hdi", 0x80),
            ("tiny.hdm", TINY_BOOT + bytes(77 * 2 * 8 * 1024 - len(TINY_BOOT)), "none", 0x90),
        ]
        for name, data, hint, daua in images:
            for model in ("8086", "v30", "286"):
                with self.subTest(image=name, model=model):
                    out = self.boot(self.img.put(name, data), hint, daua, model=model)
                    self.assertTrue(out[0].startswith("F 0 "), out[0])
                    self.assertEqual(out[1], "B 0")
                    r = out[2].split()
                    self.assertEqual(r[2:5], ["1", "1", "0"], "dead (CLI; HLT), halted, not stopped: " + out[2])
                    scr = screen(out)
                    self.assertEqual(len(scr), 25)
                    self.assertEqual(scr[0].rstrip(), "TOUHOU OS: SHRINE BOOT OK")
                    self.assertTrue(all(not l.strip() for l in scr[1:]))

    def test_tiny_boot_matches_its_source(self):
        if not shutil.which("nasm"):
            self.skipTest("needs nasm")
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(["nasm", "-f", "bin", "-o", str(OUT / "tiny.bin"), str(HERE / "pc98boot" / "tiny.asm")], check=True)
        self.assertEqual((OUT / "tiny.bin").read_bytes(), TINY_BOOT)

    def test_no_boot_sector(self):
        out = run(["init v30 10", "disk %s d88" % self.img.put("e.d88", d88({})), "boot 90"])
        self.assertEqual(out[1], "B -1")

    def test_hello_demo(self):
        if not shutil.which("nasm"):
            self.skipTest("needs nasm")
        OUT.mkdir(parents=True, exist_ok=True)
        binary = OUT / "hello.bin"
        subprocess.run(["nasm", "-f", "bin", "-o", str(binary), str(HERE / "pc98boot" / "hello.asm")], check=True)
        prog = binary.read_bytes()
        self.assertEqual(len(prog), 512)
        cases = [("hello.d88", standard_d88(prog), "d88", 0x90, "v30"),
                 ("hello.hdi", anex("hdi", 20, 4, 17, 512, first=prog), "hdi", 0x80, "286"),
                 ("hello.fdi", anex("fdi", 77, 2, 8, 1024, first=prog), "fdi", 0x91, "8086")]
        for name, data, hint, daua, model in cases:
            with self.subTest(image=name, model=model):
                out = self.boot(self.img.put(name, data), hint, daua, model=model)
                r = out[2].split()
                self.assertEqual(r[2:5], ["1", "1", "0"], out[2])
                scr = [l.rstrip() for l in screen(out)]
                self.assertEqual(scr[0], "Hakurei Shrine boot sector, no ROM inside")
                self.assertEqual(scr[1], "boot device (DA/UA): %02X" % daua)
                self.assertEqual(scr[2], "three VSYNCs seen on the text GDC")
                self.assertEqual(scr[3], "timer interrupts counted: 05")
                self.assertEqual(scr[4], "the beeper sang at 2 kHz")
                self.assertEqual(scr[5], "GDC cursor read back: 1234")
                self.assertEqual(scr[6], "")
                self.assertEqual(scr[7], "The barrier holds. Halting.")
                st = state(next(l for l in out if l.startswith("Z ")))
                self.assertEqual(st["beep_changes"], "2")
                self.assertEqual(st["beep_hz"], str(2457600 // 1229))
                self.assertGreaterEqual(int(st["frames"]), 3)
                self.assertEqual(st["unhandled"], "0")
                self.assertEqual((st["imr0"], st["isr0"]), ("fe", "0"))
                # five timer periods of 10 ms and three frames of 17.7 ms: the run took at least 50 ms of machine time
                self.assertGreater(int(st["cycles"]), 500_000)


if __name__ == "__main__":
    unittest.main()
