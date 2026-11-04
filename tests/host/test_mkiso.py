#!/usr/bin/env python3
"""Host tests for the ISO writer (tools/mkiso.py), no QEMU needed. The checks do not trust the writer: tests/isocheck.py is a separate reader, written
from ECMA-119, the El Torito specification and the UEFI GPT rules (it imports nothing from tools/), and where the machine has them, programs that have
nothing to do with this project look at the result too: `file`, `sgdisk`, `sfdisk`, `xorriso`, `isoinfo`, `7z` (each is skipped, with a note, when absent;
on Windows the Linux ones are run through WSL).

    python -m unittest discover -s tests/host -p "test_mkiso.py" -v
"""
import hashlib
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tests"))
import isocheck  # noqa: E402  (the independent reader)
import mkiso  # noqa: E402


def make_inputs(root, loader=61000, kernel=312345, initrd=1900000, seed=7):
    """A fake boot set of the sizes a real one has (in miniature): returns the {tree path: bytes} the ISO should hold."""
    rng = random.Random(seed)
    esp = Path(root) / "esp"
    (esp / "EFI" / "BOOT").mkdir(parents=True)
    blobs = {"EFI/BOOT/BOOTX64.EFI": b"MZ" + rng.randbytes(loader - 2), "kernel.elf": b"\x7fELF" + rng.randbytes(kernel - 4), "initrd.img": rng.randbytes(initrd)}
    for name, data in blobs.items():
        (esp / name).write_bytes(data)
    return esp, blobs


def build(tmp, name="t.iso", epoch="1000000000", cmdline=mkiso.DEFAULT_CMDLINE, **sizes):
    esp, blobs = make_inputs(tmp, **sizes)
    out = Path(tmp) / name
    old = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is None:
        os.environ.pop("SOURCE_DATE_EPOCH", None)
    else:
        os.environ["SOURCE_DATE_EPOCH"] = epoch
    try:
        files = mkiso.read_inputs(esp, cmdline)
        docs = [("README.TXT", mkiso.README.replace("\n", "\r\n").encode()), ("LICENSE", (REPO / "LICENSE").read_bytes()),
                ("THIRD_PARTY_NOTICES.TXT", (REPO / "THIRD_PARTY_NOTICES.md").read_bytes())]
        info = mkiso.build_iso(out, files, mkiso.timestamp(), docs)
    finally:
        if old is None:
            os.environ.pop("SOURCE_DATE_EPOCH", None)
        else:
            os.environ["SOURCE_DATE_EPOCH"] = old
    expect = {"/KERNEL.ELF": blobs["kernel.elf"], "/INITRD.IMG": blobs["initrd.img"], "/EFI/BOOT/BOOTX64.EFI": blobs["EFI/BOOT/BOOTX64.EFI"],
              "/CMDLINE.TXT": (cmdline.strip() + "\n").encode(), "/LICENSE": (REPO / "LICENSE").read_bytes()}
    return out, info, expect


# ---------------------------------------------------------------------------------------------------- outside programs

