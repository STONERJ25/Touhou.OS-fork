"""Host tests for the small tools of docs/the project plan: the Wake-on-LAN packet, the restart proof's token logic, the PC-98 disk-image reader (on an image built here, never on game data),
the BitLocker metadata parser, and the guard that no PC-98 game data or ROM is ever tracked by git.

  python -m unittest tests.host.test_machine_tools
"""
import importlib.util
import struct
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


wol = load("wol", "wol.py")
watch = load("machine_watch", "machine-watch.py")
hdi = load("hdi_look", "hdi-look.py")
bde = load("bde_probe", "bde-probe.py")


class MagicPacket(unittest.TestCase):
    def test_layout(self):
        p = wol.magic_packet("aa:bb:cc:dd:ee:ff")
        self.assertEqual(len(p), 102)
        self.assertEqual(p[:6], b"\xff" * 6)
        self.assertEqual(p[6:], bytes.fromhex("aabbccddeeff") * 16)

    def test_mac_spellings(self):
        for text in ("50-9A-4C-40-FA-D3", "aabbccddeeff", "aa:bb:cc:dd:ee:ff"):
            self.assertEqual(wol.parse_mac(text), bytes.fromhex("aabbccddeeff"))

    def test_refused(self):
        for text in ("aa:bb:cc:dd:ee", "zz:9a:4c:40:fa:d3", ""):
            with self.assertRaises(ValueError):
                wol.parse_mac(text)


