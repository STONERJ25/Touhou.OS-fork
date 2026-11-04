"""Host tests of the tools that open initrd.img when it may be a plain tar archive or a gzip stream (docs/UPDATE.md, "The compressed initrd"), without QEMU or a C compiler.

  python tools/hosttest.py discover -s tests/host -p "test_inflate_tools.py" -v
  python -m unittest tests/host/test_inflate_tools.py -v

tools/push-update.py looks inside the initrd before it sends anything (the key guard): it must read a gzip one, and refuse one that the kernel would refuse (damaged,
cut short, bytes after the end, more than 256 MiB unpacked). The Python tools and tests that open the archive with `tarfile` rely on its transparent decompression:
a canary below says it still does. What the machine's own `update recv` makes of a gzip initrd is tests/case_update.py, and the kernel's side is test_inflate.py and
tests/case_initrd.py.
"""
import gzip
import importlib.util
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent.parent
TOOL = REPO / "tools" / "push-update.py"

spec = importlib.util.spec_from_file_location("push_update", TOOL)
pu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pu)

PUB = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBlobForTheTestsNotARealKeyJustBase64Text test@example"
OTHER = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAnotherBlobForTheTestsAlsoNotARealKey other@example"


def tar_with(entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, data in entries.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def gz(data, level=9):
    return gzip.compress(data, compresslevel=level, mtime=0)


TAR = tar_with({"Hakurei/etc/ssh/authorized_keys/marisa": (PUB + "\n").encode(), "bin/gapsh": bytes(range(256)) * 300, "ScarletDevilMansion/share/x": b"text " * 5000})


class InitrdTar(unittest.TestCase):
    def test_plain_is_returned_as_it_is(self):
        self.assertIs(pu.initrd_tar(TAR), TAR)
        junk = b"not an archive at all" * 50
        self.assertIs(pu.initrd_tar(junk), junk)                      # (whether it is a tar is not this function's business)

    def test_gzip_is_unpacked(self):
        for level in (1, 6, 9):
            self.assertEqual(pu.initrd_tar(gz(TAR, level)), TAR)
        self.assertEqual(pu.initrd_tar(gz(b"")), b"")

    def test_what_the_kernel_would_refuse_is_refused(self):
        good = gz(TAR)
        flipped = bytearray(good)
        flipped[len(good) // 2] ^= 0x04
        bad_crc = bytearray(good)
        bad_crc[-8] ^= 1
        bad_size = bytearray(good)
        bad_size[-1] ^= 1
        cases = {"a bit flipped": (bytes(flipped), "damaged"), "cut in the middle": (good[:len(good) // 2], "too early"), "no trailer": (good[:-8], "too early"),
                 "wrong CRC-32": (bytes(bad_crc), "damaged"), "wrong length": (bytes(bad_size), "damaged"), "a byte after the end": (good + b"\0", "follow"),
                 "a second member": (good + gz(b"x"), "follow"), "only the magic": (b"\x1f\x8b", "too early"), "magic and method": (b"\x1f\x8b\x08\x00", "too early")}
        for name, (data, want) in cases.items():
            with self.assertRaises(ValueError, msg=name) as cm:
                pu.initrd_tar(data)
            self.assertIn(want, str(cm.exception), name)

    def test_a_bomb_is_refused_without_unpacking_it(self):
        c = zlib.compressobj(9, zlib.DEFLATED, 31)
        chunk = bytes(1 << 20)
        data = b"".join(c.compress(chunk) for _ in range(300)) + c.flush()        # 300 MiB of zeros in about 300 KB
        self.assertLess(len(data), 1 << 20)
        with self.assertRaises(ValueError) as cm:
            pu.initrd_tar(data)
        self.assertIn("256 MiB", str(cm.exception))


class KeyGuard(unittest.TestCase):
    def test_keys_are_found_in_a_gzip_initrd(self):
        self.assertEqual(pu.authorized_blobs(gz(TAR)), {PUB.split()[1]})
        self.assertEqual(pu.authorized_blobs(gz(TAR)), pu.authorized_blobs(TAR))
        self.assertEqual(pu.authorized_blobs(gz(tar_with({"bin/gapsh": b"x"}))), set())

    def test_a_damaged_gzip_initrd_gives_none(self):
        self.assertIsNone(pu.authorized_blobs(gz(TAR)[:-100]))
        self.assertIsNone(pu.authorized_blobs(gz(b"not a tar archive at all" * 50)))


class CommandLine(unittest.TestCase):
    def make(self, d, initrd, with_key=True):
        (Path(d) / "kernel.elf").write_bytes(b"k" * 100)
        (Path(d) / "initrd.img").write_bytes(initrd)
        (Path(d) / "key").write_text("private part, not read\n")
        (Path(d) / "key.pub").write_text(PUB + "\n")

    def run_tool(self, d, *args):
        return subprocess.run([sys.executable, str(TOOL), "--host", "h", "--src", d, "--key", str(Path(d) / "key"), "--dry-run", *args], capture_output=True, text=True, timeout=120)

    def test_a_gzip_initrd_with_the_key_goes_on_and_is_described(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, gz(TAR))
            r = self.run_tool(d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("gzip, unpacks to %d bytes" % len(TAR), r.stdout)

    def test_a_gzip_initrd_without_the_key_is_refused_like_a_plain_one(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, gz(tar_with({"Hakurei/etc/ssh/authorized_keys/marisa": (OTHER + "\n").encode()})))
            r = self.run_tool(d)
            self.assertEqual(r.returncode, 2)
            self.assertIn("authorizes none of the keys", r.stderr)
            self.assertEqual(self.run_tool(d, "--no-key-check").returncode, 0)

    def test_a_damaged_gzip_initrd_is_refused_before_anything_is_sent_even_without_the_key_check(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, gz(TAR)[:-1000])
            for extra in ((), ("--no-key-check",)):
                r = self.run_tool(d, *extra)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertIn("initrd.img is unusable", r.stderr)
                self.assertIn("Nothing was sent", r.stderr)

    def test_a_plain_initrd_is_described_without_a_gzip_note(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, TAR)
            r = self.run_tool(d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn("gzip", r.stdout)


class Pack(unittest.TestCase):
    """tools/initrdpack.py, the compression step of tools/build.py (it has no LLVM in it, so it can be tested here)."""

    def setUp(self):
        spec2 = importlib.util.spec_from_file_location("initrdpack", REPO / "tools" / "initrdpack.py")
        self.ip = importlib.util.module_from_spec(spec2)
        spec2.loader.exec_module(self.ip)

    def pack(self, d, env, tar=TAR):
        tar_path, img_path = Path(d) / "initrd.tar", Path(d) / "initrd.img"
        tar_path.write_bytes(tar)
        lines = []
        with mock.patch.dict(os.environ, env, clear=False):
            if "TOUHOU_INITRD" not in env:
                os.environ.pop("TOUHOU_INITRD", None)
            self.ip.pack(tar_path, img_path, say=lines.append)
        return img_path, lines

    def test_the_gzip_is_reproducible(self):
        a, b = self.ip.compress(TAR), self.ip.compress(bytes(TAR))
        self.assertEqual(a, b)
        self.assertEqual(a[:10], bytes.fromhex("1f8b0800" "00000000" "02ff"))        # magic, deflate, no flags, modification time 0, best compression, OS unknown
        self.assertEqual(gzip.decompress(a), TAR)

    def test_default_is_gzip_and_plain_on_request(self):
        with tempfile.TemporaryDirectory() as d:
            img, lines = self.pack(d, {})
            self.assertEqual(img.read_bytes()[:2], b"\x1f\x8b")
            self.assertEqual(gzip.decompress(img.read_bytes()), TAR)
            self.assertTrue(lines[0].lstrip().startswith("GZIP"), lines)
            img, lines = self.pack(d, {"TOUHOU_INITRD": "plain"})
            self.assertEqual(img.read_bytes(), TAR)
            self.assertTrue(lines[0].lstrip().startswith("COPY") and "TOUHOU_INITRD=plain" in lines[0], lines)
            img, lines = self.pack(d, {"TOUHOU_INITRD": "GZIP"})                    # (case does not matter; the form changed again, so it is made again)
            self.assertEqual(img.read_bytes()[:2], b"\x1f\x8b")

    def test_nothing_is_redone_when_nothing_changed(self):
        with tempfile.TemporaryDirectory() as d:
            img, lines = self.pack(d, {})
            first = img.stat().st_mtime_ns
            os.utime(img, ns=(first - 10**9, first - 10**9))                      # make a rewrite visible
            old = img.stat().st_mtime_ns
            img, lines = self.pack(d, {})
            self.assertEqual((img.stat().st_mtime_ns, lines), (old, []))
            img, lines = self.pack(d, {}, tar=TAR + bytes(512))                     # another archive: made again
            self.assertNotEqual(img.stat().st_mtime_ns, old)
            self.assertEqual(gzip.decompress(img.read_bytes()), TAR + bytes(512))

    def test_a_wrong_value_stops_the_build(self):
        with tempfile.TemporaryDirectory() as d, self.assertRaises(SystemExit) as cm:
            self.pack(d, {"TOUHOU_INITRD": "zstd"})
        self.assertIn("TOUHOU_INITRD", str(cm.exception))

    def test_describe(self):
        with tempfile.TemporaryDirectory() as d:
            img, _ = self.pack(d, {})
            self.assertIn("gzip", self.ip.describe(img))
            self.assertIn("%.1f MiB archive" % (len(TAR) / 1048576), self.ip.describe(img))
            img, _ = self.pack(d, {"TOUHOU_INITRD": "plain"})
            self.assertIn("plain tar", self.ip.describe(img))


class TarfileCanary(unittest.TestCase):
    """The tools and tests that read the archive of a build with `tarfile.open(path)` or `tarfile.open(fileobj=...)` (tests/mcc_util.py, tests/case_media.py, tests/case_update.py,
    push-update.py's key guard) get both forms from it, and copy members while reading forward. If a Python ever stopped doing that, this is where it would show."""

    def test_both_forms_read_the_same(self):
        names = None
        for form in (TAR, gz(TAR)):
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "initrd.img"
                p.write_bytes(form)
                for opened in (tarfile.open(p), tarfile.open(p, "r"), tarfile.open(fileobj=io.BytesIO(form))):
                    with opened as t:
                        seen = {}
                        for m in t:                                         # (reading forward and copying: what case_update.py and case_media.py do)
                            seen[m.name] = t.extractfile(m).read() if m.isfile() else None
                        self.assertEqual(seen["bin/gapsh"], bytes(range(256)) * 300)
                        names = names or sorted(seen)
                        self.assertEqual(sorted(seen), names)


if __name__ == "__main__":
    unittest.main()
