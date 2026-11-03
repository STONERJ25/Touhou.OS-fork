#!/usr/bin/env python3
"""Host-side unit tests for tools/push-update.py (the stream a push sends, and its command line).

    python -m unittest discover -s tests/host -p "test_*.py" -v
    python tests/host/test_push_update.py -v

Plain unittest, no QEMU and no ssh. The real thing (the stream parsed by `update recv`, the ESP, the trial) is tests/case_update.py.
"""
import hashlib
import importlib.util
import io
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TOOL = REPO / "tools" / "push-update.py"

spec = importlib.util.spec_from_file_location("push_update", TOOL)
pu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pu)


class StreamTests(unittest.TestCase):
    def test_layout(self):
        k, i = b"K" * 5000, b"I" * 777
        s = pu.build_stream([("kernel.elf", k), ("initrd.img", i)], "v1.2-3")
        head, _, body = s.partition(b"end\n")
        lines = head.decode("ascii").split("\n")
        self.assertEqual(lines[0], "touhou-push 1")
        self.assertEqual(lines[1], "version v1.2-3")
        self.assertEqual(lines[2], f"file kernel.elf 5000 {hashlib.sha256(k).hexdigest()}")
        self.assertEqual(lines[3], f"file initrd.img 777 {hashlib.sha256(i).hexdigest()}")
        self.assertEqual(lines[4], "")
        self.assertEqual(body, k + i)

    def test_end_inside_the_data_does_not_confuse_the_header(self):
        data = b"end\nend\n" * 10
        s = pu.build_stream([("kernel.elf", data)], "x")
        head = s[:len(s) - len(data)]
        self.assertEqual(s[len(head):], data)
        self.assertTrue(head.endswith(b"\nend\n"))
        self.assertEqual(head.count(b"\nend\n"), 1)

    def test_versions(self):
        for good in ("1", "6df1c46-dirty", "v1.2+3_4", "a" * 64):
            pu.build_stream([("kernel.elf", b"x")], good)
        for bad in ("", "has space", "new\nline", "-leading", "a" * 65, "ünï", "x;rm"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                pu.build_stream([("kernel.elf", b"x")], bad)


class CommandLineTests(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, timeout=60)

    def test_dry_run_lists_the_files(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "kernel.elf").write_bytes(b"k" * 100)
            (Path(d) / "initrd.img").write_bytes(b"i" * 200)
            r = self.run_tool("--host", "nowhere.invalid", "--src", d, "--version", "t1", "--dry-run")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("kernel.elf", r.stdout)
            self.assertIn("initrd.img", r.stdout)
            self.assertIn("2 files", r.stdout)
            self.assertNotIn("cmdline.txt", r.stdout)
            r = self.run_tool("--host", "nowhere.invalid", "--src", d, "--version", "t1", "--dry-run", "--cmdline", "text blk.ro")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("cmdline.txt", r.stdout)
            self.assertIn("3 files", r.stdout)

    def test_missing_files_and_bad_arguments(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.run_tool("--host", "h", "--src", d, "--dry-run")
            self.assertEqual(r.returncode, 2)
            self.assertIn("kernel.elf is missing", r.stderr)
            (Path(d) / "kernel.elf").write_bytes(b"k")
            (Path(d) / "initrd.img").write_bytes(b"i")
            r = self.run_tool("--host", "h", "--src", d, "--dry-run", "--loader")
            self.assertEqual(r.returncode, 2)
            self.assertIn("BOOTX64.EFI is missing", r.stderr)
            r = self.run_tool("--host", "h", "--src", d, "--dry-run", "--version", "bad version")
            self.assertEqual(r.returncode, 2)
            r = self.run_tool("--host", "h", "--src", d, "--dry-run", "--cmdline", "a", "--cmdline-file", "b")
            self.assertEqual(r.returncode, 2)
            r = self.run_tool("--src", d, "--dry-run")                       # --host is required
            self.assertEqual(r.returncode, 2)


class KeyGuardTests(unittest.TestCase):
    """An image that does not authorize the key the tool logs in with would lock the machine out: it could not be confirmed and would fall back after two
    unconfirmed starts (2026-11-03: an image built in a git worktree, which has no assets/local/ssh, did exactly that to the Dell)."""
    PUB = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBlobForTheTestsNotARealKeyJustBase64Text test@example"
    OTHER = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAnotherBlobForTheTestsAlsoNotARealKey other@example"

    @staticmethod
    def tar_with(entries):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            for name, data in entries.items():
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
        return buf.getvalue()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True, timeout=60)

    def make(self, d, entries):
        (Path(d) / "kernel.elf").write_bytes(b"k" * 100)
        (Path(d) / "initrd.img").write_bytes(self.tar_with(entries))
        (Path(d) / "key").write_text("private part, not read\n")
        (Path(d) / "key.pub").write_text(self.PUB + "\n")

    def test_authorized_blobs(self):
        t = self.tar_with({"Hakurei/etc/ssh/authorized_keys/marisa": (self.PUB + "\n# a comment\n\n" + self.OTHER + "\n").encode(), "bin/gapsh": b"x"})
        self.assertEqual(pu.authorized_blobs(t), {self.PUB.split()[1], self.OTHER.split()[1]})
        self.assertEqual(pu.authorized_blobs(self.tar_with({"bin/gapsh": b"x"})), set())
        self.assertIsNone(pu.authorized_blobs(b"not a tar archive at all" * 50))

    def test_public_blob_reads_the_pub_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, {})
            self.assertEqual(pu.public_blob(Path(d) / "key"), self.PUB.split()[1])

    def test_a_push_without_the_key_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, {"bin/gapsh": b"x"})
            r = self.run_tool("--host", "h", "--src", d, "--key", str(Path(d) / "key"), "--dry-run")
            self.assertEqual(r.returncode, 2)
            self.assertIn("authorizes none of the keys", r.stderr)
            r = self.run_tool("--host", "h", "--src", d, "--key", str(Path(d) / "key"), "--dry-run", "--no-key-check")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.make(d, {"Hakurei/etc/ssh/authorized_keys/marisa": (self.OTHER + "\n").encode()})          # another key only: still refused
            r = self.run_tool("--host", "h", "--src", d, "--key", str(Path(d) / "key"), "--dry-run")
            self.assertEqual(r.returncode, 2)

    def test_a_push_with_the_key_goes_on(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, {"Hakurei/etc/ssh/authorized_keys/marisa": (self.PUB + "\n").encode()})
            r = self.run_tool("--host", "h", "--src", d, "--key", str(Path(d) / "key"), "--dry-run")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("2 files", r.stdout)


if __name__ == "__main__":
    unittest.main()
