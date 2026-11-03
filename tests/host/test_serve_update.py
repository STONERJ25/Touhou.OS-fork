#!/usr/bin/env python3
"""Host-side unit tests for tools/serve-update.py (the update manifest, its signature and the HTTP server).

    python -m unittest discover -s tests/host -p "test_*.py" -v
    python tests/host/test_serve_update.py -v

Plain unittest, no QEMU and no compiler: they run on Linux and Windows. They need the Python package `cryptography`
and are skipped without it. Not part of tests/run_all.py (that suite boots the guest).
"""
import hashlib
import http.client
import importlib.util
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TOOL = REPO / "tools" / "serve-update.py"

spec = importlib.util.spec_from_file_location("serve_update", TOOL)
su = importlib.util.module_from_spec(spec)
spec.loader.exec_module(su)

HAVE_CRYPTO = su.ec is not None
if HAVE_CRYPTO:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

ISSUED = 1792800000


def sample_files():
    return {
        "kernel.elf": b"\x7fELF" + bytes(range(256)) * 40,
        "initrd.img": b"ustar" + b"\x00" * 5000 + b"reimu",
        "EFI/BOOT/BOOTX64.EFI": b"MZ" + b"\x90" * 3000,
        "cmdline.txt": b"video=native blk.ro\n",
    }


def make_esp(root, files=None):
    """Writes a directory laid out like build/esp."""
    for rel, data in (files or sample_files()).items():
        p = Path(root).joinpath(*rel.split("/"))
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


@unittest.skipUnless(HAVE_CRYPTO, "the Python package 'cryptography' is not installed")
class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = ec.generate_private_key(ec.SECP256R1())
        cls.other = ec.generate_private_key(ec.SECP256R1())
        cls.files = sample_files()
        cls.manifest = su.build_manifest(cls.files, cls.key, "v0.5-3-gabcdef0", ISSUED)

    def resign(self, body, key=None):
        return body + su.sign_body(body, key or self.key)

    def assertRefused(self, data, key=None):
        with self.assertRaises(su.ManifestError):
            su.verify_manifest(data, key or self.key)


