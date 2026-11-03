#!/usr/bin/env python3
"""Host tests for the HLE BIOS of the PC-98 machine (user/lib/pc98/src/pc98bios.c, docs/PC98_BIOS.md), through the
headless harness tests/host/pc98run.c.

Synthetic tests: every program was written for these tests (tests/host/pc98boot/bios*.asm, assembled with nasm when it
is installed, and a few short ones assembled by hand below) and every disk image is built here. Nothing from a real
PC-98 is used. The OwnerDisks class boots the owner's own disk images when they are present (assets/local, git-ignored,
or TOUHOU_PC98_DISKS) and is skipped otherwise; it checks behaviour (what the BIOS was asked, where the boot got to),
not the contents of the owner's files.

    python3 -m unittest discover -s tests/host -p "test_pc98bios.py" -v
    python tools/hosttest.py discover -s tests/host -p "test_pc98bios.py" -v       (Windows: runs in WSL)
"""
import importlib.util
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402
from test_pc98 import Images, pattern, standard_d88  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = x86ref.REPO
ASM = HERE / "pc98boot"


def harness():
    return x86ref.build("pc98run", [HERE / "pc98run.c", REPO / "shared" / "font8x16.c"])


def run(args, timeout=600):
    r = subprocess.run([str(harness()), *map(str, args)], capture_output=True, text=True, errors="replace",
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"), timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError("pc98run failed (%d): %s\n%s" % (r.returncode, r.stderr[-3000:], r.stdout[-3000:]))
    return r.stdout.splitlines()


def end(lines):
    line = next(l for l in lines if l.startswith("END "))
    return dict(kv.split("=", 1) for kv in line[4:].split() if "=" in kv)


def state(lines):
    line = next(l for l in lines if l.startswith("STATE "))
    return dict(kv.split("=", 1) for kv in line[6:].split())


def memory(lines):
    """The MEM lines of --dump: {address: byte}."""
    mem = {}
    for l in lines:
        if l.startswith("MEM "):
            addr, data = l[4:].split(":", 1)
            for i, b in enumerate(data.split()):
                mem[int(addr, 16) + i] = int(b, 16)
    return mem


def word(mem, a):
    return mem[a] | mem[a + 1] << 8


def screen(lines):
    return [l[1:] for l in lines if l.startswith("|")]


def assemble(name):
    """Assembles tests/host/pc98boot/NAME.asm into build/, or skips the test without nasm."""
    nasm = x86ref.find_nasm()
    if not nasm:
        raise unittest.SkipTest("needs nasm")
    out = REPO / "build" / "host-pc98" / (name + ".bin")
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([nasm, "-f", "bin", "-o", str(out), str(ASM / (name + ".asm"))], check=True)
    return out.read_bytes()


def hdi(boot, cyls=20, heads=4, spt=17, ssz=512):
    """An Anex86 HDI whose sector n starts with the word n (filled with n & FFh); sector 0 holds the boot program."""
    data = bytearray()
    for n in range(cyls * heads * spt):
        data += struct.pack("<H", n) + bytes([n & 0xFF]) * (ssz - 2)
    data[:len(boot)] = boot
    head = struct.pack("<8I", 0, 0, 4096, len(data), ssz, spt, heads, cyls)
    return head + bytes(4096 - len(head)) + bytes(data)


# ---- programs assembled by hand (they run without nasm)

# Reports the hand-over: [0600] = [0584h] (the DA/UA), [0602] = DS, [0604] = CS, [0606] = SP; then returns to the BIOS.
BOOT_REPORT = bytes([
    0xA0, 0x84, 0x05,           # mov al, [0584h]       (DS = 0)
    0xA2, 0x00, 0x06,           # mov [0600h], al
    0x8C, 0x1E, 0x02, 0x06,     # mov [0602h], ds
    0x8C, 0x0E, 0x04, 0x06,     # mov [0604h], cs
    0x89, 0x26, 0x06, 0x06,     # mov [0606h], sp
    0xB9, 0xEF, 0xBE,           # mov cx, 0BEEFh
    0xCB,                       # retf                  (back to the BIOS: "the boot program returned")
])

# Reads two sectors (C0 H0 S1 and S2) to 2000:0000 with INT 1Bh, stores AH at [0600], halts.
BOOT_READ = bytes([
    0xB8, 0x00, 0x20,           # mov ax, 2000h
    0x8E, 0xC0,                 # mov es, ax
    0x31, 0xED,                 # xor bp, bp
    0xB8, 0x80, 0x06,           # mov ax, 0680h         (read data, first hard disk)
    0xBB, 0x00, 0x04,           # mov bx, 0400h         (two 512-byte sectors)
    0x31, 0xC9,                 # xor cx, cx            (cylinder 0)
    0xBA, 0x01, 0x00,           # mov dx, 0001h         (head 0, sector 1: sectors count from 0 on a hard disk)
    0xCD, 0x1B,                 # int 1Bh
    0x88, 0x26, 0x00, 0x06,     # mov [0600h], ah
    0xFA, 0xF4,                 # cli; hlt
])

# Calls the INT 1Eh entry IO.SYS writes into the vector (FD80:27EC): the BIOS stops the CPU ("reboot requested").
BOOT_INT1E = bytes([0x9A, 0xEC, 0x27, 0x80, 0xFD, 0xFA, 0xF4])       # call far FD80:27ECh; cli; hlt

# Spins about 3.3 s of 10 MHz machine time (30 x 65536 LOOPs of 17 clocks), then reads the calendar to 0000:0600.
BOOT_WAIT_CLOCK = bytes([
    0xBA, 0x1E, 0x00,           # mov dx, 30
    0x31, 0xC9,                 # l1: xor cx, cx
    0xE2, 0xFE,                 # loop $
    0x4A,                       # dec dx
    0x75, 0xF9,                 # jnz l1
    0x1E, 0x07,                 # push ds; pop es       (ES = 0)
    0xBB, 0x00, 0x06,           # mov bx, 0600h
    0xB4, 0x00,                 # mov ah, 00h
    0xCD, 0x1C,                 # int 1Ch
    0xFA, 0xF4,                 # cli; hlt
])


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class HandOver(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def test_boot_convention(self):
        """The first sector at 1FC0:0000, entered by a far call with DS = 0 and [0584h] = DA/UA; RETF ends the boot."""
        for model in ("8086", "v30", "286", "386", "486"):
            with self.subTest(model=model):
                p = self.img.put("r.hdi", hdi(BOOT_REPORT))
                out = run(["--disk", p, "--cpu", model, "--cycles", "1", "--dump", "600:8"])
                mem = memory(out)
                self.assertEqual(mem[0x600], 0x80)
                self.assertEqual(word(mem, 0x602), 0x0000, "DS = 0")
                self.assertEqual(word(mem, 0x604), 0x1FC0, "CS = 1FC0h")
                self.assertEqual(word(mem, 0x606), 0x7C00 - 4, "the BIOS stack, below the far call's return address")
                e = end(out)
                self.assertEqual((e["reason"], e["stop"]), ("stop", "128"), out[-1])
                self.assertTrue(any(l.startswith("NOTE boot program returned to the BIOS (BEEF)") for l in out), out)

    def test_read_without_nasm(self):
        p = self.img.put("d.hdi", hdi(BOOT_READ))
        out = run(["--disk", p, "--cycles", "1", "--dump", "600:1", "--dump", "20000:4", "--dump", "20200:4"])
        mem = memory(out)
        self.assertEqual(mem[0x600], 0x00, "AH = 00h: success")
        self.assertEqual(word(mem, 0x20000), 1)
        self.assertEqual(word(mem, 0x20200), 2)
        self.assertEqual(end(out)["reason"], "dead")

    def test_int1e_entry_is_a_reboot(self):
        p = self.img.put("e.hdi", hdi(BOOT_INT1E))
        out = run(["--disk", p, "--cycles", "1"])
        e = end(out)
        self.assertEqual((e["reason"], e["stop"]), ("stop", "131"))
        self.assertTrue(any(l.startswith("NOTE reboot requested") for l in out))

    def test_no_disk_no_boot(self):
        p = self.img.put("empty.hdi", hdi(b"", cyls=1)[:4096])     # a header and no sectors
        r = subprocess.run([str(harness()), "--disk", str(p)], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Disk(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def run_disk(self, *extra):
        p = self.img.put("disk.hdi", hdi(assemble("biosdisk")))
        return run(["--disk", p, "--cycles", "5", "--dump", "600:44", "--dump", "20000:4", "--dump", "20200:4",
                    "--dump", "20400:4", "--dump", "20600:4", "--dump", "20800:4", "--dump", "20A00:4",
                    "--dump", "20C00:4", "--dump", "20E00:4", "--dump", "21000:4", *extra])

    def test_hard_disk_calls(self):
        out = self.run_disk()
        mem = memory(out)
        pairs = lambda a: (mem[a], mem[a + 1])
        self.assertEqual((mem[0x600], mem[0x601]), (0x80, 0x80), "AL and [0584h] = the DA/UA")
        self.assertEqual(word(mem, 0x602), 0)
        self.assertEqual(word(mem, 0x608), 0x1FC0)
        self.assertEqual(pairs(0x610), (0, 0), "new sense")
        self.assertEqual((word(mem, 0x612), word(mem, 0x614), word(mem, 0x616)), (512, 20, 0x0411),
                         "BX = sector size, CX = cylinders, DH = heads, DL = sectors")
        for a, what in ((0x618, "sense"), (0x61A, "8Eh"), (0x61C, "initialise"), (0x61E, "recalibrate"),
                        (0x620, "read"), (0x622, "read across a cylinder"), (0x624, "relative read"),
                        (0x626, "write"), (0x628, "read back"), (0x62A, "verify")):
            self.assertEqual(pairs(a), (0, 0), what)
        self.assertEqual([word(mem, a) for a in (0x20000, 0x20200, 0x20400)], [15, 16, 17], "on to the next head")
        self.assertEqual([word(mem, a) for a in (0x20600, 0x20800)], [67, 68], "on to the next cylinder")
        self.assertEqual([word(mem, a) for a in (0x20A00, 0x20C00)], [300, 301], "DX:CX = sector number")
        self.assertEqual(word(mem, 0x20E00), 0x5A5A, "the written sector reads back")
        self.assertEqual(pairs(0x62C), (0x60, 1), "no such unit: not ready")
        self.assertEqual(pairs(0x62E), (0xC0, 1), "beyond the disk: no data")
        self.assertEqual(pairs(0x630), (0x40, 1), "a command the BIOS does not have")
        self.assertEqual([word(mem, a) for a in range(0x632, 0x63E, 2)], [512, 2, 0x0102, 0x1000, 0x1234, 0x2000],
                         "BX CX DX BP SI ES kept")
        self.assertEqual(word(mem, 0x21000), (2 * 4 + 1) * 17 + 2, "C2 H1 S2")
        self.assertEqual(word(mem, 0x63E), 512, "new sense through DA/UA 00h")
        self.assertEqual(pairs(0x640), (0, 0), "0Fh, which the STOP-key handler of IO.SYS calls")
        self.assertTrue(any(l.startswith("BIOS-UNKNOWN 1B AH=0D") for l in out), "the unknown call is reported")
        e = end(out)
        self.assertEqual((e["reason"], e["stop"]), ("stop", "128"))
        self.assertTrue(any("boot program returned to the BIOS (B007)" in l for l in out))

    def test_write_protected(self):
        mem = memory(self.run_disk("--readonly"))
        self.assertEqual((mem[0x626], mem[0x627]), (0x70, 1), "write protected")
        self.assertEqual(word(mem, 0x20E00), 68, "the sector is unchanged")

    def test_floppy(self):
        boot = assemble("biosfd")
        self.assertEqual(len(boot), 1024)
        p = self.img.put("fd.d88", standard_d88(boot, cyls=2))
        out = run(["--disk", p, "--cycles", "2", "--dump", "600:10", "--dump", "20000:10", "--dump", "20400:10"])
        mem = memory(out)
        self.assertEqual((mem[0x600], mem[0x601]), (0x90, 0x90))
        self.assertEqual((mem[0x602], mem[0x603]), (0, 0), "multi-track read")
        self.assertEqual(bytes(mem[0x20000 + i] for i in range(16)), pattern(0 * 100 + 0 * 10 + 8, 1024)[:16], "C0 H0 R8")
        self.assertEqual(bytes(mem[0x20400 + i] for i in range(16)), pattern(0 * 100 + 1 * 10 + 1, 1024)[:16], "C0 H1 R1")
        self.assertEqual((mem[0x604], mem[0x605]), (0, 0), "sense")
        self.assertEqual((mem[0x606], mem[0x607]), (0xC0, 1), "R9 is not there")
        self.assertEqual(word(mem, 0x608), 0xA55A, "the whole 1024-byte boot sector was loaded")


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class KeyboardCrtTimer(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def test_keyboard(self):
        p = self.img.put("k.hdi", hdi(assemble("bioskey")))
        out = run(["--disk", p, "--cycles", "200", "--until-key-wait", "--keys", "aA^c\\r1!z" + "x" * 20,
                   "--dump", "600:22", "--state"])
        self.assertEqual(end(out)["reason"], "key-wait", out[-3:])
        mem = memory(out)
        keys = [(word(mem, 0x600 + 4 * i), mem[0x602 + 4 * i], mem[0x603 + 4 * i]) for i in range(6)]
        self.assertEqual(keys, [(0x1D61, 0, 0), (0x1D41, 1, 1), (0x2B03, 0x10, 0x10), (0x1C0D, 0, 0),
                                (0x0131, 0, 0), (0x0121, 1, 1)],
                         "scan code << 8 | character; SHIFT and CTRL in the shift state and in key group 0Eh")
        self.assertEqual((mem[0x618], mem[0x619]), (0, 0), "sense and read-without-waiting see an empty buffer")
        self.assertEqual(word(mem, 0x61A), 0x297A, "z: key 29h, the first of the bottom row")
        self.assertEqual(word(mem, 0x61C), 16, "the buffer holds 16 keys")
        self.assertEqual(word(mem, 0x61E), 0x2A78, "x")
        self.assertEqual(mem[0x620], 0, "a stray INT 09h with no byte waiting adds no key")
        st = state(out)
        self.assertEqual((st["kb_count"], st["kb_retry"]), ("0", "4"), "four keys did not fit")

    def test_crt(self):
        p = self.img.put("c.hdi", hdi(assemble("bioscrt")))
        out = run(["--disk", p, "--cycles", "2", "--dump", "600:5", "--dump", "A2000:2", "--dump", "A3FE2:1",
                   "--dump-text", "--state"])
        mem = memory(out)
        self.assertEqual((mem[0x600], mem[0x601]), (3, 0), "CRT mode set and read back")
        self.assertEqual(mem[0x602] & 8, 0, "beeper on: port C bit 3 low")
        self.assertEqual(mem[0x603] & 8, 8, "beeper off")
        self.assertEqual(mem[0x604], 1, "an unknown function sets CF")
        self.assertEqual(mem[0xA2000], 0x81, "the fill's attribute")
        self.assertEqual(mem[0xA3FE2], 0x48, "the fill leaves the memory switches alone")
        self.assertTrue(all(l == "*" * 80 for l in screen(out)))
        st = state(out)
        self.assertEqual((st["text_on"], st["gfx_on"], st["ead0"]), ("1", "1", "A5"), "cursor at row 2, column 5")
        self.assertEqual((st["cursor_shown"], st["cursor_blink"], st["crt_mode"], st["gfx_mode"], st["beep_on"]),
                         ("1", "0", "0", "C0", "0"))
        self.assertTrue(any(l.startswith("BIOS-UNKNOWN 18 AH=7F") for l in out))

    def test_calendar_and_interval_timer(self):
        p = self.img.put("t.hdi", hdi(assemble("biostime")))
        out = run(["--disk", p, "--cycles", "5", "--clock", "261102120000", "--dump", "600:14"])
        mem = memory(out)
        self.assertEqual(bytes(mem[0x600 + i] for i in range(6)), bytes([0x26, 0xB1, 0x02, 0x12, 0x00, 0x00]),
                         "2026-11-02 (a Monday: weekday 1, November = Bh) 12:00:00")
        self.assertEqual(bytes(mem[0x608 + i] for i in range(6)), bytes([0x01, 0x26, 0x03, 0x04, 0x05, 0x06]),
                         "set to 2001-02-03 04:05:06 (a Saturday) and read back")
        self.assertEqual(word(mem, 0x610), 3, "three timer interrupts of 10 ms")
        self.assertEqual(mem[0x612], 1, "the routine ran once")
        self.assertEqual(mem[0x613] & 1, 1, "IRQ 0 masked again")

    def test_clock_runs_with_machine_time(self):
        """The calendar advances with the machine's cycles: 23:59:58 on New Year's Eve plus 3.3 s is the next year."""
        p = self.img.put("w.hdi", hdi(BOOT_WAIT_CLOCK))
        out = run(["--disk", p, "--cycles", "40", "--clock", "991231235958", "--dump", "600:6"])
        mem = memory(out)
        self.assertEqual(bytes(mem[0x600 + i] for i in range(6)), bytes([0x00, 0x16, 0x01, 0x00, 0x00, 0x01]),
                         "(20)00-01-01, a Saturday (weekday 6), 00:00:01")


# Reads the 'A' pattern twice: through INT 18h AH=14h into 0600h, and line by line through the character generator ports
# (A1h code, A3h = 0, A5h line, A9h pattern) into 06C0h. The BIOS takes the buffer in BX:CX (segment, offset), the way TH01's
# own wrapper passes it. DS stays 0 (the boot hand-over); the buffer is 0060:0000 (= 0600h), so that a BIOS reading it
# through DS:BX would write 0000:0060 instead and the test would see nothing at 0600h.
BOOT_FONT_ANK = bytes([
    0xB4, 0x14, 0xBB, 0x60, 0x00, 0x31, 0xC9, 0xBA, 0x41, 0x00, 0xCD, 0x18,     # AH=14h, BX = 0060h, CX = 0, DX = 0041h
    0xB0, 0x41, 0xE6, 0xA1, 0x30, 0xC0, 0xE6, 0xA3, 0x31, 0xF6,                 # A1h = 41h, A3h = 0, SI = 0
    0x89, 0xF0, 0xE6, 0xA5, 0xE4, 0xA9, 0x88, 0x84, 0xC0, 0x06,                 # l: AL = SI; A5h = line; AL = A9h; [SI+06C0h] = AL
    0x46, 0x83, 0xFE, 0x10, 0x72, 0xF0,                                         # inc si; cmp si, 16; jb l
    0xFA, 0xF4,
])


def font_calls_program():
    """Reads JIS 2121h (full width) into 0600h and 2B21h (the half-width part of the kanji set) into 0640h, defines the user
    character 7621h from 34 bytes in the boot sector (INT 18h AH=1Ah) and reads it back into 0680h. The buffer is BX:CX in
    every call (the reads: BX = 60h, 64h, 68h and CX = 0, i.e. 0600h, 0640h, 0680h; the definition: BX = CS, CX the data's
    offset) while DS = CS, so a BIOS that took DS:BX would miss all four."""
    pattern = bytes([2, 2]) + b"".join(bytes([0x30 + i, 0x70 + i]) for i in range(16))      # header, then (left, right) per row

    def code(data_off):
        return bytes([
            0x0E, 0x1F,                                                                 # DS = CS
            0xB4, 0x14, 0xBB, 0x60, 0x00, 0x31, 0xC9, 0xBA, 0x21, 0x21, 0xCD, 0x18,
            0xB4, 0x14, 0xBB, 0x64, 0x00, 0x31, 0xC9, 0xBA, 0x21, 0x2B, 0xCD, 0x18,
            0x0E, 0x5B, 0xB4, 0x1A, 0xB9, data_off & 0xFF, data_off >> 8, 0xBA, 0x21, 0x76, 0xCD, 0x18,   # BX = CS; define
            0xB4, 0x14, 0xBB, 0x68, 0x00, 0x31, 0xC9, 0xBA, 0x21, 0x76, 0xCD, 0x18,
            0xB4, 0x14, 0xBB, 0x6C, 0x00, 0x31, 0xC9, 0xBA, 0x41, 0x80, 0xCD, 0x18,      # DX = 8041h: 0600h + 0C0h
            0xFA, 0xF4,
        ])
    return code(len(code(0))) + pattern, pattern


def synthetic_font_rom():
    """A 288,768-byte image in the layout pc98gfx reads (made up for the test): 8x16 ANK at 0800h ('A' = 10h + row), kanji from
    1800h in cells of 32 bytes (JIS 2121h: left half 40h + row, right half C0h + row; 2B21h: 20h + row, then blank)."""
    rom = bytearray(288768)
    rom[0x800 + 0x41 * 16:0x800 + 0x41 * 16 + 16] = bytes(0x10 + i for i in range(16))
    off = 0x1800 + ((0x21 - 0x21) * 96 + (0x21 - 0x20)) * 32
    rom[off:off + 32] = bytes(0x40 + i for i in range(16)) + bytes(0xC0 + i for i in range(16))
    off = 0x1800 + ((0x2B - 0x21) * 96 + (0x21 - 0x20)) * 32
    rom[off:off + 16] = bytes(0x20 + i for i in range(16))
    return bytes(rom)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class FontCalls(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def test_font_read_matches_the_character_generator(self):
        """INT 18h AH=14h on an ANK code: the header is the height first (2), then the width (1), in units of 8 dots (TH01's
        text routine loops over the first and takes the second as the width), then the 16 rows the character generator
        ports give for the same code (the repository's own font stands in for the ROM)."""
        p = self.img.put("a.hdi", hdi(BOOT_FONT_ANK))
        mem = memory(run(["--disk", p, "--cycles", "2", "--dump", "600:12", "--dump", "6C0:10"]))
        glyph = bytes(mem[0x6C0 + i] for i in range(16))
        self.assertTrue(any(glyph), "the repository's font draws an A")
        self.assertEqual((mem[0x600], mem[0x601]), (2, 1))
        self.assertEqual(bytes(mem[0x602 + i] for i in range(16)), glyph)

    def test_font_read_full_width_half_width_and_user_characters(self):
        """A full-width pattern comes back as (left, right) per row after the header 2, 2; the half-width kanji-set rows 29h-2Bh
        as one byte per row after 2, 1 (height, width); DH = 80h is a half-width ANK character, 18 bytes and no more; a user
        character defined with AH=1Ah reads back unchanged."""
        boot, pattern = font_calls_program()
        p = self.img.put("k.hdi", hdi(boot))
        rom = self.img.put("FONT.ROM", synthetic_font_rom())
        mem = memory(run(["--disk", p, "--cycles", "2", "--font", rom, "--dump", "600:22", "--dump", "640:12",
                          "--dump", "680:22", "--dump", "6C0:30"]))
        got = lambda a, n: bytes(mem[a + i] for i in range(n))
        # DH = 80h is the half-width ANK character DL (TH01 writes "STAGE01" with it, into 18-byte buffers): header 2, 1, then the
        # 16 rows of 'A', and nothing after them. Answered as a full-width character (34 bytes) it ran over the game's saved
        # return address, the function returned to 0000:0000 and the game exited through its own INT 06h handler.
        self.assertEqual(got(0x6C0, 18), bytes([2, 1]) + bytes(0x10 + i for i in range(16)))
        self.assertEqual(got(0x6D2, 30), bytes(30), "no byte beyond the 18 of a half-width glyph")
        self.assertEqual(got(0x600, 34), bytes([2, 2]) + b"".join(bytes([0x40 + i, 0xC0 + i]) for i in range(16)))
        self.assertEqual(got(0x640, 18), bytes([2, 1]) + bytes(0x20 + i for i in range(16)))
        self.assertEqual(got(0x680, 34), pattern)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class WorkAreaAndTraps(unittest.TestCase):
    def setUp(self):
        self.img = Images()

    def tearDown(self):
        self.img.close()

    def test_work_area_vectors_switches(self):
        p = self.img.put("m.hdi", hdi(assemble("biosmisc")))
        out = run(["--disk", p, "--cpu", "v30", "--cycles", "2", "--dump", "600:1C", "--dump", "53B:1"])
        mem = memory(out)
        self.assertEqual(mem[0x53B], 0x0F, "text rows are 16 rasters: TH01 builds its CSRFORM from this byte (lines - 1)")
        self.assertEqual(mem[0x601] & 7, 4, "640 KiB: (4 + 1) x 128 KiB")
        self.assertEqual(mem[0x601] & 0x08, 0, "not a high-resolution machine")
        self.assertEqual(word(mem, 0x602), 0x0100, "one hard disk, no floppy")
        self.assertEqual(mem[0x604], 0x80)
        self.assertEqual((word(mem, 0x606), word(mem, 0x608), mem[0x60A]), (0x502, 0x502, 0), "empty keyboard buffer")
        self.assertEqual(mem[0x60B], 0, "no extended memory")
        self.assertEqual((word(mem, 0x60C), word(mem, 0x60E), word(mem, 0x610)), (0x18 * 4, 0xFD80, 0xFD80))
        self.assertEqual(bytes(mem[0x612 + i] for i in range(8)), bytes([0x48, 0x05, 0x04, 0x00, 0x01, 0x00, 0x00, 0x6E]))
        self.assertEqual(mem[0x61A], 1, "INT 19h: not implemented, CF set")
        self.assertEqual(mem[0x61B], 0x22, "the V30 goes on after a divide error")
        self.assertTrue(any(l.startswith("NOTE CPU exception (0)") for l in out))
        self.assertTrue(any(l.startswith("NOTE call to an unknown ROM entry point (1234)") for l in out))
        self.assertEqual(end(out)["stop"], "129")

    def test_fault_loop_on_the_286(self):
        p = self.img.put("m.hdi", hdi(assemble("biosmisc")))
        out = run(["--disk", p, "--cpu", "286", "--cycles", "5", "--dump", "61B:1"])
        self.assertEqual(memory(out)[0x61B], 0x11)
        self.assertEqual(end(out)["stop"], "130", "the 286 returns to the divide: the BIOS stops the loop")


# ---- the owner's disks (never in the tree; skipped when absent)

def owner_disks():
    cands = []
    if os.environ.get("TOUHOU_PC98_DISKS"):
        cands.append(Path(os.environ["TOUHOU_PC98_DISKS"]))
    cands.append(REPO / "assets" / "local" / "(TH01-05)" / "disks")
    if ".claude" in REPO.parts:                    # an agent worktree: assets/local lives in the main checkout
        main = Path(*REPO.parts[:REPO.parts.index(".claude")])
        cands.append(main / "assets" / "local" / "(TH01-05)" / "disks")
    for c in cands:
        if (c / "th01.hdi").is_file():
            return c
    return None


DISKS = owner_disks()


def load_pc98run_tool():
    spec = importlib.util.spec_from_file_location("pc98run_tool", REPO / "tools" / "pc98run.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
@unittest.skipUnless(DISKS, "needs the owner's PC-98 disk images (assets/local)")
class OwnerDisks(unittest.TestCase):
    def test_m2_ipl_loads_io_sys(self):
        """M2: the real IPL, NEC's boot menu and the partition boot sector load IO.SYS to 0060:0000 through INT 1Bh."""
        out = run(["--disk", DISKS / "th01.hdi", "--readonly", "--cycles", "20", "--trace-int=1b",
                   "--break", "0060:0000"])
        reads = [l for l in out if l.startswith("BIOS 1B AX=06")]
        self.assertTrue(any("BX=F800" in l and "ES=0060" in l for l in reads), "62 KiB to 0060:0000")
        self.assertTrue(any(l.startswith("BREAK 0060:0000") for l in out))
        self.assertEqual(end(out)["reason"], "break")
        self.assertFalse(any(l.startswith("BIOS-UNKNOWN") for l in out), "every call of the boot chain is answered")

    def test_m3_dos_prompt_and_autoexec(self):
        """M3: IO.SYS, MSDOS.SYS and COMMAND.COM run; the banner, a prompt line, AUTOEXEC.BAT's first program."""
        with tempfile.TemporaryDirectory() as d:
            ppm = Path(d) / "th01.ppm"
            out = run(["--disk", DISKS / "th01.hdi", "--readonly", "--cycles", "60", "--trace-dos",
                       "--until-exec", "mdrv98", "--dump-text", "--ppm", ppm])
            self.assertEqual(end(out)["reason"], "exec", out[-3:])
            self.assertTrue(any(l.startswith('DOS 4B "\\COMMAND.COM"') for l in out), "IO.SYS starts COMMAND.COM")
            self.assertTrue(any("AUTOEXEC.BAT" in l for l in out if l.startswith("DOS 3D")), "COMMAND.COM opens it")
            self.assertTrue(any(l.startswith('EXEC "A:\\MDRV98.COM"') for l in out))
            scr = screen(out)
            self.assertTrue(any("MS-DOS" in l for l in scr[:8]), scr[:8])
            self.assertTrue(any(l.startswith("A>") for l in scr), "a prompt line")
            self.assertFalse(any(l.startswith("BIOS-UNKNOWN") for l in out))
            png = Path(d) / "th01.png"
            load_pc98run_tool().write_png(ppm, png)
            data = png.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            idat = data.index(b"IDAT")
            raw = zlib.decompress(data[idat + 4:idat + 4 + struct.unpack(">I", data[idat - 4:idat])[0]])
            self.assertEqual(len(raw), 400 * (1 + 640 * 3))
            self.assertTrue(any(raw[i] for i in range(0, len(raw), 7)), "something was drawn")

    def test_dos_takes_keyboard_input(self):
        """Keys go through INT 09h and the BIOS buffer into DOS: CTRL+C breaks AUTOEXEC.BAT, Y confirms, DIR /W runs,
        and DOS waits at a new prompt (its console driver reads the buffer itself: the harness sees it polling)."""
        out = run(["--disk", DISKS / "th01.hdi", "--readonly", "--cycles", "300", "--keys", "^cy\\rdir /w\\r",
                   "--keys-at", "3.0", "--until-key-wait", "--dump-text"])
        self.assertEqual(end(out)["reason"], "key-poll", out[-3:])
        scr = [l.rstrip() for l in screen(out)]
        self.assertTrue(any("AUTOEXEC BAT" in l for l in scr), "DIR /W listed the root directory")
        self.assertEqual(scr[23], "A>", "the new prompt, the function-key line below it")
        self.assertFalse(any(l.startswith("BIOS-UNKNOWN") for l in out))

    def test_th01_reaches_its_title_on_the_386_model(self):
        """M4: on the 386 model (16 MHz) ZUNSOFT and the Anniversary Edition's OP.EXE run to the title picture: no CPU stop,
        and the screen holds the white title lettering and the red spiral (checked by colour counts, not by exact pixels:
        there is no reference picture in the tree and the games' pictures never are)."""
        font = DISKS.parent / "DOSBox-X" / "FONT.ROM"
        with tempfile.TemporaryDirectory() as d:
            ppm = Path(d) / "title.ppm"
            args = ["--disk", DISKS / "th01.hdi", "--readonly", "--cpu", "386", "--mhz", "16", "--cycles", "260",
                    "--ppm", ppm]
            if font.is_file():
                args += ["--font", font]
            out = run(args)
            e = end(out)
            self.assertEqual((e["reason"], e["stop"]), ("budget", "0"), out[-3:])
            raw = ppm.read_bytes().split(b"\n", 3)[3]
            self.assertEqual(len(raw), 640 * 400 * 3)
            px = [raw[i:i + 3] for i in range(0, len(raw), 3)]
            white = sum(1 for c in px if c == b"\xff\xff\xff")
            red = sum(1 for c in px if c[0] > 0x70 and c[1] < 0x40 and c[2] < 0x40)
            self.assertGreater(white, 5000, "the title lettering")
            self.assertGreater(red, 2000, "the spiral")

    def test_th01_title_is_clean_and_the_menu_answers_a_key(self):
        """M5: what the first runs showed as "stray marks" were three BIOS slips. INT 18h AH=14h takes its buffer in BX:CX and
        answers height then width; the work-area byte 053Bh holds the text rows' raster count - 1, which TH01 ORs 80h into for
        its CSRFORM (read as 0 it made the text GDC's rows one raster high, so the memory-switch cell with its red underline
        attribute drew a red 8-pixel dash at every 4081st cell). Checked here without any reference picture: no red 8x1 dash
        stands alone on the title, the text GDC has 16-raster rows, "HIT KEY" and the Anniversary banner draw (white pixels
        where the game puts them), and after Return the menu box and its four lines appear."""
        font = DISKS.parent / "DOSBox-X" / "FONT.ROM"

        def pixels(ppm):
            raw = ppm.read_bytes().split(b"\n", 3)[3]
            return [[tuple(raw[(y * 640 + x) * 3:(y * 640 + x) * 3 + 3]) for x in range(640)] for y in range(400)]

        def count(px, x0, y0, x1, y1, pred):
            return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pred(px[y][x]))

        white = lambda c: c == (255, 255, 255)
        with tempfile.TemporaryDirectory() as d:
            before, after = Path(d) / "before.ppm", Path(d) / "after.ppm"
            args = ["--disk", DISKS / "th01.hdi", "--readonly", "--cpu", "386", "--mhz", "16", "--cycles", "300", "--quiet",
                    "--type", "250:\\r", "--ppm-at", "245:%s" % before, "--ppm-at", "300:%s" % after, "--state"]
            if font.is_file():
                args += ["--font", font]
            out = run(args)
            self.assertEqual(end(out)["stop"], "0", out[-3:])
            gdc0 = next(l for l in out if l.startswith("GDC0 "))
            lr = int(gdc0.split("csrform=")[1].split()[0], 16) & 0x1F
            self.assertEqual(lr, 15, "16-raster text rows: " + gdc0)
            px = pixels(before)
            dashes = [(x, y) for y in range(1, 399) for x in range(0, 640, 8)
                      if all(px[y][x + k] == (255, 0, 0) for k in range(8))
                      and all(px[y - 1][x + k] == (0, 0, 0) and px[y + 1][x + k] == (0, 0, 0) for k in range(8))]
            self.assertEqual(dashes, [], "no red 8x1 dash stands alone on the title picture")
            self.assertGreater(count(px, 250, 300, 380, 325, white), 150, "HIT KEY, drawn through INT 18h AH=14h")
            grey = lambda c: c == (170, 170, 170)
            self.assertGreater(count(px, 0, 360, 200, 400, grey), 600, "the Anniversary Edition banner (half-width letters)")
            self.assertEqual(count(px, 200, 360, 400, 400, grey), 0,
                             "the banner's longest line is 184 dots wide: with the header's height and width swapped every "
                             "half-width letter was drawn twice and the lines ran to twice that")
            menu = pixels(after)
            purple = lambda c: c == (153, 51, 153)
            self.assertGreater(count(menu, 205, 245, 420, 385, lambda c: c[0] > 150 and c[1] < 60 and c[2] < 40), 3000,
                               "the red menu box appeared after Return")
            self.assertGreater(count(menu, 260, 272, 360, 292, white), 100, "START in white")
            self.assertGreater(count(menu, 240, 296, 380, 352, purple), 300, "CONTINUE, OPTION and QUIT in purple")
            self.assertEqual(count(px, 205, 245, 420, 385, purple), 0, "the title before the key has no menu text")

    def test_th01_stage_one_stays_up_and_plays(self):
        """M7: START gets to the stage-1 intro and then to the stage, and the game keeps running (writable image: the harness's
        memory copy). It used to leave for DOS about 2.5 s after the intro: it writes "STAGE01" with INT 18h AH=14h and DX = 8000h
        + the ASCII code into seven 18-byte stack buffers, the BIOS answered each as a 34-byte full-width glyph, the function's
        return address was overwritten with zeros and the invalid opcode at 0000:0000 raised INT 06h, whose handler (the game's STOP
        key handler) cleans up and exits. Checked by behaviour: no COMMAND.COM reload after the stage picture loads, one VSYNC
        interrupt per frame while it plays, the screen changes once the orb is thrown (key Z) and the game keeps drawing."""
        font = DISKS.parent / "DOSBox-X" / "FONT.ROM"
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.ppm", Path(d) / "b.ppm"
            args = ["--disk", DISKS / "th01.hdi", "--cpu", "386", "--mhz", "16", "--cycles", "560", "--trace-dos",
                    "--type", "250:\\r", "--type", "300:\\r", "--type", "420:{29}<80>{A9}", "--type", "470:{3C}<600>{BC}",
                    "--ppm-at", "415:%s" % a, "--ppm-at", "555:%s" % b, "--stats-every", "140"]
            if font.is_file():
                args += ["--font", font]
            out = run(args, timeout=900)
            pa, pb = a.read_bytes().split(b"\n", 3)[3], b.read_bytes().split(b"\n", 3)[3]
        self.assertEqual(end(out)["stop"], "0", out[-3:])
        stage = next(i for i, l in enumerate(out) if l.startswith('DOS 3D "ST0.GRP"') and "17CE" in l)
        self.assertFalse(any("COMMAND.COM" in l for l in out[stage:]), "the game stays up after its stage-1 intro")
        stats = [dict(kv.split("=") for kv in l.split()[1:] if "=" in kv) for l in out if l.startswith("STATS ")]
        last, prev = stats[-2], stats[-3]       # the last full 140 M windows (the final line repeats the end state)
        frames = int(last["frames"]) - int(prev["frames"])
        self.assertGreater(frames, 400)
        self.assertAlmostEqual(int(last["irq0A"]) - int(prev["irq0A"]), frames, delta=3, msg="one VSYNC interrupt per frame")
        self.assertGreater(int(last["out7C"]) - int(prev["out7C"]), 20000, "the game keeps drawing through the GRCG")
        differing = sum(1 for i in range(0, len(pa), 3) if pa[i:i + 3] != pb[i:i + 3])
        self.assertGreater(differing, 2000, "the picture changed once the orb was thrown")

    def test_th04_runs_to_stage_one_without_himem_and_emm386(self):
        """M7: TH04's CONFIG.SYS loads HIMEM.SYS and EMM386.EXE /UMB (the latter asks for protected mode, and the CPU stops there).
        With the harness's --virtual-config (four same-length edits of the in-memory image: the lines become REM) the game runs
        GAME.BAT, PMD86 (the -86 board), OP.EXE, the weapon selection (Z) and MAIN.EXE, the stage. The only extended-memory call
        in all of it is the one XMS installation check of ZUN.COM: no INT 67h (EMS), INT 15h or INT 1Fh, and MAIN.EXE calls none."""
        font = DISKS.parent / "DOSBox-X" / "FONT.ROM"
        z = "{29}<120>{A9}"
        base = ["--disk", DISKS / "th04.hdi", "--cpu", "386", "--mhz", "16", "--trace-dos", "--trace-int=2f,67,15,1f", "--opna", "86"]
        if font.is_file():
            base += ["--font", font]
        plain = run(base + ["--cycles", "40"], timeout=900)
        self.assertEqual(end(plain)["stop"], "1", "without the edits EMM386 asks for protected mode and the CPU stops")
        keys = sum((["--type", "%d:%s" % (t, z)] for t in (200, 260, 320, 470, 520, 580)), [])
        out = run(base + ["--cycles", "640", "--virtual-config"] + keys, timeout=900)
        self.assertEqual(end(out)["stop"], "0", out[-3:])
        self.assertEqual(sum(1 for l in out if l.startswith("EDIT ") and ": 1 place" in l), 4, "the four lines of CONFIG.SYS")
        execs = [l for l in out if l.startswith("DOS 4B")]
        for name in ("PMD86.COM", "OP.EXE", "main.EXE"):
            self.assertTrue(any(name in l for l in execs), "%s ran: %s" % (name, execs[-4:]))
        calls = [l for l in out if l.startswith("INT 2F AX=43") or l.startswith(("INT 67 ", "INT 15 ", "INT 1F "))]
        self.assertEqual([l[:14] for l in calls], ["INT 2F AX=4300"], "one XMS installation check and nothing else: " + str(calls))
        self.assertLess(out.index(calls[0]), next(i for i, l in enumerate(out) if "main.EXE" in l and l.startswith("DOS 4B")))

    def test_pmd86_gets_past_the_pcm_fifo(self):
        """M7: with the PC-9801-86 on the machine TH02 to TH05 run PMD86.COM, whose interrupt entry reads A468h (the board's PCM FIFO,
        which this model had no registers for: an unhandled port reads FFh) and, for as long as bit 4 reads as set, serves the FIFO and
        reads again, with interrupts off: it never left that loop and OP.EXE never started. Checked on TH05 (OP.EXE after 10 M clocks)
        and TH02 (after 142 M): the driver starts, the program after it starts, and the board has seen the driver's register writes."""
        for name, cycles in (("th05", 30), ("th02", 160)):
            if not (DISKS / (name + ".hdi")).is_file():
                continue
            with self.subTest(image=name):
                out = run(["--disk", DISKS / (name + ".hdi"), "--cpu", "386", "--mhz", "16", "--cycles", cycles, "--trace-dos",
                           "--opna", "86"], timeout=900)
                execs = [l for l in out if l.startswith("DOS 4B")]
                self.assertTrue(any("PMD86.COM" in l for l in execs), "the -86 board makes ZUN.COM pick PMD86: " + str(execs[-3:]))
                self.assertTrue(any("OP.EXE" in l for l in execs), "PMD86 got past its start-up: " + str(execs[-3:]))
                opna = next(l for l in out if l.startswith("OPNA "))
                self.assertGreater(int(dict(kv.split("=") for kv in opna[5:].split())["register_writes"]), 100, opna)

    def test_th01_music_driver_finds_the_fm_board(self):
        """M6: with the PC-9801-86 on the machine (--opna 86) MDRV98 finds it at 188h and sets it up the way docs/PC98_SOUND.md
        ("How to check a real driver") says: the ADPCM RAM test, 29h = 83h, the timers, key-off of all six channels; and its tempo
        interrupt (timer B, NB = E0h: 32 x 16 FM samples = 9.23 ms = 147,692 clocks at 16 MHz) arrives on time, as the 27h = 3Fh
        write of its handler shows."""
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "opna.csv"
            out = run(["--disk", DISKS / "th01.hdi", "--readonly", "--cpu", "386", "--mhz", "16", "--cycles", "12", "--quiet",
                       "--opna", "86", "--opna-log", log])
            rows = [l.split(",") for l in log.read_text().splitlines()[1:]]
        opna = next(l for l in out if l.startswith("OPNA "))
        self.assertGreaterEqual(int(dict(kv.split("=") for kv in opna[5:].split())["register_writes"]), 100, opna)
        regs = [(r[1], r[2], r[3]) for r in rows]
        self.assertIn(("0", "29", "83"), regs, "six channels and the timer interrupts")
        self.assertGreaterEqual(sum(1 for b, r, v in regs if b == "1" and r == "08"), 4, "the ADPCM RAM check wrote its bytes")
        keyoff = [r for r in rows if r[1] == "0" and r[2] == "28"]
        self.assertEqual(sorted(r[3] for r in keyoff[:7]), ["00", "01", "02", "03", "04", "05", "06"], "all six channels keyed off")
        ticks = [int(r[0]) for r in rows if r[1] == "0" and r[2] == "27" and r[3] == "3F"][3:]    # the handler's resets
        gaps = sorted(b - a for a, b in zip(ticks, ticks[1:]))
        self.assertGreater(len(gaps), 30)
        self.assertAlmostEqual(gaps[len(gaps) // 2], 147_692, delta=300, msg="timer B interrupts every 9.23 ms")
        self.assertLess(gaps[-1] - gaps[0], 3000, "no interrupt came late or twice")

    def test_th01_title_music_plays_in_tune(self):
        """M6: the title music is on the FM board's registers from the moment the title appears, and what it plays is on the
        equal-tempered scale (A = 440 Hz) within a few cents on every FM note: the driver's note tables assume the board's real
        clock, so this checks the model's tuning against the driver without any reference recording. The WAV the harness wrote
        is as long as the machine time, silent before the music, audible and not clipped after it."""
        import wave
        spec = importlib.util.spec_from_file_location("opnalog_tool", REPO / "tools" / "opnalog.py")
        opnalog = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(opnalog)
        with tempfile.TemporaryDirectory() as d:
            log, wav = Path(d) / "opna.csv", Path(d) / "title.wav"
            out = run(["--disk", DISKS / "th01.hdi", "--readonly", "--cpu", "386", "--mhz", "16", "--cycles", "260", "--quiet",
                       "--opna", "86", "--opna-log", log, "--wav", wav, "--wav-rate", "48000"], timeout=900)
            notes, drums, total, last_t = opnalog.parse(log, 16)
            with wave.open(str(wav), "rb") as w:
                self.assertEqual((w.getnchannels(), w.getframerate(), w.getsampwidth()), (2, 48000, 2))
                frames = w.getnframes()
                data = w.readframes(frames)
        fm_channels = sorted(c for c in notes if c.startswith("fm"))
        self.assertGreaterEqual(len(fm_channels), 4, "four FM channels carry music by 16 s: " + str(fm_channels))
        first = min(n[0] for c in fm_channels for n in notes[c])
        self.assertTrue(8.3 < first < 9.0, "the music starts when the title is up: %.2f s" % first)
        def cents_off(h):
            n = 12 * math.log2(h / 440.0) + 69          # the MIDI note number of h with A4 = 69 at 440 Hz
            return abs(n - round(n)) * 100
        cents = [cents_off(h) for c in fm_channels for s, e, h in notes[c] if h > 0]
        self.assertGreater(len(cents), 100, "notes")
        self.assertLess(max(cents), 10, "every FM note within 10 cents of a note of the equal-tempered scale")
        self.assertAlmostEqual(frames / 48000, 260e6 / 16e6, delta=0.05, msg="the WAV is as long as the machine time")
        import array
        pcm = array.array("h")
        pcm.frombytes(data)
        quiet = pcm[:int(8.0 * 48000) * 2]
        self.assertEqual(max(abs(x) for x in quiet), 0, "silence before the title music (the logo has no sound)")
        peak = max(abs(x) for x in pcm)
        self.assertTrue(3000 < peak < 32767, "audible and not clipped: peak %d" % peak)

    def test_other_images_reach_their_batch(self):
        """TH02, TH03 and TH05 carry the same DOS: each gets to COMMAND.COM and its batch file's first program."""
        for name in ("th02", "th03", "th05"):
            if not (DISKS / (name + ".hdi")).is_file():
                continue
            with self.subTest(image=name):
                out = run(["--disk", DISKS / (name + ".hdi"), "--readonly", "--cycles", "60", "--trace-dos",
                           "--until-exec", "zun.com", "--dump-text"])
                self.assertEqual(end(out)["reason"], "exec")
                self.assertTrue(any(l.startswith("A>") for l in screen(out)))


if __name__ == "__main__":
    unittest.main()
