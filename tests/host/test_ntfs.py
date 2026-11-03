"""Host tests of the read-only NTFS driver and the BitLocker layer (kernel/fs/ntfs_core.c, ntfs_name.c, ntfs_lznt1.c, bitlocker.c), without QEMU.

  python tools/hosttest.py discover -s tests/host -p "test_ntfs.py"          (on Windows this runs in WSL's Ubuntu: it needs gcc)
  python tools/hosttest.py discover -s tests/host -p "test_ntfs.py" -k Fuzz   (one group)

The driver's parser is built for the host by tests/host/ntfstool.c with AddressSanitizer and UndefinedBehaviorSanitizer (build/host-ntfs/ntfstool) and checked against volumes that
tests/host/ntfsimg.py lays out. Three separately written pieces look at the same bytes: the generator (what the volume should contain), tools/ntfs-look.py (a Python reader) and the C
parser; where they disagree, one of them is wrong. Independent oracles are used where they exist: Windows' own LZNT1 compressor (tests/host/ntfs_lznt1_vectors.py) and the `cryptography`
package for AES, CCM and XTS. What none of this can check is the generator's and the driver's shared reading of the NTFS and BitLocker documents: docs/NTFS.md lists those spots, and the
real Windows volume on the owner's Dell is the test for them.
"""
import hashlib
import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import ntfsimg  # noqa: E402
import ntfs_lznt1_vectors as vectors  # noqa: E402
from ntfsimg import MIB  # noqa: E402

TOOL = ROOT / "build" / "host-ntfs" / "ntfstool"
WORK = ROOT / "build" / "ntfs-tests"
SOURCES = [ROOT / "tests/host/ntfstool.c", ROOT / "kernel/fs/ntfs_core.c", ROOT / "kernel/fs/ntfs_name.c", ROOT / "kernel/fs/ntfs_lznt1.c", ROOT / "kernel/fs/bitlocker.c"]
HEADERS = [ROOT / "kernel/fs/ntfs_priv.h", ROOT / "kernel/include/bitlocker.h"]


def load_look():
    spec = importlib.util.spec_from_file_location("ntfs_look", ROOT / "tools" / "ntfs-look.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


look = load_look()

try:
    import cryptography  # noqa: F401
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

HAVE_GCC = shutil.which("gcc") is not None


def build_tool():
    """Compiles the parser for the host with the sanitizers (once per run, again when a source is newer than the binary)."""
    newest = max(p.stat().st_mtime for p in SOURCES + HEADERS)
    if TOOL.exists() and TOOL.stat().st_mtime >= newest:
        return
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-DNTFS_HOST", "-DWITH_BDE", "-Wall", "-Wextra", "-Werror",
           "-Wno-unused-parameter", "-iquote", str(ROOT / "kernel/fs"), "-iquote", str(ROOT / "kernel/include"), *map(str, SOURCES), "-o", str(TOOL)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("cannot build tests/host/ntfstool.c:\n" + r.stdout + r.stderr)


ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=77", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")


def tool(image, *args, timeout=120):
    """Runs ntfstool on an image path. Returns (exit status, stdout bytes, stderr text). A sanitizer report or a signal is a test failure on its own."""
    r = subprocess.run([str(TOOL), str(image), *map(str, args)], capture_output=True, timeout=timeout, env=ENV)
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode < 0 or r.returncode >= 77 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
        raise AssertionError(f"ntfstool {' '.join(map(str, args))}: status {r.returncode}\n{err[-3000:]}")
    return r.returncode, r.stdout, err


def write_image(name, data):
    WORK.mkdir(parents=True, exist_ok=True)
    p = WORK / name
    p.write_bytes(data)
    return p


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_walk(out):
    rows = {}
    for line in out.decode("utf-8", "replace").splitlines():
        parts = line.split("\t")
        if len(parts) >= 6:
            rows[parts[1]] = dict(kind=parts[0], size=int(parts[2]), ino=int(parts[3]), mtime=int(parts[4]), tail=parts[5])
        elif parts[0] == "E":
            rows[parts[1]] = dict(kind="E", err=int(parts[2]))
    return rows


def rnd_bytes(rnd, n):
    return bytes(rnd.getrandbits(8) for _ in range(n))


def rnd_text(rnd, n):
    words = [b"gensokyo", b"reimu", b"marisa", b"touhou", b"kappa", b"fairy", b"shrine", b"\n", b" ", b"sakuya", b"patchouli"]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(words)
    return bytes(out[:n])


def assert_matches(tc, b, rows, links=None, skip=(), errors=None):
    """The C walk of a volume against what the generator put in it."""
    links = links or {}
    errors = errors or {}
    for path, exp in b.expected.items():
        if path in skip:
            continue
        row = rows.get(path)
        tc.assertIsNotNone(row, f"{path} is missing from the walk")
        kind = {"file": "f", "dir": "d", "symlink": "l", "junction": "l", "reparse": "f"}[exp["kind"]]
        tc.assertEqual(row["kind"], kind, path)
        tc.assertEqual(row["ino"], exp["no"], path)
        tc.assertEqual(row["mtime"], exp["mtime"], path)
        if kind == "f":
            tc.assertEqual(row["size"], exp["size"], path)
            if path in errors:
                tc.assertEqual(row["tail"], f"ERR={errors[path]}", path)
            elif exp["encrypted"]:
                tc.assertEqual(row["tail"], "ERR=-13", path)
            else:
                tc.assertEqual(row["tail"], "%08x" % crc(exp["data"]), path)
        elif kind == "l" and path in links:
            tc.assertEqual(row["tail"], links[path], path)
    extra = set(rows) - set(b.expected) - set(skip)
    tc.assertEqual(extra, set(), "paths the generator did not create")


# ---------------------------------------------------------------------------------------------------------------- LZNT1