def find_tool(name):
    """A command prefix that runs `name`, or None. On Windows the Linux tools come through WSL (and need /mnt/c paths: see tool_path)."""
    found = shutil.which(name)
    if found:
        return [found], False
    if name == "7z" and os.name == "nt":
        p = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "7-Zip" / "7z.exe"
        if p.is_file():
            return [str(p)], False
    if os.name == "nt" and shutil.which("wsl"):
        r = subprocess.run(["wsl", "which", name], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return ["wsl", name], True
    return None


def tool_path(path, via_wsl):
    p = str(Path(path).resolve())
    if via_wsl:
        return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")
    return p


def run_tool(name, args, path):
    """(returncode, output) of `name ARGS PATH`, or None if the machine does not have the program."""
    t = find_tool(name)
    if not t:
        return None
    cmd, wsl = t
    r = subprocess.run(cmd + list(args) + [tool_path(path, wsl)], capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout + r.stderr


class TestWriter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="mkiso-test-")
        cls.path, cls.info, cls.expect = build(cls.tmp)
        cls.data = cls.path.read_bytes()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # ------------------------------------------------------------------ the reader finds nothing wrong
    def test_the_independent_reader_finds_nothing_to_complain_about(self):
        iso = isocheck.IsoImage(self.data)
        self.assertEqual(iso.verify(self.expect), [])

    def test_what_the_reader_sees(self):
        iso = isocheck.IsoImage(self.data)
        iso.verify()
        self.assertEqual(sorted(iso.entries), ["/", "/CMDLINE.TXT", "/EFI", "/EFI/BOOT", "/EFI/BOOT/BOOTX64.EFI", "/ESP.IMG", "/INITRD.IMG", "/KERNEL.ELF", "/LICENSE",
                                               "/README.TXT", "/THIRD_PARTY_NOTICES.TXT"])
        self.assertEqual(iso.pvd["volume_id"], "TOUHOUOS")
        self.assertEqual(iso.pvd["block_size"], 2048)
        self.assertEqual(iso.pvd["space_blocks"] * 2048, len(self.data))
        self.assertEqual([t for t, _ in iso.vds], [1, 0, 255])                  # primary, boot record, terminator
        self.assertEqual(iso.catalog["platform"], 0xEF)
        self.assertEqual(len(iso.catalog["entries"]), 1)
        e = iso.catalog["entries"][0]
        self.assertTrue(e["bootable"])
        self.assertEqual((e["media"], e["rba"]), (0, self.info["esp_lb"]))
        self.assertEqual(e["count"], min(self.info["esp_len"] // 512, 0xFFFF))
        part = iso.gpt["parts"][0]
        self.assertEqual(part["type"], isocheck.ESP_GUID)
        self.assertEqual(part["first"] * 512, self.info["esp_lb"] * 2048)       # the partition IS the El Torito boot image
        self.assertEqual((part["last"] - part["first"] + 1) * 512, self.info["esp_len"])
        self.assertEqual(iso.fat["label"], "TOUHOUOS")
        self.assertEqual(iso.fat["fstype"], "FAT16")
        self.assertEqual(iso.fat["hidden"], part["first"])
        self.assertEqual(iso.read("/CMDLINE.TXT"), b"video=native blk.ro\n")

    def test_the_files_are_the_very_bytes_inside_the_fat_image(self):
        iso = isocheck.IsoImage(self.data)
        files = iso.fat_files()
        self.assertEqual(sorted(files), ["EFI/BOOT/BOOTX64.EFI", "cmdline.txt", "initrd.img", "kernel.elf"])
        for tree, inner in (("/KERNEL.ELF", "kernel.elf"), ("/INITRD.IMG", "initrd.img"), ("/CMDLINE.TXT", "cmdline.txt"), ("/EFI/BOOT/BOOTX64.EFI", "EFI/BOOT/BOOTX64.EFI")):
            off, size, contiguous, body = files[inner]
            self.assertTrue(contiguous, inner)
            self.assertEqual(iso.entries[tree].extent * 2048, self.info["esp_lb"] * 2048 + off, tree)
            self.assertEqual(body, self.expect[tree])
            self.assertEqual(off % 2048, 0, inner)
        # nothing is stored twice: the file is the boot image plus a little
        self.assertLess(len(self.data) - self.info["esp_len"], 100 * 1024, "the ISO should be the ESP image plus a few KiB")

    def test_the_gpt_and_the_catalog_are_where_the_specifications_put_them(self):
        d = self.data
        self.assertEqual(d[512:520], b"EFI PART")                              # the primary header at LBA 1
        total = len(d) // 512
        self.assertEqual(d[(total - 1) * 512:(total - 1) * 512 + 8], b"EFI PART")  # the backup header in the last sector
        self.assertEqual(d[32768 + 1:32768 + 6], b"CD001")                      # the first descriptor at block 16
        self.assertEqual(d[510:512], b"\x55\xaa")

    # ------------------------------------------------------------------ reproducible
    def test_same_epoch_same_bytes(self):
        with tempfile.TemporaryDirectory() as t2:
            p2, _, _ = build(t2, epoch="1000000000")
            self.assertEqual(hashlib.sha256(p2.read_bytes()).hexdigest(), hashlib.sha256(self.data).hexdigest())

    def test_the_epoch_is_what_the_time_stamps_say(self):
        iso = isocheck.IsoImage(self.data)
        pvd = iso.block(16)
        self.assertEqual(bytes(pvd[813:813 + 16]), b"2001090901464000")         # 1000000000 = 2001-09-09 01:46:40 UTC
        with tempfile.TemporaryDirectory() as t2:
            p2, _, _ = build(t2, epoch="1700000000")
            d2 = p2.read_bytes()
            self.assertEqual(bytes(d2[16 * 2048 + 813:16 * 2048 + 813 + 16]), b"2023111422132000")
            self.assertEqual(len(d2), len(self.data))

    def test_without_the_variable_the_clock_is_used_and_the_result_is_still_whole(self):
        with tempfile.TemporaryDirectory() as t2:
            p2, _, expect = build(t2, epoch=None)
            self.assertEqual(isocheck.IsoImage(p2).verify(expect), [])

    # ------------------------------------------------------------------ other shapes
    def test_a_big_set_and_a_tiny_set(self):
        for sizes in (dict(loader=50000, kernel=100000, initrd=400000), dict(loader=70000, kernel=900000, initrd=9000000)):
            with tempfile.TemporaryDirectory() as t2:
                p2, info, expect = build(t2, **sizes)
                self.assertEqual(isocheck.IsoImage(p2).verify(expect), [], sizes)
                self.assertGreaterEqual(info["esp_len"], 4085 * 2048)              # a FAT16 volume needs at least 4085 clusters
                self.assertEqual(info["esp_len"] % 2048, 0)

    def test_the_command_line_is_the_one_asked_for(self):
        with tempfile.TemporaryDirectory() as t2:
            p2, _, expect = build(t2, cmdline="text blk.ro tz=UTC0")
            iso = isocheck.IsoImage(p2)
            self.assertEqual(iso.verify(expect), [])
            self.assertEqual(iso.read("/CMDLINE.TXT"), b"text blk.ro tz=UTC0\n")

    def test_names_are_checked(self):
        for bad in ("lower.txt", "A B.TXT", "TOOLONG" * 6 + ".TXT", ""):
            with self.assertRaises(mkiso.IsoError):
                mkiso.file_ident(bad)
        self.assertEqual(mkiso.file_ident("LICENSE"), b"LICENSE.;1")
        self.assertEqual(mkiso.file_ident("THIRD_PARTY_NOTICES.TXT"), b"THIRD_PARTY_NOTICES.TXT;1")
        with self.assertRaises(mkiso.IsoError):
            mkiso.dir_ident("bad name")

    def test_command_line_tool(self):
        with tempfile.TemporaryDirectory() as t2:
            esp, blobs = make_inputs(t2)
            out = Path(t2) / "cli.iso"
            r = subprocess.run([sys.executable, str(REPO / "tools" / "mkiso.py"), "--no-build", "--esp", str(esp), "--out", str(out), "--cmdline", "text blk.ro"],
                               capture_output=True, text=True, env=dict(os.environ, SOURCE_DATE_EPOCH="1000000000"))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r"iso\s+.*cli\.iso\s+\(\d+ bytes, [\d.]+ MiB; the ESP image is [\d.]+ MiB of it\)")
            self.assertRegex(r.stdout, r"sha256 [0-9a-f]{64}")
            self.assertEqual(isocheck.IsoImage(out).verify({"/KERNEL.ELF": blobs["kernel.elf"]}), [])
            (esp / "kernel.elf").unlink()
            r = subprocess.run([sys.executable, str(REPO / "tools" / "mkiso.py"), "--no-build", "--esp", str(esp), "--out", str(out)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("missing", r.stderr)

    # ------------------------------------------------------------------ the checker has teeth: damage is found
    def damaged(self, edit, expect=None):
        d = bytearray(self.data)
        edit(d)
        return isocheck.IsoImage(bytes(d)).verify(expect)

    def test_damage_is_found(self):
        iso = isocheck.IsoImage(self.data)
        cat = iso.boot_catalog_block * 2048
        root = iso.entries["/"].extent * 2048
        total = len(self.data) // 512
        cases = {
            "volume space size": (lambda d: d.__setitem__(slice(16 * 2048 + 80, 16 * 2048 + 84), struct.pack("<I", 77)), "volume space size"),
            "both-endian halves": (lambda d: d.__setitem__(16 * 2048 + 132, d[16 * 2048 + 132] ^ 1), "both-endian"),
            "catalog checksum": (lambda d: d.__setitem__(cat + 28, d[cat + 28] ^ 0x10), "do not add up to zero"),
            "catalog key bytes": (lambda d: d.__setitem__(cat + 31, 0), "key bytes"),
            "catalog platform": (lambda d: d.__setitem__(cat + 1, 0), "platform"),
            "boot catalog pointer": (lambda d: d.__setitem__(slice(17 * 2048 + 71, 17 * 2048 + 75), struct.pack("<I", 5)), "validation entry"),
            "terminator": (lambda d: d.__setitem__(18 * 2048, 7), "terminator"),
            "primary GPT header crc": (lambda d: d.__setitem__(512 + 40, d[512 + 40] ^ 1), "CRC32"),
            "GPT entry array": (lambda d: d.__setitem__(1024 + 33, d[1024 + 33] ^ 1), "entry array"),
            "backup GPT header": (lambda d: d.__setitem__((total - 1) * 512 + 24, 9), "GPT"),
            "protective MBR type": (lambda d: d.__setitem__(446 + 4, 0x83), "protective MBR"),
            "directory order": (lambda d: self.swap_first_two_records(d, root), "order"),
            "path table": (lambda d: d.__setitem__(iso.pvd["l_path"] * 2048 + 2, d[iso.pvd["l_path"] * 2048 + 2] ^ 1), "path table"),
            "FAT copy": (lambda d: d.__setitem__(iso.boot_lb * 2048 + 512 * iso.fat["reserved"] + 2 * 300 + 2 * iso.fat["fatsz"] * 256 // 256 + 4 + iso.fat["fatsz"] * 512, 7), "FAT"),
            # (the tree and the boot image share these bytes, so a flipped bit shows in both views alike: only the writer's inputs can tell)
            "file contents": (lambda d: d.__setitem__(iso.entries["/KERNEL.ELF"].extent * 2048 + 100, d[iso.entries["/KERNEL.ELF"].extent * 2048 + 100] ^ 0xFF),
                              "not the bytes that were put in"),
            "truncated file": (None, "blocks"),
        }
        for what, (edit, needle) in cases.items():
            problems = isocheck.IsoImage(self.data[:-1000]).verify() if edit is None else self.damaged(edit, self.expect)
            self.assertTrue(problems, f"damage to the {what} was not noticed")
            self.assertTrue(any(needle in p for p in problems), f"damage to the {what}: expected a problem mentioning {needle!r}, got {problems[:3]}")

    @staticmethod
    def swap_first_two_records(d, root):
        # records after "." and "..": swap the first two real entries' positions by exchanging their identifiers' first letters
        pos = root
        for _ in range(2):
            pos += d[pos]
        first, second = pos, pos + d[pos]
        d[first + 33], d[second + 33] = d[second + 33], d[first + 33]

    # ------------------------------------------------------------------ programs that are not ours
    def test_file_says_iso_and_bootable(self):
        r = run_tool("file", ["-b"], self.path)
        if r is None:
            self.skipTest("no `file` program")
        self.assertIn("ISO 9660", r[1])
        self.assertIn("bootable", r[1])

    def test_sgdisk_finds_a_sound_gpt(self):
        r = run_tool("sgdisk", ["-v"], self.path)
        if r is None:
            self.skipTest("no `sgdisk` program")
        self.assertIn("No problems found", r[1], r[1])
        p = run_tool("sgdisk", ["-p"], self.path)
        self.assertIn("EF00", p[1])
        self.assertIn("EFI System Partition", p[1])

    def test_sfdisk_lists_the_esp(self):
        r = run_tool("sfdisk", ["-d"], self.path)
        if r is None:
            self.skipTest("no `sfdisk` program")
        self.assertIn("label: gpt", r[1], r[1])
        self.assertIn("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", r[1].upper())

    def test_7zip_lists_the_tree_and_the_boot_image(self):
        r = run_tool("7z", ["l"], self.path)
        if r is None:
            self.skipTest("no `7z` program")
        for name in ("README.TXT", "LICENSE", "THIRD_PARTY_NOTICES.TXT", "KERNEL.ELF", "INITRD.IMG", "BOOTX64.EFI", "CMDLINE.TXT"):
            self.assertIn(name, r[1])
        self.assertIn("Boot-NoEmul.img", r[1])

    def test_xorriso_reports_the_efi_boot_entry(self):
        r = run_tool("xorriso", ["-report_el_torito", "plain", "-indev"], self.path)
        if r is None:
            self.skipTest("no `xorriso` program")
        self.assertIn("UEFI", r[1], r[1])

    def test_isoinfo_reads_the_volume(self):
        r = run_tool("isoinfo", ["-d", "-i"], self.path)
        if r is None:
            self.skipTest("no `isoinfo` program")
        self.assertIn("Volume id: TOUHOUOS", r[1])
        self.assertIn("Logical block size is: 2048", r[1])


if __name__ == "__main__":
    unittest.main()