class TestCanonicalEncoding(Base):
    def test_exact_bytes(self):
        body, sig = su.split_manifest(self.manifest)
        h = {p: hashlib.sha256(d).hexdigest() for p, d in self.files.items()}
        n = {p: len(d) for p, d in self.files.items()}
        expected = ("touhou-update 1\n"
                    "version v0.5-3-gabcdef0\n"
                    f"issued {ISSUED}\n"
                    f"key {su.key_id_of(self.key)}\n"
                    f"file EFI/BOOT/BOOTX64.EFI {n['EFI/BOOT/BOOTX64.EFI']} {h['EFI/BOOT/BOOTX64.EFI']}\n"
                    f"file cmdline.txt {n['cmdline.txt']} {h['cmdline.txt']}\n"
                    f"file initrd.img {n['initrd.img']} {h['initrd.img']}\n"
                    f"file kernel.elf {n['kernel.elf']} {h['kernel.elf']}\n"
                    "end\n").encode("ascii")
        self.assertEqual(body, expected)
        self.assertEqual(len(sig), 64)
        last = self.manifest[len(body):]
        self.assertRegex(last.decode("ascii"), r"\Asignature [0-9a-f]{128}\n\Z")

    def test_order_does_not_depend_on_input_order(self):
        reversed_files = dict(reversed(list(self.files.items())))
        a = su.split_manifest(su.build_manifest(reversed_files, self.key, "x", ISSUED))[0]
        b = su.split_manifest(self.manifest)[0].replace(b"version v0.5-3-gabcdef0", b"version x")
        self.assertEqual(a, b)

    def test_body_is_deterministic(self):
        ents = [su.Entry(p, len(d), hashlib.sha256(d).hexdigest()) for p, d in self.files.items()]
        self.assertEqual(su.encode_body("v1", 5, "0123456789abcdef", ents), su.encode_body("v1", 5, "0123456789abcdef", ents[::-1]))

    def test_key_id(self):
        point = su.public_point(self.key)
        self.assertEqual(len(point), 65)
        self.assertEqual(point[0], 4)
        self.assertEqual(su.key_id_of(self.key), hashlib.sha256(point).hexdigest()[:16])
        self.assertEqual(su.key_id_of(self.key.public_key()), su.key_id_of(self.key))

    def test_version_sanitized(self):
        self.assertEqual(su.sanitize_version("6df1c46-dirty\n"), "6df1c46-dirty")
        self.assertEqual(su.sanitize_version("v1.0 Subterranean Animism"), "v1.0-Subterranean-Animism")
        self.assertEqual(su.sanitize_version("..//"), "unknown")
        self.assertEqual(len(su.sanitize_version("a" * 200)), 64)
        m = su.parse_manifest(su.build_manifest(self.files, self.key, "weird version/é", ISSUED))
        self.assertEqual(m.version, "weird-version--")

    def test_paths(self):
        for good in ("kernel.elf", "EFI/BOOT/BOOTX64.EFI", "a_b-c.d", "_x"):
            self.assertTrue(su.valid_path(good), good)
        for bad in ("", "/kernel.elf", "../kernel.elf", "EFI/../kernel.elf", "./kernel.elf", "EFI//BOOT", "EFI/BOOT/",
                    "EFI\\BOOT", "kernel elf", ".hidden", "..", "a:b", "x" * 65, "/".join("a" * 9), "kérnel"):
            self.assertFalse(su.valid_path(bad), bad)
        with self.assertRaises(su.ManifestError):
            su.build_manifest({"../escape": b"x"}, self.key, "v", ISSUED)

    def test_parse_rejects_noncanonical(self):
        body, _ = su.split_manifest(self.manifest)
        sha = hashlib.sha256(self.files["kernel.elf"]).hexdigest()
        n = len(self.files["kernel.elf"])
        k = f"file kernel.elf {n} {sha}".encode()
        variants = {
            "CRLF": body.replace(b"\n", b"\r\n"),
            "trailing space": body.replace(b"end\n", b"end \n"),
            "double space": body.replace(k, k.replace(b" ", b"  ", 1)),
            "tab": body.replace(k, k.replace(b" ", b"\t", 1)),
            "empty line": body.replace(b"end\n", b"\nend\n"),
            "no end": body.replace(b"end\n", b""),
            "line after end": body + b"file zz 1 " + b"0" * 64 + b"\n",
            "bad magic": body.replace(b"touhou-update 1", b"touhou-update 2"),
            "upper-case hash": body.replace(sha.encode(), sha.upper().encode()),
            "short hash": body.replace(sha.encode(), sha[:-1].encode()),
            "leading zero": body.replace(b" %d " % n, b" 0%d " % n),
            "negative size": body.replace(b" %d " % n, b" -%d " % n),
            "duplicate": body.replace(b"end\n", k + b"\nend\n"),
            "unsorted": body.replace(b"file EFI", b"file zEFI"),
            "dot-dot": body.replace(b"file initrd.img", b"file ../initrd.img"),
            "absolute": body.replace(b"file initrd.img", b"file /initrd.img"),
            "non-ascii": body.replace(b"version v0.5", b"version v\xc3\xa9"),
            "nul": body.replace(b"version v0.5", b"version v\x00"),
            "fields swapped": body.replace(k, f"file kernel.elf {sha} {n}".encode()),
            "issued missing": b"\n".join(l for l in body.split(b"\n") if not l.startswith(b"issued")),
            "key upper": body.replace(b"key " + su.key_id_of(self.key).encode(), b"key ABCDEF0123456789"),
            "no files": b"\n".join(l for l in body.split(b"\n") if not l.startswith(b"file")),
        }
        for name, bad in variants.items():
            with self.subTest(name):
                self.assertNotEqual(bad, body)
                with self.assertRaises(su.ManifestError):
                    su.parse_body(bad)
                self.assertRefused(self.resign(bad))      # even with a valid signature over the odd bytes

    def test_too_many_files_and_too_big(self):
        many = {f"f{i:02d}": b"x" for i in range(su.MAX_FILES + 1)}
        with self.assertRaises(su.ManifestError):
            su.build_manifest(many, self.key, "v", ISSUED)
        body, _ = su.split_manifest(self.manifest)
        with self.assertRaises(su.ManifestError):
            su.split_manifest(body + b"x" * su.MAX_MANIFEST + b"\nsignature " + b"0" * 128 + b"\n")

    def test_signature_line(self):
        body, sig = su.split_manifest(self.manifest)
        good = sig.hex().encode()
        for name, tail in {
            "missing": b"",
            "upper-case": b"signature " + good.upper() + b"\n",
            "short": b"signature " + good[:-2] + b"\n",
            "no newline": b"signature " + good,
            "CRLF": b"signature " + good + b"\r\n",
            "something after": b"signature " + good + b"\nextra\n",
            "two spaces": b"signature  " + good + b"\n",
            "DER instead of r||s": b"signature " + su.encode_dss_signature(1, 2).hex().encode() + b"\n",
        }.items():
            with self.subTest(name):
                self.assertRefused(body + tail)