class RestartProof(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(watch.classify(0, "abc123\nEND\n", "abc123"), "same")
        self.assertEqual(watch.classify(0, "cat: /tmp/.machine-watch-token: No such file\nEND\n", "abc123"), "new")
        self.assertIsNone(watch.classify(255, "", "abc123"))                    # no answer
        self.assertIsNone(watch.classify(0, "", "abc123"))                      # a login that ran nothing proves nothing


def build_hdi():
    """A tiny Anex86 HDI with a PC-98 partition table and a FAT12 volume whose logical sectors (1024) are larger than the disk's (512), holding /DIR/HELLO.TXT and /README.TXT."""
    secsz, spt, heads, cyls = 512, 17, 4, 10
    img = bytearray(secsz * spt * heads * cyls)
    part = bytearray(32)
    part[0], part[1] = 0xA1, 0x81                                              # bootable, MS-DOS
    # start S0 H0 C1 (bytes 8, 9, 10-11), end S0 H0 C5 (bytes 12, 13, 14-15)
    part[8], part[9], part[10], part[11] = 0, 0, 1, 0
    part[12], part[13], part[14], part[15] = 0, 0, 5, 0
    part[16:16 + 7] = b"TESTDOS"
    img[secsz:secsz + 32] = part
    start = (1 * heads + 0) * spt + 0                                          # LBA of the partition (cylinder 1)
    bps, spc, rsv, nfat, rootent, total, fatsz = 1024, 2, 1, 2, 512, 200, 1
    boot = bytearray(secsz)
    boot[3:11] = b"TESTOEM "
    struct.pack_into("<HBHBH", boot, 0x0B, bps, spc, rsv, nfat, rootent)
    struct.pack_into("<H", boot, 0x13, total)
    struct.pack_into("<H", boot, 0x16, fatsz)
    base = start * secsz
    img[base:base + secsz] = boot
    fat0, root0 = rsv, rsv + nfat * fatsz
    data0 = root0 + (rootent * 32 + bps - 1) // bps

    def vput(sector, data):
        o = base + sector * bps
        img[o:o + len(data)] = data

    fat = bytearray(bps)
    fat[0:3] = bytes([0xF8, 0xFF, 0xFF])
    for c, nxt in ((2, 0xFFF), (3, 0xFFF)):                                    # clusters 2 (the directory) and 3 (the file) each end the chain
        i = c * 3 // 2
        v = struct.unpack_from("<H", fat, i)[0]
        v = (v & 0x000F) | (nxt << 4) if c & 1 else (v & 0xF000) | nxt
        struct.pack_into("<H", fat, i, v)
    cl_readme = 4
    i = cl_readme * 3 // 2
    v = struct.unpack_from("<H", fat, i)[0]
    struct.pack_into("<H", fat, i, (v & 0xF000) | 0xFFF)
    for n in range(nfat):
        vput(fat0 + n * fatsz, fat)

    def entry(name, ext, attr, cluster, size):
        e = bytearray(32)
        e[:8], e[8:11], e[11] = name.ljust(8).encode(), ext.ljust(3).encode(), attr
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    vput(root0, entry("DIR", "", 0x10, 2, 0) + entry("README", "TXT", 0x20, cl_readme, 5))
    vput(data0 + (2 - 2) * spc, entry("HELLO", "TXT", 0x20, 3, 11))                       # the directory's cluster
    vput(data0 + (3 - 2) * spc, b"hello world")
    vput(data0 + (cl_readme - 2) * spc, b"abcde")
    header = struct.pack("<8I", 0, 0, 4096, len(img), secsz, spt, heads, cyls).ljust(4096, b"\0")
    return bytes(header + img)


class DiskImageReader(unittest.TestCase):
    def test_tree_and_files(self):
        disk = hdi.Hdi(build_hdi())
        parts = disk.partitions()
        self.assertEqual(len(parts), 1)
        self.assertEqual(parts[0][9], "TESTDOS")
        vol = hdi.Volume(disk, parts[0])
        self.assertEqual((vol.bps, vol.spc, vol.nfat, vol.fat16), (1024, 2, 2, False))
        self.assertNotEqual(vol.bps, disk.secsz)                               # the volume's sectors are larger than the disk's
        tree = {p: (isdir, size) for p, isdir, size, _ in vol.tree(2)}
        self.assertEqual(tree["/DIR"][0], True)
        self.assertEqual(tree["/DIR/HELLO.TXT"], (False, 11))
        self.assertEqual(tree["/README.TXT"], (False, 5))
        files = {p: (size, cl) for p, isdir, size, cl in vol.tree(2) if not isdir}
        self.assertEqual(vol.read_file(files["/DIR/HELLO.TXT"][1], 11), b"hello world")
        self.assertEqual(vol.read_file(files["/README.TXT"][1], 5), b"abcde")

    def test_not_an_hdi(self):
        with self.assertRaises(ValueError):
            hdi.Hdi(b"\0" * 8192)


class BitLockerMetadata(unittest.TestCase):
    def block(self, prots):
        entries = b""
        for p in prots:
            data = bytes(26) + struct.pack("<H", p) + bytes(8)
            entries += struct.pack("<HHHH", 8 + len(data), 2, 8, 1) + data
        hsize = 48
        head = b"-FVE-FS-" + bytes(0x38) + struct.pack("<III", hsize + len(entries), 2, hsize) + bytes(hsize - 12)
        return head + entries + bytes(256)

    def test_protectors(self):
        self.assertEqual(bde.protectors(self.block([0x0000])), [0])
        self.assertEqual(bde.protectors(self.block([0x0100, 0x0800, 0x0000])), [0x0100, 0x0800, 0])
        self.assertIsNone(bde.protectors(b"\0" * 512))


class NoGameData(unittest.TestCase):
    """The owner's PC-98 pack, ROMs and disk images stay in assets/local (git-ignored): nothing of the kind may ever be tracked (docs/PC98.md, docs/ASSETS.md)."""

    def test_nothing_forbidden_is_tracked(self):
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True).stdout.split(b"\0")
        names = [f.decode("utf-8", "replace") for f in out if f]
        bad = [n for n in names if n.lower().endswith((".hdi", ".fdi", ".d88", ".nfd", ".nhd", ".fdd", ".rom", ".exe", ".dll", ".wav"))
               or n.startswith("assets/local/")]
        self.assertEqual(bad, [], f"tracked files that must never be in the repository: {bad[:5]}")

    def test_assets_local_is_ignored(self):
        r = subprocess.run(["git", "check-ignore", "-q", "assets/local/anything"], cwd=ROOT)
        self.assertEqual(r.returncode, 0, "assets/local/ must be git-ignored")


if __name__ == "__main__":
    unittest.main()