@unittest.skipUnless(HAVE_GCC, "needs gcc")
class Lznt1(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    def decompress(self, comp, cap=1 << 20):
        """(exit status, output) of the C decompressor on a raw LZNT1 stream: 0 = decompressed, 1 = refused as damaged (or the output would not fit in `cap` bytes)."""
        WORK.mkdir(parents=True, exist_ok=True)
        src, dst = WORK / "lznt1.in", WORK / "lznt1.out"
        src.write_bytes(comp)
        r = subprocess.run([str(TOOL), "lznt1", str(src), str(dst), str(cap)], capture_output=True, env=ENV)
        err = r.stderr.decode("utf-8", "replace")
        if r.returncode < 0 or r.returncode >= 77 or "runtime error" in err or "AddressSanitizer" in err:
            raise AssertionError(err[-2000:])
        return r.returncode, (dst.read_bytes() if r.returncode == 0 else b"")

    def test_ms_xca_worked_example(self):
        """The 59-byte stream of MS-XCA 2.5 decompresses to the 142-byte string."""
        name, recipe, length, crc32, hexs = vectors.VECTORS[0]
        self.assertEqual(name, "xca-example")
        rc, out = self.decompress(bytes.fromhex(hexs))
        self.assertEqual(rc, 0)
        self.assertEqual(out, vectors.make(recipe))
        self.assertEqual(len(out), 142)

    def test_streams_made_by_windows(self):
        """Every stream that Windows' RtlCompressBuffer made (standard and maximum engine; text, runs, noise, periods, records) comes back byte for byte."""
        for name, recipe, length, crc32, hexs in vectors.VECTORS:
            with self.subTest(name):
                want = vectors.make(recipe)
                self.assertEqual((len(want), crc(want)), (length, crc32), "the recipe changed")
                rc, out = self.decompress(bytes.fromhex(hexs))
                self.assertEqual(rc, 0, name)
                self.assertEqual(out, want, name)
                self.assertEqual(look.lznt1_decompress(bytes.fromhex(hexs)), want, "the Python reader disagrees")

    def test_generator_compressor_is_understood_by_both_readers(self):
        """The compressor of the test generator makes valid streams: the C decompressor and the Python one return the input (and they are smaller for compressible data)."""
        rnd = random.Random(3)
        samples = [b"", b"a", b"abc" * 1000, rnd_text(rnd, 30000), rnd_bytes(rnd, 9000), bytes(10000), bytes(range(256)) * 40, rnd_text(rnd, 4096), rnd_text(rnd, 4097)]
        for i, data in enumerate(samples):
            with self.subTest(i):
                comp = ntfsimg.lznt1_compress(data)
                rc, out = self.decompress(comp, max(len(data), 1) + 4096)
                self.assertEqual((rc, out), (0, data))
                self.assertEqual(look.lznt1_decompress(comp), data)
        self.assertLess(len(ntfsimg.lznt1_compress(rnd_text(rnd, 30000))), 20000)

    def test_damaged_streams_fail_cleanly(self):
        """Truncation, wrong signature bits, copies from before the start, chunks that overrun 4096 bytes, and 3000 random mutations of real streams: an error or a result, never a crash."""
        good = bytes.fromhex(vectors.VECTORS[0][4])
        cases = [good[:-1], good[:30], good[:2], b"\x38\x80" + good[2:], b"\x00\x00", b"\xff\xff" + good[2:], bytes([good[0], good[1] & 0x8F]) + good[2:],
                 b"\x04\xb0\x01\x00\x20", b"\x03\xb0\x01\xff\xff", struct.pack("<H", 0xB000 | 8) + b"\xff" + b"\xff\xff" * 4]
        for i, c in enumerate(cases):
            with self.subTest(i):
                rc, _ = self.decompress(c, 100_000)
                self.assertIn(rc, (0, 1))
        rnd = random.Random(7)
        streams = [bytes.fromhex(v[4]) for v in vectors.VECTORS if len(v[4]) < 6000]
        for _ in range(300):
            s = bytearray(rnd.choice(streams))
            for _ in range(rnd.randint(1, 4)):
                s[rnd.randrange(len(s))] = rnd.getrandbits(8)
            rc, _ = self.decompress(bytes(s), rnd.choice([100, 5000, 70_000]))
            self.assertIn(rc, (0, 1))

    def test_small_output_buffers_are_refused_not_overrun(self):
        name, recipe, length, crc32, hexs = vectors.VECTORS[3]
        for cap in (1, 10, 100, 4000, length - 1):
            rc, _ = self.decompress(bytes.fromhex(hexs), cap)
            self.assertEqual(rc, 1, cap)


# ---------------------------------------------------------------------------------------------------------------- the generator and the Python reader agree

class GeneratorAndPythonReader(unittest.TestCase):
    """The generator is checked by a second, separately written parser before the C parser is blamed for anything."""

    def check(self, b):
        img = b.build()
        v = look.Volume(img)
        for path, exp in b.expected.items():
            no = v.lookup(path)
            self.assertEqual(no, exp["no"], path)
            st = v.stat(no)
            self.assertEqual(st["mtime"], exp["mtime"], path)
            if exp["kind"] == "file" and not exp["encrypted"]:
                self.assertEqual(v.read_path(path), exp["data"], path)
                self.assertEqual(st["size"], exp["size"], path)
                for s, d in exp["ads"].items():
                    self.assertEqual(v.read_path(path, s), d, path)
        return v

    def test_features(self):
        rnd = random.Random(5)
        b = ntfsimg.NtfsBuilder(size=32 * MIB)
        b.add_dir("/Documents")
        b.add_file("/Documents/hello.txt", b"hello\n")
        b.add_file("/Documents/frag.bin", rnd_bytes(rnd, 100_000), frag=7)
        b.add_file("/sparse.bin", rnd_bytes(rnd, 5000) + bytes(300_000) + rnd_bytes(rnd, 7000) + bytes(200_000), sparse=True, size=600_000)
        b.add_file("/comp.txt", rnd_text(rnd, 300_000), compress=True)
        b.add_file("/withads.txt", b"main", ads={"stream1": b"alt data"})
        b.add_file("/init.bin", rnd_bytes(rnd, 20000), size=40000, init=10000)
        self.check(b)

    def test_big_directory_is_a_tree(self):
        b = ntfsimg.NtfsBuilder(size=48 * MIB)
        b.add_dir("/many")
        for i in range(1500):
            b.add_file("/many/file%04d with a longer name.txt" % i, b"x%d\n" % i)
        v = self.check(b)
        self.assertGreater(b.root.children["many"].index_blocks, 30)
        names = [e["name"] for e in v.listdir("/many")]
        self.assertEqual(len(names), 1500)
        self.assertEqual(names, sorted(names, key=lambda n: ntfsimg.collation_key(n)), "the in-order walk is in collation order")


# ---------------------------------------------------------------------------------------------------------------- the C parser on generated volumes

@unittest.skipUnless(HAVE_GCC, "needs gcc")
class Parser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    def walk(self, b, name, *extra, links=None, skip=(), errors=None):
        path = write_image(name, b.build())
        rc, out, err = tool(path, *extra, "walk")
        self.assertEqual(rc, 0, err)
        rows = parse_walk(out)
        assert_matches(self, b, rows, links, skip, errors)
        return path, rows

    def test_every_feature_once(self):
        """Resident and fragmented data, sparse files, LZNT1, an uninitialised tail, alternate streams (not shown), hard links, symlinks, junctions, EFS (refused), a WOF file (refused)."""
        rnd = random.Random(5)
        b = ntfsimg.NtfsBuilder(size=32 * MIB)
        b.add_dir("/Documents")
        b.add_file("/Documents/hello.txt", b"hello\n")
        b.add_file("/Documents/frag.bin", rnd_bytes(rnd, 100_000), frag=7)
        b.add_file("/Documents/frag2.bin", rnd_bytes(rnd, 30_000), frag=2)
        b.add_file("/Documents/Ünïcode ファイル 🦊.txt", b"unicode\n")
        b.add_file("/sparse.bin", rnd_bytes(rnd, 5000) + bytes(300_000) + rnd_bytes(rnd, 7000) + bytes(200_000), sparse=True, size=600_000)
        b.add_file("/comp.txt", rnd_text(rnd, 300_000), compress=True)
        b.add_file("/comp2.bin", rnd_bytes(rnd, 70_000) + rnd_text(rnd, 200_000) + bytes(130_000) + rnd_text(rnd, 1000), compress=True)
        b.add_file("/withads.txt", b"main", ads={"stream1": b"alt data", "other": rnd_bytes(rnd, 2000)})
        b.add_file("/enc.txt", rnd_bytes(rnd, 5000), encrypted=True)
        b.add_file("/zero", b"")
        b.add_file("/init.bin", rnd_bytes(rnd, 20000), size=40000, init=10000)
        b.add_symlink("/link", "Documents\\hello.txt")
        b.add_junction("/Docs Junction", "\\??\\C:\\Documents")
        b.add_file("/orig.txt", b"link target\n")
        b.add_link("/Documents/hardlink.txt", "/orig.txt")
        b.add_reparse("/wof.dll.txt", 0x80000017, b"\x01\x00\x00\x00").data = rnd_bytes(rnd, 100)
        links = {"/link": "Documents/hello.txt", "/Docs Junction": "/mnt/windows/Documents"}
        path, rows = self.walk(b, "features.img", links=links, errors={"/wof.dll.txt": -95})
        self.assertEqual(rows["/orig.txt"]["ino"], rows["/Documents/hardlink.txt"]["ino"], "a hard link is the same file")
        self.assertNotIn("/withads.txt:stream1", rows)
        # the Python reader sees the same
        v = look.Volume(path.read_bytes())
        self.assertEqual(v.read_path("/withads.txt", "stream1"), b"alt data")

    def test_stat_and_cat_ranges(self):
        rnd = random.Random(9)
        data = rnd_bytes(rnd, 200_000)
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_file("/f.bin", data, frag=5)
        b.add_file("/c.txt", rnd_text(rnd, 150_000), compress=True)
        path = write_image("ranges.img", b.build())
        for name, expect in (("/f.bin", data), ("/c.txt", b.expected["/c.txt"]["data"])):
            for off, n in ((0, 10), (4095, 3), (4096, 4096), (65535, 70000), (len(expect) - 5, 100), (len(expect), 10), (12345, 0)):
                rc, out, _ = tool(path, "cat", name, off, n)
                self.assertEqual(out, expect[off:off + n], (name, off, n))
        rc, out, _ = tool(path, "stat", "/f.bin")
        st = dict(l.split("=") for l in out.decode().split())
        self.assertEqual((st["kind"], int(st["size"]), st["resident"]), ("f", 200_000, "0"))
        self.assertEqual(int(st["mtime"]), b.expected["/f.bin"]["mtime"])
        self.assertEqual(int(st["ctime"]), b.expected["/f.bin"]["ctime"])
        self.assertEqual(int(st["atime"]), b.expected["/f.bin"]["atime"])

    def test_case_insensitive_lookup_and_names(self):
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_dir("/Straße")
        b.add_file("/Straße/ÄÖÜ.txt", b"umlauts")
        b.add_file("/Straße/Résumé final.DOC", b"resume")
        b.add_file("/Straße/Ελληνικά.txt", b"greek")
        b.add_file("/Straße/Русский файл.txt", b"cyrillic")
        b.add_file("/Straße/𝔘𝔫𝔦𝔠𝔬𝔡𝔢.txt", b"astral")
        b.add_file("/Straße/" + "x" * 255, b"longest")
        path, _ = self.walk(b, "names.img")
        for variant in ("/straße/äöü.TXT", "/STRAßE/ÄÖÜ.txt", "/Straße/RÉSUMÉ FINAL.doc", "/Straße/ελληνικά.TXT", "/Straße/РУССКИЙ ФАЙЛ.TXT"):
            rc, out, _ = tool(path, "cat", variant)
            self.assertEqual(rc, 0, variant)
            self.assertTrue(out, variant)
        rc, out, _ = tool(path, "stat", "/Straße/nonexistent")
        self.assertIn(b"error -2", out)
        rc, out, _ = tool(path, "cat", "/Straße/" + "x" * 255)
        self.assertEqual(out, b"longest")

    def test_names_the_vfs_cannot_carry_are_skipped_and_counted(self):
        """A 255-character name of 3-byte characters is 765 bytes of UTF-8: not shown, counted, and the rest of the directory is intact."""
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_dir("/d")
        b.add_file("/d/" + "あ" * 120, b"fits in 360 bytes? no, too long")
        b.add_file("/d/short.txt", b"short")
        path = write_image("longutf.img", b.build())
        rc, out, _ = tool(path, "ls", "/d")
        text = out.decode()
        self.assertIn("short.txt", text)
        self.assertIn("skipped 1", text)

    def test_big_directory(self):
        b = ntfsimg.NtfsBuilder(size=64 * MIB)
        b.add_dir("/many")
        for i in range(3000):
            b.add_file("/many/file%04d with a longer name.txt" % i, b"x%d\n" % i)
        path, rows = self.walk(b, "many.img")
        self.assertEqual(sum(1 for p in rows if p.startswith("/many/")), 3000)
        # every name is also found by the tree search, in any case
        rnd = random.Random(2)
        for i in rnd.sample(range(3000), 40):
            rc, out, _ = tool(path, "cat", "/MANY/FILE%04d WITH A LONGER NAME.TXT" % i)
            self.assertEqual(out, b"x%d\n" % i)

    def test_index_and_cluster_geometries(self):
        """512-byte index blocks on 4096-byte clusters (sub-node numbers count 512-byte units), 512-byte clusters, 64 KiB clusters with 4 KiB blocks, 4096-byte sectors and records."""
        rnd = random.Random(4)
        for label, kw, nfiles in (("idx512", dict(index_size=512), 400), ("clu512", dict(cluster=512, size=16 * MIB), 200),
                                  ("clu64k", dict(cluster=65536, size=64 * MIB), 150), ("4kn", dict(sector=4096, record_size=4096, size=32 * MIB), 100),
                                  ("idx8k", dict(index_size=8192), 300), ("rec4096", dict(record_size=4096, index_size=4096), 120)):
            with self.subTest(label):
                kw.setdefault("size", 32 * MIB)
                b = ntfsimg.NtfsBuilder(**kw)
                b.add_dir("/d")
                for i in range(nfiles):
                    b.add_file("/d/n%03d.txt" % i, rnd_text(rnd, rnd.randrange(0, 3000)) if i % 3 == 0 else b"%d" % i)
                b.add_file("/big", rnd_bytes(rnd, 150_000), frag=4)
                self.walk(b, f"geometry-{label}.img")

    def test_attribute_lists_and_fragmented_mft(self):
        rnd = random.Random(8)
        b = ntfsimg.NtfsBuilder(size=32 * MIB, mft_fragments=4)
        b.add_file("/ext.bin", rnd_bytes(rnd, 60_000), frag=40, attrlist=True)
        b.add_file("/manyads", b"x", ads={"s%d" % i: rnd_bytes(rnd, 300) for i in range(12)})
        b.add_file("/forced.txt", b"abc", attrlist=True)
        b.add_file("/forced2.bin", rnd_bytes(rnd, 9000), attrlist=True, per_ext=2, frag=3)
        for i in range(150):
            b.add_file("/f%03d" % i, b"data %d" % i)
        path, rows = self.walk(b, "attrlist.img")
        rc, out, _ = tool(path, "info")
        self.assertIn(b"mft_runs=", out)
        rc, out, _ = tool(path, "rec", 0)
        self.assertIn(b"type 0x20", out, "record 0 has an $ATTRIBUTE_LIST")

    def test_old_style_records(self):
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_file("/old.txt", b"old style record", old_header=True, old_std=True)
        b.add_dir("/olddir", old_header=True)
        b.add_file("/olddir/x", b"x")
        self.walk(b, "old.img")

    def test_system_files_hidden_and_readable_on_request(self):
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_file("/a.txt", b"a")
        path = write_image("system.img", b.build())
        rc, out, _ = tool(path, "ls", "/")
        self.assertNotIn(b"$MFT", out)
        self.assertNotIn(b"$Boot", out)
        rc, out, _ = tool(path, "stat", "/$MFT")
        self.assertIn(b"error -2", out, "the metadata files are not reachable by name either")
        rc, out, _ = tool(path, "--system", "ls", "/")
        for name in (b"$MFT", b"$MFTMirr", b"$LogFile", b"$Volume", b"$AttrDef", b"$Bitmap", b"$Boot", b"$BadClus", b"$Secure", b"$UpCase", b"$Extend"):
            self.assertIn(name, out)
        rc, out, _ = tool(path, "--system", "cat", "/$MFT")
        self.assertEqual(out, b.mft, "$MFT reads as the bytes the generator put in the MFT")
        rc, out, _ = tool(path, "--system", "cat", "/$Boot")
        self.assertEqual(out, b.image[:len(out)])
        self.assertEqual(out[3:11], b"NTFS    ")
        rc, out, _ = tool(path, "--system", "cat", "/$UpCase")
        self.assertEqual(out, ntfsimg.upcase_table())

    def test_dos_names_are_hidden_but_resolve(self):
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_dir("/Program Files")
        b.add_file("/Program Files/A Rather Long File Name.txt", b"long")
        path = write_image("dos.img", b.build())
        rc, out, _ = tool(path, "ls", "/")
        rows = out.decode().splitlines()
        self.assertEqual(len(rows), 1, rows)
        dos = b.root.children["Program Files"].dos_names
        self.assertEqual(len(dos), 1)
        short = list(dos.values())[0]
        rc, out, _ = tool(path, "ls", "/" + short)
        self.assertIn(b"A Rather Long File Name.txt", out, "the 8.3 name still opens the directory")
        rc, out, _ = tool(path, "--dos", "ls", "/")
        self.assertEqual(len(out.decode().splitlines()), 2, "with the option both names are listed")

    def test_volume_information_and_free_space(self):
        b = ntfsimg.NtfsBuilder(size=16 * MIB, label="Gensokyo ✿", version=(3, 1), volume_flags=1)
        b.add_file("/a", b"x" * 100_000)
        path = write_image("info.img", b.build())
        rc, out, _ = tool(path, "info")
        info = dict(l.split("=", 1) for l in out.decode().splitlines())
        self.assertEqual(info["label"], "Gensokyo ✿")
        self.assertEqual(info["version"], "3.1")
        self.assertEqual(info["vol_flags"], "0x1")
        self.assertEqual((int(info["cluster"]), int(info["rec_size"]), int(info["idx_size"])), (4096, 1024, 4096))
        rc, out, _ = tool(path, "free")
        used = sum(bin(x).count("1") for x in b.bitmap)
        self.assertEqual(out.decode().strip(), f"free_clusters={b.nclusters - used} total={b.nclusters}")
        rc, out, _ = tool(path, "rec", 5)
        self.assertIn(b"type 0x90", out)

    def test_reading_is_cached(self):
        """A directory walked twice does not read the disk twice: the file records and the index blocks come from the caches the second time. (A directory of more files than the
        cache holds would not show this, hence 20.)"""
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_dir("/d")
        for i in range(20):
            b.add_file("/d/f%02d" % i, b"x")
        path = write_image("cache.img", b.build())
        rc, out, err = tool(path, "cache", "/d")
        self.assertEqual(rc, 0, err)
        stats = dict(kv.split("=") for kv in out.decode().split())
        self.assertEqual(int(stats["entries"]), 20)
        self.assertGreater(int(stats["first_reads"]), 20)
        self.assertEqual(int(stats["second_reads"]), 0, out.decode())
        self.assertGreater(int(stats["second_hits"]), 20)

    def test_the_source_has_no_way_to_write(self):
        """Read only for ever, checked in the text of the sources: no call that writes a block device, a write operation that does anything but refuse, no mount option that unlocks writing."""
        for name in ("ntfs.c", "ntfs_core.c", "ntfs_name.c", "ntfs_lznt1.c", "bitlocker.c"):
            text = (ROOT / "kernel" / "fs" / name).read_text()
            for word in ("blk_write", "blk_flush", "->write(", ".write =", "d->write", "dev.write", "io.write", "fwrite", "O_WRONLY"):
                if name == "ntfs.c" and word == ".write =":
                    continue                                                         # the refusing stub: checked below
                self.assertNotIn(word, text, f"{name} mentions {word}")
        glue = (ROOT / "kernel" / "fs" / "ntfs.c").read_text()
        self.assertIn("static ssize_t ntfs_nowrite(", glue)
        self.assertIn("return -EROFS", glue.split("static ssize_t ntfs_nowrite(")[1].split("\n")[0])
        self.assertIn("refusing a read-write mount", glue)


# ---------------------------------------------------------------------------------------------------------------- hostile and damaged volumes

@unittest.skipUnless(HAVE_GCC, "needs gcc")
class Hostile(unittest.TestCase):
    """Each test damages one thing at a known place. The parser must answer with an error (or a clean degraded result), in bounded time, without a sanitizer report."""

    @classmethod
    def setUpClass(cls):
        build_tool()
        rnd = random.Random(12)
        b = ntfsimg.NtfsBuilder(size=24 * MIB)
        b.add_dir("/d")
        for i in range(200):
            b.add_file("/d/n%03d.txt" % i, rnd_text(rnd, 400) if i % 2 else b"small %d" % i)
        b.add_file("/frag.bin", rnd_bytes(rnd, 120_000), frag=6)
        b.add_file("/comp.txt", rnd_text(rnd, 200_000), compress=True)
        b.add_file("/ext.bin", rnd_bytes(rnd, 50_000), frag=30, attrlist=True)
        b.add_file("/sparse.bin", rnd_bytes(rnd, 4000) + bytes(100_000) + rnd_bytes(rnd, 4000), sparse=True)
        cls.b = b
        cls.image = bytearray(b.build())

    def damaged(self, edit, name="hostile.img"):
        img = bytearray(self.image)
        edit(img)
        return write_image(name, bytes(img))

    def attr_offset(self, img, rec_no, atype, name=None, nth=0):
        """Offset in the image of an attribute header of a record (searching the unpatched structure)."""
        base = self.b.record_offset(rec_no)
        pos = struct.unpack_from("<H", img, base + 20)[0]
        used = struct.unpack_from("<I", img, base + 24)[0]
        count = 0
        while pos + 8 <= used:
            t, ln = struct.unpack_from("<II", img, base + pos)
            if t == 0xFFFFFFFF or ln == 0:
                break
            nm = img[base + pos + struct.unpack_from("<H", img, base + pos + 10)[0]:][:2 * img[base + pos + 9]].decode("utf-16-le") if img[base + pos + 9] else ""
            if t == atype and (name is None or nm == name):
                if count == nth:
                    return base + pos
                count += 1
            pos += ln
        raise AssertionError("attribute not found")

    def expect_open_fails(self, path, text=None):
        rc, out, err = tool(path, "info")
        self.assertEqual(rc, 1, out)
        self.assertIn(b"open failed", out)
        if text:
            self.assertIn(text.encode(), out)

    def test_boot_sector_damage(self):
        cases = [("oem", 3, b"NTFT    ", "no NTFS signature"), ("sig", 510, b"\x00\x00", "boot sector signature"), ("bps0", 11, b"\x00\x00", "sector size"),
                 ("bps3", 11, struct.pack("<H", 3), "sector size"), ("spc0", 13, b"\x00", "cluster size"), ("spc3", 13, b"\x03", "cluster size"),
                 ("spc200", 13, b"\xc8", "cluster size"), ("fatfields", 16, b"\x02", "FAT fields"), ("total0", 40, bytes(8), "larger than its device"),
                 ("totalhuge", 40, b"\xff" * 8, "larger than its device"), ("mft_out", 48, struct.pack("<Q", 1 << 40), "outside"), ("recsize", 64, b"\x05", "record size"),
                 ("recsize2", 64, b"\xef", "record size"), ("idxsize", 68, b"\x7f", "index block size"), ("idxsize2", 68, b"\xf8", "index block size")]
        for label, off, data, text in cases:
            with self.subTest(label):
                def edit(img, off=off, data=data):
                    img[off:off + len(data)] = data
                self.expect_open_fails(self.damaged(edit), text)

    def test_mft_record_zero_damage(self):
        base = self.b.record_offset(0)
        for label, off, data in (("magic", 0, b"BAAD"), ("usa_off", 4, b"\x01\x00"), ("usa_cnt", 6, b"\x09\x00"), ("usa_cnt0", 6, b"\x00\x00"), ("first_attr", 20, b"\xf0\x03"),
                                 ("first_attr_odd", 20, b"\x39\x00"), ("used", 24, b"\xff\xff\xff\x7f"), ("fixup_tail", 510, b"\x34\x12"), ("zeroed", 0, bytes(1024))):
            with self.subTest(label):
                def edit(img, off=off, data=data):
                    img[base + off:base + off + len(data)] = data
                self.expect_open_fails(self.damaged(edit))

    def test_attribute_damage_in_the_mft_record(self):
        data_at = self.attr_offset(self.image, 0, 0x80)
        for label, off, data in (("len0", 4, b"\x00\x00\x00\x00"), ("len_huge", 4, b"\xff\xff\xff\x7f"), ("len_unaligned", 4, b"\x45\x00\x00\x00"), ("nameoff", 10, b"\xff\x00"),
                                 ("runs_off_huge", 32, b"\xff\xff"), ("runs_off_small", 32, b"\x04\x00"), ("cunit", 34, b"\x20"), ("size_huge", 48, b"\xff" * 8),
                                 ("alloc_huge", 40, b"\xff" * 8)):
            with self.subTest(label):
                def edit(img, off=off, data=data):
                    img[data_at + off:data_at + off + len(data)] = data
                path = self.damaged(edit)
                rc, out, err = tool(path, "info")
                self.assertIn(rc, (0, 1))

    def test_one_damaged_file_does_not_take_the_volume_with_it(self):
        """A bad fixup in one file's record: that file errors, its neighbours and the directory listing are fine."""
        victim = self.b.expected["/d/n010.txt"]["no"]
        off = self.b.record_offset(victim) + 510
        path = self.damaged(lambda img: img.__setitem__(slice(off, off + 2), b"\xaa\xbb"))
        rc, out, err = tool(path, "walk")
        rows = parse_walk(out)
        self.assertEqual(rows["/d/n010.txt"]["kind"], "E")
        self.assertEqual(rows["/d/n010.txt"]["err"], -5)
        self.assertEqual(rows["/d/n011.txt"]["kind"], "f")
        self.assertEqual(rows["/frag.bin"]["tail"], "%08x" % crc(self.b.expected["/frag.bin"]["data"]))

    def test_a_loop_in_the_directory_index(self):
        """A sub-node pointer that leads back to its own block, or up to an ancestor: the walk and the search end with an error instead of looping or recursing. (A three-level
        tree is needed for that, so this builds its own: 512-byte index blocks.)"""
        rnd = random.Random(13)
        b = ntfsimg.NtfsBuilder(size=16 * MIB, index_size=512)
        b.add_dir("/d")
        for i in range(300):
            b.add_file("/d/n%03d.txt" % i, rnd_text(rnd, 100))
        img = b.build()
        v = look.Volume(img)
        d = b.root.children["d"]
        alloc = next(a for a in v.attrs(d.no) if a.type == 0xA0)
        self.assertEqual(len(alloc.runs), 1)
        base = alloc.runs[0][1] * b.cluster

        def block(vcn):
            return base + vcn * 512                                 # blocks are 512 bytes: sub-node numbers count 512-byte units

        branches = [vcn for vcn in range(d.index_blocks) if struct.unpack_from("<B", img, block(vcn) + 0x18 + 12)[0] & 1]
        self.assertGreaterEqual(len(branches), 2, "need a three-level tree")
        loops = {}
        for label, edit in (("self", lambda im, vcn: vcn), ("up", lambda im, vcn: branches[-1])):
            for vcn in branches[:3]:
                with self.subTest((label, vcn)):
                    im = bytearray(img)
                    first = struct.unpack_from("<I", im, block(vcn) + 0x18)[0]
                    pos = block(vcn) + 0x18 + first
                    while True:                                      # the first entry that has a sub-node
                        elen, klen, flags = struct.unpack_from("<HHH", im, pos + 8)
                        if flags & 1 or flags & 2:
                            break
                        pos += elen
                    self.assertTrue(flags & 1)
                    struct.pack_into("<Q", im, pos + elen - 8, edit(im, vcn))
                    path = write_image("loop.img", bytes(im))
                    rc, out, err = tool(path, "walk", timeout=30)
                    rows = parse_walk(out)
                    self.assertIn(rc, (0, 1))
                    # either the directory reports an error, or (a pointer that happens to be a valid tree edge) it lists; it must end
                    rc, out, err = tool(path, "ls", "/d", timeout=30)
                    self.assertIn(rc, (0, 1))
                    loops[(label, vcn)] = (rc, b"error" in out)
        self.assertTrue(any(failed for rc, failed in loops.values()), "at least one of the loops must have been detected as damage")

    def test_truncated_devices(self):
        for size in (4096, 512 * 9, 1 * MIB, 3 * MIB + 77):
            with self.subTest(size):
                path = write_image("trunc.img", bytes(self.image[:size]))
                rc, out, err = tool(path, "info")
                self.assertEqual(rc, 1)
        # cut in half: the boot sector claims more sectors than the device has, so the volume is not opened at all
        path = write_image("trunc2.img", bytes(self.image[:len(self.image) // 2]))
        rc, out, err = tool(path, "walk")
        self.assertEqual(rc, 1)
        self.assertIn(b"open failed", out)

    def test_runs_that_point_outside_the_volume(self):
        victim = self.b.expected["/frag.bin"]["no"]
        at = self.attr_offset(self.image, victim, 0x80)
        runs = at + struct.unpack_from("<H", self.image, at + 32)[0]
        img = bytearray(self.image)
        img[runs:runs + 10] = b"\x44\x01\x00\x00\x00\xff\xff\xff\x7f\x00"          # one cluster at cluster 0x7fffffff: far outside the volume
        path = write_image("runsout.img", bytes(img))
        rc, out, err = tool(path, "cat", "/frag.bin")
        self.assertEqual(rc, 1)
        self.assertIn("read error", err)
        rc, out, err = tool(path, "walk")
        self.assertIn(rc, (0, 1))

    def test_absurd_sizes_do_not_make_reads_slow(self):
        victim = self.b.expected["/sparse.bin"]["no"]
        at = self.attr_offset(self.image, victim, 0x80)
        img = bytearray(self.image)
        struct.pack_into("<Q", img, at + 48, (1 << 62) - 7)                    # data size: 4 EiB
        struct.pack_into("<Q", img, at + 56, 5000)                              # but only 5000 bytes initialised
        path = write_image("hugesize.img", bytes(img))
        rc, out, err = tool(path, "cat", "/sparse.bin", 0, 8192, timeout=30)
        self.assertEqual(len(out), 8192)
        rc, out, err = tool(path, "cat", "/sparse.bin", (1 << 40), 4096, timeout=30)
        self.assertEqual(out, bytes(4096), "beyond the initialised size it reads as zeros, and quickly")

    def test_compressed_data_that_is_garbage(self):
        victim = self.b.expected["/comp.txt"]["no"]
        at = self.attr_offset(self.image, victim, 0x80)
        runs = at + struct.unpack_from("<H", self.image, at + 32)[0]
        nrun = self.image[runs]
        lcn = int.from_bytes(self.image[runs + 1 + (nrun & 15):runs + 1 + (nrun & 15) + (nrun >> 4)], "little")
        img = bytearray(self.image)
        rnd = random.Random(1)
        img[lcn * 4096:lcn * 4096 + 4096] = rnd_bytes(rnd, 4096)
        path = write_image("garbage.img", bytes(img))
        rc, out, err = tool(path, "cat", "/comp.txt")
        self.assertIn(rc, (0, 1))

    def test_attribute_list_damage(self):
        victim = self.b.expected["/ext.bin"]["no"]
        at = self.attr_offset(self.image, victim, 0x20)
        val = at + struct.unpack_from("<H", self.image, at + 20)[0]
        for label, off, data in (("entry_len0", 4, b"\x00\x00"), ("entry_len_small", 4, b"\x10\x00"), ("entry_len_huge", 4, b"\xff\xff"), ("name_off", 7, b"\xff"), ("ref", 16, b"\xff" * 8),
                                 ("ref_self", 16, struct.pack("<Q", victim)), ("inst", 24, b"\x77\x77")):
            with self.subTest(label):
                path = self.damaged(lambda img, off=off, data=data: img.__setitem__(slice(val + off, val + off + len(data)), data))
                rc, out, err = tool(path, "walk", timeout=30)
                self.assertIn(rc, (0, 1))
                rc, out, err = tool(path, "cat", "/ext.bin", timeout=30)
                self.assertIn(rc, (0, 1))

    def test_index_entry_damage(self):
        """Lengths of 0 and past the node, keys that overrun, absurd name lengths: all errors, none loops."""
        d = self.b.root.children["d"]
        root = self.attr_offset(self.image, d.no, 0x90)
        vo = root + struct.unpack_from("<H", self.image, root + 20)[0]
        first = vo + 16 + struct.unpack_from("<I", self.image, vo + 16)[0]
        # offsets are relative to the first entry, except the node header's own fields (index length at vo + 20, first-entry offset at vo + 16)
        for label, where, data in (("elen0", first + 8, b"\x00\x00"), ("elen_huge", first + 8, b"\xf8\xff"), ("elen_odd", first + 8, b"\x41\x00"), ("klen_huge", first + 10, b"\xff\xff"),
                                   ("namelen", first + 16 + 64, b"\xff"), ("flags", first + 12, b"\xff\xff"), ("total", vo + 20, b"\xff\xff\xff\x7f"), ("first", vo + 16, b"\xf0\xff\xff\x7f"),
                                   ("first0", vo + 16, b"\x00\x00\x00\x00")):
            with self.subTest(label):
                path = self.damaged(lambda img, where=where, data=data: img.__setitem__(slice(where, where + len(data)), data), f"idx-{label}.img")
                for args in (("ls", "/d"), ("walk",), ("cat", "/d/n001.txt")):
                    rc, out, err = tool(path, *args, timeout=30)
                    self.assertIn(rc, (0, 1))


# ---------------------------------------------------------------------------------------------------------------- mutation fuzz

@unittest.skipUnless(HAVE_GCC, "needs gcc")
class Fuzz(unittest.TestCase):
    """Random damage to whole volumes, thousands of times, under AddressSanitizer and UBSan, each iteration limited to 20 seconds."""

    @classmethod
    def setUpClass(cls):
        build_tool()

    def run_fuzz(self, b, name, seeds, count, nmut):
        path = write_image(name, b.build())
        for seed in seeds:
            rc, out, err = tool(path, "fuzz", seed, count, nmut, timeout=900)
            self.assertEqual(rc, 0, err)
            self.assertIn(b"fuzz:", out)
        return out.decode()

    def test_plain_volume(self):
        rnd = random.Random(21)
        b = ntfsimg.NtfsBuilder(size=16 * MIB)
        b.add_dir("/d")
        for i in range(120):
            b.add_file("/d/n%03d with a name.txt" % i, rnd_text(rnd, 300) if i % 3 else b"s%d" % i)
        b.add_file("/frag.bin", rnd_bytes(rnd, 60_000), frag=5)
        b.add_file("/comp.txt", rnd_text(rnd, 90_000), compress=True)
        b.add_file("/sparse.bin", rnd_bytes(rnd, 3000) + bytes(70_000) + rnd_bytes(rnd, 3000), sparse=True)
        b.add_file("/ext.bin", rnd_bytes(rnd, 20_000), frag=20, attrlist=True)
        b.add_symlink("/l", "d\\n001 with a name.txt")
        b.add_junction("/j", "\\??\\C:\\d")
        print(self.run_fuzz(b, "fuzz1.img", (1, 2, 3), 400, 3))

    def test_volume_with_many_mutations(self):
        rnd = random.Random(22)
        b = ntfsimg.NtfsBuilder(size=16 * MIB, index_size=512)
        b.add_dir("/big")
        for i in range(300):
            b.add_file("/big/f%03d" % i, rnd_text(rnd, 200))
        b.add_file("/c.txt", rnd_text(rnd, 70_000), compress=True)
        print(self.run_fuzz(b, "fuzz2.img", (11, 12), 300, 16))

    def test_fragmented_mft_and_4kn(self):
        rnd = random.Random(23)
        b = ntfsimg.NtfsBuilder(size=24 * MIB, mft_fragments=4)
        for i in range(100):
            b.add_file("/f%03d" % i, rnd_text(rnd, 700))
        b.add_file("/ext.bin", rnd_bytes(rnd, 20_000), frag=30, attrlist=True)
        print(self.run_fuzz(b, "fuzz3.img", (31,), 300, 4))
        b = ntfsimg.NtfsBuilder(size=24 * MIB, sector=4096, record_size=4096)
        for i in range(60):
            b.add_file("/g%03d" % i, rnd_text(rnd, 900))
        print(self.run_fuzz(b, "fuzz4.img", (41,), 200, 4))


# ---------------------------------------------------------------------------------------------------------------- BitLocker

@unittest.skipUnless(HAVE_GCC and HAVE_CRYPTO, "needs gcc and the cryptography package")
class BitLocker(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    @staticmethod
    def run_cipher(*args):
        """(exit status, stdout text, stderr text) of one of the ciphers driven by hex strings (see tests/host/ntfstool.c)."""
        r = subprocess.run([str(TOOL), *map(str, args)], capture_output=True, env=ENV)
        err = r.stderr.decode("utf-8", "replace")
        if r.returncode or "runtime error" in err or "AddressSanitizer" in err:
            raise AssertionError(f"{args[:2]}: {r.returncode} {err[-1500:]}")
        return r.returncode, r.stdout.decode().strip(), err

    def test_aes_fips197(self):
        """FIPS-197 appendix C: the three key sizes, both directions."""
        pt = "00112233445566778899aabbccddeeff"
        for key, ct in (("000102030405060708090a0b0c0d0e0f", "69c4e0d86a7b0430d8cdb78070b4c55a"),
                        ("000102030405060708090a0b0c0d0e0f1011121314151617", "dda97ca4864cdfe06eaf70a0ec0d7191"),
                        ("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "8ea2b7ca516745bfeafc49904b496089")):
            self.assertEqual(self.run_cipher("aes", "enc", key, pt)[1], ct)
            self.assertEqual(self.run_cipher("aes", "dec", key, ct)[1], pt)

    def test_aes_random_blocks_against_cryptography(self):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        rnd = random.Random(1)
        for klen in (16, 24, 32):
            for _ in range(40):
                key, blk = rnd_bytes(rnd, klen), rnd_bytes(rnd, 16)
                e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
                want = e.update(blk) + e.finalize()
                self.assertEqual(self.run_cipher("aes", "enc", key.hex(), blk.hex())[1], want.hex())
                self.assertEqual(self.run_cipher("aes", "dec", key.hex(), want.hex())[1], blk.hex())

    def test_ccm_against_cryptography(self):
        rnd = random.Random(2)
        for klen in (16, 24, 32):
            for n in (1, 15, 16, 17, 44, 76, 100):
                key, nonce, plain = rnd_bytes(rnd, klen), rnd_bytes(rnd, 12), rnd_bytes(rnd, n)
                sealed = ntfsimg.ccm_seal(key, nonce, plain)
                tag, ct = sealed[:16], sealed[16:]
                self.assertEqual(self.run_cipher("ccm", key.hex(), nonce.hex(), tag.hex(), ct.hex())[1], "ok " + plain.hex())
                bad = bytearray(ct)
                bad[0] ^= 1
                self.assertTrue(self.run_cipher("ccm", key.hex(), nonce.hex(), tag.hex(), bytes(bad).hex())[1].startswith("fail"), "a changed ciphertext must not open")
                badtag = bytearray(tag)
                badtag[5] ^= 0x40
                self.assertTrue(self.run_cipher("ccm", key.hex(), nonce.hex(), bytes(badtag).hex(), ct.hex())[1].startswith("fail"), "a changed tag must not open")

    def test_sector_ciphers_against_the_generator(self):
        """XTS (cryptography, IEEE 1619), CBC with the offset-derived IV, and CBC with the Elephant diffuser (the paper's encryption direction in the generator, the decryption
        direction in the driver), at 512 and 4096 bytes."""
        rnd = random.Random(3)
        for method, klen in (("xts128", 32), ("xts256", 64), ("cbc128", 16), ("cbc256", 32), ("cbc128-diffuser", 64), ("cbc256-diffuser", 64)):
            for ss in (512, 4096):
                for _ in range(4):
                    fvek = rnd_bytes(rnd, klen)
                    off = rnd.randrange(0, 1 << 30) // ss * ss
                    plain = rnd_bytes(rnd, ss)
                    ct = ntfsimg.bde_encrypt_sector(method, plain, off, ss, fvek)
                    self.assertNotEqual(ct, plain)
                    out = self.run_cipher("sector", ntfsimg.BDE_METHODS[method], fvek.hex(), off, ss, ct.hex())[1]
                    self.assertEqual(out, plain.hex(), (method, ss))

    def test_xts_is_ieee1619(self):
        """IEEE 1619-2007 test vector 2 (Key1 = 0x11.., Key2 = 0x22.., data unit 0x3333333333, 32 bytes of 0x44): the oracle reproduces the published ciphertext, and the driver turns
        the start of a longer sector made the same way back into the plaintext."""
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        key = b"\x11" * 16 + b"\x22" * 16
        unit = 0x3333333333
        vector = bytes.fromhex("c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0")
        e = Cipher(algorithms.AES(key), modes.XTS(unit.to_bytes(16, "little"))).encryptor()
        plain = b"\x44" * 32 + bytes(480)
        sector = e.update(plain) + e.finalize()
        self.assertEqual(sector[:32], vector, "the oracle reproduces the published vector")
        self.assertEqual(self.run_cipher("sector", 0x8004, key.hex(), unit * 512, 512, sector.hex())[1], plain.hex())

    def volume(self, method="xts128", protectors=("clear",), nfiles=3, **kw):
        rnd = random.Random(5)
        b = ntfsimg.NtfsBuilder(size=24 * MIB, tail_clusters=80)
        b.add_dir("/Documents")
        b.add_file("/Documents/hello.txt", b"hello from a BitLocker volume\n")
        b.add_file("/Documents/big.bin", rnd_bytes(rnd, 300_000), frag=4)
        b.add_file("/comp.txt", rnd_text(rnd, 120_000), compress=True)
        for i in range(nfiles):
            b.add_file("/Documents/n%d" % i, rnd_text(rnd, 700))
        raw = b.build()
        wrapped, info = ntfsimg.bitlocker_wrap(raw, b, method=method, protectors=protectors, **kw)
        return b, raw, wrapped, info

    def test_every_method_unlocks_and_mounts(self):
        for method in ntfsimg.BDE_METHODS:
            with self.subTest(method):
                b, raw, wrapped, info = self.volume(method)
                path = write_image(f"bde-{method}.img", wrapped)
                rc, out, err = tool(path, "--bde", "walk")
                self.assertEqual(rc, 0, out.decode() + err)
                text = out.decode()
                self.assertIn("unlocked with the clear key", text)
                rows = parse_walk(out)
                assert_matches(self, b, rows)
                self.assertNotEqual(wrapped[:4096], raw[:4096])
                self.assertEqual(wrapped[3:11], b"-FVE-FS-")

    def test_decrypted_view_equals_the_plain_volume(self):
        """Every sector below the metadata area reads back as the plain volume's, in all positions: the relocated header, the neighbours of the header, random places."""
        b, raw, wrapped, info = self.volume("xts128")
        path = write_image("bde-view.img", wrapped)
        area = info["area"]
        rnd = random.Random(6)
        spots = [(0, 512), (0, 16 * 512), (512, 512), (15 * 512, 512), (16 * 512, 512), (8 * 512, 16 * 512)] + [(rnd.randrange(0, area // 512 - 20) * 512, rnd.choice((512, 4096, 65536))) for _ in range(25)]
        for off, n in spots:
            n = min(n, area - off)
            rc, out, err = tool(path, "bde-read", off, n)
            self.assertEqual(rc, 0, err)
            self.assertEqual(out, raw[off:off + n], (off, n))

    def test_metadata_layouts_real_volumes_might_use(self):
        rnd = random.Random(8)
        for label, kw in (("entries aligned to 8", dict(entry_align=8)), ("padded key blob", dict(fvek_pad=32)), ("copy 1 damaged", dict(damage_copies=(0,))), ("copies 1 and 2 damaged", dict(damage_copies=(0, 1))),
                          ("only one copy", dict(metadata_copies=1)), ("8 header sectors", dict(header_sectors=8)), ("32 header sectors", dict(header_sectors=32)),
                          ("clear key last", dict(protectors=("tpm", "recovery", "clear"))), ("clear key first", dict(protectors=("clear", "tpm"))),
                          ("another key method code", dict(clear_key_method=0x2003))):
            with self.subTest(label):
                b, raw, wrapped, info = self.volume(**kw)
                path = write_image("bde-layout.img", wrapped)
                rc, out, err = tool(path, "--bde", "walk")
                self.assertEqual(rc, 0, out.decode() + err)
                assert_matches(self, b, parse_walk(out))

    def test_partly_encrypted_volume_reads_the_plain_part_as_it_is(self):
        """The metadata says only the first part of the volume is encrypted: the rest is plain and must not be run through the cipher."""
        rnd = random.Random(9)
        b = ntfsimg.NtfsBuilder(size=24 * MIB, tail_clusters=80)
        b.add_file("/early.bin", rnd_bytes(rnd, 100_000))
        b.add_file("/filler.bin", bytes(9 * MIB), nonres=True)
        b.add_file("/late.bin", rnd_bytes(rnd, 100_000))
        raw = b.build()
        wrapped, info = ntfsimg.bitlocker_wrap(raw, b, encrypted_size=8 * MIB)
        path = write_image("bde-partial.img", wrapped)
        rc, out, err = tool(path, "--bde", "walk")
        self.assertEqual(rc, 0, out.decode() + err)
        rows = parse_walk(out)
        self.assertEqual(rows["/late.bin"]["tail"], "%08x" % crc(b.expected["/late.bin"]["data"]))
        self.assertEqual(rows["/early.bin"]["tail"], "%08x" % crc(b.expected["/early.bin"]["data"]))

    def test_volumes_without_a_clear_key_stay_locked(self):
        for prots in (("tpm",), ("recovery",), ("tpm", "recovery"), ("password",), ("tpm-pin", "startup")):
            with self.subTest(prots):
                b, raw, wrapped, info = self.volume(protectors=prots)
                path = write_image("bde-locked.img", wrapped)
                rc, out, err = tool(path, "bde-info")
                self.assertEqual(rc, 1)
                text = out.decode()
                self.assertIn("no clear-key protector", text)
                for p in prots:
                    name = {"tpm": "TPM", "recovery": "recovery password", "password": "password", "tpm-pin": "TPM + PIN", "startup": "startup key"}[p]
                    self.assertIn(name, text)
                rc, out, err = tool(path, "--bde", "walk")
                self.assertEqual(rc, 1)

    def test_a_wrong_or_damaged_key_is_refused(self):
        b, raw, wrapped, info = self.volume()
        path = write_image("bde-ok.img", wrapped)
        meta = bytes(info["meta"])
        at = meta.find(info["clear"])                                       # the clear key sits in the nested "key" property; the wrapped VMK follows it
        self.assertGreater(at, 0)
        fvek_entry = meta.find(struct.pack("<HHHH", 8 + 12 + 16 + 12 + 32, 3, 5, 1))      # size, entry type 3 (FVEK), value type 5 (AES-CCM encrypted key), version 1
        self.assertGreater(fvek_entry, 0)
        # a changed byte in the clear key, in the tag of the wrapped VMK, or in the wrapped FVEK: the CCM tag no longer verifies
        for label, pos in (("clear key", at + 3), ("encrypted VMK", at + 32 + 8 + 20), ("encrypted FVEK", fvek_entry + 8 + 12 + 16 + 5)):
            with self.subTest(label):
                damaged = bytearray(wrapped)
                for o in info["md_offsets"]:
                    damaged[o + pos] ^= 0x10
                p = write_image("bde-bad.img", bytes(damaged))
                rc, out, err = tool(p, "bde-info")
                self.assertEqual(rc, 1, out.decode())

    def test_a_key_that_does_not_produce_ntfs_is_refused_with_a_clear_message(self):
        b, raw, wrapped, info = self.volume()
        damaged = bytearray(wrapped)
        damaged[info["hdr_copy"]:info["hdr_copy"] + 512] = bytes(512)                     # the relocated sector 0 is gone
        path = write_image("bde-nontfs.img", bytes(damaged))
        rc, out, err = tool(path, "bde-info")
        self.assertEqual(rc, 1)
        self.assertIn("not an NTFS boot sector", out.decode())

    def test_python_reader_agrees(self):
        """tools/ntfs-look.py unlocks the same volumes with its own code."""
        for method in ("xts128", "xts256", "cbc128", "cbc256-diffuser"):
            with self.subTest(method):
                b, raw, wrapped, info = self.volume(method)
                v = look.Volume(wrapped)
                self.assertEqual(v.bde.fvek[:len(info["fvek"])], info["fvek"])
                for path, exp in b.expected.items():
                    if exp["kind"] == "file":
                        self.assertEqual(v.read_path(path), exp["data"], path)

    def test_fuzz_the_metadata(self):
        """Random damage to the BitLocker header and metadata: an error or a result, never a crash."""
        b, raw, wrapped, info = self.volume()
        rnd = random.Random(10)
        hot = [(0, 512), (info["md_offsets"][0], 1024), (info["md_offsets"][1], 1024), (info["md_offsets"][2], 1024), (info["hdr_copy"], 512)]
        for i in range(120):
            img = bytearray(wrapped)
            for _ in range(rnd.randint(1, 6)):
                lo, n = rnd.choice(hot)
                img[lo + rnd.randrange(n)] = rnd.getrandbits(8)
            path = write_image("bde-fuzz.img", bytes(img))
            rc, out, err = tool(path, "--bde", "walk", timeout=60)
            self.assertIn(rc, (0, 1))


if __name__ == "__main__":
    unittest.main()