class TestSignature(Base):
    def test_round_trip(self):
        m = su.verify_manifest(self.manifest, self.key.public_key())
        self.assertEqual(m.version, "v0.5-3-gabcdef0")
        self.assertEqual(m.issued, ISSUED)
        self.assertEqual([e.path for e in m.files], sorted(self.files, key=lambda p: p.encode()))
        for e in m.files:
            su.check_file(e, self.files[e.path])

    def test_what_the_os_checks(self):
        """The OS hashes the body with SHA-256 and checks r and s against the 65-byte point: do exactly that, independently."""
        body, sig = su.split_manifest(self.manifest)
        pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), su.public_point(self.key))
        der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
        pub.verify(der, body, ec.ECDSA(hashes.SHA256()))   # raises if not valid

    def test_every_body_byte_is_covered(self):
        body, _ = su.split_manifest(self.manifest)
        tail = self.manifest[len(body):]
        for i in range(0, len(body), 7):
            with self.subTest(offset=i):
                b = bytearray(body)
                b[i] = ord("0") if b[i] != ord("0") else ord("1")
                self.assertRefused(bytes(b) + tail)

    def test_tampered_signature(self):
        body, sig = su.split_manifest(self.manifest)
        order = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
        r, s = sig[:32], sig[32:]
        bad = {
            "flipped bit": bytes([sig[0] ^ 1]) + sig[1:],
            "r and s swapped": s + r,
            "zero s": r + bytes(32),
            "zero r": bytes(32) + s,
            "s = n": r + order.to_bytes(32, "big"),
            "r = n": order.to_bytes(32, "big") + s,
            "all ones": b"\xff" * 64,
        }
        for name, bsig in bad.items():
            with self.subTest(name):
                self.assertRefused(body + b"signature " + bsig.hex().encode() + b"\n")

    def test_wrong_key(self):
        self.assertRefused(self.manifest, self.other.public_key())
        # signed by another key but claiming ours: the key line is signed, so this only works if the signature is ours
        body, _ = su.split_manifest(su.build_manifest(self.files, self.other, "v", ISSUED))
        body = body.replace(su.key_id_of(self.other).encode(), su.key_id_of(self.key).encode())
        self.assertRefused(self.resign(body, self.other))

    def test_changed_files_refused(self):
        m = su.verify_manifest(self.manifest, self.key)
        k = m.entry("kernel.elf")
        data = self.files["kernel.elf"]
        for name, bad in {"one byte": data[:-1] + bytes([data[-1] ^ 0x80]), "truncated": data[:-1],
                          "extended": data + b"\x00", "empty": b"", "other file": self.files["initrd.img"]}.items():
            with self.subTest(name), self.assertRaises(su.ManifestError):
                su.check_file(k, bad)
        su.check_file(k, data)

    def test_manifest_built_from_changed_file_differs(self):
        f = dict(self.files)
        f["kernel.elf"] = f["kernel.elf"] + b"!"
        m2 = su.verify_manifest(su.build_manifest(f, self.key, "v", ISSUED), self.key)
        with self.assertRaises(su.ManifestError):
            su.check_file(m2.entry("kernel.elf"), self.files["kernel.elf"])


class TestKeys(Base):
    def test_write_and_load(self):
        with tempfile.TemporaryDirectory() as d:
            p = su.write_private_key(Path(d) / "sub" / "update-signing-key.pem", self.key)
            if os.name == "posix":
                self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)
            self.assertEqual(su.key_id_of(su.load_private_key(p)), su.key_id_of(self.key))
            self.assertEqual(su.key_id_of(su.load_public_key(p)), su.key_id_of(self.key))
            with self.assertRaises(FileExistsError):
                su.write_private_key(p, self.other)
            self.assertEqual(su.key_id_of(su.load_private_key(p)), su.key_id_of(self.key))

            pem = Path(d) / "pub.pem"
            pem.write_bytes(self.key.public_key().public_bytes(serialization.Encoding.PEM,
                                                               serialization.PublicFormat.SubjectPublicKeyInfo))
            self.assertEqual(su.key_id_of(su.load_public_key(pem)), su.key_id_of(self.key))
            hx = Path(d) / "pub.hex"
            hx.write_text(su.public_point(self.key).hex() + "\n")
            self.assertEqual(su.key_id_of(su.load_public_key(hx)), su.key_id_of(self.key))

    def test_encrypted_key_from_environment(self):
        with tempfile.TemporaryDirectory() as d:
            p = su.write_private_key(Path(d) / "k.pem", self.key, b"yin-yang orb")
            self.assertIn(b"ENCRYPTED", p.read_bytes())
            old = os.environ.get(su.PASS_ENV)
            os.environ[su.PASS_ENV] = "yin-yang orb"
            try:
                self.assertEqual(su.key_id_of(su.load_private_key(p)), su.key_id_of(self.key))
            finally:
                if old is None:
                    del os.environ[su.PASS_ENV]
                else:
                    os.environ[su.PASS_ENV] = old

    def test_other_curve_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = su.write_private_key(Path(d) / "p384.pem", ec.generate_private_key(ec.SECP384R1()))
            with self.assertRaises(su.ManifestError):
                su.load_private_key(p)

    @unittest.skipUnless(shutil.which("git") and (REPO / ".git").exists(), "needs git and a checkout")
    def test_key_location(self):
        self.assertIsNotNone(su.key_location_problem(REPO / "tools" / "update-key.pem"))
        self.assertIsNotNone(su.key_location_problem(REPO / "secret.pem"))
        self.assertIsNone(su.key_location_problem(REPO / "build" / "update-key.pem"))           # build/ is ignored
        self.assertIsNone(su.key_location_problem(REPO / "update-signing-key.pem"))             # the .gitignore pattern
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(su.key_location_problem(Path(d) / "k.pem"))

    def test_c_header(self):
        h = su.c_header(self.key)
        self.assertIn(su.key_id_of(self.key), h)
        nums = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", h)]
        self.assertEqual(bytes(nums), su.public_point(self.key))


class TestCollect(Base):
    def test_from_directory(self):
        with tempfile.TemporaryDirectory() as d:
            make_esp(d)
            files, note = su.collect_files(d)
            self.assertEqual(files, self.files)
            self.assertIn("cmdline.txt", note)
            self.assertEqual(list(files), sorted(files, key=lambda p: p.encode()))

            files, _ = su.collect_files(d, cmdline="  text blk.ro tz=EST5EDT  ")
            self.assertEqual(files["cmdline.txt"], b"text blk.ro tz=EST5EDT\n")
            files, _ = su.collect_files(d, with_cmdline=False)
            self.assertNotIn("cmdline.txt", files)
            with self.assertRaises(ValueError):
                su.collect_files(d, cmdline="café")

            (Path(d) / "cmdline.txt").unlink()
            files, _ = su.collect_files(d)
            self.assertEqual(files["cmdline.txt"], (su.DEFAULT_CMDLINE + "\n").encode())

            (Path(d) / "kernel.elf").unlink()
            with self.assertRaises(FileNotFoundError):
                su.collect_files(d)


class TestServer(Base):
    def setUp(self):
        self.srv = su.make_server("127.0.0.1", 0, self.manifest, self.files, quiet=True)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(10)

    def request(self, path, method="GET"):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            c.request(method, path)
            r = c.getresponse()
            return r.status, r.read(), dict(r.getheaders())
        finally:
            c.close()

    def test_manifest_and_files(self):
        st, body, hdr = self.request("/manifest")
        self.assertEqual(st, 200)
        self.assertEqual(body, self.manifest)
        self.assertEqual(int(hdr["Content-Length"]), len(self.manifest))
        m = su.verify_manifest(body, self.key.public_key())
        for e in m.files:
            st, data, hdr = self.request("/files/" + e.path)
            self.assertEqual(st, 200, e.path)
            self.assertEqual(hdr["Content-Type"], "application/octet-stream")
            su.check_file(e, data)
        self.assertEqual(self.request("/manifest?x=1")[1], self.manifest)

    def test_head(self):
        st, body, hdr = self.request("/files/kernel.elf", "HEAD")
        self.assertEqual(st, 200)
        self.assertEqual(body, b"")
        self.assertEqual(int(hdr["Content-Length"]), len(self.files["kernel.elf"]))

    def test_reference_client(self):
        m = su.check_server(f"http://127.0.0.1:{self.port}", self.key.public_key())
        self.assertEqual(len(m.files), 4)
        with self.assertRaises(su.ManifestError):
            su.check_server(f"http://127.0.0.1:{self.port}/", self.other.public_key())

    def test_unknown_and_traversal_refused(self):
        for path in ("/files/", "/files/../kernel.elf", "/files/EFI/../kernel.elf", "/files/./kernel.elf",
                     "/files//kernel.elf", "/files/%2e%2e/kernel.elf", "/files/kernel%2eelf", "/files/KERNEL.ELF",
                     "/files/EFI%2FBOOT%2FBOOTX64.EFI", "/files/EFI\\BOOT\\BOOTX64.EFI", "/../../etc/passwd",
                     "/files/../../tools/serve-update.py", "/files/..%2f..%2fetc%2fpasswd", "/kernel.elf",
                     "/manifest/", "/manifest/../files/kernel.elf", "/files/kernel.elf/", "/secret.txt"):
            # ("//manifest" is not here: http.server itself folds leading slashes into one, which reaches only the manifest)
            with self.subTest(path):
                st, body, _ = self.request(path)
                self.assertEqual(st, 404)
                self.assertNotIn(b"ELF", body)

    def test_post_refused(self):
        st, _, _ = self.request("/manifest", "POST")
        self.assertEqual(st, 501)

    def test_tampered_file_on_the_wire_refused(self):
        bad = dict(self.files)
        bad["initrd.img"] = bad["initrd.img"][:-1] + b"X"
        evil = su.UpdateServer("127.0.0.1", 0, self.manifest, bad, quiet=True)   # skips make_server's own check
        t = threading.Thread(target=evil.serve_forever, daemon=True)
        t.start()
        try:
            with self.assertRaises(su.ManifestError):
                su.check_server(f"http://127.0.0.1:{evil.server_address[1]}/", self.key)
        finally:
            evil.shutdown()
            evil.server_close()
            t.join(10)

    def test_make_server_checks_its_files(self):
        bad = dict(self.files)
        bad["kernel.elf"] += b"!"
        with self.assertRaises(su.ManifestError):
            su.make_server("127.0.0.1", 0, self.manifest, bad, quiet=True)
        extra = dict(self.files)
        extra["secret.txt"] = b"marisa's notes"
        with self.assertRaises(ValueError):
            su.make_server("127.0.0.1", 0, self.manifest, extra, quiet=True)


@unittest.skipUnless(HAVE_CRYPTO, "the Python package 'cryptography' is not installed")
class TestCommandLine(unittest.TestCase):
    def run_tool(self, *args, env=None):
        e = dict(os.environ)
        e.update(env or {})
        return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True, env=e, timeout=120)

    def test_keygen_manifest_verify(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            key = d / "keys" / "update-signing-key.pem"
            env = {su.KEY_ENV: str(key)}
            r = self.run_tool("keygen", env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(key.is_file())
            r = self.run_tool("keygen", env=env)
            self.assertNotEqual(r.returncode, 0, "keygen must not replace an existing key")

            make_esp(d / "esp")
            r = self.run_tool("manifest", "--src", d / "esp", "--out", d / "manifest", "--version", "test", env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = self.run_tool("verify", d / "manifest", "--src", d / "esp", env=env)
            self.assertEqual(r.returncode, 0, r.stderr)

            r = self.run_tool("pubkey", "--format", "hex", "--out", d / "pub.hex", env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = self.run_tool("verify", d / "manifest", "--pub", d / "pub.hex", env={su.KEY_ENV: str(d / "nothing")})
            self.assertEqual(r.returncode, 0, r.stderr)

            (d / "esp" / "kernel.elf").write_bytes(b"not the kernel that was signed")
            r = self.run_tool("verify", d / "manifest", "--src", d / "esp", env=env)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("REFUSED", r.stderr)

            m = (d / "manifest").read_bytes().replace(b"version test", b"version tesT")
            (d / "manifest").write_bytes(m)
            r = self.run_tool("verify", d / "manifest", env=env)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("REFUSED", r.stderr)

    @unittest.skipUnless(shutil.which("git") and (REPO / ".git").exists(), "needs git and a checkout")
    def test_keygen_refuses_the_repository(self):
        target = REPO / "tools" / "do-not-commit.pem"
        r = self.run_tool("keygen", "--out", target)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("refused", r.stderr)
        self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
